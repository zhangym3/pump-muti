// Fault and timing tests use the firmware's actual scheduler, governor and session.
namespace pump {
struct GovernorTestAccess {
  static void sequence(MeshTxGovernor &g,uint32_t n){g.leaseSeq_=n;}
};
}
void v4_wire_and_session() {
  for(auto op:{Op::Start,Op::Lease,Op::PrepareOK,Op::StartOK,Op::TargetClosed,Op::TargetClosedAck}) {
    auto p=keyed(op,1); CHECK(controlFormat(p) && !MeshTxGovernor::segmented(p));
    for(unsigned n:{0U,7U,9U,12U,13U}){p.size=uint8_t(n);CHECK(!controlFormat(p));}
  }
  auto prep=sarPacket(1);CHECK(controlFormat(prep));prep.size=35;CHECK(!controlFormat(prep));
  auto nack=keyed(Op::CommandAck,1);Writer nw{nack};nw.u8(uint8_t(Op::Start));nw.u8(uint8_t(Reply::Ok));
  CHECK(!controlFormat(nack));nack.bytes[9]=uint8_t(Reply::Stale);CHECK(controlFormat(nack));
  ValveConfig cfg;cfg.commissioned=true;IO io;FlowJobController valve(io,cfg);valve.boot({});
  ValveMeshSession session(valve);MeshTxGovernor tx{1};
  auto send=[&](const Packet &p){return tx.enqueue(p,100);};
  auto retire=[&](uint16_t peer,JobKey k,Op op){tx.retire(peer,k,op);};
  Dose dose;dose.key={1,1};dose.ml=500;dose.pulses=2940;dose.k=5880;dose.version=1;dose.maxMs=60000;dose.leaseFloor=0x80000001U;
  CHECK(valve.prepare(dose,100)==Reply::Ok);
  CHECK(session.handle(keyed(Op::Start,1),110,true,true,send,retire));
  CHECK(valve.compactLease(1,0x80000000U,120)==Reply::Stale);
  CHECK(valve.compactLease(2,UINT32_MAX,120)==Reply::Stale);
  CHECK(session.handle(keyed(Op::Lease,1,{1,1},0x80000001U),1000,true,true,send,retire));
  CHECK(valve.compactLease(1,0x80000001U,12999)==Reply::Ok);
  CHECK(valve.compactLease(1,0x80000001U,13000)==Reply::Stale && !io.on);
  CHECK(valve.compactLease(1,UINT32_MAX,13001)==Reply::Stale && !io.on);
  valve.tick(13500);dose.key={1,2};dose.leaseFloor=0x80000002U;
  CHECK(valve.prepare(dose,14000)==Reply::Ok && valve.start(dose.key,14010)==Reply::Ok);
  CHECK(valve.compactLease(1,0x80000001U,14020)==Reply::Stale);
  CHECK(valve.compactLease(1,UINT32_MAX,14030)==Reply::Ok);
  CHECK(valve.compactLease(1,0x80000002U,14040)==Reply::Stale);
  // TargetClosed retries are bounded, ACK retirement does not manufacture a result.
  io.reading.pulses=2940;valve.tick(14050);CHECK(!io.on && valve.status().reason==Reason::Target);
  unsigned closed=0;auto count=[&](const Packet &p){CHECK(p.op==Op::TargetClosed);++closed;return true;};
  for(unsigned n=0;n<20;++n) session.poll(1,0,14050+n*250,count);
  CHECK(closed==6 && valve.status().phase==Phase::Settling);
  CHECK(session.handle(keyed(Op::TargetClosedAck,1,{1,2}),19051,true,true,send,retire));
  session.poll(1,0,20000,count);CHECK(closed==6);
  std::puts("PASS V4 strict wire lengths, high sequence/floor/epoch isolation, duplicate expiry and bounded closure ACK");
}
void v4_sequence_scope() {
  // A rejection supersedes a queued success without withdrawing a real ticket.
  for(auto op:{Op::Prepare,Op::Start}) {
    MeshTxGovernor tx;MeshTxGovernor::Dispatch d;
    const auto ok=op==Op::Prepare?Op::PrepareOK:Op::StartOK;
    CHECK(tx.enqueue(keyed(ok,1),100));
    auto nack=keyed(Op::CommandAck,1);Writer w{nack};w.u8(uint8_t(op));w.u8(uint8_t(Reply::Stale));
    CHECK(tx.enqueue(nack,101) && tx.queued()==1 && tx.next(102,d));
    CHECK(d.packet.op==Op::CommandAck);
  }
  for(bool commitFirst:{false,true}) {
    MeshTxGovernor g;uint32_t floor=0;MeshTxGovernor::Dispatch d;
    CHECK(g.beginLeaseScope({1,1},floor));CHECK(g.enqueue(keyed(Op::Lease,2),0));
    if(commitFirst) CHECK(g.next(1,d) && leaseSequence(d.packet)==1);
    CHECK(g.beginLeaseScope({1,2},floor) && floor==(commitFirst?2U:1U));
    CHECK(!g.enqueue(keyed(Op::Lease,2,{1,1}),2));
    CHECK(g.enqueue(keyed(Op::Lease,3,{1,2}),2) && g.next(2,d));
    CHECK(leaseSequence(d.packet)==floor);
  }
  MeshTxGovernor g;uint32_t floor;MeshTxGovernor::Dispatch d;
  GovernorTestAccess::sequence(g,UINT32_MAX-1);
  CHECK(g.beginLeaseScope({2,1},floor) && floor==UINT32_MAX);
  CHECK(g.enqueue(keyed(Op::Lease,2,{2,1}),100) && g.next(100,d) && leaseSequence(d.packet)==UINT32_MAX);
  g.event(d.ticket,1,0,101);drain(g,101);
  CHECK(!g.beginLeaseScope({2,2},floor));
  CHECK(g.enqueue(keyed(Op::Lease,2,{2,1}),200) && !g.next(200,d) && !g.healthy());
  CHECK(g.diagnostics(200).first.reason==MeshTxGovernor::FaultReason::SequenceExhausted);
  std::puts("PASS serialized scope/commit both orders, UINT32_MAX exhaustion without wrap or ticket release");
}
void v4_timing_link() {
  // Business delivery and SAR end are independent. Both callback orders are legal.
  for(uint32_t sar:{646U,1116U,1514U,3200U}) {
    Bench b(5);b.prepareLife=sar;b.sarMs=sar;b.jitter=100;b.begin(1);b.finish();
    CHECK(b.scheduler.report().phase==SchedulePhase::Complete && b.io.starts==1);
    CHECK(b.preparations==5 && b.link.tx.metrics().peakSar<=2);
    b.clean();
  }
  {Bench b(1);b.startReplyDelay=1300;b.ackDelay=1200;b.begin(1);b.finish();
    CHECK(b.scheduler.report().phase==SchedulePhase::Complete && b.pumpStartedAt-b.firstStart>=2500 && b.io.starts==1);}
  for(bool reject:{false,true}) {
    Bench b(5);b.acceptedDelay=300;b.rejectStart=reject;b.begin(1);b.finish();
    CHECK(b.io.starts==(reject?0U:1U));
    CHECK(b.scheduler.report().phase==(reject?SchedulePhase::Failed:SchedulePhase::Complete));
  }
  {Bench b(1);b.acceptedDelay=300;b.rejectLease=true;b.begin(1);b.finish();CHECK(!b.io.starts);}
  // Renewal ACKs arriving 4.5s late still match a retained record; keep flowing long enough.
  {Bench b(1);b.pulsesPerTick=1;b.begin(1);while(!b.io.on)b.step();b.ackDelay=4500;b.run(10000);
    CHECK(b.io.on && b.oldestApplied>=4500 && b.scheduler.node(0).leaseCursor<10);
    b.ackDelay=0;b.finish();CHECK(b.scheduler.report().phase==SchedulePhase::Complete);}
  for(unsigned mode=0;mode<4;++mode) {
    Bench b(5);b.dropClosed=mode==0;b.dropClosedAck=mode==1;b.dropResultAck=mode==2;b.dropResultsUntilQuery=mode==3;
    b.begin(1);b.finish();CHECK(b.scheduler.report().phase==SchedulePhase::Complete && b.scheduler.report().complete==31);
    CHECK(!b.scheduler.report().unknown && !b.scheduler.report().failed);b.clean();
  }
  // One lane closes early; the other four keep supplying. The last proof stops the pump immediately.
  {Bench b(5);b.begin(1);while(!b.io.on)b.step();auto &s=*b.slaves[0];s.io.reading.pulses=2940;
    b.run(100);CHECK(!s.io.on && b.io.on && b.scheduler.closedMask()==1 && !b.scheduler.finalMask());
    for(unsigned i=1;i<5;++i)b.slaves[i]->io.reading.pulses=2940;
    b.run(100);CHECK(!b.io.on && b.scheduler.closedMask()==31);b.finish();}
  unsigned ok=0;
  for(unsigned id=1;id<=1000;++id) {
    Bench b(5);b.seed=id*7919;b.jitter=150;b.randomLoss=true;b.duplicate=true;b.sarMs=646+id%3*435;
    b.begin(1);b.finish();
    if(b.scheduler.report().phase!=SchedulePhase::Complete) std::fprintf(stderr,"random id=%u guard=%s reason=%u starts=%u prepared=%u unknown=%x first=%u pump=%u now=%u\n",id,b.scheduler.guard(),unsigned(b.scheduler.report().reason),b.io.starts,b.preparations,b.scheduler.report().unknown,b.firstStart,b.pumpStartedAt,b.now);
    CHECK(b.scheduler.report().phase==SchedulePhase::Complete && b.io.starts==1);
    b.clean();++ok;
  }
  std::printf("PASS V4 independent accepted/delivery/SAR end, field latency, five long prepares, late ACK, closure loss and %u randomized five-node batches\n",ok);
}
void v4_matrix(){v4_wire_and_session();v4_sequence_scope();v4_timing_link();}
