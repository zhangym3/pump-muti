// Diagnostic only: reuse the existing simulator and link the production controllers.
// No hardware IO, firmware parameters, or production source are changed.
#define main unused_mesh_suite_entrypoint
#include "../../test/mesh_tests.cpp"
#undef main

void baseline_and_speed() {
  for (uint16_t speed : {SpeedLowPermille, SpeedMediumPermille, SpeedHighPermille}) {
    ValveConfig cfg; cfg.commissioned = true;
    IO io; io.reading.pulses = 253;
    FlowJobController valve(io, cfg); valve.boot({});
    Dose d; d.key = {315, 87}; d.ml = 250; d.k = 5880; d.version = 1;
    d.maxMs = 60000; d.speedFactorPermille = speed;
    CHECK(targetPulses(d.ml, d.k, d.pulses));
    CHECK(valve.prepare(d, 100) == Reply::Ok);
    CHECK(valve.start(d.key, 200) == Reply::Ok);
    valve.tick(5199); CHECK(io.on && valve.status().pulses == 0);
    valve.tick(5200); CHECK(!io.on && valve.status().reason == Reason::NoFlow);
    CHECK(valve.status().phase == Phase::Settling && valve.status().pulses == 0);
    valve.tick(5700); CHECK(valve.status().phase == Phase::Fault);
    std::printf("{\"case\":\"baseline_no_edges\",\"speed_permille\":%u,\"raw\":253,\"job_pulses\":0,\"close_after_ms\":5000,\"reason\":\"NO_FLOW\"}\n", speed);
  }
  ValveConfig cfg; cfg.commissioned = true;
  IO io; io.reading.pulses = 253;
  FlowJobController valve(io, cfg); valve.boot({});
  Dose d; d.key = {315, 88}; d.ml = 250; d.k = 5880; d.version = 1; d.maxMs = 60000;
  CHECK(targetPulses(d.ml, d.k, d.pulses));
  CHECK(valve.prepare(d, 100) == Reply::Ok && valve.start(d.key, 200) == Reply::Ok);
  io.reading.pulses += 12; io.reading.lastMs = 300; valve.tick(300);
  CHECK(valve.status().pulses == 12 && valve.status().phase == Phase::Running);
  valve.tick(1799); CHECK(io.on); valve.tick(1800);
  CHECK(!io.on && valve.status().reason == Reason::NoFlow && valve.status().pulses == 12);
  std::puts("{\"case\":\"new_edges_are_counted\",\"baseline\":253,\"raw\":265,\"job_pulses\":12,\"stop_after_last_pulse_ms\":1500}");
}

void startup_delay(uint32_t delay) {
  Bench b(1); b.pulsesPerTick = 0; b.startReplyDelay = delay;
  b.slaves[0]->io.reading.pulses = 253; b.begin(1);
  uint32_t opened = 0, closed = 0, pumpStopped = 0;
  while (b.scheduler.active() && b.now < 15000) {
    const bool wasPump = b.io.on;
    b.step();
    if (b.slaves[0]->io.on && !opened) opened = b.now;
    if (opened && !b.slaves[0]->io.on && !closed) closed = b.now;
    if (wasPump && !b.io.on && !pumpStopped) pumpStopped = b.now;
  }
  const auto s = b.slaves[0]->valve.status();
  CHECK(opened && closed && b.pumpStartedAt && closed - opened == 5000);
  CHECK(s.pulses == 0 && s.reason == Reason::NoFlow && !b.io.on);
  std::printf("{\"case\":\"startup_no_edges\",\"injected_start_reply_delay_ms\":%u,\"valve_open_at\":%u,\"pump_on_at\":%u,\"valve_close_at\":%u,\"pump_off_at\":%u,\"pump_to_valve_close_ms\":%u,\"master_reason\":\"%s\",\"slave_reason\":\"%s\"}\n",
    delay, opened, b.pumpStartedAt, closed, pumpStopped, closed - b.pumpStartedAt,
    reasonName(b.scheduler.report().reason), reasonName(s.reason));
}

void stale_before_terminal_result() {
  Bench b(1); b.pulsesPerTick = 0;
  // Intentional fault injection: withhold Result so a post-close LeaseAck
  // reaches the master first. This is a code-path proof, not RF emulation.
  b.dropResultsUntilQuery = true; b.begin(1);
  uint32_t closed = 0, stopped = 0;
  while (b.scheduler.active() && b.now < 20000) {
    const bool wasPump = b.io.on;
    b.step();
    if (b.pumpStartedAt && !b.slaves[0]->io.on && !closed) closed = b.now;
    if (wasPump && !b.io.on && !stopped) {
      stopped = b.now;
      CHECK(b.slaves[0]->valve.status().reason == Reason::NoFlow);
      CHECK(b.scheduler.report().reason == Reason::Lease);
      CHECK(stopped - b.scheduler.node(0).leaseAckAt < LeaseAckTimeoutMs);
    }
  }
  CHECK(closed && stopped && !b.scheduler.active() && !b.io.on);
  CHECK(b.scheduler.report().reason == Reason::Lease);
  CHECK(b.scheduler.node(0).status.reason == Reason::NoFlow);
  CHECK(b.scheduler.report().unknown == 0);
  std::printf("{\"case\":\"stale_ack_precedes_result\",\"valve_close_at\":%u,\"pump_off_at\":%u,\"master_reason\":\"LEASE\",\"slave_reason\":\"NO_FLOW\",\"late_result_reconciled\":true,\"ack_timeout_required\":false}\n", closed, stopped);
}

int main() {
  baseline_and_speed();
  for (uint32_t delay : {0U, 1500U, 3800U}) startup_delay(delay);
  stale_before_terminal_result();
  std::puts("{\"all_diagnostic_probes_passed\":true}");
  return 0;
}
