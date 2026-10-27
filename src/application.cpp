#include "application.h"
#include "mesh_tx_diagnostics.h"
#include "recovery_diagnostics.h"
#include "hardware.h"
#include "result_commit.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstring>

namespace pump {
namespace {
bool decodeResult(const Packet &p, ValveStatus &s) {
  Reader r{p}; s.key = r.key(); s.pulses = r.u32(); s.closePulses = r.u32(); s.k = r.u32(); s.revision = r.u32();
  s.phase = Phase(r.u8()); s.reason = Reason(r.u8()); const auto flags = r.u8();
  s.valveCommand = flags & 1; s.quiet = flags & 2; s.truncated = flags & 4;
  return r.done() && s.key.valid() && (s.phase == Phase::Done || s.phase == Phase::Fault) &&
    s.reason <= Reason::Protocol && !(flags & ~7) && !s.valveCommand && validK(s.k);
}
}
bool Application::idle() const {
  return storageHealthy_ && resultsSettled() && !store_.maintenance().locked && !mode_.kind && !scheduler_.active() && !pump_.output() && !store_.removingAddress() && !flowCalibrationLocked();
}
uint32_t Application::registeredMask() const {
  uint32_t mask=0;
  for (size_t i=0;i<MaxNodes;++i) if (store_.state().nodes[i].address) mask|=uint32_t(1)<<i;
  return mask;
}
void Application::maintenanceState() {
  const auto &m=store_.maintenance(); char line[160];
  snprintf(line,sizeof(line),"MAINT locked=%u addr=%04x token=%lu:%lu boot=%lu pending=%u",
    m.locked,m.address,(unsigned long)m.epoch,(unsigned long)m.generation,(unsigned long)m.boot,mode_.kind);
  gateway_.publish(line);
}
bool Application::beginMode(uint8_t kind,uint16_t address,uint32_t now) {
  if (mode_.kind || !storageHealthy_ || !mesh_.healthy() || !mesh_.ready() || scheduler_.active() ||
      pump_.output() || pump_.testing() || commission_.active || mesh_.managementBusy() ||
      store_.removingAddress() || !resultsSettled() || flowCalibrationLocked()) return false;
  if (kind!=3 && store_.maintenance().locked) return false;
  if (kind==3 && !store_.maintenance().locked) return false;
  if (modeSequence_==UINT32_MAX) return false;
  mode_={}; mode_.kind=kind; mode_.address=address;
  mode_.token=kind==3 ? JobKey{store_.maintenance().epoch,store_.maintenance().generation} :
    JobKey{store_.state().epoch,++modeSequence_};
  mode_.expected=registeredMask(); mode_.began=now; mode_.sentAt=now-100;
  if (!mode_.expected) { mode_={}; return false; }
  return true;
}
void Application::modeTick(uint32_t now) {
  if (!mode_.kind) return;
  if (elapsed(now,mode_.began,12000) || !storageHealthy_ || !mesh_.healthy()) {
    const auto pending=mode_; mode_={};
    if (pending.kind==1) response(pending.ingress,pending.request,Reply::NotReady,{});
    gateway_.publish("MODE_PROOF_FAILED outputs_locked"); maintenanceState(); return;
  }
  if (mode_.received==mode_.expected) {
    const auto pending=mode_; mode_={};
    if (pending.kind==1) {
      modeApproved_=true; request(pending.request,pending.ingress,now); modeApproved_=false;
    } else if (pending.kind==2) {
      const int slot=scheduler_.find(pending.address);
      if (slot<0 || modeSequence_==UINT32_MAX) { gateway_.publish("MAINT NOT_READY"); return; }
      MaintenanceRecord record; record.locked=1; record.address=pending.address;
      record.epoch=store_.state().epoch; record.generation=++modeSequence_; record.boot=pending.boot[slot];
      if (!store_.writeMaintenance(record)) { storageHealthy_=false; gateway_.publish("STORAGE maintenance_locked"); return; }
      auto grant=packet(Op::MaintenanceGrant,record.address); Writer w{grant};
      w.key({record.epoch,record.generation}); w.u32(record.boot); w.u32(60000);
      mesh_.send(grant); // A lost grant leaves the durable pump lock in place; never auto-unlock.
      maintenanceState();
    } else {
      auto record=store_.maintenance(); record.locked=0;
      mesh_.retire(record.address,{record.epoch,record.generation},Op::MaintenanceGrant);
      if (!store_.writeMaintenance(record)) storageHealthy_=false;
      maintenanceState();
    }
    return;
  }
  if (!elapsed(now,mode_.sentAt,100)) return;
  for (size_t n=0;n<MaxNodes;++n) {
    const auto i=mode_.cursor++%MaxNodes; const uint32_t bit=uint32_t(1)<<i;
    if (!(mode_.expected & bit) || (mode_.received & bit)) continue;
    auto p=packet(mode_.kind==3 ? Op::MaintenanceRetire : Op::ModeGet,store_.state().nodes[i].address);
    Writer w{p}; w.key(mode_.token);
    if (mode_.kind==3) w.u32(store_.maintenance().boot);
    mesh_.send(p); mode_.sentAt=now; return;
  }
}
bool Application::resultsSettled() const {
  const auto &state = store_.state();
  if (!savedJob_ || state.lastJob.unknown) return false;
  if (!state.lastJob.key.valid()) return true;
  if (state.lastJob.phase != SchedulePhase::Complete && state.lastJob.phase != SchedulePhase::Failed) return false;
  for (size_t i=0;i<MaxNodes;++i) if (state.lastJob.selected & (uint32_t(1)<<i)) {
    const auto &s=state.lastResults[i];
    if (!terminalResult(s) || s.key!=state.lastJob.key || s.reason==Reason::Reboot) return false;
  }
  return true;
}
bool Application::commitReport(uint32_t now) {
  if (!storageHealthy_) return false;
  auto next=store_.state(); const auto report=scheduler_.report();
  if (report.key!=next.lastJob.key) return false;
  bool changed=report.selected!=next.lastJob.selected || report.complete!=next.lastJob.complete ||
    report.failed!=next.lastJob.failed || report.unknown!=next.lastJob.unknown ||
    report.phase!=next.lastJob.phase || report.reason!=next.lastJob.reason;
  next.lastJob=report;
  bool all=report.unknown==0 && !scheduler_.active();
  for (size_t i=0;i<MaxNodes;++i) if (report.selected & (uint32_t(1)<<i)) {
    const auto &s=scheduler_.node(i).status;
    if (terminalResult(s) && s.key==report.key) {
      changed=changed || !sameResult(s,next.lastResults[i]); next.lastResults[i]=s;
    } else all=false;
    if (s.reason==Reason::Reboot) all=false;
  }
  if (changed && !store_.write(next)) {
    storageHealthy_=false; savedJob_=false;
    scheduler_.stop(Reason::Transport,now); pump_.trip(PumpFault::Transport,now);
    gateway_.publish("STORAGE result_pending outputs_locked"); return false;
  }
  savedJob_=all; return true;
}
bool Application::flowCalibrationLocked() const {
  const auto phase = store_.flowCalibration().phase;
  return phase == FlowCalibrationPhase::Starting || phase == FlowCalibrationPhase::Preout ||
    phase == FlowCalibrationPhase::MarkPending || phase == FlowCalibrationPhase::Measuring ||
    phase == FlowCalibrationPhase::Stopping || phase == FlowCalibrationPhase::Unknown;
}
void Application::response(uint16_t ingress, const ControlRequest &q, Reply result, JobKey key) {
  JobReport report; VolumeReport volume;
  if (key.valid() && key == scheduler_.report().key &&
      (scheduler_.active() || !savedJob_ || key != store_.state().lastJob.key)) {
    report = scheduler_.report(); volume = scheduler_.volume();
  } else if (key.valid() && key == store_.state().lastJob.key) {
    report = store_.state().lastJob;
    uint64_t ml = 0;
    for (size_t i = 0; i < MaxNodes; ++i) {
      const auto &s = store_.state().lastResults[i]; const uint32_t bit = uint32_t(1) << i;
      if ((report.selected & bit) && s.key == key && s.reason != Reason::Reboot) { ml += volumeMl(s.pulses, s.k); volume.measured |= bit; }
    }
    volume.ml = ml > UINT32_MAX ? UINT32_MAX : uint32_t(ml);
  } else if (key.valid() && result == Reply::Ok) result = Reply::Unknown;
  if (key==scheduler_.report().key && !savedJob_ &&
      (report.phase==SchedulePhase::Complete || report.phase==SchedulePhase::Failed)) {
    report.phase=SchedulePhase::Draining;
    report.unknown |= report.selected & ~(store_.state().lastJob.complete | store_.state().lastJob.failed);
  }
  auto p = packet(Op::Response, ingress); Writer w{p}; w.u64(q.client); w.u32(q.sequence); w.u8(uint8_t(result)); w.key(key);
  w.u8(uint8_t(report.phase)); w.u32(report.selected); w.u32(report.complete); w.u32(report.failed); w.u32(report.unknown);
  w.u32(volume.ml); w.u32(volume.measured); w.u8(uint8_t(report.reason));
  if (!ingress) publishResponse(p); else mesh_.send(p);
}
void Application::publishResponse(const Packet &p) {
  Reader r{p}; const uint64_t client = r.u64(); const uint32_t seq = r.u32(); const Reply code = Reply(r.u8());
  const JobKey key = r.key(); const auto phase = r.u8(); const uint32_t selected = r.u32(), complete = r.u32(), failed = r.u32(), unknown = r.u32();
  const uint32_t ml = r.u32(), measured = r.u32();
  const Reason reason = r.at < p.size ? Reason(r.u8()) : Reason::None;
  if (!r.done() || code > Reply::Unknown || phase > uint8_t(SchedulePhase::Failed) || reason > Reason::Protocol) return;
  snprintf(pendingResponse_[0], sizeof(pendingResponse_[0]),
    "R client=%llx seq=%lu reply=%s job=%lu:%lu phase=%u mask=%08lx done=%08lx failed=%08lx unknown=%08lx reason=%s",
    static_cast<unsigned long long>(client), static_cast<unsigned long>(seq), replyName(code),
    static_cast<unsigned long>(key.epoch), static_cast<unsigned long>(key.id), phase,
    static_cast<unsigned long>(selected), static_cast<unsigned long>(complete), static_cast<unsigned long>(failed), static_cast<unsigned long>(unknown), reasonName(reason));
  snprintf(pendingResponse_[1], sizeof(pendingResponse_[1]), "V job=%lu:%lu total_ml=%lu measured=%08lx",
    static_cast<unsigned long>(key.epoch), static_cast<unsigned long>(key.id),
    static_cast<unsigned long>(ml), static_cast<unsigned long>(measured));
  responseLinePending_[0] = responseLinePending_[1] = true;
  retryResponse();
}
void Application::retryResponse() {
  for (size_t i = 0; i < 2; ++i) if (responseLinePending_[i] && gateway_.publish(pendingResponse_[i]))
    responseLinePending_[i] = false;
}
void Application::publishFlow(uint16_t address, JobKey key, uint32_t pulses, uint32_t k,
                              Phase phase, Reason reason, uint32_t revision, bool terminal) {
  char text[192];
  snprintf(text, sizeof(text), "FLOW addr=%04x job=%lu:%lu pulses=%lu ml=%lu phase=%s revision=%lu terminal=%u measured=%u reason=%s",
    address, static_cast<unsigned long>(key.epoch), static_cast<unsigned long>(key.id),
    static_cast<unsigned long>(pulses), static_cast<unsigned long>(volumeMl(pulses, k)), phaseName(phase),
    static_cast<unsigned long>(revision), terminal, reason != Reason::Reboot, reasonName(reason));
  gateway_.publish(text);
}
void Application::replayFlows(uint32_t now) {
  if (!flowReplayKey_.valid() || !elapsed(now, flowReplayAt_, 50)) return;
  const JobReport report = flowReplayKey_ == scheduler_.report().key ? scheduler_.report() : store_.state().lastJob;
  while (flowReplayCursor_ < MaxNodes) {
    const size_t i = flowReplayCursor_;
    const uint32_t bit = uint32_t(1) << i;
    if (!(flowReplayMask_ & bit) || !(report.selected & bit)) { ++flowReplayCursor_; continue; }
    const auto &saved = store_.state().lastResults[i];
    const ValveStatus &s = saved; // Only durable terminal measurements are replayable.
    if (s.key != flowReplayKey_) { ++flowReplayCursor_; continue; }
    const auto address = store_.resultAddress(flowReplayKey_, i);
    if (!unicast(address)) { ++flowReplayCursor_; continue; }
    char text[192];
    snprintf(text, sizeof(text), "FLOW addr=%04x job=%lu:%lu pulses=%lu ml=%lu phase=%s revision=%lu terminal=%u measured=%u reason=%s",
      address, static_cast<unsigned long>(flowReplayKey_.epoch),
      static_cast<unsigned long>(flowReplayKey_.id), static_cast<unsigned long>(s.pulses),
      static_cast<unsigned long>(volumeMl(s.pulses, s.k)), phaseName(s.phase),
      static_cast<unsigned long>(s.revision), s.phase == Phase::Done || s.phase == Phase::Fault,
      s.reason != Reason::Reboot, reasonName(s.reason));
    if (gateway_.publish(text)) { ++flowReplayCursor_; flowReplayAt_ = now; }
    return;
  }
  flowReplayKey_ = {}; flowReplayMask_ = 0;
}
void Application::requeryUnknown(uint32_t now) {
  if (!resultQueryKey_.valid() || !elapsed(now, resultQueryAt_, 75)) return;
  const JobReport report = resultQueryKey_ == scheduler_.report().key ? scheduler_.report() : store_.state().lastJob;
  while (resultQueryCursor_ < MaxNodes) {
    const size_t i = resultQueryCursor_;
    const uint32_t bit = uint32_t(1) << i;
    if (!(resultQueryMask_ & bit) || !(report.unknown & bit) || !scheduler_.node(i).config.address) {
      ++resultQueryCursor_; continue;
    }
    auto query = packet(Op::ResultQuery, scheduler_.node(i).config.address);
    Writer writer{query}; writer.key(resultQueryKey_);
    if (mesh_.send(query)) { ++resultQueryCursor_; resultQueryAt_ = now; }
    return;
  }
  resultQueryKey_ = {}; resultQueryMask_ = 0;
}
void Application::request(ControlRequest q, uint16_t ingress, uint32_t now) {
  if (ingress && scheduler_.find(ingress) < 0) { response(ingress, q, Reply::Unauthorized, {}); return; }
  if (q.action == Action::Stop) {
    if (q.epoch != store_.state().epoch || q.stopKey != scheduler_.report().key || !q.stopKey.valid()) {
      response(ingress, q, Reply::Stale, q.stopKey); return;
    }
    scheduler_.stop(Reason::Manual, now); response(ingress, q, Reply::Ok, q.stopKey); return;
  }
  const auto &persisted = store_.state(); int slot = -1, free = -1;
  for (size_t i = 0; i < 16; ++i) {
    if (persisted.clients[i].client == q.client) slot = int(i);
    if (!persisted.clients[i].client && free < 0) free = int(i);
  }
  if (q.action == Action::Query) {
    if (slot < 0 || persisted.clients[slot].sequence != q.sequence) response(ingress, q, Reply::Unknown, {});
    else {
      const JobKey key = persisted.clients[slot].key;
      const JobReport report = key == scheduler_.report().key ? scheduler_.report() : persisted.lastJob;
      flowReplayKey_ = key; flowReplayMask_ = report.selected; flowReplayCursor_ = 0; flowReplayAt_ = now;
      resultQueryKey_ = key; resultQueryMask_ = report.unknown; resultQueryCursor_ = 0; resultQueryAt_ = now;
      resultQueryOwner_ = q; resultQueryIngress_ = ingress; resultQueryOwnerPending_ = report.unknown != 0;
      response(ingress, q, Reply::Ok, key);
    }
    return;
  }
  if (!mesh_.ready() || mesh_.address() != store_.state().master || !storageHealthy_) { response(ingress, q, Reply::NotReady, {}); return; }
  if (!mesh_.healthy()) { response(ingress, q, Reply::NotReady, {}); return; }
  if (mesh_.managementBusy()) { response(ingress, q, Reply::Busy, {}); return; }
  if (slot >= 0) {
    const auto &old = persisted.clients[slot];
    const Reply replay = replayDecision(old, q, persisted.clientAmounts[slot]);
    if (replay != Reply::Pending) { response(ingress, q, replay, old.key); return; }
  } else if (free < 0) { response(ingress, q, Reply::Capacity, {}); return; }
  if (nodeCount(registeredMask())>ProductMaxNodes) { response(ingress,q,Reply::Capacity,{}); return; }
  if (q.individual()) { response(ingress,q,Reply::Invalid,{}); return; }
  if (store_.maintenance().locked) { response(ingress,q,Reply::Busy,{}); return; }
  if (mode_.kind) {
    if (mode_.kind==1 && mode_.request.client==q.client && mode_.request.sequence==q.sequence &&
        mode_.request.mask==q.mask && mode_.request.ml==q.ml && mode_.request.maxMs==q.maxMs) return;
    response(ingress,q,Reply::Busy,{}); return;
  }
  if (!resultsSettled()) { response(ingress,q,Reply::Pending,{}); return; }
  if (q.epoch != persisted.epoch || persisted.jobCounter == UINT32_MAX) { response(ingress, q, Reply::Stale, {}); return; }
  const auto allowed = scheduler_.check(q.mask, q.ml, q.maxMs, now, q.individual() ? q.amounts : nullptr);
  if (allowed != Reply::Ok) { response(ingress, q, allowed, {}); return; }
  if (!modeApproved_) {
    if (!beginMode(1,0,now)) { response(ingress,q,Reply::NotReady,{}); return; }
    mode_.request=q; mode_.ingress=ingress; return;
  }
  // Durable acceptance precedes PREPARE or any physical output. No eviction of client high-water marks.
  auto next = persisted; const JobKey key{next.epoch, ++next.jobCounter};
  auto &record = next.clients[slot >= 0 ? slot : free];
  record.client = q.client; record.sequence = q.sequence; record.mask = q.mask;
  record.ml = q.ml; record.maxMs = q.maxMs; record.key = key;
  memcpy(next.clientAmounts[slot >= 0 ? slot : free], q.amounts, MaxNodes);
  next.lastJob = {}; next.lastJob.key = key; next.lastJob.selected = q.mask;
  next.lastJob.phase = SchedulePhase::Preparing;
  for (auto &result : next.lastResults) result = {};
  if (!store_.write(next)) { storageHealthy_ = false; response(ingress, q, Reply::Storage, {}); return; }
  scheduler_.begin(key, q.mask, q.ml, q.maxMs, now, q.individual() ? q.amounts : nullptr); savedJob_ = false;
  owner_ = q; ownerIngress_ = ingress; response(ingress, q, Reply::Ok, key);
}
void Application::incoming(const Packet &p, uint32_t now) {
  if (p.peer == store_.removingAddress()) return;
  if (p.op==Op::NodeState) {
    Reader r{p}; const auto token=r.key(); const uint32_t boot=r.u32(); const auto flags=r.u8(); r.u8();
    const int slot=scheduler_.find(p.peer);
    if (!controlFormat(p) || !r.done() || !boot || !mode_.kind || token!=mode_.token || slot<0) return;
    const uint32_t bit=uint32_t(1)<<slot;
    if ((mode_.expected & bit) && flags==AutoSafeFlags) { mode_.received|=bit; mode_.boot[slot]=boot; }
    else mode_.received&=~bit;
    return;
  }
  Reader r{p};
  if (scheduler_.find(p.peer) < 0) {
    if (p.op != Op::Hello || !mesh_.ownsSlave(p.peer) || !idle() || !storageHealthy_) return;
    if (nodeCount(registeredMask())>=ProductMaxNodes) return;
    Reader hello{p}; const uint32_t k=hello.u32(), version=hello.u32(); const auto configured=hello.u8();
    const uint32_t buildId=hello.u32();
    if (!hello.done() || !validK(k) || !version || configured>1 || !buildId) return;
    int slot=-1; for (size_t i=0;i<MaxNodes;++i) if (!store_.state().nodes[i].address) { slot=int(i); break; }
    if (slot<0) return;
    auto next=store_.state(); next.nodes[slot]={p.peer,k,version};
    if (!store_.write(next)) { storageHealthy_=false; return; }
    if (scheduler_.registerNode(slot,{p.peer,k,version})!=Reply::Ok) return;
  }
  if (p.op == Op::Hello) {
    const uint32_t k = r.u32(), version = r.u32(); const bool commissioned = r.u8() == 1;
    const uint32_t buildId = r.u32();
    if (!r.done() || !buildId) return;
    const auto &previous = scheduler_.node(scheduler_.find(p.peer));
    const bool recover = !previous.seen || elapsed(now,previous.seenAt,15000) || store_.state().lastJob.unknown;
    scheduler_.hello(p.peer, k, version, commissioned, now, buildId);
    if (validK(k) && version) mesh_.slaveHello(p.peer);
    auto reply = packet(Op::HelloReply, p.peer); Writer w{reply}; w.u32(store_.state().epoch);
    w.u8(uint8_t(pump_.fault())); w.u8(uint8_t(scheduler_.report().phase)); mesh_.send(reply);
    // Read saved results after a reboot without reissuing PREPARE/START.
    if (recover && !scheduler_.active()) { auto query = packet(Op::ResultQuery, p.peer); Writer qw{query}; qw.key({}); mesh_.send(query); }
  } else if (p.op == Op::Capabilities) {
    const uint32_t nonce=r.u32(); const uint16_t revision=r.u16(), features=r.u16(); const uint32_t build=r.u32();
    if (r.done()) {
      const bool applied=scheduler_.capabilities(p.peer,nonce,revision,features,build,now);
      ESP_LOGD("cap_rx", "t=%lu peer=%04x nonce=%lu protocol=%u features=%04x build=%08lx applied=%u",
        (unsigned long)now,p.peer,(unsigned long)nonce,revision,features,(unsigned long)build,applied);
    }
  } else if (p.op == Op::LeaseAck) {
    if (!controlFormat(p)) return;
    const uint32_t epoch=r.u32(), sequence=r.u32();
    const Reply reply=p.size==8 ? Reply::Ok : Reply(r.u8());
    bool matched=false; uint32_t issued=0;
    const auto &node=scheduler_.node(scheduler_.find(p.peer));
    for (const auto &v:node.leases) if (v.sequence==sequence) { matched=true; issued=v.issuedAt; }
    const bool applied=scheduler_.compactLeaseAck(p.peer,epoch,sequence,reply,now);
    ESP_LOGD("lease_rx", "t=%lu peer=%04x epoch=%lu seq=%lu result=%s matched=%u issued=%lu age=%lu applied=%u",
      (unsigned long)now,p.peer,(unsigned long)epoch,(unsigned long)sequence,replyName(reply),matched,
      (unsigned long)issued,(unsigned long)(matched?now-issued:0),applied);
  } else if (p.op == Op::CommandAck || p.op==Op::PrepareOK || p.op==Op::StartOK) {
    if (!controlFormat(p)) return;
    const auto key=r.key();
    const Op command=p.op==Op::PrepareOK ? Op::Prepare : p.op==Op::StartOK ? Op::Start : Op(r.u8());
    const Reply reply=p.op==Op::CommandAck ? Reply(r.u8()) : Reply::Ok;
    const bool applied=scheduler_.commandReply(p.peer,key,command,reply,now);
    ESP_LOGD("command_rx","t=%lu peer=%04x job=%lu:%lu command=%02x reply=%s applied=%u guard=%s",
      (unsigned long)now,p.peer,(unsigned long)key.epoch,(unsigned long)key.id,uint8_t(command),replyName(reply),applied,scheduler_.guard());
  } else if (p.op==Op::Progress) {
    if (!controlFormat(p)) return;
    const auto key=r.key(); const uint32_t pulses=r.u32(), runtime=r.u32();
    if (!r.done() || !scheduler_.progress(p.peer,key,pulses,runtime,now)) return;
    const auto &live=scheduler_.node(scheduler_.find(p.peer)).live;
    char text[192];
    snprintf(text,sizeof(text),"LIVE addr=%04x job=%lu:%lu pulses=%lu ml=%lu runtime_ms=%lu rate_ml_min=%lu",
      p.peer,(unsigned long)key.epoch,(unsigned long)key.id,(unsigned long)live.pulses,
      (unsigned long)live.milliliters,(unsigned long)live.runtimeMs,(unsigned long)live.millilitersPerMinute);
    gateway_.publishLive(text);
    const auto &flow = store_.flowCalibration();
    if (flow.key == key && (flow.phase == FlowCalibrationPhase::Preout ||
                            flow.phase == FlowCalibrationPhase::Measuring)) flowCalibrationStatus();
  } else if (p.op == Op::MeasureMarkAck) {
    if (!controlFormat(p)) return;
    const JobKey key = r.key(); const uint32_t pulses = r.u32(); const Reply reply = Reply(r.u8());
    if (!r.done()) return;
    const auto &flow = store_.flowCalibration();
    if (flow.key != key || flow.phase != FlowCalibrationPhase::MarkPending) return;
    const bool applied = scheduler_.calibrationMarkAck(p.peer, key, pulses, reply, now);
    if (!applied) return;
    auto next = flow;
    if (reply == Reply::Ok) {
      next.startPulses = pulses; next.marked = true;
      next.phase = FlowCalibrationPhase::Measuring;
    } else {
      next.reason = Reason::Transport; next.phase = FlowCalibrationPhase::Invalid;
    }
    if (!store_.writeFlowCalibration(next)) {
      storageHealthy_ = false; scheduler_.stop(Reason::Transport, now);
      flowCalibrationStatus(Reply::Storage); return;
    }
    flowCalibrationStatus(reply);
  } else if (p.op==Op::TargetClosed) {
    const auto key=r.key(); if (!r.done()) return;
    const bool applied=scheduler_.targetClosed(p.peer,key,now);
    if (applied) { auto ack=packet(Op::TargetClosedAck,p.peer); Writer w{ack}; w.key(key); mesh_.send(ack); }
    ESP_LOGD("closed_rx","t=%lu peer=%04x job=%lu:%lu applied=%u pump=%u",
      (unsigned long)now,p.peer,(unsigned long)key.epoch,(unsigned long)key.id,applied,pump_.output());
  } else if (p.op == Op::CommissionReply) {
    const uint32_t request = r.u32(); const uint8_t rawReply = r.u8(), configured = r.u8();
    const uint32_t k = r.u32(), version = r.u32();
    if (!r.done() || !commission_.active || p.peer != commission_.address || request != commission_.request ||
        rawReply > uint8_t(Reply::Unknown) || configured > 1) return;
    Reply result = Reply(rawReply);
    if (result == Reply::Ok && (!configured || !validK(k) || !version)) result = Reply::Invalid;
    if (configured && validK(k) && version) scheduler_.hello(p.peer, k, version, true, now);
    if (result == Reply::Ok) mesh_.slaveHello(p.peer);
    commissionResult(request, commission_.slot, p.peer, result, configured == 1);
    commission_.active = false;
  } else if (p.op == Op::Result) {
    ValveStatus s; if (!decodeResult(p, s) || !terminalResult(s)) return;
    const int identitySlot=scheduler_.find(p.peer);
    if (identitySlot<0 || store_.resultAddress(s.key,size_t(identitySlot))!=p.peer ||
        s.key!=store_.state().lastJob.key || !(store_.state().lastJob.selected & (uint32_t(1)<<identitySlot))) return;
    if (!scheduler_.result(p.peer, s, now)) return;
    const int slot = scheduler_.find(p.peer); const uint32_t bit = uint32_t(1) << slot;
    const auto &current = scheduler_.node(slot).status;
    const auto &report = scheduler_.report();
    // result() can return true even when status() rejected a stale revision.
    if (!sameResult(current,s) || report.key!=s.key || !(report.selected & bit)) return;
    const auto &flow = store_.flowCalibration();
    if (flow.key == s.key && flow.phase != FlowCalibrationPhase::Idle &&
        (s.phase == Phase::Done || s.phase == Phase::Fault)) {
      auto next = flow; next.closePulses = s.closePulses; next.finalPulses = s.pulses; next.reason = s.reason;
      if (s.reason == Reason::Reboot) next.phase = FlowCalibrationPhase::Unknown;
      else if (flow.marked && s.reason == Reason::Manual && s.quiet && !s.truncated &&
               s.closePulses > flow.startPulses && s.pulses >= s.closePulses)
        next.phase = FlowCalibrationPhase::Complete;
      else next.phase = FlowCalibrationPhase::Invalid;
      next.operation = FlowCalibrationOperation::Stop;
      if (!store_.writeFlowCalibration(next)) {
        storageHealthy_ = false; pump_.trip(PumpFault::Transport, now);
      }
      flowCalibrationStatus(next.phase == FlowCalibrationPhase::Complete ? Reply::Ok : Reply::NotReady);
    }
    if (!commitReport(now)) return;
    if (!sameResult(store_.state().lastResults[slot],s)) return;
    publishFlow(p.peer,s.key,s.pulses,s.k,s.phase,s.reason,s.revision,true);
    if (resultQueryOwnerPending_ && resultQueryKey_==s.key) {
      response(resultQueryIngress_,resultQueryOwner_,Reply::Ok,s.key);
      if (resultsSettled()) resultQueryOwnerPending_=false;
    }
    auto ack = packet(Op::ResultAck, p.peer); Writer w{ack}; w.key(s.key); w.u32(s.revision); mesh_.send(ack);
  } else if (p.op == Op::Request) {
    ControlRequest q; if (decodeRequest(p, q)) request(q, p.peer, now);
  }
}
void Application::localState(uint32_t now) {
  char text[192];
  snprintf(text, sizeof(text), "LOCAL pump_command=%u pump_phase=%u fault=%s test_supported=1 test_active=%u test_ms=%lu test_client=%llx test_seq=%lu pump_ready=%u",
    pump_.output(), unsigned(pump_.phase()), pumpFaultName(pump_.fault()), pump_.testing(),
    static_cast<unsigned long>(pump_.testRemaining(now)), static_cast<unsigned long long>(pumpTestOwner_.client),
    static_cast<unsigned long>(pumpTestOwner_.sequence), pump_.testReady(now) && storageHealthy_ &&
    !scheduler_.active() && !store_.state().lastJob.unknown);
  gateway_.publish(text);
}
void Application::meshState() {
  publishMeshDiagnostics(mesh_.diagnostics(),mesh_.healthy(),[&](const char *line) { gateway_.publish(line); });
}
void Application::pumpPwmState(Reply result) {
  char text[192];
  snprintf(text, sizeof(text), "PUMP_PWM epoch=%lu rev=%lu level=%u duty=%u hz=%lu output=%u reply=%s test_manual=1 dry_max_ms=3000 dry_active=%u",
    static_cast<unsigned long>(store_.state().epoch), static_cast<unsigned long>(pwmRevision_),
    pump_.speed(), pump_.duty(), static_cast<unsigned long>(board::PumpPwmHz), pump_.output(), replyName(result), pump_.dryTesting());
  gateway_.publish(text);
}
void Application::pumpTestReply(const ControlRequest &q, Reply result, uint32_t now) {
  const char *state = "UNKNOWN";
  for (const auto &record : store_.state().clients) {
    if (record.client != q.client) continue;
    if (record.sequence > q.sequence || (record.sequence == q.sequence &&
        record.key.epoch == q.epoch && !record.mask && record.ml <= 1)) state = "STOPPED";
  }
  if (pump_.testing() && pumpTestOwner_.client == q.client && pumpTestOwner_.sequence == q.sequence) state = "RUNNING";
  char text[160];
  snprintf(text, sizeof(text), "PUMP_TEST client=%llx seq=%lu epoch=%lu reply=%s state=%s op=%s",
    static_cast<unsigned long long>(q.client), static_cast<unsigned long>(q.sequence),
    static_cast<unsigned long>(store_.state().epoch), replyName(result), state,
    q.action == Action::Start ? "START" : q.action == Action::Stop ? "STOP" : "QUERY");
  testOutputPublished_ = pump_.testing(); testPublishedAt_ = now;
  gateway_.publish(text); localState(now); pumpPwmState();
}
void Application::command(CommandLine &line, uint32_t now) {
  if (!gateway_.authorized(line)) return;
  char *tokens[12] = {}; size_t count = 0; char *save = nullptr;
  for (char *p = strtok_r(line.text, " \t", &save); p; p = strtok_r(nullptr, " \t", &save)) {
    if (count == 12) { gateway_.publish("INVALID too_many_fields"); return; } tokens[count++] = p;
  }
  if (!count) return;
  auto number = [&](size_t index, uint64_t &v, unsigned base = 10) { return index < count && unsignedNumber(tokens[index], v, base); };
  const char *op = tokens[0];
  if (!strcmp(op,"master-identity") || !strcmp(op,"master-identity-init") || !strcmp(op,"master-identity-prove")) {
    if (line.bluetooth) { gateway_.publish("UNAUTHORIZED identity_installation_requires_USB"); return; }
    auto &identity=store_.masterIdentity();
    if (!strcmp(op,"master-identity-init")) {
      if (count!=2 || strcmp(tokens[1],"CONFIRM")) { gateway_.publish("INVALID identity_confirmation"); return; }
      if (!idle() || mesh_.managementBusy()) { gateway_.publish("BUSY identity_requires_idle"); return; }
      if (identity.state()==recovery::JournalState::Ready) { gateway_.publish("CONFLICT identity_already_initialized"); return; }
      if (!identity.initialize()) { storageHealthy_=false; gateway_.publish("STORAGE identity_preserved_outputs_locked"); return; }
    } else if ((!strcmp(op,"master-identity") && count!=1) || (!strcmp(op,"master-identity-prove") && count!=2)) {
      gateway_.publish("INVALID identity_fields"); return;
    }
    const auto *id=identity.get();
    if (!id) { gateway_.publish(identity.state()==recovery::JournalState::Empty?
      "MASTER_ID initialized=0 recovery_enabled=0":"STORAGE identity_unavailable"); return; }
    auto hex=[](const uint8_t *bytes,size_t size,char *out) {
      constexpr char digits[]="0123456789abcdef";
      for(size_t i=0;i<size;++i) {out[2*i]=digits[bytes[i]>>4];out[2*i+1]=digits[bytes[i]&15];} out[2*size]=0;
    };
    if (!strcmp(op,"master-identity-prove")) {
      if (!idle() || mesh_.managementBusy()) {gateway_.publish("BUSY identity_requires_idle");return;}
      recovery::Proof challenge{};recovery::Signature proof{};
      if (strlen(tokens[1])!=64) {gateway_.publish("INVALID identity_challenge");return;}
      for(size_t i=0;i<challenge.size();++i) {
        auto nibble=[](char c)->int {return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;};
        const int high=nibble(tokens[1][2*i]),low=nibble(tokens[1][2*i+1]);
        if(high<0 || low<0) {gateway_.publish("INVALID identity_challenge");return;}
        challenge[i]=uint8_t((high<<4)|low);
      }
      if (!identity.prove(challenge,proof)) {gateway_.publish("INVALID identity_challenge_or_crypto");return;}
      char r[65],s[65],message[192];hex(proof.data(),32,r);hex(proof.data()+32,32,s);
      snprintf(message,sizeof(message),"MASTER_PROOF nonce=%s r=%s",tokens[1],r);gateway_.publish(message);
      snprintf(message,sizeof(message),"MASTER_PROOF nonce=%s s=%s",tokens[1],s);gateway_.publish(message);
      return;
    }
    char master[33],network[33],device[33],pubA[67],pubB[65],message[192];
    hex(id->master.data(),16,master);hex(id->network.data(),16,network);hex(id->device.data(),16,device);
    hex(id->publicKey.data(),33,pubA);hex(id->publicKey.data()+33,32,pubB);
    snprintf(message,sizeof(message),"MASTER_ID schema=1 initialized=1 epoch=%lu master=%s network=%s uuid=%s recovery_enabled=0",
      (unsigned long)id->epoch,master,network,device);gateway_.publish(message);
    snprintf(message,sizeof(message),"MASTER_KEY master=%s part=1 value=%s",master,pubA);gateway_.publish(message);
    snprintf(message,sizeof(message),"MASTER_KEY master=%s part=2 value=%s",master,pubB);gateway_.publish(message);
    return;
  }
  if (!strcmp(op,"maintenance-status") && count==1) { maintenanceState(); return; }
  if (!strcmp(op,"maintenance-enter") || !strcmp(op,"maintenance-exit")) {
    if (line.bluetooth) { gateway_.publish("UNAUTHORIZED maintenance_requires_USB"); return; }
    const bool enter=!strcmp(op,"maintenance-enter"); uint64_t address=0;
    if ((enter && (count!=3 || !number(1,address,16) || address>0x7fff ||
        scheduler_.find(uint16_t(address))<0 || strcmp(tokens[2],"CONFIRM"))) ||
        (!enter && (count!=2 || strcmp(tokens[1],"CONFIRM")))) { gateway_.publish("INVALID maintenance_fields"); return; }
    if (!beginMode(enter?2:3,uint16_t(address),now)) gateway_.publish("BUSY maintenance");
    else maintenanceState();
    return;
  }
  // Basic release keeps recovery/query/stop paths for old sessions, but admits
  // only the shared-volume task through the all-branch mode proof.
  if (!strcmp(op,"flow-cal-start") || !strcmp(op,"pump-test") || !strcmp(op,"pump-dry-test") ||
      !strcmp(op,"start-each") || (!strcmp(op,"pump-speed") && (scheduler_.active() || store_.maintenance().locked || mode_.kind))) {
    gateway_.publish("UNSUPPORTED basic_release"); return;
  }
  if (!strcmp(op, "hello") && count == 1) {
    char text[192]; snprintf(text, sizeof(text), "HELLO role=%s addr=%04x master=%04x epoch=%lu mesh=%u commissioned=%u pump_fault=%s individual=0 build=%08lx tx_healthy=%u calibration=0 flow_calibration=0",
      "PUMP_MASTER", mesh_.address(), store_.state().master,
      static_cast<unsigned long>(store_.state().epoch), mesh_.ready(),
      board::pumpConfig().valid(), pumpFaultName(pump_.fault()),(unsigned long)PUMP_BUILD_ID,mesh_.healthy()); gateway_.publish(text);
    snprintf(text,sizeof(text),"CAPS epoch=%lu max_nodes=%u max_concurrent=%u basic=1 maintenance=1",
      (unsigned long)store_.state().epoch,unsigned(ProductMaxNodes),unsigned(board::MaxConcurrent)); gateway_.publish(text);
    maintenanceState(); return;
  }
  if (!strcmp(op, "local") && count == 1) {
    localState(now); pumpPwmState(); meshState(); return;
  }
  if (!strcmp(op, "mesh-diag") && count == 1) { meshState(); mesh_.logDiscoveryDiagnostics(); return; }
  if (!strcmp(op, "recovery-resources") && count == 1) {
    if (line.bluetooth) { gateway_.publish("UNAUTHORIZED diagnostics_requires_USB"); return; }
    recovery::publishRecoveryResources([&](const char *text) { gateway_.publish(text); });
    return;
  }
#if defined(PUMP_DISCOVERY_TRACE)
  if (!strcmp(op, "discovery-phone-adv")) {
    if (line.bluetooth) { gateway_.publish("UNAUTHORIZED maintenance_requires_USB"); return; }
    if (count != 2 || (strcmp(tokens[1], "on") && strcmp(tokens[1], "off"))) {
      gateway_.publish("INVALID discovery_phone_adv_fields"); return;
    }
    if (!idle() || pump_.testing() || commission_.active || mesh_.managementBusy()) {
      gateway_.publish("BUSY discovery_phone_adv"); return;
    }
    gateway_.publish(gateway_.setDiagnosticAdvertising(!strcmp(tokens[1], "on"))
      ? "OK discovery_phone_adv_requested" : "BUSY discovery_phone_adv_failed");
    return;
  }
#endif
  if (!strcmp(op, "flow-cal-start")) {
    uint64_t epoch=0, client=0, request=0, slot=0, address=0, standardMl=0, speed=0;
    if (count != 8 || !number(1,epoch) || !epoch || epoch>UINT32_MAX ||
        !number(2,client,16) || !client || client>UINT64_MAX || !number(3,request) || !request || request>UINT32_MAX ||
        !number(4,slot) || slot<1 || slot>MaxNodes || !number(5,address,16) || !unicast(address) || address>0x7fff ||
        !number(6,standardMl) || standardMl<50 || standardMl>2000 ||
        !number(7,speed) || speed<1 || speed>3) {
      gateway_.publish("INVALID flow_cal_fields"); return;
    }
    const auto &savedFlow = store_.flowCalibration();
    if (epoch != store_.state().epoch) { flowCalibrationStatus(Reply::Stale,client,uint32_t(request)); return; }
    if (savedFlow.client == client && savedFlow.request == request && savedFlow.key.valid()) {
      if (savedFlow.slot != slot || savedFlow.address != address || savedFlow.standardMl != standardMl || savedFlow.speed != speed)
        flowCalibrationStatus(Reply::Conflict,client,uint32_t(request));
      else flowCalibrationStatus(Reply::Ok);
      return;
    }
    if (!storageHealthy_) { flowCalibrationStatus(Reply::Storage,client,uint32_t(request)); return; }
    if (flowCalibrationLocked() || scheduler_.active() || pump_.output() || pump_.testing() ||
        commission_.active || store_.state().lastJob.unknown || mesh_.managementBusy()) {
      flowCalibrationStatus(Reply::Busy,client,uint32_t(request)); return;
    }
    if (!mesh_.ready() || !mesh_.healthy() || mesh_.address() != store_.state().master) {
      flowCalibrationStatus(Reply::NotReady,client,uint32_t(request)); return;
    }
    const size_t index = size_t(slot-1);
    const auto &cfg = store_.state().nodes[index];
    if (cfg.address != address || !mesh_.ownsSlave(cfg.address)) {
      flowCalibrationStatus(Reply::Stale,client,uint32_t(request)); return;
    }
    const Reply allowed = scheduler_.checkCalibration(index,uint32_t(standardMl),uint8_t(speed),60000,now);
    if (allowed != Reply::Ok) { flowCalibrationStatus(allowed,client,uint32_t(request)); return; }
    const auto &persisted = store_.state();
    if (persisted.jobCounter == UINT32_MAX) { flowCalibrationStatus(Reply::Stale,client,uint32_t(request)); return; }
    auto next = persisted;
    const JobKey key{next.epoch,++next.jobCounter};
    const uint32_t mask = uint32_t(1) << index;
    next.lastJob = {}; next.lastJob.key=key; next.lastJob.selected=mask; next.lastJob.phase=SchedulePhase::Preparing;
    for (auto &result : next.lastResults) result = {};
    if (!store_.write(next)) { storageHealthy_=false; flowCalibrationStatus(Reply::Storage,client,uint32_t(request)); return; }
    FlowCalibrationState session; session.client=client; session.request=uint32_t(request); session.key=key;
    session.standardMl=uint32_t(standardMl); session.maxMs=60000; session.address=uint16_t(address);
    session.slot=uint8_t(slot); session.speed=uint8_t(speed); session.phase=FlowCalibrationPhase::Starting;
    session.operation=FlowCalibrationOperation::Start; session.reason=Reason::None;
    if (!store_.writeFlowCalibration(session)) {
      storageHealthy_=false;
      auto failed = store_.state(); failed.lastJob.phase=SchedulePhase::Failed;
      failed.lastJob.reason=Reason::Transport; failed.lastJob.failed=mask;
      store_.write(failed);
      flowCalibrationStatus(Reply::Storage,client,uint32_t(request)); return;
    }
    scheduler_.begin(key,mask,uint32_t(standardMl),60000,now,nullptr,true); savedJob_=false;
    flowCalibrationStatus(Reply::Ok); return;
  }
  if (!strcmp(op, "flow-cal-mark") || !strcmp(op, "flow-cal-stop")) {
    const bool mark = !strcmp(op,"flow-cal-mark");
    uint64_t epoch=0,client=0,request=0,sequence=0,jobEpoch=0,jobId=0;
    if (count!=7 || !number(1,epoch) || !epoch || epoch>UINT32_MAX ||
        !number(2,client,16) || !client || !number(3,request) || !request || request>UINT32_MAX ||
        !number(4,sequence) || !sequence || sequence>UINT32_MAX || !number(5,jobEpoch) || jobEpoch>UINT32_MAX ||
        !number(6,jobId) || jobId>UINT32_MAX) {
      gateway_.publish("INVALID flow_cal_operation_fields"); return;
    }
    const bool noJobKey = jobEpoch == 0 && jobId == 0;
    if ((jobEpoch == 0) != (jobId == 0) || (mark && noJobKey)) {
      gateway_.publish("INVALID flow_cal_operation_fields"); return;
    }
    auto flow=store_.flowCalibration();
    if (epoch!=store_.state().epoch || flow.client!=client || flow.request!=request ||
        (!noJobKey && flow.key!=JobKey{uint32_t(jobEpoch),uint32_t(jobId)})) {
      flowCalibrationStatus(Reply::Stale,client,uint32_t(request)); return;
    }
    if (!mark) {
      pump_.stop(now);
      if (flow.operationSequence==sequence && flow.operation==FlowCalibrationOperation::Stop) {
        flowCalibrationStatus(Reply::Ok); return;
      }
      if (flow.operationSequence==UINT32_MAX || sequence<=flow.operationSequence ||
          !(flow.phase==FlowCalibrationPhase::Starting || flow.phase==FlowCalibrationPhase::Preout ||
            flow.phase==FlowCalibrationPhase::MarkPending ||
            flow.phase==FlowCalibrationPhase::Measuring || flow.phase==FlowCalibrationPhase::Unknown ||
            flow.phase==FlowCalibrationPhase::Stopping)) {
        flowCalibrationStatus(Reply::NotReady,client,uint32_t(request)); return;
      }
      flow.operationSequence=uint32_t(sequence); flow.operation=FlowCalibrationOperation::Stop;
      flow.phase=FlowCalibrationPhase::Stopping;
      const bool persisted = store_.writeFlowCalibration(flow);
      if (!persisted) storageHealthy_=false;
      if (scheduler_.active() && scheduler_.report().key==flow.key) scheduler_.stop(Reason::Manual,now);
      else {
        auto close=packet(Op::Close,flow.address); Writer w{close}; w.key(flow.key); w.u8(uint8_t(Reason::Manual));
        if (!mesh_.send(close)) {
          flow.phase=FlowCalibrationPhase::Unknown; flow.reason=Reason::Transport;
          if (!store_.writeFlowCalibration(flow)) storageHealthy_=false;
          flowCalibrationStatus(persisted ? Reply::NotReady : Reply::Storage); return;
        }
      }
      flowCalibrationStatus(persisted ? Reply::Ok : Reply::Storage); return;
    }
    if (epoch!=flow.key.epoch || !scheduler_.active() || scheduler_.report().key!=flow.key ||
        flow.operationSequence==UINT32_MAX || sequence!=flow.operationSequence+1 ||
        flow.phase!=FlowCalibrationPhase::Preout || flow.marked) {
      flowCalibrationStatus(Reply::NotReady,client,uint32_t(request)); return;
    }
    flow.operationSequence=uint32_t(sequence); flow.operation=FlowCalibrationOperation::Mark;
    flow.phase=FlowCalibrationPhase::MarkPending;
    if (!store_.writeFlowCalibration(flow)) {
      storageHealthy_=false; scheduler_.stop(Reason::Transport,now); flowCalibrationStatus(Reply::Storage); return;
    }
    const Reply result=scheduler_.markCalibration(now);
    if (result!=Reply::Ok) {
      flow.phase=FlowCalibrationPhase::Invalid; flow.reason=Reason::Transport;
      if (!store_.writeFlowCalibration(flow)) storageHealthy_=false;
      scheduler_.stop(Reason::Transport,now); flowCalibrationStatus(result); return;
    }
    flowCalibrationStatus(Reply::Pending); return;
  }
  if (!strcmp(op,"flow-cal-query")) {
    uint64_t epoch=0,client=0,request=0,jobEpoch=0,jobId=0;
    const bool hasKey=count==6;
    if ((count!=4 && !hasKey) || !number(1,epoch) || !epoch || epoch>UINT32_MAX ||
        !number(2,client,16) || !client || !number(3,request) || !request || request>UINT32_MAX ||
        (hasKey && (!number(4,jobEpoch) || jobEpoch>UINT32_MAX || !number(5,jobId) || jobId>UINT32_MAX ||
          ((jobEpoch==0)!=(jobId==0))))) {
      gateway_.publish("INVALID flow_cal_query_fields"); return;
    }
    if (epoch!=store_.state().epoch) { flowCalibrationStatus(Reply::Stale,client,uint32_t(request)); return; }
    const auto &flow=store_.flowCalibration();
    if (flow.client!=client || flow.request!=request || (hasKey && jobEpoch &&
        flow.key!=JobKey{uint32_t(jobEpoch),uint32_t(jobId)})) {
      flowCalibrationStatus(Reply::Unknown,client,uint32_t(request)); return;
    }
    flowCalibrationStatus(Reply::Ok); return;
  }
  if (!strcmp(op, "cal-get") || !strcmp(op, "cal-set")) {
    const bool write = !strcmp(op, "cal-set");
    uint64_t epoch=0, request=0, slot=0, address=0, revision=0, baseK=0, baseVersion=0, low=0, medium=0, high=0;
    if (count != (write ? 11U : 5U) || !number(1,epoch) || !epoch || epoch>UINT32_MAX ||
        !number(2,request) || !request || request>UINT32_MAX || !number(3,slot) || slot<1 || slot>MaxNodes ||
        !number(4,address,16) || !unicast(address) || address>0x7fff ||
        (write && (!number(5,revision) || revision>UINT32_MAX || !number(6,baseK) || !number(7,baseVersion) ||
          !baseVersion || baseVersion>UINT32_MAX || baseK>1000000 || !validK(baseK) ||
          !number(8,low) || !number(9,medium) || !number(10,high) || low>1000000 || medium>1000000 || high>1000000 ||
          !validK(low) || !validK(medium) || !validK(high)))) {
      gateway_.publish("INVALID calibration_fields"); return;
    }
    const size_t index = size_t(slot-1);
    auto reply = [&](Reply result) { calibrationState(uint32_t(request), index, result); };
    const auto &cfg = store_.state().nodes[index];
    if (epoch != store_.state().epoch || address != cfg.address) { reply(Reply::Stale); return; }
    if (!storageHealthy_) { reply(Reply::Storage); return; }
    if (!mesh_.ownsSlave(cfg.address)) { reply(Reply::Unauthorized); return; }
    if (!write) { reply(Reply::Ok); return; }
    if (baseK != cfg.k || baseVersion != cfg.version) { reply(Reply::Stale); return; }
    if (!idle() || pump_.testing() || commission_.active || store_.state().lastJob.unknown || mesh_.managementBusy()) {
      reply(Reply::Busy); return;
    }
    auto current = store_.calibration(index);
    if (revision != current.revision || revision == UINT32_MAX) { reply(Reply::Stale); return; }
    SpeedCalibration value; value.address=cfg.address; value.baseK=cfg.k; value.baseVersion=cfg.version;
    value.revision=uint32_t(revision)+1; value.low=uint32_t(low); value.medium=uint32_t(medium); value.high=uint32_t(high);
    const auto &node = scheduler_.node(index);
    if (node.config.address!=cfg.address || node.config.k!=cfg.k || node.config.version!=cfg.version ||
        node.status.valveCommand || (node.status.phase>=Phase::Prepared && node.status.phase<=Phase::Settling)) {
      reply(Reply::NotReady); return;
    }
    if (!store_.writeCalibration(index,value)) { storageHealthy_=false; reply(Reply::Storage); return; }
    if (scheduler_.setCalibration(index,value)!=Reply::Ok) { storageHealthy_=false; reply(Reply::Storage); return; }
    reply(Reply::Ok); return;
  }
  if (!strcmp(op, "pump-speed")) {
    uint64_t epoch = 0, revision = 0, level = 0;
    if (count != 4 || !number(1, epoch) || !epoch || epoch > UINT32_MAX ||
        !number(2, revision) || revision > UINT32_MAX || !number(3, level) || level < 1 || level > 3) {
      gateway_.publish("INVALID pump_speed_fields"); return;
    }
    Reply result = Reply::Ok;
    if (epoch != store_.state().epoch || revision != pwmRevision_ || pwmRevision_ == UINT32_MAX) result = Reply::Stale;
    else if (!storageHealthy_) result = Reply::Storage;
    else if (scheduler_.active() || flowCalibrationLocked() || store_.state().lastJob.unknown || mesh_.managementBusy()) result = Reply::Busy;
    else {
      result = pump_.setSpeed(uint8_t(level), now);
      if (result == Reply::Ok) ++pwmRevision_;
    }
    pumpPwmState(result); localState(now); return;
  }
  if (!strcmp(op, "pump-test") || !strcmp(op, "pump-dry-test") || !strcmp(op, "pump-test-stop") || !strcmp(op, "pump-test-query")) {
    const bool dryRun = !strcmp(op, "pump-dry-test");
    const bool stop = !strcmp(op, "pump-test-stop");
    const bool query = !strcmp(op, "pump-test-query");
    uint64_t client = 0, seq = 0, epoch = 0, duration = 0;
    if (count != (stop || query ? 4U : 5U) || !number(1, client, 16) || !client ||
        !number(2, seq) || !seq || seq > UINT32_MAX || !number(3, epoch) || !epoch || epoch > UINT32_MAX ||
        (!stop && !query && (!number(4, duration) || !duration || duration > (dryRun ? 3000U : 10000U) || duration > board::pumpConfig().maxRunMs))) {
      gateway_.publish("INVALID pump_test_fields"); return;
    }
    ControlRequest q; q.client = client; q.sequence = uint32_t(seq); q.epoch = uint32_t(epoch); q.maxMs = uint32_t(duration);
    q.action = stop ? Action::Stop : query ? Action::Query : Action::Start;
    q.ml = dryRun ? 1 : 0; // mask=0 reserves ml=1 for dry tests without changing the stored record format.
    if (q.epoch != store_.state().epoch) { pumpTestReply(q, Reply::Stale, now); return; }
    const auto &persisted = store_.state(); int slot = -1, free = -1;
    for (size_t i = 0; i < 16; ++i) {
      if (persisted.clients[i].client == q.client) slot = int(i);
      if (!persisted.clients[i].client && free < 0) free = int(i);
    }
    if (query) { pumpTestReply(q, Reply::Ok, now); return; }
    if (stop) {
      // Stop the named test immediately, even if persistent storage has failed.
      if (pumpTestOwner_.client == q.client && pumpTestOwner_.sequence == q.sequence && pump_.testing()) pump_.stop(now);
      if (slot >= 0 && persisted.clients[slot].sequence >= q.sequence) {
        const auto &record = persisted.clients[slot];
        pumpTestReply(q, record.sequence == q.sequence && (record.mask || record.ml > 1) ? Reply::Conflict : Reply::Ok, now); return;
      }
      if (free < 0 && slot < 0) { pumpTestReply(q, Reply::Capacity, now); return; }
      if (!storageHealthy_) { pumpTestReply(q, Reply::Storage, now); return; }
      // A durable tombstone cancels even a start still waiting in the BLE queue.
      auto next = persisted;
      next.clients[slot >= 0 ? slot : free] = {q.client, q.sequence, 0, 0, 0, {q.epoch, 0}};
      if (!store_.write(next)) { storageHealthy_ = false; pumpTestReply(q, Reply::Storage, now); return; }
      pumpTestReply(q, Reply::Ok, now); return;
    }
    if (slot >= 0) {
      const Reply replay = replayDecision(persisted.clients[slot], q);
      if (replay != Reply::Pending) { pumpTestReply(q, replay, now); return; }
    } else if (free < 0) { pumpTestReply(q, Reply::Capacity, now); return; }
    if (!storageHealthy_) { pumpTestReply(q, Reply::Storage, now); return; }
    if (scheduler_.active() || flowCalibrationLocked() || pump_.testing() || mesh_.managementBusy()) { pumpTestReply(q, Reply::Busy, now); return; }
    if (!pump_.testReady(now) || persisted.lastJob.unknown) { pumpTestReply(q, Reply::NotReady, now); return; }
    if (dryRun && pwmRevision_ == UINT32_MAX) { pumpTestReply(q, Reply::Stale, now); return; }
    if (persisted.jobCounter == UINT32_MAX) { pumpTestReply(q, Reply::Stale, now); return; }
    auto next = persisted; const JobKey key{next.epoch, ++next.jobCounter};
    auto &record = next.clients[slot >= 0 ? slot : free];
    record = {q.client, q.sequence, 0, q.ml, q.maxMs, key};
    // Share the durable client high-water marks with dosing; replays never restart a test.
    if (!store_.write(next)) { storageHealthy_ = false; pumpTestReply(q, Reply::Storage, now); return; }
    pumpTestOwner_ = q;
    const Reply result = pump_.startTest(key, 1, q.maxMs, now, dryRun);
    if (dryRun && result == Reply::Ok) ++pwmRevision_; // Forced low speed must have a new device revision.
    pumpTestReply(q, result, now); return;
  }
  if (!strcmp(op, "start") || !strcmp(op, "start-each") || !strcmp(op, "stop") || !strcmp(op, "query")) {
    ControlRequest q;
    if (!parseControl(tokens, count, store_.state().epoch, q)) {
      gateway_.publish("INVALID control_fields"); return;
    }
    if (q.action != Action::Stop && !elapsed(now, lastRequestAt_, 250)) { gateway_.publish("RATE_LIMIT"); return; }
    lastRequestAt_ = now; request(q, 0, now); return;
  }
  // Kept only for backwards-compatible maintenance tooling. New Master
  // firmware performs this cleanup automatically during Mesh init; the phone
  // never sends this command.
  if (!strcmp(op, "mesh-migrate")) {
    if (line.bluetooth) { gateway_.publish("UNAUTHORIZED maintenance_requires_USB"); return; }
    if (count != 2 || strcmp(tokens[1], "CONFIRM")) { gateway_.publish("INVALID mesh_migrate_confirmation"); return; }
    if (strcmp(mesh_.management().phase, "LEGACY_NETWORK")) { gateway_.publish("MESH_MIGRATION_NOT_REQUIRED"); return; }
    if (!idle() || store_.state().lastJob.unknown || !storageHealthy_ || mesh_.managementBusy()) {
      gateway_.publish("BUSY mesh_migration_requires_idle"); return;
    }
    if (!mesh_.migrateLegacyNetwork()) { gateway_.publish("MESH_MIGRATION_FAILED"); return; }
    gateway_.publish("OK mesh_migration_reset rebooting");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return;
  }
  if (!strcmp(op, "mesh-remove")) {
    uint64_t epoch=0, request=0, address=0;
    if (count != 5 || !number(1,epoch) || !epoch || epoch > UINT32_MAX ||
        !number(2,request) || !request || request > UINT32_MAX ||
        !number(3,address,16) || !unicast(address) || address > 0x7fff || strcmp(tokens[4], "CONFIRM")) {
      gateway_.publish("INVALID mesh_remove_fields"); return;
    }
    auto reject = [&](Reply reply) {
      char text[128]; snprintf(text,sizeof(text),"REMOVE epoch=%lu req=%lu addr=%04x reply=%s",
        (unsigned long)store_.state().epoch,(unsigned long)request,unsigned(address),replyName(reply)); gateway_.publish(text);
    };
    if (epoch != store_.state().epoch) { reject(Reply::Stale); return; }
    // Re-reading the same operation never issues a second reset/delete.
    const auto &m = mesh_.management();
    if (m.operation == request && m.target == address &&
        (!strncmp(m.phase,"REMOVE",6) || (store_.removingAddress() == address && removalStarted_))) {
      meshState(uint32_t(request)); return;
    }
    if (!storageHealthy_) { reject(Reply::Storage); return; }
    const bool retryRemoval = store_.removingAddress() == address &&
      !strcmp(m.phase,"REMOVE_PENDING");
    const bool safeRetry = retryRemoval && resultsSettled() && !store_.maintenance().locked &&
      !mode_.kind && !scheduler_.active() && !pump_.output() && !flowCalibrationLocked();
    if ((!idle() && !safeRetry) || pump_.testing() || commission_.active || store_.state().lastJob.unknown || mesh_.managementBusy()) {
      reject(Reply::Busy); return;
    }
    if (!mesh_.ownsSlave(uint16_t(address))) { reject(Reply::Unauthorized); return; }
    // A separate additive NVS journal makes both sides of local deletion
    // resumable without changing the deployed task/state ABI.
    if (!retryRemoval && !store_.beginRemoval(uint16_t(address))) { storageHealthy_=false; reject(Reply::Storage); return; }
    removalStarted_ = mesh_.removeSlave(uint16_t(address),uint32_t(request),now);
    if (!removalStarted_) { storageHealthy_=false; reject(Reply::Storage); return; }
    meshState(uint32_t(request)); return;
  }
  if (!strcmp(op, "mesh-reset")) {
    if (count != 2 || strcmp(tokens[1], "CONFIRM")) { gateway_.publish("INVALID mesh_reset_confirmation"); return; }
    if (!strcmp(mesh_.management().phase, "LEGACY_NETWORK")) { gateway_.publish("MESH_RESET_NOT_REQUIRED"); return; }
    if (!mesh_.management().capable) { gateway_.publish("MESH_RESET_NOT_READY"); return; }
    if (!idle() || store_.state().lastJob.unknown || !storageHealthy_ || mesh_.managementBusy()) {
      gateway_.publish("BUSY mesh_reset_requires_idle"); return;
    }
    if (!mesh_.resetProvisionedSlaves(now)) { gateway_.publish("MESH_RESET_FAILED"); return; }
    meshResetPending_ = true;
    gateway_.publish("OK mesh_reset_started");
    return;
  }
  if (!strcmp(op,"inventory") || !strcmp(op,"mesh-state") || !strcmp(op,"mesh-scan") || !strcmp(op,"mesh-join") || !strcmp(op,"mesh-retry")) {
    uint64_t epoch=0, request=0;
    const bool join=!strcmp(op,"mesh-join"), retry=!strcmp(op,"mesh-retry");
    if (count!=(join||retry?4U:3U) || !number(1,epoch) || epoch!=store_.state().epoch || !number(2,request) || !request || request>UINT32_MAX) { gateway_.publish("INVALID mesh_fields"); return; }
    if (!strcmp(op,"inventory")) { beginInventory(uint32_t(request),now); return; }
    if (!strcmp(op,"mesh-state")) { meshState(uint32_t(request)); return; }
    if (!idle() || store_.state().lastJob.unknown || !storageHealthy_) { gateway_.publish("BUSY mesh_control"); return; }
    bool accepted=false;
    if (!strcmp(op,"mesh-scan")) accepted=mesh_.scanSlaves(uint32_t(request),now);
    else if (join) {
      uint8_t id[16]; bool valid=strlen(tokens[3])==32;
      for (size_t i=0;valid && i<16;++i) { char byte[3]={tokens[3][2*i],tokens[3][2*i+1],0}; uint64_t v; if (!unsignedNumber(byte,v,16) || v>255) valid=false; else id[i]=uint8_t(v); }
      if (valid) accepted=mesh_.joinSlave(id,uint32_t(request),now);
    } else if (retry) { uint64_t address; if(number(3,address,16) && address>0 && address<=0x7fff) accepted=mesh_.retrySlave(uint16_t(address),uint32_t(request),now); }
    if (!accepted) gateway_.publish("BUSY mesh_not_accepted");
    meshState(uint32_t(request)); return;
  }
  if (!strcmp(op, "commission")) {
    uint64_t epoch=0, request=0, slot=0;
    if (count != 5 || !number(1,epoch) || !number(2,request) || !number(3,slot) ||
        strcmp(tokens[4], "CONFIRM") || !epoch || epoch > UINT32_MAX || !request || request > UINT32_MAX ||
        slot < 1 || slot > MaxNodes) {
      commissionResult(0, 0, 0, Reply::Invalid, false); return;
    }
    const uint32_t req = uint32_t(request); const uint8_t targetSlot = uint8_t(slot);
    uint16_t address = store_.state().nodes[slot - 1].address;
    auto fail = [&](Reply reply, bool configured=false) { commissionResult(req,targetSlot,address,reply,configured); };
    if (epoch != store_.state().epoch) { fail(Reply::Stale); return; }
    if (commission_.active) { fail(Reply::Busy); return; }
    if (!storageHealthy_) { fail(Reply::Storage); return; }
    if (!mesh_.ready() || !mesh_.ownsSlave(address)) { fail(Reply::Unauthorized); return; }
    if (!idle() || pump_.testing() || mesh_.managementBusy()) {
      fail(Reply::Busy); return;
    }
    const auto &node = scheduler_.node(slot - 1);
    if (scheduler_.find(address) != int(slot - 1) || !node.seen || elapsed(now,node.seenAt,15000)) { fail(Reply::Offline); return; }
    if (!validK(node.config.k) || !node.config.version || node.status.valveCommand) { fail(Reply::NotReady); return; }
    if (node.commissioned) { fail(Reply::Ok,true); return; }
    auto packetOut = packet(Op::Commission,address); Writer writer{packetOut}; writer.u32(req);
    if (!mesh_.send(packetOut)) { fail(Reply::Offline); return; }
    commission_={true,req,now,address,targetSlot,1};
    return;
  }
  if (!strcmp(op, "pump-reset")) {
    if (!line.bluetooth && count == 1) {
      if (!idle() || commission_.active || pump_.testing() || mesh_.managementBusy()) {
        gateway_.publish("BUSY pump_reset_requires_idle"); return;
      }
      gateway_.publish(replyName(pump_.reset(now))); return;
    }
  }
  if (line.bluetooth) { gateway_.publish("UNAUTHORIZED maintenance_requires_USB"); return; }
  if (!strcmp(op, "estop") && count == 1) {
    scheduler_.stop(Reason::Manual, now);
    gateway_.publish("LOCAL_STOP"); return;
  }
  if (!strcmp(op, "nodes") && count == 1) {
    for (size_t i = 0; i < MaxNodes; ++i) if (store_.state().nodes[i].address) {
      const auto &n = scheduler_.node(i); char text[160];
      snprintf(text, sizeof(text), "NODE slot=%u addr=%04x k=%lu version=%lu online=%u commissioned=%u phase=%s pulses=%lu age_ms=%lu",
        unsigned(i + 1), n.config.address, static_cast<unsigned long>(n.config.k), static_cast<unsigned long>(n.config.version),
        n.seen && !elapsed(now, n.seenAt, 15000), n.commissioned, phaseName(n.status.phase),
        static_cast<unsigned long>(n.telemetrySeen ? n.telemetryPulses : n.status.pulses), static_cast<unsigned long>(now - n.seenAt)); gateway_.publish(text);
    }
    return;
  }
  if (!idle()) { gateway_.publish("BUSY"); return; }
  if (!strcmp(op, "pump-reset") && count == 1) {
    gateway_.publish(replyName(pump_.reset(now))); return;
  }
  if (!strcmp(op, "node") && count == 5) {
    uint64_t slot, address, k, ver;
    if (!number(1, slot) || !number(2, address, 16) || !number(3, k) || !number(4, ver) ||
        slot < 1 || slot > MaxNodes || address > 0x7FFF || !unicast(address) || address == store_.state().master ||
        k > 1000000 || !validK(k) || !ver || ver > UINT32_MAX) { gateway_.publish("INVALID node"); return; }
    const int old = scheduler_.find(address);
    if (!store_.state().nodes[slot-1].address && nodeCount(registeredMask())>=ProductMaxNodes) { gateway_.publish("CAPACITY node"); return; }
    if (store_.state().nodes[slot-1].address && store_.state().nodes[slot-1].address!=address) { gateway_.publish("CONFLICT remove_node_first"); return; }
    if (old >= 0 && size_t(old) != slot - 1) { gateway_.publish("CONFLICT node_address"); return; }
    auto next = store_.state(); next.nodes[slot - 1] = {uint16_t(address), uint32_t(k), uint32_t(ver)};
    if (!store_.write(next)) { storageHealthy_ = false; gateway_.publish("STORAGE"); return; }
    const auto registered = scheduler_.registerNode(slot - 1, {uint16_t(address), uint32_t(k), uint32_t(ver)});
    if (registered == Reply::Ok) {
      const auto calibration = store_.calibration(slot - 1);
      if (calibration.valid()) scheduler_.setCalibration(slot - 1, calibration);
    }
    gateway_.publish(replyName(registered)); return;
  }
  if (!strcmp(op, "master") && count == 2) {
    uint64_t address;
    if (!number(1, address, 16) || address > 0x7FFF || !unicast(address) || scheduler_.find(address) >= 0) { gateway_.publish("INVALID master"); return; }
    auto next = store_.state(); next.master = address;
    if (!store_.write(next)) { storageHealthy_ = false; gateway_.publish("STORAGE"); return; }
    gateway_.publish("OK master"); return;
  }
  if (!strcmp(op, "pin") && count == 2) {
    uint64_t pin;
    if (!number(1, pin) || pin < 100000 || pin > 999999) { gateway_.publish("INVALID six_digit_pin"); return; }
    auto next = store_.state(); next.gatewayPin = pin;
    if (!store_.write(next)) { storageHealthy_ = false; gateway_.publish("STORAGE"); return; }
    gateway_.publish("OK pin_saved restart_required"); return;
  }
  gateway_.publish("HELP hello | start CLIENThex SEQ EPOCH MASKhex ML MAX_MS | stop CLIENThex SEQ EPOCH JOB_EPOCH JOB_ID | query CLIENThex SEQ | mesh-reset CONFIRM");
}
void Application::calibrationState(uint32_t request, size_t slot, Reply reply) {
  if (slot >= MaxNodes) return;
  const auto &cfg = store_.state().nodes[slot]; auto value = store_.calibration(slot);
  if (!value.valid()) value.low = value.medium = value.high = cfg.k;
  char line[192];
  snprintf(line,sizeof(line),"CAL epoch=%lu req=%lu slot=%u addr=%04x rev=%lu bk=%lu bv=%lu low=%lu mid=%lu high=%lu reply=%s",
    (unsigned long)store_.state().epoch,(unsigned long)request,unsigned(slot+1),cfg.address,
    (unsigned long)value.revision,(unsigned long)cfg.k,(unsigned long)cfg.version,
    (unsigned long)value.low,(unsigned long)value.medium,(unsigned long)value.high,replyName(reply));
  gateway_.publish(line);
}
void Application::flowCalibrationStatus(Reply reply, uint64_t client, uint32_t request) {
  const auto &stored=store_.flowCalibration();
  const bool overrideIdentity=client || request;
  const FlowCalibrationState empty{};
  const auto &s=overrideIdentity && (stored.client!=client || stored.request!=request) ? empty : stored;
  const JobKey key=s.key;
  uint32_t livePulses=0, runtimeMs=0;
  const int index=key.valid() ? scheduler_.find(s.address) : -1;
  if (index>=0) {
    const auto &live=scheduler_.node(size_t(index)).live;
    if (live.seen && live.key==key) { livePulses=live.pulses; runtimeMs=live.runtimeMs; }
  }
  char line[192];
  snprintf(line,sizeof(line),"FC e=%lu c=%llx q=%lu j=%lu:%lu s=%u a=%04x p=%u o=%lu r=%u ml=%lu v=%u b=%lu x=%lu f=%lu n=%lu t=%lu w=%u",
    (unsigned long)store_.state().epoch,(unsigned long long)(overrideIdentity?client:s.client),
    (unsigned long)(overrideIdentity?request:s.request),(unsigned long)key.epoch,(unsigned long)key.id,
    unsigned(s.slot),s.address,unsigned(s.phase),(unsigned long)s.operationSequence,
    unsigned(reply),(unsigned long)s.standardMl,unsigned(s.speed),(unsigned long)s.startPulses,
    (unsigned long)s.closePulses,(unsigned long)s.finalPulses,(unsigned long)livePulses,
    (unsigned long)runtimeMs,unsigned(s.reason));
  gateway_.publish(line);
}
void Application::meshState(uint32_t request) {
  const auto &s=mesh_.management(); char line[192];
  snprintf(line,sizeof(line),"MESH epoch=%lu req=%lu op=%lu scan=%lu phase=%s busy=%u capable=%u reason=%s found=%u target=%04x step=%u attempt=%u retry_wait=%u retry_ms=%u remove=1",
    (unsigned long)store_.state().epoch,(unsigned long)request,(unsigned long)s.operation,(unsigned long)s.scan,s.phase,s.busy,s.capable,s.reason,s.found,s.target,
    unsigned(s.configStep),unsigned(s.configAttempt),s.configRetryWait,unsigned(s.configRetryRemainingMs)); gateway_.publish(line);
  snprintf(line,sizeof(line),"MESH_BUDGET epoch=%lu op=%lu remaining_ms=%lu recovery=%s",
    (unsigned long)store_.state().epoch,(unsigned long)s.operation,(unsigned long)s.remainingTotalMs,s.recoveryAction);
  gateway_.publish(line);
  for (unsigned i=0;i<s.found;++i) { char id[33]; for (unsigned j=0;j<16;++j) snprintf(id+2*j,3,"%02x",s.candidates[i].uuid[j]);
    snprintf(line,sizeof(line),"CAND epoch=%lu scan=%lu uuid=%s",(unsigned long)store_.state().epoch,(unsigned long)s.scan,id); gateway_.publish(line); }
}
void Application::beginInventory(uint32_t request,uint32_t now) {
  if (inventorySending_) return;
  inventoryRequest_=request; inventoryCount_=0; inventoryCursor_=0;
  for (size_t i=0;i<MaxNodes;++i) { const auto &n=scheduler_.node(i); if (!n.config.address) continue;
    if (mesh_.management().capable && !mesh_.ownsSlave(n.config.address)) continue;
    inventory_[inventoryCount_++]={n.config.address,uint8_t(i+1),n.seen && !elapsed(now,n.seenAt,15000),n.commissioned}; }
  uint16_t paired[32]; const size_t count=mesh_.pairedAddresses(paired,32);
  for(size_t i=0;i<count && inventoryCount_<32;++i) { bool exists=false; for(unsigned j=0;j<inventoryCount_;++j) if(inventory_[j].address==paired[i]) exists=true;
    if(!exists) inventory_[inventoryCount_++]={paired[i],0,false,false}; }
  char line[96]; snprintf(line,sizeof(line),"INV_BEGIN epoch=%lu req=%lu count=%u",(unsigned long)store_.state().epoch,(unsigned long)request,inventoryCount_); gateway_.publish(line);
  inventorySending_=true; inventoryAt_=now;
}
void Application::inventoryTick(uint32_t now) {
  if(!inventorySending_ || !elapsed(now,inventoryAt_,100)) return;
  inventoryAt_=now; char line[160];
  if(inventoryCursor_<inventoryCount_) { const auto &n=inventory_[inventoryCursor_++];
    snprintf(line,sizeof(line),"INV_NODE epoch=%lu req=%lu slot=%u addr=%04x online=%u commissioned=%u",(unsigned long)store_.state().epoch,(unsigned long)inventoryRequest_,n.slot,n.address,n.online,n.commissioned);
  } else { snprintf(line,sizeof(line),"INV_END epoch=%lu req=%lu count=%u",(unsigned long)store_.state().epoch,(unsigned long)inventoryRequest_,inventoryCount_); inventorySending_=false; }
  gateway_.publish(line);
}
void Application::commissionResult(uint32_t request,uint8_t slot,uint16_t address,Reply reply,bool commissioned) {
  char line[144];
  snprintf(line,sizeof(line),"COMMISSION epoch=%lu req=%lu slot=%u addr=%04x reply=%s commissioned=%u",
    (unsigned long)store_.state().epoch,(unsigned long)request,unsigned(slot),address,replyName(reply),commissioned);
  gateway_.publish(line);
}
void Application::commissionTick(uint32_t now) {
  if (!commission_.active || !elapsed(now,commission_.sentAt,800)) return;
  if (commission_.attempts < 3) {
    auto retry=packet(Op::Commission,commission_.address); Writer writer{retry}; writer.u32(commission_.request);
    if (mesh_.send(retry)) { ++commission_.attempts; commission_.sentAt=now; return; }
  }
  if (!elapsed(now,commission_.sentAt,8000)) return;
  commissionResult(commission_.request,commission_.slot,commission_.address,Reply::Offline,false);
  commission_.active=false;
}
void Application::removalTick(uint32_t now) {
  const uint16_t address = store_.removingAddress();
  if (!address || !storageHealthy_) return;
  const auto &m = mesh_.management();
  if (!removalStarted_ && m.capable && !mesh_.managementBusy()) {
    // On reboot the Mesh node may already be gone. removeSlave then goes
    // directly to cleanup, without resetting any newly discovered device.
    removalStarted_ = mesh_.removeSlave(address,0,now);
    if (!removalStarted_) storageHealthy_ = false;
    return;
  }
  if (!removalStarted_ || strcmp(m.phase,"REMOVE_CLEANUP") || m.target != address) return;
  auto next = store_.state(); bool saved = true;
  for (size_t i=0; i<MaxNodes; ++i) if (next.nodes[i].address == address) {
    if (!store_.clearCalibration(i)) { saved = false; break; }
    next.nodes[i] = {};
  }
  if (saved) saved = store_.write(next);
  if (saved) {
    const int slot = scheduler_.find(address);
    if (slot >= 0) saved = scheduler_.unregisterNode(size_t(slot),address) == Reply::Ok;
  }
  if (saved) saved = store_.finishRemoval();
  if (!saved) { storageHealthy_=false; pump_.trip(PumpFault::Transport,now); }
  mesh_.finishSlaveRemoval(saved);
  removalStarted_ = false;
  meshState(m.operation);
}
void Application::periodic(uint32_t now) {
  commissionTick(now);
  if (meshResetPending_) {
    const auto &meshState = mesh_.management();
    if (!strcmp(meshState.phase, "FAILED")) {
      meshResetPending_ = false; gateway_.publish("MESH_RESET_FAILED");
    } else if (!strcmp(meshState.phase, "RESET_DONE")) {
      auto next = store_.state();
      for (auto &node : next.nodes) node = {};
      if (!store_.clearCalibrations() || !store_.write(next)) {
        storageHealthy_ = false; meshResetPending_ = false; gateway_.publish("MESH_RESET_FAILED");
      } else {
        meshResetPending_ = false; gateway_.publish("OK mesh_reset rebooting");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
      }
    }
  }
  if (pumpTestOwner_.client && (testOutputPublished_ != pump_.testing() ||
      (pump_.testing() && elapsed(now, testPublishedAt_, 1000)))) pumpTestReply(pumpTestOwner_, Reply::Ok, now);
  const auto report = scheduler_.report();
  if (!savedJob_ && !scheduler_.active() && !pump_.output()) commitReport(now);
  if (report.phase != lastPhase_ || strcmp(lastGuard_,scheduler_.guard())!=0) {
    lastGuard_=scheduler_.guard();
    ESP_LOGD("start_guard","t=%lu job=%lu:%lu phase=%u guard=%s closed=%08lx final=%08lx",
      (unsigned long)now,(unsigned long)report.key.epoch,(unsigned long)report.key.id,unsigned(report.phase),lastGuard_,
      (unsigned long)scheduler_.closedMask(),(unsigned long)scheduler_.finalMask());
    for(size_t i=0;i<MaxNodes;++i) if(report.selected & (uint32_t(1)<<i)) {
      const auto &n=scheduler_.node(i);
      ESP_LOGD("start_proof","peer=%04x start=%lu start_ack=%lu lease=%lu seq=%lu proof_age=%lu accepted=%u",
        n.config.address,(unsigned long)n.firstStartIssuedAt,(unsigned long)n.startAckAt,
        (unsigned long)n.leaseAckAt,(unsigned long)n.leaseAckSequence,
        (unsigned long)(n.leaseAckSeen?now-n.leaseAckAt:0),n.commandAccepted);
    }
  }
  if (report.phase != lastPhase_) {
    lastPhase_ = report.phase;
    ESP_LOGI("schedule", "t=%lu job=%lu:%lu phase=%u reason=%s selected=%08lx done=%08lx failed=%08lx unknown=%08lx",
      (unsigned long)now,(unsigned long)report.key.epoch,(unsigned long)report.key.id,unsigned(report.phase),reasonName(report.reason),
      (unsigned long)report.selected,(unsigned long)report.complete,(unsigned long)report.failed,(unsigned long)report.unknown);
    if (owner_.client) response(ownerIngress_, owner_, Reply::Ok, report.key);
  }
  retryResponse();
  replayFlows(now);
  requeryUnknown(now);
}
void Application::run() {
  esp_task_wdt_add(nullptr);
  ESP_LOGI("build", "id=%08lx protocol=%u features=%04x tracked=1 max_concurrent=%u",(unsigned long)PUMP_BUILD_ID,ProtocolRevision,RequiredFeatures,board::MaxConcurrent);
  uint32_t previousTick=nowMs(), maxTick=0, diagnosticAt=previousTick;
  uint32_t reportedMeshFaults=0;
  bool previousOutput=false;
  char ready[192];
  snprintf(ready, sizeof(ready), "PUMP_MULTI_V1 ready role=%s commissioned=%u; type hello or local",
    "PUMP_MASTER",
    board::pumpConfig().valid());
  gateway_.publish(ready);
  TickType_t tick = xTaskGetTickCount();
  for (;;) {
    const uint32_t now = nowMs();
    const uint32_t interval=now-previousTick; previousTick=now;
    if (interval>maxTick) maxTick=interval;
    if (elapsed(now,diagnosticAt,10000)) {
      const auto m=mesh_.metrics(); diagnosticAt=now;
      ESP_LOGD("mesh_diag", "healthy=%u submitted=%lu ended=%lu merged=%lu expired=%lu faults=%lu hello_timeout=%lu peak_sar=%u peak_pending=%u sar_max_ms=%lu tick_max_ms=%lu trace_lost=%lu",
        mesh_.healthy(),(unsigned long)m.submitted,(unsigned long)m.ended,(unsigned long)m.merged,(unsigned long)m.expired,
        (unsigned long)m.faults,(unsigned long)m.helloTimeouts,m.peakSar,m.peakPending,(unsigned long)m.maxSarMs,(unsigned long)maxTick,(unsigned long)m.traceLost);
      if (m.faults != reportedMeshFaults) {
        reportedMeshFaults=m.faults;
        publishMeshDiagnostics(mesh_.diagnostics(),mesh_.healthy(),[](const char *line) { ESP_LOGW("mesh_state","%s",line); });
      }
      ESP_LOGD("runtime_mem", "heap_free=%lu heap_min=%lu largest_free=%lu app_stack_min_bytes=%lu btc_stack_min_bytes=%lu",
        (unsigned long)esp_get_free_heap_size(),(unsigned long)esp_get_minimum_free_heap_size(),
        (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
        (unsigned long)uxTaskGetStackHighWaterMark(nullptr),
        (unsigned long)gateway_.bluetoothStackMinimumBytes());
    }
    mesh_.managementTick(now); removalTick(now); inventoryTick(now);
    pump_.tick(now);
    scheduler_.submissions(now);
    if (!mesh_.healthy() || mesh_.takeOverflow() || gateway_.overflow()) {
      pump_.trip(PumpFault::Transport, now); scheduler_.stop(Reason::Transport, now);
    }
    Packet p;
    for (unsigned count = 0; count < 4 && mesh_.receive(p); ++count) incoming(p, now);
    CommandLine commandLine; if (gateway_.receive(commandLine)) command(commandLine, now);
    modeTick(now);
    scheduler_.tick(now);
    const auto &calibration = store_.flowCalibration();
    if (calibration.phase == FlowCalibrationPhase::Starting &&
        scheduler_.report().key == calibration.key) {
      if (scheduler_.report().phase == SchedulePhase::Supplying && pump_.output()) {
        auto next = calibration; next.phase = FlowCalibrationPhase::Preout;
        if (store_.writeFlowCalibration(next)) flowCalibrationStatus(Reply::Ok);
        else {
          storageHealthy_ = false; pump_.stop(now); scheduler_.stop(Reason::Transport, now);
          flowCalibrationStatus(Reply::Storage);
        }
      } else if (!scheduler_.active() && scheduler_.report().phase == SchedulePhase::Failed) {
        auto next = calibration; next.phase = FlowCalibrationPhase::Invalid;
        next.reason = scheduler_.report().reason;
        if (!store_.writeFlowCalibration(next)) storageHealthy_ = false;
        flowCalibrationStatus(storageHealthy_ ? Reply::NotReady : Reply::Storage);
      }
    }
    if (previousOutput != pump_.output()) {
      previousOutput=pump_.output(); ESP_LOGI("output","t=%lu command=%u",(unsigned long)now,previousOutput);
    }
    periodic(now); esp_task_wdt_reset(); vTaskDelayUntil(&tick, pdMS_TO_TICKS(5));
  }
}
}
