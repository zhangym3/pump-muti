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
  {ESP_BLE_MESH_MODEL_OP_3(0xC0, CompanyId), 28, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xC1, CompanyId), 8, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xC2, CompanyId), 12, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xC3, CompanyId), 9, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xC4, CompanyId), 8, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xC5, CompanyId), 27, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xC6, CompanyId), 18, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xC7, CompanyId), 12, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xC8, CompanyId), 37, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xC9, CompanyId), 46, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xCA, CompanyId), 9, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xCB, CompanyId), 6, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xCC, CompanyId), 4, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xCD, CompanyId), 14, 0},
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
  if (!instance || event != ESP_BLE_MESH_MODEL_OPERATION_EVT) return;
  const auto &m = p->model_operation;
  if (!m.ctx || m.ctx->net_idx != 0 || m.ctx->app_idx != 0 ||
      m.length > sizeof(Packet::bytes) || (m.opcode & 0xFFFF) != CompanyId) return;
  const uint8_t code = uint8_t(m.opcode >> 16);
  if (code < uint8_t(Op::Prepare) || code > uint8_t(Op::CommissionReply)) return;
  auto in = packet(Op(code), m.ctx->addr); in.size = uint8_t(m.length);
  memcpy(in.bytes, m.msg, in.size); instance->incoming(in);
}
bool urgent(const Packet &p) {
  return p.op == Op::Close || p.op == Op::Lease ||
    (p.op == Op::Request && p.size && p.bytes[0] == uint8_t(Action::Stop));
}
}
esp_ble_mesh_client_t *managementConfigClient() { return &meshConfigClient; }
bool MeshTransport::init(uint16_t masterAddress) {
  instance = this;
  rx_ = xQueueCreate(48, sizeof(Packet)); urgentRx_ = xQueueCreate(12, sizeof(Packet));
  tx_ = xQueueCreate(48, sizeof(Packet)); urgentTx_ = xQueueCreate(12, sizeof(Packet));
  if (!rx_ || !urgentRx_ || !tx_ || !urgentTx_) return false;
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
  if (!ready() || !p.peer || p.size > sizeof(p.bytes)) return false;
  const bool ok = xQueueSend(urgent(p) ? urgentTx_ : tx_, &p, 0) == pdTRUE;
  if (!ok && urgent(p)) overflow_ = true;
  return ok;
}
void MeshTransport::incoming(const Packet &p) {
  // Master receives remote state and forwarded requests; peer authorization is checked by Application.
  if (p.op != Op::Hello && p.op != Op::Status && p.op != Op::Summary && p.op != Op::Request &&
      p.op != Op::CommissionReply) return;
  if (xQueueSend(urgent(p) ? urgentRx_ : rx_, &p, 0) != pdTRUE && urgent(p)) overflow_ = true;
}
bool MeshTransport::receive(Packet &p) {
  return xQueueReceive(urgentRx_, &p, 0) == pdTRUE || xQueueReceive(rx_, &p, 0) == pdTRUE;
}
void MeshTransport::worker() {
  for (;;) {
    Packet p;
    if (xQueueReceive(urgentTx_, &p, 0) != pdTRUE && xQueueReceive(tx_, &p, pdMS_TO_TICKS(10)) != pdTRUE) continue;
    if (ready()) {
      esp_ble_mesh_msg_ctx_t ctx{}; ctx.net_idx = 0; ctx.app_idx = 0; ctx.addr = p.peer; ctx.send_ttl = 3;
      const auto err = esp_ble_mesh_server_model_send_msg(&vendorModels[0], &ctx,
        ESP_BLE_MESH_MODEL_OP_3(uint8_t(p.op), CompanyId), p.size, p.bytes);
      // Requests have application-level retries; do not block the control task.
      if (err != ESP_OK) ESP_LOGW("mesh", "send op=%02x err=%s", uint8_t(p.op), esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
}
