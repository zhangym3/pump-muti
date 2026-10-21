#pragma once
#include "pump_safety_controller.h"

#include "sdkconfig.h"
#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error This project builds only the ESP32-S3 pump_master. Use pump-muti-slave for ESP32-C3.
#endif
namespace pump::board {
constexpr uint32_t ProfileId = 0x504D0001; // Preserve the deployed ESP32 master NVS profile.
constexpr int Output = 4; // Pump PWM; N16R8 GPIO35-37 belong to octal PSRAM.
constexpr uint32_t PumpPwmHz = 20000; // Provisional: match the pump driver's specification.
// The default remains the deployed limit until the five-device water gate passes.
#ifndef PUMP_MAX_CONCURRENT
#define PUMP_MAX_CONCURRENT 1
#endif
constexpr uint8_t MaxConcurrent = PUMP_MAX_CONCURRENT;
constexpr PumpConfig pumpConfig() {
  PumpConfig c;
  c.commissioned = true; // GPIO4 PWM configured; bounded phone tests do not need a USB reset.
  // No master sensor/feedback inputs. Adjust command timing for the selected pump.
  return c;
}
static_assert(MaxConcurrent >= 1 && MaxConcurrent <= MaxNodes);
}
