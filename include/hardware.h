#pragma once
#include "board_profile.h"
namespace pump {
class Hardware final : public PumpIO {
 public:
  bool init();
  void pump(bool on) override;
  bool setDuty(uint8_t percent) override;
  bool healthy() const override { return pwmHealthy_; }
  void safeOff();
 private:
  bool pwmInitialized_ = false, pwmHealthy_ = true, pumpOn_ = false;
  uint8_t duty_ = 30;
};
uint32_t nowMs();
}
