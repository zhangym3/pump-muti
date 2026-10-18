#include "pump_safety_controller.h"
namespace pump {
bool PumpConfig::valid() const {
  return commissioned && maxRunMs && maxRunMs <= 3600000 && minOffMs &&
    minOffMs <= 60000 && authorizationMs && authorizationMs <= 3000 &&
    lowDuty > 0 && lowDuty < mediumDuty && mediumDuty < highDuty && highDuty <= 100;
}
void PumpSafetyController::boot(uint32_t now) {
  testMs_ = 0;
  testBootLock_ = dryTest_ = false;
  io_.pump(false); output_ = false; offAt_ = now; key_ = {};
  phase_ = PumpPhase::Fault; fault_ = PumpFault::BootLock;
  speed_ = 1; if (!io_.setDuty(duty())) fault_ = PumpFault::Transport;
}
bool PumpSafetyController::ready(uint32_t now) {
  return cfg_.valid() && io_.healthy() && phase_ == PumpPhase::Off && elapsed(now, offAt_, cfg_.minOffMs);
}
bool PumpSafetyController::testReady(uint32_t now) {
  return cfg_.valid() && io_.healthy() && !output_ &&
    ((phase_ == PumpPhase::Off && fault_ == PumpFault::None) ||
     (phase_ == PumpPhase::Fault && fault_ == PumpFault::BootLock)) && elapsed(now, offAt_, cfg_.minOffMs);
}
Reply PumpSafetyController::reset(uint32_t now) {
  if (output_) return Reply::NotReady;
  if (!cfg_.valid() || !io_.healthy()) return Reply::NotReady;
  key_ = {}; phase_ = PumpPhase::Off; fault_ = PumpFault::None; offAt_ = now;
  testBootLock_ = dryTest_ = false;
  return Reply::Ok; // Reset never starts the old task.
}
Reply PumpSafetyController::start(JobKey key, uint32_t seq, uint32_t now) {
  if (!key.valid() || !seq) return Reply::Invalid;
  if (!ready(now)) return Reply::NotReady;
  testMs_ = 0; key_ = key; seq_ = seq; authAt_ = startAt_ = now;
  phase_ = PumpPhase::Running; output_ = true; io_.pump(true);
  if (!io_.healthy()) { trip(PumpFault::Transport, now); return Reply::NotReady; }
  return Reply::Ok;
}
Reply PumpSafetyController::setSpeed(uint8_t level, uint32_t now) {
  if (level < 1 || level > 3) return Reply::Invalid;
  if (!cfg_.valid() || (fault_ != PumpFault::None && fault_ != PumpFault::BootLock)) return Reply::NotReady;
  if (dryTesting() && level != 1) return Reply::Busy;
  if (!io_.healthy()) { trip(PumpFault::Transport, now); return Reply::NotReady; }
  const auto previous = speed_; speed_ = level;
  if (!io_.setDuty(duty())) { speed_ = previous; trip(PumpFault::Transport, now); return Reply::NotReady; }
  return Reply::Ok; // Never changes startAt_, authAt_ or the bounded test deadline.
}
Reply PumpSafetyController::startTest(JobKey key, uint32_t seq, uint32_t durationMs, uint32_t now, bool dryRun) {
  if (!key.valid() || !seq || !durationMs || durationMs > (dryRun ? 3000U : 10000U) ||
      durationMs > cfg_.maxRunMs) return Reply::Invalid;
  if (!testReady(now)) return Reply::NotReady;
  if (dryRun) {
    speed_ = 1;
    if (!io_.setDuty(duty())) { trip(PumpFault::Transport, now); return Reply::NotReady; }
  }
  testBootLock_ = fault_ == PumpFault::BootLock;
  fault_ = PumpFault::None; testMs_ = durationMs; dryTest_ = dryRun;
  key_ = key; seq_ = seq; authAt_ = startAt_ = now;
  phase_ = PumpPhase::Running; output_ = true; io_.pump(true);
  if (!io_.healthy()) { trip(PumpFault::Transport, now); return Reply::NotReady; }
  return Reply::Ok; // A bounded phone test never unlocks normal dosing.
}
uint32_t PumpSafetyController::testRemaining(uint32_t now) const {
  const uint32_t spent = uint32_t(now - startAt_);
  return testing() && spent < testMs_ ? testMs_ - spent : 0;
}
Reply PumpSafetyController::renew(JobKey key, uint32_t seq, uint32_t now) {
  if (testing()) return Reply::Stale; // A lease must never extend a bounded manual test.
  if (!output_ || key != key_ || !serialNewer(seq, seq_)) return Reply::Stale;
  if (elapsed(now, authAt_, cfg_.authorizationMs)) { trip(PumpFault::Authorization, now); return Reply::Stale; }
  seq_ = seq; authAt_ = now; return Reply::Ok;
}
void PumpSafetyController::off(uint32_t now) {
  testMs_ = 0;
  io_.pump(false); if (output_) offAt_ = now; output_ = false;
}
void PumpSafetyController::stop(uint32_t now) {
  off(now); key_ = {};
  if (testBootLock_) { fault_ = PumpFault::BootLock; phase_ = PumpPhase::Fault; }
  testBootLock_ = dryTest_ = false;
  if (fault_ == PumpFault::None) phase_ = PumpPhase::Off;
}
void PumpSafetyController::trip(PumpFault f, uint32_t now) {
  testBootLock_ = dryTest_ = false;
  off(now); key_ = {}; fault_ = f; phase_ = PumpPhase::Fault;
}
void PumpSafetyController::tick(uint32_t now) {
  if (!output_) return;
  if (!io_.healthy()) { trip(PumpFault::Transport, now); return; }
  if (testing()) {
    if (elapsed(now, startAt_, testMs_)) stop(now);
    return; // Test duration is enforced here, independently of phone connectivity.
  }
  if (elapsed(now, authAt_, cfg_.authorizationMs)) { trip(PumpFault::Authorization, now); return; }
  if (elapsed(now, startAt_, cfg_.maxRunMs)) { trip(PumpFault::Runtime, now); return; }
}
const char *pumpFaultName(PumpFault f) {
  switch (f) {
    case PumpFault::None: return "NONE";
    case PumpFault::BootLock: return "BOOT_LOCK";
    case PumpFault::NotCommissioned: return "NOT_COMMISSIONED";
    case PumpFault::Authorization: return "AUTHORIZATION";
    case PumpFault::Runtime: return "RUNTIME";
    case PumpFault::Transport: return "TRANSPORT";
  }
  return "INVALID";
}
}
