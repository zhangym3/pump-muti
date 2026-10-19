#include "mesh_tx_governor.h"
#include <cstdio>
#include <cstdlib>
using namespace pump;
#define REQUIRE(v) do { if (!(v)) std::exit(2); } while (false)
Packet hello() {
  auto p=packet(Op::Hello,1); Writer w{p}; w.u32(5880); w.u32(1); w.u8(1); return p;
}
void drain(MeshTxGovernor &g,uint32_t at) { MeshSubmission s; while(g.collect(at,s)) {} }
void report(const char *scenario,MeshTxGovernor &g) {
  const auto m=g.metrics(); unsigned rejected=0;
  for(uint32_t t=10000;t<=60000;t+=10000) {
    if(!g.enqueue(hello(),t)) ++rejected;
    MeshTxGovernor::Dispatch d; REQUIRE(!g.next(t,d));
  }
  REQUIRE(!g.healthy() && rejected==6 && g.inFlight()==0);
  std::printf("{\"scenario\":\"%s\",\"healthy\":false,\"in_flight\":%zu,\"faults\":%u,\"ended\":%u,\"expired\":%u,\"hellos_rejected_60s\":%u}\n",scenario,g.inFlight(),m.faults,m.ended,m.expired,rejected);
}
int main() {
  {
    MeshTxGovernor g{1}; MeshTxGovernor::Dispatch d;
    REQUIRE(g.enqueue(hello(),100) && g.next(100,d));
    g.event(d.ticket,1,0,101); drain(g,101);
    // One real completed SAR timeout: the context is released, the fault stays.
    g.event(d.ticket,3,-110,2877); drain(g,2877);
    report("completed_sar_error",g);
  }
  {
    MeshTxGovernor g{1}; MeshTxGovernor::Dispatch d;
    auto status=packet(Op::Status,1); Writer sw{status};
    sw.key({116,28}); sw.u32(0); sw.u32(0); sw.u32(5880); sw.u32(1010);
    sw.u8(uint8_t(Phase::Running)); sw.u8(0); sw.u8(1);
    REQUIRE(g.enqueue(status,100) && g.next(100,d));
    g.event(d.ticket,1,0,101); drain(g,101);
    auto ack=packet(Op::LeaseAck,1); Writer aw{ack}; aw.key({116,28}); aw.u32(1); aw.u8(0);
    REQUIRE(g.enqueue(ack,200));
    MeshTxGovernor::Dispatch other; REQUIRE(!g.next(999,other) && g.healthy());
    REQUIRE(!g.next(1000,other) && !g.healthy());
    // Even a later successful end leaves every subsequent Hello blocked.
    g.event(d.ticket,3,0,2877); drain(g,2877);
    report("lease_ack_expired_behind_status",g);
  }
  {
    MeshTxGovernor g{1}; MeshTxGovernor::Dispatch d;
    REQUIRE(g.enqueue(hello(),0)); REQUIRE(!g.next(2000,d));
    REQUIRE(g.healthy() && g.enqueue(hello(),10000) && g.next(10000,d));
    std::puts("{\"scenario\":\"hello_expiry_only\",\"healthy\":true,\"next_hello_dispatched\":true}");
  }
}
