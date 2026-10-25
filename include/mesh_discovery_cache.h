#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace pump {
struct MeshCandidate { uint8_t uuid[16] = {}; uint8_t addr[6] = {}; uint8_t type = 0; uint16_t oob = 0; };

// Main-task only. The SDK scans continuously, including between UI searches.
class MeshDiscoveryCache {
 public:
  static constexpr size_t Capacity = 8;
  static constexpr uint32_t LifetimeMs = 30000;

  void clear() { count_ = 0; }
  void forget(const uint8_t uuid[16]) {
    for (size_t i = 0; i < count_; ++i) if (!std::memcmp(entries_[i].candidate.uuid, uuid, 16)) {
      erase(i); return;
    }
  }
  void observe(const MeshCandidate &candidate, uint32_t now) {
    expire(now);
    size_t slot = count_;
    for (size_t i = 0; i < count_; ++i) if (!std::memcmp(entries_[i].candidate.uuid, candidate.uuid, 16)) {
      slot = i; break;
    }
    if (slot == Capacity) {
      slot = 0;
      for (size_t i = 1; i < count_; ++i)
        if (uint32_t(now - entries_[i].seenAt) > uint32_t(now - entries_[slot].seenAt)) slot = i;
    } else if (slot == count_) ++count_;
    entries_[slot] = {candidate, now};
  }
  size_t copyFresh(MeshCandidate *output, size_t capacity, uint32_t now) {
    expire(now);
    const size_t size = count_ < capacity ? count_ : capacity;
    for (size_t i = 0; i < size; ++i) output[i] = entries_[i].candidate;
    return size;
  }

 private:
  struct Entry { MeshCandidate candidate; uint32_t seenAt = 0; };
  Entry entries_[Capacity];
  size_t count_ = 0;
  void erase(size_t index) {
    for (size_t i = index + 1; i < count_; ++i) entries_[i - 1] = entries_[i];
    --count_;
  }
  void expire(uint32_t now) {
    for (size_t i = 0; i < count_;)
      if (uint32_t(now - entries_[i].seenAt) >= LifetimeMs) erase(i);
      else ++i;
  }
};
}
