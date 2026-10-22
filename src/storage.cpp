#include "storage.h"
#include "esp_log.h"
#include "nvs_flash.h"
namespace pump {
bool Storage::open(uint32_t profile) {
  // Deliberately do not erase NVS on errors: that would remove replay fences/keys.
  if (nvs_flash_init() != ESP_OK || nvs_open("pumpmulti", NVS_READWRITE, &handle_) != ESP_OK) return false;
  size_t size = 0;
  const auto err = nvs_get_blob(handle_, "state", nullptr, &size);
  if (err == ESP_ERR_NVS_NOT_FOUND) { state_ = PersistentState{}; state_.profile = profile; }
  else {
    if (err != ESP_OK || (size != sizeof(PersistentState) && size != sizeof(LegacyPersistentState))) return false;
    auto blob = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[size]);
    if (!blob || nvs_get_blob(handle_, "state", blob.get(), &size) != ESP_OK ||
        !decodePersistentState(blob.get(), size, profile, state_)) return false;
  }
  if (!unicast(state_.master) || !validK(state_.k) || !state_.kVersion) return false;
  for (size_t i = 0; i < MaxNodes; ++i) {
    const auto &n = state_.nodes[i];
    if (!n.address) continue;
    if (!unicast(n.address) || n.address == state_.master || !validK(n.k) || !n.version) return false;
    for (size_t j = 0; j < i; ++j) if (state_.nodes[j].address == n.address) return false;
  }
  // Separate additive NVS key: deployed identity, fences and result ABI are unchanged.
  CalibrationStore saved; size_t calibrationSize = sizeof(saved);
  const auto calibrationErr = nvs_get_blob(handle_, "speed_cal", &saved, &calibrationSize);
  if (calibrationErr != ESP_ERR_NVS_NOT_FOUND &&
      (calibrationErr != ESP_OK || !decodeCalibrations(&saved, calibrationSize, calibrations_))) return false;
  const auto removalErr = nvs_get_u16(handle_, "remove_addr", &removingAddress_);
  if (removalErr != ESP_ERR_NVS_NOT_FOUND && (removalErr != ESP_OK || !unicast(removingAddress_))) return false;
  ResultAddresses addresses; size_t addressSize = sizeof(addresses);
  const auto addressErr = nvs_get_blob(handle_, "result_addrs", &addresses, &addressSize);
  if (addressErr != ESP_ERR_NVS_NOT_FOUND &&
      (addressErr != ESP_OK || !decodeResultAddresses(&addresses, addressSize, resultAddresses_))) return false;
  size_t flowCalibrationSize = sizeof(flowCalibration_);
  const auto flowCalibrationErr = nvs_get_blob(handle_, "flow_cal", &flowCalibration_, &flowCalibrationSize);
  if (flowCalibrationErr != ESP_ERR_NVS_NOT_FOUND &&
      (flowCalibrationErr != ESP_OK || flowCalibrationSize != sizeof(flowCalibration_) ||
       !validFlowCalibrationState(flowCalibration_))) return false;
  if (flowCalibrationErr == ESP_ERR_NVS_NOT_FOUND) flowCalibration_ = {};
  if (state_.epoch == UINT32_MAX) return false;
  ++state_.epoch;
  if (state_.lastJob.phase >= SchedulePhase::Preparing && state_.lastJob.phase <= SchedulePhase::Between) {
    state_.lastJob.phase = SchedulePhase::Failed;
    state_.lastJob.reason = Reason::Reboot;
    state_.lastJob.unknown |= state_.lastJob.selected & ~state_.lastJob.complete;
  }
  if (flowCalibration_.phase == FlowCalibrationPhase::Starting ||
      flowCalibration_.phase == FlowCalibrationPhase::Preout ||
      flowCalibration_.phase == FlowCalibrationPhase::MarkPending ||
      flowCalibration_.phase == FlowCalibrationPhase::Measuring ||
      flowCalibration_.phase == FlowCalibrationPhase::Stopping) {
    flowCalibration_.phase = FlowCalibrationPhase::Unknown;
    if (!writeFlowCalibration(flowCalibration_)) return false;
  }
  return write(state_);
}
bool Storage::writeFlowCalibration(const FlowCalibrationState &value) {
  if (!handle_ || !validFlowCalibrationState(value) ||
      nvs_set_blob(handle_, "flow_cal", &value, sizeof(value)) != ESP_OK || nvs_commit(handle_) != ESP_OK) return false;
  FlowCalibrationState verify; size_t size = sizeof(verify);
  if (nvs_get_blob(handle_, "flow_cal", &verify, &size) != ESP_OK || size != sizeof(value) ||
      memcmp(&verify, &value, size)) return false;
  flowCalibration_ = value;
  return true;
}
bool Storage::write(const PersistentState &next) {
  if (!handle_ || nvs_set_blob(handle_, "state", &next, sizeof(next)) != ESP_OK ||
      nvs_commit(handle_) != ESP_OK) return false;
  auto verify = std::unique_ptr<PersistentState>(new (std::nothrow) PersistentState); size_t size = sizeof(next);
  if (!verify || nvs_get_blob(handle_, "state", verify.get(), &size) != ESP_OK || size != sizeof(next) ||
      memcmp(verify.get(), &next, size)) return false;
  state_ = next; return true;
}
SpeedCalibration Storage::calibration(size_t slot) const {
  if (slot >= MaxNodes) return {};
  const auto &n = state_.nodes[slot]; const auto &c = calibrations_.nodes[slot];
  return c.matches(n.address, n.k, n.version) ? c : SpeedCalibration{};
}
bool Storage::writeCalibration(size_t slot, const SpeedCalibration &value) {
  if (!handle_ || slot >= MaxNodes || !value.valid()) return false;
  const auto &n = state_.nodes[slot];
  if (!value.matches(n.address, n.k, n.version)) return false;
  auto next = calibrations_; next.nodes[slot] = value;
  if (nvs_set_blob(handle_, "speed_cal", &next, sizeof(next)) != ESP_OK || nvs_commit(handle_) != ESP_OK) return false;
  CalibrationStore verify; size_t size = sizeof(verify);
  if (nvs_get_blob(handle_, "speed_cal", &verify, &size) != ESP_OK || size != sizeof(next) || memcmp(&verify, &next, size)) return false;
  calibrations_ = next; return true;
}
bool Storage::clearCalibrations() {
  if (!handle_) return false;
  const auto err = nvs_erase_key(handle_, "speed_cal");
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return false;
  if (nvs_commit(handle_) != ESP_OK) return false;
  size_t size = 0;
  if (nvs_get_blob(handle_, "speed_cal", nullptr, &size) != ESP_ERR_NVS_NOT_FOUND) return false;
  calibrations_ = {}; return true;
}
bool Storage::clearCalibration(size_t slot) {
  if (!handle_ || slot >= MaxNodes) return false;
  auto next = calibrations_; next.nodes[slot] = {};
  if (nvs_set_blob(handle_, "speed_cal", &next, sizeof(next)) != ESP_OK || nvs_commit(handle_) != ESP_OK) return false;
  CalibrationStore verify; size_t size = sizeof(verify);
  if (nvs_get_blob(handle_, "speed_cal", &verify, &size) != ESP_OK || size != sizeof(next) || memcmp(&verify, &next, size)) return false;
  calibrations_ = next; return true;
}
bool Storage::beginRemoval(uint16_t address) {
  if (!handle_ || removingAddress_ || !unicast(address)) return false;
  if (state_.lastJob.key.valid() && resultAddresses_.key != state_.lastJob.key) {
    ResultAddresses next; next.key = state_.lastJob.key;
    for (size_t i = 0; i < MaxNodes; ++i) next.nodes[i] = state_.nodes[i].address;
    if (nvs_set_blob(handle_, "result_addrs", &next, sizeof(next)) != ESP_OK || nvs_commit(handle_) != ESP_OK) return false;
    ResultAddresses verify; size_t size = sizeof(verify);
    if (nvs_get_blob(handle_, "result_addrs", &verify, &size) != ESP_OK || size != sizeof(next) || memcmp(&verify, &next, size)) return false;
    resultAddresses_ = next;
  }
  if (nvs_set_u16(handle_, "remove_addr", address) != ESP_OK || nvs_commit(handle_) != ESP_OK) return false;
  uint16_t verify = 0;
  if (nvs_get_u16(handle_, "remove_addr", &verify) != ESP_OK || verify != address) return false;
  removingAddress_ = address; return true;
}
bool Storage::finishRemoval() {
  if (!handle_) return false;
  const auto err = nvs_erase_key(handle_, "remove_addr");
  if ((err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) || nvs_commit(handle_) != ESP_OK) return false;
  uint16_t verify = 0;
  if (nvs_get_u16(handle_, "remove_addr", &verify) != ESP_ERR_NVS_NOT_FOUND) return false;
  removingAddress_ = 0; return true;
}
}
