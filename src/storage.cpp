#include "storage.h"
#include "esp_log.h"
#include "nvs_flash.h"
namespace pump {
bool Storage::open(uint32_t profile) {
  // Deliberately do not erase NVS on errors: that would remove replay fences/keys.
  if (nvs_flash_init() != ESP_OK || nvs_open("pumpmulti", NVS_READWRITE, &handle_) != ESP_OK) return false;
  size_t size = sizeof(state_);
  const auto err = nvs_get_blob(handle_, "state", &state_, &size);
  if (err == ESP_ERR_NVS_NOT_FOUND) { state_ = PersistentState{}; state_.profile = profile; }
  else if (err != ESP_OK || size != sizeof(state_) || state_.magic != 0x504D5431 ||
           state_.format != 1 || state_.profile != profile) return false;
  if (!unicast(state_.master) || !validK(state_.k) || !state_.kVersion) return false;
  for (size_t i = 0; i < MaxNodes; ++i) {
    const auto &n = state_.nodes[i];
    if (!n.address) continue;
    if (!unicast(n.address) || n.address == state_.master || !validK(n.k) || !n.version) return false;
    for (size_t j = 0; j < i; ++j) if (state_.nodes[j].address == n.address) return false;
  }
  if (state_.epoch == UINT32_MAX) return false;
  ++state_.epoch;
  if (state_.lastJob.phase >= SchedulePhase::Preparing && state_.lastJob.phase <= SchedulePhase::Between) {
    state_.lastJob.phase = SchedulePhase::Failed;
    state_.lastJob.unknown |= state_.lastJob.selected & ~state_.lastJob.complete;
  }
  return write(state_);
}
bool Storage::write(const PersistentState &next) {
  if (!handle_ || nvs_set_blob(handle_, "state", &next, sizeof(next)) != ESP_OK ||
      nvs_commit(handle_) != ESP_OK) return false;
  PersistentState verify; size_t size = sizeof(verify);
  if (nvs_get_blob(handle_, "state", &verify, &size) != ESP_OK || size != sizeof(verify) ||
      memcmp(&verify, &next, size)) return false;
  state_ = next; return true;
}
}
