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
#include "master_identity.h"
#include "recovery_nvs.h"
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
  recovery::MasterIdentityStore &masterIdentity() { return identity_; }
  uint16_t resultAddress(JobKey job, size_t slot) const {
    return slot < MaxNodes ? resultAddresses_.address(job, slot, state_.nodes[slot].address) : 0;
  }
 private:
  static bool identityRandom(uint8_t *out,size_t size);
  recovery::NvsBlobStore identityBackend_;
  recovery::Journal identityJournal_{identityBackend_};
  recovery::MasterIdentityStore identity_{identityJournal_,identityRandom};
  nvs_handle_t handle_ = 0;
  PersistentState state_;
  CalibrationStore calibrations_;
  FlowCalibrationState flowCalibration_;
  uint16_t removingAddress_ = 0;
  ResultAddresses resultAddresses_;
  MaintenanceRecord maintenance_;
};
}
