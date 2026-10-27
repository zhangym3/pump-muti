#pragma once
#include "protocol.h"
namespace pump {
// Never discard a DevKey merely because its node is offline. Pending keeps
// the durable removal intent and credentials available for an explicit retry.
class MeshRemoval {
 public:
  enum class Stage { Idle, Remote, Pending, DeleteReady, Deleting, Cleanup, Done, Failed };
  void begin(uint16_t address, uint32_t request, uint32_t now, bool exists) {
    address_ = address; request_ = request; at_ = now; remote_ = false;
    stage_ = exists ? Stage::Remote : Stage::Cleanup;
  }
  void remoteReply(uint16_t address, bool success) {
    if (stage_ != Stage::Remote || address != address_) return;
    remote_ = success; stage_ = success ? Stage::DeleteReady : Stage::Pending;
  }
  void tick(uint32_t now) {
    if (stage_ == Stage::Remote && elapsed(now, at_, 5000)) stage_ = Stage::Pending;
    // An unconfirmed SDK delete must not release the lock and reuse an address.
    if (stage_ == Stage::Deleting && elapsed(now, at_, 10000)) stage_ = Stage::Failed;
  }
  void deleting(uint32_t now) { if (stage_ == Stage::DeleteReady) { stage_ = Stage::Deleting; at_ = now; } }
  void deleted(uint16_t address, bool success) {
    if (stage_ == Stage::Deleting && address == address_) stage_ = success ? Stage::Cleanup : Stage::Failed;
  }
  void complete(bool saved) { if (stage_ == Stage::Cleanup) stage_ = saved ? Stage::Done : Stage::Failed; }
  Stage stage() const { return stage_; }
  uint16_t address() const { return address_; }
  uint32_t request() const { return request_; }
  bool remoteReset() const { return remote_; }
 private:
  Stage stage_ = Stage::Idle;
  uint16_t address_ = 0;
  uint32_t request_ = 0, at_ = 0;
  bool remote_ = false;
};
}
