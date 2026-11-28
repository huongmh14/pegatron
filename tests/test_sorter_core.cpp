// Compile-time verification (no EXE needed, works under Windows Application Control):
//   g++ -std=c++17 -Wall -Wextra -Werror -fsyntax-only tests/test_sorter_core.cpp
// Full host binary (WSL/Linux/CI):
//   g++ -std=c++17 -Wall -Wextra -Werror tests/test_sorter_core.cpp -o test_sorter_core && ./test_sorter_core
#include "../sorter_core.h"

#include <cassert>
#include <cmath>
#include <initializer_list>
#include <limits>

namespace {

using namespace sorter_core;

constexpr bool approx_eq(double a, double b, double eps = 1e-9) noexcept {
    const double diff = a - b;
    return (diff < 0.0 ? -diff : diff) <= eps;
}

constexpr bool test_classify_and_route() noexcept {
    // Boundary and invalid threshold checks (SORT-R04)
    if (classify(40.0, 40.0, 60.0) != Size::S) return false;
    if (classify(40.0 + 1e-12, 40.0, 60.0) != Size::M) return false;
    if (classify(60.0, 40.0, 60.0) != Size::M) return false;
    if (classify(60.0 + 1e-12, 40.0, 60.0) != Size::L) return false;
    if (classify(50.0, 60.0, 40.0) != Size::Invalid) return false;
    if (classify(50.0, 40.0, 40.0) != Size::Invalid) return false;

    const double invalids[3] = {
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
    };
    for (double bad : invalids) {
        if (classify(bad, 40.0, 60.0) != Size::Invalid) return false;
        if (classify(50.0, bad, 60.0) != Size::Invalid) return false;
        if (classify(50.0, 40.0, bad) != Size::Invalid) return false;
    }

    // Routing matrix: station 1 pushes L, station 2 pushes S, M passes.
    if (route(Size::S, 1) != Action::Pass) return false;
    if (route(Size::S, 2) != Action::Push) return false;
    if (route(Size::M, 1) != Action::Pass) return false;
    if (route(Size::M, 2) != Action::Pass) return false;
    if (route(Size::L, 1) != Action::Push) return false;
    if (route(Size::L, 2) != Action::Missed) return false;
    if (route(Size::Invalid, 1) != Action::Invalid) return false;
    if (route(Size::Invalid, 2) != Action::Invalid) return false;
    if (route(Size::M, 0) != Action::Invalid) return false;
    if (route(Size::L, 3) != Action::Invalid) return false;

    // Guard bands and out-of-range heights are invalid, never an implicit S.
    if (classify_ranges(12.0, 7.0, 18.0, 24.0, 60.0) != Size::M) return false;
    if (classify_ranges(21.0, 7.0, 18.0, 24.0, 60.0) != Size::Invalid) return false;
    if (classify_ranges(30.0, 7.0, 18.0, 24.0, 60.0) != Size::L) return false;
    if (classify_ranges(5.0, 7.0, 18.0, 24.0, 60.0) != Size::S) return false;
    if (classify_ranges(65.0, 7.0, 18.0, 24.0, 60.0) != Size::Invalid) return false;
    // Overlap gives L priority
    if (classify_ranges(20.0, 7.0, 22.0, 18.0, 60.0) != Size::L) return false;
    // Invalid bounds rejected
    if (classify_ranges(15.0, 18.0, 18.0, 24.0, 60.0) != Size::Invalid) return false;
    if (classify_ranges(15.0, -1.0, 18.0, 24.0, 60.0) != Size::Invalid) return false;
    if (classify_ranges(15.0, 7.0, 18.0, 60.0, 24.0) != Size::Invalid) return false;
    return true;
}

constexpr bool test_height_and_median() noexcept {
    // SORT-R03 / C2 linear height calibration
    if (!approx_eq(calibrate_height(50.0, 100.0), 50.0)) return false;
    if (!approx_eq(calibrate_height(50.0, 100.0, 1.0, 0.0), 50.0)) return false;
    if (!approx_eq(calibrate_height(50.0, 100.0, 2.0, 10.0), 20.0)) return false;
    if (is_finite(calibrate_height(50.0, 100.0, 0.0, 0.0))) return false;
    if (is_finite(calibrate_height(50.0, 100.0, -1.0, 0.0))) return false;
    if (is_finite(calibrate_height(std::numeric_limits<double>::quiet_NaN(), 100.0))) return false;

    // Median odd/even + 1-point outlier rejection
    double s3[3] = {50.0, 50.2, 49.9};
    if (!approx_eq(median_of(s3, 3), 50.0)) return false;

    double s4[4] = {10.0, 40.0, 20.0, 30.0};
    if (!approx_eq(median_of(s4, 4), 25.0)) return false;

    double s5_spike[5] = {50.0, 99.0, 50.2, 49.9, 50.1};
    if (!approx_eq(median_of(s5_spike, 5), 50.1)) return false;

    double s_nan[3] = {50.0, std::numeric_limits<double>::quiet_NaN(), 49.9};
    if (is_finite(median_of(s_nan, 3))) return false;
    if (is_finite(median_of(s3, 0))) return false;
    if (is_finite(median_of(nullptr, 3))) return false;
    return true;
}

constexpr StationConfig production_config(unsigned station = 1) {
    StationConfig c;
    c.station = station;
    c.use_ranges = true;
    c.m_min_mm = 18; c.m_max_mm = 28;
    c.l_min_mm = 28; c.l_max_mm = 100;
    return c;
}

constexpr DetectionResult measure(ItemDetector& d, double distance, std::uint32_t start = 0) {
    for (std::uint32_t i = 0; i < 10; ++i) {
        const auto r = d.feed(distance, start + i * 10);
        if (r.ready || r.started != (i == 0)) return {};
    }
    if (d.feed(100, start + 100).ready) return {};
    return d.feed(100, start + 110);
}

constexpr bool complete_passage_trims_before_sorting() {
    ItemDetector d(production_config());
    const double seq[] = {20, 21, 40, 50, 60, 70, 80, 90, 22, 23};
    for (std::uint32_t i = 0; i < 10; ++i) {
        const auto r = d.feed(seq[i], i * 10);
        if (r.ready || r.started != (i == 0)) return false;
    }
    if (d.feed(100, 100).ready) return false;
    const auto r = d.feed(100, 110);
    return r.ready && r.total_samples == 10 && r.kept_samples == 6 &&
           r.median_distance_mm == 65 && r.height_mm == 35 &&
           r.action == Action::Push && d.state() == ItemDetector::State::Idle;
}

constexpr bool two_stations_route_each_size() {
    for (unsigned station = 1; station <= 2; ++station) {
        ItemDetector d(production_config(station));
        const auto l = measure(d, 67);
        const auto m = measure(d, 78, 200);
        const auto s = measure(d, 85, 400);
        if (!l.ready || l.size != Size::L || l.action != route(Size::L, station)) return false;
        if (!m.ready || m.size != Size::M || m.action != Action::Pass) return false;
        if (!s.ready || s.size != Size::S || s.action != route(Size::S, station)) return false;
    }
    return true;
}

constexpr bool invalid_samples_keep_temporal_slots() {
    ItemDetector d(production_config());
    if (d.feed(-1, 0).started) return false;
    const double seq[] = {20,21,22, 60,-1,61,62,63,64,65,-1,66, 23,24,25};
    for (std::uint32_t i = 0; i < 15; ++i) d.feed(seq[i], i * 10);
    d.feed(100, 150);
    const auto r = d.feed(100, 160);
    return r.ready && r.total_samples == 15 && r.kept_samples == 7 &&
           r.median_distance_mm == 63 && r.status == MeasurementStatus::Valid;
}

constexpr bool short_and_corrupt_sessions_are_invalid() {
    ItemDetector d(production_config());
    d.feed(67, 0); d.feed(67, 10); d.feed(67, 20); d.feed(100, 30);
    const auto short_item = d.feed(100, 40);
    if (!short_item.ready || short_item.action != Action::Invalid ||
        short_item.status != MeasurementStatus::TooFewSamples) return false;
    for (std::uint32_t i = 0; i < 10; ++i) d.feed(i == 5 ? -1 : 67, 50 + i * 10);
    d.feed(100, 150);
    const auto bad = d.feed(100, 160);
    return bad.ready && bad.action == Action::Invalid && bad.status == MeasurementStatus::TooFewSamples;
}

constexpr bool timeout_waits_for_clear_and_handles_clock_wrap() {
    auto c = production_config(); c.max_session_ms = 200;
    ItemDetector d(c);
    const std::uint32_t start = 0xffffff80U;
    d.feed(67, start);
    if (d.feed(-1, start + 199).ready) return false;
    const auto r = d.feed(-1, start + 200);
    if (!r.ready || r.status != MeasurementStatus::Timeout || r.action != Action::Invalid) return false;
    if (d.feed(67, start + 210).started || d.feed(67, start + 220).ready) return false;
    d.feed(100, start + 230);
    d.feed(-1, start + 240); // invalid reading cannot confirm clearance
    d.feed(100, start + 250);
    if (d.state() != ItemDetector::State::WaitClear) return false;
    d.feed(100, start + 260);
    return d.feed(67, start + 270).started;
}

constexpr bool overflow_returns_one_invalid_result() {
    auto c = production_config(); c.max_session_ms = 10000;
    ItemDetector d(c);
    for (std::uint32_t i = 0; i < MAX_MEDIAN_SAMPLES; ++i)
        if (d.feed(67, i).ready) return false;
    const auto r = d.feed(67, MAX_MEDIAN_SAMPLES);
    return r.ready && r.status == MeasurementStatus::BufferFull &&
           r.action == Action::Invalid && !d.feed(67, MAX_MEDIAN_SAMPLES + 1).ready;
}

constexpr bool full_buffer_median_sorts_in_place() {
    double samples[MAX_MEDIAN_SAMPLES]{};
    for (std::size_t i = 0; i < MAX_MEDIAN_SAMPLES; ++i)
        samples[i] = static_cast<double>(MAX_MEDIAN_SAMPLES - i);
    return median_of(samples, MAX_MEDIAN_SAMPLES) == 128.5 &&
           samples[0] == 1.0 && samples[MAX_MEDIAN_SAMPLES - 1] == 256.0;
}

constexpr bool configuration_and_boundaries() {
    auto c = production_config();
    if (!config_valid(c)) return false;
    if (classify_ranges(4.9, 18,28,28,100) != Size::Invalid ||
        classify_ranges(5, 18,28,28,100) != Size::S ||
        classify_ranges(17.9, 18,28,28,100) != Size::S ||
        classify_ranges(18, 18,28,28,100) != Size::M ||
        classify_ranges(28, 18,28,28,100) != Size::L ||
        classify_ranges(101, 18,28,28,100) != Size::Invalid) return false;
    ItemDetector d(c);
    d.feed(67, 0);
    c.min_valid_samples = 0;
    if (d.set_config(c)) return false;
    c.min_valid_samples = 10;
    if (!d.set_config(c) || d.state() != ItemDetector::State::Idle) return false;
    c.s_min_mm = c.m_min_mm;
    return !config_valid(c);
}

static_assert(test_classify_and_route());
static_assert(test_height_and_median());
static_assert(complete_passage_trims_before_sorting());
static_assert(two_stations_route_each_size());
static_assert(invalid_samples_keep_temporal_slots());
static_assert(short_and_corrupt_sessions_are_invalid());
static_assert(timeout_waits_for_clear_and_handles_clock_wrap());
static_assert(overflow_returns_one_invalid_result());
static_assert(full_buffer_median_sorts_in_place());
static_assert(configuration_and_boundaries());
}  // namespace

int main() {
    assert(classify(std::nextafter(40.0, 60.0), 40.0, 60.0) == Size::M);
    assert(classify(std::nextafter(60.0, 100.0), 40.0, 60.0) == Size::L);
    return 0;
}
