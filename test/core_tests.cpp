#include "control_request.h"
#include "master_scheduler.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <limits>
using namespace pump;
#define CHECK(...) do { if (!(__VA_ARGS__)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__); std::exit(1); } } while (false)

struct FakePump : PumpIO {
  bool on = false;
  unsigned starts = 0;
  uint8_t duty = 0; bool pwmOk = true;
  bool setDuty(uint8_t v) override { duty = v; return pwmOk; }
  bool healthy() const override { return pwmOk; }
  void pump(bool v) override { on = v; if (v) ++starts; }
};
PumpConfig pumpConfig() {
  PumpConfig c; c.commissioned = true;
  c.maxRunMs = 5000; c.minOffMs = 50; return c;
}
struct FakeMesh : MeshSender {
  std::vector<Packet> sent;
  FakePump *pump = nullptr;
  bool send(const Packet &p) override {
    if (p.op == Op::Close && pump) CHECK(!pump->on);
    sent.push_back(p); return true;
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
  PumpSafetyController pump{io,pumpConfig()};
  MasterScheduler scheduler;
  explicit ScheduleFixture(uint8_t concurrent = 1) : scheduler(net,pump,concurrent) {
    net.pump = &io; pump.boot(0); CHECK(pump.reset(0) == Reply::Ok);
    for (size_t i=0;i<MaxNodes;++i) { CHECK(scheduler.registerNode(i,{uint16_t(i+2),1000,1}) == Reply::Ok); scheduler.hello(uint16_t(i+2),1000,1,true,100); }
  }
  void status(size_t i, Phase phase, Reason reason = Reason::None, uint32_t revision = 1, uint32_t now = 200) {
    ValveStatus s; s.key = {1,1}; s.k = 1000; s.phase = phase; s.reason = reason; s.revision = revision;
    s.valveCommand = phase == Phase::Opening || phase == Phase::Running; s.quiet = phase == Phase::Done;
    if (phase == Phase::Done) s.pulses = 100;
    scheduler.status(uint16_t(i+2),s,now);
  }
};
void scheduler_order_and_completion() {
  ScheduleFixture f; CHECK(f.scheduler.check(1,100,10000,100) == Reply::Ok);
  f.scheduler.begin({1,1},1,100,10000,100); f.scheduler.tick(150); CHECK(f.net.count(Op::Prepare) == 1 && !f.io.on);
  f.status(0,Phase::Prepared); f.scheduler.tick(200); CHECK(!f.io.on);
  f.scheduler.tick(350); CHECK(f.net.count(Op::Start) == 1 && !f.io.on);
  f.status(0,Phase::Opening,Reason::None,2,400); f.scheduler.tick(400); CHECK(f.io.on);
  f.status(0,Phase::Settling,Reason::Target,3,450); f.scheduler.tick(450); CHECK(!f.io.on);
  f.status(0,Phase::Done,Reason::Target,4,950); f.pump.tick(950); f.scheduler.tick(950);
  CHECK(f.scheduler.report().phase == SchedulePhase::Complete && f.scheduler.report().complete == 1);
}
void scheduler_cancel_retries_and_timeout() {
  ScheduleFixture f; f.scheduler.begin({1,1},1,100,10000,100); f.scheduler.stop(Reason::Manual,200);
  CHECK(f.net.count(Op::Close) == 1);
  for (uint32_t t=205;t<10300;t+=5) { if (t % 100 == 0) f.scheduler.stop(Reason::Manual,t); f.pump.tick(t); f.scheduler.tick(t); }
  CHECK(f.net.count(Op::Close) >= 10);
  CHECK(!f.scheduler.active() && f.scheduler.report().unknown == 1 && !f.io.on);
}
void scheduler_faults_and_ordering() {
  ScheduleFixture f; f.scheduler.begin({1,1},1,100,10000,100);
  f.status(0,Phase::Prepared); f.scheduler.tick(200); f.status(0,Phase::Opening,Reason::None,2,250); f.scheduler.tick(250); CHECK(f.io.on);
  f.status(0,Phase::Prepared,Reason::None,1,260); CHECK(f.scheduler.node(0).status.phase == Phase::Opening);
  f.status(0,Phase::Settling,Reason::NoFlow,3,300); CHECK(!f.io.on && f.net.count(Op::Close) == 1);
  f.status(0,Phase::Fault,Reason::NoFlow,4,800); f.pump.tick(800); f.scheduler.tick(800);
  CHECK(f.scheduler.report().phase == SchedulePhase::Failed);
}
void thirty_two_nodes_and_batches() {
  ScheduleFixture f; CHECK(f.scheduler.check(0xffffffff,100,10000,100) == Reply::Ok);
  CHECK(f.scheduler.registerNode(32,{99,1000,1}) == Reply::Capacity);
  CHECK(f.scheduler.registerNode(1,{2,1000,1}) == Reply::Conflict);
  f.scheduler.begin({1,1},0xffffffff,100,10000,100);
  uint32_t now = 100;
  for (size_t i=0;i<32;++i) {
    f.status(i,Phase::Prepared,Reason::None,1,now); f.scheduler.tick(now);
    f.status(i,Phase::Done,Reason::Target,3,now+10); f.scheduler.tick(now+10); f.scheduler.tick(now+20);
    now += 50;
    if (i < 31) f.scheduler.tick(now);
  }
  CHECK(f.scheduler.report().phase == SchedulePhase::Complete && f.scheduler.report().complete == 0xffffffff);
  CHECK(f.io.starts == 0); // Already delivered by residual pressure: never start the pump.
}
void telemetry_cannot_control_outputs() {
  ScheduleFixture f; f.scheduler.begin({1,1},1,100,10000,100); f.status(0,Phase::Prepared);
  CHECK(!f.scheduler.summary(2,{2,1},100,1,210));
  CHECK(f.scheduler.summary(2,{1,1},100,1,210));
  CHECK(!f.scheduler.summary(2,{1,1},50,1,220));
  f.scheduler.tick(220); CHECK(f.scheduler.report().phase == SchedulePhase::Opening && !f.io.on);
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
  f.status(0,Phase::Prepared,Reason::None,17,150); f.scheduler.tick(150);
  f.status(0,Phase::Opening,Reason::None,18,160); f.scheduler.tick(160); CHECK(f.io.on);
  // Remote reboot report, independent of the slave controller implementation.
  ValveStatus reboot; reboot.key = {1,1}; reboot.k = 1000; reboot.revision = 33;
  reboot.phase = Phase::Fault; reboot.reason = Reason::Reboot; reboot.truncated = true;
  f.scheduler.status(2,reboot,200);
  f.status(0,Phase::Running,Reason::None,19,201);
  f.pump.tick(200); f.scheduler.tick(200);
  CHECK(f.scheduler.report().phase == SchedulePhase::Failed && f.scheduler.report().unknown == 1);
  CHECK(f.scheduler.node(0).status.reason == Reason::Reboot && !f.io.on);
  CHECK(f.scheduler.volume().measured == 0);
}
void bounded_pump_test() {
  FakePump io; auto cfg = pumpConfig(); cfg.maxRunMs = 20000;
  PumpSafetyController c(io, cfg); c.boot(0);
  CHECK(!c.testReady(49) && c.testReady(50) && !io.on);
  CHECK(c.start({1,1},1,50) == Reply::NotReady);
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
  ControlRequest q; q.client = 1; q.sequence = 5; q.epoch = 7; q.maxMs = 3000;
  ClientRecord cancelled{1,5,0,0,0,{7,0}};
  CHECK(replayDecision(cancelled,q) == Reply::Conflict);
  ClientRecord accepted{1,5,0,0,3000,{7,1}};
  CHECK(replayDecision(accepted,q) == Reply::Ok);
}
void pwm_idle_and_bounded_switching() {
  FakePump io; PumpSafetyController c(io,pumpConfig()); c.boot(0);
  CHECK(!io.on && io.duty == 50 && c.speed() == 1);
  CHECK(c.setSpeed(2,0) == Reply::Ok && !io.on && c.fault() == PumpFault::None);
  CHECK(c.reset(0) == Reply::Ok);
  CHECK(c.setSpeed(0,1) == Reply::Invalid && c.setSpeed(4,1) == Reply::Invalid);
  CHECK(c.setSpeed(2,1) == Reply::Ok && io.duty == 75 && !io.on && io.starts == 0);
  CHECK(c.startTest({1,1},1,3000,50) == Reply::Ok && io.on);
  CHECK(c.setSpeed(3,1500) == Reply::Ok && io.duty == 100 && io.starts == 1);
  CHECK(c.testRemaining(1500) == 1550);
  CHECK(c.setSpeed(1,3000) == Reply::Ok && io.duty == 50 && io.starts == 1);
  c.tick(3050); CHECK(!io.on && !c.testing() && c.speed() == 1);
  CHECK(c.setSpeed(2,3051) == Reply::Ok && !io.on);
  CHECK(c.startTest({1,2},1,3000,3100) == Reply::Ok && io.duty == 75);
  c.stop(3150); CHECK(!io.on && c.speed() == 2);
  c.boot(3200); CHECK(!io.on && c.speed() == 1 && io.duty == 50);
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
  CHECK(io.on && io.duty == 50 && c.speed() == 1);
  CHECK(c.setSpeed(3,500) == Reply::Busy && io.duty == 50);
  CHECK(c.setSpeed(1,1000) == Reply::Ok && c.testRemaining(1000) == 2050);
  CHECK(c.renew({7,1},2,1000) == Reply::Stale);
  c.tick(3049); CHECK(io.on); c.tick(3050);
  CHECK(!io.on && !c.dryTesting() && c.fault() == PumpFault::None);
  CHECK(!c.testReady(3099) && c.testReady(3100) && c.ready(3100));
  CHECK(c.start({7,2},1,3100) == Reply::Ok);
  c.stop(3150);
  CHECK(c.setSpeed(2,3150) == Reply::Ok);
  CHECK(c.startTest({7,2},1,3000,3200) == Reply::Ok && io.duty == 75);
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
  CHECK(uint8_t(Op::Prepare) == 0xC0 && uint8_t(Op::HelloReply) == 0xCB);
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
}
int main() {
  struct Test { const char *name; void (*run)(); } tests[] = {
    {"output-only pump and commissioning",pump_output_only_and_commissioning}, {"pump authorization and replay",pump_authorization_and_replay},
    {"pump stop cooldown and timer wrap",pump_stop_cooldown_and_wrap}, {"pump runtime and fault reset",pump_runtime_and_fault_reset},
    {"scheduler valve before pump",scheduler_order_and_completion}, {"cancel retries and bounded timeout",scheduler_cancel_retries_and_timeout},
    {"scheduler faults and message ordering",scheduler_faults_and_ordering}, {"32 logical nodes and sequential batches",thirty_two_nodes_and_batches},
    {"telemetry cannot control outputs",telemetry_cannot_control_outputs}, {"request parsing and replay prevention",request_parser_and_replay},
    {"reboot volume remains unknown",reboot_volume_is_unknown},
    {"bounded independent pump test",bounded_pump_test}, {"test wrap limits and cancellation replay",bounded_pump_test_wrap_and_limits},
    {"PWM idle selection and bounded switching",pwm_idle_and_bounded_switching},
    {"PWM failure and authorization deadline",pwm_failure_and_authorization},
    {"phone tests do not relock dosing",phone_test_does_not_relock_dosing},
    {"dry test wrap failures and replay",dry_test_wrap_failure_and_replay},
    {"wire protocol contract vectors",wire_contract_vectors}
  };
  for (auto &t : tests) { t.run(); std::printf("PASS %s\n",t.name); }
  std::printf("%zu test groups passed\n",sizeof(tests)/sizeof(*tests)); return 0;
}
