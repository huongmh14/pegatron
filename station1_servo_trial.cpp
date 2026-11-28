#include <stdio.h>

#include "hardware/clocks.h"
#include "hardware/i2c.h"
#include "hardware/pwm.h"
#include "pico/stdlib.h"
#include "sorter_core.h"
#include "station1_servo_trial_logic.h"
#include "vl53l1x.h"

// Bench/calibration firmware for station 1 (ToF + LM393 + SG90). UF2: TestServoTram1.uf2.
// Boots DISARMED with the servo at HOME. Default mode pushes ANY size (so the pusher can be
// tuned first); 'x' switches to "L only" (same rule as the main firmware). Type '?' for keys.
static constexpr uint SDA_PIN = 0;
static constexpr uint SCL_PIN = 1;
static constexpr uint SERVO_PIN = 10;
static constexpr uint POSITION_PIN = 12;
static constexpr uint16_t SERVO_MIN_US = 1000;  // hard safety clamp for every pulse
static constexpr uint16_t SERVO_MAX_US = 2500;
static constexpr uint32_t TOF_TIMEOUT_MS = 800;
static constexpr uint32_t RETRY_MS = 2500;

// Runtime-tunable (starting values; the keys below change them live).
static bool position_active_low = true;    // 'l': flip if DO polarity is the other way round
static uint16_t home_us = 1500;            // 'q'/'w'  -/+10 us, servo moves to HOME at once
static uint16_t push_us = 2500;           // 'e'/'r'  -/+10 us, takes effect on next push ('m' to try)
static double h0_mm = 82.0;                // 'z' = take current empty-belt distance
static bool any_size = false;              // 'x'

static uint32_t millis() { return to_ms_since_boot(get_absolute_time()); }

static uint16_t clampUs(int v) {
    return static_cast<uint16_t>(v < SERVO_MIN_US ? SERVO_MIN_US : v > SERVO_MAX_US ? SERVO_MAX_US : v);
}

static void initServo() {
    gpio_set_function(SERVO_PIN, GPIO_FUNC_PWM);
    const uint slice = pwm_gpio_to_slice_num(SERVO_PIN);
    pwm_config config = pwm_get_default_config();
    pwm_config_set_clkdiv(&config, static_cast<float>(clock_get_hz(clk_sys)) / 1000000.0f);
    pwm_config_set_wrap(&config, 19999);  // 1 MHz counter, 20 ms period.
    pwm_init(slice, &config, false);
    pwm_set_gpio_level(SERVO_PIN, home_us);
    pwm_set_enabled(slice, true);
}

static bool startTof(VL53L1X &tof) {
    i2c_init(i2c0, 100000);
    gpio_set_function(SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(SDA_PIN);
    gpio_pull_up(SCL_PIN);
    if (!tof.init(true)) {
        const auto &diag = tof.initDiagnostic();
        printf("TOF_INIT_FAIL stage=%s model=0x%04X\n", diag.stage, diag.model_id);
        return false;
    }
    tof.setDistanceMode(VL53L1X::Medium);
    if (!tof.setMeasurementTimingBudget(15000)) {
        printf("TOF_TIMING_FAIL\n");
        return false;
    }
    tof.startContinuous(15);
    printf("TOF_READY\n");
    return true;
}

static void printHelp() {
    printf("KEYS: a=arm/disarm  m=push now(test, needs disarmed)  x=any-size/L-only  s=status\n"
           "      q/w=HOME -/+10us  e/r=PUSH -/+10us  t/T=push_ms -/+50  f/F=LM393 stable -/+5ms\n"
           "      g/G=wait_limit -/+200ms  y/Y=retract wait -/+100ms  u/U=empty-zone -/+50ms\n"
           "      l=LM393 polarity  z=H0 from current distance (belt EMPTY)\n");
}

int main() {
    stdio_init_all();
    gpio_init(POSITION_PIN);
    gpio_set_dir(POSITION_PIN, GPIO_IN);
    gpio_pull_up(POSITION_PIN);
    initServo();
    sleep_ms(1500);
    printf("TEST SERVO TRAM 1 (calibration, AUTO=ON, de BANG TAI TRONG 1 giay dau de lay H0). ");
    printHelp();

    sorter_core::StationConfig config;
    config.station = 1;
    config.h0_mm = h0_mm;
    config.detect_delta_mm = 6.0;
    config.clear_delta_mm = 5.0;
    config.use_ranges = true;
    config.l_min_mm = 28.0;  // same as main.cpp; ToF reads ~7 mm low: S~13-20, M~22, L~33
    config.l_max_mm = 100.0;
    config.m_min_mm = 18.0;
    config.m_max_mm = 28.0;
    config.min_valid_samples = 10;
    config.clear_samples = 2;
    static sorter_core::ItemDetector detector(config);
    VL53L1X tof(i2c0);
    bool tof_ok = startTof(tof);
    bool enabled = true;  // boots ARMED (servo is at HOME); 'a' disarms for manual 'm' tests
    unsigned h0_count = 0;  // first 20 valid readings of the EMPTY belt set H0 automatically
    double h0_sum = 0.0;
    Station1TrialGate gate;
    uint32_t measurement_ticket = 0;
    uint32_t last_sample = millis();
    uint32_t last_retry = millis();
    uint32_t last_report = millis();
    uint16_t distance = 0;
    uint8_t status = 255;
    bool valid = false;
    uint16_t current_pulse = home_us;

    while (true) {
        const uint32_t now = millis();
        const int ch = getchar_timeout_us(0);
        bool force_status = false;
        switch (ch) {
        case 'a': case 'A':
            enabled = !enabled;
            gate.reset(); detector.reset(); measurement_ticket = 0;
            printf("AUTO=%s mode=%s\n", enabled ? "ON" : "OFF", any_size ? "ANY_SIZE" : "L_ONLY");
            break;
        case 'm': case 'M':
            if (!enabled && gate.force_push(now)) printf("MANUAL_PUSH\n");
            else printf("MANUAL_PUSH refused (disarm with 'a' first / servo busy)\n");
            break;
        case 'x': case 'X':
            any_size = !any_size;
            gate.reset(); detector.reset(); measurement_ticket = 0;
            printf("MODE=%s\n", any_size ? "ANY_SIZE" : "L_ONLY");
            break;
        case 'q': home_us = clampUs(home_us - 10); force_status = true; break;
        case 'w': home_us = clampUs(home_us + 10); force_status = true; break;
        case 'e': push_us = clampUs(push_us - 10); force_status = true; break;
        case 'r': push_us = clampUs(push_us + 10); force_status = true; break;
        case 't': gate.push_ms = gate.push_ms > 100 ? gate.push_ms - 50 : 50; force_status = true; break;
        case 'T': gate.push_ms += 50; force_status = true; break;
        case 'f': gate.position_stable_ms = gate.position_stable_ms > 5 ? gate.position_stable_ms - 5 : 0; force_status = true; break;
        case 'F': gate.position_stable_ms += 5; force_status = true; break;
        case 'g': gate.wait_limit_ms = gate.wait_limit_ms > 400 ? gate.wait_limit_ms - 200 : 200; force_status = true; break;
        case 'G': gate.wait_limit_ms += 200; force_status = true; break;
        case 'y': gate.recover_ms = gate.recover_ms > 100 ? gate.recover_ms - 50 : 50; force_status = true; break;
        case 'Y': gate.recover_ms += 50; force_status = true; break;
        case 'u': gate.clear_ms = gate.clear_ms > 20 ? gate.clear_ms - 20 : 0; force_status = true; break;
        case 'U': gate.clear_ms += 20; force_status = true; break;
        case 'l': case 'L':
            position_active_low = !position_active_low;
            gate.reset(); detector.reset(); measurement_ticket = 0;
            printf("LM393 active=%s\n", position_active_low ? "LOW(DO=0)" : "HIGH(DO=1)");
            break;
        case 'z': case 'Z':
            if (valid) {  // also un-sticks a detector stuck on a stale H0 (set_config resets it)
                h0_mm = distance;
                config.h0_mm = h0_mm;
                gate.reset(); measurement_ticket = 0;
                detector.set_config(config);
                printf("H0 set to %.0f mm (belt must be EMPTY)\n", h0_mm);
            } else {
                printf("H0 not set: need a valid reading with nothing under the ToF\n");
            }
            break;
        case '?': printHelp(); force_status = true; break;
        case 's': case 'S': force_status = true; break;
        default: break;
        }

        const bool raw = gpio_get(POSITION_PIN);
        const bool position_active = position_active_low ? !raw : raw;

        static bool last_raw = raw;  // log every LM393 edge so the sensor can be checked by hand
        if (raw != last_raw) {
            last_raw = raw;
            printf("LM393 DO=%u -> %s (t=%lu ms)\n", raw, position_active ? "CO VAT" : "KHONG VAT",
                   static_cast<unsigned long>(now));
        }

        if (tof_ok && tof.dataReady()) {
            distance = tof.read(false);
            status = tof.ranging_data.range_status;
            valid = status == 0 && distance > 0 && distance < 4000;
            last_sample = now;
            sorter_core::DetectionResult result;
            if (h0_count < 20) {
                if (valid && ++h0_count && (h0_sum += distance, h0_count == 20)) {
                    h0_mm = h0_sum / 20.0;
                    config.h0_mm = h0_mm;
                    detector.set_config(config);
                    printf("H0 auto = %.1f mm (belt was empty). AUTO=ON, san sang day\n", h0_mm);
                }
            } else {
                result = detector.feed(valid ? distance : -1.0, now);
            }
            if (result.started)
                measurement_ticket = enabled && !any_size ? gate.reserve(result.t0_ms) : 0;
            if (result.ready) {
                const char *size = result.size == sorter_core::Size::L ? "L"
                                 : result.size == sorter_core::Size::M ? "M"
                                 : result.size == sorter_core::Size::S ? "S" : "INVALID";
                const bool push = result.action == sorter_core::Action::Push;
                const bool queued = enabled && !any_size && gate.complete(measurement_ticket, push);
                measurement_ticket = 0;
                printf("TOF_OBJECT 20/60/20 N=%u giu=%u d_med=%.1f h=%.1f size=%s status=%u -> %s | queue=%u%s\n",
                       static_cast<unsigned>(result.total_samples), static_cast<unsigned>(result.kept_samples),
                       result.median_distance_mm, result.height_mm, size, static_cast<unsigned>(result.status),
                       push ? "PUSH" : "PASS", static_cast<unsigned>(gate.q_count),
                       any_size ? " (ANY: chi log)" : !enabled ? " (AUTO=OFF)" : queued ? "" : " (VE DA HUY)");
            }
        }
        if (tof_ok && now - last_sample > TOF_TIMEOUT_MS) {
            tof_ok = false;
            valid = false;
            detector.reset();
            last_retry = now;
            printf("TOF_LOST\n");
        }
        if (!tof_ok && now - last_retry >= RETRY_MS) {
            last_retry = now;
            tof_ok = startTof(tof);
            last_sample = now;
            detector.reset();
        }

        // Manual push ('m') runs with enabled=false in the gate loop, so keep it alive while Extended.
        const bool gate_enabled = enabled || gate.stage == TrialStage::Extended;
        // ANY_SIZE = Arduino reference behaviour: every LM393 item is pushed, ToF is only logged.
        if (enabled && any_size && position_active && gate.q_count == 0 && !gate.need_release)
            gate.enqueue(true, now);
        const TrialEvent event = gate.tick(now, gate_enabled, tof_ok || !enabled || any_size, position_active);
        switch (event) {
        case TrialEvent::Passed: printf("LM393: phoi M/S -> cho qua | hang doi=%u\n", static_cast<unsigned>(gate.q_count)); break;
        case TrialEvent::Unmatched: printf("UNMATCHED: mat dong bo -> khoa day; don bang tai, tat/bat AUTO\n"); break;
        case TrialEvent::Push:
            printf("CO VAT -> SERVO_PUSH %u us (giu %lu ms)\n", push_us, static_cast<unsigned long>(gate.push_ms));
            break;
        case TrialEvent::Returned: printf("THU VE -> SERVO_HOME %u us, cho vat roi cam bien\n", home_us); break;
        case TrialEvent::Ready: printf("SAN SANG cho vat tiep theo\n"); break;
        case TrialEvent::TimedOut: printf("POSITION_TIMEOUT: mat dong bo -> khoa day; don bang tai, tat/bat AUTO\n"); break;
        case TrialEvent::NotReady: printf("NOT_READY: bo ve dang do, khong day muon\n"); break;
        case TrialEvent::Busy: printf("BUSY: bo ve phoi nay, khong day muon\n"); break;
        case TrialEvent::SyncLost: printf("SYNC_LOST: khoa day; don bang tai, tat/bat AUTO\n"); break;
        case TrialEvent::SensorLost: printf("SENSOR_LOST: servo home, khoa day; don bang tai, tat/bat AUTO\n"); break;
        default: break;
        }
        const uint16_t wanted_pulse = gate.stage == TrialStage::Extended ? push_us : home_us;
        if (wanted_pulse != current_pulse) {
            pwm_set_gpio_level(SERVO_PIN, wanted_pulse);
            current_pulse = wanted_pulse;
        }
        if (force_status || now - last_report >= 1000) {
            printf("STATUS auto=%u mode=%s tof=%s d=%u st=%u H0=%.0f LM393_DO=%u pos=%u(active_%s) stage=%u "
                   "servo=%u us | HOME=%u PUSH=%u push_ms=%lu stable=%lu wait=%lu thu=%lu roi=%lu q=%u\n",
                   enabled, any_size ? "ANY" : "L", tof_ok ? "OK" : "FAIL", distance, status, h0_mm, raw,
                   position_active, position_active_low ? "LOW" : "HIGH",
                   static_cast<unsigned>(gate.stage), current_pulse, home_us, push_us,
                   static_cast<unsigned long>(gate.push_ms),
                   static_cast<unsigned long>(gate.position_stable_ms),
                   static_cast<unsigned long>(gate.wait_limit_ms),
                   static_cast<unsigned long>(gate.recover_ms), static_cast<unsigned long>(gate.clear_ms),
                   static_cast<unsigned>(gate.q_count));
            last_report = now;
        }
        sleep_ms(1);
    }
}
