#include "master_scheduler.h"
#include "control_request.h"
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
void MasterScheduler::hello(uint16_t source, uint32_t k, uint32_t ver, bool commissioned,
                            uint32_t now, uint32_t buildId) {
  const int i = find(source); if (i < 0) return;
  auto &n = nodes_[i]; n.seen = true; n.seenAt = now;
  if (n.helloBuildId != buildId) n.capable = false;
  n.helloBuildId = buildId;
  n.commissioned = commissioned && n.config.k == k && n.config.version == ver;
}
Reply MasterScheduler::check(uint32_t mask, uint32_t ml, uint32_t maxMs, uint32_t now, const uint8_t *amounts) {
  if (active()) return Reply::Busy;
  if (!mask || (amounts ? ml != 0 || !validAmounts(mask, amounts) : !ml) ||
      !maxMs || maxMs > 240000 || !concurrent_ || concurrent_ > MaxNodes) return Reply::Invalid;
  if (!pump_.ready(now)) return Reply::NotReady;
  for (size_t i = 0; i < MaxNodes; ++i) if (mask & (uint32_t(1) << i)) {
    const auto &n = nodes_[i]; uint32_t pulses = 0;
    if (!n.config.address || !n.seen || elapsed(now, n.seenAt, 15000)) return Reply::Offline;
    if (!n.commissioned || !targetPulses(amounts ? uint32_t(amounts[i]) * 50 : ml, n.config.k, pulses)) return Reply::NotReady;
    if (n.status.phase >= Phase::Prepared && n.status.phase <= Phase::Settling) return Reply::Busy;
  }
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
void MasterScheduler::begin(JobKey key, uint32_t mask, uint32_t ml, uint32_t maxMs, uint32_t now, const uint8_t *amounts) {
  report_ = {}; report_.key = key; report_.selected = remaining_ = mask;
  ml_ = ml; maxMs_ = maxMs; pumpSeq_ = 0; abort_ = false; stopReason_ = Reason::None;
  capNonce_ = key.epoch ^ (key.id * 2654435761U); if (!capNonce_) capNonce_ = 1;
  preflight_ = true;
  for (auto &n : nodes_) {
    if (n.capable && (n.capabilityRevision != ProtocolRevision ||
        (n.capabilityFeatures & RequiredFeatures) != RequiredFeatures ||
        (n.helloBuildId && n.helloBuildId != n.buildId) || !n.seen || elapsed(now, n.seenAt, 15000)))
      n.capable = false;
  }
  for (size_t i = 0; i < MaxNodes; ++i) amounts_[i] = amounts ? amounts[i] : 0;
  batch(now);
}
void MasterScheduler::batch(uint32_t now) {
  batch_ = prepared_ = opened_ = closed_ = final_ = 0;
  bootstrap_ = false;
  for (auto &n : nodes_) {
    n.commandPending = n.leasePending = n.commandSent = n.leaseSent = false;
    n.attempts = n.leaseCursor = 0; n.leaseAckSeen = false;
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
bool MasterScheduler::sendNode(size_t i, Op op) {
  auto p = packet(op, nodes_[i].config.address); Writer w{p}; w.key(report_.key);
  if (op == Op::Prepare) {
    const uint32_t ml = ml_ ? ml_ : uint32_t(amounts_[i]) * 50;
    uint32_t pulses = 0; targetPulses(ml, nodes_[i].config.k, pulses);
    w.u32(ml); w.u32(pulses); w.u32(nodes_[i].config.k);
    w.u32(nodes_[i].config.version); w.u32(maxMs_);
    const uint16_t speed = pump_.speed() == 1 ? SpeedLowPermille : pump_.speed() == 2 ? SpeedMediumPermille : SpeedHighPermille;
    w.u16(speed); w.u32(LeaseMs); w.u8(0);
  }
  return net_.send(p);
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
  abort_ = true; stopReason_ = report_.reason = r; report_.failed |= remaining_ | (batch_ & ~final_); remaining_ = 0;
  net_.fence(report_.key);
  closeBatch(r); report_.phase = SchedulePhase::Draining; stageAt_ = closeAt_ = now; txAt_ = now;
}
bool MasterScheduler::summary(uint16_t source, JobKey key, uint32_t pulses, uint32_t revision, uint32_t now) {
  const int i = find(source); if (i < 0) return false;
  auto &n = nodes_[i];
  if (key != n.status.key || revision != n.status.revision ||
      pulses < n.status.pulses || (n.telemetrySeen && pulses < n.telemetryPulses)) return false;
  n.telemetryPulses = pulses; n.telemetryAt = now; n.telemetrySeen = true;
  n.seen = true; n.seenAt = now; return true; // Monitoring only; never advances the state machine.
}
void MasterScheduler::status(uint16_t source, const ValveStatus &s, uint32_t now) {
  const int index = find(source); if (index < 0) return;
  auto &n = nodes_[index]; n.seenAt = now; n.seen = true;
  // The detailed event counter rejects out-of-order phases within the same dose.
  if (s.key == n.status.key && s.revision < n.status.revision) return;
  if (n.status.key.valid() && s.key != n.status.key && !s.key.newer(n.status.key)) return;
  if (s.key != n.status.key) n.telemetrySeen = false;
  n.status = s;
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
      if (s.reason != Reason::Target || !s.quiet || s.truncated) report_.failed |= bit;
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
  if (s.phase == Phase::Prepared) prepared_ |= bit;
  if (s.phase == Phase::Opening || s.phase == Phase::Running) {
    if (!(opened_ & bit)) n.openedAt = now;
    opened_ |= bit;
  }
  if (s.phase == Phase::Settling || s.phase == Phase::Done || s.phase == Phase::Fault) {
    opened_ |= bit; closed_ |= bit;
  }
  if (s.phase == Phase::Done || s.phase == Phase::Fault) {
    final_ |= bit; report_.complete |= bit;
    if (s.reason == Reason::Reboot) report_.unknown |= bit;
    if (s.reason != Reason::Target || !s.quiet || s.truncated) report_.failed |= bit;
  }
}
void MasterScheduler::submissions(uint32_t now) {
  MeshSubmission sent;
  while (net_.takeSubmission(sent)) {
    const int i = find(sent.packet.peer);
    if (i < 0 || !active()) continue;
    auto &n = nodes_[i];
    if (sent.packet.op == Op::CapabilitiesGet) {
      if (!preflight_ || sent.committed) continue;
      Reader r{sent.packet}; if (r.u32() != capNonce_) continue;
      n.commandPending = false; n.commandSent = true; n.commandAt = sent.issuedAt; ++n.attempts;
    } else {
      Reader r{sent.packet}; if (r.key() != report_.key || abort_) continue;
      if (sent.packet.op == Op::Prepare || sent.packet.op == Op::Start) {
        if ((sent.packet.op == Op::Prepare && report_.phase != SchedulePhase::Preparing) ||
            (sent.packet.op == Op::Start && report_.phase != SchedulePhase::Opening)) continue;
        if (sent.packet.op == Op::Start && !bootstrap_) { bootstrap_ = true; bootstrapAt_ = sent.issuedAt; }
        if (sent.committed) continue;
        n.commandPending = false; n.commandSent = true; n.commandAt = sent.issuedAt; ++n.attempts;
      } else if (sent.packet.op == Op::Lease) {
        const uint32_t sequence = r.u32();
        if (sent.committed) {
          auto &v=n.leases[n.leaseCursor++ % 3]; v={}; v.sequence=sequence; v.issuedAt=sent.issuedAt;
          continue;
        }
        n.leasePending = false; n.leaseSent = true; n.leaseAt = sent.issuedAt;
        for (auto &v:n.leases) if (v.sequence == sequence && !sent.error) {
          v.accepted=true;
          if (v.early) { v.early=false; leaseAck(sent.packet.peer,report_.key,sequence,v.result,now); }
        }
      }
    }
    if (sent.error) stop(Reason::Transport,now);
  }
}
bool MasterScheduler::capabilities(uint16_t source, uint32_t nonce, uint16_t revision,
                                   uint16_t features, uint32_t buildId, uint32_t now) {
  const int i = find(source);
  if (i < 0 || !preflight_ || report_.phase != SchedulePhase::Preparing || nonce != capNonce_ ||
      !(report_.selected & (uint32_t(1) << i))) return false;
  if (revision != ProtocolRevision || (features & RequiredFeatures) != RequiredFeatures || !approvedSlaveBuild(buildId)) {
    report_.failed |= uint32_t(1) << i; stop(Reason::Protocol,now); return true;
  }
  auto &n = nodes_[i];
  if (!n.seen || elapsed(now, n.seenAt, 15000) || (n.helloBuildId && n.helloBuildId != buildId)) {
    report_.failed |= uint32_t(1) << i; stop(Reason::Protocol,now); return true;
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
  if (report_.phase == SchedulePhase::Opening && bootstrap_ && elapsed(now,bootstrapAt_,2400)) return false;
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
    if (n.leaseAckSeen && !serialNewer(sequence,n.leaseAckSequence)) return false;
    n.leaseAckSeen = true; n.leaseAckSequence = sequence; n.leaseAckAt = v.issuedAt;
    return true;
  }
  return false; // Unknown / not-yet-issued sequence can never pre-authorize a future lease.
}
void MasterScheduler::leases(uint32_t now) {
  for (size_t i=0;i<MaxNodes;++i) {
    const uint32_t bit=uint32_t(1)<<i; auto &n=nodes_[i];
    if (!(batch_ & bit) || !(opened_ & bit) || (closed_ & bit) || n.leasePending) continue;
    if (!n.leaseSent || elapsed(now,n.leaseAt,1000)) {
      auto p=packet(Op::Lease,n.config.address); Writer w{p}; w.key(report_.key); w.u32(0);
      if (net_.send(p)) n.leasePending=true;
      else { stop(Reason::Transport,now); return; }
    }
  }
}
bool MasterScheduler::commandReply(uint16_t source, JobKey key, Op command, Reply result, uint32_t now) {
  const int index = find(source);
  if (index < 0 || !active() || key != report_.key || result > Reply::Unknown ||
      (command != Op::Prepare && command != Op::Start)) return false;
  if ((command == Op::Prepare && report_.phase != SchedulePhase::Preparing) ||
      (command == Op::Start && report_.phase != SchedulePhase::Opening)) return false;
  const uint32_t bit = uint32_t(1) << index;
  if (!(batch_ & bit)) return false;
  if (result == Reply::Ok) {
    if (command == Op::Prepare) prepared_ |= bit;
    else opened_ |= bit;
    return true;
  }
  report_.failed |= bit;
  stop(Reason::Transport, now);
  return true;
}
bool MasterScheduler::result(uint16_t source, const ValveStatus &s, uint32_t now) {
  if (s.phase != Phase::Done && s.phase != Phase::Fault) return false;
  status(source, s, now);
  return true;
}
void MasterScheduler::tick(uint32_t now) {
  submissions(now);
  if (!active()) return;
  if (!net_.healthy() && report_.phase != SchedulePhase::Draining) stop(Reason::Transport,now);
  if (pump_.phase() == PumpPhase::Fault && report_.phase != SchedulePhase::Draining) stop(Reason::PumpFault,now);
  if (report_.phase == SchedulePhase::Between) {
    if (pump_.ready(now)) batch(now);
    else if (elapsed(now,stageAt_,60000)) stop(Reason::PumpFault,now);
    return;
  }
  if (report_.phase == SchedulePhase::Preparing || report_.phase == SchedulePhase::Opening) {
    // Capability probes for ALL selected nodes finish before any Start.
    const uint32_t targets = preflight_ ? report_.selected : batch_;
    const uint32_t acked = report_.phase == SchedulePhase::Preparing ? prepared_ : opened_;
    bool allCapable = true;
    for (size_t i=0;i<MaxNodes;++i) {
      const uint32_t bit=uint32_t(1)<<i; if (!(targets & bit)) continue;
      auto &n=nodes_[i];
      if (preflight_) { if (n.capable) continue; allCapable=false; }
      else if (acked & bit) continue;
      const uint32_t backoff = n.attempts <= 1 ? 300 : n.attempts == 2 ? 600 : 1000;
      if (n.commandPending || (n.commandSent && !elapsed(now,n.commandAt,backoff))) continue;
      bool ok;
      if (preflight_) {
        auto p=packet(Op::CapabilitiesGet,n.config.address); Writer w{p}; w.u32(capNonce_); ok=net_.send(p);
      } else ok=sendNode(i,report_.phase == SchedulePhase::Preparing ? Op::Prepare : Op::Start);
      if (ok) n.commandPending=true; else { stop(Reason::Transport,now); return; }
    }
    if (preflight_ && allCapable) {
      preflight_=false; stageAt_=now;
      for (auto &n:nodes_) { n.commandPending=n.commandSent=false; n.attempts=0; }
      return;
    }
    if (report_.phase == SchedulePhase::Preparing && !preflight_ && (prepared_ & batch_) == batch_) {
      report_.phase=SchedulePhase::Opening; stageAt_=now;
      for (auto &n:nodes_) { n.commandPending=n.commandSent=false; n.attempts=0; }
    }
    if (report_.phase == SchedulePhase::Opening) {
      // Fixed bootstrap starts at the FIRST actual API submission, never on a retry.
      if ((bootstrap_ && elapsed(now,bootstrapAt_,2400)) || elapsed(now,stageAt_,4000)) {
        stop(Reason::Transport,now); return;
      }
      if (closed_ == batch_) { report_.phase=SchedulePhase::Draining; stageAt_=now; return; }
      leases(now); if (report_.phase != SchedulePhase::Opening) return;
      bool fresh = bootstrap_ && (opened_ & batch_) == batch_;
      for (size_t i=0;i<MaxNodes;++i) if ((batch_ & (uint32_t(1)<<i)) && !(closed_ & (uint32_t(1)<<i))) {
        const auto &n=nodes_[i];
        if (!n.leaseAckSeen || elapsed(now,n.leaseAckAt,1001) || int32_t(n.leaseAckAt-n.openedAt)<0) fresh=false;
      }
      if (fresh) {
        if (pump_.start(report_.key,++pumpSeq_,now)!=Reply::Ok) stop(Reason::PumpFault,now);
        else { report_.phase=SchedulePhase::Supplying; stageAt_=pumpRenewAt_=now; }
      }
    } else if (elapsed(now,stageAt_,10000)) stop(preflight_ ? Reason::Protocol : Reason::Transport,now);
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
    if ((final_ & batch_) != batch_ && elapsed(now,txAt_,300)) {
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
