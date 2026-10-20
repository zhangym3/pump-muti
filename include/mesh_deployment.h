#pragma once
#include <cstdint>

namespace pump {
// Reviewed slave build source SHA-256: dd63e1adb7f669048a4ba6be88d18835be6fb64d68cc3bff063c0276441841af.
// Hall diagnostic build SHA-256: 7ba339a86e723349745692e9e59800d00172415b82af11b566211237728f2aef.
// Keep the deployed V3 build compatible while rolling out USB-only diagnostics.
// This is a deployment gate, not firmware attestation.
constexpr uint32_t ApprovedSlaveBuildIds[] = {0xdd63e1adU, 0x7ba339a8U};
inline bool approvedSlaveBuild(uint32_t id) {
  for (const auto approved : ApprovedSlaveBuildIds) if (id == approved) return true;
  return false;
}
}
