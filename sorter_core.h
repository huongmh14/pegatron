#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace sorter_core {

enum class Size { Invalid, S, M, L };
enum class Action { Invalid, Pass, Push, Missed };

constexpr std::size_t MAX_MEDIAN_SAMPLES = 256;

constexpr bool is_finite(double x) noexcept {
    constexpr double inf = std::numeric_limits<double>::infinity();
    return x == x && x != inf && x != -inf;
}

// SORT-R04: caller supplies a calibrated height for an already detected item.
constexpr Size classify(double height, double threshold_sm, double threshold_ml) noexcept {
    if (!is_finite(height) || !is_finite(threshold_sm) ||
        !is_finite(threshold_ml) || threshold_sm >= threshold_ml)
        return Size::Invalid;
    return height <= threshold_sm ? Size::S : height <= threshold_ml ? Size::M : Size::L;
}

// L takes precedence at a shared boundary, then M. S occupies [s_min, m_min).
// Out-of-range heights must never fall back to S: station 2 now pushes S.
constexpr Size classify_ranges(double height, double m_min, double m_max,
                               double l_min, double l_max, double s_min = 5.0) noexcept {
    if (!is_finite(height) || !is_finite(m_min) || !is_finite(m_max) ||
        !is_finite(l_min) || !is_finite(l_max) ||
        !is_finite(s_min) || s_min < 0.0 || s_min >= m_min ||
        l_min < 0.0 || m_min >= m_max || l_min >= l_max) {
        return Size::Invalid;
    }
    if (height >= l_min && height <= l_max) return Size::L;
    if (height >= m_min && height <= m_max) return Size::M;
    return height >= s_min && height < m_min ? Size::S : Size::Invalid;
}

// Routing: station 1 pushes L, station 2 pushes S, M passes.
// Missed must be reported by the caller, never actuated.
constexpr Action route(Size size, unsigned station) noexcept {
    if (station != 1 && station != 2) return Action::Invalid;
    switch (size) {
        case Size::S: return station == 2 ? Action::Push : Action::Pass;
        case Size::L: return station == 1 ? Action::Push : Action::Missed;
        case Size::M: return Action::Pass;
        default: return Action::Invalid;
    }
}

// SORT-R03 / C2: h = ((H0 - d) - c) / k (k=1, c=0 before linear calibration).
constexpr double calibrate_height(double distance_mm, double h0_mm,
                                  double k = 1.0, double c = 0.0) noexcept {
    if (!is_finite(distance_mm) || !is_finite(h0_mm) ||
        !is_finite(k) || !is_finite(c) || k <= 0.0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return ((h0_mm - distance_mm) - c) / k;
}

// Sort the retained session buffer in place: do not copy 2 KB onto the Pico stack.
// ponytail: insertion sort is O(n^2), bounded at 256; use selection if this limit grows.
constexpr double median_of(double* samples, std::size_t count) noexcept {
    if (samples == nullptr || count == 0 || count > MAX_MEDIAN_SAMPLES) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (!is_finite(samples[i])) {
            return std::numeric_limits<double>::quiet_NaN();
        }
    }
    for (std::size_t i = 1; i < count; ++i) {
        double key = samples[i];
        std::size_t j = i;
        while (j > 0 && samples[j - 1] > key) {
            samples[j] = samples[j - 1];
            --j;
        }
        samples[j] = key;
    }
    if ((count & 1U) != 0U) {
        return samples[count / 2];
    }
    return 0.5 * (samples[count / 2 - 1] + samples[count / 2]);
}

struct StationConfig {
    unsigned station = 1;
    double h0_mm = 100.0;
    double detect_delta_mm = 5.0;
    double clear_delta_mm = 5.0;
    double k = 1.0;
    double c = 0.0;
    double threshold_sm_mm = 40.0;
    double threshold_ml_mm = 60.0;
    bool use_ranges = false;
    double s_min_mm = 5.0;
    double m_min_mm = 7.0;
    double m_max_mm = 20.0;
    double l_min_mm = 22.0;
    double l_max_mm = 100.0;
    std::size_t min_valid_samples = 10;
    std::size_t clear_samples = 2;
    double max_valid_distance_mm = 4000.0;
    std::uint32_t max_session_ms = 2000;
};

constexpr bool config_valid(const StationConfig& cfg) noexcept {
    const bool bounds_ok = cfg.use_ranges
        ? (is_finite(cfg.m_min_mm) && is_finite(cfg.m_max_mm) &&
           cfg.m_min_mm >= 0.0 && cfg.m_min_mm < cfg.m_max_mm &&
           is_finite(cfg.l_min_mm) && is_finite(cfg.l_max_mm) &&
           cfg.l_min_mm >= 0.0 && cfg.l_min_mm < cfg.l_max_mm &&
           is_finite(cfg.s_min_mm) &&
           cfg.s_min_mm >= 0.0 && cfg.s_min_mm < cfg.m_min_mm)
        : (is_finite(cfg.threshold_sm_mm) && is_finite(cfg.threshold_ml_mm) &&
           cfg.threshold_sm_mm < cfg.threshold_ml_mm);
    return (cfg.station == 1 || cfg.station == 2) &&
           is_finite(cfg.h0_mm) && cfg.h0_mm > 0.0 &&
           is_finite(cfg.detect_delta_mm) && cfg.detect_delta_mm > 0.0 &&
           is_finite(cfg.clear_delta_mm) && cfg.clear_delta_mm >= 0.0 &&
           cfg.clear_delta_mm <= cfg.detect_delta_mm &&
           is_finite(cfg.k) && cfg.k > 0.0 &&
           is_finite(cfg.c) &&
           bounds_ok &&
           cfg.min_valid_samples >= 5 && cfg.min_valid_samples <= MAX_MEDIAN_SAMPLES &&
           cfg.clear_samples >= 1 && cfg.clear_samples <= MAX_MEDIAN_SAMPLES &&
           cfg.max_session_ms > 0 && cfg.max_session_ms < 0x80000000U &&
           is_finite(cfg.max_valid_distance_mm) && cfg.max_valid_distance_mm > cfg.h0_mm;
}

enum class MeasurementStatus { Valid, TooFewSamples, Timeout, BufferFull, InvalidHeight };

struct DetectionResult {
    bool ready = false;
    std::uint32_t t0_ms = 0;
    double median_distance_mm = 0.0;
    double height_mm = 0.0;
    Size size = Size::Invalid;
    Action action = Action::Invalid;
    bool started = false;
    std::size_t total_samples = 0;
    std::size_t kept_samples = 0;
    MeasurementStatus status = MeasurementStatus::Valid;
};

// One session per complete passage. Trim in acquisition order BEFORE sorting.
class ItemDetector {
public:
    enum class State { Idle, Collecting, WaitClear };

    constexpr explicit ItemDetector(const StationConfig& cfg) noexcept : cfg_(cfg) {}

    constexpr State state() const noexcept { return state_; }

    constexpr bool set_config(const StationConfig& cfg) noexcept {
        if (!config_valid(cfg)) return false;
        cfg_ = cfg;
        reset();
        return true;
    }

    constexpr void reset() noexcept {
        state_ = State::Idle;
        t0_ms_ = 0;
        sample_count_ = 0;
        clear_count_ = 0;
    }

    constexpr DetectionResult feed(double distance_mm, std::uint32_t now_ms) noexcept {
        if (!config_valid(cfg_)) return {};
        const bool valid = is_finite(distance_mm) && distance_mm > 0.0 &&
                           distance_mm <= cfg_.max_valid_distance_mm;
        const bool clear = valid && distance_mm >= cfg_.h0_mm - cfg_.clear_delta_mm;
        if (state_ == State::WaitClear) {
            clear_count_ = clear ? clear_count_ + 1 : 0;
            if (clear_count_ >= cfg_.clear_samples) reset();
            return {};
        }
        if (state_ == State::Idle) {
            if (!valid || distance_mm >= cfg_.h0_mm - cfg_.detect_delta_mm) return {};
            state_ = State::Collecting;
            t0_ms_ = now_ms;
            samples_[sample_count_++] = distance_mm;
            DetectionResult result{};
            result.started = true;
            result.t0_ms = now_ms;
            return result;
        }
        if (now_ms - t0_ms_ >= cfg_.max_session_ms)
            return finish_(MeasurementStatus::Timeout, false);

        clear_count_ = clear ? clear_count_ + 1 : 0;
        if (clear_count_ >= cfg_.clear_samples) {
            // Earlier clear-confirmation samples were buffered, but are not part of the object.
            sample_count_ -= clear_count_ - 1;
            return finish_(MeasurementStatus::Valid, true);
        }
        if (sample_count_ == MAX_MEDIAN_SAMPLES)
            return finish_(MeasurementStatus::BufferFull, false);
        // Invalid readings retain their temporal slot, but cannot contribute to the median.
        samples_[sample_count_++] = valid ? distance_mm : 0.0;
        return {};
    }

    // Chot som khi phoi cham LM393 trong luc duoi phoi chua kip roi khoi vung do ToF.
    constexpr DetectionResult finish_early() noexcept {
        if (state_ != State::Collecting || sample_count_ < cfg_.min_valid_samples) return {};
        return finish_(MeasurementStatus::Valid, false);
    }

private:
    constexpr DetectionResult finish_(MeasurementStatus status, bool cleared) noexcept {
        DetectionResult result{};
        result.ready = true;
        result.t0_ms = t0_ms_;
        result.total_samples = sample_count_;
        result.status = status;
        if (status == MeasurementStatus::Valid) {
            const std::size_t trim = sample_count_ / 5;
            std::size_t valid_count = 0;
            for (std::size_t i = 0; i < sample_count_; ++i)
                if (samples_[i] > 0.0) ++valid_count;
            std::size_t kept = 0;
            for (std::size_t i = trim; i < sample_count_ - trim; ++i)
                if (samples_[i] > 0.0) samples_[kept++] = samples_[i];
            result.kept_samples = kept;
            const std::size_t min_kept = cfg_.min_valid_samples - 2 * (cfg_.min_valid_samples / 5);
            if (valid_count < cfg_.min_valid_samples || kept < min_kept) {
                result.status = MeasurementStatus::TooFewSamples;
            } else {
                result.median_distance_mm = median_of(samples_, kept);
                result.height_mm = calibrate_height(result.median_distance_mm, cfg_.h0_mm, cfg_.k, cfg_.c);
                result.size = cfg_.use_ranges
                    ? classify_ranges(result.height_mm, cfg_.m_min_mm, cfg_.m_max_mm,
                                      cfg_.l_min_mm, cfg_.l_max_mm, cfg_.s_min_mm)
                    : classify(result.height_mm, cfg_.threshold_sm_mm, cfg_.threshold_ml_mm);
                result.action = route(result.size, cfg_.station);
                if (result.size == Size::Invalid) result.status = MeasurementStatus::InvalidHeight;
            }
        }
        reset();
        if (!cleared) state_ = State::WaitClear;
        return result;
    }

    StationConfig cfg_;
    State state_ = State::Idle;
    std::uint32_t t0_ms_ = 0;
    double samples_[MAX_MEDIAN_SAMPLES]{};
    std::size_t sample_count_ = 0;
    std::size_t clear_count_ = 0;
};

}  // namespace sorter_core
