#include "gateway_session.h"
#include <cassert>
#include <cstdio>
int main() {
  pump::GatewaySession gate;
  assert(!gate.accepts(0) && !gate.token());
  assert(!gate.authorize(0));
  const auto first=gate.begin();
  assert(first && !gate.token() && !gate.accepts(first));
  assert(gate.authorize(first));
  assert(gate.authorize(first)); // Duplicate completion is idempotent.
  const auto queued=gate.token();
  assert(queued && gate.accepts(queued));
  gate.revoke();
  assert(!gate.accepts(queued) && !gate.token());
  assert(!gate.authorize(first)); // A late completion cannot undo disconnect.
  const auto next=gate.begin();
  assert(next!=first && !gate.authorize(first));
  assert(gate.authorize(next));
  assert(!gate.accepts(queued) && gate.accepts(gate.token()));
  gate.revoke();gate.revoke();
  assert(!gate.token());
  for (unsigned i=0;i<100000;++i) {
    const auto link=gate.begin();
    assert(gate.authorize(link));
    const auto token=gate.token();
    assert(gate.accepts(token));gate.revoke();assert(!gate.accepts(token));
    assert(!gate.authorize(link));
    assert(!gate.authorize(link|2u));
  }
  puts("PASS gateway: unauthenticated, disconnect, stale queued commands/auth callbacks, 100000 generations");
}
