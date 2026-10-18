#pragma once
#include "control_gateway.h"
#include "control_request.h"
#include "storage.h"
#include "board_profile.h"
#include "mesh_transport.h"
namespace pump {
class Application {
 public:
  Application(Storage &store, MeshTransport &mesh, ControlGateway &gateway,
              PumpSafetyController &pump, MasterScheduler &scheduler)
    : store_(store), mesh_(mesh), gateway_(gateway), pump_(pump), scheduler_(scheduler) {}
  void run();
 private:
  void incoming(const Packet &p, uint32_t now);
  void command(CommandLine &c, uint32_t now);
  void request(ControlRequest q, uint16_t ingress, uint32_t now);
  void response(uint16_t ingress, const ControlRequest &q, Reply reply, JobKey key);
  void publishResponse(const Packet &p);
  void periodic(uint32_t now);
  void localState(uint32_t now);
  void pumpPwmState(Reply reply = Reply::Ok);
  void pumpTestReply(const ControlRequest &q, Reply result, uint32_t now);
  bool idle() const;
  void meshState(uint32_t request);
  void beginInventory(uint32_t request, uint32_t now);
  void inventoryTick(uint32_t now);
  struct InventoryNode { uint16_t address=0; uint8_t slot=0; bool online=false, commissioned=false; };
  InventoryNode inventory_[32];
  uint32_t inventoryRequest_=0, inventoryAt_=0;
  uint8_t inventoryCount_=0, inventoryCursor_=0;
  bool inventorySending_=false;
  Storage &store_;
  MeshTransport &mesh_;
  ControlGateway &gateway_;
  PumpSafetyController &pump_;
  MasterScheduler &scheduler_;
  uint32_t lastRequestAt_ = 0;
  bool storageHealthy_ = true, savedJob_ = true;
  bool meshResetPending_ = false;
  ControlRequest owner_;
  ControlRequest pumpTestOwner_;
  bool testOutputPublished_ = false;
  uint32_t testPublishedAt_ = 0;
  uint32_t pwmRevision_ = 0;
  uint16_t ownerIngress_ = 0;
  SchedulePhase lastPhase_ = SchedulePhase::Idle;
};
}
