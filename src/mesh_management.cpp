#include "mesh_transport.h"
#include "mesh_management_internal.h"
#include "mesh_removal.h"
#include "mesh_config_retry.h"
#include "hardware.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/task.h"
#include <cstring>
#include <cerrno>
#if defined(PUMP_DISCOVERY_TRACE)
static_assert(CONFIG_BLE_MESH_STACK_TRACE_LEVEL >= 4, "Discovery trace requires SDK debug logs");
#endif
namespace pump {
namespace {
MeshManagement state;
MeshDiscoveryCache recentDiscovery;
enum class Kind : uint8_t { Enable, DisableNode, Key, Bind, Candidate, Provisioned, Closed, Add, Cancelled, NodeDeleted, Config };
struct Event { Kind kind{}; int error = 0; uint32_t opcode = 0; uint16_t address = 0; MeshCandidate candidate{}; bool valid = false; };
QueueHandle_t events;
std::atomic<bool> lost{false};
uint16_t primary; uint32_t began, sentAt; uint8_t step, attempts;
MeshConfigRetry configRetry;
uint32_t configBegan=0, operationBegan=0, cancelBegan=0;
bool provisionOperation=false, quarantined=false;
bool joining = false, cancelling = false; uint8_t selected[16];
const char *cancelReason = "PROVISION_TIMEOUT";
bool roleRecoveryAttempted = false, roleRecoveryPending = false;
bool resetting = false;
std::atomic<uint32_t> discoveryReceived{0}, discoveryMatched{0}, discoveryRejected{0}, discoveryDropped{0};
std::atomic<int> uuidMatchError{INT32_MIN}, provisionerEnableError{INT32_MIN};
uint32_t discoveryOutsideScan = 0, discoveryDuplicates = 0, discoveryKnown = 0, discoveryFull = 0;
uint32_t discoverySeeded = 0;
#if defined(PUMP_DISCOVERY_TRACE)
bool discoveryTraceActive = false;
esp_log_level_t discoveryPreviousLevel = ESP_LOG_INFO;
#endif
void stopDiscoveryTrace() {
#if defined(PUMP_DISCOVERY_TRACE)
  if (discoveryTraceActive) {
    esp_log_level_set("BLE_MESH", discoveryPreviousLevel);
    discoveryTraceActive = false;
  }
#endif
}
void startDiscoveryTrace() {
#if defined(PUMP_DISCOVERY_TRACE)
  if (!discoveryTraceActive) {
    discoveryPreviousLevel = esp_log_level_get("BLE_MESH");
    discoveryTraceActive = true;
    esp_log_level_set("BLE_MESH", ESP_LOG_DEBUG);
  }
#endif
}
MeshRemoval removal;
uint16_t resetAddresses[32] = {};
uint8_t resetCount = 0, resetIndex = 0;
esp_ble_mesh_client_t *client = nullptr;
void enqueue(const Event &event) { if (events && xQueueSend(events, &event, 0) != pdTRUE) lost = true; }
void fail(const char *reason) {
  stopDiscoveryTrace();
  ESP_LOGW("mesh", "management failed: %s addr=%04x step=%u attempt=%u", reason, state.target, unsigned(step), unsigned(attempts));
  state.phase = "FAILED"; state.reason = reason; state.busy = joining; state.configRetryWait = false; step = 0;
}
void quarantine(const char *reason) {
  fail(reason); quarantined=true; state.busy=true;
  state.recoveryAction="RESTART_REQUIRED"; // SDK owns its contexts until a real terminal callback/reboot.
}
void cancelJoin(const char *reason) {
  // START_PROV_NOW entries otherwise retry automatically on later beacons,
  // hiding the device from discovery even after the application reports failure.
  stopDiscoveryTrace();
  cancelReason = reason;
  cancelling = true;
  cancelBegan=nowMs();
  state.phase = "CANCELLING"; state.reason = reason; state.busy = true;
  esp_ble_mesh_device_delete_t dev{};
  dev.flag = DEL_DEV_UUID_FLAG; memcpy(dev.uuid, selected, 16);
  if (esp_ble_mesh_provisioner_delete_dev(&dev) != ESP_OK) {
    quarantine("CANCEL_SEND");
  }
}
uint16_t u16(const uint8_t *p) { return p[0] | uint16_t(p[1]) << 8; }
bool pumpComposition(const uint8_t *p, size_t n) {
  // Our actuator is the vendor model on the primary element. Do not bind an arbitrary model.
  if (!p || n < 14 || u16(p) != CompanyId) return false;
  const size_t offset = 14 + 2 * p[12], vendors = p[13];
  if (offset + 4 * vendors > n) return false;
  for (size_t i = 0; i < vendors; ++i) if (u16(p + offset + 4*i) == CompanyId && u16(p + offset + 4*i + 2) == ModelId) return true;
  return false;
}
void configCallback(esp_ble_mesh_cfg_client_cb_event_t event, esp_ble_mesh_cfg_client_cb_param_t *p) {
  if (!p || !p->params || (event != ESP_BLE_MESH_CFG_CLIENT_GET_STATE_EVT && event != ESP_BLE_MESH_CFG_CLIENT_SET_STATE_EVT && event != ESP_BLE_MESH_CFG_CLIENT_TIMEOUT_EVT)) return;
  Event e{}; e.kind = Kind::Config; e.address = p->params->ctx.addr; e.opcode = p->params->opcode;
  e.error = p->error_code; if (event == ESP_BLE_MESH_CFG_CLIENT_TIMEOUT_EVT) e.error = -1;
  if (!e.error) switch (e.opcode) {
    case ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_GET: {
      const auto &s = p->status_cb.comp_data_status; e.valid = s.page == 0 && s.composition_data && pumpComposition(s.composition_data->data, s.composition_data->len);
      if (e.valid) esp_ble_mesh_provisioner_store_node_comp_data(e.address, s.composition_data->data, s.composition_data->len);
      break;
    }
    case ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD: { const auto &s=p->status_cb.appkey_status; e.valid = s.status == 0 && s.net_idx == 0 && s.app_idx == 0; break; }
    case ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND: { const auto &s=p->status_cb.model_app_status; e.valid = s.status == 0 && s.element_addr == e.address && s.app_idx == 0 && s.company_id == CompanyId && s.model_id == ModelId; break; }
    case ESP_BLE_MESH_MODEL_OP_MODEL_SUB_ADD: { const auto &s=p->status_cb.model_sub_status; e.valid = s.status == 0 && s.element_addr == e.address && s.sub_addr == 0xc001 && s.company_id == CompanyId && s.model_id == ModelId; break; }
    case ESP_BLE_MESH_MODEL_OP_NODE_RESET: e.valid = true; break;
    default: return;
  }
  enqueue(e);
}
uint32_t opcodeForStep() {
  switch(step) { case 1:return ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_GET; case 2:return ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD; case 3:return ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND; case 4:return ESP_BLE_MESH_MODEL_OP_MODEL_SUB_ADD; default:return 0; }
}
bool sendStep(uint32_t now) {
  esp_ble_mesh_client_common_param_t p{}; p.model = client->model; p.opcode = opcodeForStep();
  p.ctx.addr = state.target; p.ctx.net_idx = 0; p.ctx.app_idx = 0; p.ctx.send_ttl = 3; p.msg_timeout = MeshConfigRetry::ResponseTimeoutMs;
  esp_err_t result;
  if (step == 1) { esp_ble_mesh_cfg_client_get_state_t get{}; get.comp_data_get.page = 0; result = esp_ble_mesh_config_client_get_state(&p, &get); }
  else {
    esp_ble_mesh_cfg_client_set_state_t set{};
    if (step == 2) { const auto key = esp_ble_mesh_provisioner_get_local_app_key(0,0); if (!key) return false; memcpy(set.app_key_add.app_key,key,16); set.app_key_add.net_idx=0; set.app_key_add.app_idx=0; }
    if (step == 3) { set.model_app_bind.element_addr=state.target; set.model_app_bind.model_app_idx=0; set.model_app_bind.company_id=CompanyId; set.model_app_bind.model_id=ModelId; }
    if (step == 4) { set.model_sub_add.element_addr=state.target; set.model_sub_add.sub_addr=0xc001; set.model_sub_add.company_id=CompanyId; set.model_sub_add.model_id=ModelId; }
    result=esp_ble_mesh_config_client_set_state(&p,&set);
  }
  if (result == ESP_OK) {
    if (!configRetry.submitted(now)) return false;
    attempts = configRetry.attempts();
    sentAt = now;
    state.configAttempt = attempts;
    state.configRetryWait = false;
  }
  ESP_LOGI("mesh", "config send: addr=%04x step=%u opcode=0x%06lx attempt=%u result=%d",
    state.target, unsigned(step), (unsigned long)p.opcode, unsigned(attempts + (result == ESP_OK ? 0 : 1)), int(result));
  return result == ESP_OK;
}
void configure(uint16_t address, uint32_t now) {
  configBegan=now;
  state.target=address; state.phase="CONFIGURING"; state.reason="NONE"; state.busy=true;
  step=1; attempts=0; state.configStep=step; state.configAttempt=0; state.configRetryWait=false;
  configRetry.beginStep();
  if (!sendStep(now)) fail("CONFIG_SEND");
}
}
bool managementInit(uint16_t address) {
  primary=address; events=xQueueCreate(24,sizeof(Event)); if (!events) return false;
  extern esp_ble_mesh_client_t *managementConfigClient(); client=managementConfigClient();
  if (esp_ble_mesh_register_config_client_callback(configCallback) != ESP_OK) return false;
  // Older field builds used the Master as a Mesh Node through an external
  // provisioning tool.  A phone must not require a separate migration step:
  // clear only the persisted Mesh role/credentials and reboot once so the
  // normal Provisioner setup below can run. Application NVS (PIN, pump
  // settings, tasks and node records) is untouched by local reset.
  if (esp_ble_mesh_node_is_provisioned()) {
    ESP_LOGW("mesh", "legacy Node role detected; clearing Mesh credentials and rebooting automatically");
    if (esp_ble_mesh_node_local_reset() != ESP_OK) {
      state.phase="FAILED"; state.reason="AUTO_MIGRATION_FAILED"; state.busy=false; return true;
    }
    state.phase="AUTO_MIGRATING"; state.reason="REBOOT_REQUIRED"; state.busy=true;
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return true;
  }
  const uint8_t match[] = {0xdd,0xdd,0x50,0x4d};
  if (esp_ble_mesh_provisioner_set_dev_uuid_match(match,sizeof(match),0,false) != ESP_OK) return false;
  if (esp_ble_mesh_provisioner_prov_enable(ESP_BLE_MESH_PROV_ADV) != ESP_OK) { fail("ENABLE"); return true; }
  return true;
}
void managementProvisionEvent(esp_ble_mesh_prov_cb_event_t event, esp_ble_mesh_prov_cb_param_t *p) {
  if (!p) return;
  if (p) {
    switch (event) {
      case ESP_BLE_MESH_PROVISIONER_SET_DEV_UUID_MATCH_COMP_EVT:
        uuidMatchError = p->provisioner_set_dev_uuid_match_comp.err_code;
        ESP_LOGI("mesh", "discovery UUID filter complete: error=%d", uuidMatchError.load());
        break;
      case ESP_BLE_MESH_PROVISIONER_PROV_ENABLE_COMP_EVT:
        provisionerEnableError = p->provisioner_prov_enable_comp.err_code;
        ESP_LOGI("mesh", "Provisioner enable complete: error=%d", provisionerEnableError.load());
        break;
      case ESP_BLE_MESH_PROVISIONER_PROV_LINK_OPEN_EVT:
        ESP_LOGI("mesh", "PB-ADV provisioning link opened: bearer=0x%x",
          unsigned(p->provisioner_prov_link_open.bearer));
        break;
      case ESP_BLE_MESH_PROVISIONER_PROV_LINK_CLOSE_EVT:
        ESP_LOGW("mesh", "PB-ADV provisioning link closed: bearer=0x%x reason=0x%02x",
          unsigned(p->provisioner_prov_link_close.bearer),
          unsigned(p->provisioner_prov_link_close.reason));
        break;
      case ESP_BLE_MESH_PROVISIONER_PROV_COMPLETE_EVT:
        ESP_LOGI("mesh", "PB-ADV provisioning complete: addr=0x%04x elements=%u net_idx=%u",
          p->provisioner_prov_complete.unicast_addr,
          unsigned(p->provisioner_prov_complete.element_num),
          unsigned(p->provisioner_prov_complete.netkey_idx));
        break;
      case ESP_BLE_MESH_PROVISIONER_ADD_UNPROV_DEV_COMP_EVT:
        ESP_LOGI("mesh", "add unprovisioned device complete: err=%d",
          p->provisioner_add_unprov_dev_comp.err_code);
        break;
      default:
        break;
    }
  }
  Event e{};
  switch(event) {
    case ESP_BLE_MESH_PROVISIONER_PROV_ENABLE_COMP_EVT: e.kind=Kind::Enable; e.error=p->provisioner_prov_enable_comp.err_code; break;
    case ESP_BLE_MESH_NODE_PROV_DISABLE_COMP_EVT: e.kind=Kind::DisableNode; e.error=p->node_prov_disable_comp.err_code; break;
    case ESP_BLE_MESH_PROVISIONER_ADD_LOCAL_APP_KEY_COMP_EVT: e.kind=Kind::Key; e.error=p->provisioner_add_app_key_comp.err_code; break;
    case ESP_BLE_MESH_PROVISIONER_BIND_APP_KEY_TO_MODEL_COMP_EVT: e.kind=Kind::Bind; e.error=p->provisioner_bind_app_key_to_model_comp.err_code; break;
    case ESP_BLE_MESH_PROVISIONER_RECV_UNPROV_ADV_PKT_EVT: { const auto &s=p->provisioner_recv_unprov_adv_pkt;
      ++discoveryReceived;
      if (!(s.bearer & ESP_BLE_MESH_PROV_ADV) || memcmp(s.dev_uuid,"\xdd\xddPM",4) || s.dev_uuid[10]!=2) { ++discoveryRejected; return; }
      ++discoveryMatched;
      e.kind=Kind::Candidate; memcpy(e.candidate.uuid,s.dev_uuid,16); memcpy(e.candidate.addr,s.addr,6); e.candidate.type=s.addr_type; e.candidate.oob=s.oob_info; break; }
    case ESP_BLE_MESH_PROVISIONER_PROV_COMPLETE_EVT: e.kind=Kind::Provisioned; e.address=p->provisioner_prov_complete.unicast_addr; memcpy(e.candidate.uuid,p->provisioner_prov_complete.device_uuid,16); break;
    case ESP_BLE_MESH_PROVISIONER_PROV_LINK_CLOSE_EVT: e.kind=Kind::Closed; e.error=p->provisioner_prov_link_close.reason; break;
    case ESP_BLE_MESH_PROVISIONER_ADD_UNPROV_DEV_COMP_EVT: e.kind=Kind::Add; e.error=p->provisioner_add_unprov_dev_comp.err_code; break;
    case ESP_BLE_MESH_PROVISIONER_DELETE_DEV_COMP_EVT:
      e.kind=Kind::Cancelled;
      e.error=p->provisioner_delete_dev_comp.err_code; break;
    case ESP_BLE_MESH_PROVISIONER_DELETE_NODE_WITH_ADDR_COMP_EVT:
      e.kind=Kind::NodeDeleted; e.error=p->provisioner_delete_node_with_addr_comp.err_code;
      e.address=p->provisioner_delete_node_with_addr_comp.unicast_addr; break;
    default:return;
  }
  if (e.kind == Kind::Candidate) {
    if (!events || xQueueSend(events, &e, 0) != pdTRUE) { ++discoveryDropped; lost = true; }
  } else enqueue(e);
}
void MeshTransport::managementTick(uint32_t now) {
  if (!quarantined && cancelling && elapsed(now,cancelBegan,18000)) quarantine("CANCEL_WATCHDOG");
  if (!quarantined && (joining || step)) {
    const uint32_t total=provisionOperation ? 420000U : 330000U;
    const uint32_t age=now-operationBegan;
    state.remainingTotalMs=age>=total ? 0 : total-age;
    if (age>=total) quarantine("TOTAL_DEADLINE");
  }
  if (!quarantined && step) {
    const uint32_t budget=provisionOperation ? 420000U : 330000U;
    const uint32_t age=now-operationBegan;
    state.remainingTotalMs=age>=budget ? 0 : budget-age;
    if (age>=budget || elapsed(now,configBegan,330000)) quarantine("TOTAL_DEADLINE");
    else if (step<5 && configRetry.callbackExpired(now)) quarantine("CALLBACK_WATCHDOG");
    else if (step<5 && configRetry.stepExpired(now)) quarantine("STEP_DEADLINE");
  }
  if (lost.exchange(false)) {
    quarantine("EVENT_OVERFLOW");
    if (removal.stage() != MeshRemoval::Stage::Idle && removal.stage() != MeshRemoval::Stage::Done) state.busy = true;
  }
  Event e; for (int budget=0; budget<8 && xQueueReceive(events,&e,0)==pdTRUE; ++budget) {
    if (quarantined) continue; // No late callback may reopen a quarantined SDK operation.
    switch(e.kind) {
      case Kind::Enable: {
        if (e.error) {
          // Older firmware persisted the Node role even without completing
          // provisioning. node_is_provisioned() is false in that case, but
          // ESP-IDF still rejects the Provisioner with -EIO (Mismatch role 1).
          // Release that unprovisioned role using the SDK, then retry once.
          if (e.error == -EIO && !roleRecoveryAttempted &&
              !esp_ble_mesh_node_is_provisioned() &&
              address() == 0) {
            roleRecoveryAttempted = true;
            roleRecoveryPending = true;
            ESP_LOGW("mesh", "releasing unprovisioned Node role before enabling Provisioner");
            if (esp_ble_mesh_node_prov_disable(esp_ble_mesh_prov_bearer_t(ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT)) != ESP_OK) {
              roleRecoveryPending = false; fail("ROLE_RELEASE");
            }
          } else fail("ENABLE_FAILED");
          break;
        }
        const uint8_t *existing=esp_ble_mesh_provisioner_get_local_app_key(0,0); uint8_t key[16];
        if (existing) { if (esp_ble_mesh_provisioner_bind_app_key_to_local_model(primary,0,ModelId,CompanyId)!=ESP_OK) fail("LOCAL_BIND"); }
        else { esp_fill_random(key,16); if (esp_ble_mesh_provisioner_add_local_app_key(key,0,0)!=ESP_OK) fail("LOCAL_KEY"); } break; }
      case Kind::DisableNode:
        if (roleRecoveryPending) {
          roleRecoveryPending = false;
          if (e.error) fail("ROLE_RELEASE");
          else if (esp_ble_mesh_provisioner_prov_enable(ESP_BLE_MESH_PROV_ADV) != ESP_OK) fail("ENABLE_FAILED");
        }
        break;
      case Kind::Key: if (e.error || esp_ble_mesh_provisioner_bind_app_key_to_local_model(primary,0,ModelId,CompanyId)!=ESP_OK) fail("LOCAL_KEY"); break;
      case Kind::Bind: if (e.error) fail("LOCAL_BIND"); else { state.phase="IDLE"; state.capable=true; state.busy=false; } break;
      case Kind::Candidate:
        if (esp_ble_mesh_provisioner_get_node_with_uuid(e.candidate.uuid)) {
          recentDiscovery.forget(e.candidate.uuid); ++discoveryKnown; break;
        }
        if (!resetting) recentDiscovery.observe(e.candidate, now);
        if (!strcmp(state.phase,"SCANNING")) {
        bool exists=false; for (unsigned i=0;i<state.found;++i) if (!memcmp(state.candidates[i].uuid,e.candidate.uuid,16)) {
          state.candidates[i] = e.candidate; exists=true; break;
        }
        if (exists) ++discoveryDuplicates;
        else if (state.found>=8) ++discoveryFull;
        else state.candidates[state.found++]=e.candidate;
      } else ++discoveryOutsideScan;
      break;
      case Kind::Provisioned:
        stopDiscoveryTrace();
        recentDiscovery.forget(e.candidate.uuid);
        if (joining && !memcmp(selected,e.candidate.uuid,16)) {
        joining=false;
        for (unsigned i=0;i<state.found;++i) if (!memcmp(selected,state.candidates[i].uuid,16)) {
          for (unsigned j=i+1;j<state.found;++j) state.candidates[j-1]=state.candidates[j];
          --state.found; break;
        }
        if (cancelling) { state.target=e.address; } else configure(e.address,now);
      } break;
      case Kind::Closed: if (joining && e.error && !cancelling) cancelJoin("PROVISION_FAILED"); break;
      case Kind::Add: if (joining && e.error && !cancelling) cancelJoin("PROVISION_START"); break;
      case Kind::Cancelled: if (cancelling) {
        cancelling=false;
        if (e.error) { quarantine("PROVISION_CANCEL_FAILED"); }
        else { joining=false; fail(cancelReason); }
      } break;
      case Kind::NodeDeleted:
        removal.deleted(e.address, !e.error);
        if (resetting && e.address == resetAddresses[resetIndex]) {
        if (e.error) {
          resetting = false; state.busy = false; state.phase = "FAILED"; state.reason = "RESET_DELETE_FAILED";
        } else if (++resetIndex < resetCount) {
          if (esp_ble_mesh_provisioner_delete_node_with_addr(resetAddresses[resetIndex]) != ESP_OK) {
            resetting = false; state.busy = false; state.phase = "FAILED"; state.reason = "RESET_DELETE_START";
          }
        } else {
          resetting = false; state.busy = false; state.phase = "RESET_DONE"; state.reason = "NONE"; state.found = 0; state.target = 0;
        }
      } break;
      case Kind::Config:
        if (quarantined) break;
        if (e.opcode == ESP_BLE_MESH_MODEL_OP_NODE_RESET) { removal.remoteReply(e.address, !e.error && e.valid); break; }
        if (step && step<5 && e.address==state.target && e.opcode==opcodeForStep()) {
        ESP_LOGI("mesh", "config reply: addr=%04x step=%u opcode=0x%06lx error=%d valid=%u",
          e.address, unsigned(step), (unsigned long)e.opcode, e.error, unsigned(e.valid));
        if (e.error) {
          if (e.error!=-1) { quarantine("CONFIG_STACK_ERROR"); break; }
          const auto result = configRetry.timeout(now);
          if (result == MeshConfigRetry::TimeoutResult::Exhausted) fail("CONFIG_TIMEOUT");
          else if (result == MeshConfigRetry::TimeoutResult::RetryScheduled) {
            state.configRetryWait = true;
            ESP_LOGW("mesh", "config retry deferred: addr=%04x step=%u attempt=%u delay_ms=%lu",
              state.target, unsigned(step), unsigned(attempts), (unsigned long)MeshConfigRetry::SarCleanupDelayMs);
          } else ESP_LOGW("mesh", "stale config timeout ignored: addr=%04x step=%u opcode=0x%06lx",
            e.address, unsigned(step), (unsigned long)e.opcode);
        }
        else if (!configRetry.response()) {
          ESP_LOGW("mesh", "stale config response ignored: addr=%04x step=%u opcode=0x%06lx",
            e.address, unsigned(step), (unsigned long)e.opcode);
        }
        else if (!e.valid) fail("CONFIG_REJECTED");
        else {
          ++step; attempts=0; state.configStep=step; state.configAttempt=0; state.configRetryWait=false;
          if(step==5) { state.phase="WAIT_HELLO"; sentAt=now; configRetry.beginStep(); }
          else { configRetry.beginStep(); if (!sendStep(now)) fail("CONFIG_SEND"); }
        }
      } break;
    }
  }
  removal.tick(now);
  if (removal.stage() == MeshRemoval::Stage::Pending) {
    state.phase = "REMOVE_PENDING"; state.reason = "REMOTE_UNCONFIRMED";
    state.busy = false; // Application's persistent removal journal still blocks jobs.
  }
  if (removal.stage() == MeshRemoval::Stage::DeleteReady) {
    removal.deleting(now);
    if (esp_ble_mesh_provisioner_delete_node_with_addr(removal.address()) != ESP_OK) removal.deleted(removal.address(), false);
  }
  if (removal.stage() == MeshRemoval::Stage::Cleanup) state.phase = "REMOVE_CLEANUP";
  if (removal.stage() == MeshRemoval::Stage::Failed && strcmp(state.phase,"FAILED")) {
    state.phase = "FAILED"; state.reason = "REMOVE_RESTART_REQUIRED"; state.busy = true;
  }
  if (!strcmp(state.phase,"SCANNING") && elapsed(now,began,15000)) {
    stopDiscoveryTrace(); state.phase="SCAN_DONE"; state.busy=false;
    logDiscoveryDiagnostics();
  }
  if (joining && !cancelling && elapsed(now,began,90000)) {
    cancelJoin("PROVISION_TIMEOUT");
  }
  if (step>0 && step<5 && state.configRetryWait && configRetry.retryDue(now)) {
    ESP_LOGI("mesh", "config retry ready: addr=%04x step=%u attempt=%u",
      state.target, unsigned(step), unsigned(attempts + 1));
    if (!sendStep(now)) fail("CONFIG_SEND");
  }
  state.configRetryRemainingMs = static_cast<uint16_t>(configRetry.remainingMs(now));
  if (step==5 && elapsed(now,sentAt,25000)) fail("HELLO_TIMEOUT");
}
bool MeshTransport::managementBusy() const { return state.busy; }
const MeshManagement &MeshTransport::management() const { return state; }
void MeshTransport::logDiscoveryDiagnostics() const {
  ESP_LOGI("mesh", "MESH_DISCOVERY received=%lu matched=%lu rejected=%lu dropped=%lu outside_scan=%lu duplicate=%lu known=%lu full=%lu seeded=%lu found=%u uuid_filter_error=%d enable_error=%d sdk_nodes=%u trace_build=%u",
    (unsigned long)discoveryReceived.load(), (unsigned long)discoveryMatched.load(),
    (unsigned long)discoveryRejected.load(), (unsigned long)discoveryDropped.load(),
    (unsigned long)discoveryOutsideScan, (unsigned long)discoveryDuplicates,
    (unsigned long)discoveryKnown, (unsigned long)discoveryFull, (unsigned long)discoverySeeded, unsigned(state.found),
    uuidMatchError.load(), provisionerEnableError.load(),
    unsigned(esp_ble_mesh_provisioner_get_prov_node_count()),
#if defined(PUMP_DISCOVERY_TRACE)
    1U
#else
    0U
#endif
  );
}
bool MeshTransport::scanSlaves(uint32_t request, uint32_t now) {
  if (!state.capable || state.busy) return false;
  startDiscoveryTrace();
  state.phase="SCANNING"; state.reason="NONE"; state.scan=request; state.operation=request; state.found=0; state.target=0; state.busy=true;
  const size_t fresh = recentDiscovery.copyFresh(state.candidates, 8, now);
  for (size_t i = 0; i < fresh; ++i) {
    if (esp_ble_mesh_provisioner_get_node_with_uuid(state.candidates[i].uuid)) {
      recentDiscovery.forget(state.candidates[i].uuid); ++discoveryKnown;
    } else state.candidates[state.found++] = state.candidates[i];
  }
  discoverySeeded += state.found;
  began=now;
  logDiscoveryDiagnostics();
  return true;
}
bool MeshTransport::joinSlave(const uint8_t id[16],uint32_t request,uint32_t now) {
  if (!state.capable || (state.busy && strcmp(state.phase,"SCANNING")) || esp_ble_mesh_provisioner_get_prov_node_count()>=ProductMaxNodes || esp_ble_mesh_provisioner_get_node_with_uuid(id)) return false;
  for (unsigned i=0;i<state.found;++i) { const auto &c=state.candidates[i]; if (memcmp(c.uuid,id,16)) continue;
    startDiscoveryTrace();
    esp_ble_mesh_unprov_dev_add_t dev{}; memcpy(dev.uuid,c.uuid,16); memcpy(dev.addr,c.addr,6); dev.addr_type=esp_ble_mesh_addr_type_t(c.type); dev.oob_info=c.oob; dev.bearer=ESP_BLE_MESH_PROV_ADV;
    if (esp_ble_mesh_provisioner_add_unprov_dev(&dev,ADD_DEV_RM_AFTER_PROV_FLAG|ADD_DEV_START_PROV_NOW_FLAG|ADD_DEV_FLUSHABLE_DEV_FLAG)!=ESP_OK) { stopDiscoveryTrace(); return false; }
    state.phase="JOINING"; state.reason="NONE"; state.busy=true; state.operation=request; state.target=0; joining=true; began=now; operationBegan=now; provisionOperation=true; memcpy(selected,id,16); return true;
  }
  return false;
}
bool MeshTransport::migrateLegacyNetwork() {
  if (!esp_ble_mesh_node_is_provisioned()) return false;
  const esp_err_t result = esp_ble_mesh_node_local_reset();
  if (result != ESP_OK) {
    ESP_LOGE("mesh", "legacy network migration reset failed: %s (0x%x)",
      esp_err_to_name(result), unsigned(result));
    return false;
  }
  ESP_LOGW("mesh", "legacy Mesh Node credentials and role cleared; reboot required for Provisioner init");
  return true;
}
bool MeshTransport::resetProvisionedSlaves(uint32_t now) {
  if (!state.capable || state.busy || esp_ble_mesh_node_is_provisioned()) return false;
  recentDiscovery.clear();
  // Reset is a complete local Provisioner reset. Do not filter by the
  // application's UUID marker: a previous/external tool may have left nodes
  // in the persisted Provisioner table and those credentials must be removed
  // as well.
  resetCount = uint8_t(provisionedAddresses(resetAddresses, 32));
  resetIndex = 0;
  resetting = true;
  state.phase = "RESETTING"; state.reason = "NONE"; state.busy = true; state.operation = now; state.target = 0; state.found = 0;
  if (!resetCount) {
    resetting = false; state.busy = false; state.phase = "RESET_DONE"; return true;
  }
  if (esp_ble_mesh_provisioner_delete_node_with_addr(resetAddresses[0]) != ESP_OK) {
    resetting = false; state.busy = false; state.phase = "FAILED"; state.reason = "RESET_DELETE_START"; return false;
  }
  return true;
}
bool MeshTransport::retrySlave(uint16_t address,uint32_t request,uint32_t now) {
  if (!state.capable || state.busy || !ownsSlave(address)) return false;
  operationBegan=now; provisionOperation=false; state.recoveryAction="NONE";
  state.operation=request; configure(address,now); return true;
}
bool MeshTransport::removeSlave(uint16_t address, uint32_t request, uint32_t now) {
  if (!state.capable || state.busy || !unicast(address) || address == primary) return false;
  const auto *node = esp_ble_mesh_provisioner_get_node_with_addr(address);
  if (node && !ownsSlave(address)) return false;
  step = 0; joining = cancelling = false;
  state.operation = request; state.target = address; state.found = 0;
  state.phase = "REMOVING"; state.reason = "NONE"; state.busy = true;
  removal.begin(address, request, now, node != nullptr);
  if (node) {
    esp_ble_mesh_client_common_param_t p{};
    p.model = client->model; p.opcode = ESP_BLE_MESH_MODEL_OP_NODE_RESET;
    p.ctx.addr = address; p.ctx.net_idx = 0; p.ctx.app_idx = 0; p.ctx.send_ttl = 3; p.msg_timeout = 4000;
    esp_ble_mesh_cfg_client_set_state_t set{};
    if (esp_ble_mesh_config_client_set_state(&p, &set) != ESP_OK) removal.remoteReply(address, false);
  }
  return true;
}
void MeshTransport::finishSlaveRemoval(bool saved) {
  if (removal.stage() != MeshRemoval::Stage::Cleanup) return;
  removal.complete(saved);
  state.phase = saved ? "REMOVE_DONE" : "FAILED";
  state.reason = saved ? (removal.remoteReset() ? "REMOTE_RESET" : "LOCAL_ONLY") : "REMOVE_STORAGE";
  state.busy = !saved;
}
void MeshTransport::slaveHello(uint16_t address) {
  if (quarantined) return;
  if (step==5 && state.target==address) { step=0; state.phase="READY"; state.reason="NONE"; state.busy=false; }
}
bool MeshTransport::ownsSlave(uint16_t address) const {
  if (!state.capable) return false;
  const auto node=esp_ble_mesh_provisioner_get_node_with_addr(address);
  return node && node->unicast_addr==address && node->net_idx==0 && !memcmp(node->dev_uuid,"\xdd\xddPM",4) && node->dev_uuid[10]==2;
}
size_t MeshTransport::pairedAddresses(uint16_t *out,size_t capacity) const {
  size_t count=0;
  if (!state.capable) return 0;
  const auto table=esp_ble_mesh_provisioner_get_node_table_entry();
  if (table) for (unsigned i=0;i<CONFIG_BLE_MESH_MAX_PROV_NODES && count<capacity;++i) if (table[i] && ownsSlave(table[i]->unicast_addr)) out[count++]=table[i]->unicast_addr;
  return count;
}
size_t MeshTransport::provisionedAddresses(uint16_t *out,size_t capacity) const {
  size_t count=0;
  if (!state.capable || !out || !capacity) return 0;
  const auto table=esp_ble_mesh_provisioner_get_node_table_entry();
  if (!table) return 0;
  for (unsigned i=0;i<CONFIG_BLE_MESH_MAX_PROV_NODES && count<capacity;++i) {
    const auto *node=table[i];
    if (node && node->unicast_addr && node->unicast_addr <= 0x7fff)
      out[count++]=node->unicast_addr;
  }
  return count;
}
}
