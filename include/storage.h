#pragma once
#include "protocol.h"
#include "valve_status.h"
#include "master_scheduler.h"
#include "control_request.h"
#include "nvs.h"
#include "persistent_state.h"
namespace pump {
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
