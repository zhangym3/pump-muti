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
void MasterScheduler::hello(uint16_t source, uint32_t k, uint32_t ver, bool commissioned, uint32_t now) {
  const int i = find(source); if (i < 0) return;
  auto &n = nodes_[i]; n.seen = true; n.seenAt = now;
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
  ml_ = ml; maxMs_ = maxMs; leaseSeq_ = pumpSeq_ = 0; abort_ = false;
  for (size_t i = 0; i < MaxNodes; ++i) amounts_[i] = amounts ? amounts[i] : 0;
  batch(now);
}
void MasterScheduler::batch(uint32_t now) {
  batch_ = prepared_ = opened_ = closed_ = final_ = 0;
  uint8_t count = 0;
  for (size_t i = 0; i < MaxNodes && count < concurrent_; ++i) {
    const uint32_t bit = uint32_t(1) << i;
    if (remaining_ & bit) { batch_ |= bit; remaining_ &= ~bit; ++count; }
  }
  report_.phase = SchedulePhase::Preparing; stageAt_ = now;
  txAt_ = now - 100; leaseAt_ = now - 1000; cursor_ = 0;
}
void MasterScheduler::sendNode(size_t i, Op op) {
  auto p = packet(op, nodes_[i].config.address); Writer w{p}; w.key(report_.key);
  if (op == Op::Prepare) {
    const uint32_t ml = ml_ ? ml_ : uint32_t(amounts_[i]) * 50;
    uint32_t pulses = 0; targetPulses(ml, nodes_[i].config.k, pulses);
    w.u32(ml); w.u32(pulses); w.u32(nodes_[i].config.k);
    w.u32(nodes_[i].config.version); w.u32(maxMs_);
  }
  net_.send(p);
}
void MasterScheduler::closeBatch(Reason r) {
  auto p = packet(Op::Close, AllSlaves); Writer w{p}; w.key(report_.key); w.u8(uint8_t(r)); net_.send(p);
}
void MasterScheduler::stop(Reason r, uint32_t now) {
  pump_.stop(now); // Always local pump first, before any radio operation.
  if (!active()) return;
  if (abort_ && report_.phase == SchedulePhase::Draining) return;
  abort_ = true; stopReason_ = r; report_.failed |= remaining_; remaining_ = 0;
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
  if (s.phase == Phase::Opening || s.phase == Phase::Running) opened_ |= bit;
  if (s.phase == Phase::Settling || s.phase == Phase::Done || s.phase == Phase::Fault) {
    opened_ |= bit; closed_ |= bit;
  }
  if (s.phase == Phase::Done || s.phase == Phase::Fault) {
    final_ |= bit; report_.complete |= bit;
    if (s.reason == Reason::Reboot) report_.unknown |= bit;
    if (s.reason != Reason::Target || !s.quiet || s.truncated) report_.failed |= bit;
  }
}
bool MasterScheduler::leaseAck(uint16_t source, JobKey key, uint32_t sequence, Reply result, uint32_t now) {
  const int index = find(source);
  if (index < 0 || !active() || report_.phase != SchedulePhase::Supplying ||
      key != report_.key || !sequence || sequence != leaseSeq_ || result > Reply::Unknown) return false;
  const uint32_t bit = uint32_t(1) << index;
  if (!(batch_ & bit) || !(opened_ & bit) || (closed_ & bit)) return false;
  auto &node = nodes_[index];
  // A retransmitted ACK for the same multicast lease proves only the original
  // renewal arrived; it must not keep the master's lease watchdog alive.
  if (node.leaseAckSeen && node.leaseAckSequence == sequence) return false;
  if (result != Reply::Ok) {
    stop(Reason::Transport, now);
    return true;
  }
  node.leaseAckAt = now;
  node.leaseAckSequence = sequence;
  node.leaseAckSeen = true;
  return true;
}
bool MasterScheduler::commandReply(uint16_t source, JobKey key, Op command, Reply result, uint32_t now) {
  const int index = find(source);
  if (index < 0 || !active() || key != report_.key || result > Reply::Unknown ||
      (command != Op::Prepare && command != Op::Start)) return false;
  if ((command == Op::Prepare && report_.phase != SchedulePhase::Preparing) ||
      (command == Op::Start && report_.phase != SchedulePhase::Opening)) return false;
  const uint32_t bit = uint32_t(1) << index;
  if (!(batch_ & bit)) return false;
  if (result == Reply::Ok) return true; // STATUS remains the physical-state acknowledgement.
  report_.failed |= bit;
  stop(Reason::Transport, now);
  return true;
}
void MasterScheduler::tick(uint32_t now) {
  if (!active()) return;
  if (pump_.phase() == PumpPhase::Fault && report_.phase != SchedulePhase::Draining) {
    stop(Reason::PumpFault, now);
  }
  if (report_.phase == SchedulePhase::Between) {
    if (pump_.ready(now)) batch(now);
    else if (elapsed(now, stageAt_, 60000)) stop(Reason::PumpFault, now);
    return;
  }
  if (report_.phase == SchedulePhase::Supplying && elapsed(now, leaseAt_, 1000)) {
    auto p = packet(Op::Lease, AllSlaves); Writer w{p}; w.key(report_.key); w.u32(++leaseSeq_);
    net_.send(p); leaseAt_ = now;
  }
  if (report_.phase == SchedulePhase::Preparing || report_.phase == SchedulePhase::Opening) {
    if (elapsed(now, txAt_, 150)) {
      const uint32_t acked = report_.phase == SchedulePhase::Preparing ? prepared_ : opened_;
      for (size_t attempt = 0; attempt < MaxNodes; ++attempt) {
        const size_t i = cursor_++ % MaxNodes;
        if ((batch_ & (uint32_t(1) << i)) && !(acked & (uint32_t(1) << i))) {
          sendNode(i, report_.phase == SchedulePhase::Preparing ? Op::Prepare : Op::Start); break;
        }
      }
      txAt_ = now;
    }
    if (report_.phase == SchedulePhase::Preparing && (prepared_ & batch_) == batch_) {
      report_.phase = SchedulePhase::Opening; stageAt_ = now; txAt_ = now - 150;
    } else if (report_.phase == SchedulePhase::Opening && (opened_ & batch_) == batch_) {
      if (closed_ == batch_) {
        report_.phase = SchedulePhase::Draining; stageAt_ = now;
      } else if (pump_.start(report_.key, ++pumpSeq_, now) != Reply::Ok) stop(Reason::PumpFault, now);
      else {
        for (size_t i = 0; i < MaxNodes; ++i) if (batch_ & (uint32_t(1) << i)) {
          nodes_[i].leaseAckAt = now;
          nodes_[i].leaseAckSequence = 0;
          nodes_[i].leaseAckSeen = false;
        }
        report_.phase = SchedulePhase::Supplying; stageAt_ = pumpRenewAt_ = now;
      }
    } else if (elapsed(now, stageAt_, report_.phase == SchedulePhase::Preparing ? 10000 : 4000)) {
      stop(Reason::Transport, now);
    }
  } else if (report_.phase == SchedulePhase::Supplying) {
    if (elapsed(now, pumpRenewAt_, 250)) {
      pump_.renew(report_.key, ++pumpSeq_, now); pumpRenewAt_ = now;
    }
    bool leaseExpired = false;
    for (size_t i = 0; i < MaxNodes; ++i) {
      const uint32_t bit = uint32_t(1) << i;
      if ((batch_ & bit) && (opened_ & bit) && !(closed_ & bit) &&
          elapsed(now, nodes_[i].leaseAckAt, LeaseAckTimeoutMs)) { leaseExpired = true; break; }
    }
    if (leaseExpired) stop(Reason::Lease, now);
    else if (closed_ == batch_) {
      pump_.stop(now); report_.phase = SchedulePhase::Draining; stageAt_ = now;
    } else if (elapsed(now, stageAt_, maxMs_ + 5000)) stop(Reason::Runtime, now);
  } else if (report_.phase == SchedulePhase::Draining) {
    if (abort_ && elapsed(now, closeAt_, 700)) { closeBatch(stopReason_); closeAt_ = now; }
    if ((final_ & batch_) != batch_ && elapsed(now, txAt_, 300)) {
      for (size_t j = 0; j < MaxNodes; ++j) {
        const size_t i = cursor_++ % MaxNodes;
        if ((batch_ & (uint32_t(1) << i)) && !(final_ & (uint32_t(1) << i))) { sendNode(i, Op::StatusGet); break; }
      }
      txAt_ = now;
    }
    if ((final_ & batch_) == batch_ || elapsed(now, stageAt_, 10000)) {
      report_.unknown |= batch_ & ~final_;
      if (pump_.output()) return;
      if (report_.failed || report_.unknown || abort_) {
        report_.failed |= remaining_; remaining_ = 0; report_.phase = SchedulePhase::Failed;
      } else if (remaining_) { report_.phase = SchedulePhase::Between; stageAt_ = now; }
      else report_.phase = SchedulePhase::Complete;
    }
  }
}
}
