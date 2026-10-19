#include "master_scheduler.h"
#include "flow_job_controller.h"
#include "mesh_tx_diagnostics.h"
#include <cstdio>
#include <cstdlib>
#include <array>
#include <memory>
#include <vector>
#include <string>
using namespace pump;
#define CHECK(...) do { if (!(__VA_ARGS__)) { std::fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#__VA_ARGS__); std::exit(1); } } while(false)
Packet keyed(Op op,uint16_t peer,JobKey job={1,1},uint32_t seq=1) {
  auto p=packet(op,peer); Writer w{p}; w.key(job);
  if (op==Op::Lease || op==Op::LeaseAck) w.u32(seq);
  if (op==Op::LeaseAck) w.u8(uint8_t(Reply::Ok));
  if (op==Op::Close) w.u8(uint8_t(Reason::Manual));
  return p;
}
void drain(MeshTxGovernor &g,uint32_t now) { MeshSubmission r; while(g.collect(now,r)) {} }
void governor_matrix() {
  for (unsigned order=0;order<2;++order) {
    MeshTxGovernor g; MeshTxGovernor::Dispatch d,other;
    CHECK(g.enqueue(keyed(Op::Lease,2),100) && g.next(100,d));
    CHECK(g.enqueue(keyed(Op::Lease,2),101) && !g.next(101,other));
    g.event(d.ticket,order?3:1,0,110); drain(g,110);
    CHECK(g.inFlight()==1 && !g.next(111,other));
    g.event(d.ticket,order?1:3,0,120); g.event(d.ticket,3,0,120); drain(g,120);
    CHECK(g.inFlight()==0 && g.metrics().ended==1);
    CHECK(g.next(121,other) && other.ticket!=d.ticket);
    g.event(d.ticket,3,-9,122); CHECK(g.healthy()); // late old ticket cannot free a new one
  }
  for (int error:{-12,-16,-22,-105}) {
    MeshTxGovernor g; MeshTxGovernor::Dispatch d;
    CHECK(g.enqueue(keyed(Op::Lease,2),100) && g.next(100,d));
    g.event(d.ticket,2,error,101); drain(g,101);
    CHECK(!g.healthy() && g.inFlight()==0 && g.metrics().faults==1);
  }
  for (bool accepted:{false,true}) {
    MeshTxGovernor g; MeshTxGovernor::Dispatch d;
    CHECK(g.enqueue(keyed(Op::Lease,2),100) && g.next(100,d));
    if(accepted) g.event(d.ticket,1,0,101);
    drain(g,102); CHECK(!g.next(6100,d));
    CHECK(!g.healthy() && g.inFlight()==1); // missing end / mesh reset never fake releases
  }
  {
    MeshTxGovernor g; MeshTxGovernor::Dispatch d;
    CHECK(g.enqueue(keyed(Op::Lease,2),100) && g.next(100,d));
    g.event(d.ticket,1,0,101); g.event(d.ticket,3,-110,102); drain(g,102);
    CHECK(!g.healthy() && g.inFlight()==0);
  }
  {
    MeshTxGovernor g; MeshTxGovernor::Dispatch a,b,c;
    CHECK(g.enqueue(keyed(Op::Start,2),0)); // Real START is 8 bytes, unsegmented.
    auto ordinary=keyed(Op::Prepare,2); Writer w{ordinary}; w.u32(1);
    CHECK(g.enqueue(ordinary,0)); ordinary.peer=3; CHECK(g.enqueue(ordinary,0));
    CHECK(g.next(0,a) && a.packet.op==Op::Start); g.event(a.ticket,1,0); drain(g,0);
    CHECK(g.next(1,a) && a.packet.op==Op::Prepare); g.event(a.ticket,1,0);
    CHECK(!g.next(2,b)); // ordinary SAR reserves the second context
    CHECK(g.enqueue(keyed(Op::Lease,3),2) && g.next(2,b));
    CHECK(!g.next(3,c) && g.metrics().peakSar==2);
    g.event(a.ticket,3,0,10); g.event(b.ticket,1,0,10); g.event(b.ticket,3,0,10); drain(g,10);
    CHECK(g.inFlight()==0 && g.next(11,c));
  }
  {
    MeshTxGovernor g; MeshTxGovernor::Dispatch d;
    CHECK(g.enqueue(keyed(Op::Start,2),100) && g.enqueue(keyed(Op::Lease,2),100));
    CHECK(g.next(101,d)); // commit may legally precede stop
    g.fence({1,1}); CHECK(g.queued()==0 && !g.enqueue(keyed(Op::Start,2),102));
    CHECK(!g.next(102,d)); CHECK(g.enqueue(keyed(Op::Close,AllSlaves),102));
    CHECK(g.next(102,d) && d.packet.op==Op::Close);
  }
  {
    MeshTxGovernor g; MeshTxGovernor::Dispatch d;
    CHECK(g.enqueue(keyed(Op::Lease,2),100));
    for(uint32_t t=110;t<800;t+=10) CHECK(g.enqueue(keyed(Op::Lease,2),t));
    CHECK(g.queued()==1 && !g.next(900,d) && !g.healthy()); // merge never slides deadline
  }
  {
    MeshTxGovernor g; MeshTxGovernor::Dispatch d;
    auto p=packet(Op::Status,1); Writer w{p}; w.key({1,1}); w.u32(4); w.u32(0); w.u32(5880); w.u32(2); w.u8(2); w.u8(0); w.u8(1);
    CHECK(g.enqueue(p,100)); p.bytes[8]=7; CHECK(g.enqueue(p,110));
    p.bytes[20]=3; p.bytes[24]=uint8_t(Phase::Fault); CHECK(g.enqueue(p,120));
    CHECK(g.queued()==1 && g.next(121,d) && d.packet.bytes[20]==3 && d.packet.bytes[24]==uint8_t(Phase::Fault));
  }
  {
    MeshTxGovernor g; auto p=keyed(Op::Summary,2);
    for(unsigned i=0;i<48;++i) { p.peer=uint16_t(i+2); CHECK(g.enqueue(p,0)); }
    p.peer=99; CHECK(!g.enqueue(p,0) && g.healthy());
    for(unsigned i=0;i<16;++i) CHECK(g.enqueue(keyed(Op::Start,uint16_t(100+i)),0));
    CHECK(!g.enqueue(keyed(Op::Lease,199),0) && !g.healthy());
  }
  std::puts("PASS governor callback/order/error/window/merge/deadline/fence/capacity matrix");
}
struct Pump : PumpIO { bool on=false; unsigned starts=0; void pump(bool v) override {on=v; if(v)++starts;} bool setDuty(uint8_t) override{return true;} };
struct Link : MeshSender {
  MeshTxGovernor tx{2}; uint32_t now=0;
  bool send(const Packet &p) override {return tx.enqueue(p,now);}
  bool takeSubmission(MeshSubmission &r) override {return tx.collect(now,r);}
  void fence(JobKey k) override {tx.fence(k);}
  bool healthy() const override {return tx.healthy();}
};
struct IO : ValveIO {
  bool on=false; unsigned opens=0; FlowReading reading; JobKey fence;
  FlowReading flow() override{return reading;} void valve(bool v) override{on=v;if(v)++opens;}
  bool interlock() override{return true;} bool saveFence(JobKey k) override{fence=k;return true;}
};
Packet heartbeat() {
  auto p=packet(Op::Hello,1); Writer w{p}; w.u32(5880); w.u32(1); w.u8(1); return p;
}
Packet terminal(JobKey key={1,1}) {
  auto p=packet(Op::Status,1); Writer w{p}; w.key(key); w.u32(12); w.u32(12);
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
    CHECK(g.inFlight()==0 && fault.ticket==d.ticket && fault.packet.op==Op::Status);
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
    CHECK(g.next(at+1011,next) && next.packet.op==Op::Status && !(next.packet.bytes[26]&1));
    g.event(next.ticket,1,0,at+1012); g.event(next.ticket,3,0,at+1020); drain(g,at+1020);
    CHECK(!g.healthy() && g.inFlight()==0 && g.queued()==0 && g.metrics().faults==1);
    std::vector<std::string> lines;
    publishMeshDiagnostics(g.diagnostics(at+1020),g.healthy(),[&](const char *line){lines.emplace_back(line);CHECK(lines.back().size()<192);});
    CHECK(lines.size()==3 && lines[1].find(rejected?"reason=REJECTED":"reason=SAR_END")!=std::string::npos);
    CHECK(g.diagnostics(at+1020).first.ticket==fault.ticket);
  }
  {
    MeshTxGovernor g{1}; MeshTxGovernor::Dispatch d,blocked;
    CHECK(g.enqueue(terminal(),100) && g.next(100,d)); g.event(d.ticket,1,0,101); drain(g,101);
    CHECK(g.enqueue(keyed(Op::LeaseAck,1),110));
    CHECK(!g.next(910,blocked) && !g.healthy());
    CHECK(g.diagnostics(1910).first.reason==ReasonT::QueueExpired && g.inFlight()==1);
    CHECK(!g.enqueue(heartbeat(),1910)); // backoff alone never releases SAR
    g.event(d.ticket,3,0,2877); drain(g,2877);
    CHECK(g.enqueue(heartbeat(),2877) && g.next(2877,blocked));
    CHECK(g.diagnostics(2877).first.packet.op==Op::LeaseAck && !g.healthy());
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
  IO io; FlowJobController valve; MeshTxGovernor tx{1};
  uint32_t acked=UINT32_MAX,sent=UINT32_MAX,at=0,summaryAt=0,helloAt=0;
  JobKey ackedKey,sentKey; unsigned attempts=0;
  explicit Slave(ValveConfig cfg):valve(io,cfg){valve.boot({});}
  void status(uint32_t now,bool force=false) {
    auto s=valve.status(); if (!force && ackedKey==s.key && acked==s.revision) return;
    auto p=packet(Op::Status,1); Writer w{p}; w.key(s.key); w.u32(s.pulses); w.u32(s.closePulses);
    w.u32(s.k); w.u32(s.revision); w.u8(uint8_t(s.phase)); w.u8(uint8_t(s.reason));
    w.u8((s.valveCommand?1:0)|(s.quiet?2:0)|(s.truncated?4:0)); CHECK(tx.enqueue(p,now) || !tx.healthy());
  }
};
struct Flight { Packet p; uint32_t ticket,deliverAt,endAt; int from; bool delivered=false; };
struct Bench {
  Pump io; PumpConfig cfg; PumpSafetyController pump; Link link; MasterScheduler scheduler;
  std::array<std::unique_ptr<Slave>,5> slaves;
  std::vector<Flight> flights;
  uint32_t now=100,seed=1; unsigned count; bool dropAck=false,dropEvent=false,missingEnd=false,duplicate=false;
  int slow=-1; uint32_t sarMs=65, ackDelay=0;
  static PumpConfig config(){PumpConfig c;c.commissioned=true;c.minOffMs=50;c.maxRunMs=240000;return c;}
  explicit Bench(unsigned n):cfg(config()),pump(io,cfg),scheduler(link,pump,uint8_t(n)),count(n) {
    pump.boot(0);
    ValveConfig vc;vc.commissioned=true;
    for(unsigned i=0;i<n;++i){slaves[i]=std::make_unique<Slave>(vc);CHECK(scheduler.registerNode(i,{uint16_t(i+2),5880,1})==Reply::Ok);scheduler.hello(uint16_t(i+2),5880,1,true,now);}
  }
  void enqueue(unsigned i,Packet p){CHECK(slaves[i]->tx.enqueue(p,now) || !slaves[i]->tx.healthy());}
  void deliver(const Flight &f){
    const auto &p=f.p; Reader r{p};
    if(f.from<0){
      for(unsigned i=0;i<count;++i){
        if(p.peer!=i+2 && p.peer!=AllSlaves)continue;
        auto &s=*slaves[i]; Reader v{p};
        if(p.op==Op::CapabilitiesGet){auto reply=packet(Op::Capabilities,1);Writer w{reply};w.u32(v.u32());w.u16(ProtocolRevision);w.u16(RequiredFeatures);w.u32(ApprovedSlaveBuildIds[0]);enqueue(i,reply);}
        else if(p.op==Op::Prepare){Dose d;d.key=v.key();d.ml=v.u32();d.pulses=v.u32();d.k=v.u32();d.version=v.u32();d.maxMs=v.u32();CHECK(s.valve.prepare(d,now)==Reply::Ok);s.status(now);}
        else if(p.op==Op::Start){const auto key=v.key();CHECK(s.valve.start(key,now)==Reply::Ok);if(duplicate)CHECK(s.valve.start(key,now)==Reply::Ok);s.status(now);}
        else if(p.op==Op::Lease){const auto key=v.key();const auto seq=v.u32();const auto current=s.valve.status();if(current.key==key && !current.valveCommand && current.reason==Reason::Target && (current.phase==Phase::Settling || current.phase==Phase::Done)){s.status(now,true);continue;}const auto result=s.valve.lease(key,seq,now);auto ack=keyed(Op::LeaseAck,1,key,seq);ack.bytes[12]=uint8_t(result);enqueue(i,ack);}
        else if(p.op==Op::Close){const auto key=v.key();s.valve.close(key,Reason(v.u8()),now);s.status(now);}
        else if(p.op==Op::StatusGet)s.status(now,true);
        else if(p.op==Op::EventAck && !dropEvent){const auto key=v.key();const auto rev=v.u32();if(key==s.valve.status().key && rev==s.valve.status().revision){s.acked=rev;s.ackedKey=key;}}
      }
    }else{
      const uint16_t source=uint16_t(f.from+2);
      if(p.op==Op::Capabilities){const auto nonce=r.u32();const auto rev=r.u16(),features=r.u16();scheduler.capabilities(source,nonce,rev,features,r.u32(),now);}
      else if(p.op==Op::Status){ValveStatus s;s.key=r.key();s.pulses=r.u32();s.closePulses=r.u32();s.k=r.u32();s.revision=r.u32();s.phase=Phase(r.u8());s.reason=Reason(r.u8());const auto flags=r.u8();s.valveCommand=flags&1;s.quiet=flags&2;s.truncated=flags&4;scheduler.status(source,s,now);auto ack=keyed(Op::EventAck,source,s.key);Writer w{ack};w.u32(s.revision);CHECK(link.send(ack) || !link.healthy());}
      else if(p.op==Op::LeaseAck && !dropAck){const auto key=r.key();const auto seq=r.u32();scheduler.leaseAck(source,key,seq,Reply(r.u8()),now);}
      else if(p.op==Op::Hello)scheduler.hello(source,5880,1,true,now);
    }
  }
  void dispatch(MeshTxGovernor &g,int from){
    MeshTxGovernor::Trace trace;while(g.takeTrace(trace)){}
    MeshTxGovernor::Dispatch d;if(!g.next(now,d))return;
    g.event(d.ticket,1,0,now);
    seed=seed*1664525U+1013904223U;
    const uint32_t delay=20+(seed%5)*5;
    uint32_t life=(from==slow && slow>=0)?900:sarMs;
    flights.push_back({d.packet,d.ticket,now+delay+(d.packet.op==Op::LeaseAck?ackDelay:0),now+life,from,false});
    CHECK(flights.size()<128);
  }
  void step(){
    now+=5;link.now=now;pump.tick(now);scheduler.submissions(now);
    for(unsigned i=0;i<count;++i){
      auto &s=*slaves[i]; if(io.on && s.io.on){s.io.reading.pulses+=3;s.io.reading.lastMs=now;}
      s.valve.tick(now);drain(s.tx,now);
      if(!s.tx.healthy())s.valve.emergency(Reason::Transport,now);
      const auto v=s.valve.status();
      if(s.sentKey!=v.key || s.sent!=v.revision){s.sentKey=v.key;s.sent=v.revision;s.at=now-1000;s.attempts=0;}
      if((s.ackedKey!=v.key || s.acked!=v.revision) && s.attempts<6 && elapsed(now,s.at,1000+(i+2)*13)){s.at=now;++s.attempts;s.status(now);}
      if(elapsed(now,s.helloAt,10000)){auto p=packet(Op::Hello,1);Writer w{p};w.u32(5880);w.u32(1);w.u8(1);enqueue(i,p);s.helloAt=now;}
      if(s.valve.busy() && elapsed(now,s.summaryAt,TelemetryMs)){auto p=keyed(Op::Summary,1,v.key);Writer w{p};w.u32(v.pulses);w.u32(v.revision);w.u8(uint8_t(v.phase));w.u8(v.valveCommand);enqueue(i,p);s.summaryAt=now;}
    }
    for(size_t j=0;j<flights.size();){
      auto &f=flights[j];
      if(!f.delivered && int32_t(now-f.deliverAt)>=0){f.delivered=true;const auto copy=f;deliver(copy);}
      if(int32_t(now-f.endAt)>=0){
        if(!missingEnd)(f.from<0?link.tx:slaves[unsigned(f.from)]->tx).event(f.ticket,3,0,now);
        if(f.delivered){flights.erase(flights.begin()+j);continue;}
        f.endAt=now+60000;
      }
      ++j;
    }
    scheduler.tick(now);dispatch(link.tx,-1);
    for(unsigned i=0;i<count;++i)dispatch(slaves[i]->tx,int(i));
  }
  void begin(uint32_t id){link.now=now;CHECK(scheduler.check((1U<<count)-1,500,60000,now)==Reply::Ok);scheduler.begin({1,id},(1U<<count)-1,500,60000,now);}
  void run(uint32_t duration){const auto end=now+duration;while(int32_t(end-now)>0)step();}
  void finish(){const auto start=now;while(scheduler.active() && !elapsed(now,start,20000))step();CHECK(!scheduler.active() && !io.on);run(1000);}
  void clean(){
    const auto start=now;
    for(;;){
      bool quiet=link.tx.queued()==0 && link.tx.inFlight()==0;
      for(unsigned i=0;i<count;++i){const auto &s=*slaves[i];const auto v=s.valve.status();quiet=quiet && s.tx.queued()==0 && s.tx.inFlight()==0 && (s.attempts==6 || (s.ackedKey==v.key && s.acked==v.revision));}
      if(quiet)break;
      CHECK(!elapsed(now,start,9000));step();
    }
    CHECK(link.tx.healthy());for(unsigned i=0;i<count;++i)CHECK(slaves[i]->tx.healthy());
  }
};
void joint_runs(){
  for(unsigned n:{1U,2U,5U}){
    Bench b(n);
    for(unsigned id=1;id<=1000;++id){
      b.dropEvent=id%3==0;b.duplicate=true;b.begin(id);b.finish();
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
    std::printf("PASS %u nodes x 1000 batches, 500 mL/2940 pulse simulation, duplicate Start/drop EventAck, bounded/empty windows\n",n);
  }
  for(unsigned mode=0;mode<4;++mode){
    Bench b(5);b.begin(1);
    if(mode==0)b.dropAck=true;
    if(mode==1)b.slow=4;
    if(mode==2)b.missingEnd=true;
    if(mode==3){b.run(1800);b.scheduler.stop(Reason::Manual,b.now);CHECK(!b.io.on);}
    b.finish();CHECK(b.scheduler.report().phase==SchedulePhase::Failed && !b.io.on);
    for(unsigned i=0;i<5;++i)CHECK(!b.slaves[i]->io.on);
  }
  std::puts("PASS five-node ACK loss/slow node/missing SAR end/manual stop: local stop and remote closure");
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
      for(size_t i=0;i<b.flights.size();++i) {
        const auto f=b.flights[i];
        if(f.from!=0 || !MeshTxGovernor::segmented(f.p)) continue;
        // The dispatcher already accepted this ticket. Inject its real end
        // error; API rejection is covered separately by the governor matrix.
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
      for(size_t i=0;i<b.flights.size();++i) {
        const auto f=b.flights[i];
        if(f.from>=0 || !MeshTxGovernor::segmented(f.p)) continue;
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
int main(){governor_matrix();governor_recovery_matrix();recovery_link_runs();joint_runs();return 0;}
