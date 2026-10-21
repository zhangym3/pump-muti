#pragma once
#include "protocol.h"
namespace pump {
// K is pulses per 1000 mL; the phone displays K / 1000 pulses per mL.
struct SpeedCalibration {
  uint16_t address = 0;
  uint32_t baseK = 0, baseVersion = 0, revision = 0;
  uint32_t low = 0, medium = 0, high = 0;
  bool valid() const {
    return unicast(address) && validK(baseK) && baseVersion && revision &&
      validK(low) && validK(medium) && validK(high);
  }
  bool matches(uint16_t a, uint32_t k, uint32_t v) const {
    return valid() && address == a && baseK == k && baseVersion == v;
  }
  uint32_t coefficient(uint8_t level) const { return level == 1 ? low : level == 2 ? medium : high; }
};
struct CalibrationStore {
  uint32_t magic = 0x43414C31, format = 1;
  SpeedCalibration nodes[MaxNodes] = {};
};
inline bool decodeCalibrations(const void *bytes, size_t size, CalibrationStore &out) {
  if (!bytes || size != sizeof(CalibrationStore)) return false;
  CalibrationStore value; memcpy(&value, bytes, size);
  if (value.magic != 0x43414C31 || value.format != 1) return false;
  for (const auto &n : value.nodes) if (n.address && !n.valid()) return false;
  out = value; return true;
}
}
