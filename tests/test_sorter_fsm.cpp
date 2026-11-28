// Compile-time verification (no EXE needed, works under Windows Application Control):
//   g++ -std=c++17 -Wall -Wextra -Werror -fsyntax-only tests/test_sorter_fsm.cpp
// Full host binary (WSL/Linux/CI):
//   g++ -std=c++17 -Wall -Wextra -Werror tests/test_sorter_fsm.cpp -o test_sorter_fsm && ./test_sorter_fsm
#include "../sorter_fsm.h"

#include <cassert>

namespace {

using namespace sorter_core;

constexpr bool test_nc_controls() {
    SystemFsm fsm;
    if (!fsm.init_ok().ok || !fsm.request_start().ok) return false;
    fsm.update_inputs(true, false);
    if (fsm.state() != SystemState::Idle) return false;
    if (!fsm.request_start().ok) return false;
    fsm.update_inputs(false, true);
    if (fsm.state() != SystemState::Estop || fsm.request_start().ok) return false;
    fsm.update_inputs(false, false);
    if (fsm.request_start().ok || fsm.request_reset(false).ok) return false;
    return fsm.request_reset(true).ok && fsm.state() == SystemState::Idle;
}
static_assert(test_nc_controls(), "NC stop and estop must stop and latch");

constexpr bool test_system_fsm_sort_ac01() noexcept {
    SystemFsm fsm;
    if (fsm.state() != SystemState::Init || fsm.outputs_allowed()) return false;

    if (!fsm.init_ok().ok || fsm.state() != SystemState::Idle) return false;
    if (!fsm.request_start().ok || fsm.state() != SystemState::Running) return false;
    if (!fsm.outputs_allowed()) return false;

    // Cannot switch to Manual or Calib while Running (SORT-R01 / SORT-AC01).
    if (fsm.request_manual().ok || fsm.state() != SystemState::Running) return false;
    if (fsm.request_calib().ok || fsm.state() != SystemState::Running) return false;

    if (!fsm.request_stop().ok || fsm.state() != SystemState::Idle) return false;
    if (fsm.outputs_allowed()) return false;

    if (!fsm.request_manual().ok || fsm.state() != SystemState::Manual) return false;
    if (!fsm.outputs_allowed()) return false;
    if (!fsm.exit_manual().ok || fsm.state() != SystemState::Idle) return false;

    if (!fsm.request_calib().ok || fsm.state() != SystemState::Calib) return false;
    if (fsm.outputs_allowed()) return false;
    if (!fsm.exit_calib().ok || fsm.state() != SystemState::Idle) return false;

    // E-STOP overrides Running, locks Start, requires release + hard button RESET.
    if (!fsm.request_start().ok) return false;
    if (!fsm.trigger_estop().ok || fsm.state() != SystemState::Estop) return false;
    if (fsm.outputs_allowed() || !fsm.estop_active()) return false;
    if (fsm.request_start().ok) return false;
    if (fsm.request_reset(true).ok) return false;

    fsm.release_estop();
    if (fsm.state() != SystemState::Estop) return false;
    if (fsm.request_reset(false).ok) return false;  // Remote/soft reset must fail.
    if (!fsm.request_reset(true).ok || fsm.state() != SystemState::Idle) return false;

    // Hardware/sensor Fault stops outputs and resets cleanly to Idle.
    if (!fsm.request_start().ok) return false;
    if (!fsm.trigger_fault(FaultCode::SensorTimeout).ok) return false;
    if (fsm.state() != SystemState::Fault || fsm.fault() != FaultCode::SensorTimeout) return false;
    if (fsm.outputs_allowed()) return false;
    if (!fsm.request_reset(false).ok || fsm.state() != SystemState::Idle) return false;
    if (fsm.fault() != FaultCode::None) return false;

    return true;
}

constexpr bool test_trigger_delay_and_fifo_sort_r06() noexcept {
    // td = ((120 + 0 + 20) / 200) * 1000 - 20 = 680 ms
    const TriggerGeometry g{120.0, 0.0, 40.0, 200.0, 20.0, 999};
    if (compute_trigger_delay_ms(g) != 680u) return false;

    // Clamp negative td to 0 and fall back when velocity <= 0.
    const TriggerGeometry neg{10.0, 0.0, 10.0, 1000.0, 50.0, 123};
    if (compute_trigger_delay_ms(neg) != 0u) return false;
    const TriggerGeometry no_vel{120.0, 0.0, 40.0, 0.0, 20.0, 345};
    if (compute_trigger_delay_ms(no_vel) != 345u) return false;

    SolenoidActuator act(SolenoidConfig{100, 600, 60000, 0.25, 0.80});
    if (!act.enqueue(1000, 680)) return false;  // fires at 1680
    if (!act.enqueue(2000, 680)) return false;  // fires at 2680

    if (act.tick(1679, true).solenoid_on) return false;
    const ActuatorTick fire1 = act.tick(1680, true);
    if (!fire1.solenoid_on || !fire1.changed || !fire1.fired_now) return false;
    if (!act.tick(1779, true).solenoid_on) return false;
    const ActuatorTick off1 = act.tick(1780, true);
    if (off1.solenoid_on || !off1.changed) return false;

    if (!act.tick(2680, true).fired_now || !act.is_on()) return false;
    if (act.tick(2780, true).solenoid_on) return false;
    return act.queue_size() == 0;
}

constexpr bool test_thermal_and_ton_cap_sort_ac06() noexcept {
    // 1. Clamp requested on_time_ms (900) to max_on_time_ms (600) in auto and manual.
    SolenoidActuator cap_act(SolenoidConfig{900, 600, 60000, 0.50, 0.80});
    if (!cap_act.enqueue(100, 0)) return false;
    if (!cap_act.tick(100, true).solenoid_on) return false;
    if (!cap_act.tick(699, true).solenoid_on) return false;
    if (cap_act.tick(700, true).solenoid_on) return false;

    if (!cap_act.trigger_manual(1000, 900)) return false;
    if (!cap_act.tick(1599, true).solenoid_on) return false;
    if (cap_act.tick(1600, true).solenoid_on) return false;

    // 2. Thermal sliding window: window=1000ms, rated_duty=0.40 => rated=400ms, warn>320ms.
    SolenoidActuator therm(SolenoidConfig{200, 600, 1000, 0.40, 0.80});
    // Pulse 1: [100, 300] -> 200ms (50%)
    if (!therm.enqueue(100, 0)) return false;
    if (!therm.tick(100, true).solenoid_on) return false;
    const ActuatorTick p1 = therm.tick(300, true);
    if (p1.solenoid_on || p1.overheat_warn || p1.overheat_fault) return false;

    // Pulse 2: [400, 550] -> cumulative 350ms (> 320ms warn, <= 400ms fault)
    if (!therm.trigger_manual(400, 200)) return false;
    const ActuatorTick mid2 = therm.tick(550, true);
    if (!mid2.solenoid_on || !mid2.overheat_warn || mid2.overheat_fault) return false;

    // Still on at 601 -> cumulative [100,300] + [400,601] = 401ms > 400ms -> overheat fault!
    if (!therm.enqueue(800, 0)) return false;  // queued item must be dropped on fault
    const ActuatorTick fault2 = therm.tick(601, true);
    if (fault2.solenoid_on || !fault2.overheat_fault || therm.queue_size() != 0) return false;

    // After sliding window expires (t = 1700, window=[700, 1700]), duty returns to 0.
    const ActuatorTick cooled = therm.tick(1700, true);
    if (cooled.overheat_warn || cooled.overheat_fault || cooled.duty_ratio != 0.0) return false;

    return true;
}

constexpr bool test_back_to_back_retract_gap() noexcept {
    // Back-to-back items must force an OFF retract phase between pushes.
    SolenoidActuator act(SolenoidConfig{200, 600, 60000, 0.50, 0.80, 120});
    if (!act.enqueue(100, 0) || !act.enqueue(200, 0)) return false;

    const ActuatorTick f1 = act.tick(100, true);
    if (!f1.solenoid_on || !f1.changed || !f1.fired_now) return false;

    const ActuatorTick rel1 = act.tick(300, true);
    if (rel1.solenoid_on || !rel1.changed || rel1.fired_now || act.queue_size() != 1) return false;

    const ActuatorTick wait1 = act.tick(419, true);
    if (wait1.solenoid_on || wait1.changed || act.queue_size() != 1) return false;

    const ActuatorTick f2 = act.tick(420, true);
    if (!f2.solenoid_on || !f2.changed || !f2.fired_now || act.queue_size() != 0) return false;

    const ActuatorTick rel2 = act.tick(620, true);
    if (rel2.solenoid_on || !rel2.changed) return false;

    // Even with min_off_time_ms == 0, finishing pulse 1 must emit an OFF transition.
    SolenoidActuator zero_gap(SolenoidConfig{200, 600, 60000, 0.50, 0.80, 0});
    if (!zero_gap.enqueue(100, 0) || !zero_gap.enqueue(200, 0)) return false;
    if (!zero_gap.tick(100, true).solenoid_on) return false;
    const ActuatorTick zrel = zero_gap.tick(300, true);
    if (zrel.solenoid_on || !zrel.changed || zero_gap.queue_size() != 1) return false;
    const ActuatorTick zf2 = zero_gap.tick(301, true);
    if (!zf2.solenoid_on || !zf2.changed || !zf2.fired_now || zero_gap.queue_size() != 0) return false;

    // set_config dynamically updates on_time_ms and min_off_time_ms.
    zero_gap.set_config(SolenoidConfig{100, 600, 60000, 0.50, 0.80, 50});
    if (zero_gap.tick(501, true).solenoid_on) return false;  // ends at +200 from prior pulse
    if (!zero_gap.enqueue(600, 0)) return false;
    if (!zero_gap.tick(600, true).solenoid_on) return false;
    if (zero_gap.tick(699, true).solenoid_on == false) return false;
    if (zero_gap.tick(700, true).solenoid_on) return false;
    return true;
}

static_assert(test_system_fsm_sort_ac01(), "SORT-AC01 failed");
static_assert(test_trigger_delay_and_fifo_sort_r06(), "SORT-R06 failed");
static_assert(test_thermal_and_ton_cap_sort_ac06(), "SORT-AC06 failed");
static_assert(test_back_to_back_retract_gap(), "Back-to-back retract gap failed");

}  // namespace

int main() {
    assert(test_system_fsm_sort_ac01());
    assert(test_trigger_delay_and_fifo_sort_r06());
    assert(test_thermal_and_ton_cap_sort_ac06());
    assert(test_back_to_back_retract_gap());
    return 0;
}
