#include "control_request.h"
#include "master_scheduler.h"
#include "persistent_state.h"
#include "mesh_removal.h"
#include "mesh_config_retry.h"
#include "result_commit.h"
#include "maintenance_session.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <deque>
#include <limits>
using namespace pump;
constexpr uint32_t TestSlaveBuildId = 0x12345678U;
#define CHECK(...) do { if (!(__VA_ARGS__)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__); std::exit(1); } } while (false)

void mesh_config_retry_policy() {
  MeshConfigRetry retry;
  retry.beginStep();
  CHECK(retry.submitted(100) && retry.attempts() == 1 && retry.waiting());
  CHECK(!retry.submitted(101));
  CHECK(retry.timeout(15100) == MeshConfigRetry::TimeoutResult::RetryScheduled);
  CHECK(!retry.waiting() && retry.retryPending());
  CHECK(retry.remainingMs(15100) == 12000 && retry.remainingMs(27099) == 1);
  CHECK(retry.remainingMs(27100) == 0);
  CHECK(!retry.retryDue(27099) && retry.retryDue(27100));
  CHECK(!retry.retryDue(27101));
  CHECK(retry.submitted(27100) && retry.attempts() == 2);
  CHECK(retry.response() && !retry.response());
  retry.beginStep();
  CHECK(retry.attempts() == 0 && retry.submitted(30000));
  CHECK(retry.timeout(45000) == MeshConfigRetry::TimeoutResult::RetryScheduled);
  CHECK(retry.retryDue(57000) && retry.submitted(57000));
  CHECK(retry.timeout(72000) == MeshConfigRetry::TimeoutResult::RetryScheduled);
  CHECK(retry.retryDue(84000) && retry.submitted(84000));
  CHECK(retry.timeout(99000) == MeshConfigRetry::TimeoutResult::Exhausted);
  CHECK(retry.timeout(100000) == MeshConfigRetry::TimeoutResult::Ignored);
  retry.beginStep();
  const uint32_t start = UINT32_MAX - 6000;
  CHECK(retry.submitted(start));
  CHECK(retry.timeout(start + MeshConfigRetry::ResponseTimeoutMs) == MeshConfigRetry::TimeoutResult::RetryScheduled);
  CHECK(!retry.retryDue(start + MeshConfigRetry::ResponseTimeoutMs + MeshConfigRetry::SarCleanupDelayMs - 1));
  CHECK(retry.retryDue(start + MeshConfigRetry::ResponseTimeoutMs + MeshConfigRetry::SarCleanupDelayMs));
  std::puts("PASS Mesh config retry serialization, SAR cleanup delay, response ownership, bounded attempts and timer wrap");
}

struct FakePump : PumpIO {
  bool on = false;
  unsigned starts = 0;
  uint8_t duty = 0; bool pwmOk = true;
  bool setDuty(uint8_t v) override { duty = v; return pwmOk; }
  bool healthy() const override { return pwmOk; }
  void pump(bool v) override { on = v; if (v) ++starts; }
};
PumpConfig pumpConfig(uint32_t maxRunMs = 5000) {
  PumpConfig c; c.commissioned = true;
  c.maxRunMs = maxRunMs; c.minOffMs = 50; return c;
}
struct FakeMesh : MeshSender {
  std::vector<Packet> sent;
  FakePump *pump = nullptr;
  uint32_t now = 100, sequence = 0;
  std::deque<MeshSubmission> pending;
  JobKey fenced;
  bool healthy() const override { return true; }
  bool beginLeaseScope(JobKey, uint32_t &floor) override { floor=sequence+1; return sequence!=UINT32_MAX; }
  MeshTxGovernor::Admission prepare(const Packet &p) override { return send(p) ? MeshTxGovernor::Admission::Accepted : MeshTxGovernor::Admission::Failed; }
  void retire(uint16_t,JobKey,Op) override {}
  void fence(JobKey key) override { fenced = key; CHECK(!pump || !pump->on); }
  bool takeSubmission(MeshSubmission &out) override {
    if (pending.empty()) return false;
    out=pending.front(); pending.pop_front(); return true;
  }
  bool send(const Packet &p) override {
    if (p.op == Op::Close && pump) CHECK(!pump->on);
    Packet committed=p;
    if (p.op == Op::Lease) { const uint32_t seq=++sequence; for (unsigned i=0;i<4;++i) committed.bytes[4+i]=uint8_t(seq>>(8*i)); }
    sent.push_back(committed); pending.push_back({committed,now,0,true}); pending.push_back({committed,now,0,false}); return true;
  }
  size_t count(Op op) const { size_t n = 0; for (auto &p : sent) if (p.op == op) ++n; return n; }
};

void pump_output_only_and_commissioning() {
  FakePump io; PumpSafetyController c(io,{}); c.boot(0);
  CHECK(!io.on && c.phase() == PumpPhase::Off && c.fault() == PumpFault::None && c.reset(0) == Reply::NotReady);
  PumpSafetyController p(io,pumpConfig()); p.boot(0);
  CHECK(p.start({},1,100) == Reply::Invalid && p.start({1,1},0,100) == Reply::Invalid);
  CHECK(p.start({1,1},1,49) == Reply::NotReady); CHECK(p.start({1,1},1,50) == Reply::Ok);
  p.tick(200); p.tick(700); CHECK(io.on && p.phase() == PumpPhase::Running);
  CHECK(p.reset(701) == Reply::NotReady && io.starts == 1);
  p.stop(702); CHECK(!io.on && p.phase() == PumpPhase::Off);
  auto invalid = pumpConfig(); invalid.maxRunMs = 0; CHECK(!invalid.valid());
  invalid = pumpConfig(); invalid.minOffMs = 0; CHECK(!invalid.valid());
  invalid = pumpConfig(); invalid.authorizationMs = 3001; CHECK(!invalid.valid());
}
void pump_authorization_and_replay() {
  FakePump io; PumpSafetyController p(io,pumpConfig()); p.boot(0); CHECK(p.reset(0) == Reply::Ok);
  CHECK(p.start({1,1},1,100) == Reply::Ok);
  CHECK(p.renew({1,2},2,200) == Reply::Stale);
  CHECK(p.renew({1,1},1,1099) == Reply::Stale);
  CHECK(p.renew({1,1},2,1100) == Reply::Stale);
  CHECK(!io.on && p.fault() == PumpFault::Authorization);
  CHECK(p.start({1,2},1,1200) == Reply::NotReady);
  FakePump b; PumpSafetyController renewed(b,pumpConfig()); renewed.boot(0);
  CHECK(renewed.reset(0) == Reply::Ok && renewed.start({1,1},1,100) == Reply::Ok);
  CHECK(renewed.renew({1,1},2,1000) == Reply::Ok);
  CHECK(renewed.renew({1,1},2,1999) == Reply::Stale);
  renewed.tick(1999); CHECK(b.on); renewed.tick(2000); CHECK(!b.on);
}
void pump_stop_cooldown_and_wrap() {
  FakePump io; PumpSafetyController p(io,pumpConfig()); const uint32_t t = UINT32_MAX - 80;
  p.boot(t); CHECK(p.reset(t) == Reply::Ok);
  CHECK(p.start({1,1},1,t+50) == Reply::Ok); p.stop(t+60); CHECK(!io.on);
  p.stop(t+100); CHECK(!p.ready(t+109) && p.ready(t+110));
  CHECK(p.renew({1,1},2,t+110) == Reply::Stale && !io.on);
  CHECK(p.start({1,2},1,t+110) == Reply::Ok && io.starts == 2);
  p.tick(t+1109); CHECK(io.on); p.tick(t+1110);
  CHECK(!io.on && p.fault() == PumpFault::Authorization);
}
void pump_runtime_and_fault_reset() {
  FakePump io; auto cfg = pumpConfig(); cfg.maxRunMs = 800;
  PumpSafetyController p(io,cfg); p.boot(0); CHECK(p.reset(0) == Reply::Ok); CHECK(p.start({1,1},1,100) == Reply::Ok);
  CHECK(p.renew({1,1},2,700) == Reply::Ok); p.tick(899); CHECK(io.on); p.tick(900);
  CHECK(p.fault() == PumpFault::Runtime && !io.on);
  p.stop(901); CHECK(p.fault() == PumpFault::Runtime);
  CHECK(p.renew({1,1},3,902) == Reply::Stale);
  CHECK(p.start({1,2},1,1000) == Reply::NotReady);
  CHECK(p.reset(1001) == Reply::Ok && !io.on && !p.ready(1050) && p.ready(1051));
}
struct ScheduleFixture {
  FakePump io;
  FakeMesh net;
  PumpSafetyController pump;
  MasterScheduler scheduler;
  explicit ScheduleFixture(uint8_t concurrent = 1, uint32_t maxRunMs = 5000)
      : pump(io,pumpConfig(maxRunMs)), scheduler(net,pump,concurrent) {
    net.pump = &io; pump.boot(0); CHECK(pump.reset(0) == Reply::Ok);
    for (size_t i=0;i<MaxNodes;++i) { CHECK(scheduler.registerNode(i,{uint16_t(i+2),1000,1}) == Reply::Ok); scheduler.hello(uint16_t(i+2),1000,1,true,100,TestSlaveBuildId); }
  }
  void tick(uint32_t now) {
    net.now=now;
    // Simulated compatible peers answer capability probes only. Lease ACKs
    // remain explicit so these tests cannot silently authorize a pump.
    for (unsigned pass=0;pass<4;++pass) {
      scheduler.tick(now);
      for (const auto &p:net.sent) if (p.op==Op::CapabilitiesGet) {
        Reader r{p}; scheduler.capabilities(p.peer,r.u32(),ProtocolRevision,
            RequiredFeatures | ManualCalibrationFeature,TestSlaveBuildId,now);
      }
    }
  }
  void prove(uint32_t now) {
    tick(now); scheduler.submissions(now);
    for (const auto &p:net.sent) if (p.op==Op::Lease) {
      Reader r{p}; CHECK(r.u32()==p.context.epoch); const auto k=p.context; const auto seq=r.u32();
      scheduler.leaseAck(p.peer,k,seq,Reply::Ok,now);
    }
    tick(now);
  }
  void status(size_t i, Phase phase, Reason reason = Reason::None, uint32_t revision = 1, uint32_t now = 200) {
    ValveStatus s; s.key = {1,1}; s.k = 1000; s.phase = phase; s.reason = reason; s.revision = revision;
    s.valveCommand = phase == Phase::Opening || phase == Phase::Running; s.quiet = phase == Phase::Done;
    if (phase == Phase::Done) s.pulses = 100;
    tick(now);
    if (phase==Phase::Prepared || phase==Phase::Opening || phase==Phase::Running)
      scheduler.commandReply(uint16_t(i+2),s.key,phase==Phase::Prepared ? Op::Prepare : Op::Start,Reply::Ok,now);
    scheduler.status(uint16_t(i+2),s,now);
  }
};
void result_commit_and_reconciliation() {
  ValveStatus s; s.key={1,1}; s.k=1000; s.phase=Phase::Done; s.reason=Reason::Target;
  s.revision=4; s.pulses=105; s.closePulses=100; s.quiet=true;
  ResultCommit durable; unsigned writes=0;
  auto save=[&](const ValveStatus &) { ++writes; return true; };
  CHECK(durable.commit(s,save) && durable.contains(s) && writes==1);
  CHECK(durable.commit(s,save) && writes==1);
  auto restored=s; restored.revision+=16;
  CHECK(sameMeasurement(s,restored) && !sameResult(s,restored));
  CHECK(durable.commit(restored,save) && writes==2);
  ResultCommit failed;
  CHECK(!failed.commit(s,[&](const ValveStatus &) { ++writes; return false; }));
  CHECK(!failed.healthy() && !failed.contains(s));
  CHECK(!failed.commit(s,save) && writes==3); // Failure stays latched; no ACK or retry-write loop.
  for (unsigned invalid=0;invalid<6;++invalid) {
    auto bad=s;
    switch(invalid) {
      case 0: bad.key={}; break; case 1: bad.revision=0; break;
      case 2: bad.valveCommand=true; break; case 3: bad.closePulses=106; break;
      case 4: bad.k=0; break; default: bad.reason=Reason(255); break;
    }
    ResultCommit c; CHECK(!terminalResult(bad) && !c.commit(bad,save) && !c.healthy());
  }
  ScheduleFixture f(5); f.scheduler.begin({1,1},1,100,10000,100);
  CHECK(f.scheduler.result(2,s,200));
  auto conflict=s; conflict.pulses++;
  CHECK(!f.scheduler.result(2,conflict,201));
  conflict.revision++; CHECK(!f.scheduler.result(2,conflict,202));
  CHECK(f.scheduler.result(2,restored,203)); // Persisted measurement survives a slave restart.
  CHECK(!f.scheduler.result(2,s,204));
  CHECK(!f.scheduler.result(100,restored,205));
  CHECK(sameResult(f.scheduler.node(0).status,restored));
  ScheduleFixture reboot;
  reboot.scheduler.begin({1,1},1,100,10000,100);
  auto unknown=s; unknown.reason=Reason::Reboot; unknown.truncated=true; unknown.quiet=false;
  unknown.pulses=unknown.closePulses=0; unknown.phase=Phase::Fault;
  CHECK(reboot.scheduler.result(2,unknown,200));
  CHECK(reboot.scheduler.result(2,restored,201));
  CHECK(!reboot.scheduler.report().unknown);
  ScheduleFixture capacity(5);
  CHECK(capacity.scheduler.check(0x3f,100,10000,300)==Reply::Capacity);
  std::puts("PASS durable result commit failure latch, invalid/duplicate results, revision conflicts and reboot reconciliation");
}
void maintenance_tokens_and_deadlines() {
  MaintenanceSession s(42);
  CHECK(!s.grant({1,2},0,60000,100) && !s.grant({1,2},41,60000,100));
  CHECK(!s.grant({1,2},42,999,100) && !s.grant({1,2},42,60001,100));
  CHECK(s.retire({1,1}) && s.retired({1,1}));
  CHECK(!s.grant({1,1},42,60000,100));
  CHECK(s.grant({1,2},42,1000,100) && s.allowed(1099));
  CHECK(s.grant({1,2},42,60000,1099)); // Retransmission cannot extend the original lease.
  CHECK(!s.allowed(1100) && s.retired({1,2}));
  CHECK(!s.grant({1,2},42,60000,1101));
  CHECK(s.grant({1,3},42,1000,1200));
  CHECK(!s.retire({1,1}) && s.allowed(1201));
  CHECK(s.retire({1,3}) && !s.allowed(1202));
  CHECK(!s.grant({1,3},42,1000,1203));
  CHECK(s.retire({1,5}) && !s.grant({1,4},42,1000,1204)); // Retire overtakes Grant.
  s.boot(43); CHECK(!s.grant({1,6},42,1000,1300));
  CHECK(s.retire({2,1}) && !s.grant({1,99},43,1000,1400)); // New master boot fences old packets.
  const uint32_t at=UINT32_MAX-500;
  CHECK(s.grant({2,2},43,1000,at) && s.allowed(at+999) && !s.allowed(at+1000));
  CHECK(s.retired({2,2}) && !s.grant({2,2},43,1000,at+1001));
  std::puts("PASS boot-bound maintenance tokens, lost/reordered grant, irreversible retirement, non-renewal and timer wrap");
}
void scheduler_order_and_completion() {
  ScheduleFixture f; CHECK(f.scheduler.check(1,100,10000,100) == Reply::Ok);
  f.scheduler.begin({1,1},1,100,10000,100); f.tick(150); CHECK(f.net.count(Op::Prepare) == 1 && !f.io.on);
  f.status(0,Phase::Prepared); f.tick(200); CHECK(!f.io.on);
  f.tick(350); CHECK(f.net.count(Op::Start) == 1 && !f.io.on);
  f.status(0,Phase::Opening,Reason::None,2,400); f.prove(400); CHECK(!f.io.on);
  CHECK(std::string(f.scheduler.guard())=="VALVE_OPEN_WAIT");
  f.tick(699); CHECK(!f.io.on); f.tick(700); CHECK(f.io.on);
  f.status(0,Phase::Settling,Reason::Target,3,750); f.tick(750); CHECK(!f.io.on);
  f.status(0,Phase::Done,Reason::Target,4,1250); f.pump.tick(1250); f.tick(1250);
  CHECK(f.scheduler.report().phase == SchedulePhase::Complete && f.scheduler.report().complete == 1);
}
void scheduler_valve_open_lead_and_cancellation() {
  // Every valve in a concurrent batch gets its own lead time, including the last ACK.
  {
    ScheduleFixture f(5); f.scheduler.begin({1,1},31,100,10000,100);
    for (unsigned i=0;i<5;++i) f.status(i,Phase::Prepared,Reason::None,1,150);
    f.tick(150);
    for (unsigned i=0;i<5;++i) {
      const uint32_t now=200+i*100;
      f.status(i,Phase::Opening,Reason::None,2,now); f.prove(now);
      CHECK(!f.io.on && f.io.starts==0);
    }
    f.tick(899); CHECK(!f.io.on); f.tick(900);
    CHECK(f.io.on && f.io.starts==1 && f.scheduler.report().phase==SchedulePhase::Supplying);
  }
  // Retransmitted StartOK must not postpone an already accepted opening command.
  {
    ScheduleFixture f; f.scheduler.begin({1,1},1,100,10000,100);
    f.status(0,Phase::Prepared,Reason::None,1,150); f.tick(150);
    f.status(0,Phase::Opening,Reason::None,2,160); f.prove(160);
    CHECK(f.scheduler.commandReply(2,{1,1},Op::Start,Reply::Ok,300));
    CHECK(f.scheduler.node(0).startAckAt==160);
    f.tick(459); CHECK(!f.io.on); f.tick(460); CHECK(f.io.on);
  }
  // Cancellation, residual-pressure target closure and faults during the lead
  // must all prevent even a momentary pump pulse when the timer expires.
  for (unsigned outcome=0;outcome<3;++outcome) {
    ScheduleFixture f; f.scheduler.begin({1,1},1,100,10000,100);
    f.status(0,Phase::Prepared,Reason::None,1,150); f.tick(150);
    f.status(0,Phase::Opening,Reason::None,2,160); f.prove(160); CHECK(!f.io.on);
    if (outcome==0) f.scheduler.stop(Reason::Manual,300);
    else if (outcome==1) CHECK(f.scheduler.targetClosed(2,{1,1},300));
    else f.status(0,Phase::Settling,Reason::NoFlow,3,300);
    f.tick(460); f.tick(1000);
    CHECK(!f.io.on && f.io.starts==0 && f.scheduler.report().phase==SchedulePhase::Draining);
  }
  // The lead does not make an old lease proof fresh. Renewals continue while
  // waiting for another valve, and a new proof is still required before pumping.
  {
    ScheduleFixture f(2); f.scheduler.begin({1,1},3,100,10000,100);
    f.status(0,Phase::Prepared,Reason::None,1,150);
    f.status(1,Phase::Prepared,Reason::None,1,150); f.tick(150);
    f.status(0,Phase::Opening,Reason::None,2,160); f.prove(160);
    f.status(1,Phase::Opening,Reason::None,2,1960); f.tick(1960);
    const auto last=f.scheduler.node(1).leases[0];
    CHECK(last.accepted && f.scheduler.leaseAck(3,{1,1},last.sequence,Reply::Ok,1960));
    f.tick(2259); CHECK(!f.io.on); f.tick(2260);
    CHECK(!f.io.on && std::string(f.scheduler.guard())=="LEASE_PROOF_OLD");
    uint32_t renewed=0;
    for (const auto &record:f.scheduler.node(0).leases)
      if (record.accepted && record.issuedAt==1960) renewed=record.sequence;
    CHECK(renewed && f.scheduler.leaseAck(2,{1,1},renewed,Reply::Ok,2261));
    f.tick(2261); CHECK(f.io.on);
  }
  // A late StartOK cannot extend the fixed bootstrap budget to fit the lead.
  {
    ScheduleFixture f; f.scheduler.begin({1,1},1,100,10000,100);
    f.status(0,Phase::Prepared,Reason::None,1,150); f.tick(150);
    const auto first=f.scheduler.node(0).firstStartIssuedAt;
    f.status(0,Phase::Opening,Reason::None,2,first+3800); f.prove(first+3800);
    f.tick(first+3999); CHECK(!f.io.on); f.tick(first+4000);
    CHECK(!f.io.on && f.io.starts==0 && std::string(f.scheduler.guard())=="BOOTSTRAP_TIMEOUT");
  }
}
void scheduler_cancel_retries_and_timeout() {
  ScheduleFixture f; f.scheduler.begin({1,1},1,100,10000,100); f.scheduler.stop(Reason::Manual,200);
  CHECK(f.net.count(Op::Close) == 1);
  for (uint32_t t=205;t<10300;t+=5) { if (t % 100 == 0) f.scheduler.stop(Reason::Manual,t); f.pump.tick(t); f.tick(t); }
  CHECK(f.net.count(Op::Close) >= 10);
  CHECK(!f.scheduler.active() && f.scheduler.report().unknown == 1 && !f.io.on);
}
void scheduler_calibration_can_stop_before_open_ack() {
  ScheduleFixture f;
  f.scheduler.begin({1,1},1,500,60000,100,nullptr,true);
  f.tick(150);
  CHECK(f.scheduler.report().phase == SchedulePhase::Preparing && !f.io.on);
  f.scheduler.stop(Reason::Manual,160);
  CHECK(!f.io.on && f.scheduler.report().phase == SchedulePhase::Draining);
  CHECK(f.net.count(Op::Close) == 1);
  f.scheduler.stop(Reason::Manual,170);
  CHECK(f.net.count(Op::Close) >= 1 && !f.io.on);
}
void scheduler_manual_calibration_ack_and_terminal_result() {
  ScheduleFixture f;
  CHECK(f.scheduler.checkCalibration(0,500,1,60000,100)==Reply::Ok);
  f.scheduler.begin({1,1},1,500,60000,100,nullptr,true);
  f.net.now=150; f.scheduler.tick(150);
  Packet probe; bool found=false;
  for (const auto &p:f.net.sent) if (p.op==Op::CapabilitiesGet) { probe=p; found=true; }
  CHECK(found); Reader nonce{probe};
  CHECK(f.scheduler.capabilities(2,nonce.u32(),ProtocolRevision,
      RequiredFeatures|ManualCalibrationFeature,TestSlaveBuildId,150));
  f.tick(151);
  f.status(0,Phase::Prepared,Reason::None,1,200);
  f.tick(250);
  f.status(0,Phase::Opening,Reason::None,2,300);
  f.prove(300);
  CHECK(!f.io.on); f.tick(600);
  CHECK(f.io.on && f.scheduler.report().phase==SchedulePhase::Supplying);

  CHECK(f.scheduler.markCalibration(700)==Reply::Ok);
  CHECK(f.net.sent.back().op==Op::MeasureMark && f.net.sent.back().peer==2);
  Reader mark{f.net.sent.back()}; CHECK(mark.key()==JobKey{1,1} && mark.done());
  CHECK(!f.scheduler.calibrationMarkAck(3,{1,1},100,Reply::Ok,701));
  CHECK(!f.scheduler.calibrationMarkAck(2,{1,2},100,Reply::Ok,701));
  CHECK(f.scheduler.calibrationMarkAck(2,{1,1},100,Reply::Ok,701));
  CHECK(f.scheduler.calibrationMarkAck(2,{1,1},100,Reply::Ok,702));
  CHECK(!f.scheduler.calibrationMarkAck(2,{1,1},101,Reply::Ok,702));
  CHECK(f.scheduler.markCalibration(702)==Reply::NotReady);

  f.scheduler.stop(Reason::Manual,800);
  CHECK(!f.io.on && f.scheduler.report().failed==0);
  ValveStatus terminal; terminal.key={1,1}; terminal.k=1000;
  terminal.phase=Phase::Done; terminal.reason=Reason::Manual;
  terminal.quiet=true; terminal.closePulses=2400; terminal.pulses=2415; terminal.revision=3;
  CHECK(f.scheduler.result(2,terminal,1400));
  CHECK(f.scheduler.report().complete==1 && f.scheduler.report().failed==0);
  CHECK(f.scheduler.node(0).status.closePulses-100==2300);
  CHECK(f.scheduler.node(0).status.pulses-f.scheduler.node(0).status.closePulses==15);

  ScheduleFixture legacy;
  legacy.scheduler.begin({1,1},1,500,60000,100,nullptr,true);
  legacy.net.now=150; legacy.scheduler.tick(150);
  Packet legacyProbe; found=false;
  for (const auto &p:legacy.net.sent) if (p.op==Op::CapabilitiesGet) { legacyProbe=p; found=true; }
  CHECK(found); Reader legacyNonce{legacyProbe};
  CHECK(legacy.scheduler.capabilities(2,legacyNonce.u32(),ProtocolRevision,
      RequiredFeatures,TestSlaveBuildId,150));
  CHECK(legacy.scheduler.stopReason()==Reason::Protocol && !legacy.io.on);
  CHECK(legacy.net.count(Op::Prepare)==0 && legacy.net.count(Op::Start)==0);
}
void scheduler_faults_and_ordering() {
  ScheduleFixture f; f.scheduler.begin({1,1},1,100,10000,100);
  f.status(0,Phase::Prepared); f.tick(200); f.status(0,Phase::Opening,Reason::None,2,250); f.prove(250); f.tick(550); CHECK(f.io.on);
  f.status(0,Phase::Prepared,Reason::None,1,560); CHECK(f.scheduler.node(0).status.phase == Phase::Opening);
  f.status(0,Phase::Settling,Reason::NoFlow,3,600); CHECK(!f.io.on && f.net.count(Op::Close) == 1);
  f.status(0,Phase::Fault,Reason::NoFlow,4,1100); f.pump.tick(1100); f.tick(1100);
  CHECK(f.scheduler.report().phase == SchedulePhase::Failed);
}
void lease_acknowledgements_and_timeout() {
  ScheduleFixture acknowledged; acknowledged.scheduler.begin({1,1},1,100,10000,100);
  acknowledged.status(0,Phase::Prepared); acknowledged.tick(200);
  acknowledged.tick(350); acknowledged.status(0,Phase::Opening,Reason::None,2,400);
  acknowledged.tick(400); CHECK(!acknowledged.io.on);
  acknowledged.tick(401);
  CHECK(acknowledged.net.count(Op::Lease) == 1);
  Reader lease{acknowledged.net.sent.back()}; CHECK(lease.u32() == 1);
  const uint32_t sequence = lease.u32(); CHECK(lease.done() && sequence != 0);
  CHECK(!acknowledged.scheduler.leaseAck(3,{1,1},sequence,Reply::Ok,450));
  CHECK(!acknowledged.scheduler.leaseAck(2,{2,1},sequence,Reply::Ok,450));
  CHECK(!acknowledged.scheduler.leaseAck(2,{1,1},sequence+1,Reply::Ok,450));
  CHECK(acknowledged.scheduler.leaseAck(2,{1,1},sequence,Reply::Ok,450));
  CHECK(!acknowledged.scheduler.leaseAck(2,{1,1},sequence,Reply::Ok,451));
  acknowledged.tick(451); CHECK(!acknowledged.io.on);
  acknowledged.tick(700); CHECK(acknowledged.io.on);

  ScheduleFixture lost; lost.scheduler.begin({1,1},1,100,10000,100);
  lost.status(0,Phase::Prepared); lost.tick(200); lost.tick(350);
  lost.status(0,Phase::Opening,Reason::None,2,400); lost.tick(400);
  for (uint32_t now=401; now<=4300; now+=50) { lost.pump.tick(now); lost.tick(now); }
  CHECK(!lost.io.on);
  lost.pump.tick(4400); lost.tick(4400);
  CHECK(!lost.io.on && lost.scheduler.report().phase == SchedulePhase::Draining);
}
void stale_lease_ack_stops_pump_as_lease_failure() {
  ScheduleFixture f; f.scheduler.begin({1,1},1,100,10000,100);
  f.status(0,Phase::Prepared); f.tick(200); f.tick(350);
  f.status(0,Phase::Opening,Reason::None,2,400); f.prove(400);
  f.tick(700);
  CHECK(f.io.on && f.scheduler.report().phase == SchedulePhase::Supplying);

  for (uint32_t now = 800; now <= 1600; now += 200) f.tick(now);
  f.tick(1800);
  f.scheduler.submissions(1800);
  const Packet *renewal = nullptr;
  for (const auto &p : f.net.sent) if (p.op == Op::Lease) renewal = &p;
  CHECK(renewal && f.net.count(Op::Lease) >= 2);
  Reader lease{*renewal};
  CHECK(lease.u32() == 1);
  const uint32_t sequence = lease.u32(); CHECK(lease.done() && sequence != 0);
  CHECK(f.scheduler.leaseAck(2,{1,1},sequence,Reply::Stale,1850));
  CHECK(!f.io.on && f.scheduler.report().phase == SchedulePhase::Draining);
  CHECK(f.scheduler.report().reason == Reason::Lease && f.net.fenced == JobKey{1,1});
}
void rejected_commands_and_late_terminal_reconciliation() {
  ScheduleFixture rejected; rejected.scheduler.begin({1,1},1,100,10000,100); rejected.tick(150);
  CHECK(rejected.scheduler.commandReply(2,{1,1},Op::Prepare,Reply::Busy,150));
  CHECK(!rejected.io.on && (rejected.scheduler.report().failed & 1));
  CHECK(!rejected.scheduler.commandReply(2,{1,1},Op::Start,Reply::Ok,151));

  ScheduleFixture restored;
  JobReport report; report.key={1,1}; report.selected=1; report.unknown=1; report.phase=SchedulePhase::Failed;
  ValveStatus saved; saved.key={1,1}; saved.phase=Phase::Fault; saved.reason=Reason::Reboot;
  saved.revision=7; saved.k=1000;
  ValveStatus results[MaxNodes]{}; results[0]=saved;
  restored.scheduler.restore(report,results);
  CHECK(restored.scheduler.report().unknown == 1);
  ValveStatus terminal; terminal.key={1,1}; terminal.k=1000; terminal.pulses=100;
  terminal.phase=Phase::Done; terminal.reason=Reason::Target; terminal.quiet=true; terminal.revision=8;
  restored.scheduler.status(2,terminal,500);
  CHECK(restored.scheduler.report().complete == 1);
  CHECK(restored.scheduler.report().unknown == 0);
  CHECK(restored.scheduler.report().phase == SchedulePhase::Complete);
}
void thirty_two_nodes_and_batches() {
  ScheduleFixture f;
  CHECK(f.scheduler.check(0xffffffff,100,10000,100)==Reply::Capacity);
  CHECK(f.scheduler.registerNode(32,{99,1000,1})==Reply::Capacity);
  CHECK(f.scheduler.registerNode(1,{2,1000,1})==Reply::Conflict);
  // Deployed slot 32 remains readable; product limits must not erase history.
  CHECK(f.scheduler.node(31).config.address==33);
  JobReport report; report.key={1,1}; report.selected=0xffffffff;
  report.phase=SchedulePhase::Failed; report.unknown=0xffffffff;
  ValveStatus results[MaxNodes]{}; f.scheduler.restore(report,results);
  CHECK(f.scheduler.report().unknown==0xffffffff);
}
void telemetry_cannot_control_outputs() {
  ScheduleFixture f; f.scheduler.begin({1,1},1,100,10000,100); f.status(0,Phase::Prepared);
  CHECK(!f.scheduler.summary(2,{2,1},100,1,210));
  CHECK(f.scheduler.summary(2,{1,1},100,1,210));
  CHECK(!f.scheduler.summary(2,{1,1},50,1,220));
  f.tick(220); CHECK(f.scheduler.report().phase == SchedulePhase::Opening && !f.io.on);
  CHECK(f.scheduler.node(0).telemetryPulses == 100 && f.scheduler.node(0).status.pulses == 0);
  CHECK(f.scheduler.volume().ml == 100 && f.scheduler.volume().measured == 1);
}
void request_parser_and_replay() {
  uint32_t pulses; CHECK(targetPulses(1000,5880,pulses) && pulses == 5880);
  CHECK(!targetPulses(1,1,pulses) && !targetPulses(UINT32_MAX,1000,pulses));
  const char *start[] = {"start","abcdef0123456789","42","7","80000001","500","60000"};
  ControlRequest q; CHECK(parseControl(start,7,7,q)); CHECK(q.mask == 0x80000001 && q.client == 0xabcdef0123456789ULL);
  CHECK(!parseControl(start,6,7,q));
  const char *bad[] = {"start","1","-1","7","1","500","60000"}; CHECK(!parseControl(bad,7,7,q));
  const char *big[] = {"start","1","4294967296","7","1","500","60000"}; CHECK(!parseControl(big,7,7,q));
  CHECK(parseControl(start,7,7,q));
  auto p = encodeRequest(1,q); CHECK(p.size == 37); ControlRequest decoded; CHECK(decodeRequest(p,decoded));
  CHECK(decoded.client == q.client && decoded.sequence == q.sequence && decoded.mask == q.mask);
  p.size--; CHECK(!decodeRequest(p,decoded));
  ClientRecord old{q.client,q.sequence,q.mask,q.ml,q.maxMs,{q.epoch,9}};
  CHECK(replayDecision(old,q) == Reply::Ok); q.ml++; CHECK(replayDecision(old,q) == Reply::Conflict);
  q.ml--; q.sequence--; CHECK(replayDecision(old,q) == Reply::Stale);
  q.sequence += 2; CHECK(replayDecision(old,q) == Reply::Pending);
  const char *stop[] = {"stop","1","43","7","7","9"}; CHECK(parseControl(stop,6,7,q) && q.stopKey == JobKey{7,9});
  const char *query[] = {"query","1","42"}; CHECK(parseControl(query,3,8,q) && q.epoch == 8);
  uint64_t n; CHECK(!unsignedNumber("18446744073709551616",n)); CHECK(!unsignedNumber("1e3",n));
}
void reboot_volume_is_unknown() {
  ScheduleFixture f; f.scheduler.begin({1,1},1,100,10000,100);
  f.status(0,Phase::Prepared,Reason::None,17,150); f.tick(150);
  f.status(0,Phase::Opening,Reason::None,18,160); f.prove(160); f.tick(460); CHECK(f.io.on);
  // Remote reboot report, independent of the slave controller implementation.
  ValveStatus reboot; reboot.key = {1,1}; reboot.k = 1000; reboot.revision = 33;
  reboot.phase = Phase::Fault; reboot.reason = Reason::Reboot; reboot.truncated = true;
  f.scheduler.status(2,reboot,500);
  f.status(0,Phase::Running,Reason::None,19,501);
  f.pump.tick(501); f.tick(501);
  CHECK(f.scheduler.report().phase == SchedulePhase::Failed && f.scheduler.report().unknown == 1);
  CHECK(f.scheduler.node(0).status.reason == Reason::Reboot && !f.io.on);
  CHECK(f.scheduler.volume().measured == 0);
}
void bounded_pump_test() {
  FakePump io; auto cfg = pumpConfig(); cfg.maxRunMs = 20000;
  PumpSafetyController c(io, cfg); c.boot(0);
  CHECK(!c.testReady(49) && c.testReady(50) && !io.on);
  CHECK(c.start({1,1},1,49) == Reply::NotReady);
  CHECK(c.reset(0) == Reply::Ok);
  CHECK(c.startTest({1,1},1,0,50) == Reply::Invalid);
  CHECK(c.startTest({1,1},1,10001,50) == Reply::Invalid);
  CHECK(c.startTest({1,1},1,3000,49) == Reply::NotReady);
  CHECK(c.startTest({1,1},1,3000,50) == Reply::Ok && c.testing());
  CHECK(c.startTest({1,1},1,3000,51) == Reply::NotReady && io.starts == 1);
  CHECK(c.renew({1,1},2,500) == Reply::Stale);
  c.tick(1050); CHECK(io.on && c.testRemaining(1050) == 2000);
  c.tick(3049); CHECK(io.on);
  c.tick(3050); CHECK(!io.on && !c.testing() && c.fault() == PumpFault::None);
  CHECK(c.testRemaining(3050) == 0 && !c.ready(3099) && c.ready(3100));
  CHECK(c.startTest({1,2},1,10000,3100) == Reply::Ok);
  c.stop(3200); CHECK(!io.on && !c.testing());
  CHECK(c.startTest({1,3},1,3000,3250) == Reply::Ok);
  c.trip(PumpFault::Transport,3300); CHECK(!io.on && c.fault() == PumpFault::Transport);
  CHECK(c.startTest({1,4},1,3000,4000) == Reply::NotReady);
}
void bounded_pump_test_wrap_and_limits() {
  FakePump io; PumpSafetyController c(io,pumpConfig());
  c.boot(UINT32_MAX - 100); CHECK(c.reset(UINT32_MAX - 100) == Reply::Ok);
  CHECK(c.startTest({1,1},1,5001,UINT32_MAX - 50) == Reply::Invalid);
  CHECK(c.startTest({1,1},1,3000,UINT32_MAX - 50) == Reply::Ok);
  c.tick(2948); CHECK(io.on && c.testRemaining(2948) == 1);
  c.tick(2949); CHECK(!io.on);
  CHECK(c.start({1,2},1,2999) == Reply::Ok);
  c.tick(3999); CHECK(!io.on && c.fault() == PumpFault::Authorization);
  auto cfg = pumpConfig(); cfg.commissioned = false;
  PumpSafetyController unconfigured(io,cfg); unconfigured.boot(0);
  CHECK(unconfigured.reset(0) == Reply::NotReady);
  CHECK(unconfigured.startTest({1,1},1,3000,1000) == Reply::NotReady);
  ControlRequest q; q.action = Action::Start; q.client = 1; q.sequence = 5; q.epoch = 7; q.maxMs = 3000;
  CHECK(!q.individual()); // mask=0 is the existing independent pump-test namespace.
  ClientRecord cancelled{1,5,0,0,0,{7,0}};
  CHECK(replayDecision(cancelled,q) == Reply::Conflict);
  ClientRecord accepted{1,5,0,0,3000,{7,1}};
  CHECK(replayDecision(accepted,q) == Reply::Ok);
}
void pwm_idle_and_bounded_switching() {
  FakePump io; PumpSafetyController c(io,pumpConfig()); c.boot(0);
  CHECK(!io.on && io.duty == 65 && c.speed() == 1);
  CHECK(c.setSpeed(2,0) == Reply::Ok && !io.on && c.fault() == PumpFault::None);
  CHECK(c.reset(0) == Reply::Ok);
  CHECK(c.setSpeed(0,1) == Reply::Invalid && c.setSpeed(4,1) == Reply::Invalid);
  CHECK(c.setSpeed(2,1) == Reply::Ok && io.duty == 80 && !io.on && io.starts == 0);
  CHECK(c.startTest({1,1},1,3000,50) == Reply::Ok && io.on);
  CHECK(c.setSpeed(3,1500) == Reply::Ok && io.duty == 100 && io.starts == 1);
  CHECK(c.testRemaining(1500) == 1550);
  CHECK(c.setSpeed(1,3000) == Reply::Ok && io.duty == 65 && io.starts == 1);
  c.tick(3050); CHECK(!io.on && !c.testing() && c.speed() == 1);
  CHECK(c.setSpeed(2,3051) == Reply::Ok && !io.on);
  CHECK(c.startTest({1,2},1,3000,3100) == Reply::Ok && io.duty == 80);
  c.stop(3150); CHECK(!io.on && c.speed() == 2);
  c.boot(3200); CHECK(!io.on && c.speed() == 1 && io.duty == 65);
}
void pwm_failure_and_authorization() {
  FakePump io; PumpSafetyController c(io,pumpConfig()); c.boot(0); CHECK(c.reset(0) == Reply::Ok);
  CHECK(c.start({1,1},1,50) == Reply::Ok);
  CHECK(c.setSpeed(3,1049) == Reply::Ok);
  c.tick(1050); CHECK(!io.on && c.fault() == PumpFault::Authorization);
  CHECK(c.reset(1100) == Reply::Ok);
  CHECK(c.startTest({1,2},1,3000,1150) == Reply::Ok);
  io.pwmOk = false;
  CHECK(c.setSpeed(2,1200) == Reply::NotReady && !io.on && c.fault() == PumpFault::Transport);
  CHECK(c.speed() == 3 && c.reset(1300) == Reply::NotReady);
  auto cfg = pumpConfig(); cfg.lowDuty = 0; CHECK(!cfg.valid());
  cfg = pumpConfig(); cfg.mediumDuty = cfg.lowDuty; CHECK(!cfg.valid());
  cfg = pumpConfig(); cfg.highDuty = 101; CHECK(!cfg.valid());
  cfg = pumpConfig(); cfg.commissioned = false; PumpSafetyController locked(io,cfg); locked.boot(0);
  CHECK(locked.setSpeed(3,1) == Reply::NotReady && !io.on);
}
void phone_test_does_not_relock_dosing() {
  FakePump io; PumpSafetyController c(io,pumpConfig()); c.boot(0);
  CHECK(!io.on && !c.ready(49) && c.ready(50) && c.testReady(50));
  CHECK(c.start({7,1},1,49) == Reply::NotReady);
  CHECK(c.setSpeed(3,0) == Reply::Ok && !io.on);
  CHECK(c.startTest({7,1},1,3001,50,true) == Reply::Invalid);
  CHECK(c.startTest({7,1},1,3000,50,true) == Reply::Ok && c.dryTesting());
  CHECK(io.on && io.duty == 65 && c.speed() == 1);
  CHECK(c.setSpeed(3,500) == Reply::Busy && io.duty == 65);
  CHECK(c.setSpeed(1,1000) == Reply::Ok && c.testRemaining(1000) == 2050);
  CHECK(c.renew({7,1},2,1000) == Reply::Stale);
  c.tick(3049); CHECK(io.on); c.tick(3050);
  CHECK(!io.on && !c.dryTesting() && c.fault() == PumpFault::None);
  CHECK(!c.testReady(3099) && c.testReady(3100) && c.ready(3100));
  CHECK(c.start({7,2},1,3100) == Reply::Ok);
  c.stop(3150);
  CHECK(c.setSpeed(2,3150) == Reply::Ok);
  CHECK(c.startTest({7,2},1,3000,3200) == Reply::Ok && io.duty == 80);
  c.stop(3300); CHECK(!io.on && c.fault() == PumpFault::None);
  CHECK(c.start({7,3},1,3350) == Reply::Ok);
  c.stop(3400);
  c.trip(PumpFault::Authorization,3400);
  CHECK(!c.testReady(4000) && c.startTest({7,3},1,3000,4000,true) == Reply::NotReady);
}
void dry_test_wrap_failure_and_replay() {
  FakePump io; PumpSafetyController c(io,pumpConfig()); c.boot(UINT32_MAX - 100);
  CHECK(c.startTest({7,1},1,3000,UINT32_MAX - 50,true) == Reply::Ok);
  c.tick(2948); CHECK(io.on); c.tick(2949);
  CHECK(!io.on && c.fault() == PumpFault::None);
  io.pwmOk = false;
  CHECK(c.startTest({7,2},1,3000,2999,true) == Reply::NotReady && !io.on);
  ControlRequest q; q.client = 1; q.sequence = 5; q.epoch = 7; q.maxMs = 3000; q.ml = 1;
  ClientRecord dry{1,5,0,1,3000,{7,1}};
  CHECK(replayDecision(dry,q) == Reply::Ok);
  q.ml = 0; CHECK(replayDecision(dry,q) == Reply::Conflict);
  q.ml = 1; ClientRecord cancelled{1,5,0,0,0,{7,0}};
  CHECK(replayDecision(cancelled,q) == Reply::Conflict);
}
void wire_contract_vectors() {
  CHECK(uint16_t(CompanyId) == 0x02E5 && uint16_t(ModelId) == 0x0001 && uint16_t(AllSlaves) == 0xC001);
  CHECK(uint8_t(Op::Prepare) == 0xC0 && uint8_t(Op::ResultQuery) == 0xC4 &&
    uint8_t(Op::Result) == 0xC5 && uint8_t(Op::ResultAck) == 0xC7 &&
    uint8_t(Op::Hello) == 0xCA && uint8_t(Op::HelloReply) == 0xCB &&
    uint8_t(Op::CommandAck) == 0xCF && uint8_t(Op::CapabilitiesGet) == 0xD0 &&
    uint8_t(Op::Capabilities) == 0xD1 && uint8_t(Op::Progress) == 0xD6);
  auto hello=packet(Op::Hello,2); Writer helloWriter{hello}; helloWriter.u32(5880);
  helloWriter.u32(1); helloWriter.u8(1); helloWriter.u32(TestSlaveBuildId);
  CHECK(hello.size==13);
  ControlRequest q; q.action = Action::Start; q.epoch = 0x11223344; q.client = 0x0102030405060708ULL;
  q.sequence = 0x99AABBCC; q.mask = 0x80000001; q.ml = 500; q.maxMs = 60000; q.stopKey = {0x55667788, 0xAABBCCDD};
  const uint8_t expected[] = {1, 0x44, 0x33, 0x22, 0x11, 8, 7, 6, 5, 4, 3, 2, 1,
    0xCC, 0xBB, 0xAA, 0x99, 1, 0, 0, 0x80, 0xF4, 1, 0, 0, 0x60, 0xEA, 0, 0,
    0x88, 0x77, 0x66, 0x55, 0xDD, 0xCC, 0xBB, 0xAA};
  Packet p = encodeRequest(0x1234, q); CHECK(p.op == Op::Request && p.peer == 0x1234 && p.size == sizeof(expected));
  for (size_t i = 0; i < sizeof(expected); ++i) CHECK(p.bytes[i] == expected[i]);
  ControlRequest decoded; CHECK(decodeRequest(p, decoded)); CHECK(decoded.action == q.action && decoded.epoch == q.epoch &&
    decoded.client == q.client && decoded.sequence == q.sequence && decoded.mask == q.mask && decoded.ml == q.ml &&
    decoded.maxMs == q.maxMs && decoded.stopKey == q.stopKey);
  --p.size; CHECK(!decodeRequest(p, decoded));
  auto progress=packet(Op::Progress,2); Writer progressWriter{progress};
  progressWriter.key({1,2}); progressWriter.u32(120); progressWriter.u32(2000);
  const uint8_t expectedProgress[] = {1,0,0,0,2,0,0,0,0x78,0,0,0,0xD0,0x07,0,0};
  CHECK(progress.peer==2 && unicast(progress.peer) && controlFormat(progress) && progress.size==sizeof(expectedProgress));
  for(size_t i=0;i<sizeof(expectedProgress);++i) CHECK(progress.bytes[i]==expectedProgress[i]);
}
void individual_request_and_durable_replay() {
  char plan[65]; memset(plan, '0', 64); plan[64] = 0;
  plan[1] = '6'; plan[62] = '2'; plan[63] = '8'; // 300 mL and 2000 mL, slots 1 and 32.
  const char *tokens[] = {"start-each", "ffffffffffffffff", "4294967295", "7", "80000001", "240000", plan};
  ControlRequest q; CHECK(parseControl(tokens, 7, 7, q));
  CHECK(q.individual() && q.amounts[0] == 6 && q.amounts[31] == 40 && q.maxMs == 240000);
  auto packet = encodeRequest(1, q); CHECK(packet.size == 61);
  ControlRequest decoded; CHECK(decodeRequest(packet, decoded));
  CHECK(!memcmp(decoded.amounts, q.amounts, MaxNodes));
  CHECK(decoded.client == q.client && decoded.mask == q.mask && decoded.sequence == q.sequence);
  --packet.size; CHECK(!decodeRequest(packet, decoded));
  plan[3] = '1'; CHECK(!parseControl(tokens, 7, 7, decoded)); plan[3] = '0';
  plan[1] = '0'; CHECK(!parseControl(tokens, 7, 7, decoded)); plan[1] = '6';
  plan[63] = '9'; CHECK(!parseControl(tokens, 7, 7, decoded)); plan[63] = '8';
  plan[1] = 'g'; CHECK(!parseControl(tokens, 7, 7, decoded)); plan[1] = '6';
  plan[63] = 0; CHECK(!parseControl(tokens, 7, 7, decoded));
  ClientRecord record{q.client,q.sequence,q.mask,0,q.maxMs,{q.epoch,10}};
  CHECK(replayDecision(record,q,q.amounts) == Reply::Ok);
  auto changed = q; changed.amounts[0] = 7;
  CHECK(replayDecision(record,changed,q.amounts) == Reply::Conflict);
  changed = q; changed.ml = 300;
  CHECK(replayDecision(record,changed,q.amounts) == Reply::Conflict);
  changed = q; --changed.sequence;
  CHECK(replayDecision(record,changed,q.amounts) == Reply::Stale);
}
void individual_scheduler_prepares_each_slot_and_retains_global_limit() {
  ScheduleFixture f(2);
  uint8_t plan[MaxNodes] = {}; plan[0] = 6; plan[31] = 40;
  CHECK(f.scheduler.check(0x80000001,0,60000,100,plan) == Reply::Ok);
  CHECK(f.scheduler.check(1,0,60000,100,plan) == Reply::Invalid);
  CHECK(f.scheduler.check(0x80000001,300,60000,100,plan) == Reply::Invalid);
  CHECK(f.scheduler.check(0x80000001,0,240001,100,plan) == Reply::Invalid);
  f.scheduler.begin({1,1},0x80000001,0,60000,100,plan);
  f.tick(150);
  Packet p;
  for (const auto &sent:f.net.sent) if (sent.op==Op::Prepare && sent.peer==2) p=sent;
  Reader first{p}; CHECK(p.op == Op::Prepare && p.peer == 2);
  CHECK(first.key() == JobKey{1,1}); CHECK(first.u32() == 300 && first.u32() == 300);
  CHECK(first.u32() == 1000 && first.u32() == 1 && first.u32() == 60000);
  CHECK(first.u16() == SpeedLowPermille && first.u32() == LeaseMs && first.u8() == PrepareLiveTelemetry && first.u32() == 1 && first.done());
  for (const auto &sent:f.net.sent) if (sent.op==Op::Prepare && sent.peer==33) p=sent;
  Reader second{p}; CHECK(p.op == Op::Prepare && p.peer == 33);
  CHECK(second.key() == JobKey{1,1}); CHECK(second.u32() == 2000 && second.u32() == 2000);
  CHECK(second.u32() == 1000 && second.u32() == 1 && second.u32() == 60000);
  CHECK(second.u16() == SpeedLowPermille && second.u32() == LeaseMs && second.u8() == PrepareLiveTelemetry && second.u32() == 1 && second.done());
  // Legacy per-slot plans remain readable, but are never silently split into batches.
  f.status(0,Phase::Prepared,Reason::None,1,350);
  f.status(31,Phase::Prepared,Reason::None,1,350); f.tick(350);
  f.status(0,Phase::Done,Reason::Target,3,360);
  f.status(31,Phase::Done,Reason::Target,3,360); f.tick(360); f.tick(370);
  CHECK(f.scheduler.report().phase == SchedulePhase::Complete && f.scheduler.report().complete == 0x80000001);
  CHECK(!f.io.on && f.io.starts == 0);
}
void persistent_v1_migration_preserves_identity_results_and_plan_replay() {
  LegacyPersistentState old; old.profile = 42; old.epoch = 111; old.jobCounter = 19; old.gatewayPin = 123456;
  old.nodes[31] = {33,5880,2}; old.clients[15] = {77,99,0x80000000,1000000,60000,{111,19}};
  old.lastJob.key = {111,19}; old.lastJob.selected = 0x80000000; old.lastJob.phase = SchedulePhase::Complete;
  old.lastResults[31].key = {111,19}; old.lastResults[31].pulses = 1234;
  PersistentState upgraded;
  CHECK(decodePersistentState(&old,sizeof(old),42,upgraded));
  CHECK(upgraded.format == 3 && upgraded.lastJob.reason == Reason::None && upgraded.epoch == 111 && upgraded.jobCounter == 19 && upgraded.gatewayPin == old.gatewayPin);
  CHECK(upgraded.nodes[31].address == 33 && upgraded.nodes[31].version == 2 && upgraded.clients[15].sequence == 99);
  CHECK(upgraded.clients[15].ml == 1000000 && upgraded.lastJob.key == old.lastJob.key && upgraded.lastResults[31].pulses == 1234);
  for (auto &amounts : upgraded.clientAmounts) for (auto v : amounts) CHECK(v == 0);
  auto &c = upgraded.clients[15]; c.ml = 0; upgraded.clientAmounts[15][31] = 40;
  PersistentState restored; CHECK(decodePersistentState(&upgraded,sizeof(upgraded),42,restored));
  ControlRequest q; q.action = Action::Start; q.client = c.client; q.sequence = c.sequence; q.mask = c.mask;
  q.epoch = c.key.epoch; q.maxMs = c.maxMs; q.amounts[31] = 40;
  CHECK(replayDecision(restored.clients[15],q,restored.clientAmounts[15]) == Reply::Ok);
  q.amounts[31] = 39; CHECK(replayDecision(restored.clients[15],q,restored.clientAmounts[15]) == Reply::Conflict);
  CHECK(!decodePersistentState(&upgraded,sizeof(upgraded)-1,42,restored));
  CHECK(!decodePersistentState(&old,sizeof(old),43,restored));
  upgraded.clientAmounts[15][31] = 0; CHECK(!decodePersistentState(&upgraded,sizeof(upgraded),42,restored));
  old.format = 2; CHECK(!decodePersistentState(&old,sizeof(old),42,restored));
}
void lease_proof_age_and_boundary_matrix() {
  for (uint32_t age : {900U,1100U,1800U,1999U,2000U,2500U}) {
    ScheduleFixture f; f.scheduler.begin({1,1},1,100,10000,100);
    f.status(0,Phase::Prepared,Reason::None,1,150); f.tick(150);
    f.status(0,Phase::Opening,Reason::None,2,160); f.tick(160);
    const auto record=f.scheduler.node(0).leases[0]; CHECK(record.accepted && record.sequence);
    CHECK(!f.scheduler.leaseAck(3,{1,1},record.sequence,Reply::Ok,160));
    CHECK(!f.scheduler.leaseAck(2,{1,2},record.sequence,Reply::Ok,160));
    CHECK(!f.scheduler.leaseAck(2,{1,1},record.sequence+999,Reply::Ok,160));
    const auto now=record.issuedAt+age;
    CHECK(f.scheduler.leaseAck(2,{1,1},record.sequence,Reply::Ok,now)==(age<4000));
    f.tick(now); CHECK(f.io.on==(age<2000));
    if (age<4000) {
      CHECK(f.scheduler.node(0).leaseAckAt==record.issuedAt);
      CHECK(!f.scheduler.leaseAck(2,{1,1},record.sequence,Reply::Ok,now+1));
      CHECK(f.scheduler.node(0).leaseAckAt==record.issuedAt);
    }
  }
  ScheduleFixture f(1,20000); f.scheduler.begin({1,1},1,100,10000,100);
  f.status(0,Phase::Prepared,Reason::None,1,150); f.tick(150);
  f.status(0,Phase::Opening,Reason::None,2,160); f.prove(160); f.tick(460); CHECK(f.io.on);
  const auto issued=f.scheduler.node(0).leaseAckAt;
  for(uint32_t t=issued+400;t<issued+8000;t+=200){f.pump.tick(t);f.tick(t);CHECK(f.io.on);}
  f.tick(issued+7999);CHECK(f.io.on);f.tick(issued+8000);
  CHECK(!f.io.on && f.scheduler.report().reason==Reason::Lease && f.net.fenced==JobKey{1,1});
}
void early_ack_commit_and_rejection_matrix() {
  for (bool reject : {false,true}) {
    ScheduleFixture f; f.scheduler.begin({1,1},1,100,10000,100);
    f.status(0,Phase::Prepared,Reason::None,1,150); f.tick(150);
    f.status(0,Phase::Opening,Reason::None,2,160);
    f.net.now=160; f.scheduler.tick(160);
    CHECK(f.net.pending.size()==2);
    const auto accepted=f.net.pending.back(); f.net.pending.pop_back();
    Reader r{accepted.packet};const auto epoch=r.u32();const auto key=accepted.packet.context; CHECK(key.epoch==epoch);const auto seq=r.u32();
    CHECK(!f.scheduler.leaseAck(2,key,seq,Reply::Ok,160)); // not committed in ledger yet
    f.scheduler.submissions(160);
    CHECK(!f.scheduler.leaseAck(2,key,seq,Reply::Ok,170));
    CHECK(!f.scheduler.node(0).leaseAckSeen);
    auto result=accepted;result.error=reject?-12:0;f.net.pending.push_back(result);
    f.scheduler.submissions(180);f.scheduler.tick(180);
    CHECK(!f.io.on); f.tick(460);
    CHECK(f.io.on==!reject);
    if(reject)CHECK(f.scheduler.report().reason==Reason::Transport);
    else CHECK(f.scheduler.node(0).leaseAckAt==160);
  }
  ScheduleFixture f;f.scheduler.begin({1,1},1,100,10000,100);f.tick(100);
  f.status(0,Phase::Prepared,Reason::None,1,150);f.tick(150);
  f.tick(1650);f.tick(3150);CHECK(!f.io.on && f.net.count(Op::Start)==3);
  f.tick(4150);CHECK(f.scheduler.report().reason==Reason::Transport && !f.io.on);
}
void capability_identity_and_incompatibility_matrix() {
  // Any nonzero build can pass with matching HELLO and compatible capabilities.
  // Include the field build that previously failed the deployment allowlist.
  for (const uint32_t buildId : {1U, 0xc0f6b78cU, 0xdeadbeefU, UINT32_MAX}) {
    ScheduleFixture f;
    f.scheduler.hello(2,1000,1,true,100,buildId);
    f.scheduler.begin({1,1},1,100,10000,100);
    f.scheduler.tick(100);
    CHECK(f.net.sent.size()==1 && f.net.sent.front().op==Op::CapabilitiesGet);
    Reader request{f.net.sent.front()};
    CHECK(f.scheduler.capabilities(2,request.u32(),ProtocolRevision,RequiredFeatures,buildId,101));
    CHECK(f.scheduler.node(0).capable && f.scheduler.node(0).buildId==buildId);
    f.scheduler.tick(102); f.scheduler.tick(103);
    CHECK(f.scheduler.report().phase==SchedulePhase::Preparing && f.scheduler.report().reason==Reason::None);
    CHECK(f.net.count(Op::Prepare)==1 && f.net.count(Op::Start)==0 && !f.io.on);
  }
  for (unsigned mode=0;mode<4;++mode) {
    ScheduleFixture f; f.scheduler.begin({1,1},1,100,10000,100);
    f.net.now=100; f.scheduler.tick(100);
    CHECK(f.net.sent.size()==1 && f.net.sent.front().op==Op::CapabilitiesGet);
    Reader r{f.net.sent.front()}; const auto nonce=r.u32();
    CHECK(!f.scheduler.capabilities(3,nonce,ProtocolRevision,RequiredFeatures,TestSlaveBuildId,101));
    CHECK(!f.scheduler.capabilities(2,nonce+1,ProtocolRevision,RequiredFeatures,TestSlaveBuildId,101));
    CHECK(!f.scheduler.capabilities(99,nonce,ProtocolRevision,RequiredFeatures,TestSlaveBuildId,101));
    CHECK(!f.scheduler.node(0).capable && !f.io.on);
    CHECK(f.scheduler.capabilities(2,nonce,mode==0?1:ProtocolRevision,
      mode==1?RequiredFeatures & ~1U:RequiredFeatures,mode==2?0:mode==3?0xdeadbeefU:TestSlaveBuildId,102));
    CHECK(f.scheduler.report().reason==Reason::Protocol && !f.io.on);
    CHECK(f.net.count(Op::Start)==0 && f.net.fenced==JobKey{1,1});
  }
  ScheduleFixture f; f.scheduler.begin({1,1},1,100,10000,100);
  f.scheduler.tick(100); f.scheduler.tick(15100);
  CHECK(f.scheduler.report().reason==Reason::Protocol && !f.io.on && f.net.count(Op::Start)==0);
}
void capability_cache_build_identity_and_restart() {
  auto primeAndStop=[](ScheduleFixture &f) {
    f.scheduler.begin({1,1},1,100,10000,100);
    f.scheduler.tick(100);
    CHECK(f.net.count(Op::CapabilitiesGet)==1);
    Reader request{f.net.sent.back()}; CHECK(f.net.sent.back().op==Op::CapabilitiesGet);
    CHECK(f.scheduler.capabilities(2,request.u32(),ProtocolRevision,RequiredFeatures,
      TestSlaveBuildId,101));
    CHECK(f.scheduler.node(0).capable);
    f.scheduler.stop(Reason::Manual,102);
    ValveStatus terminal; terminal.key={1,1}; terminal.k=1000; terminal.phase=Phase::Fault;
    terminal.reason=Reason::Manual; terminal.quiet=true; terminal.revision=1;
    CHECK(f.scheduler.result(2,terminal,103));
    f.scheduler.tick(103);
    CHECK(!f.scheduler.active());
  };

  ScheduleFixture sameBuild; primeAndStop(sameBuild);
  sameBuild.scheduler.hello(2,1000,1,true,200,TestSlaveBuildId);
  sameBuild.scheduler.begin({1,2},1,100,10000,200);
  sameBuild.scheduler.tick(200); sameBuild.scheduler.tick(201);
  CHECK(sameBuild.scheduler.node(0).capable && sameBuild.net.count(Op::CapabilitiesGet)==1);

  ScheduleFixture changedBuild; primeAndStop(changedBuild);
  const uint32_t changedId=TestSlaveBuildId^1U;
  changedBuild.scheduler.hello(2,1000,1,true,200,changedId);
  CHECK(!changedBuild.scheduler.node(0).capable);
  changedBuild.scheduler.begin({1,2},1,100,10000,200);
  changedBuild.scheduler.tick(200);
  CHECK(changedBuild.net.count(Op::CapabilitiesGet)==2);
  Reader changedRequest{changedBuild.net.sent.back()};
  CHECK(changedBuild.net.sent.back().op==Op::CapabilitiesGet);
  CHECK(changedBuild.scheduler.capabilities(2,changedRequest.u32(),ProtocolRevision,RequiredFeatures,changedId,201));
  changedBuild.scheduler.tick(202); changedBuild.scheduler.tick(203);
  CHECK(changedBuild.scheduler.node(0).capable && changedBuild.scheduler.node(0).buildId==changedId);
  CHECK(changedBuild.scheduler.report().reason==Reason::None && changedBuild.net.count(Op::Prepare)==1);
  CHECK(!changedBuild.io.on);

  ScheduleFixture restartedMaster;
  CHECK(!restartedMaster.scheduler.node(0).capable);
  restartedMaster.scheduler.begin({1,1},1,100,10000,100);
  restartedMaster.scheduler.tick(100);
  CHECK(restartedMaster.net.count(Op::CapabilitiesGet)==1);
}
void lease_cross_round_and_timer_wrap() {
  const uint32_t start=UINT32_MAX-200;
  ScheduleFixture f(1,20000); f.scheduler.hello(2,1000,1,true,start,TestSlaveBuildId);
  f.scheduler.begin({1,1},1,100,10000,start);
  f.tick(start+50); f.status(0,Phase::Prepared,Reason::None,1,start+50); f.tick(start+50);
  f.status(0,Phase::Opening,Reason::None,2,start+60); f.prove(start+60); CHECK(!f.io.on);
  f.tick(start+359); CHECK(!f.io.on); f.tick(start+360); CHECK(f.io.on);
  const auto issued=f.scheduler.node(0).leaseAckAt;
  for (uint32_t delta=400;delta<=2000;delta+=200) { f.pump.tick(issued+delta); f.tick(issued+delta); CHECK(f.io.on); }
  const auto &n=f.scheduler.node(0);
  uint32_t previousSequence=0;
  for (const auto &v:n.leases) if (v.issuedAt==issued+1400) previousSequence=v.sequence;
  CHECK(previousSequence!=0); // A newer round has been committed, but this proof is still valid.
  CHECK(f.scheduler.leaseAck(2,{1,1},previousSequence,Reply::Ok,issued+2050));
  CHECK(f.scheduler.node(0).leaseAckAt==issued+1400);
  CHECK(!f.scheduler.leaseAck(2,{1,1},previousSequence,Reply::Ok,issued+2100));
  for (uint32_t delta=2200;delta<9400;delta+=200) { f.pump.tick(issued+delta); f.tick(issued+delta); CHECK(f.io.on); }
  f.tick(issued+9399); CHECK(f.io.on); f.tick(issued+9400);
  CHECK(!f.io.on && f.scheduler.report().reason==Reason::Lease);
}
void stop_reason_migration_and_late_target() {
  PersistentState old;old.profile=42;old.format=2;old.lastJob.reason=Reason(255);
  PersistentState migrated;CHECK(decodePersistentState(&old,sizeof(old),42,migrated));
  CHECK(migrated.format==3 && migrated.lastJob.reason==Reason::None);
  migrated.lastJob.reason=Reason::Lease;
  PersistentState restored;CHECK(decodePersistentState(&migrated,sizeof(migrated),42,restored));
  CHECK(restored.lastJob.reason==Reason::Lease);
  migrated.lastJob.reason=Reason(255);CHECK(!decodePersistentState(&migrated,sizeof(migrated),42,restored));
  ScheduleFixture f;f.scheduler.begin({1,1},1,100,10000,100);f.scheduler.stop(Reason::Transport,110);
  f.status(0,Phase::Done,Reason::Target,5,120);f.tick(120);
  CHECK(f.scheduler.report().phase==SchedulePhase::Failed && f.scheduler.report().failed==1);
  CHECK(f.scheduler.report().reason==Reason::Transport && !f.io.on);
}

void v4_exact_boundaries_and_ledger() {
  // A business StartOK cannot override a later SDK rejection of that submission.
  for(bool reject:{false,true}) {
    ScheduleFixture f;f.scheduler.begin({1,1},1,100,10000,100);
    f.status(0,Phase::Prepared,Reason::None,1,150);
    for(unsigned n=0;n<4 && !f.net.count(Op::Start);++n)f.scheduler.tick(150);
    CHECK(f.net.pending.size()==2 && f.net.pending.back().packet.op==Op::Start);
    auto accepted=f.net.pending.back();f.net.pending.pop_back();
    f.scheduler.submissions(150);
    CHECK(!f.scheduler.commandReply(2,{1,1},Op::Start,Reply::Ok,160));
    CHECK(!f.scheduler.node(0).startAckSeen && !f.io.on);
    accepted.error=reject?-12:0;f.net.pending.push_back(accepted);
    f.scheduler.submissions(170);
    CHECK(f.scheduler.node(0).startAckSeen==!reject && !f.io.on);
    if(reject)CHECK(std::string(f.scheduler.guard())=="TX_ERROR");
    else {f.prove(180);CHECK(!f.io.on);f.tick(469);CHECK(!f.io.on);f.tick(470);CHECK(f.io.on);}
  }
  // Nonconsecutive registry slots still occupy distinct renewal phases.
  {ScheduleFixture f(5);constexpr uint32_t mask=0x00108421;
    f.scheduler.begin({1,1},mask,100,10000,100);
    for(unsigned i=0;i<5;++i)f.status(i*5,Phase::Prepared,Reason::None,1,150);
    f.tick(150);
    for(unsigned i=0;i<5;++i)f.status(i*5,Phase::Opening,Reason::None,2,160);
    f.prove(160);CHECK(!f.io.on);f.tick(460);CHECK(f.io.on);
    const uint32_t nextLeaseAt[]={1460,1660,1860,2060,1260};
    for(unsigned i=0;i<5;++i)CHECK(f.scheduler.node(i*5).nextLeaseAt==nextLeaseAt[i]);
  }
  // Same-clock field replay: total startup 2680ms, first lease proof age 1283ms.
  {ScheduleFixture f;f.scheduler.begin({1,1},1,100,10000,100);
    f.status(0,Phase::Prepared,Reason::None,1,150);f.tick(150);
    const auto first=f.scheduler.node(0).firstStartIssuedAt;CHECK(first==150);
    f.status(0,Phase::Opening,Reason::None,2,first+1397);f.tick(first+1397);
    const auto record=f.scheduler.node(0).leases[0];CHECK(record.issuedAt==first+1397);
    CHECK(f.scheduler.leaseAck(2,{1,1},record.sequence,Reply::Ok,first+2680));
    f.tick(first+2680);CHECK(f.io.on && first+2680-record.issuedAt==1283);
  }
  for(uint32_t age:{3999U,4000U}) {
    ScheduleFixture f;f.scheduler.begin({1,1},1,100,10000,100);
    f.status(0,Phase::Prepared,Reason::None,1,150);f.tick(150);
    const auto first=f.scheduler.node(0).firstStartIssuedAt;
    f.status(0,Phase::Opening,Reason::None,2,first+3000);f.tick(first+3000);
    const auto record=f.scheduler.node(0).leases[0];
    CHECK(f.scheduler.leaseAck(2,{1,1},record.sequence,Reply::Ok,first+age)==(age<4000));
    f.tick(first+age);CHECK(f.io.on==(age<4000));
    if(age==4000) CHECK(std::string(f.scheduler.guard())=="BOOTSTRAP_TIMEOUT");
  }
  // Exercise ring indexing independently of the physical task's 240-second runtime bound.
  ScheduleFixture f;f.scheduler.begin({1,1},1,100,10000,100);
  f.status(0,Phase::Prepared,Reason::None,1,150);f.tick(150);
  f.status(0,Phase::Opening,Reason::None,2,160);f.tick(160);
  for(uint32_t seq=1000;seq<1300;++seq) {
    auto p=packet(Op::Lease,2);p.context={1,1};Writer w{p};w.u32(1);w.u32(seq);
    f.net.pending.push_back({p,200+(seq-1000)*10,0,true});
    f.net.pending.push_back({p,200+(seq-1000)*10,0,false});
    f.scheduler.submissions(200+(seq-1000)*10);
    CHECK(f.scheduler.node(0).leaseCursor<10);
  }
  for(uint32_t seq=1290;seq<1300;++seq) {
    bool found=false;for(const auto &v:f.scheduler.node(0).leases) if(v.sequence==seq)found=true;
    CHECK(found);
  }
  CHECK(!f.scheduler.targetClosed(99,{1,1},3200));
  f.scheduler.stop(Reason::Manual,3200);CHECK(!f.io.on);
  CHECK(!f.scheduler.commandReply(2,{1,1},Op::Start,Reply::Ok,3201));
  CHECK(!f.scheduler.compactLeaseAck(2,1,1299,Reply::Ok,3201));
  CHECK(f.scheduler.targetClosed(2,{1,1},3201) && !f.io.on && !f.scheduler.finalMask());
}
void speed_calibration_storage_and_preflight() {
  CalibrationStore saved, decoded;
  CHECK(decodeCalibrations(&saved,sizeof(saved),decoded));
  CHECK(!decoded.nodes[0].valid());
  SpeedCalibration c; c.address=2; c.baseK=1000; c.baseVersion=1; c.revision=1;
  c.low=4000; c.medium=5000; c.high=6000;
  saved.nodes[0]=c;
  CHECK(decodeCalibrations(&saved,sizeof(saved),decoded));
  CHECK(decoded.nodes[0].matches(2,1000,1));
  CHECK(!decoded.nodes[0].matches(3,1000,1) && !decoded.nodes[0].matches(2,1000,2));
  CHECK(!decoded.nodes[0].matches(2,2000,1));
  CHECK(!decodeCalibrations(&saved,sizeof(saved)-1,decoded));
  saved.nodes[0].low=0; CHECK(!decodeCalibrations(&saved,sizeof(saved),decoded));
  saved.nodes[0]=c; saved.format=2; CHECK(!decodeCalibrations(&saved,sizeof(saved),decoded));
  for(uint8_t level=1;level<=3;++level) {
    ScheduleFixture f;
    CHECK(f.pump.setSpeed(level,100)==Reply::Ok);
    CHECK(f.scheduler.setCalibration(0,c)==Reply::Ok);
    CHECK(f.scheduler.check(1,500,60000,100)==Reply::Ok);
    f.scheduler.begin({1,1},1,500,60000,100); f.net.now=150; f.scheduler.tick(150);
    Packet probe; bool found=false;
    for(const auto &p:f.net.sent) if(p.op==Op::CapabilitiesGet) { probe=p; found=true; }
    CHECK(found && !f.io.on); Reader r{probe};
    CHECK(f.scheduler.capabilities(2,r.u32(),ProtocolRevision,RequiredFeatures|JobCalibrationFeature,TestSlaveBuildId,150));
    for(unsigned step=0;step<4;++step) { f.net.now=200+step*100; f.scheduler.tick(f.net.now); }
    bool prepared=false;
    for(const auto &p:f.net.sent) if(p.op==Op::Prepare) {
      Reader d{p}; CHECK(d.key()==JobKey{1,1}); CHECK(d.u32()==500);
      CHECK(d.u32()==c.coefficient(level)/2); CHECK(d.u32()==c.coefficient(level));
      CHECK(d.u32()==1 && d.u32()==60000);
      CHECK(d.u16()==(level==1?500:level==2?750:1000)); CHECK(d.u32()==LeaseMs);
      CHECK(d.u8()==(PrepareCalibrated | PrepareLiveTelemetry) && d.u32()>0 && d.done()); prepared=true;
    }
    CHECK(prepared && !f.io.on && f.scheduler.setCalibration(0,c)==Reply::Busy);
  }
  ScheduleFixture legacy; CHECK(legacy.scheduler.setCalibration(0,c)==Reply::Ok);
  legacy.scheduler.begin({1,1},1,500,60000,100); legacy.tick(150);
  CHECK(legacy.scheduler.stopReason()==Reason::Protocol && !legacy.io.on);
  CHECK(legacy.net.count(Op::Prepare)==0 && legacy.net.count(Op::Start)==0);
}
void single_slave_removal_waits_for_exact_events_and_durable_cleanup() {
  using Stage = MeshRemoval::Stage;
  MeshRemoval removal;
  removal.begin(2, 42, 100, true);
  removal.remoteReply(3, true); CHECK(removal.stage() == Stage::Remote);
  removal.tick(5099); CHECK(removal.stage() == Stage::Remote);
  removal.remoteReply(2, true); CHECK(removal.stage() == Stage::DeleteReady && removal.remoteReset());
  removal.deleting(5100);
  removal.deleted(3, true); CHECK(removal.stage() == Stage::Deleting);
  removal.complete(true); CHECK(removal.stage() == Stage::Deleting);
  removal.deleted(2, true); CHECK(removal.stage() == Stage::Cleanup);
  removal.complete(false); CHECK(removal.stage() == Stage::Failed);

  // Timeout retains the DevKey and registry, including across counter wrap.
  removal.begin(10, 43, UINT32_MAX - 1000, true);
  removal.tick(3998); CHECK(removal.stage() == Stage::Remote);
  removal.tick(3999); CHECK(removal.stage() == Stage::Pending && !removal.remoteReset());
  removal.deleting(4000); removal.deleted(10, true); removal.complete(true);
  CHECK(removal.stage() == Stage::Pending);
  removal.remoteReply(10, true); CHECK(removal.stage() == Stage::Pending);

  removal.begin(10, 44, 0, true); removal.remoteReply(10, false);
  CHECK(removal.stage() == Stage::Pending);
  removal.begin(10, 45, 10, true); removal.remoteReply(10, true); removal.deleting(100);
  removal.tick(10099); CHECK(removal.stage() == Stage::Deleting);
  removal.tick(10100); CHECK(removal.stage() == Stage::Failed);
  removal.deleted(10, true); CHECK(removal.stage() == Stage::Failed);
  // A restart after SDK deletion resumes only the durable registry cleanup.
  removal.begin(10, 0, 0, false); CHECK(removal.stage() == Stage::Cleanup);
  removal.complete(true); CHECK(removal.stage() == Stage::Done && !removal.remoteReset());
}
void unregister_only_the_selected_slave_and_keep_other_nodes() {
  ScheduleFixture f;
  SpeedCalibration c; c.address=10; c.baseK=1000; c.baseVersion=1; c.revision=1;
  c.low=1100; c.medium=1200; c.high=1300;
  CHECK(f.scheduler.setCalibration(8,c) == Reply::Ok);
  CHECK(f.scheduler.unregisterNode(MaxNodes,10) == Reply::Invalid);
  CHECK(f.scheduler.unregisterNode(8,2) == Reply::Invalid);
  CHECK(f.scheduler.node(8).config.address == 10);
  CHECK(f.scheduler.unregisterNode(8,10) == Reply::Ok);
  CHECK(f.scheduler.find(10) == -1 && !f.scheduler.node(8).calibration.valid());
  f.scheduler.hello(10,1000,1,true,200,TestSlaveBuildId);
  CHECK(!f.scheduler.node(8).seen && !f.scheduler.node(8).commissioned);
  CHECK(f.scheduler.node(0).config.address == 2 && f.scheduler.node(0).commissioned);
  CHECK(f.scheduler.node(31).config.address == 33 && f.scheduler.node(31).commissioned);
  f.scheduler.begin({1,1},1,100,10000,100);
  CHECK(f.scheduler.unregisterNode(31,33) == Reply::Busy);
  CHECK(f.scheduler.node(31).config.address == 33);
}
void removed_result_addresses_survive_restart_and_slot_reuse() {
  ResultAddresses saved, restored; saved.key = {7, 3};
  saved.nodes[0] = 2; saved.nodes[31] = 33;
  CHECK(decodeResultAddresses(&saved, sizeof(saved), restored));
  CHECK(restored.address({7,3}, 0, 0) == 2);
  CHECK(restored.address({7,3}, 31, 100) == 33);
  CHECK(restored.address({7,4}, 31, 100) == 100);
  CHECK(!decodeResultAddresses(&saved, sizeof(saved)-1, restored));
  saved.nodes[0] = 0x8000; CHECK(!decodeResultAddresses(&saved, sizeof(saved), restored));
  saved.nodes[0] = 2; saved.magic = 0; CHECK(!decodeResultAddresses(&saved, sizeof(saved), restored));
}
int main() {
  result_commit_and_reconciliation();
  maintenance_tokens_and_deadlines();
  mesh_config_retry_policy();
  single_slave_removal_waits_for_exact_events_and_durable_cleanup();
  unregister_only_the_selected_slave_and_keep_other_nodes();
  removed_result_addresses_survive_restart_and_slot_reuse();
  std::puts("PASS single-slave removal, offline/wrap/recovery, exact callbacks, cleanup failure and busy safeguards");
  speed_calibration_storage_and_preflight();
  std::puts("PASS per-speed K storage validation, slot identity, three PREPARE vectors and legacy preflight rejection");
  v4_exact_boundaries_and_ledger();
  std::puts("PASS V4 early StartOK/rejection, sparse-slot phases, field 2680ms/1283ms, 3999/4000 boundary, 300 ledger commits and cancel late proofs");
  struct Test { const char *name; void (*run)(); } tests[] = {
    {"output-only pump and commissioning",pump_output_only_and_commissioning}, {"pump authorization and replay",pump_authorization_and_replay},
    {"pump stop cooldown and timer wrap",pump_stop_cooldown_and_wrap}, {"pump runtime and fault reset",pump_runtime_and_fault_reset},
    {"scheduler valve before pump",scheduler_order_and_completion}, {"cancel retries and bounded timeout",scheduler_cancel_retries_and_timeout},
    {"valve lead, staggered batch, duplicate ACK, cancellation and fresh proof",scheduler_valve_open_lead_and_cancellation},
    {"manual calibration stop during startup",scheduler_calibration_can_stop_before_open_ack},
    {"manual calibration capability, mark ACK and terminal result",scheduler_manual_calibration_ack_and_terminal_result},
    {"scheduler faults and message ordering",scheduler_faults_and_ordering}, {"32-slot history preserved, sixth-node run rejected",thirty_two_nodes_and_batches},
    {"lease acknowledgement and timeout",lease_acknowledgements_and_timeout},
    {"stale lease acknowledgement stops pump as lease failure",stale_lease_ack_stops_pump_as_lease_failure},
    {"command rejection and reboot reconciliation",rejected_commands_and_late_terminal_reconciliation},
    {"telemetry cannot control outputs",telemetry_cannot_control_outputs}, {"request parsing and replay prevention",request_parser_and_replay},
    {"reboot volume remains unknown",reboot_volume_is_unknown},
    {"bounded independent pump test",bounded_pump_test}, {"test wrap limits and cancellation replay",bounded_pump_test_wrap_and_limits},
    {"PWM idle selection and bounded switching",pwm_idle_and_bounded_switching},
    {"PWM failure and authorization deadline",pwm_failure_and_authorization},
    {"phone tests do not relock dosing",phone_test_does_not_relock_dosing},
    {"dry test wrap failures and replay",dry_test_wrap_failure_and_replay},
    {"individual command parsing and durable replay",individual_request_and_durable_replay},
    {"individual targets in one autonomous job",individual_scheduler_prepares_each_slot_and_retains_global_limit},
    {"format 1 to 3 storage migration",persistent_v1_migration_preserves_identity_results_and_plan_replay},
    {"lease proof age and exact protection boundary",lease_proof_age_and_boundary_matrix},
    {"early ACK commit rejection and fixed bootstrap",early_ack_commit_and_rejection_matrix},
    {"capability nonce source revision features and timeout",capability_identity_and_incompatibility_matrix},
    {"capability cache build identity and master restart",capability_cache_build_identity_and_restart},
    {"lease cross round and timer wrap",lease_cross_round_and_timer_wrap},
    {"format 2 to 3 stop reason and late Target",stop_reason_migration_and_late_target},
    {"wire protocol contract vectors",wire_contract_vectors}
  };
  for (auto &t : tests) { t.run(); std::printf("PASS %s\n",t.name); }
  std::printf("%zu test groups passed\n",sizeof(tests)/sizeof(*tests)); return 0;
}
