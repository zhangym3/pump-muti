#pragma once
#include <cstdint>

namespace pump {
// Reviewed slave build manifest: 80ba7f643b1c290306c81b567ece81de21
// bd045187039924f7a624f48ece5388. Update after rebuilding/reviewing the
// slave, then rebuild the master. This is a deployment gate, not attestation.
constexpr uint32_t ApprovedSlaveBuildIds[] = {0x80ba7f64U};
inline bool approvedSlaveBuild(uint32_t id) {
  for (const auto approved : ApprovedSlaveBuildIds) if (id == approved) return true;
  return false;
}
}
