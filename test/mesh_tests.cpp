#include "master_scheduler.h"
#include "flow_job_controller.h"
#include "valve_mesh_session.h"
#include "mesh_tx_diagnostics.h"
#include <cstdio>
#include <cstdlib>
#include <array>
#include <memory>
#include <vector>
#include <string>
using namespace pump;
constexpr uint32_t TestSlaveBuildId = 0x12345678U;
#define CHECK(...) do { if (!(__VA_ARGS__)) { std::fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#__VA_ARGS__); std::exit(1); } } while(false)
Packet keyed(Op op,uint16_t peer,JobKey job={1,1},uint32_t seq=1) {
  auto p=packet(op,peer); p.context=job; Writer w{p};
  if (op==Op::Lease || op==Op::LeaseAck) { w.u32(job.epoch); w.u32(seq); }
  else w.key(job);
  if (op==Op::Close) w.u8(uint8_t(Reason::Manual));
  return p;
}
Packet sarPacket(uint16_t peer=2) {
  auto p=keyed(Op::Prepare,peer); while(p.size<39) { Writer w{p}; w.u8(0); } return p;
}
Packet helloPacket(uint16_t peer) {
  auto p=packet(Op::Hello,peer); Writer w{p}; w.u32(5880); w.u32(1); w.u8(1); w.u32(0); return p;
}
void drain(MeshTxGovernor &g,uint32_t now) { MeshSubmission r; while(g.collect(now,r)) {} }
void governor_matrix() {
  for (bool endFirst:{false,true}) {
    MeshTxGovernor g; MeshTxGovernor::Dispatch d;
    CHECK(g.enqueue(sarPacket(),100) && g.next(100,d));
    g.event(d.ticket,endFirst?3:1,0,110); drain(g,110); CHECK(g.inFlight()==1);
    g.event(d.ticket,endFirst?1:3,0,120); drain(g,120); CHECK(g.inFlight()==0 && g.metrics().ended==1);
    g.event(d.ticket,3,-9,122); CHECK(g.healthy());
  }
  for (int error:{-12,-16,-22,-105}) {
    MeshTxGovernor g; MeshTxGovernor::Dispatch d;
    CHECK(g.enqueue(keyed(Op::Lease,2),100) && g.next(100,d));
    g.event(d.ticket,2,error,101); drain(g,101); CHECK(!g.healthy() && !g.inFlight());
  }
  for (bool accepted:{false,true}) {
    MeshTxGovernor g; MeshTxGovernor::Dispatch d;
    CHECK(g.enqueue(sarPacket(),100) && g.next(100,d));
    if (accepted) g.event(d.ticket,1,0,101);
    drain(g,102); CHECK(!g.next(6100,d) && !g.healthy() && g.inFlight()==1);
  }
  {
    MeshTxGovernor g; MeshTxGovernor::Dispatch a,b,c;
    CHECK(g.enqueue(sarPacket(),0) && g.next(0,a)); g.event(a.ticket,1,0,0); drain(g,0);
    auto other=sarPacket(3); CHECK(g.enqueue(other,1) && !g.next(1,b));
    // Compact authorization, success replies and closure bypass existing SAR, including same peer.
    for (auto op:{Op::Start,Op::Lease,Op::LeaseAck,Op::PrepareOK,Op::StartOK,Op::TargetClosed,Op::TargetClosedAck}) {
      CHECK(g.enqueue(keyed(op,2),2) && g.next(2,b) && !MeshTxGovernor::segmented(b.packet));
      g.event(b.ticket,1,0,3); drain(g,3); CHECK(g.inFlight()==1);
    }
    g.event(a.ticket,3,0,100); drain(g,100); CHECK(g.next(101,c) && g.metrics().peakSar==1);
  }
  {
    MeshTxGovernor g; uint32_t floor; MeshTxGovernor::Dispatch d;
    CHECK(g.beginLeaseScope({1,1},floor) && floor==1);
    CHECK(g.enqueue(keyed(Op::Lease,2),100) && g.next(100,d));
    CHECK(leaseSequence(d.packet)==1);
    CHECK(g.enqueue(keyed(Op::Start,2),101));
    CHECK(g.beginLeaseScope({1,2},floor) && floor==2 && !g.queued() && g.inFlight()==1);
    CHECK(!g.enqueue(keyed(Op::Lease,2),102)); // stale scope cannot commit after floor
    for(unsigned peer=2;peer<7;++peer) {
      CHECK(g.enqueue(keyed(Op::Lease,uint16_t(peer),{1,2}),103));
      MeshTxGovernor::Dispatch n; CHECK(g.next(103,n) && leaseSequence(n.packet)==peer);
      g.event(n.ticket,1,0,104); drain(g,104);
    }
    g.fence({1,2}); CHECK(!g.enqueue(keyed(Op::Start,2,{1,2}),105));
    CHECK(g.enqueue(keyed(Op::Close,2,{1,2}),105));
  }
  {
    MeshTxGovernor g; MeshTxGovernor::Dispatch d; auto p=keyed(Op::StartOK,1);
    CHECK(g.enqueue(p,0) && g.next(0,d)); CHECK(g.enqueue(p,1) && !g.queued());
    g.event(d.ticket,1,0,2); drain(g,2); CHECK(g.enqueue(p,3) && g.queued()==1);
    g.retire(1,{1,1},Op::StartOK); CHECK(!g.queued());
    CHECK(g.enqueue(keyed(Op::Start,2),0) && g.enqueue(keyed(Op::Start,3),0));
    g.retire(2,{1,1},Op::Start); CHECK(g.queued()==1); // no whole-job fence
  }
  {
    MeshTxGovernor g; MeshTxGovernor::Dispatch d;
    CHECK(g.enqueue(keyed(Op::Lease,2),100));
    for(uint32_t t=110;t<8000;t+=10) CHECK(g.enqueue(keyed(Op::Lease,2),t));
    CHECK(g.queued()==1 && !g.next(8100,d) && !g.healthy());
  }
  {
    MeshTxGovernor g; auto p=packet(Op::CapabilitiesGet,2); Writer w{p}; w.u32(1);
    for(unsigned i=0;i<48;++i) { p.peer=uint16_t(i+2); CHECK(g.enqueue(p,0)); }
    p.peer=99; CHECK(!g.enqueue(p,0) && g.healthy());
    for(unsigned i=0;i<16;++i) CHECK(g.enqueue(keyed(Op::Start,uint16_t(100+i)),0));
    CHECK(!g.enqueue(keyed(Op::Lease,199),0) && !g.healthy());
  }
  std::puts("PASS governor real callbacks, missing completion, compact SAR bypass, global scope/floor, dedup, retirement, TTL and capacity");
}
struct Pump : PumpIO { bool on=false; unsigned starts=0; void pump(bool v) override {on=v; if(v)++starts;} bool setDuty(uint8_t) override{return true;} };
struct Link : MeshSender {
  MeshTxGovernor tx{2}; uint32_t now=0;
  bool send(const Packet &p) override {return tx.enqueue(p,now);}
  bool takeSubmission(MeshSubmission &r) override {return tx.collect(now,r);}
  void fence(JobKey k) override {tx.fence(k);}
  bool healthy() const override {return tx.healthy();}
  bool beginLeaseScope(JobKey k,uint32_t &floor) override {return tx.beginLeaseScope(k,floor);}
  MeshTxGovernor::Admission prepare(const Packet &p) override {return tx.prepare(p,now);}
  void retire(uint16_t peer,JobKey k,Op op) override {tx.retire(peer,k,op);}
};
struct IO : ValveIO {
  bool on=false; unsigned opens=0; FlowReading reading; JobKey fence;
  FlowReading flow() override{return reading;} void valve(bool v) override{on=v;if(v)++opens;}
  bool interlock() override{return true;} bool saveFence(JobKey k) override{fence=k;return true;}
};
Packet heartbeat() {
  auto p=packet(Op::Hello,1); Writer w{p}; w.u32(5880); w.u32(1); w.u8(1);
  w.u32(TestSlaveBuildId); return p;
}
Packet terminal(JobKey key={1,1}) {
  auto p=packet(Op::Result,1); Writer w{p}; w.key(key); w.u32(12); w.u32(12);
  w.u32(5880); w.u32(3); w.u8(uint8_t(Phase::Fault)); w.u8(uint8_t(Reason::Transport)); w.u8(2);
  return p;
}
void governor_recovery_matrix() {
  using ReasonT=MeshTxGovernor::FaultReason;
  // Reject or real end, in either callback order: keep business locked, clear
  // stale intents, then send only a new heartbeat/closed state after backoff.
  for (bool rejected:{false,true}) for (bool endFirst:{false,true}) {
    MeshTxGovernor g{1}; MeshTxGovernor::Dispatch d,next;
    CHECK(g.enqueue(terminal(),100) && g.next(100,d));
    CHECK(g.enqueue(keyed(Op::Lease,2),101) && g.enqueue(keyed(Op::Start,2),101));
    if(rejected) g.event(d.ticket,2,-12,110);
    else {
      g.event(d.ticket,endFirst?3:1,endFirst?-110:0,110);
      if(!endFirst) g.event(d.ticket,3,-110,120);
    }
    const auto at=rejected || endFirst ? 110U : 120U;
    CHECK(!g.healthy() && g.queued()==0);
    CHECK(!g.enqueue(heartbeat(),at+999));
    CHECK(!g.next(at+1000,next)); // end-before-accepted still owns its ticket
    if(!rejected && endFirst) g.event(d.ticket,1,0,130);
    drain(g,140);
    const auto fault=g.diagnostics(at+1000).first;
    CHECK(g.inFlight()==0 && fault.ticket==d.ticket && fault.packet.op==Op::Result);
    CHECK(fault.reason==(rejected?ReasonT::Rejected:ReasonT::SarEnd));
    CHECK(fault.at==at && fault.issuedAt==100 && fault.inFlight==1 && fault.pending==2);
    CHECK(!g.enqueue(keyed(Op::Start,2,{1,2}),at+1000));
    CHECK(!g.enqueue(keyed(Op::LeaseAck,1),at+1000)); // no OK proof in recovery
    auto open=terminal(); open.bytes[26]|=1; CHECK(!g.enqueue(open,at+1000));
    open.bytes[26]=2; open.bytes[24]=uint8_t(Phase::Prepared); CHECK(!g.enqueue(open,at+1000));
    CHECK(g.enqueue(terminal(),at+1000) && g.enqueue(heartbeat(),at+1000));
    CHECK(g.next(at+1000,next) && next.packet.op==Op::Hello && next.ticket>d.ticket);
    CHECK(!g.next(at+1001,d)); // exactly one recovery transmission at a time
    g.event(next.ticket,1,0,at+1001); g.event(next.ticket,3,0,at+1010); drain(g,at+1010);
    CHECK(g.next(at+1011,next) && next.packet.op==Op::Result && !(next.packet.bytes[26]&1));
    g.event(next.ticket,1,0,at+1012); g.event(next.ticket,3,0,at+1020); drain(g,at+1020);
    CHECK(!g.healthy() && g.inFlight()==0 && g.queued()==0 && g.metrics().faults==1);
    std::vector<std::string> lines;
    publishMeshDiagnostics(g.diagnostics(at+1020),g.healthy(),[&](const char *line){lines.emplace_back(line);CHECK(lines.back().size()<192);});
    CHECK(lines.size()==3 && lines[1].find(rejected?"reason=REJECTED":"reason=SAR_END")!=std::string::npos);
    CHECK(g.diagnostics(at+1020).first.ticket==fault.ticket);
  }
  {
    MeshTxGovernor g{1}; MeshTxGovernor::Dispatch d;
    CHECK(g.enqueue(keyed(Op::LeaseAck,1),110));
    CHECK(!g.next(8110,d) && !g.healthy());
    CHECK(g.diagnostics(9110).first.reason==ReasonT::QueueExpired && g.inFlight()==0);
    CHECK(!g.enqueue(heartbeat(),9109));
    CHECK(g.enqueue(heartbeat(),9110) && g.next(9110,d));
    CHECK(g.diagnostics(9110).first.packet.op==Op::LeaseAck && !g.healthy());
  }
  for (bool missingAccept:{false,true}) {
    MeshTxGovernor g{1}; MeshTxGovernor::Dispatch d,next;
    CHECK(g.enqueue(terminal(),100) && g.next(100,d));
    if(missingAccept) g.event(d.ticket,3,-110,110); else g.event(d.ticket,1,0,110);
    drain(g,110); CHECK(!g.next(6100,next));
    auto state=g.diagnostics(20000);
    CHECK(state.restartRequired && !state.diagnosticReady && g.inFlight()==1);
    // Even a late completion cannot silently undo the maintenance requirement.
    g.event(d.ticket,missingAccept?1:3,0,20001); drain(g,20001);
    CHECK(g.inFlight()==0 && !g.enqueue(heartbeat(),30000) && !g.next(30000,next));
    CHECK(g.diagnostics(30000).last.reason==ReasonT::MissingCompletion);
  }
  {
    MeshTxGovernor g; MeshTxGovernor::Dispatch d;
    const uint32_t at=UINT32_MAX-500;
    CHECK(g.enqueue(terminal(),at-1) && g.next(at-1,d)); g.event(d.ticket,2,-12,at); drain(g,at);
    CHECK(!g.enqueue(heartbeat(),at+999) && g.enqueue(heartbeat(),at+1000) && g.next(at+1000,d));
    const auto original=g.diagnostics(at+1000).first.ticket;
    uint32_t failed=at+1001, delay=2000;
    for(unsigned i=0;i<5;++i) {
      g.event(d.ticket,2,-16,failed); drain(g,failed);
      CHECK(g.diagnostics(failed).backoffMs==delay);
      CHECK(!g.enqueue(heartbeat(),failed+delay-1));
      CHECK(g.enqueue(heartbeat(),failed+delay) && g.next(failed+delay,d));
      CHECK(g.diagnostics(failed+delay).first.ticket==original);
      failed+=delay+1; if(delay<8000)delay*=2;
    }
  }
  {
    MeshTxGovernor g; MeshTxGovernor::Dispatch d;
    auto p=heartbeat(); CHECK(g.enqueue(p,100));
    CHECK(!g.next(2100,d) && g.healthy() && g.diagnostics(2100).first.reason==ReasonT::None);
  }
  std::puts("PASS recovery: immutable first fault, cleared business intents, closed-only reports, callback order, 2777ms SAR, missing completion lock, bounded backoff and timer wrap");
}
struct Slave {
  IO io; FlowJobController valve; ValveMeshSession session; MeshTxGovernor tx{1};
  uint32_t acked=UINT32_MAX,sent=UINT32_MAX,at=0,helloAt=0;
  JobKey ackedKey,sentKey; unsigned attempts=0;
  explicit Slave(ValveConfig cfg):valve(io,cfg),session(valve){valve.boot({});}
  bool report(uint32_t now,bool force=false) {
    const auto s=valve.status();
    if ((s.phase!=Phase::Done && s.phase!=Phase::Fault) || (!force && ackedKey==s.key && acked==s.revision)) return false;
    auto p=packet(Op::Result,1); Writer w{p}; w.key(s.key); w.u32(s.pulses); w.u32(s.closePulses);
    w.u32(s.k); w.u32(s.revision); w.u8(uint8_t(s.phase)); w.u8(uint8_t(s.reason));
    w.u8((s.valveCommand?1:0)|(s.quiet?2:0)|(s.truncated?4:0));
    if (!tx.enqueue(p,now)) { CHECK(!tx.healthy()); return false; }
    return true;
  }
  void commandAck(JobKey key,Op command,Reply result,uint32_t now) {
    auto p=packet(Op::CommandAck,1); Writer w{p}; w.key(key); w.u8(uint8_t(command)); w.u8(uint8_t(result));
    CHECK(tx.enqueue(p,now) || !tx.healthy());
  }
};
struct Flight {
  Packet p; uint32_t ticket,deliverAt,endAt; int from; bool delivered=false;
  uint32_t acceptedAt=0; bool accepted=false,ended=false,drop=false,rejected=false;
};
struct Bench {
  Pump io; PumpConfig cfg; PumpSafetyController pump; Link link; MasterScheduler scheduler;
  std::array<std::unique_ptr<Slave>,5> slaves;
  std::vector<Flight> flights;
  uint32_t now=100,seed=1; unsigned count; bool dropAck=false,dropResultAck=false,missingEnd=false,duplicate=false;
  bool dropResultsUntilQuery=false; unsigned summaryFrames=0,resultQueries=0,resultAcks=0;
  unsigned progressFrames=0,progressApplied=0;
  static constexpr uint32_t SlowSarMs=3200;
  int slow=-1; uint32_t sarMs=65, ackDelay=0;
  uint32_t startReplyDelay=0, acceptedDelay=0, prepareLife=0, jitter=0, pulsesPerTick=3;
  bool rejectStart=false, rejectLease=false, randomLoss=false, dropClosed=false,dropClosedAck=false;
  uint16_t supportedFeatures=RequiredFeatures;
  unsigned preparations=0,leaseApplied=0,lossCount=0,lossBudget=1; uint32_t oldestApplied=0;
  std::array<bool,256> lost{}; uint32_t lastLease[5]={};
  uint32_t firstStart=0,pumpStartedAt=0,lastClosedAt=0;
  uint32_t random(){seed=seed*1664525U+1013904223U;return seed;}
  static PumpConfig config(){PumpConfig c;c.commissioned=true;c.minOffMs=50;c.maxRunMs=240000;return c;}
  explicit Bench(unsigned n):cfg(config()),pump(io,cfg),scheduler(link,pump,uint8_t(n)),count(n) {
    pump.boot(0);
    ValveConfig vc;vc.commissioned=true;
    for(unsigned i=0;i<n;++i){slaves[i]=std::make_unique<Slave>(vc);CHECK(scheduler.registerNode(i,{uint16_t(i+2),5880,1})==Reply::Ok);scheduler.hello(uint16_t(i+2),5880,1,true,now,TestSlaveBuildId);}
  }
  void enqueue(unsigned i,Packet p){CHECK(slaves[i]->tx.enqueue(p,now) || !slaves[i]->tx.healthy());}
  void deliver(const Flight &f){
    const auto &p=f.p; Reader r{p};
    if(f.from<0){
      for(unsigned i=0;i<count;++i){
        if(p.peer!=i+2 && p.peer!=AllSlaves)continue;
        auto incoming=p; incoming.peer=1;
        auto &s=*slaves[i]; Reader v{p};
        if(p.op==Op::CapabilitiesGet){auto reply=packet(Op::Capabilities,1);Writer w{reply};w.u32(v.u32());w.u16(ProtocolRevision);w.u16(supportedFeatures);w.u32(TestSlaveBuildId);enqueue(i,reply);}
        else if(s.session.handle(incoming,now,s.tx.healthy(),true,[&](const Packet &out){ return s.tx.enqueue(out,now); },[&](uint16_t peer,JobKey key,Op op){s.tx.retire(peer,key,op);})) {
          if(duplicate && p.op==Op::Start) s.session.handle(incoming,now,s.tx.healthy(),true,[&](const Packet &out){return s.tx.enqueue(out,now);},[&](uint16_t peer,JobKey key,Op op){s.tx.retire(peer,key,op);});
        }
        else if(p.op==Op::Close){const auto key=v.key();s.valve.close(key,Reason(v.u8()),now);}
        else if(p.op==Op::ResultQuery){++resultQueries;dropResultsUntilQuery=false;s.report(now,true);}
        else if(p.op==Op::ResultAck){const auto key=v.key();const auto revision=v.u32();const auto current=s.valve.status();if(key==current.key && revision==current.revision){s.acked=revision;s.ackedKey=key;s.session.resultAck(1,key,[&](uint16_t peer,JobKey k,Op op){s.tx.retire(peer,k,op);});++resultAcks;}}
        else if(p.op==Op::Summary)++summaryFrames;
      }
    }else{
      const uint16_t source=uint16_t(f.from+2); CHECK(p.peer==1);
      if(p.op==Op::Capabilities){const auto nonce=r.u32();const auto rev=r.u16(),features=r.u16();scheduler.capabilities(source,nonce,rev,features,r.u32(),now);}
      else if(p.op==Op::PrepareOK || p.op==Op::StartOK){scheduler.commandReply(source,r.key(),p.op==Op::PrepareOK?Op::Prepare:Op::Start,Reply::Ok,now);}
      else if(p.op==Op::TargetClosed){const auto k=r.key();if(scheduler.targetClosed(source,k,now)){CHECK(link.send(keyed(Op::TargetClosedAck,source,k)) || !link.healthy());}}
      else if(p.op==Op::CommandAck){const auto key=r.key();const auto command=Op(r.u8());const auto result=Reply(r.u8());if(r.done())scheduler.commandReply(source,key,command,result,now);}
      else if(p.op==Op::Result){ValveStatus s;s.key=r.key();s.pulses=r.u32();s.closePulses=r.u32();s.k=r.u32();s.revision=r.u32();s.phase=Phase(r.u8());s.reason=Reason(r.u8());const auto flags=r.u8();s.valveCommand=flags&1;s.quiet=flags&2;s.truncated=flags&4;if(dropResultsUntilQuery)return;if(scheduler.result(source,s,now) && !dropResultAck){auto ack=keyed(Op::ResultAck,source,s.key);Writer w{ack};w.u32(s.revision);CHECK(link.send(ack) || !link.healthy());}}
      else if(p.op==Op::LeaseAck && !dropAck){const auto epoch=r.u32();const auto seq=r.u32();if(scheduler.compactLeaseAck(source,epoch,seq,p.size==8?Reply::Ok:Reply(r.u8()),now)) {
        ++leaseApplied; const auto age=now-scheduler.node(unsigned(f.from)).leaseAckAt;
        if(age>oldestApplied) oldestApplied=age;
      }}
      else if(p.op==Op::Progress){const auto key=r.key();const auto pulses=r.u32(),runtime=r.u32();if(r.done() && scheduler.progress(source,key,pulses,runtime,now))++progressApplied;}
      else if(p.op==Op::Hello){const auto k=r.u32(),version=r.u32();const bool commissioned=r.u8()==1;const auto build=r.u32();CHECK(r.done());scheduler.hello(source,k,version,commissioned,now,build);}
    }
  }
  void dispatch(MeshTxGovernor &g,int from){
    MeshTxGovernor::Trace trace;while(g.takeTrace(trace)){}
    MeshTxGovernor::Dispatch d;if(!g.next(now,d))return;
    if(d.packet.op==Op::Summary)++summaryFrames;
    if(from>=0 && d.packet.op==Op::Progress)++progressFrames;
    const uint32_t delay=20+(random()%5)*5+(jitter?random()%(jitter+1):0);
    uint32_t life=(from==slow && slow>=0)?SlowSarMs:sarMs;
    if(from<0 && d.packet.op==Op::Prepare && prepareLife) life=prepareLife;
    if(from<0 && d.packet.op==Op::Prepare) ++preparations;
    if(from<0 && d.packet.op==Op::Start && !firstStart) firstStart=now;
    if(from<0 && d.packet.op==Op::Lease) {
      const unsigned peer=d.packet.peer-2; CHECK(peer<count);
      if(lastLease[peer]) CHECK(elapsed(now,lastLease[peer],1000));
      lastLease[peer]=now;
    }
    const auto op=d.packet.op;
    const bool reject=from<0 && ((rejectStart && op==Op::Start) || (rejectLease && op==Op::Lease));
    bool drop=(dropClosed && op==Op::TargetClosed) || (dropClosedAck && op==Op::TargetClosedAck);
    // At most one loss per opcode per batch in the supported random envelope.
    if(randomLoss && lossCount<lossBudget && !lost[uint8_t(op)] && random()%9==0) {lost[uint8_t(op)]=true;++lossCount;drop=true;}
    const uint32_t extra=op==Op::LeaseAck?ackDelay:op==Op::StartOK?startReplyDelay:0;
    flights.push_back({d.packet,d.ticket,now+delay+extra,now+life,from,false,
      now+((from<0 && (op==Op::Start || op==Op::Lease))?acceptedDelay:0),false,false,drop||reject,reject});
    CHECK(flights.size()<128);
  }
  void step(){
    now+=5;link.now=now;pump.tick(now);scheduler.submissions(now);
    for(unsigned i=0;i<count;++i){
      auto &s=*slaves[i]; if(io.on && s.io.on){s.io.reading.pulses+=pulsesPerTick;s.io.reading.lastMs=now;}
      s.valve.tick(now);
      if(!s.valve.status().valveCommand)s.tx.retire(1,s.valve.status().key,Op::Progress);
      s.session.poll(1,i,now,[&](const Packet &p){return s.tx.enqueue(p,now);});drain(s.tx,now);
      if(!s.tx.healthy())s.valve.emergency(Reason::Transport,now);
      const auto v=s.valve.status();
      if(s.sentKey!=v.key || s.sent!=v.revision){s.sentKey=v.key;s.sent=v.revision;s.at=now-1000;s.attempts=0;}
      if(v.phase==Phase::Done || v.phase==Phase::Fault){if(s.sentKey!=v.key || s.sent!=v.revision){s.sentKey=v.key;s.sent=v.revision;s.at=now-1000;s.attempts=0;}if((s.ackedKey!=v.key || s.acked!=v.revision) && s.attempts<6 && elapsed(now,s.at,1000+(i+2)*13)){if(s.report(now)){s.at=now;++s.attempts;}}}
      if(elapsed(now,s.helloAt,10000)){auto p=packet(Op::Hello,1);Writer w{p};w.u32(5880);w.u32(1);w.u8(1);w.u32(TestSlaveBuildId);enqueue(i,p);s.helloAt=now;}
    }
    for(size_t j=0;j<flights.size();){
      auto &f=flights[j];
      auto &governor=f.from<0?link.tx:slaves[unsigned(f.from)]->tx;
      if(!f.accepted && int32_t(now-f.acceptedAt)>=0) {
        f.accepted=true; governor.event(f.ticket,f.rejected?2:1,f.rejected?-12:0,now);
      }
      if(!f.delivered && int32_t(now-f.deliverAt)>=0){f.delivered=true;const auto copy=f;if(!f.drop)deliver(copy);}
      if(!f.ended && int32_t(now-f.endAt)>=0){
        f.ended=true;if(!missingEnd)governor.event(f.ticket,3,0,now);
      }
      if(f.delivered && f.accepted && f.ended){flights.erase(flights.begin()+j);continue;}
      ++j;
    }
    scheduler.tick(now);
    if(io.on && !pumpStartedAt) {
      pumpStartedAt=now; CHECK(firstStart && now-firstStart<4000);
      for(unsigned i=0;i<count;++i) {
        const auto &n=scheduler.node(i); const auto v=slaves[i]->valve.status();
        if(v.valveCommand) CHECK(n.startIssued && n.startAckSeen && n.leaseAckSeen && now-n.leaseAckAt<2000);
      }
    }
    dispatch(link.tx,-1);
    for(unsigned i=0;i<count;++i)dispatch(slaves[i]->tx,int(i));
  }
  void begin(uint32_t id){lossCount=0;lost.fill(false);firstStart=pumpStartedAt=0;for(auto &t:lastLease)t=0;link.now=now;CHECK(scheduler.check((1U<<count)-1,500,60000,now)==Reply::Ok);scheduler.begin({1,id},(1U<<count)-1,500,60000,now);}
  void run(uint32_t duration){const auto end=now+duration;while(int32_t(end-now)>0)step();}
  void finish(){const auto start=now;while(scheduler.active() && !elapsed(now,start,40000))step();CHECK(!scheduler.active() && !io.on);run(1000);}
  void clean(){
    const auto start=now;
    for(;;){
      bool quiet=link.tx.queued()==0 && link.tx.inFlight()==0;
      for(unsigned i=0;i<count;++i){const auto &s=*slaves[i];const auto v=s.valve.status();quiet=quiet && s.tx.queued()==0 && s.tx.inFlight()==0 && (s.attempts==6 || (s.ackedKey==v.key && s.acked==v.revision));}
      if(quiet)break;
      CHECK(!elapsed(now,start,20000));step();
    }
    CHECK(link.tx.healthy());for(unsigned i=0;i<count;++i)CHECK(slaves[i]->tx.healthy());
  }
};
void joint_runs(){
  for(unsigned n:{1U,2U,5U}){
    Bench b(n);
    for(unsigned id=1;id<=1000;++id){
      b.dropResultAck=id%3==0;b.duplicate=true;b.begin(id);b.finish();
      if(b.scheduler.report().phase!=SchedulePhase::Complete) {
        const auto r=b.scheduler.report();
        std::fprintf(stderr,"FAILED n=%u id=%u now=%u phase=%u done=%x failed=%x unknown=%x txhealthy=%u reason=%u\n",n,id,b.now,unsigned(r.phase),r.complete,r.failed,r.unknown,b.link.tx.healthy(),unsigned(b.scheduler.stopReason()));
        for(unsigned i=0;i<n;++i){const auto s=b.slaves[i]->valve.status();std::fprintf(stderr,"slave %u phase=%u reason=%u pulses=%u healthy=%u\n",i,unsigned(s.phase),unsigned(s.reason),s.pulses,b.slaves[i]->tx.healthy());}
      }
      CHECK(b.scheduler.report().phase==SchedulePhase::Complete && b.scheduler.report().complete==((1U<<n)-1));
      for(unsigned i=0;i<n;++i){const auto s=b.slaves[i]->valve.status();CHECK(s.reason==Reason::Target && s.quiet && s.pulses>=2940 && b.slaves[i]->io.opens==id);}
      b.clean();
    }
    CHECK(b.link.tx.metrics().peakSar<=2);
    CHECK(b.summaryFrames==0);
    std::printf("PASS %u nodes x 1000 batches, 500 mL/2940 pulse simulation, duplicate Start/drop ResultAck, no SUMMARY\n",n);
  }
  for(unsigned mode=0;mode<3;++mode){
    Bench b(5);b.begin(1);
    if(mode==0)b.dropAck=true;
    if(mode==1)b.missingEnd=true;
    if(mode==2){b.run(1800);b.scheduler.stop(Reason::Manual,b.now);CHECK(!b.io.on);}
    b.finish();
    CHECK(b.scheduler.report().phase==SchedulePhase::Failed && !b.io.on);
    for(unsigned i=0;i<5;++i)CHECK(!b.slaves[i]->io.on);
  }
  std::puts("PASS five-node lease ACK loss/missing SAR end/manual stop: local stop and remote closure");
  {
    Bench b(1); b.dropResultsUntilQuery=true; b.begin(1); b.finish();
    CHECK(b.scheduler.report().phase==SchedulePhase::Complete && b.scheduler.report().complete==1 &&
      b.scheduler.report().unknown==0 && b.resultQueries>0 && b.resultAcks>=1 && b.summaryFrames==0);
  }
  Bench soak(5);uint32_t id=0;const auto start=soak.now;
  while(!elapsed(soak.now,start,8U*60*60*1000)){
    soak.begin(++id);soak.finish();CHECK(soak.scheduler.report().phase==SchedulePhase::Complete);soak.run(1000);soak.clean();
  }
  std::printf("PASS simulated 8h/5 nodes: %u completed batches, no accumulated tickets/queues (no RF or water test)\n",id);
}
void recovery_link_runs() {
  {
    Bench b(1); b.begin(1); bool injected=false; unsigned opens=0;
    for(unsigned step=0;step<2000 && !injected;++step) {
      b.step(); if(!b.io.on) continue;
      if(step%3==0) b.slaves[0]->tx.enqueue(heartbeat(),b.now);
      for(size_t i=0;i<b.flights.size();++i) {
        const auto f=b.flights[i];
        if(f.from!=0 || !MeshTxGovernor::segmented(f.p)) continue;
        // The dispatcher already accepted this ticket. Inject its real end
        // error; API rejection is covered separately by the governor matrix.
        if(!f.accepted)b.slaves[0]->tx.event(f.ticket,1,0,b.now);
        b.slaves[0]->tx.event(f.ticket,3,-110,b.now);
        b.flights.erase(b.flights.begin()+i); injected=true; opens=b.slaves[0]->io.opens; break;
      }
    }
    CHECK(injected && opens==1);
    const auto sent=b.slaves[0]->tx.metrics().submitted;
    b.step(); CHECK(!b.slaves[0]->io.on && b.slaves[0]->valve.transportFault());
    b.run(30000);
    CHECK(!b.io.on && !b.slaves[0]->io.on && b.slaves[0]->io.opens==opens);
    CHECK(b.scheduler.report().phase==SchedulePhase::Failed && b.scheduler.report().unknown==0);
    CHECK(b.scheduler.node(0).status.phase==Phase::Fault && !b.scheduler.node(0).status.valveCommand);
    CHECK(!elapsed(b.now,b.scheduler.node(0).seenAt,15000));
    CHECK(!b.slaves[0]->tx.healthy() && b.slaves[0]->tx.metrics().submitted>sent);
    CHECK(!b.slaves[0]->tx.enqueue(keyed(Op::Start,1,{1,2}),b.now));
  }
  {
    Bench b(1); b.begin(1); bool injected=false; unsigned starts=0,opens=0;
    for(unsigned step=0;step<2000 && !injected;++step) {
      b.step(); if(!b.io.on) continue;
      if(step%3==0) {auto diagnostic=terminal();diagnostic.peer=2;b.link.send(diagnostic);}
      for(size_t i=0;i<b.flights.size();++i) {
        const auto f=b.flights[i];
        if(f.from>=0 || !MeshTxGovernor::segmented(f.p)) continue;
        if(!f.accepted)b.link.tx.event(f.ticket,1,0,b.now);
        b.link.tx.event(f.ticket,3,-110,b.now);
        b.flights.erase(b.flights.begin()+i); injected=true;
        starts=b.io.starts; opens=b.slaves[0]->io.opens; break;
      }
    }
    CHECK(injected && starts==1 && opens==1);
    b.step(); CHECK(!b.io.on);
    b.run(30000);
    CHECK(!b.io.on && !b.slaves[0]->io.on && b.io.starts==starts && b.slaves[0]->io.opens==opens);
    CHECK(b.scheduler.report().phase==SchedulePhase::Failed && b.scheduler.report().unknown==0);
    CHECK(!elapsed(b.now,b.scheduler.node(0).seenAt,15000));
    CHECK(!b.link.tx.healthy() && b.link.tx.diagnostics(b.now).first.reason==MeshTxGovernor::FaultReason::SarEnd);
    CHECK(!b.link.tx.enqueue(keyed(Op::Start,2,{1,2}),b.now));
  }
  std::puts("PASS integrated fault recovery: stopped outputs, heartbeat online, late terminal reconciled, no new authorization");
}

#include "v4_tests.inc"
#include "live_tests.inc"
void calibrated_link_runs() {
  for(uint8_t level=1;level<=3;++level) {
    Bench b(5); b.supportedFeatures=RequiredFeatures|JobCalibrationFeature;
    CHECK(b.pump.setSpeed(level,b.now)==Reply::Ok);
    for(size_t i=0;i<5;++i) {
      SpeedCalibration c; c.address=uint16_t(i+2); c.baseK=5880; c.baseVersion=1; c.revision=1;
      c.low=4000+uint32_t(i)*100; c.medium=5000+uint32_t(i)*100; c.high=6000+uint32_t(i)*100;
      CHECK(b.scheduler.setCalibration(i,c)==Reply::Ok);
    }
    b.duplicate=true; b.dropResultAck=true; b.begin(1); b.finish();
    CHECK(b.scheduler.report().phase==SchedulePhase::Complete && b.summaryFrames==0);
    for(size_t i=0;i<5;++i) {
      const auto status=b.slaves[i]->valve.status();
      const auto k=(level==1?4000U:level==2?5000U:6000U)+uint32_t(i)*100;
      CHECK(status.k==k && status.pulses>=k/2 && status.reason==Reason::Target && status.quiet);
      CHECK(b.slaves[i]->valve.config().k==5880 && b.slaves[i]->valve.config().version==1);
    }
    b.clean();
  }
  Bench legacy(1); SpeedCalibration c; c.address=2; c.baseK=5880; c.baseVersion=1; c.revision=1;
  c.low=c.medium=c.high=5000; CHECK(legacy.scheduler.setCalibration(0,c)==Reply::Ok);
  legacy.begin(1); legacy.finish();
  CHECK(legacy.scheduler.stopReason()==Reason::Protocol && !legacy.pumpStartedAt && !legacy.preparations);
  std::puts("PASS calibrated three speeds x five nodes, final K snapshot, duplicate/drop ACK and old slave rejected before PREPARE");
}
int main(){progress_governor();live_link_runs();calibrated_link_runs();v4_matrix();governor_matrix();governor_recovery_matrix();recovery_link_runs();joint_runs();return 0;}
