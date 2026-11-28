#pragma once

#include "sorter_core.h"

#include <cstddef>
#include <cstdint>

namespace sorter_core {

// SORT-R01 / SORT-AC01: 7-state deterministic Core 0 machine state.
enum class SystemState { Init, Idle, Running, Manual, Calib, Fault, Estop };

enum class FaultCode {
    None,
    SensorInitFailed,
    SensorTimeout,
    ItemJam,
    Overheat,
    EstopActive
};

struct TransitionResult {
    bool ok = false;
    const char* reason = "OK";
};

class SystemFsm {
public:
    constexpr SystemState state() const noexcept { return state_; }
    constexpr FaultCode fault() const noexcept { return fault_; }
    constexpr bool estop_active() const noexcept { return estop_active_; }

    constexpr bool outputs_allowed() const noexcept {
        return !estop_active_ &&
               (state_ == SystemState::Running || state_ == SystemState::Manual);
    }

    constexpr TransitionResult init_ok() noexcept {
        if (state_ != SystemState::Init) return {false, "INVALID_STATE"};
        state_ = SystemState::Idle;
        fault_ = FaultCode::None;
        return {true, "OK"};
    }

    constexpr TransitionResult init_fail(FaultCode code = FaultCode::SensorInitFailed) noexcept {
        if (state_ != SystemState::Init) return {false, "INVALID_STATE"};
        state_ = SystemState::Fault;
        fault_ = code;
        return {true, "OK"};
    }

    constexpr TransitionResult request_start() noexcept {
        if (estop_active_ || state_ == SystemState::Estop) return {false, "ESTOP_ACTIVE"};
        if (state_ != SystemState::Idle) return {false, "INVALID_STATE"};
        state_ = SystemState::Running;
        return {true, "OK"};
    }

    constexpr TransitionResult request_stop() noexcept {
        if (state_ != SystemState::Running) return {false, "INVALID_STATE"};
        state_ = SystemState::Idle;
        return {true, "OK"};
    }

    // Belt must be stopped (Idle) before entering Manual or Calib (SORT-R01).
    constexpr TransitionResult request_manual() noexcept {
        if (estop_active_ || state_ == SystemState::Estop) return {false, "ESTOP_ACTIVE"};
        if (state_ != SystemState::Idle) return {false, "INVALID_STATE"};
        state_ = SystemState::Manual;
        return {true, "OK"};
    }

    constexpr TransitionResult exit_manual() noexcept {
        if (state_ != SystemState::Manual) return {false, "INVALID_STATE"};
        state_ = SystemState::Idle;
        return {true, "OK"};
    }

    constexpr TransitionResult request_calib() noexcept {
        if (estop_active_ || state_ == SystemState::Estop) return {false, "ESTOP_ACTIVE"};
        if (state_ != SystemState::Idle) return {false, "INVALID_STATE"};
        state_ = SystemState::Calib;
        return {true, "OK"};
    }

    constexpr TransitionResult exit_calib() noexcept {
        if (state_ != SystemState::Calib) return {false, "INVALID_STATE"};
        state_ = SystemState::Idle;
        return {true, "OK"};
    }

    constexpr TransitionResult trigger_fault(FaultCode code) noexcept {
        if (state_ == SystemState::Estop) return {false, "ESTOP_ACTIVE"};
        state_ = SystemState::Fault;
        fault_ = code;
        return {true, "OK"};
    }

    constexpr TransitionResult trigger_estop() noexcept {
        estop_active_ = true;
        state_ = SystemState::Estop;
        fault_ = FaultCode::EstopActive;
        return {true, "OK"};
    }

    constexpr void release_estop() noexcept { estop_active_ = false; }

    // NC inputs read high when pressed or disconnected.
    constexpr void update_inputs(bool stop_open, bool estop_open) noexcept {
        if (estop_open) trigger_estop();
        else release_estop();
        if (stop_open || estop_open) request_stop();
    }

    // ESTOP can only clear via physical RESET after releasing E-STOP (SORT-R01).
    constexpr TransitionResult request_reset(bool is_hard_button) noexcept {
        if (state_ == SystemState::Estop) {
            if (estop_active_) return {false, "ESTOP_ACTIVE"};
            if (!is_hard_button) return {false, "HARD_RESET_REQUIRED"};
            state_ = SystemState::Idle;
            fault_ = FaultCode::None;
            return {true, "OK"};
        }
        if (state_ == SystemState::Fault) {
            state_ = SystemState::Idle;
            fault_ = FaultCode::None;
            return {true, "OK"};
        }
        return {false, "INVALID_STATE"};
    }

private:
    SystemState state_ = SystemState::Init;
    FaultCode fault_ = FaultCode::None;
    bool estop_active_ = false;
};

// SORT-R06: dynamic trigger delay from t0 to solenoid activation.
struct TriggerGeometry {
    double lp_mm = 0.0;
    double e_det_mm = 0.0;
    double item_len_mm = 0.0;
    double velocity_mm_per_s = 0.0;
    double t_act_ms = 0.0;
    std::uint32_t fallback_delay_ms = 0;
};

constexpr std::uint32_t compute_trigger_delay_ms(const TriggerGeometry& g) noexcept {
    if (!is_finite(g.lp_mm) || !is_finite(g.e_det_mm) || !is_finite(g.item_len_mm) ||
        !is_finite(g.velocity_mm_per_s) || !is_finite(g.t_act_ms) ||
        g.velocity_mm_per_s <= 0.0 || g.lp_mm < 0.0 || g.item_len_mm < 0.0 ||
        g.t_act_ms < 0.0) {
        return g.fallback_delay_ms;
    }
    const double raw_ms =
        ((g.lp_mm + g.e_det_mm + 0.5 * g.item_len_mm) / g.velocity_mm_per_s) * 1000.0 -
        g.t_act_ms;
    if (!is_finite(raw_ms) || raw_ms <= 0.0) return 0u;
    return static_cast<std::uint32_t>(raw_ms);
}

constexpr std::size_t MAX_ACTUATOR_QUEUE = 8;
// ponytail: fixed 64-pulse thermal ring buffer; raise if <200ms pulses exceed 64/window.
constexpr std::size_t MAX_THERMAL_HISTORY = 64;

struct SolenoidConfig {
    std::uint32_t on_time_ms = 500;
    std::uint32_t max_on_time_ms = 600;
    std::uint32_t window_ms = 60000;
    double rated_duty = 0.25;
    double warn_fraction = 0.80;
    std::uint32_t min_off_time_ms = 0;
};

struct ActuatorTick {
    bool solenoid_on = false;
    bool changed = false;
    bool fired_now = false;
    bool overheat_warn = false;
    bool overheat_fault = false;
    double duty_ratio = 0.0;
};

// SORT-R06 / SORT-R07 / SORT-AC06: per-station FIFO trigger queue, t_on_max clamp,
// and sliding-window thermal duty-cycle guard.
class SolenoidActuator {
public:
    constexpr explicit SolenoidActuator(const SolenoidConfig& cfg = {}) noexcept : cfg_(cfg) {}

    constexpr bool is_on() const noexcept { return active_; }
    constexpr std::size_t queue_size() const noexcept { return q_count_; }

    constexpr void set_config(const SolenoidConfig& cfg) noexcept { cfg_ = cfg; }

    constexpr void clear_queue() noexcept {
        q_head_ = 0;
        q_tail_ = 0;
        q_count_ = 0;
    }

    constexpr bool enqueue(std::uint32_t t0_ms, std::uint32_t delay_ms) noexcept {
        if (q_count_ >= MAX_ACTUATOR_QUEUE) return false;
        q_fire_ms_[q_tail_] = t0_ms + delay_ms;
        q_tail_ = (q_tail_ + 1) % MAX_ACTUATOR_QUEUE;
        ++q_count_;
        return true;
    }

    constexpr bool trigger_manual(std::uint32_t now_ms, std::uint32_t duration_ms) noexcept {
        if (active_) return false;
        active_ = true;
        active_start_ms_ = now_ms;
        active_duration_ms_ = clamp_on_time_(duration_ms);
        return true;
    }

    constexpr ActuatorTick tick(std::uint32_t now_ms, bool allow_output) noexcept {
        const bool was_on = active_;
        bool fired_now = false;
        bool just_released = false;

        // Release completed pulse before checking queue or thermal headroom.
        if (active_ &&
            static_cast<std::int32_t>(now_ms - active_start_ms_) >=
                static_cast<std::int32_t>(active_duration_ms_)) {
            record_pulse_(active_start_ms_, now_ms);
            active_ = false;
            last_off_ms_ = now_ms;
            has_released_ = true;
            just_released = true;
        }

        double used_ms = compute_window_on_ms_(now_ms);
        const double rated_on_ms =
            (cfg_.window_ms > 0 && is_finite(cfg_.rated_duty) && cfg_.rated_duty > 0.0)
                ? cfg_.rated_duty * static_cast<double>(cfg_.window_ms)
                : 0.0;
        bool overheat_fault = rated_on_ms > 0.0 && used_ms > rated_on_ms;

        if (!allow_output || overheat_fault) {
            if (active_) {
                record_pulse_(active_start_ms_, now_ms);
                active_ = false;
                last_off_ms_ = now_ms;
                has_released_ = true;
                just_released = true;
            }
            clear_queue();
        } else if (!active_ && !just_released && q_count_ > 0) {
            const bool retract_done =
                !has_released_ ||
                static_cast<std::int32_t>(now_ms - last_off_ms_) >=
                    static_cast<std::int32_t>(cfg_.min_off_time_ms);
            const std::uint32_t fire_at = q_fire_ms_[q_head_];
            if (retract_done && static_cast<std::int32_t>(now_ms - fire_at) >= 0) {
                q_head_ = (q_head_ + 1) % MAX_ACTUATOR_QUEUE;
                --q_count_;
                const std::uint32_t dur = clamp_on_time_(cfg_.on_time_ms);
                if (dur > 0) {
                    active_ = true;
                    active_start_ms_ = now_ms;
                    active_duration_ms_ = dur;
                    fired_now = true;
                }
            }
        }

        used_ms = compute_window_on_ms_(now_ms);
        const double duty_ratio = rated_on_ms > 0.0 ? (used_ms / rated_on_ms) : 0.0;
        overheat_fault = rated_on_ms > 0.0 && used_ms > rated_on_ms;
        const bool overheat_warn =
            rated_on_ms > 0.0 && is_finite(cfg_.warn_fraction) &&
            used_ms > cfg_.warn_fraction * rated_on_ms;

        return ActuatorTick{
            active_,
            active_ != was_on,
            fired_now,
            overheat_warn,
            overheat_fault,
            duty_ratio,
        };
    }

private:
    struct PulseRecord {
        std::uint32_t start_ms = 0;
        std::uint32_t end_ms = 0;
        bool valid = false;
    };

    constexpr std::uint32_t clamp_on_time_(std::uint32_t requested_ms) const noexcept {
        if (cfg_.max_on_time_ms == 0) return requested_ms;
        return requested_ms < cfg_.max_on_time_ms ? requested_ms : cfg_.max_on_time_ms;
    }

    constexpr void record_pulse_(std::uint32_t start_ms, std::uint32_t end_ms) noexcept {
        if (static_cast<std::int32_t>(end_ms - start_ms) <= 0) return;
        history_[hist_head_] = PulseRecord{start_ms, end_ms, true};
        hist_head_ = (hist_head_ + 1) % MAX_THERMAL_HISTORY;
    }

    static constexpr double overlap_ms_(std::uint32_t a, std::uint32_t b,
                                        std::uint32_t win_start,
                                        std::uint32_t win_end) noexcept {
        const std::uint32_t s = a > win_start ? a : win_start;
        const std::uint32_t e = b < win_end ? b : win_end;
        return e > s ? static_cast<double>(e - s) : 0.0;
    }

    constexpr double compute_window_on_ms_(std::uint32_t now_ms) const noexcept {
        if (cfg_.window_ms == 0) return 0.0;
        const std::uint32_t win_start = now_ms > cfg_.window_ms ? (now_ms - cfg_.window_ms) : 0u;
        double total = 0.0;
        for (std::size_t i = 0; i < MAX_THERMAL_HISTORY; ++i) {
            if (history_[i].valid) {
                total += overlap_ms_(history_[i].start_ms, history_[i].end_ms, win_start, now_ms);
            }
        }
        if (active_) {
            total += overlap_ms_(active_start_ms_, now_ms, win_start, now_ms);
        }
        return total;
    }

    SolenoidConfig cfg_{};
    bool active_ = false;
    bool has_released_ = false;
    std::uint32_t active_start_ms_ = 0;
    std::uint32_t active_duration_ms_ = 0;
    std::uint32_t last_off_ms_ = 0;

    std::uint32_t q_fire_ms_[MAX_ACTUATOR_QUEUE]{};
    std::size_t q_head_ = 0;
    std::size_t q_tail_ = 0;
    std::size_t q_count_ = 0;

    PulseRecord history_[MAX_THERMAL_HISTORY]{};
    std::size_t hist_head_ = 0;
};

}  // namespace sorter_core
