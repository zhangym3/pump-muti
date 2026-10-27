#pragma once
#include <atomic>
#include <cstdint>
namespace pump {
// A queued command belongs to one authenticated connection. Tokens never wrap.
// Low bits: 0=revoked, 1=connected, 3=authorized. A revoked connection cannot
// be authorized again, even if its authentication completion arrives late.
class GatewaySession {
 public:
  uint32_t begin() {
    auto old=state_.load();
    for (;;) {
      if ((old & ~3u)==0xfffffffcu) { revoke(); return 0; }
      const auto next=((old & ~3u)+4)|1u;
      if (state_.compare_exchange_weak(old,next)) return next;
    }
  }
  bool authorize(uint32_t connection) {
    if ((connection&3u)!=1u) return false;
    auto expected=connection;
    return state_.compare_exchange_strong(expected,connection|2u) || expected==(connection|2u);
  }
  void revoke() { state_.fetch_and(~3u); }
  uint32_t token() const { const auto value=state_.load();return (value&3u)==3u ? value : 0; }
  bool accepts(uint32_t token) const { return (token&3u)==3u && state_.load()==token; }
 private:
  std::atomic<uint32_t> state_{0};
};
}
