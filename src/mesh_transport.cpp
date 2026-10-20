#include "mesh_transport.h"
#include "mesh_management_internal.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_tracked.h"
#include "esp_timer.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_local_data_operation_api.h"
#include "freertos/task.h"
#include <cstdio>
#include <new>

namespace pump {
namespace {
MeshTransport *instance = nullptr;
uint8_t uuid[16] = {0xDD, 0xDD, 0x50, 0x4D};
esp_ble_mesh_cfg_srv_t config;
esp_ble_mesh_model_op_t operations[] = {
  {ESP_BLE_MESH_MODEL_OP_3(0xC0, CompanyId), 39, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xC1, CompanyId), 8, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xC2, CompanyId), 8, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xC3, CompanyId), 9, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xC4, CompanyId), 8, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xC5, CompanyId), 27, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xC6, CompanyId), 0, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xC7, CompanyId), 12, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xC8, CompanyId), 37, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xC9, CompanyId), 46, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xCA, CompanyId), 13, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xCB, CompanyId), 6, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xCC, CompanyId), 4, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xCD, CompanyId), 14, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xCE, CompanyId), 8, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xCF, CompanyId), 10, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xD0, CompanyId), 4, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xD1, CompanyId), 12, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xD2, CompanyId), 8, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xD3, CompanyId), 8, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xD4, CompanyId), 8, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xD5, CompanyId), 8, 0},
  ESP_BLE_MESH_MODEL_OP_END
};
esp_ble_mesh_client_t meshConfigClient;
esp_ble_mesh_model_t sigModels[] = { ESP_BLE_MESH_MODEL_CFG_SRV(&config), ESP_BLE_MESH_MODEL_CFG_CLI(&meshConfigClient) };
esp_ble_mesh_model_t vendorModels[] = { ESP_BLE_MESH_VENDOR_MODEL(CompanyId, ModelId, operations, nullptr, nullptr) };
esp_ble_mesh_elem_t elements[] = { ESP_BLE_MESH_ELEMENT(0, sigModels, vendorModels) };
esp_ble_mesh_comp_t composition;
esp_ble_mesh_prov_t provisioning{};
void provisionCb(esp_ble_mesh_prov_cb_event_t event, esp_ble_mesh_prov_cb_param_t *p) {
  managementProvisionEvent(event, p);
  if (event == ESP_BLE_MESH_NODE_SET_UNPROV_DEV_NAME_COMP_EVT && p->node_set_unprov_dev_name_comp.err_code) {
    ESP_LOGE("mesh", "device name update failed: %d", p->node_set_unprov_dev_name_comp.err_code);

  }
}
void modelCb(esp_ble_mesh_model_cb_event_t event, esp_ble_mesh_model_cb_param_t *p) {
  if (!instance || !p || event != ESP_BLE_MESH_MODEL_OPERATION_EVT) return;
  const auto &m = p->model_operation;
  if (!m.ctx || m.ctx->net_idx != 0 || m.ctx->app_idx != 0 ||
      m.length > sizeof(Packet::bytes) || (m.opcode & 0xFFFF) != CompanyId) return;
  const uint8_t code = uint8_t(m.opcode >> 16);
  if (code < uint8_t(Op::Prepare) || code > uint8_t(Op::StartOK)) return;
  auto in = packet(Op(code), m.ctx->addr); in.size = uint8_t(m.length);
  memcpy(in.bytes, m.msg, in.size); instance->incoming(in);
}
bool urgent(const Packet &p) {
  return p.op == Op::TargetClosed || p.op == Op::TargetClosedAck || p.op == Op::PrepareOK || p.op == Op::StartOK || p.op == Op::Close || p.op == Op::Lease || p.op == Op::LeaseAck ||
    p.op == Op::Result || p.op == Op::ResultAck || p.op == Op::CommandAck ||
    (p.op == Op::Request && p.size && p.bytes[0] == uint8_t(Action::Stop));
}
}
esp_ble_mesh_client_t *managementConfigClient() { return &meshConfigClient; }
bool MeshTransport::init(uint16_t masterAddress) {
  instance = this;
  rx_ = xQueueCreate(48, sizeof(Packet)); urgentRx_ = xQueueCreate(12, sizeof(Packet));
  if (!rx_ || !urgentRx_) return false;
  if (esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT) != ESP_OK) return false;
  esp_bt_controller_config_t bt = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  if (esp_bt_controller_init(&bt) != ESP_OK || esp_bt_controller_enable(ESP_BT_MODE_BLE) != ESP_OK ||
      esp_bluedroid_init() != ESP_OK || esp_bluedroid_enable() != ESP_OK) return false;
  if (esp_read_mac(uuid + 4, ESP_MAC_BT) != ESP_OK) return false;
  uuid[10] = 1;
  snprintf(deviceName_, sizeof(deviceName_), "PUMP_MASTER_%02X%02X", uuid[8], uuid[9]);
  config.net_transmit = ESP_BLE_MESH_TRANSMIT(1, 20);
  config.relay = ESP_BLE_MESH_RELAY_DISABLED;
  config.relay_retransmit = ESP_BLE_MESH_TRANSMIT(1, 20);
  config.beacon = ESP_BLE_MESH_BEACON_ENABLED;
  config.gatt_proxy = ESP_BLE_MESH_GATT_PROXY_ENABLED;
  config.friend_state = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED; config.default_ttl = 3;
  composition.cid = CompanyId; composition.element_count = 1; composition.elements = elements;
  provisioning.uuid = uuid;
  new (&provisioning) esp_ble_mesh_prov_t{ .uuid = uuid, .prov_uuid = uuid,
    .prov_unicast_addr = masterAddress, .prov_start_address = uint16_t(masterAddress + 1) };
  if (esp_ble_mesh_register_prov_callback(provisionCb) != ESP_OK ||
      esp_ble_mesh_register_custom_model_callback(modelCb) != ESP_OK ||
      esp_ble_mesh_init(&provisioning, &composition) != ESP_OK) return false;
  // Mesh initializes the shared Bluedroid ATT limit to 23. Restore the business
  // MTU after Mesh init, before enabling provisioning or registering the gateway.
  const auto mtuResult = esp_ble_gatt_set_local_mtu(185);
  if (mtuResult != ESP_OK) {
    ESP_LOGE("mesh", "cannot set local ATT MTU=185: %s", esp_err_to_name(mtuResult));
    return false;
  }
  ESP_LOGI("mesh", "local ATT MTU=185 requested after Mesh init");
  // Set the Mesh scan response as well as the GAP name before enabling provisioning.
  if (esp_ble_mesh_set_unprovisioned_device_name(deviceName_) != ESP_OK ||
      esp_ble_gap_set_device_name(deviceName_) != ESP_OK) return false;
  if (!managementInit(masterAddress)) return false;
  return xTaskCreatePinnedToCore([](void *p) { static_cast<MeshTransport *>(p)->worker(); },
    "mesh_tx", 4096, this, 5, nullptr, 0) == pdPASS;
}
bool MeshTransport::ready() const {
  if (!address()) return false;
  for (const auto key : vendorModels[0].keys) if (key == 0) return true;
  return false;
}
uint16_t MeshTransport::address() const { return esp_ble_mesh_get_primary_element_address(); }
bool MeshTransport::send(const Packet &p) {
  if (!ready()) return false;
  portENTER_CRITICAL(&txLock_);
  const bool ok = tx_.enqueue(p, uint32_t(esp_timer_get_time()/1000));
  portEXIT_CRITICAL(&txLock_);
  return ok;
}
void MeshTransport::fence(JobKey job) {
  portENTER_CRITICAL(&txLock_); tx_.fence(job); portEXIT_CRITICAL(&txLock_);
}
void MeshTransport::retire(uint16_t peer, JobKey job, Op op) {
  portENTER_CRITICAL(&txLock_); tx_.retire(peer,job,op); portEXIT_CRITICAL(&txLock_);
}
bool MeshTransport::beginLeaseScope(JobKey job, uint32_t &floor) {
  portENTER_CRITICAL(&txLock_); const bool ok=tx_.beginLeaseScope(job,floor); portEXIT_CRITICAL(&txLock_); return ok;
}
MeshTxGovernor::Admission MeshTransport::prepare(const Packet &p) {
  portENTER_CRITICAL(&txLock_); const auto result=tx_.prepare(p,uint32_t(esp_timer_get_time()/1000)); portEXIT_CRITICAL(&txLock_); return result;
}
bool MeshTransport::healthy() const {
  portENTER_CRITICAL(&txLock_); const bool ok = tx_.healthy(); portEXIT_CRITICAL(&txLock_); return ok;
}
MeshTxGovernor::Metrics MeshTransport::metrics() const {
  portENTER_CRITICAL(&txLock_); const auto m = tx_.metrics(); portEXIT_CRITICAL(&txLock_); return m;
}
MeshTxGovernor::Diagnostics MeshTransport::diagnostics() const {
  portENTER_CRITICAL(&txLock_);
  const auto d = tx_.diagnostics(uint32_t(esp_timer_get_time()/1000));
  portEXIT_CRITICAL(&txLock_); return d;
}
bool MeshTransport::takeSubmission(MeshSubmission &out) {
  portENTER_CRITICAL(&txLock_);
  const bool ok = tx_.collect(uint32_t(esp_timer_get_time()/1000), out);
  portEXIT_CRITICAL(&txLock_); return ok;
}
void MeshTransport::trackedEvent(uint32_t ticket, uint8_t event, int error) {
  portENTER_CRITICAL(&txLock_); tx_.event(ticket,event,error,uint32_t(esp_timer_get_time()/1000)); portEXIT_CRITICAL(&txLock_);
}
extern "C" void esp_ble_mesh_tracked_send_event(uint32_t ticket, uint8_t event, int error) {
  if (instance) instance->trackedEvent(ticket,event,error);
}
void MeshTransport::incoming(const Packet &p) {
  if (!controlFormat(p)) return;
  // Master receives remote state and forwarded requests; peer authorization is checked by Application.
  if (p.op != Op::Hello && p.op != Op::Result && p.op != Op::Request &&
      p.op != Op::TargetClosed && p.op != Op::PrepareOK && p.op != Op::StartOK && p.op != Op::CommissionReply && p.op != Op::LeaseAck && p.op != Op::CommandAck && p.op != Op::Capabilities) return;
  if (xQueueSend(urgent(p) ? urgentRx_ : rx_, &p, 0) != pdTRUE && urgent(p)) overflow_ = true;
}
bool MeshTransport::receive(Packet &p) {
  return xQueueReceive(urgentRx_, &p, 0) == pdTRUE || xQueueReceive(rx_, &p, 0) == pdTRUE;
}
void MeshTransport::worker() {
  uint32_t memoryAt=uint32_t(esp_timer_get_time()/1000);
  for (;;) {
    MeshTxGovernor::Dispatch d;
    bool dispatch = false;
    if (ready()) {
      // This lock is the commit point shared with fence(). SDK/BTC may block;
      // an already committed dispatch remains tracked across a later stop.
      portENTER_CRITICAL(&txLock_);
      dispatch = tx_.next(uint32_t(esp_timer_get_time()/1000), d);
      portEXIT_CRITICAL(&txLock_);
    }
    if (dispatch) {
      esp_ble_mesh_msg_ctx_t ctx{};
      ctx.net_idx=0; ctx.app_idx=0; ctx.addr=d.packet.peer; ctx.send_ttl=3;
      const auto err = esp_ble_mesh_server_model_send_msg_tracked(&vendorModels[0], &ctx,
        ESP_BLE_MESH_MODEL_OP_3(uint8_t(d.packet.op), CompanyId), d.packet.size, d.packet.bytes, d.ticket);
      if (err != ESP_OK) trackedEvent(d.ticket, ESP_BLE_MESH_TRACKED_REJECTED, err);
    }
    for (unsigned i=0;i<4;++i) {
      MeshTxGovernor::Trace t;
      portENTER_CRITICAL(&txLock_); const bool found=tx_.takeTrace(t); portEXIT_CRITICAL(&txLock_);
      if (!found) break;
      ESP_LOGI("mesh_tx", "t=%lu ticket=%lu peer=%04x op=%02x event=%u job=%lu:%lu seq=%lu rev=%lu issued=%lu age=%lu queued=%lu wait=%lu seg=%u err=%d",
        (unsigned long)t.at,(unsigned long)t.ticket,t.peer,t.op,t.event,(unsigned long)t.job.epoch,
        (unsigned long)t.job.id,(unsigned long)t.sequence,(unsigned long)t.revision,(unsigned long)t.issuedAt,
        (unsigned long)(t.at-t.issuedAt),(unsigned long)t.queuedAt,(unsigned long)(t.issuedAt-t.queuedAt),t.segmented,t.error);
    }
    const uint32_t memoryNow=uint32_t(esp_timer_get_time()/1000);
    if (elapsed(memoryNow,memoryAt,10000)) {
      memoryAt=memoryNow;
      ESP_LOGI("runtime_mem", "mesh_stack_min_bytes=%lu",(unsigned long)uxTaskGetStackHighWaterMark(nullptr));
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}
}
