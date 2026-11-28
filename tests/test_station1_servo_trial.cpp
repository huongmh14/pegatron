// Compile-time check (works where EXE is blocked):
//   g++ -std=c++17 -Wall -Wextra -Werror -fsyntax-only tests/test_station1_servo_trial.cpp
#include "../station1_servo_trial_logic.h"
#include "../sorter_core.h"

namespace {
using E = TrialEvent;
using S = TrialStage;

// Runs the gate from t0 to t1 (1 ms steps) with LM393 active in [on, off). Counts events.
struct Run {
    int push = 0, passed = 0, timed_out = 0, unmatched = 0, ready = 0;
};
constexpr void run(Station1TrialGate& g, Run& r, std::uint32_t t0, std::uint32_t t1,
                   std::uint32_t on, std::uint32_t off) {
    for (std::uint32_t t = t0; t < t1; ++t) {
        switch (g.tick(t, true, true, t >= on && t < off)) {
        case E::Push: ++r.push; break;
        case E::Passed: ++r.passed; break;
        case E::TimedOut: ++r.timed_out; break;
        case E::Unmatched: ++r.unmatched; break;
        case E::Ready: ++r.ready; break;
        default: break;
        }
    }
}

// Fixed timings so the scenarios below don't change when the firmware defaults are retuned.
constexpr Station1TrialGate base() {
    Station1TrialGate g;
    g.position_stable_ms = 20;
    g.wait_limit_ms = 1500;
    g.push_ms = 250;
    g.recover_ms = 200;
    g.clear_ms = 60;
    return g;
}

constexpr bool l_pushes_once_with_set_timings() {
    Station1TrialGate g = base();
    Run r;
    g.enqueue(true, 0);
    run(g, r, 0, 119, 100, 400);
    if (r.push != 0) return false;  // LM393 active 19 ms < 20 ms
    if (g.tick(120, true, true, true) != E::Push || g.stage != S::Extended) return false;
    if (g.tick(369, true, true, true) != E::None) return false;
    if (g.tick(370, true, true, true) != E::Returned) return false;  // 250 ms hold
    if (g.tick(569, true, true, true) != E::None) return false;
    if (g.tick(570, true, true, true) != E::Ready || g.stage != S::Idle) return false;  // 200 ms retract
    run(g, r, 571, 2000, 100, 400);
    return r.push == 0 && r.unmatched == 0 && g.q_count == 0;
}

constexpr bool early_lm393_retires_pending_ticket() {
    auto g = base();
    const auto early = g.reserve(0);
    const auto next = g.reserve(10);
    g.tick(100, true, true, true);
    if (g.tick(120, true, true, true) != E::NotReady || g.q_count != 1) return false;
    if (g.complete(early, true) || !g.complete(next, false)) return false;
    if (g.complete(next, true)) return false; // a decision is immutable
    g.tick(200, true, true, false);
    g.tick(260, true, true, false);
    g.tick(300, true, true, true);
    return g.tick(320, true, true, true) == E::Passed && g.q_count == 0 && g.synchronized;
}

constexpr bool s_m_pass_without_push() {
    Station1TrialGate g = base();
    Run r;
    g.enqueue(false, 0);
    run(g, r, 0, 1000, 100, 400);
    return r.push == 0 && r.passed == 1 && g.stage == S::Idle;
}

constexpr bool queue_keeps_order_l_m_l() {  // three items in flight before the first arrives
    Station1TrialGate g = base();
    Run r;
    g.enqueue(true, 0);
    g.enqueue(false, 10);
    g.enqueue(true, 20);
    run(g, r, 0, 700, 100, 400);
    if (r.push != 1 || r.passed != 0) return false;
    run(g, r, 700, 1100, 700, 900);
    if (r.passed != 1) return false;
    run(g, r, 1100, 2000, 1100, 1300);
    return r.push == 2 && r.timed_out == 0 && g.q_count == 0;
}

constexpr bool two_stations_keep_separate_queues() {
    Station1TrialGate first = base(), second = base();
    Run r1, r2;
    first.enqueue(true, 0);    // L at station 1
    second.enqueue(false, 0);  // L missed station 1, must pass station 2
    second.enqueue(true, 10);  // S must push at station 2
    run(first, r1, 0, 800, 100, 250);
    run(second, r2, 0, 400, 100, 200);
    run(second, r2, 400, 1000, 500, 600);
    return r1.push == 1 && r1.passed == 0 && first.q_count == 0 &&
           r2.push == 1 && r2.passed == 1 && r2.timed_out == 0 && second.q_count == 0;
}

constexpr bool lm393_never_arrives_times_out() {
    Station1TrialGate g = base();
    Run r;
    g.enqueue(true, 0);
    run(g, r, 0, 1499, 9999, 9999);
    if (r.timed_out != 0) return false;
    run(g, r, 1499, 1600, 9999, 9999);
    return r.timed_out == 1 && r.push == 0 && g.q_count == 0;
}

constexpr bool lm393_noise_is_ignored() {
    Station1TrialGate g = base();
    Run r;
    g.enqueue(true, 0);
    run(g, r, 0, 100, 10, 29);   // 19 ms glitch
    run(g, r, 100, 200, 130, 149);
    return r.push == 0 && r.unmatched == 0 && g.q_count == 1;
}

constexpr bool item_without_tof_decision_is_unmatched() {
    Station1TrialGate g = base();
    Run r;
    run(g, r, 0, 500, 100, 300);
    return r.unmatched == 1 && r.push == 0;
}

constexpr bool stuck_active_lm393_consumes_only_one_item() {
    Station1TrialGate g = base();
    Run r;
    g.enqueue(true, 0);
    g.enqueue(true, 0);
    run(g, r, 0, 1400, 0, 5000);  // LM393 never clears
    return r.push == 1 && g.q_count == 1;
}

constexpr bool tof_loss_aborts_and_clears_queue() {
    Station1TrialGate g = base();
    g.enqueue(true, 0);
    if (g.tick(5, true, false, true) != E::SensorLost) return false;
    if (g.q_count != 0 || g.synchronized || g.tick(10, true, true, true) != E::None) return false;
    if (g.enqueue(true, 11) || g.force_push(12)) return false;
    g.tick(15, false, true, false);
    return g.synchronized && g.enqueue(true, 20);
}

constexpr bool disable_during_push_returns_idle() {
    Station1TrialGate g = base();
    Run r;
    g.enqueue(true, 0);
    run(g, r, 0, 30, 0, 999);
    if (r.push != 1 || g.stage != S::Extended) return false;
    g.tick(50, false, true, true);  // pause / E-stop mid push
    return g.stage == S::Idle && g.q_count == 0;
}

constexpr bool queue_full_is_reported() {
    Station1TrialGate g = base();
    for (std::size_t i = 0; i < Station1TrialGate::QUEUE_MAX; ++i)
        if (!g.enqueue(true, 0)) return false;
    return !g.enqueue(true, 0);
}

constexpr bool manual_push_only_from_idle() {
    Station1TrialGate g = base();
    if (!g.force_push(0) || g.force_push(1)) return false;
    return g.tick(250, true, true, false) == E::Returned;
}

constexpr bool arrivals_during_push_and_recovery_are_consumed() {
    auto g = base(); g.position_stable_ms = 0; g.clear_ms = 0;
    g.enqueue(true, 0); g.enqueue(true, 0); g.enqueue(false, 0); g.enqueue(true, 0);
    if (g.tick(0, true, true, true) != E::Push) return false;
    g.tick(10, true, true, false);
    if (g.tick(11, true, true, true) != E::Busy || g.q_count != 2) return false;
    g.tick(12, true, true, false);
    if (g.tick(13, true, true, true) != E::Passed || g.q_count != 1) return false;
    if (g.tick(250, true, true, true) != E::Returned) return false;
    g.tick(251, true, true, false);
    if (g.tick(252, true, true, true) != E::Busy || g.q_count != 0) return false;
    return g.tick(450, true, true, false) == E::Ready && g.stage == S::Idle;
}

constexpr bool synchronization_faults_latch_until_reset() {
    auto g = base();
    const auto old = g.reserve(0);
    g.complete(old, false); // even an expired pass entry means alignment is unknown
    if (g.tick(1500, true, true, false) != E::TimedOut || g.synchronized) return false;
    if (g.enqueue(true, 1501) || g.force_push(1501) || g.complete(old, true)) return false;
    g.reset();
    const auto fresh = g.reserve(1502);
    if (old == fresh || g.complete(old, true) || !g.complete(fresh, true)) return false;
    g.reset();
    for (std::size_t i = 0; i < g.QUEUE_MAX; ++i) g.reserve(1600);
    if (g.reserve(1600) != 0 || g.synchronized || g.q_count != 0) return false;
    if (g.tick(1601, true, true, true) != E::SyncLost) return false;
    if (g.tick(1602, true, true, true) != E::None) return false;
    g.reset();
    g.tick(1700, true, true, true);
    return g.tick(1720, true, true, true) == E::Unmatched && !g.synchronized;
}

constexpr bool gate_clock_wrap_is_safe() {
    auto g = base();
    const std::uint32_t start = 0xfffffff0U;
    g.enqueue(true, start);
    g.tick(start, true, true, true);
    if (g.tick(start + 20, true, true, true) != E::Push) return false;
    return g.tick(start + 270, true, true, true) == E::Returned;
}

constexpr bool detector_and_gate_reject_late_completion() {
    sorter_core::StationConfig c;
    c.station = 1;
    sorter_core::ItemDetector d(c);
    auto g = base();
    const auto start = d.feed(30, 0);
    if (!start.started) return false;
    const auto ticket = g.reserve(start.t0_ms);
    g.tick(10, true, true, true);
    if (g.tick(30, true, true, true) != E::NotReady) return false;
    for (std::uint32_t i = 1; i < 10; ++i) d.feed(30, 30 + i * 10);
    d.feed(100, 140);
    const auto done = d.feed(100, 150);
    return done.ready && done.action == sorter_core::Action::Push &&
           !g.complete(ticket, true) && g.q_count == 0 && g.stage == S::Idle;
}

static_assert(arrivals_during_push_and_recovery_are_consumed());
static_assert(synchronization_faults_latch_until_reset());
static_assert(gate_clock_wrap_is_safe());
static_assert(detector_and_gate_reject_late_completion());
static_assert(l_pushes_once_with_set_timings());
static_assert(early_lm393_retires_pending_ticket());
static_assert(s_m_pass_without_push());
static_assert(queue_keeps_order_l_m_l());
static_assert(two_stations_keep_separate_queues());
static_assert(lm393_never_arrives_times_out());
static_assert(lm393_noise_is_ignored());
static_assert(item_without_tof_decision_is_unmatched());
static_assert(stuck_active_lm393_consumes_only_one_item());
static_assert(tof_loss_aborts_and_clears_queue());
static_assert(disable_during_push_returns_idle());
static_assert(queue_full_is_reported());
static_assert(manual_push_only_from_idle());
}  // namespace

int main() { return 0; }
