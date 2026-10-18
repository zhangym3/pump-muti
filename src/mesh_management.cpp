#include "mesh_transport.h"
#include "mesh_management_internal.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/task.h"
#include <cstring>
#include <cerrno>
namespace pump {
namespace {
MeshManagement state;
enum class Kind : uint8_t { Enable, DisableNode, Key, Bind, Candidate, Provisioned, Closed, Add, Cancelled, ResetDeleted, Config };
struct Event { Kind kind{}; int error = 0; uint32_t opcode = 0; uint16_t address = 0; MeshCandidate candidate{}; bool valid = false; };
QueueHandle_t events;
std::atomic<bool> lost{false};
uint16_t primary; uint32_t began, sentAt; uint8_t step, attempts;
bool joining = false, cancelling = false; uint8_t selected[16];
bool roleRecoveryAttempted = false, roleRecoveryPending = false;
bool resetting = false;
uint16_t resetAddresses[32] = {};
uint8_t resetCount = 0, resetIndex = 0;
esp_ble_mesh_client_t *client = nullptr;
void enqueue(const Event &event) { if (events && xQueueSend(events, &event, 0) != pdTRUE) lost = true; }
void fail(const char *reason) { state.phase = "FAILED"; state.reason = reason; state.busy = joining; step = 0; ESP_LOGW("mesh", "management failed: %s addr=%04x", reason, state.target); }
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
    default: return;
  }
  enqueue(e);
}
uint32_t opcodeForStep() {
  switch(step) { case 1:return ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_GET; case 2:return ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD; case 3:return ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND; case 4:return ESP_BLE_MESH_MODEL_OP_MODEL_SUB_ADD; default:return 0; }
}
bool sendStep(uint32_t now) {
  esp_ble_mesh_client_common_param_t p{}; p.model = client->model; p.opcode = opcodeForStep();
  p.ctx.addr = state.target; p.ctx.net_idx = 0; p.ctx.app_idx = 0; p.ctx.send_ttl = 3; p.msg_timeout = 4000;
  esp_err_t result;
  if (step == 1) { esp_ble_mesh_cfg_client_get_state_t get{}; get.comp_data_get.page = 0; result = esp_ble_mesh_config_client_get_state(&p, &get); }
  else {
    esp_ble_mesh_cfg_client_set_state_t set{};
    if (step == 2) { const auto key = esp_ble_mesh_provisioner_get_local_app_key(0,0); if (!key) return false; memcpy(set.app_key_add.app_key,key,16); set.app_key_add.net_idx=0; set.app_key_add.app_idx=0; }
    if (step == 3) { set.model_app_bind.element_addr=state.target; set.model_app_bind.model_app_idx=0; set.model_app_bind.company_id=CompanyId; set.model_app_bind.model_id=ModelId; }
    if (step == 4) { set.model_sub_add.element_addr=state.target; set.model_sub_add.sub_addr=0xc001; set.model_sub_add.company_id=CompanyId; set.model_sub_add.model_id=ModelId; }
    result=esp_ble_mesh_config_client_set_state(&p,&set);
  }
  sentAt=now; ++attempts; return result == ESP_OK;
}
void configure(uint16_t address, uint32_t now) {
  state.target=address; state.phase="CONFIGURING"; state.reason="NONE"; state.busy=true; step=1; attempts=0; if (!sendStep(now)) fail("CONFIG_SEND");
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
  if (p) {
    switch (event) {
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
      if (!(s.bearer & ESP_BLE_MESH_PROV_ADV) || memcmp(s.dev_uuid,"\xdd\xddPM",4) || s.dev_uuid[10]!=2) return;
      e.kind=Kind::Candidate; memcpy(e.candidate.uuid,s.dev_uuid,16); memcpy(e.candidate.addr,s.addr,6); e.candidate.type=s.addr_type; e.candidate.oob=s.oob_info; break; }
    case ESP_BLE_MESH_PROVISIONER_PROV_COMPLETE_EVT: e.kind=Kind::Provisioned; e.address=p->provisioner_prov_complete.unicast_addr; memcpy(e.candidate.uuid,p->provisioner_prov_complete.device_uuid,16); break;
    case ESP_BLE_MESH_PROVISIONER_PROV_LINK_CLOSE_EVT: e.kind=Kind::Closed; e.error=p->provisioner_prov_link_close.reason; break;
    case ESP_BLE_MESH_PROVISIONER_ADD_UNPROV_DEV_COMP_EVT: e.kind=Kind::Add; e.error=p->provisioner_add_unprov_dev_comp.err_code; break;
    case ESP_BLE_MESH_PROVISIONER_DELETE_DEV_COMP_EVT:
      e.kind=resetting ? Kind::ResetDeleted : Kind::Cancelled;
      e.error=p->provisioner_delete_dev_comp.err_code; break;
    default:return;
  } enqueue(e);
}
void MeshTransport::managementTick(uint32_t now) {
  if (lost.exchange(false)) fail("EVENT_OVERFLOW");
  Event e; for (int budget=0; budget<8 && xQueueReceive(events,&e,0)==pdTRUE; ++budget) {
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
      case Kind::Candidate: if (!strcmp(state.phase,"SCANNING")) {
        bool exists=false; for (unsigned i=0;i<state.found;++i) if (!memcmp(state.candidates[i].uuid,e.candidate.uuid,16)) exists=true;
        if (!exists && state.found<8 && !esp_ble_mesh_provisioner_get_node_with_uuid(e.candidate.uuid)) state.candidates[state.found++]=e.candidate; } break;
      case Kind::Provisioned: if (joining && !memcmp(selected,e.candidate.uuid,16)) {
        joining=false;
        for (unsigned i=0;i<state.found;++i) if (!memcmp(selected,state.candidates[i].uuid,16)) {
          for (unsigned j=i+1;j<state.found;++j) state.candidates[j-1]=state.candidates[j];
          --state.found; break;
        }
        if (cancelling) { state.target=e.address; } else configure(e.address,now);
      } break;
      case Kind::Closed: if (joining && e.error && !cancelling) { joining=false; fail("PROVISION_FAILED"); } break;
      case Kind::Add: if (joining && e.error) { joining=false; fail("PROVISION_START"); } break;
      case Kind::Cancelled: if (cancelling) {
        cancelling=false;
        if (e.error) { state.phase="FAILED"; state.reason="RESTART_REQUIRED"; state.busy=true; }
        else { joining=false; fail("PROVISION_TIMEOUT"); }
      } break;
      case Kind::ResetDeleted: if (resetting) {
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
      case Kind::Config: if (step && step<5 && e.address==state.target && e.opcode==opcodeForStep()) {
        if (e.error) { if (attempts>=3 || !sendStep(now)) fail("CONFIG_TIMEOUT"); }
        else if (!e.valid) fail("CONFIG_REJECTED");
        else { ++step; attempts=0; if(step==5) { state.phase="WAIT_HELLO"; sentAt=now; } else if (!sendStep(now)) fail("CONFIG_SEND"); }
      } break;
    }
  }
  if (!strcmp(state.phase,"SCANNING") && elapsed(now,began,15000)) { state.phase="SCAN_DONE"; state.busy=false; }
  if (joining && !cancelling && elapsed(now,began,90000)) {
    esp_ble_mesh_device_delete_t dev{}; dev.flag=DEL_DEV_UUID_FLAG; memcpy(dev.uuid,selected,16);
    cancelling=true; state.phase="CANCELLING"; state.reason="PROVISION_TIMEOUT";
    if (esp_ble_mesh_provisioner_delete_dev(&dev)!=ESP_OK) { state.phase="FAILED"; state.reason="RESTART_REQUIRED"; }
  }
  if (step>0 && step<5 && elapsed(now,sentAt,7000)) { if (attempts>=3 || !sendStep(now)) fail("CONFIG_TIMEOUT"); }
  if (step==5 && elapsed(now,sentAt,25000)) fail("HELLO_TIMEOUT");
}
bool MeshTransport::managementBusy() const { return state.busy; }
const MeshManagement &MeshTransport::management() const { return state; }
bool MeshTransport::scanSlaves(uint32_t request, uint32_t now) {
  if (!state.capable || state.busy) return false;
  state.phase="SCANNING"; state.reason="NONE"; state.scan=request; state.operation=request; state.found=0; state.target=0; state.busy=true;
  began=now;
  return true;
}
bool MeshTransport::joinSlave(const uint8_t id[16],uint32_t request,uint32_t now) {
  if (!state.capable || (state.busy && strcmp(state.phase,"SCANNING")) || esp_ble_mesh_provisioner_get_prov_node_count()>=32 || esp_ble_mesh_provisioner_get_node_with_uuid(id)) return false;
  for (unsigned i=0;i<state.found;++i) { const auto &c=state.candidates[i]; if (memcmp(c.uuid,id,16)) continue;
    esp_ble_mesh_unprov_dev_add_t dev{}; memcpy(dev.uuid,c.uuid,16); memcpy(dev.addr,c.addr,6); dev.addr_type=esp_ble_mesh_addr_type_t(c.type); dev.oob_info=c.oob; dev.bearer=ESP_BLE_MESH_PROV_ADV;
    if (esp_ble_mesh_provisioner_add_unprov_dev(&dev,ADD_DEV_RM_AFTER_PROV_FLAG|ADD_DEV_START_PROV_NOW_FLAG|ADD_DEV_FLUSHABLE_DEV_FLAG)!=ESP_OK) return false;
    state.phase="JOINING"; state.reason="NONE"; state.busy=true; state.operation=request; state.target=0; joining=true; began=now; memcpy(selected,id,16); return true;
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
  state.operation=request; configure(address,now); return true;
}
void MeshTransport::slaveHello(uint16_t address) {
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
