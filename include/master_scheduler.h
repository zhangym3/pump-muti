#pragma once
#include "valve_status.h"
#include "pump_safety_controller.h"
#include "mesh_tx_governor.h"
#include "mesh_deployment.h"
namespace pump {
struct RegisteredNode { uint16_t address = 0; uint32_t k = 5880, version = 1; };
struct NodeRuntime {
  struct LeaseRecord {
    uint32_t sequence = 0, issuedAt = 0;
    bool accepted = false, acked = false, early = false; Reply result = Reply::Unknown;
  } leases[3];
  RegisteredNode config;
  ValveStatus status;
  uint32_t seenAt = 0;
  uint32_t leaseAckAt = 0, leaseAckSequence = 0;
  uint32_t telemetryPulses = 0, telemetryAt = 0;
  bool telemetrySeen = false, leaseAckSeen = false;
  bool seen = false, commissioned = false;
  uint32_t commandAt = 0, leaseAt = 0, openedAt = 0, buildId = 0;
  uint8_t attempts = 0, leaseCursor = 0;
  bool commandPending = false, leasePending = false, commandSent = false, leaseSent = false;
  bool capable = false;
};
enum class SchedulePhase : uint8_t { Idle, Preparing, Opening, Supplying, Draining, Between, Complete, Failed };
class MeshSender {
 public:
  virtual ~MeshSender() = default;
  virtual bool send(const Packet &p) = 0;
  virtual bool takeSubmission(MeshSubmission &out) = 0;
  virtual void fence(JobKey key) = 0;
  virtual bool healthy() const = 0;
};
struct JobReport {
  JobKey key;
  uint32_t selected = 0, complete = 0, failed = 0, unknown = 0;
  SchedulePhase phase = SchedulePhase::Idle;
  Reason reason = Reason::None; // Uses former padding; persisted only by format 3.
};
struct VolumeReport { uint32_t ml = 0, measured = 0; };
class MasterScheduler {
 public:
  MasterScheduler(MeshSender &net, PumpSafetyController &pump, uint8_t concurrent)
    : net_(net), pump_(pump), concurrent_(concurrent) {}
  Reply registerNode(size_t slot, RegisteredNode cfg);
  Reply check(uint32_t mask, uint32_t ml, uint32_t maxMs, uint32_t now, const uint8_t *amounts = nullptr);
  void restore(const JobReport &report, const ValveStatus results[MaxNodes]);
  void begin(JobKey key, uint32_t mask, uint32_t ml, uint32_t maxMs, uint32_t now, const uint8_t *amounts = nullptr);
  void stop(Reason reason, uint32_t now);
  void tick(uint32_t now);
  void hello(uint16_t source, uint32_t k, uint32_t version, bool commissioned, uint32_t now);
  void status(uint16_t source, const ValveStatus &s, uint32_t now);
  bool leaseAck(uint16_t source, JobKey key, uint32_t sequence, Reply result, uint32_t now);
  bool commandReply(uint16_t source, JobKey key, Op command, Reply result, uint32_t now);
  bool summary(uint16_t source, JobKey key, uint32_t pulses, uint32_t revision, uint32_t now);
  bool capabilities(uint16_t source, uint32_t nonce, uint16_t revision, uint16_t features, uint32_t buildId, uint32_t now);
  void submissions(uint32_t now);
  bool active() const;
  int find(uint16_t address) const;
  const NodeRuntime &node(size_t i) const { return nodes_[i]; }
  JobReport report() const { return report_; }
  Reason stopReason() const { return stopReason_; }
  VolumeReport volume() const;
 private:
  void batch(uint32_t now);
  void closeBatch(Reason reason);
  bool sendNode(size_t i, Op op);
  void leases(uint32_t now);
  MeshSender &net_;
  PumpSafetyController &pump_;
  uint8_t concurrent_;
  NodeRuntime nodes_[MaxNodes] = {};
  JobReport report_;
  uint32_t remaining_ = 0, batch_ = 0, prepared_ = 0, opened_ = 0, closed_ = 0, final_ = 0;
  uint32_t ml_ = 0, maxMs_ = 0, stageAt_ = 0, txAt_ = 0, leaseAt_ = 0;
  uint8_t amounts_[MaxNodes] = {};
  uint32_t pumpSeq_ = 0, pumpRenewAt_ = 0;
  uint32_t closeAt_ = 0;
  size_t cursor_ = 0;
  bool abort_ = false;
  Reason stopReason_ = Reason::None;
  uint32_t capNonce_ = 0, bootstrapAt_ = 0;
  bool preflight_ = false, bootstrap_ = false;
};
constexpr uint32_t LeaseAckTimeoutMs = 2400;
}
