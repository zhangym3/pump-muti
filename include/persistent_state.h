#pragma once
#include "protocol.h"
#include "valve_status.h"
#include "master_scheduler.h"
#include "control_request.h"
#include <memory>
#include <type_traits>
namespace pump {
struct NodeConfig { uint16_t address = 0; uint32_t k = 5880, version = 1; };
struct LegacyPersistentState {
  uint32_t magic = 0x504D5431, format = 1, profile = 0, epoch = 0, jobCounter = 0;
  uint16_t master = 1;
  uint32_t gatewayPin = 0;
  // Reserved legacy local-valve fields: retain order, types and defaults for NVS format 1.
  uint32_t k = 5880, kVersion = 1;
  uint32_t revisionBase = 0;
  JobKey fence;
  bool resultValid = false;
  ValveStatus result;
  // Active master registry and request/result ledger.
  NodeConfig nodes[MaxNodes] = {};
  ClientRecord clients[16] = {};
  JobReport lastJob;
  ValveStatus lastResults[MaxNodes] = {};
};
// Format 2 appended each client's plan. Format 3 uses a former JobReport padding
// byte for the first stop reason; legacy padding is explicitly ignored on load.
struct PersistentState : LegacyPersistentState {
  uint8_t clientAmounts[16][MaxNodes] = {};
  PersistentState() { format = 3; }
};
static_assert(std::is_trivially_copyable<LegacyPersistentState>::value, "legacy NVS must remain byte-copyable");
static_assert(std::is_trivially_copyable<PersistentState>::value, "NVS must remain byte-copyable");
static_assert(sizeof(JobReport) == 28 && offsetof(JobReport, reason) == 25,
              "stop reason must fit in the legacy JobReport padding");
#ifdef ESP_PLATFORM
static_assert(sizeof(LegacyPersistentState) == 2040, "ESP32 format-1 NVS ABI changed");
#endif
inline bool decodePersistentState(const void *bytes, size_t size, uint32_t profile, PersistentState &out) {
  if (!bytes) return false;
  auto storage = std::unique_ptr<PersistentState>(new (std::nothrow) PersistentState);
  if (!storage) return false;
  auto &decoded = *storage;
  if (size == sizeof(LegacyPersistentState)) {
    memcpy(static_cast<LegacyPersistentState *>(&decoded), bytes, size);
    if (decoded.format != 1) return false;
    memset(decoded.clientAmounts, 0, sizeof(decoded.clientAmounts));
    decoded.lastJob.reason = Reason::None; decoded.format = 3;
  } else if (size == sizeof(PersistentState)) {
    memcpy(&decoded, bytes, size);
    if (decoded.format != 2 && decoded.format != 3) return false;
    if (decoded.format == 2) decoded.lastJob.reason = Reason::None;
    decoded.format = 3;
    if (decoded.lastJob.reason > Reason::Protocol) return false;
    for (size_t i = 0; i < 16; ++i) {
      const auto &c = decoded.clients[i];
      if (c.client && c.mask && !c.ml && !validAmounts(c.mask, decoded.clientAmounts[i])) return false;
    }
  } else return false;
  if (decoded.magic != 0x504D5431 || decoded.profile != profile) return false;
  out = decoded;
  return true;
}
}
