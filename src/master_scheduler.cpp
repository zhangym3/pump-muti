#include "master_scheduler.h"
#include "control_request.h"
#include "result_commit.h"
namespace pump {
bool MasterScheduler::active() const {
  return report_.phase >= SchedulePhase::Preparing && report_.phase <= SchedulePhase::Between;
}
VolumeReport MasterScheduler::volume() const {
  VolumeReport out; uint64_t ml = 0;
  for (size_t i = 0; i < MaxNodes; ++i) {
    const auto &n = nodes_[i]; const uint32_t bit = uint32_t(1) << i;
    if (!(report_.selected & bit) || !report_.key.valid() || n.status.key != report_.key || n.status.reason == Reason::Reboot) continue;
    out.measured |= bit;
    ml += volumeMl(n.telemetrySeen ? n.telemetryPulses : n.status.pulses, n.status.k);
  }
  out.ml = ml > UINT32_MAX ? UINT32_MAX : uint32_t(ml); return out;
}
int MasterScheduler::find(uint16_t a) const {
  for (size_t i = 0; i < MaxNodes; ++i) if (nodes_[i].config.address == a && a) return int(i);
  return -1;
}
Reply MasterScheduler::registerNode(size_t slot, RegisteredNode cfg) {
  if (active()) return Reply::Busy;
  if (slot >= MaxNodes) return Reply::Capacity;
  if (!unicast(cfg.address) || !validK(cfg.k) || !cfg.version) return Reply::Invalid;
  const int existing = find(cfg.address);
  if (existing >= 0 && size_t(existing) != slot) return Reply::Conflict;
  nodes_[slot] = {}; nodes_[slot].config = cfg; return Reply::Ok;
}
Reply MasterScheduler::unregisterNode(size_t slot, uint16_t address) {
  if (active() || pump_.output() || pump_.testing()) return Reply::Busy;
  if (slot >= MaxNodes || !address || nodes_[slot].config.address != address) return Reply::Invalid;
  // Keep the completed task's measurement available to existing query/replay.
  auto &node = nodes_[slot];
  const auto status = node.status;
  const auto pulses = node.telemetryPulses, telemetryAt = node.telemetryAt;
  const bool telemetrySeen = node.telemetrySeen;
  node = {}; node.status = status; node.telemetryPulses = pulses;
  node.telemetryAt = telemetryAt; node.telemetrySeen = telemetrySeen;
  return Reply::Ok;
}
void MasterScheduler::hello(uint16_t source, uint32_t k, uint32_t ver, bool commissioned,
                            uint32_t now, uint32_t buildId) {
  const int i = find(source); if (i < 0) return;
  auto &n = nodes_[i]; n.seen = true; n.seenAt = now;
  if (n.helloBuildId != buildId) n.capable = false;
  n.helloBuildId = buildId;
  n.commissioned = commissioned && n.config.k == k && n.config.version == ver;
}
uint32_t MasterScheduler::coefficient(size_t slot) const {
  const auto &n = nodes_[slot];
  return n.calibration.valid() ? n.calibration.coefficient(pump_.speed()) : n.config.k;
}
uint16_t MasterScheduler::features(size_t slot) const {
  return RequiredFeatures | (nodes_[slot].calibration.valid() ? JobCalibrationFeature : 0) |
      (manualCalibration_ ? ManualCalibrationFeature : 0);
}
Reply MasterScheduler::setCalibration(size_t slot, const SpeedCalibration &value) {
  if (active() || pump_.output() || pump_.testing()) return Reply::Busy;
  if (slot >= MaxNodes) return Reply::Invalid;
  const auto &cfg = nodes_[slot].config;
  if (!value.matches(cfg.address, cfg.k, cfg.version)) return Reply::Invalid;
  nodes_[slot].calibration = value;
  nodes_[slot].capable = false; // Revalidate support before the next task.
  return Reply::Ok;
}
Reply MasterScheduler::check(uint32_t mask, uint32_t ml, uint32_t maxMs, uint32_t now, const uint8_t *amounts) {
  if (active()) return Reply::Busy;
  if (!mask || (amounts ? ml != 0 || !validAmounts(mask, amounts) : !ml) ||
      !maxMs || maxMs > 240000 || !concurrent_ || concurrent_ > MaxNodes) return Reply::Invalid;
  if (nodeCount(mask)>ProductMaxNodes) return Reply::Capacity;
  if (nodeCount(mask)>concurrent_) return Reply::NotReady; // Never silently split a simultaneous task.
  if (!pump_.ready(now)) return Reply::NotReady;
  for (size_t i = 0; i < MaxNodes; ++i) if (mask & (uint32_t(1) << i)) {
    const auto &n = nodes_[i]; uint32_t pulses = 0;
    if (!n.config.address || !n.seen || elapsed(now, n.seenAt, 15000)) return Reply::Offline;
    if (!n.commissioned || !targetPulses(amounts ? uint32_t(amounts[i]) * 50 : ml, coefficient(i), pulses)) return Reply::NotReady;
    if (n.status.phase >= Phase::Prepared && n.status.phase <= Phase::Settling) return Reply::Busy;
  }
  return Reply::Ok;
}
Reply MasterScheduler::checkCalibration(size_t slot, uint32_t standardMl, uint8_t speed, uint32_t maxMs, uint32_t now) {
  if (active()) return Reply::Busy;
  if (slot >= MaxNodes || standardMl < 50 || standardMl > 2000 || speed < 1 || speed > 3 ||
      !maxMs || maxMs > 60000) return Reply::Invalid;
  if (pump_.speed() != speed) return Reply::NotReady;
  if (!pump_.ready(now)) return Reply::NotReady;
  const auto &n = nodes_[slot];
  if (!n.config.address || !n.seen || elapsed(now,n.seenAt,15000)) return Reply::Offline;
  if (!n.commissioned) return Reply::NotReady;
  if (n.status.phase >= Phase::Prepared && n.status.phase <= Phase::Settling) return Reply::Busy;
  return Reply::Ok;
}
void MasterScheduler::restore(const JobReport &report, const ValveStatus results[MaxNodes]) {
  report_ = report;
  stopReason_ = report.reason;
  for (size_t i = 0; i < MaxNodes; ++i) {
    nodes_[i].status = results[i];
    nodes_[i].telemetrySeen = false;
    if (report_.key.valid() && results[i].key == report_.key && results[i].reason != Reason::Reboot) {
      nodes_[i].telemetryPulses = results[i].pulses;
      nodes_[i].telemetrySeen = true;
    }
  }
}
void MasterScheduler::begin(JobKey key, uint32_t mask, uint32_t ml, uint32_t maxMs, uint32_t now,
                            const uint8_t *amounts, bool manualCalibration) {
  report_ = {}; report_.key = key; report_.selected = remaining_ = mask;
  manualCalibration_ = manualCalibration; calibrationMarked_ = calibrationMarkPending_ = false;
  calibrationStartPulses_ = calibrationMarkAt_ = 0; calibrationMarkAttempts_ = 0;
  ml_ = ml; maxMs_ = maxMs; pumpSeq_ = 0; abort_ = false; stopReason_ = Reason::None;
  capNonce_ = key.epoch ^ (key.id * 2654435761U); if (!capNonce_) capNonce_ = 1;
  preflight_ = true;
  for (auto &n : nodes_) {
    n.live = {};
    if (n.capable && (n.capabilityRevision != ProtocolRevision ||
        (n.capabilityFeatures & features(size_t(&n - nodes_))) != features(size_t(&n - nodes_)) ||
        (n.helloBuildId && n.helloBuildId != n.buildId) || !n.seen || elapsed(now, n.seenAt, 15000)))
      n.capable = false;
  }
  for (size_t i = 0; i < MaxNodes; ++i) amounts_[i] = amounts ? amounts[i] : 0;
  batch(now);
  guard_="NONE";
  if (!net_.beginLeaseScope(key,leaseFloor_)) { guard_="LEASE_SCOPE_FAILED"; stop(Reason::Transport,now); }
}
void MasterScheduler::batch(uint32_t now) {
  batch_ = prepared_ = opened_ = closed_ = final_ = 0;
  bootstrap_ = false;
  for (auto &n : nodes_) {
    n.commandPending = n.leasePending = n.commandSent = n.leaseSent = false;
    n.attempts = n.leaseCursor = 0; n.leaseAckSeen = false;
    n.openedAt=n.firstStartIssuedAt=n.startAckAt=n.nextLeaseAt=0;
    n.startIssued=n.startAckSeen=n.commandCommitted=n.commandAccepted=n.commandEarly=false;
    for (auto &v : n.leases) v = {};
  }
  uint8_t count = 0;
  for (size_t i = 0; i < MaxNodes && count < concurrent_; ++i) {
    const uint32_t bit = uint32_t(1) << i;
    if (remaining_ & bit) { batch_ |= bit; remaining_ &= ~bit; ++count; }
  }
  report_.phase = SchedulePhase::Preparing; stageAt_ = now;
  txAt_ = now - 100; leaseAt_ = now - 1000; cursor_ = 0;
}
MeshTxGovernor::Admission MasterScheduler::sendNode(size_t i, Op op) {
  auto p = packet(op, nodes_[i].config.address); Writer w{p}; w.key(report_.key);
  if (op == Op::Prepare) {
    const uint32_t ml = ml_ ? ml_ : uint32_t(amounts_[i]) * 50;
    const uint32_t k = coefficient(i);
    uint32_t pulses = 0; if (!manualCalibration_) targetPulses(ml, k, pulses);
    w.u32(ml); w.u32(pulses); w.u32(k);
    w.u32(nodes_[i].config.version); w.u32(maxMs_);
    // Preserve legacy wire speed tags for deployed slaves; actual PWM duty is reported by PUMP_PWM.
    const uint16_t speed = pump_.speed() == 1 ? SpeedLowPermille : pump_.speed() == 2 ? SpeedMediumPermille : SpeedHighPermille;
    w.u16(speed); w.u32(LeaseMs);
    w.u8((nodes_[i].calibration.valid() ? PrepareCalibrated : 0) | PrepareLiveTelemetry |
         (manualCalibration_ ? PrepareManualCalibration : 0)); w.u32(leaseFloor_);
  }
  if (op == Op::Prepare) return net_.prepare(p);
  return net_.send(p) ? MeshTxGovernor::Admission::Accepted : MeshTxGovernor::Admission::Failed;
}
void MasterScheduler::closeBatch(Reason r) {
  for (size_t i = 0; i < MaxNodes; ++i) {
    const uint32_t bit = uint32_t(1) << i;
    if (!(batch_ & bit) || (final_ & bit)) continue;
    auto p = packet(Op::Close, nodes_[i].config.address); Writer w{p}; w.key(report_.key); w.u8(uint8_t(r));
    net_.send(p);
  }
}
void MasterScheduler::stop(Reason r, uint32_t now) {
  pump_.stop(now); // Always local pump first, before any radio operation.
  if (!active()) return;
  if (abort_ && report_.phase == SchedulePhase::Draining) return;
  abort_ = true; stopReason_ = report_.reason = r;
  if (!(manualCalibration_ && r == Reason::Manual && calibrationMarked_))
    report_.failed |= remaining_ | (batch_ & ~final_);
  remaining_ = 0; calibrationMarkPending_ = false;
  net_.fence(report_.key);
  closeBatch(r); report_.phase = SchedulePhase::Draining; stageAt_ = closeAt_ = now; txAt_ = now;
}
Reply MasterScheduler::markCalibration(uint32_t now) {
  if (!manualCalibration_ || abort_ || report_.phase != SchedulePhase::Supplying || calibrationMarked_)
    return Reply::NotReady;
  if (calibrationMarkPending_) return Reply::Pending;
  size_t target = MaxNodes;
  for (size_t i=0;i<MaxNodes;++i) if (batch_ & (uint32_t(1)<<i)) { target=i; break; }
  if (target == MaxNodes || (opened_ & (uint32_t(1)<<target)) == 0 || (closed_ & (uint32_t(1)<<target))) return Reply::NotReady;
  auto p=packet(Op::MeasureMark,nodes_[target].config.address); Writer w{p}; w.key(report_.key);
  if (!net_.send(p)) return Reply::NotReady;
  calibrationMarkPending_=true; calibrationMarkAt_=now; calibrationMarkAttempts_=1;
  return Reply::Ok;
}
bool MasterScheduler::calibrationMarkAck(uint16_t source, JobKey key, uint32_t pulses, Reply reply, uint32_t now) {
  const int index=find(source);
  if (index<0 || !manualCalibration_ || key!=report_.key || abort_ ||
      report_.phase!=SchedulePhase::Supplying || !(batch_ & (uint32_t(1)<<index)) ||
      !(opened_ & (uint32_t(1)<<index)) || (closed_ & (uint32_t(1)<<index))) return false;
  if (calibrationMarked_) return reply == Reply::Ok && pulses==calibrationStartPulses_;
  if (!calibrationMarkPending_ || elapsed(now,calibrationMarkAt_,CriticalQueueTtlMs)) return false;
  calibrationMarkPending_=false;
  if (reply != Reply::Ok) {
    report_.failed |= uint32_t(1) << index;
    stop(Reason::Transport,now);
    return true;
  }
  calibrationStartPulses_=pulses; calibrationMarked_=true; calibrationMarkPending_=false;
  return true;
}
bool MasterScheduler::summary(uint16_t source, JobKey key, uint32_t pulses, uint32_t revision, uint32_t now) {
  const int i = find(source); if (i < 0) return false;
  auto &n = nodes_[i];
  if (key != n.status.key || revision != n.status.revision ||
      pulses < n.status.pulses || (n.telemetrySeen && pulses < n.telemetryPulses)) return false;
  n.telemetryPulses = pulses; n.telemetryAt = now; n.telemetrySeen = true;
  n.seen = true; n.seenAt = now; return true; // Monitoring only; never advances the state machine.
}
bool MasterScheduler::progress(uint16_t source, JobKey key, uint32_t pulses, uint32_t runtimeMs, uint32_t now) {
  const int index = find(source);
  if (index < 0 || !active() || abort_ || key != report_.key || !runtimeMs || runtimeMs > maxMs_ ||
      (report_.phase != SchedulePhase::Opening && report_.phase != SchedulePhase::Supplying)) return false;
  const uint32_t bit = uint32_t(1) << index;
  if (!(batch_ & bit) || !(opened_ & bit) || (closed_ & bit) || (final_ & bit)) return false;
  auto &node = nodes_[index];
  auto &previous = node.live;
  if (previous.seen && (previous.key != key || !serialNewer(runtimeMs, previous.runtimeMs) || pulses < previous.pulses)) return false;
  if (node.status.key == key && pulses < node.status.pulses) return false;
  const uint32_t k = coefficient(size_t(index));
  // First sample uses the interval since opening; later samples use pulse deltas.
  const uint32_t deltaMs = runtimeMs - (previous.seen ? previous.runtimeMs : 0);
  const uint32_t deltaPulses = pulses - (previous.seen ? previous.pulses : 0);
  const uint64_t value = uint64_t(deltaPulses) * 60000000ULL / (uint64_t(k) * deltaMs);
  const uint32_t rate = value > UINT32_MAX ? UINT32_MAX : uint32_t(value);
  previous = {key, pulses, runtimeMs, now, volumeMl(pulses, k), rate, true};
  node.seen = true; node.seenAt = now;
  return true;
}
void MasterScheduler::status(uint16_t source, const ValveStatus &s, uint32_t now) {
  const int index = find(source); if (index < 0) return;
  auto &n = nodes_[index]; n.seenAt = now; n.seen = true;
  // The detailed event counter rejects out-of-order phases within the same dose.
  if (s.key == n.status.key && s.revision < n.status.revision) return;
  if (s.key == n.status.key && s.revision == n.status.revision && !sameResult(s,n.status)) return;
  if (s.key == n.status.key && terminalResult(n.status) && n.status.reason!=Reason::Reboot && !sameMeasurement(s,n.status)) return;
  if (n.status.key.valid() && s.key != n.status.key && !s.key.newer(n.status.key)) return;
  if (s.key != n.status.key) n.telemetrySeen = false;
  n.status = s;
  if (s.phase == Phase::Settling || s.phase == Phase::Done || s.phase == Phase::Fault) n.live.seen = false;
  if (!n.telemetrySeen || s.pulses >= n.telemetryPulses) {
    n.telemetryPulses = s.pulses; n.telemetryAt = now; n.telemetrySeen = true;
  }
  const uint32_t bit = uint32_t(1) << index;
  if (!active()) {
    // Reconcile late terminal telemetry in memory as well as in persistent storage.
    if (s.key == report_.key && (report_.selected & bit) &&
        (s.phase == Phase::Done || s.phase == Phase::Fault)) {
      report_.complete |= bit;
      report_.unknown &= ~bit;
      if (s.reason == Reason::Reboot) report_.unknown |= bit;
      const bool validCalibration = manualCalibration_ && s.reason == Reason::Manual && s.quiet && !s.truncated &&
          calibrationMarked_ && s.closePulses > calibrationStartPulses_;
      if (manualCalibration_ ? !validCalibration : (s.reason != Reason::Target || !s.quiet || s.truncated)) report_.failed |= bit;
      if ((report_.complete & report_.selected) == report_.selected && !report_.unknown)
        report_.phase = report_.failed ? SchedulePhase::Failed : SchedulePhase::Complete;
      else if (report_.unknown || report_.failed) report_.phase = SchedulePhase::Failed;
    }
    return;
  }
  if (s.key != report_.key) return;
  if (!(batch_ & bit)) return;
  if ((s.phase == Phase::Settling || s.phase == Phase::Done || s.phase == Phase::Fault) &&
      s.reason != Reason::Target && report_.phase != SchedulePhase::Draining) {
    stop(s.reason, now);
  }
  if (s.phase == Phase::Settling || s.phase == Phase::Done || s.phase == Phase::Fault) {
    closed_ |= bit;
    net_.retire(source,report_.key,Op::Start); net_.retire(source,report_.key,Op::Lease);
    if (closed_ == batch_ && report_.phase != SchedulePhase::Draining) {
      pump_.stop(now); report_.phase=SchedulePhase::Draining; stageAt_=txAt_=now;
    }
  }
  if (s.phase == Phase::Done || s.phase == Phase::Fault) {
    final_ |= bit; report_.complete |= bit;
    report_.unknown &= ~bit;
    if (s.reason == Reason::Reboot) report_.unknown |= bit;
    const bool validCalibration = manualCalibration_ && s.reason == Reason::Manual && s.quiet && !s.truncated &&
        calibrationMarked_ && s.closePulses > calibrationStartPulses_;
    if (manualCalibration_ ? !validCalibration : (s.reason != Reason::Target || !s.quiet || s.truncated)) report_.failed |= bit;
  }
}
void MasterScheduler::submissions(uint32_t now) {
  MeshSubmission sent;
  while (net_.takeSubmission(sent)) {
    const int i = find(sent.packet.peer);
    if (i < 0 || !active()) continue;
    auto &n=nodes_[i]; const auto op=sent.packet.op;
    if (op == Op::CapabilitiesGet) {
      Reader r{sent.packet}; if (!preflight_ || r.u32()!=capNonce_) continue;
      if (sent.committed) { n.commandSent=true; n.commandAt=sent.issuedAt; ++n.attempts; }
      else n.commandPending=false;
    } else {
      if (MeshTxGovernor::key(sent.packet)!=report_.key || abort_) continue;
      if (sent.error) { guard_="TX_ERROR"; stop(Reason::Transport,now); continue; }
      if (op==Op::Prepare || op==Op::Start) {
        if ((op==Op::Prepare && (preflight_ || report_.phase!=SchedulePhase::Preparing)) ||
            (op==Op::Start && report_.phase!=SchedulePhase::Opening)) continue;
        if (sent.committed) {
          n.commandCommitted=n.commandSent=true; n.commandAt=sent.issuedAt; ++n.attempts;
          if (op==Op::Start && !n.startIssued) {
            n.startIssued=true; n.firstStartIssuedAt=sent.issuedAt;
            if (!bootstrap_) { bootstrap_=true; bootstrapAt_=sent.issuedAt; }
          }
        } else {
          n.commandPending=false; n.commandAccepted=true;
          if (n.commandEarly) { n.commandEarly=false; commandReply(sent.packet.peer,report_.key,op,n.commandResult,now); }
        }
      } else if (op==Op::Lease) {
        const uint32_t sequence=leaseSequence(sent.packet);
        if (sent.committed) {
          auto &v=n.leases[n.leaseCursor]; n.leaseCursor=uint8_t((n.leaseCursor+1)%10);
          v={}; v.sequence=sequence; v.issuedAt=sent.issuedAt;
          n.leaseSent=true; n.leaseAt=sent.issuedAt;
          const uint32_t offset=report_.phase==SchedulePhase::Supplying ?
            (leasePhase(size_t(i))+1000-uint32_t(sent.issuedAt-stageAt_)%1000)%1000 : 0;
          n.nextLeaseAt=sent.issuedAt+1000+offset;
        } else {
          n.leasePending=false;
          for (auto &v:n.leases) if (v.sequence==sequence) {
            v.accepted=true;
            if (v.early) { v.early=false; leaseAck(sent.packet.peer,report_.key,sequence,v.result,now); }
          }
        }
      }
    }
    if (sent.error) { guard_="TX_ERROR"; stop(Reason::Transport,now); }
  }
}
bool MasterScheduler::capabilities(uint16_t source, uint32_t nonce, uint16_t revision,
                                   uint16_t features, uint32_t buildId, uint32_t now) {
  const int i = find(source);
  if (i < 0 || !preflight_ || report_.phase != SchedulePhase::Preparing || nonce != capNonce_ ||
      !(report_.selected & (uint32_t(1) << i))) return false;
  // Compatibility comes from the protocol and features, not a list of build hashes.
  if (revision != ProtocolRevision || (features & this->features(size_t(i))) != this->features(size_t(i)) || !buildId) {
    guard_="PROTOCOL_MISMATCH"; report_.failed |= uint32_t(1) << i; stop(Reason::Protocol,now); return true;
  }
  auto &n = nodes_[i];
  if (!n.seen || elapsed(now, n.seenAt, 15000) || (n.helloBuildId && n.helloBuildId != buildId)) {
    guard_="PROTOCOL_MISMATCH"; report_.failed |= uint32_t(1) << i; stop(Reason::Protocol,now); return true;
  }
  n.capable = true; n.buildId = buildId; n.capabilityRevision = revision;
  n.capabilityFeatures = features;
  if (!n.helloBuildId) n.helloBuildId = buildId;
  return true;
}
bool MasterScheduler::leaseAck(uint16_t source, JobKey key, uint32_t sequence, Reply result, uint32_t now) {
  const int i = find(source);
  if (i < 0 || abort_ || (report_.phase != SchedulePhase::Opening && report_.phase != SchedulePhase::Supplying) ||
      key != report_.key || !sequence || result > Reply::Unknown) return false;
  if (report_.phase == SchedulePhase::Opening && bootstrap_ && elapsed(now,bootstrapAt_,4000)) return false;
  const uint32_t bit = uint32_t(1) << i;
  if (!(batch_ & bit) || !(opened_ & bit) || (closed_ & bit)) return false;
  auto &n = nodes_[i];
  for (auto &v : n.leases) if (v.sequence == sequence) {
    if (v.acked || elapsed(now,v.issuedAt,LeaseAckTimeoutMs)) return false;
    if (!v.accepted) { if (!v.early) { v.early=true; v.result=result; } return false; }
    v.acked = true;
    if (result != Reply::Ok) {
      report_.failed |= bit;
      stop(result == Reply::Stale ? Reason::Lease : Reason::Transport,now);
      return true;
    }
    if (n.leaseAckSeen && sequence<=n.leaseAckSequence) return false;
    n.leaseAckSeen = true; n.leaseAckSequence = sequence; n.leaseAckAt = v.issuedAt;
    return true;
  }
  return false; // Unknown / not-yet-issued sequence can never pre-authorize a future lease.
}
bool MasterScheduler::compactLeaseAck(uint16_t source, uint32_t epoch, uint32_t sequence, Reply result, uint32_t now) {
  if (epoch!=report_.key.epoch) return false;
  return leaseAck(source,report_.key,sequence,result,now);
}
bool MasterScheduler::targetClosed(uint16_t source, JobKey key, uint32_t now) {
  const int i=find(source); if (i<0 || !active() || key!=report_.key) return false;
  const uint32_t bit=uint32_t(1)<<i;
  if (!(batch_ & bit) || !nodes_[i].startIssued) return false;
  closed_|=bit;
  nodes_[i].live.seen=false;
  net_.retire(source,key,Op::Start); net_.retire(source,key,Op::Lease);
  if (closed_==batch_ && (report_.phase==SchedulePhase::Opening || report_.phase==SchedulePhase::Supplying)) {
    pump_.stop(now); report_.phase=SchedulePhase::Draining; stageAt_=txAt_=now;
  }
  return true; // Does not set final, pulses, complete or clear an existing fault.
}
uint32_t MasterScheduler::leasePhase(size_t slot) const {
  unsigned ordinal=0;
  for(size_t i=0;i<slot;++i) if(batch_ & (uint32_t(1)<<i)) ++ordinal;
  return (ordinal%5)*200;
}
void MasterScheduler::leases(uint32_t now) {
  for (size_t i=0;i<MaxNodes;++i) {
    const uint32_t bit=uint32_t(1)<<i; auto &n=nodes_[i];
    if (!(batch_ & bit) || !(opened_ & bit) || (closed_ & bit) || n.leasePending) continue;
    if (!n.leaseSent || int32_t(now-n.nextLeaseAt)>=0) {
      auto p=packet(Op::Lease,n.config.address); p.context=report_.key;
      Writer w{p}; w.u32(report_.key.epoch); w.u32(0);
      if (net_.send(p)) n.leasePending=true;
      else { guard_="TX_ERROR"; stop(Reason::Transport,now); return; }
    }
  }
}
bool MasterScheduler::commandReply(uint16_t source, JobKey key, Op command, Reply result, uint32_t now) {
  const int index=find(source);
  if (index<0 || !active() || abort_ || preflight_ || key!=report_.key || result>Reply::Unknown ||
      (command!=Op::Prepare && command!=Op::Start)) return false;
  if ((command==Op::Prepare && report_.phase!=SchedulePhase::Preparing) ||
      (command==Op::Start && report_.phase!=SchedulePhase::Opening)) return false;
  if (command==Op::Prepare && elapsed(now,stageAt_,15000)) return false;
  const uint32_t bit=uint32_t(1)<<index; auto &n=nodes_[index];
  if (!(batch_ & bit) || (closed_ & bit) || !n.commandCommitted) return false;
  if (!n.commandAccepted) { if (!n.commandEarly) { n.commandEarly=true; n.commandResult=result; } return false; }
  if (command==Op::Start && bootstrap_ && elapsed(now,bootstrapAt_,4000)) return false;
  if (result==Reply::Ok) {
    net_.retire(source,key,command); n.commandPending=false;
    if (command==Op::Prepare) prepared_|=bit;
    else {
      if (!n.startAckSeen) { n.startAckSeen=true; n.startAckAt=n.openedAt=now; }
      opened_|=bit;
    }
    return true;
  }
  guard_="COMMAND_REJECT"; report_.failed|=bit; stop(Reason::Transport,now); return true;
}
bool MasterScheduler::result(uint16_t source, const ValveStatus &s, uint32_t now) {
  if (!terminalResult(s) || find(source)<0) return false;
  status(source, s, now);
  return sameResult(nodes_[find(source)].status,s);
}
void MasterScheduler::tick(uint32_t now) {
  submissions(now);
  if (!active()) return;
  if (calibrationMarkPending_ && report_.phase == SchedulePhase::Supplying) {
    if (elapsed(now,calibrationMarkAt_,CriticalQueueTtlMs)) {
      guard_="CALIBRATION_MARK_TIMEOUT";
      stop(Reason::Transport,now);
      return;
    }
    if (calibrationMarkAttempts_ < 4 && elapsed(now,calibrationMarkAt_,500U * calibrationMarkAttempts_)) {
      size_t target=MaxNodes;
      for (size_t i=0;i<MaxNodes;++i) if (batch_ & (uint32_t(1)<<i)) { target=i; break; }
      if (target<MaxNodes) {
        auto p=packet(Op::MeasureMark,nodes_[target].config.address); Writer w{p}; w.key(report_.key);
        if (!net_.send(p)) { guard_="TX_ERROR"; stop(Reason::Transport,now); return; }
        ++calibrationMarkAttempts_;
      }
    }
  }
  if (!net_.healthy() && report_.phase != SchedulePhase::Draining) { guard_="TX_ERROR"; stop(Reason::Transport,now); }
  if (pump_.phase() == PumpPhase::Fault && report_.phase != SchedulePhase::Draining) stop(Reason::PumpFault,now);
  if (report_.phase == SchedulePhase::Between) {
    if (pump_.ready(now)) batch(now);
    else if (elapsed(now,stageAt_,60000)) stop(Reason::PumpFault,now);
    return;
  }
  if (report_.phase == SchedulePhase::Preparing || report_.phase == SchedulePhase::Opening) {
    if (report_.phase==SchedulePhase::Preparing && elapsed(now,stageAt_,15000)) {
      guard_=preflight_ ? "PROTOCOL_MISMATCH" : "PREPARE_TIMEOUT";
      stop(preflight_ ? Reason::Protocol : Reason::Transport,now); return;
    }
    // Capability probes for ALL selected nodes finish before any Start.
    const uint32_t targets = preflight_ ? report_.selected : batch_;
    const uint32_t acked = report_.phase == SchedulePhase::Preparing ? prepared_ : opened_;
    bool allCapable = true;
    // First submissions precede retries; rotate the first slot for fair admission.
    for (unsigned pass=0;pass<2;++pass) for (size_t j=0;j<MaxNodes;++j) {
      const size_t i=(commandCursor_+j)%MaxNodes;
      const uint32_t bit=uint32_t(1)<<i; if (!(targets & bit) || (closed_ & bit)) continue;
      auto &n=nodes_[i];
      if (preflight_) { if (n.capable) continue; allCapable=false; }
      else if (acked & bit) continue;
      if ((n.attempts==0)!=(pass==0) || n.attempts>=3 || n.commandPending ||
          (n.commandSent && !elapsed(now,n.commandAt,1500))) continue;
      MeshTxGovernor::Admission admission;
      if (preflight_) {
        auto p=packet(Op::CapabilitiesGet,n.config.address); Writer w{p}; w.u32(capNonce_);
        admission=net_.send(p) ? MeshTxGovernor::Admission::Accepted : MeshTxGovernor::Admission::Failed;
      } else admission=sendNode(i,report_.phase==SchedulePhase::Preparing ? Op::Prepare : Op::Start);
      if (admission==MeshTxGovernor::Admission::Accepted) n.commandPending=true;
      else if (admission==MeshTxGovernor::Admission::Failed) { guard_="TX_ERROR"; stop(Reason::Transport,now); return; }
    }
    commandCursor_=(commandCursor_+1)%MaxNodes;
    if (preflight_ && allCapable) {
      preflight_=false; stageAt_=now;
      for (auto &n:nodes_) { n.commandPending=n.commandSent=n.commandCommitted=n.commandAccepted=n.commandEarly=false; n.attempts=0; }
      return;
    }
    if (report_.phase == SchedulePhase::Preparing && !preflight_ && (prepared_ & batch_) == batch_) {
      report_.phase=SchedulePhase::Opening; stageAt_=now;
      for (auto &n:nodes_) { n.commandPending=n.commandSent=n.commandCommitted=n.commandAccepted=n.commandEarly=false; n.attempts=0; }
    }
    if (report_.phase == SchedulePhase::Opening) {
      // Fixed bootstrap starts at the FIRST actual API submission, never on a retry.
      if ((bootstrap_ && elapsed(now,bootstrapAt_,4000)) || (!bootstrap_ && elapsed(now,stageAt_,1000))) {
        guard_=bootstrap_ ? "BOOTSTRAP_TIMEOUT" : "START_DISPATCH_TIMEOUT";
        stop(Reason::Transport,now); return;
      }
      if (closed_ == batch_) { report_.phase=SchedulePhase::Draining; stageAt_=now; return; }
      leases(now); if (report_.phase != SchedulePhase::Opening) return;
      bool fresh = bootstrap_ && ((opened_ | closed_) & batch_) == batch_;
      bool valvesReady = true;
      for (size_t i=0;i<MaxNodes;++i) if ((batch_ & (uint32_t(1)<<i)) && !(closed_ & (uint32_t(1)<<i))) {
        const auto &n=nodes_[i];
        if (n.leaseAckSeen && elapsed(now,n.leaseAckAt,2000)) guard_="LEASE_PROOF_OLD";
        if (!n.startIssued || !n.commandAccepted || !n.startAckSeen || !n.leaseAckSeen || elapsed(now,n.leaseAckAt,2000) || int32_t(n.leaseAckAt-n.startAckAt)<0) fresh=false;
        if (!n.startAckSeen || !elapsed(now,n.startAckAt,ValveOpenLeadMs)) valvesReady=false;
      }
      if (fresh && !valvesReady) guard_="VALVE_OPEN_WAIT";
      if (fresh && valvesReady) {
        if (pump_.start(report_.key,++pumpSeq_,now)!=Reply::Ok) stop(Reason::PumpFault,now);
        else {
          guard_="NONE"; report_.phase=SchedulePhase::Supplying; stageAt_=pumpRenewAt_=now;
          for (size_t i=0;i<MaxNodes;++i) if (batch_ & (uint32_t(1)<<i)) {
            auto &n=nodes_[i]; uint32_t minimum=n.leaseAt+1000;
            if(int32_t(now-minimum)>0) minimum=now;
            n.nextLeaseAt=minimum+(leasePhase(size_t(i))+1000-uint32_t(minimum-now)%1000)%1000;
          }
        }
      }
    } else if (elapsed(now,stageAt_,15000)) stop(preflight_ ? Reason::Protocol : Reason::Transport,now);
  } else if (report_.phase == SchedulePhase::Supplying) {
    bool expired=false;
    for (size_t i=0;i<MaxNodes;++i) {
      const uint32_t bit=uint32_t(1)<<i; const auto &n=nodes_[i];
      if ((batch_ & bit) && !(closed_ & bit) && (!n.leaseAckSeen || elapsed(now,n.leaseAckAt,LeaseAckTimeoutMs))) expired=true;
    }
    // Protection comes BEFORE every local authorization renewal.
    if (expired) { stop(Reason::Lease,now); return; }
    if (closed_ == batch_) {
      pump_.stop(now); report_.phase=SchedulePhase::Draining; stageAt_=now;
    } else if (elapsed(now,stageAt_,maxMs_+5000)) stop(Reason::Runtime,now);
    else {
      leases(now); if (report_.phase != SchedulePhase::Supplying) return;
      if (elapsed(now,pumpRenewAt_,250)) {
        if (pump_.renew(report_.key,++pumpSeq_,now)!=Reply::Ok) stop(Reason::PumpFault,now);
        pumpRenewAt_=now;
      }
    }
  } else if (report_.phase == SchedulePhase::Draining) {
    if (abort_ && elapsed(now,closeAt_,700)) { closeBatch(stopReason_); closeAt_=now; }
    if ((final_ & batch_) != batch_ && elapsed(now,stageAt_,3500) && elapsed(now,txAt_,1000)) {
      for (size_t j=0;j<MaxNodes;++j) {
        const size_t i=cursor_++%MaxNodes;
        if ((batch_ & (uint32_t(1)<<i)) && !(final_ & (uint32_t(1)<<i))) { sendNode(i,Op::ResultQuery); break; }
      }
      txAt_=now;
    }
    if ((final_ & batch_)==batch_ || elapsed(now,stageAt_,10000)) {
      report_.unknown |= batch_ & ~final_;
      if (pump_.output()) return;
      if (report_.failed || report_.unknown || abort_) {
        report_.failed |= remaining_; remaining_=0; report_.phase=SchedulePhase::Failed;
      } else if (remaining_) { report_.phase=SchedulePhase::Between; stageAt_=now; }
      else { net_.fence(report_.key); report_.phase=SchedulePhase::Complete; }
    }
  }
}
}
