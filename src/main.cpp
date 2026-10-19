

#include "application.h"
#include "hardware.h"
#include "esp_log.h"
#include "freertos/task.h"
using namespace pump;
extern "C" void app_main() {
  static Storage storage;
  static Hardware hardware;
  hardware.safeOff();
  if (!storage.open(board::ProfileId) || !hardware.init()) {
    ESP_LOGE("boot", "storage/profile/hardware invalid: output OFF; NVS preserved"); return;
  }
  static PumpSafetyController pump(hardware, board::pumpConfig());
  pump.boot(nowMs());
  static MeshTransport mesh;
  static ControlGateway gateway;
  if (!mesh.init(storage.state().master) || !gateway.init(storage.state().gatewayPin, mesh.deviceName())) {
    hardware.safeOff(); ESP_LOGE("boot", "Mesh/gateway initialization failed: output OFF"); return;
  }
  ESP_LOGI("boot", "Bluetooth name: %s", mesh.deviceName());
  static MasterScheduler scheduler(mesh, pump, board::MaxConcurrent);
  for (size_t i = 0; i < MaxNodes; ++i) {
    const auto &n = storage.state().nodes[i];
    if (n.address) scheduler.registerNode(i, {n.address, n.k, n.version});
  }
  scheduler.restore(storage.state().lastJob, storage.state().lastResults);
  static Application application(storage, mesh, gateway, pump, scheduler);
  constexpr BaseType_t controlCore = portNUM_PROCESSORS > 1 ? 1 : 0;
  if (xTaskCreatePinnedToCore([](void *p) { static_cast<Application *>(p)->run(); }, "local_control",
      24576, &application, 10, nullptr, controlCore) != pdPASS) {
    hardware.safeOff(); ESP_LOGE("boot", "control task allocation failed: output OFF");
  }
}
