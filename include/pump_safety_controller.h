#pragma once
#include "protocol.h"
namespace pump {
// Preserve the existing diagnostic numbers; unused monitored-pump values are reserved.
enum class PumpPhase : uint8_t { Off = 0, Running = 2, Fault = 4 };
enum class PumpFault : uint8_t { None = 0, BootLock = 1, NotCommissioned = 2,
  Authorization = 7, Runtime = 8, Transport = 11 };
struct PumpConfig {
  bool commissioned = false;
  // Command timing only: the output-only master has no pump/sensor feedback.
  uint32_t maxRunMs = 240000, minOffMs = 1000;
  uint32_t authorizationMs = 1000;
  uint8_t lowDuty = 65, mediumDuty = 80, highDuty = 100;
  bool valid() const;
};
class PumpIO {
 public:
  virtual ~PumpIO() = default;
  virtual void pump(bool on) = 0;
  virtual bool setDuty(uint8_t percent) = 0;
  virtual bool healthy() const { return true; }
};
class PumpSafetyController {
 public:
  PumpSafetyController(PumpIO &io, PumpConfig cfg) : io_(io), cfg_(cfg) {}
  void boot(uint32_t now);
  Reply reset(uint32_t now);
  Reply start(JobKey key, uint32_t sequence, uint32_t now);
  Reply startTest(JobKey key, uint32_t sequence, uint32_t durationMs, uint32_t now, bool dryRun = false);
  Reply setSpeed(uint8_t level, uint32_t now);
  uint8_t speed() const { return speed_; }
  uint8_t duty() const { return speed_ == 1 ? cfg_.lowDuty : speed_ == 2 ? cfg_.mediumDuty : cfg_.highDuty; }
  bool testing() const { return output_ && testMs_ != 0; }
  bool dryTesting() const { return testing() && dryTest_; }
  bool testReady(uint32_t now);
  uint32_t testRemaining(uint32_t now) const;
  Reply renew(JobKey key, uint32_t sequence, uint32_t now);
  void stop(uint32_t now);
  void trip(PumpFault fault, uint32_t now);
  void tick(uint32_t now);
  bool ready(uint32_t now);
  bool output() const { return output_; }
  PumpPhase phase() const { return phase_; }
  PumpFault fault() const { return fault_; }
 private:
  void off(uint32_t now);
  PumpIO &io_;
  PumpConfig cfg_;
  PumpPhase phase_ = PumpPhase::Off;
  PumpFault fault_ = PumpFault::None;
  JobKey key_;
  bool output_ = false;
  uint32_t seq_ = 0, authAt_ = 0, startAt_ = 0, offAt_ = 0;
  uint32_t testMs_ = 0;
  bool dryTest_ = false;
  uint8_t speed_ = 1;
};
const char *pumpFaultName(PumpFault f);
}
