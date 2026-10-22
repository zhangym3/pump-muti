#pragma once
#include "protocol.h"

namespace pump {
enum class FlowCalibrationPhase : uint8_t {
  Idle, Preout, MarkPending, Measuring, Stopping, Complete, Invalid, Unknown, Starting
};
enum class FlowCalibrationOperation : uint8_t { None, Start, Mark, Stop };

struct FlowCalibrationState {
  uint32_t magic = 0x504D4341;
  uint64_t client = 0;
  uint32_t request = 0;
  JobKey key;
  uint32_t standardMl = 0, maxMs = 0;
  uint32_t startPulses = 0, closePulses = 0, finalPulses = 0;
  uint32_t operationSequence = 0;
  uint16_t address = 0;
  uint8_t slot = 0, speed = 0;
  FlowCalibrationPhase phase = FlowCalibrationPhase::Idle;
  FlowCalibrationOperation operation = FlowCalibrationOperation::None;
  Reason reason = Reason::None;
  bool marked = false;
};

inline bool validFlowCalibrationState(const FlowCalibrationState &s) {
  const bool validPhase = s.phase == FlowCalibrationPhase::Idle ||
    s.phase == FlowCalibrationPhase::Preout || s.phase == FlowCalibrationPhase::MarkPending ||
    s.phase == FlowCalibrationPhase::Measuring || s.phase == FlowCalibrationPhase::Stopping ||
    s.phase == FlowCalibrationPhase::Complete || s.phase == FlowCalibrationPhase::Invalid ||
    s.phase == FlowCalibrationPhase::Unknown || s.phase == FlowCalibrationPhase::Starting;
  if (s.magic != 0x504D4341 || !validPhase ||
      s.operation > FlowCalibrationOperation::Stop || s.reason > Reason::Protocol) return false;
  if (s.phase == FlowCalibrationPhase::Idle) return !s.key.valid() && !s.client;
  return s.client && s.request && s.key.valid() && unicast(s.address) &&
    s.slot >= 1 && s.slot <= MaxNodes && s.speed >= 1 && s.speed <= 3 &&
    s.standardMl >= 50 && s.standardMl <= 2000 && s.maxMs && s.maxMs <= 60000 &&
    (!s.marked || s.phase == FlowCalibrationPhase::Measuring ||
      s.phase == FlowCalibrationPhase::Stopping || s.phase == FlowCalibrationPhase::Complete ||
      s.phase == FlowCalibrationPhase::Invalid || s.phase == FlowCalibrationPhase::Unknown);
}
}
