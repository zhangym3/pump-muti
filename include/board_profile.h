#pragma once
#include "pump_safety_controller.h"

#include "sdkconfig.h"
#if !defined(CONFIG_IDF_TARGET_ESP32)
#error This project builds only the ESP32 pump_master. Use pump-muti-slave for ESP32-C3.
#endif
namespace pump::board {
constexpr uint32_t ProfileId = 0x504D0001; // Preserve the deployed ESP32 master NVS profile.
constexpr int Output = 25; // Pump driver PWM input.
constexpr uint32_t PumpPwmHz = 20000; // Provisional: match the pump driver's specification.
// The default remains the deployed limit until the five-device water gate passes.
#ifndef PUMP_MAX_CONCURRENT
#define PUMP_MAX_CONCURRENT 1
#endif
constexpr uint8_t MaxConcurrent = PUMP_MAX_CONCURRENT;
constexpr PumpConfig pumpConfig() {
  PumpConfig c;
  c.commissioned = true; // GPIO25 PWM configured. USB reset unlocks dosing; bounded phone tests do not need it.
  // No master sensor/feedback inputs. Adjust command timing for the selected pump.
  return c;
}
static_assert(MaxConcurrent >= 1 && MaxConcurrent <= MaxNodes);
}
