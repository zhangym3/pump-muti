#pragma once
#include "master_scheduler.h"
#include "mesh_tx_governor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <atomic>
namespace pump {
struct MeshCandidate { uint8_t uuid[16] = {}; uint8_t addr[6] = {}; uint8_t type = 0; uint16_t oob = 0; };
struct MeshManagement {
  const char *phase = "STARTING", *reason = "NONE";
  bool capable = false, busy = true;
  uint32_t operation = 0, scan = 0; uint8_t found = 0; uint16_t target = 0;
  MeshCandidate candidates[8];
};
class MeshTransport final : public MeshSender {
 public:
  bool init(uint16_t master);
  bool send(const Packet &p) override;
  bool beginLeaseScope(JobKey job, uint32_t &floor) override;
  MeshTxGovernor::Admission prepare(const Packet &p) override;
  bool receive(Packet &p);
  bool ready() const;
  uint16_t address() const;
  const char *deviceName() const { return deviceName_; }
  bool takeOverflow() { return overflow_.exchange(false); }
  void incoming(const Packet &p);
  void worker();
  bool takeSubmission(MeshSubmission &out) override;
  void fence(JobKey job) override;
  void retire(uint16_t peer, JobKey job, Op op) override;
  bool healthy() const override;
  MeshTxGovernor::Metrics metrics() const;
  MeshTxGovernor::Diagnostics diagnostics() const;
  void trackedEvent(uint32_t ticket, uint8_t event, int error);
  void managementTick(uint32_t now);
  bool managementBusy() const;
  const MeshManagement &management() const;
  bool scanSlaves(uint32_t request, uint32_t now);
  bool joinSlave(const uint8_t uuid[16], uint32_t request, uint32_t now);
  bool retrySlave(uint16_t address, uint32_t request, uint32_t now);
  bool removeSlave(uint16_t address, uint32_t request, uint32_t now);
  void finishSlaveRemoval(bool saved);
  // Field migration from the legacy Mesh role. Clears persisted Mesh
  // credentials/role only; application NVS (PIN, tasks and pump settings)
  // remains untouched.
  bool migrateLegacyNetwork();
  // Starts an asynchronous removal of every provisioned slave while keeping
  // the local Provisioner role, PIN and application settings intact.
  bool resetProvisionedSlaves(uint32_t now);
  void slaveHello(uint16_t address);
  bool ownsSlave(uint16_t address) const;
  size_t pairedAddresses(uint16_t *addresses, size_t capacity) const;
  // Returns every remote node currently held by the Provisioner. This is
  // intentionally broader than ownsSlave(): a reset must also remove nodes
  // left behind by an external provisioning tool.
  size_t provisionedAddresses(uint16_t *addresses, size_t capacity) const;
 private:
  char deviceName_[24] = {};
  QueueHandle_t rx_ = nullptr, urgentRx_ = nullptr;
  mutable portMUX_TYPE txLock_ = portMUX_INITIALIZER_UNLOCKED;
  MeshTxGovernor tx_{2};
  std::atomic<bool> overflow_{false};
};
}
