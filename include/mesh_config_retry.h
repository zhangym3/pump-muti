#pragma once
#include <cstdint>

namespace pump {
class MeshConfigRetry {
 public:
  static constexpr uint32_t ResponseTimeoutMs = 15000;
  static constexpr uint32_t SarCleanupDelayMs = 12000;
  static constexpr uint8_t MaxAttempts = 3;
  static constexpr uint32_t CallbackWatchdogMs = 18000, StepDeadlineMs = 75000;

  enum class TimeoutResult : uint8_t { Ignored, RetryScheduled, Exhausted };

  void beginStep() {
    attempts_ = 0;
    waiting_ = false;
    retryPending_ = false;
    started_ = false;
  }

  bool submitted(uint32_t now) {
    if (waiting_ || attempts_ >= MaxAttempts) return false;
    if (!started_) { firstAt_ = now; started_ = true; }
    ++attempts_;
    sentAt_ = now;
    waiting_ = true;
    retryPending_ = false;
    return true;
  }

  bool response() {
    if (!waiting_) return false;
    waiting_ = false;
    retryPending_ = false;
    return true;
  }

  TimeoutResult timeout(uint32_t now) {
    if (!waiting_) return TimeoutResult::Ignored;
    waiting_ = false;
    if (attempts_ >= MaxAttempts) return TimeoutResult::Exhausted;
    retryAt_ = now + SarCleanupDelayMs;
    retryPending_ = true;
    return TimeoutResult::RetryScheduled;
  }

  bool retryDue(uint32_t now) {
    if (!retryPending_ || static_cast<int32_t>(now - retryAt_) < 0) return false;
    retryPending_ = false;
    return true;
  }

  uint32_t remainingMs(uint32_t now) const {
    if (!retryPending_) return 0;
    const int32_t remaining = static_cast<int32_t>(retryAt_ - now);
    return remaining > 0 ? static_cast<uint32_t>(remaining) : 0;
  }

  uint8_t attempts() const { return attempts_; }
  bool waiting() const { return waiting_; }
  bool retryPending() const { return retryPending_; }
  uint32_t sentAt() const { return sentAt_; }
  bool callbackExpired(uint32_t now) const { return waiting_ && uint32_t(now-sentAt_)>=CallbackWatchdogMs; }
  bool stepExpired(uint32_t now) const { return started_ && uint32_t(now-firstAt_)>=StepDeadlineMs; }

 private:
  uint32_t sentAt_ = 0;
  uint32_t retryAt_ = 0;
  uint32_t firstAt_ = 0;
  bool started_ = false;
  uint8_t attempts_ = 0;
  bool waiting_ = false;
  bool retryPending_ = false;
};
}
