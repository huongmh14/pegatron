#pragma once

#include <cstddef>
#include <cstdint>

// Each ToF passage reserves one FIFO ticket; only that ticket can receive its result.
// A debounced LM393 arrival consumes one ticket, including pending/pass/busy items.
// ponytail: identity is FIFO order, not physical tracking; a missed/double ToF detection
// within the travel window cannot be proven. Upgrade: encoder-based item tracking.
enum class TrialStage { Idle, Extended, Recovery };
enum class TrialEvent { None, Push, Passed, Returned, Ready, TimedOut, Unmatched,
                        SensorLost, NotReady, Busy, SyncLost };

struct Station1TrialGate {
    static constexpr std::size_t QUEUE_MAX = 8;

    // Calibratable timings (ms). Defaults are starting values; tune on the bench.
    std::uint32_t position_stable_ms = 10;  // LM393 must stay active this long (noise filter)
    std::uint32_t wait_limit_ms = 1500;     // max time from ToF start to LM393; expiry loses synchronization
    std::uint32_t push_ms = 160;            // servo held at PUSH (SG90 ~150 ms for 90 deg at 5 V)
    std::uint32_t recover_ms = 150;         // time for the servo to retract to HOME
    std::uint32_t clear_ms = 30;            // LM393 empty continuously before the next item counts

    TrialStage stage = TrialStage::Idle;
    std::uint32_t stage_since = 0;
    std::uint32_t position_since = 0;
    bool position_timing = false;
    bool need_release = false;  // consumed item still in front of the LM393
    std::uint32_t release_since = 0;
    bool release_timing = false;

    bool synchronized = true;
    bool report_sync_loss = false;
    std::uint32_t next_ticket = 0;  // Never reset: late completion cannot match a reused slot.
    bool q_push[QUEUE_MAX]{};
    bool q_ready[QUEUE_MAX]{};
    std::uint32_t q_ticket[QUEUE_MAX]{};
    std::uint32_t q_time[QUEUE_MAX]{};
    std::size_t q_head = 0;
    std::size_t q_count = 0;

    constexpr void reset() {
        stage = TrialStage::Idle;
        position_timing = false;
        need_release = false;
        release_timing = false;
        q_head = 0;
        q_count = 0;
        synchronized = true;
        report_sync_loss = false;
    }

    constexpr std::uint32_t reserve(std::uint32_t now) {
        if (!synchronized) return 0;
        if (q_count == QUEUE_MAX) {
            lose_sync();
            report_sync_loss = true;
            return 0;
        }
        if (++next_ticket == 0) ++next_ticket;
        const std::size_t i = (q_head + q_count) % QUEUE_MAX;
        q_push[i] = false;
        q_ready[i] = false;
        q_ticket[i] = next_ticket;
        q_time[i] = now;
        ++q_count;
        return next_ticket;
    }

    constexpr bool complete(std::uint32_t ticket, bool push) {
        if (!synchronized || ticket == 0) return false;
        for (std::size_t n = 0; n < q_count; ++n) {
            const std::size_t i = (q_head + n) % QUEUE_MAX;
            if (q_ticket[i] == ticket && !q_ready[i]) {
                q_push[i] = push;
                q_ready[i] = true;
                return true;
            }
        }
        return false;  // Already consumed, expired or reset: never enqueue a late result.
    }

    constexpr bool enqueue(bool push, std::uint32_t now) {
        const auto ticket = reserve(now);
        return complete(ticket, push);
    }

    // Bench/manual push: only from Idle, bypasses ToF+LM393. Caller must gate on a safe state.
    constexpr bool force_push(std::uint32_t now) {
        if (!synchronized || stage != TrialStage::Idle) return false;
        stage = TrialStage::Extended;
        stage_since = now;
        return true;
    }

    constexpr TrialEvent tick(std::uint32_t now, bool enabled, bool tof_ok, bool position_active) {
        if (!enabled) {
            reset();
            return TrialEvent::None;
        }
        if (!tof_ok) {
            const bool was_synchronized = synchronized;
            lose_sync();
            return was_synchronized ? TrialEvent::SensorLost : TrialEvent::None;
        }

        if (!synchronized) {
            const bool report = report_sync_loss;
            report_sync_loss = false;
            return report ? TrialEvent::SyncLost : TrialEvent::None;
        }
        TrialEvent transition = TrialEvent::None;
        if (stage == TrialStage::Extended && now - stage_since >= push_ms) {
            stage = TrialStage::Recovery;
            stage_since = now;
            transition = TrialEvent::Returned;
        } else if (stage == TrialStage::Recovery && now - stage_since >= recover_ms) {
            stage = TrialStage::Idle;
            transition = TrialEvent::Ready;
        }
        if (q_count > 0 && now - q_time[q_head] >= wait_limit_ms) {
            lose_sync();
            return TrialEvent::TimedOut;
        }
        if (need_release) {
            if (position_active) {
                release_timing = false;
            } else {
                if (!release_timing) {
                    release_timing = true;
                    release_since = now;
                }
                if (now - release_since >= clear_ms) need_release = false;
            }
            return transition;
        }
        if (!position_active) {
            position_timing = false;
            return transition;
        }
        if (!position_timing) {
            position_timing = true;
            position_since = now;
        }
        if (now - position_since < position_stable_ms) return transition;
        position_timing = false;
        need_release = true;
        release_timing = false;
        if (q_count == 0) {
            lose_sync();
            return TrialEvent::Unmatched;
        }
        const bool ready = q_ready[q_head];
        const bool push = q_push[q_head];
        pop();
        if (!ready) return TrialEvent::NotReady;
        if (!push) return TrialEvent::Passed;
        if (stage != TrialStage::Idle) return TrialEvent::Busy;
        stage = TrialStage::Extended;
        stage_since = now;
        return TrialEvent::Push;
    }

private:
    constexpr void lose_sync() {
        stage = TrialStage::Idle;
        q_count = 0;
        synchronized = false;
    }

    constexpr void pop() {
        q_head = (q_head + 1) % QUEUE_MAX;
        --q_count;
    }
};
