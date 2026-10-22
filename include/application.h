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
  void retryResponse();
  void replayFlows(uint32_t now);
  void requeryUnknown(uint32_t now);
  void publishFlow(uint16_t address, JobKey key, uint32_t pulses, uint32_t k,
                   Phase phase, Reason reason, uint32_t revision, bool terminal);
  void periodic(uint32_t now);
  void localState(uint32_t now);
  void meshState();
  void pumpPwmState(Reply reply = Reply::Ok);
  void calibrationState(uint32_t request, size_t slot, Reply reply);
  void flowCalibrationStatus(Reply reply = Reply::Ok, uint64_t client = 0, uint32_t request = 0);
  void pumpTestReply(const ControlRequest &q, Reply result, uint32_t now);
  bool idle() const;
  bool flowCalibrationLocked() const;
  void meshState(uint32_t request);
  void beginInventory(uint32_t request, uint32_t now);
  void inventoryTick(uint32_t now);
  void commissionTick(uint32_t now);
  void removalTick(uint32_t now);
  void commissionResult(uint32_t request, uint8_t slot, uint16_t address, Reply reply, bool commissioned);
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
  char pendingResponse_[2][192] = {};
  bool responseLinePending_[2] = {};
  JobKey flowReplayKey_, resultQueryKey_;
  ControlRequest resultQueryOwner_;
  uint32_t flowReplayMask_ = 0, flowReplayAt_ = 0, resultQueryMask_ = 0, resultQueryAt_ = 0;
  uint16_t resultQueryIngress_ = 0;
  uint8_t flowReplayCursor_ = 0, resultQueryCursor_ = 0;
  bool resultQueryOwnerPending_ = false;
  bool meshResetPending_ = false;
  bool removalStarted_ = false;
  struct CommissionOperation { bool active=false; uint32_t request=0, sentAt=0; uint16_t address=0; uint8_t slot=0, attempts=0; } commission_;
  ControlRequest owner_;
  ControlRequest pumpTestOwner_;
  bool testOutputPublished_ = false;
  uint32_t testPublishedAt_ = 0;
  uint32_t pwmRevision_ = 0;
  uint32_t flowCalibrationPublishAt_ = 0;
  uint32_t flowCalibrationPublishedPulses_ = 0;
  uint8_t flowCalibrationPublishedPhase_ = 0xFF;
  uint16_t ownerIngress_ = 0;
  const char *lastGuard_ = "NONE";
  SchedulePhase lastPhase_ = SchedulePhase::Idle;
};
}
