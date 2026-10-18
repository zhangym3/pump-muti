#include "control_gateway.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_log.h"
#include "esp_ble_mesh_ble_api.h"
#include "driver/uart.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace pump {
namespace {
ControlGateway *gateway = nullptr;
uint32_t pinCode = 0;
esp_gatt_if_t interface = ESP_GATT_IF_NONE;
uint16_t handles[6] = {}, connection = 0, mtu = 23;
uint8_t remote[6] = {};
std::atomic<bool> connected{false}, secure{false}, subscribed{false}, advertising{false}, pendingAdv{false};
uint8_t advIndex = 0;
portMUX_TYPE textMux = portMUX_INITIALIZER_UNLOCKED;
char latest[192] = "PUMP_MULTI_V1\n";
char deviceName[24] = {};
uint8_t serviceUuid[16] = {0x00,0x10,0x0E,0x53,0x70,0xA5,0xB7,0x8D,0x6E,0x4B,0x9C,0x2F,0x01,0x00,0x4D,0xA6};
uint8_t rxUuid[16] = {0x00,0x10,0x0E,0x53,0x70,0xA5,0xB7,0x8D,0x6E,0x4B,0x9C,0x2F,0x02,0x00,0x4D,0xA6};
uint8_t txUuid[16] = {0x00,0x10,0x0E,0x53,0x70,0xA5,0xB7,0x8D,0x6E,0x4B,0x9C,0x2F,0x03,0x00,0x4D,0xA6};
uint16_t primaryUuid = ESP_GATT_UUID_PRI_SERVICE, charUuid = ESP_GATT_UUID_CHAR_DECLARE, cccdUuid = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
uint8_t rxProperty = ESP_GATT_CHAR_PROP_BIT_WRITE, txProperty = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
uint8_t cccd[2] = {}, empty = 0;
esp_gatts_attr_db_t attributes[6];
void attr(size_t i, uint16_t uuidLen, uint8_t *uuid, uint16_t permission,
          uint16_t maximum, uint16_t length, uint8_t *value, bool automatic) {
  attributes[i].attr_control.auto_rsp = automatic ? ESP_GATT_AUTO_RSP : ESP_GATT_RSP_BY_APP;
  auto &a = attributes[i].att_desc; a.uuid_length = uuidLen; a.uuid_p = uuid; a.perm = permission;
  a.max_length = maximum; a.length = length; a.value = value;
}
void startAdvertising() {
  if (!pinCode || connected || advertising || pendingAdv.exchange(true)) return;
  esp_ble_mesh_ble_adv_param_t params{};
  params.interval = 0xA0; params.adv_type = ADV_TYPE_IND; params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
  params.duration = 120; params.period = 500; params.count = ESP_BLE_MESH_BLE_ADV_INFINITE;
  params.priority = ESP_BLE_MESH_BLE_ADV_PRIO_LOW;
  esp_ble_mesh_ble_adv_data_t data{};
  data.adv_data[0] = 2; data.adv_data[1] = 0x01; data.adv_data[2] = 0x06;
  data.adv_data[3] = 17; data.adv_data[4] = 0x07; memcpy(data.adv_data + 5, serviceUuid, 16);
  data.adv_data_len = 21;
  const size_t nameLength = strlen(deviceName);
  data.scan_rsp_data[0] = uint8_t(nameLength + 1); data.scan_rsp_data[1] = 0x09;
  memcpy(data.scan_rsp_data + 2, deviceName, nameLength);
  data.scan_rsp_data_len = uint8_t(nameLength + 2);
  ESP_LOGI("gateway", "BLE advertising request: name=%s interval=0x%04x adv_len=%u scan_rsp_len=%u",
    deviceName, unsigned(params.interval), unsigned(data.adv_data_len), unsigned(data.scan_rsp_data_len));
  const esp_err_t result = esp_ble_mesh_start_ble_advertising(&params, &data);
  if (result != ESP_OK) {
    pendingAdv = false;
    ESP_LOGE("gateway", "BLE advertising request failed: %s (0x%x)",
      esp_err_to_name(result), unsigned(result));
  }
}
void meshBleCb(esp_ble_mesh_ble_cb_event_t event, esp_ble_mesh_ble_cb_param_t *p) {
  if (event == ESP_BLE_MESH_START_BLE_ADVERTISING_COMP_EVT) {
    pendingAdv = false; advertising = p->start_ble_advertising_comp.err_code == 0;
    advIndex = p->start_ble_advertising_comp.index;
    if (advertising) {
      ESP_LOGI("gateway", "BLE advertising active: index=%u name=%s", unsigned(advIndex), deviceName);
    } else {
      ESP_LOGE("gateway", "BLE advertising rejected: index=%u err_code=%d",
        unsigned(advIndex), p->start_ble_advertising_comp.err_code);
    }
    if (connected && advertising) esp_ble_mesh_stop_ble_advertising(advIndex);
  } else if (event == ESP_BLE_MESH_STOP_BLE_ADVERTISING_COMP_EVT) {
    advertising = false;
    ESP_LOGI("gateway", "BLE advertising stopped: index=%u err_code=%d",
      unsigned(p->stop_ble_advertising_comp.index), p->stop_ble_advertising_comp.err_code);
    if (!connected) startAdvertising();
  }
}
bool sameRemote(const uint8_t *bda) { return connected && memcmp(remote, bda, 6) == 0; }
struct PeerAddress {
  char text[18];
  explicit PeerAddress(const uint8_t *bda) {
    snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
      unsigned(bda[0]), unsigned(bda[1]), unsigned(bda[2]), unsigned(bda[3]), unsigned(bda[4]), unsigned(bda[5]));
  }
};
void gapCb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *p) {
  if (event == ESP_GAP_BLE_SEC_REQ_EVT) {
    auto &request = p->ble_security.ble_req;
    const bool accept = sameRemote(request.bd_addr);
    const esp_err_t result = esp_ble_gap_security_rsp(request.bd_addr, accept);
    ESP_LOGI("gateway", "BLE security request: peer=%s accept=%d result=%s",
      PeerAddress(request.bd_addr).text, int(accept), esp_err_to_name(result));
  } else if (event == ESP_GAP_BLE_PASSKEY_REQ_EVT) {
    auto &request = p->ble_security.ble_req;
    const bool accept = sameRemote(request.bd_addr);
    const esp_err_t result = esp_ble_passkey_reply(request.bd_addr, accept, pinCode);
    ESP_LOGI("gateway", "BLE passkey request: peer=%s accept=%d result=%s",
      PeerAddress(request.bd_addr).text, int(accept), esp_err_to_name(result));
  } else if (event == ESP_GAP_BLE_PASSKEY_NOTIF_EVT && sameRemote(p->ble_security.key_notif.bd_addr)) {
    ESP_LOGI("gateway", "BLE PIN entry required on phone: peer=%s current PIN=%06lu",
      PeerAddress(p->ble_security.key_notif.bd_addr).text,
      static_cast<unsigned long>(p->ble_security.key_notif.passkey));
  } else if (event == ESP_GAP_BLE_KEY_EVT && sameRemote(p->ble_security.ble_key.bd_addr)) {
    // Log key types only, never encryption or identity key material.
    ESP_LOGI("gateway", "BLE key exchanged: peer=%s type=0x%02x",
      PeerAddress(p->ble_security.ble_key.bd_addr).text, unsigned(p->ble_security.ble_key.key_type));
  } else if (event == ESP_GAP_BLE_NC_REQ_EVT) {
    ESP_LOGW("gateway", "BLE numeric comparison rejected: peer=%s; PIN entry required",
      PeerAddress(p->ble_security.key_notif.bd_addr).text);
    esp_ble_confirm_reply(p->ble_security.key_notif.bd_addr, false);
  } else if (event == ESP_GAP_BLE_AUTH_CMPL_EVT) {
    const auto &auth = p->ble_security.auth_cmpl;
    const bool active = sameRemote(auth.bd_addr);
    ESP_LOGI("gateway", "BLE auth complete: peer=%s active=%d success=%d reason=0x%02x auth_mode=0x%02x addr_type=%u",
      PeerAddress(auth.bd_addr).text, int(active), int(auth.success), unsigned(auth.fail_reason),
      unsigned(auth.auth_mode), unsigned(auth.addr_type));
    if (!active) return;
    secure = auth.success && (auth.auth_mode & ESP_LE_AUTH_REQ_MITM);
    if (secure) {
      ESP_LOGI("gateway", "BLE authenticated encryption ready: conn=%u bonded_peers=%d",
        unsigned(connection), esp_ble_get_bond_device_num());
    } else {
      ESP_LOGE("gateway", "BLE PIN authentication failed; closing conn=%u", unsigned(connection));
      esp_ble_gatts_close(interface, connection);
    }
  }
}
void gattsCb(esp_gatts_cb_event_t event, esp_gatt_if_t iface, esp_ble_gatts_cb_param_t *p) {
  if (event == ESP_GATTS_REG_EVT) {
    ESP_LOGI("gateway", "GATT app register: status=%d app_id=0x%04x iface=%u",
      p->reg.status, unsigned(p->reg.app_id), unsigned(iface));
    if (p->reg.status != ESP_GATT_OK || p->reg.app_id != 0x5055) {
      ESP_LOGE("gateway", "GATT app registration rejected");
      return;
    }
    interface = iface; esp_ble_gatts_create_attr_tab(attributes, iface, 6, 0); return;
  }
  if (iface != interface && iface != ESP_GATT_IF_NONE) return;
  if (event == ESP_GATTS_CREAT_ATTR_TAB_EVT) {
    if (p->add_attr_tab.status != ESP_GATT_OK || p->add_attr_tab.num_handle != 6) {
      ESP_LOGE("gateway", "GATT attribute table failed: status=%d handles=%u",
        p->add_attr_tab.status, unsigned(p->add_attr_tab.num_handle));
    } else {
      memcpy(handles, p->add_attr_tab.handles, sizeof(handles));
      ESP_LOGI("gateway", "GATT attribute table ready: service_handle=0x%04x", unsigned(handles[0]));
      const esp_err_t result = esp_ble_gatts_start_service(handles[0]);
      if (result != ESP_OK) ESP_LOGE("gateway", "GATT service start request failed: %s (0x%x)",
        esp_err_to_name(result), unsigned(result));
    }
  } else if (event == ESP_GATTS_START_EVT) {
    if (p->start.status != ESP_GATT_OK) {
      ESP_LOGE("gateway", "GATT service start failed: status=%d", p->start.status);
    } else {
      ESP_LOGI("gateway", "GATT service started; starting BLE advertising");
      startAdvertising();
    }
  } else if (event == ESP_GATTS_CONNECT_EVT) {
    if (connected && p->connect.conn_id != connection) { esp_ble_gatts_close(interface, p->connect.conn_id); return; }
    connection = p->connect.conn_id; memcpy(remote, p->connect.remote_bda, 6);
    connected = true; secure = false; subscribed = false; mtu = 23;
    ESP_LOGI("gateway", "BLE connected: peer=%s conn=%u; requesting authenticated encryption",
      PeerAddress(remote).text, unsigned(connection));
    if (advertising) esp_ble_mesh_stop_ble_advertising(advIndex);
    // Master initiates security; the phone waits for bonding before GATT setup.
    const esp_err_t result = esp_ble_set_encryption(remote, ESP_BLE_SEC_ENCRYPT_MITM);
    ESP_LOGI("gateway", "BLE encryption request: result=%s (0x%x)", esp_err_to_name(result), unsigned(result));
    if (result != ESP_OK) esp_ble_gatts_close(interface, connection);
  } else if (event == ESP_GATTS_DISCONNECT_EVT && p->disconnect.conn_id == connection) {
    ESP_LOGW("gateway", "BLE disconnected: peer=%s conn=%u reason=0x%02x secure=%d subscribed=%d",
      PeerAddress(p->disconnect.remote_bda).text, unsigned(connection), unsigned(p->disconnect.reason),
      int(secure.load()), int(subscribed.load()));
    connected = secure = subscribed = false;
    startAdvertising(); // The supply task is deliberately independent of phone connection lifetime.
  } else if (event == ESP_GATTS_MTU_EVT && p->mtu.conn_id == connection) {
    mtu = p->mtu.mtu;
    ESP_LOGI("gateway", "negotiated ATT MTU=%u conn=%u", unsigned(mtu), unsigned(connection));
  } else if (event == ESP_GATTS_WRITE_EVT) {
    auto status = ESP_GATT_OK;
    if (p->write.conn_id != connection || !secure) status = ESP_GATT_INSUF_AUTHENTICATION;
    else if (p->write.is_prep || p->write.offset) status = ESP_GATT_REQ_NOT_SUPPORTED;
    else if (p->write.handle == handles[2]) {
      if (!gateway->submit(reinterpret_cast<const char *>(p->write.value), p->write.len, true)) status = ESP_GATT_INVALID_ATTR_LEN;
    } else if (p->write.handle == handles[5] && p->write.len == 2) {
      if (p->write.value[1] || p->write.value[0] > 1) status = ESP_GATT_REQ_NOT_SUPPORTED;
      else {
        subscribed = p->write.value[0] == 1;
        ESP_LOGI("gateway", "BLE notifications: conn=%u enabled=%d secure=%d",
          unsigned(connection), int(subscribed.load()), int(secure.load()));
      }
    } else status = ESP_GATT_WRITE_NOT_PERMIT;
    if (p->write.need_rsp) esp_ble_gatts_send_response(iface, p->write.conn_id, p->write.trans_id, status, nullptr);
  } else if (event == ESP_GATTS_READ_EVT && (p->read.handle == handles[4] || p->read.handle == handles[5])) {
    esp_gatt_rsp_t response{}; response.attr_value.handle = p->read.handle; response.attr_value.offset = p->read.offset;
    char copy[192]; portENTER_CRITICAL(&textMux); memcpy(copy, latest, sizeof(copy)); portEXIT_CRITICAL(&textMux);
    const uint8_t descriptor[2] = {uint8_t(subscribed.load() ? 1 : 0), 0};
    const bool isDescriptor = p->read.handle == handles[5];
    const size_t len = isDescriptor ? 2 : strlen(copy); esp_gatt_status_t status = ESP_GATT_OK;
    if (p->read.conn_id != connection || !secure) status = ESP_GATT_INSUF_AUTHENTICATION;
    else if (p->read.offset > len) status = ESP_GATT_INVALID_OFFSET;
    else {
      response.attr_value.len = std::min<size_t>(len - p->read.offset, mtu - 1);
      const uint8_t *value = isDescriptor ? descriptor : reinterpret_cast<const uint8_t *>(copy);
      memcpy(response.attr_value.value, value + p->read.offset, response.attr_value.len);
    }
    esp_ble_gatts_send_response(iface, p->read.conn_id, p->read.trans_id, status, &response);
  }
}
}
bool ControlGateway::init(uint32_t passkey, const char *name) {
  gateway = this; pinCode = passkey >= 100000 && passkey <= 999999 ? passkey : 0;
  if (!name || !name[0] || strlen(name) >= sizeof(deviceName)) return false;
  memcpy(deviceName, name, strlen(name) + 1);
  commands_ = xQueueCreate(12, sizeof(CommandLine)); output_ = xQueueCreate(32, 192);
  if (!commands_ || !output_) return false;
  if (uart_driver_install(UART_NUM_0, 1024, 0, 0, nullptr, 0) != ESP_OK) return false;
  uart_config_t uart{};
  uart.baud_rate = 115200;
  uart.data_bits = UART_DATA_8_BITS;
  uart.parity = UART_PARITY_DISABLE;
  uart.stop_bits = UART_STOP_BITS_1;
  uart.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  uart.source_clk = UART_SCLK_DEFAULT;
  // Drain startup logs before reconfiguring the UART and resetting its FIFOs.
  if (uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(100)) != ESP_OK ||
      uart_param_config(UART_NUM_0, &uart) != ESP_OK) {
    uart_driver_delete(UART_NUM_0);
    return false;
  }
  if (!pinCode) {
    // Keep USB maintenance available, but never expose an unauthenticated
    // business service when the device has not been commissioned with a PIN.
    ESP_LOGE("gateway", "Master control GATT disabled: no valid six-digit PIN; run `pin 123456` over USB, then reboot");
  } else {
    ESP_LOGI("gateway", "Master control GATT enabled: service=a64d0001-2f9c-4b6e-8db7-a570530e1000; PIN authentication required; current PIN=%06lu",
      static_cast<unsigned long>(pinCode));
    attr(0, 2, reinterpret_cast<uint8_t *>(&primaryUuid), ESP_GATT_PERM_READ, 16, 16, serviceUuid, true);
    attr(1, 2, reinterpret_cast<uint8_t *>(&charUuid), ESP_GATT_PERM_READ, 1, 1, &rxProperty, true);
    attr(2, 16, rxUuid, ESP_GATT_PERM_WRITE_ENC_MITM, 159, 0, &empty, false);
    attr(3, 2, reinterpret_cast<uint8_t *>(&charUuid), ESP_GATT_PERM_READ, 1, 1, &txProperty, true);
    attr(4, 16, txUuid, ESP_GATT_PERM_READ_ENC_MITM, 191, 0, &empty, false);
    attr(5, 2, reinterpret_cast<uint8_t *>(&cccdUuid), ESP_GATT_PERM_READ_ENC_MITM | ESP_GATT_PERM_WRITE_ENC_MITM, 2, 2, cccd, false);
    esp_ble_auth_req_t auth = ESP_LE_AUTH_REQ_SC_MITM_BOND; uint8_t io = ESP_IO_CAP_OUT, keySize = 16;
    // IDF defaults include BR/EDR LinkKey (0x08). This BLE-only build must
    // distribute only LE encryption/identity keys and never request CTKD.
    uint8_t initKeys = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t responseKeys = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    const auto check = [](const char *step, esp_err_t result) {
      if (result == ESP_OK) return true;
      ESP_LOGE("gateway", "%s failed: %s (0x%x)", step, esp_err_to_name(result), unsigned(result));
      return false;
    };
    if (!check("GAP callback registration", esp_ble_gap_register_callback(gapCb)) ||
        !check("GATT callback registration", esp_ble_gatts_register_callback(gattsCb)) ||
        !check("Mesh BLE callback registration", esp_ble_mesh_register_ble_callback(meshBleCb)) ||
        !check("SM auth mode", esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth, sizeof(auth))) ||
        !check("SM IO capability", esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &io, sizeof(io))) ||
        !check("SM key size", esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &keySize, sizeof(keySize))) ||
        !check("SM initiator BLE keys", esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &initKeys, sizeof(initKeys))) ||
        !check("SM responder BLE keys", esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &responseKeys, sizeof(responseKeys))) ||
        !check("SM static passkey", esp_ble_gap_set_security_param(ESP_BLE_SM_SET_STATIC_PASSKEY, &pinCode, sizeof(pinCode))) ||
        !check("GATT app registration", esp_ble_gatts_app_register(0x5055))) return false;
    ESP_LOGI("gateway", "BLE security: SC+MITM+BOND display-only key_size=%u init_keys=0x%02x rsp_keys=0x%02x; BR/EDR CTKD disabled",
      unsigned(keySize), unsigned(initKeys), unsigned(responseKeys));
  }
  return xTaskCreatePinnedToCore([](void *p) { static_cast<ControlGateway *>(p)->ioWorker(); },
    "console_ble", 4096, this, 3, nullptr, 0) == pdPASS;
}
bool ControlGateway::submit(const char *text, size_t length, bool bluetooth) {
  if (!text || !length || length >= sizeof(CommandLine::text) || memchr(text, 0, length)) return false;
  CommandLine cmd; cmd.bluetooth = bluetooth; memcpy(cmd.text, text, length);
  // One write = one command, not an unbounded stream or a list of commands.
  for (size_t i = 0; i < length; ++i) if (cmd.text[i] == '\r' || cmd.text[i] == '\n') {
    if (i != length - 1) return false;
    cmd.text[i] = 0;
  }
  const bool stop = strncmp(cmd.text, "stop ", 5) == 0 || strncmp(cmd.text, "pump-test-stop ", 15) == 0;
  if ((stop ? xQueueSendToFront(commands_, &cmd, 0) : xQueueSend(commands_, &cmd, 0)) != pdTRUE) {
    if (stop) overflow_ = true;
    return false;
  }
  return true;
}
bool ControlGateway::receive(CommandLine &cmd) { return xQueueReceive(commands_, &cmd, 0) == pdTRUE; }
void ControlGateway::publish(const char *text) {
  char line[192] = {}; snprintf(line, sizeof(line), "%s\n", text);
  xQueueSend(output_, line, 0);
}
void ControlGateway::ioWorker() {
  char input[160] = {}; size_t used = 0; bool discarding = false;
  for (;;) {
    uint8_t bytes[32]; const int n = uart_read_bytes(UART_NUM_0, bytes, sizeof(bytes), pdMS_TO_TICKS(5));
    for (int i = 0; i < n; ++i) {
      const char c = char(bytes[i]);
      if (c == '\r' || c == '\n') {
        if (used && !discarding) submit(input, used, false);
        used = 0; discarding = false;
      } else if (used < sizeof(input) - 1 && !discarding) input[used++] = c;
      else { discarding = true; used = 0; }
    }
    char out[192];
    if (xQueueReceive(output_, out, 0) == pdTRUE) {
      uart_write_bytes(UART_NUM_0, out, strlen(out));
      portENTER_CRITICAL(&textMux); memcpy(latest, out, sizeof(latest)); portEXIT_CRITICAL(&textMux);
      if (connected && secure && subscribed) {
        const size_t len = strlen(out), chunk = std::min<size_t>(mtu - 3, 180);
        for (size_t at = 0; at < len; at += chunk) {
          if (esp_ble_gatts_send_indicate(interface, connection, handles[4],
              std::min(chunk, len - at), reinterpret_cast<uint8_t *>(out + at), false) != ESP_OK) break;
        }
      }
    }
  }
}
}
