#pragma once
#include "protocol.h"
#include "valve_status.h"
#include "master_scheduler.h"
#include "control_request.h"
#include "nvs.h"
#include "persistent_state.h"
#include "flow_calibration_state.h"
#include "speed_calibration.h"
#include "maintenance_session.h"
namespace pump {
class Storage {
 public:
  bool open(uint32_t profile);
  const PersistentState &state() const { return state_; }
  bool write(const PersistentState &next);
  SpeedCalibration calibration(size_t slot) const;
  bool writeCalibration(size_t slot, const SpeedCalibration &value);
  bool clearCalibrations();
  bool clearCalibration(size_t slot);
  const FlowCalibrationState &flowCalibration() const { return flowCalibration_; }
  bool writeFlowCalibration(const FlowCalibrationState &value);
  uint16_t removingAddress() const { return removingAddress_; }
  bool beginRemoval(uint16_t address);
  bool finishRemoval();
  const MaintenanceRecord &maintenance() const { return maintenance_; }
  bool writeMaintenance(const MaintenanceRecord &value);
  uint16_t resultAddress(JobKey job, size_t slot) const {
    return slot < MaxNodes ? resultAddresses_.address(job, slot, state_.nodes[slot].address) : 0;
  }
 private:
  nvs_handle_t handle_ = 0;
  PersistentState state_;
  CalibrationStore calibrations_;
  FlowCalibrationState flowCalibration_;
  uint16_t removingAddress_ = 0;
  ResultAddresses resultAddresses_;
  MaintenanceRecord maintenance_;
};
}
