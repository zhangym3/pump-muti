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
  if (state_.epoch == UINT32_MAX) return false;
  ++state_.epoch;
  if (state_.lastJob.phase >= SchedulePhase::Preparing && state_.lastJob.phase <= SchedulePhase::Between) {
    state_.lastJob.phase = SchedulePhase::Failed;
    state_.lastJob.reason = Reason::Reboot;
    state_.lastJob.unknown |= state_.lastJob.selected & ~state_.lastJob.complete;
  }
  return write(state_);
}
bool Storage::write(const PersistentState &next) {
  if (!handle_ || nvs_set_blob(handle_, "state", &next, sizeof(next)) != ESP_OK ||
      nvs_commit(handle_) != ESP_OK) return false;
  auto verify = std::unique_ptr<PersistentState>(new (std::nothrow) PersistentState); size_t size = sizeof(next);
  if (!verify || nvs_get_blob(handle_, "state", verify.get(), &size) != ESP_OK || size != sizeof(next) ||
      memcmp(verify.get(), &next, size)) return false;
  state_ = next; return true;
}
}
