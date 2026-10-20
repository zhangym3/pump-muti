#include "application.h"
#include "mesh_tx_diagnostics.h"
#include "hardware.h"
#include "esp_log.h"
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
  return !scheduler_.active() && !pump_.output();
}
void Application::response(uint16_t ingress, const ControlRequest &q, Reply result, JobKey key) {
  JobReport report; VolumeReport volume;
  if (key.valid() && key == scheduler_.report().key) {
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
    const auto &live = scheduler_.node(i).status;
    const auto &saved = store_.state().lastResults[i];
    const ValveStatus &s = live.key == flowReplayKey_ ? live : saved;
    if (s.key != flowReplayKey_) { ++flowReplayCursor_; continue; }
    char text[192];
    snprintf(text, sizeof(text), "FLOW addr=%04x job=%lu:%lu pulses=%lu ml=%lu phase=%s revision=%lu terminal=%u measured=%u reason=%s",
      scheduler_.node(i).config.address, static_cast<unsigned long>(flowReplayKey_.epoch),
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
  if (!mesh_.ready() || mesh_.address() != store_.state().master || !storageHealthy_) { response(ingress, q, Reply::NotReady, {}); return; }
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
  if (persisted.lastJob.unknown) { response(ingress, q, Reply::NotReady, {}); return; }
  if (!mesh_.healthy()) { response(ingress, q, Reply::NotReady, {}); return; }
  if (mesh_.managementBusy()) { response(ingress, q, Reply::Busy, {}); return; }
  if (slot >= 0) {
    const auto &old = persisted.clients[slot];
    const Reply replay = replayDecision(old, q, persisted.clientAmounts[slot]);
    if (replay != Reply::Pending) { response(ingress, q, replay, old.key); return; }
  } else if (free < 0) { response(ingress, q, Reply::Capacity, {}); return; }
  if (q.epoch != persisted.epoch || persisted.jobCounter == UINT32_MAX) { response(ingress, q, Reply::Stale, {}); return; }
  const auto allowed = scheduler_.check(q.mask, q.ml, q.maxMs, now, q.individual() ? q.amounts : nullptr);
  if (allowed != Reply::Ok) { response(ingress, q, allowed, {}); return; }
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
  Reader r{p};
  if (scheduler_.find(p.peer) < 0) {
    if (p.op != Op::Hello || !mesh_.ownsSlave(p.peer) || !idle() || !storageHealthy_) return;
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
      ESP_LOGI("cap_rx", "t=%lu peer=%04x nonce=%lu protocol=%u features=%04x build=%08lx applied=%u",
        (unsigned long)now,p.peer,(unsigned long)nonce,revision,features,(unsigned long)build,applied);
    }
  } else if (p.op == Op::LeaseAck) {
    const JobKey key = r.key(); const uint32_t sequence = r.u32(); const uint8_t rawReply = r.u8();
    if (r.done() && rawReply <= uint8_t(Reply::Unknown)) {
      bool matched=false; uint32_t issued=0;
      const auto &node=scheduler_.node(scheduler_.find(p.peer));
      for (const auto &v:node.leases) if (v.sequence==sequence) { matched=true; issued=v.issuedAt; }
      const bool applied=scheduler_.leaseAck(p.peer,key,sequence,Reply(rawReply),now);
      ESP_LOGI("lease_rx", "t=%lu peer=%04x job=%lu:%lu seq=%lu result=%s matched=%u issued=%lu age=%lu applied=%u",
        (unsigned long)now,p.peer,(unsigned long)key.epoch,(unsigned long)key.id,(unsigned long)sequence,
        replyName(Reply(rawReply)),matched,(unsigned long)issued,(unsigned long)(matched?now-issued:0),applied);
    }
  } else if (p.op == Op::CommandAck) {
    const JobKey key = r.key(); const uint8_t rawCommand = r.u8(), rawReply = r.u8();
    if (r.done() && (rawCommand == uint8_t(Op::Prepare) || rawCommand == uint8_t(Op::Start)) &&
        rawReply <= uint8_t(Reply::Unknown))
      scheduler_.commandReply(p.peer, key, Op(rawCommand), Reply(rawReply), now);
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
    ValveStatus s; if (!decodeResult(p, s)) return;
    if (!scheduler_.result(p.peer, s, now)) return;
    const int slot = scheduler_.find(p.peer); const uint32_t bit = uint32_t(1) << slot;
    const auto &current = scheduler_.node(slot).status;
    const auto &report = scheduler_.report();
    if (current.key == s.key && current.revision == s.revision && s.key == report.key &&
        (report.selected & bit) && (s.phase == Phase::Done || s.phase == Phase::Fault))
      publishFlow(p.peer, s.key, s.pulses, s.k, s.phase, s.reason, s.revision, true);
    const auto &last = store_.state().lastJob;
    if (!scheduler_.active() && !pump_.output() && s.key.valid() && s.key == last.key &&
        (last.selected & bit) && (last.unknown & bit) && s.reason != Reason::Reboot &&
        (s.phase == Phase::Done || s.phase == Phase::Fault)) {
      auto next = store_.state(); next.lastResults[slot] = s; next.lastJob = scheduler_.report();
      if (!store_.write(next)) { storageHealthy_ = false; pump_.trip(PumpFault::Transport, now); }
      else {
        savedJob_ = true;
        if (resultQueryOwnerPending_ && resultQueryKey_ == s.key) {
          response(resultQueryIngress_, resultQueryOwner_, Reply::Ok, s.key);
          if (!scheduler_.report().unknown) resultQueryOwnerPending_ = false;
        }
      }
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
  char *tokens[10] = {}; size_t count = 0; char *save = nullptr;
  for (char *p = strtok_r(line.text, " \t", &save); p; p = strtok_r(nullptr, " \t", &save)) {
    if (count == 10) { gateway_.publish("INVALID too_many_fields"); return; } tokens[count++] = p;
  }
  if (!count) return;
  auto number = [&](size_t index, uint64_t &v, unsigned base = 10) { return index < count && unsignedNumber(tokens[index], v, base); };
  const char *op = tokens[0];
  if (!strcmp(op, "hello") && count == 1) {
    char text[192]; snprintf(text, sizeof(text), "HELLO role=%s addr=%04x master=%04x epoch=%lu mesh=%u commissioned=%u pump_fault=%s individual=1 build=%08lx tx_healthy=%u",
      "PUMP_MASTER", mesh_.address(), store_.state().master,
      static_cast<unsigned long>(store_.state().epoch), mesh_.ready(),
      board::pumpConfig().valid(), pumpFaultName(pump_.fault()),(unsigned long)PUMP_BUILD_ID,mesh_.healthy()); gateway_.publish(text); return;
  }
  if (!strcmp(op, "local") && count == 1) {
    localState(now); pumpPwmState(); meshState(); return;
  }
  if (!strcmp(op, "mesh-diag") && count == 1) { meshState(); return; }
  if (!strcmp(op, "pump-speed")) {
    uint64_t epoch = 0, revision = 0, level = 0;
    if (count != 4 || !number(1, epoch) || !epoch || epoch > UINT32_MAX ||
        !number(2, revision) || revision > UINT32_MAX || !number(3, level) || level < 1 || level > 3) {
      gateway_.publish("INVALID pump_speed_fields"); return;
    }
    Reply result = Reply::Ok;
    if (epoch != store_.state().epoch || revision != pwmRevision_ || pwmRevision_ == UINT32_MAX) result = Reply::Stale;
    else if (!storageHealthy_) result = Reply::Storage;
    else if (scheduler_.active() || store_.state().lastJob.unknown || mesh_.managementBusy()) result = Reply::Busy;
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
    if (scheduler_.active() || pump_.testing() || mesh_.managementBusy()) { pumpTestReply(q, Reply::Busy, now); return; }
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
    if (scheduler_.active() || pump_.output() || pump_.testing() || store_.state().lastJob.unknown || mesh_.managementBusy()) {
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
      if (!storageHealthy_ || commission_.active || scheduler_.active() || pump_.output() || pump_.testing() ||
          store_.state().lastJob.unknown || mesh_.managementBusy()) {
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
    if (old >= 0 && size_t(old) != slot - 1) { gateway_.publish("CONFLICT node_address"); return; }
    auto next = store_.state(); next.nodes[slot - 1] = {uint16_t(address), uint32_t(k), uint32_t(ver)};
    if (!store_.write(next)) { storageHealthy_ = false; gateway_.publish("STORAGE"); return; }
    gateway_.publish(replyName(scheduler_.registerNode(slot - 1, {uint16_t(address), uint32_t(k), uint32_t(ver)}))); return;
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
void Application::meshState(uint32_t request) {
  const auto &s=mesh_.management(); char line[192];
  snprintf(line,sizeof(line),"MESH epoch=%lu req=%lu op=%lu scan=%lu phase=%s busy=%u capable=%u reason=%s found=%u target=%04x",
    (unsigned long)store_.state().epoch,(unsigned long)request,(unsigned long)s.operation,(unsigned long)s.scan,s.phase,s.busy,s.capable,s.reason,s.found,s.target); gateway_.publish(line);
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
void Application::periodic(uint32_t now) {
  commissionTick(now);
  if (meshResetPending_) {
    const auto &meshState = mesh_.management();
    if (!strcmp(meshState.phase, "FAILED")) {
      meshResetPending_ = false; gateway_.publish("MESH_RESET_FAILED");
    } else if (!strcmp(meshState.phase, "RESET_DONE")) {
      auto next = store_.state();
      for (auto &node : next.nodes) node = {};
      if (!store_.write(next)) {
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
  if (!savedJob_ && !scheduler_.active() && !pump_.output()) {
    auto next = store_.state(); next.lastJob = report;
    for (size_t i = 0; i < MaxNodes; ++i) next.lastResults[i] = scheduler_.node(i).status;
    storageHealthy_ = store_.write(next); savedJob_ = true;
    if (!storageHealthy_) pump_.trip(PumpFault::Transport, now);
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
  ESP_LOGI("build", "id=%08lx protocol=%u features=%04x tracked=1",(unsigned long)PUMP_BUILD_ID,ProtocolRevision,RequiredFeatures);
  uint32_t previousTick=nowMs(), maxTick=0, diagnosticAt=previousTick;
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
      ESP_LOGI("mesh_diag", "healthy=%u submitted=%lu ended=%lu merged=%lu expired=%lu faults=%lu hello_timeout=%lu peak_sar=%u peak_pending=%u sar_max_ms=%lu tick_max_ms=%lu trace_lost=%lu",
        mesh_.healthy(),(unsigned long)m.submitted,(unsigned long)m.ended,(unsigned long)m.merged,(unsigned long)m.expired,
        (unsigned long)m.faults,(unsigned long)m.helloTimeouts,m.peakSar,m.peakPending,(unsigned long)m.maxSarMs,(unsigned long)maxTick,(unsigned long)m.traceLost);
      publishMeshDiagnostics(mesh_.diagnostics(),mesh_.healthy(),[](const char *line) { ESP_LOGI("mesh_state","%s",line); });
    }
    mesh_.managementTick(now); inventoryTick(now);
    pump_.tick(now);
    scheduler_.submissions(now);
    if (!mesh_.healthy() || mesh_.takeOverflow() || gateway_.overflow()) {
      pump_.trip(PumpFault::Transport, now); scheduler_.stop(Reason::Transport, now);
    }
    Packet p;
    for (unsigned count = 0; count < 4 && mesh_.receive(p); ++count) incoming(p, now);
    CommandLine commandLine; if (gateway_.receive(commandLine)) command(commandLine, now);
    scheduler_.tick(now);
    if (previousOutput != pump_.output()) {
      previousOutput=pump_.output(); ESP_LOGI("output","t=%lu command=%u",(unsigned long)now,previousOutput);
    }
    periodic(now); esp_task_wdt_reset(); vTaskDelayUntil(&tick, pdMS_TO_TICKS(5));
  }
}
}
