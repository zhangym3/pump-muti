#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <atomic>
#include <cstdint>
namespace pump {
struct CommandLine { bool bluetooth = false; uint32_t session = 0; char text[160] = {}; };
class ControlGateway {
 public:
  bool init(uint32_t passkey, const char *name);
  bool receive(CommandLine &cmd);
  bool submit(const char *text, size_t length, bool bluetooth);
  bool authorized(const CommandLine &cmd) const;
  bool publish(const char *text);
  bool publishLive(const char *text);
  void ioWorker();
  uint32_t bluetoothStackMinimumBytes() const;
#if defined(PUMP_DISCOVERY_TRACE)
  bool setDiagnosticAdvertising(bool enabled);
#endif
  bool overflow() { return overflow_.exchange(false); }
 private:
  QueueHandle_t commands_ = nullptr, output_ = nullptr, liveOutput_ = nullptr;
  std::atomic<bool> overflow_{false};
};
}
