Packet progressPacket(uint16_t peer=2,uint32_t pulses=10,uint32_t runtime=2000,JobKey job={1,1}) {
  auto p=packet(Op::Progress,peer); Writer w{p}; w.key(job); w.u32(pulses); w.u32(runtime); return p;
}

void progress_governor() {
  {
    MeshTxGovernor g;
    CHECK(!g.enqueue(progressPacket(AllSlaves),100));
    auto invalid=progressPacket(); invalid.size=8;
    CHECK(!g.enqueue(invalid,100));
  }
  {
    MeshTxGovernor g; MeshTxGovernor::Dispatch d;
    CHECK(g.enqueue(progressPacket(2,10,2000),100));
    CHECK(g.enqueue(progressPacket(2,20,4000),200));
    CHECK(g.queued()==1 && g.metrics().merged==1 && g.next(200,d));
    Reader r{d.packet}; CHECK(r.key()==JobKey{1,1} && r.u32()==20 && r.u32()==4000 && r.done());
    CHECK(d.packet.peer==2 && d.packet.size==16);
  }
  {
    MeshTxGovernor g; MeshTxGovernor::Dispatch d;
    CHECK(g.enqueue(progressPacket(2,10,2000),100));
    CHECK(g.enqueue(keyed(Op::LeaseAck,2),101));
    CHECK(g.next(101,d) && d.packet.op==Op::LeaseAck);
  }
  {
    MeshTxGovernor g; MeshTxGovernor::Dispatch d;
    CHECK(g.enqueue(progressPacket(),100) && g.next(100,d));
    g.event(d.ticket,1,0,101); drain(g,101);
    g.retire(2,{1,1},Op::Progress);
    CHECK(g.inFlight()==1);
    g.event(d.ticket,3,0,110); drain(g,110);
    CHECK(g.inFlight()==0 && g.healthy());
  }
  for (bool accepted:{false,true}) {
    MeshTxGovernor g; MeshTxGovernor::Dispatch d;
    CHECK(g.enqueue(progressPacket(),100) && g.next(100,d));
    if (accepted) g.event(d.ticket,1,0,101);
    g.event(d.ticket,accepted?3:2,accepted?-110:-12,102); drain(g,102);
    CHECK(g.healthy() && g.inFlight()==0);
  }
  {
    MeshTxGovernor g; MeshTxGovernor::Dispatch d;
    CHECK(g.enqueue(progressPacket(),100) && g.next(100,d));
    g.event(d.ticket,1,0,101); drain(g,101);
    CHECK(!g.next(5101,d));
    CHECK(!g.next(6100,d));
    CHECK(g.diagnostics(10102).restartRequired && g.inFlight()==1);
  }
  std::puts("PASS Progress: unicast-only, 16-byte validation, coalescing, low priority, close retirement, best-effort SDK failures and real missing-completion lock");
}

void live_link_runs() {
  for(unsigned n:{1U,2U,5U}) {
    Bench b(n); b.pulsesPerTick=1; b.begin(1);
    const uint32_t selected=(1U<<n)-1; uint32_t observed=0;
    for(unsigned tick=0;tick<2000 && observed!=selected;++tick) {
      b.step();
      for(unsigned i=0;i<n;++i) {
        const auto &live=b.scheduler.node(i).live;
        if(live.key==JobKey{1,1} && live.seen && live.pulses>0 && live.runtimeMs>0)
          observed|=uint32_t(1)<<i;
      }
    }
    CHECK(observed==selected);
    b.finish();
    CHECK(b.scheduler.report().phase==SchedulePhase::Complete);
    CHECK(b.progressFrames>=n && b.progressApplied>=n);
    CHECK(b.summaryFrames==0 && !b.io.on && b.link.tx.healthy());
    for(unsigned i=0;i<n;++i) {
      const auto &live=b.scheduler.node(i).live;
      CHECK(!live.seen); // terminal status clears live telemetry; RESULT remains authoritative.
      CHECK(!b.slaves[i]->io.on && b.slaves[i]->tx.healthy());
    }
    b.clean();
    std::printf("PASS unicast live flow %u node(s): active snapshots forwarded, terminal state cleared, final result authoritative\n",n);
  }
}
