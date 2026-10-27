#pragma once
#include "pump_safety_controller.h"

#include "sdkconfig.h"
#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error This project builds only the ESP32-S3 pump_master. Use pump-muti-slave for the ESP32-C3 valve role.
#endif
namespace pump::board {
constexpr uint32_t ProfileId = 0x504D0001; // Preserve the deployed ESP32 master NVS profile.
// B24-12 DC24V four-wire brushless pump: red=+24V, black=GND, blue=PWM, yellow=tach.
// Blue/yellow electrical levels, PWM polarity/frequency and stop behavior await supplier specs.
// Use a compatible signal interface; GPIO4 must not power the pump. See docs/PUMP_B24_12_24V.md.
constexpr int Output = 4; // PWM command; N16R8 GPIO35-37 are reserved for PSRAM.
constexpr uint32_t PumpPwmHz = 20000; // Existing setting only; not verified for the B24-12.
// The default remains the deployed limit until the five-device water gate passes.
#ifndef PUMP_MAX_CONCURRENT
#define PUMP_MAX_CONCURRENT 1
#endif
constexpr uint8_t MaxConcurrent = PUMP_MAX_CONCURRENT;
constexpr PumpConfig pumpConfig() {
  PumpConfig c;
  c.commissioned = true; // Existing software readiness; not hardware acceptance of the new pump.
  // No master sensor/feedback inputs; the yellow tach wire is not sampled.
  // Existing 65/80/100% duties and GPIO-low OFF require B24-12 interface verification.
  return c;
}
static_assert(MaxConcurrent >= 1 && MaxConcurrent <= MaxNodes);
}
