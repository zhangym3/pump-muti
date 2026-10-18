#pragma once
#include "protocol.h"
#include "valve_status.h"
#include "master_scheduler.h"
#include "control_request.h"
#include "nvs.h"
namespace pump {
struct NodeConfig { uint16_t address = 0; uint32_t k = 5880, version = 1; };
struct PersistentState {
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
class Storage {
 public:
  bool open(uint32_t profile);
  const PersistentState &state() const { return state_; }
  bool write(const PersistentState &next);
 private:
  nvs_handle_t handle_ = 0;
  PersistentState state_;
};
}
