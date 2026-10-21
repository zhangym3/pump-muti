#pragma once
#include "protocol.h"
#include "valve_status.h"
#include "master_scheduler.h"
#include "control_request.h"
#include "nvs.h"
#include "persistent_state.h"
#include "speed_calibration.h"
namespace pump {
class Storage {
 public:
  bool open(uint32_t profile);
  const PersistentState &state() const { return state_; }
  bool write(const PersistentState &next);
  SpeedCalibration calibration(size_t slot) const;
  bool writeCalibration(size_t slot, const SpeedCalibration &value);
  bool clearCalibrations();
 private:
  nvs_handle_t handle_ = 0;
  PersistentState state_;
  CalibrationStore calibrations_;
};
}
