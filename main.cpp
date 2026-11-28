/**
 * =====================================================================
 *  BANG TAI PHAN LOAI TU DONG - Raspberry Pi Pico 2 W (VL53L1X)
 * =====================================================================
 *  Phan cung:
 *    - ToF 1 (VL53L1X) : I2C0  SDA=GP0, SCL=GP1, LPN=3V3 -> Tram 1
 *    - ToF 2 (VL53L1X) : I2C1  SDA=GP6, SCL=GP7, LPN=3V3 -> Tram 2
 *    - Servo T1/T2     : GP10/GP11 PWM 50 Hz (nguon 5V rieng, GND chung)
 *    - LM393 T1/T2 DO  : GP12/GP13 - xac nhan phoi tai vi tri day
 *
 *  Logic:
 *    - 2 tram chay DOC LAP va DONG THOI, khong chan CPU (Non-blocking).
 *    - Tram 1 day L; tram 2 day S. ToF chot kich thuoc, LM393 xac nhan vi tri,
 *      servo day trong on_time_ms roi ve HOME. Mat ToF/tam dung -> HOME.
 *    - Size M di thang; phep do loi/ngoai khoang khong duoc day.
 *    - Nguong va thoi gian gat co the chinh truc tiep tu Web Dashboard.
 *    - Co bo dem san pham phan loai, loc nhieu (debounce), chong day lap.
 * =====================================================================
 */
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "pico/cyw43_arch.h"
#include "pico/flash.h"
#include "pico/multicore.h"
#include "pico/util/queue.h"
#include "pegatron_monitor.h"
#include "pegatron_local_config.h"
#include "vl53l1x.h"
#include "station1_servo_trial_logic.h"

#ifndef USE_SORTER_ENGINE
#define USE_SORTER_ENGINE 1
#endif

#if USE_SORTER_ENGINE
#include "sorter_fsm.h"
#include "calib_math.h"
#endif

#ifndef USE_SORTER_ENGINE
#define USE_SORTER_ENGINE 1
#endif

#if USE_SORTER_ENGINE
#include "sorter_fsm.h"
#include "calib_math.h"
#endif

// ======================= CAU HINH CHAN =======================
#define TOF1_I2C        i2c0
#define TOF1_SDA_PIN    0
#define TOF1_SCL_PIN    1

#define TOF2_I2C        i2c1
#define TOF2_SDA_PIN    6
#define TOF2_SCL_PIN    7

#define SOL1_PIN        10     // Servo tram 1
#define SOL2_PIN        11     // Servo tram 2; chung PWM slice voi GP10
#define LM393_1_PIN     12
#define LM393_2_PIN     13
#define START_PIN       19
#define STOP_PIN        20
#define ESTOP_PIN       21
// 1: GP21 noi tiep diem NO cua E-STOP (binh thuong=1, nhan E-STOP=0V);
// 0: GP21 noi tiep diem NC cua E-STOP (binh thuong=0V, nhan/ho=1).
// Luu y: E-STOP van co tiep diem NC doc lap cat truc tiep cuon MY4.
#ifndef ESTOP_ACTIVE_LOW
#define ESTOP_ACTIVE_LOW 1
#endif
#define RELAY_PIN       26
// -1: chua xac minh; 0: relay hut khi IN=0; 1: relay hut khi IN=1.
// Relay dang dung tiep diem NC: hut = dung, nha = chay.
#ifndef RELAY_ACTIVE_LEVEL
#define RELAY_ACTIVE_LEVEL 0
#endif
static_assert(RELAY_ACTIVE_LEVEL >= -1 && RELAY_ACTIVE_LEVEL <= 1, "RELAY_ACTIVE_LEVEL phai la -1, 0 hoac 1");

#ifndef ENABLE_WIFI
#define ENABLE_WIFI 0          // 0: tam bo Wi-Fi/MQTT, chay doc lap
#endif
#ifndef AUTO_START_ON_BOOT
#define AUTO_START_ON_BOOT 0   // 0: cho nhan nut START (GP19) moi chay va mo relay GP26
#endif

#define I2C_BAUDRATE    (50 * 1000)    // 50 kHz: chong nhieu EMI dong co bang tai tot hon 100 kHz

// ===== Servo + LM393 hai tram =====
#define LM393_ACTIVE_LOW    1      // 1: co vat -> DO=0. Do bang dong ho roi doi 0/1 neu nguoc.
// Co cau banh rang - thanh rang: Z=20, D=6cm, hanh trinh day 6cm (~115 deg), SG90 5-6V
// Co cau banh rang - thanh rang: Z=20, D=6cm, hanh trinh day ~7cm (~134 deg), SG90 5-6V
#define SERVO1_HOME_US      600    // Tram 1 XUNG_BAT_DAU: vi tri goc thu gon (~9 deg)
#define SERVO1_PUSH_US      1880   // Tram 1 XUNG_DAY_RA: day ra du 6cm (+115 deg)
#define SERVO1_PUSH_US      2100   // Tram 1 XUNG_DAY_RA: 1880 + 220us (~+1cm -> tong ~7cm)
#define SERVO2_HOME_US      600    // Tram 2 XUNG_BAT_DAU (tinh chinh rieng neu co khi lech)
#define SERVO2_PUSH_US      1880   // Tram 2 XUNG_DAY_RA
#define SERVO2_PUSH_US      2100   // Tram 2 XUNG_DAY_RA: ~7cm
#define SERVO_INIT_MS       250    // TRE_KHOI_DONG: on dinh vi tri goc khi boot
#define SERVO_PUSH_MS       240    // TRE_DAY_RA: pha day 6cm
#define SERVO_RECOVER_MS    260    // TRE_THU_VE: pha rut ve goc roi ngat xung giu (detach)
#define SERVO_PUSH_MS       300    // TRE_DAY_RA: du thoi gian day het 7cm va dut khoat
#define SERVO_RECOVER_MS    300    // TRE_THU_VE: thu het 7cm ve goc roi ngat xung giu (detach)
#define SERVO_MIN_US        500    // Dai xung rong SG90 (500us - 2500us)
#define SERVO_MAX_US        2500   // SG90 toi da ~2500us; kiem tra co khi truoc khi tang PUSH
#define LM393_CLEAR_MS      30     // Phoi da xu ly phai roi LM393 >= ms nay truoc khi nhan phoi tiep
#define LM393_STABLE_MS     10     // LM393 phai on dinh ACTIVE >= ms nay (chong nhieu)
#define LM393_WAIT_LIMIT_MS 4000   // Qua 4s tu luc ToF chot -> tu bo ve cu (khong khoa tram)
static_assert(SERVO1_HOME_US >= SERVO_MIN_US && SERVO1_HOME_US <= SERVO_MAX_US, "HOME1 ngoai khoang an toan");
static_assert(SERVO1_PUSH_US >= SERVO_MIN_US && SERVO1_PUSH_US <= SERVO_MAX_US, "PUSH1 ngoai khoang an toan");
static_assert(SERVO2_HOME_US >= SERVO_MIN_US && SERVO2_HOME_US <= SERVO_MAX_US, "HOME2 ngoai khoang an toan");
static_assert(SERVO2_PUSH_US >= SERVO_MIN_US && SERVO2_PUSH_US <= SERVO_MAX_US, "PUSH2 ngoai khoang an toan");

// ======================= THAM SO HE THONG =======================
#define BELT_CLEAR_MIN_MM   80     // Mat bang tai 85-90mm; >= 80mm moi cho nhan phoi tiep
#define DEBOUNCE_COUNT      2      // So mau lien tiep trong khoang size de xac nhan phoi
#define CLEAR_COUNT         1      // So mau lien tiep thay mat bang tai de chong kich lap
#define SENSOR_RETRY_MS     300    // Thu khoi tao lai cam bien nhanh neu nhieu I2C khi dong co chay
#define SENSOR_TIMEOUT_MS   800    // Timeout tin hieu cam bien
#define PRINT_PERIOD_MS     500    // Chu ky in trang thai (500ms de khong troi log su kien)
#define QUEUE_SIZE          8      // So luong vat toi da trong hang doi tre di chuyen
#define MAX_VALID_RANGE_MM  4000   // Khoang cach do toi da hop le cua VL53L1X (4m)

#if !USE_SORTER_ENGINE
#error "Tram 1 dung SG90 + LM393, can USE_SORTER_ENGINE=1 (tranh 2 co che cung dieu khien GP10)"
#endif
#if USE_SORTER_ENGINE
#define DEFAULT_H0_MM       89.0   // Mat bang tai thuc te ~88-92mm (dung neu luc boot bi vat che ToF)
#define DETECT_DELTA_MM     8.0    // Nguong phat hien vat thap hon mat bang (mm), cao hon nhieu bang trong
#define CLEAR_DELTA_MM      5.0    // Tre hysteresis mo khoa WaitClear (mm)
#define THRESHOLD_SM_MM     7.0    // Nguong T_SM (<=7mm: S, >7mm: M)
#define THRESHOLD_ML_MM     20.0   // Nguong T_ML (>20mm: L)
#define L_HEIGHT_MIN_MM     28.0   // L that 40mm, ToF do ~33 (thap ~7mm). h>=28 la L (Tram 1)
#define L_HEIGHT_MAX_MM     100.0  // Chieu cao toi da size L (Tram 1)
#define M_HEIGHT_MIN_MM     18.0   // M that 30mm, ToF do ~22. 18<=h<28 la M di thang; S do ~13-17 (Tram 2 day S)
#define M_HEIGHT_MAX_MM     28.0   // L duoc xet truoc nen h>=28 la L
#define MIN_VALID_SAMPLES   5      // Toi thieu 5 mau hop le (~75ms @ 68Hz, dung yeu cau config_valid)
#define S_HEIGHT_MIN_MM     5.0    // S: [5, M_HEIGHT_MIN_MM), can hieu chuan tren may
#define SOLENOID_MAX_ON_MS  600    // Chan tren thoi gian hut solenoid (ms)
#define SOLENOID_MIN_OFF_MS 120    // Thoi gian hoi toi thieu giua hai nhip gat (ms)
#define SOLENOID_RATED_DUTY 0.25   // Gioi han nhiet trong cua so 60s
#endif

// ======================= KIEU DU LIEU =======================
enum StationMode {
    MODE_PULSE,   // Kich Solenoid trong on_time_ms roi nha
    MODE_HOLD     // Kich Solenoid giu cho den khi vat di qua
};

enum StationState {
    ST_IDLE,        // Cho vat
    ST_ACTIVE,      // Solenoid dang hut
    ST_WAIT_CLEAR   // Cho vat roi khoi vung do truoc khi nhan vat tiep theo
};

struct Station {
    // --- Cau hinh tram ---
    const char   *name;
    i2c_inst_t   *i2c;
    uint          sda_pin, scl_pin, sol_pin;
    StationMode   mode;
    uint16_t      min_mm, max_mm;  // Khoang cach nhan size, bao gom hai dau mut (mm)
    uint32_t      travel_delay_ms; // Thoi gian vat chay tu cam bien -> tay gat (0 = gat ngay)
    uint32_t      on_time_ms;      // Thoi gian Solenoid hut (ms)

    // --- Trang thai runtime ---
    VL53L1X      *tof;
    bool          sensor_ok;
    bool          sensor_initialized;
    bool          has_valid_target;
    uint16_t      distance_mm;
    uint32_t      last_data_ms;
    uint32_t      last_retry_ms;
    uint8_t       hit_count;
    uint8_t       clear_count;
    StationState  state;
    uint32_t      state_start_ms;
    uint32_t      total_count;

    // --- Hang doi tre di chuyen (FIFO queue) ---
    uint32_t      queue_trigger_time[QUEUE_SIZE];
    uint8_t       q_head;
    uint8_t       q_tail;
    uint8_t       q_count;
    uint32_t      push_count;
#if USE_SORTER_ENGINE
    sorter_core::StationConfig    detector_cfg{};
    sorter_core::TriggerGeometry  geometry{};
    sorter_core::ItemDetector     detector{sorter_core::StationConfig{}};
    sorter_core::SolenoidActuator actuator{};
    uint32_t                      measurement_ticket = 0;
    uint32_t                      missed_count = 0;
    bool                          manual_test_active = false;
    bool                          servo = false;
    Station1TrialGate             servo_gate{};
    uint                          position_pin = 0;
    uint16_t                      servo_home_us = SERVO1_HOME_US;
    uint16_t                      servo_push_us = SERVO1_PUSH_US;
    uint16_t                      servo_us = 0;
    bool                          position_last_raw = false;
    uint16_t                      raw_mm = 0;          // chan doan: mau ToF tho cuoi cung
    uint8_t                       raw_status = 255;    // chan doan: range_status cuoi cung
    uint32_t                      bad_samples = 0;     // chan doan: so mau khong hop le
#endif
};

// Web dashboard can pause sorting (belt keeps being measured, solenoids stay off).
static bool sorting_enabled = false;
#if USE_SORTER_ENGINE
static sorter_core::SystemFsm system_fsm;
static uint32_t tray_m_count = 0;
#endif

static VL53L1X tof1(TOF1_I2C);
static VL53L1X tof2(TOF2_I2C);

// Tram 1 day L; tram 2 day S; M di thang.
static Station stations[2] = {
    { "TRAM1", TOF1_I2C, TOF1_SDA_PIN, TOF1_SCL_PIN, SOL1_PIN, MODE_PULSE, 0, 62, 0, SERVO_PUSH_MS, &tof1 },
    { "TRAM2", TOF2_I2C, TOF2_SDA_PIN, TOF2_SCL_PIN, SOL2_PIN, MODE_PULSE, 62, 75, 0, SERVO_PUSH_MS, &tof2 },
};

// ======================= TIEN ICH =======================
static inline uint32_t millis() {
    return to_ms_since_boot(get_absolute_time());
}

#if !USE_SORTER_ENGINE
static void queuePush(Station &s, uint32_t trigger_at) {
    if (s.q_count < QUEUE_SIZE) {
        s.queue_trigger_time[s.q_tail] = trigger_at;
        s.q_tail = (s.q_tail + 1) % QUEUE_SIZE;
        s.q_count++;
    }
}

static bool queuePop(Station &s, uint32_t *trigger_at) {
    if (s.q_count == 0) return false;
    *trigger_at = s.queue_trigger_time[s.q_head];
    s.q_head = (s.q_head + 1) % QUEUE_SIZE;
    s.q_count--;
    return true;
}

static bool queuePeek(const Station &s, uint32_t *trigger_at) {
    if (s.q_count == 0) return false;
    *trigger_at = s.queue_trigger_time[s.q_head];
    return true;
}
#endif

// =====================================================================
//                       HAM DIEU KHIEN SOLENOID
// =====================================================================
static void solenoidInit(uint pin) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_OUT);
    gpio_put(pin, 0);
}

static void solenoidSet(Station &s, bool on) {
    gpio_put(s.sol_pin, on ? 1 : 0);
    if (on) ++s.push_count;
    printf("[%s] Solenoid (GP%u) -> %s | d=%u mm | Tong: %lu\n",
           s.name, s.sol_pin, on ? "HUT (ON)" : "NHA (OFF)", s.distance_mm, (unsigned long)s.total_count);
}

// ---------------------- Servo + LM393 (moi tram mot gate rieng) ----------------------
static inline bool servoUsesDetach(const Station &) {
    return true;  // Ca 2 tram: ngat xung giu sau khi thu ve goc (chong rung & nong SG90)
}

static void servoWrite(Station &s, uint16_t us) {
    if (us < SERVO_MIN_US) us = SERVO_MIN_US;
    if (us > SERVO_MAX_US) us = SERVO_MAX_US;
    if (us == s.servo_us) return;
    pwm_set_gpio_level(s.sol_pin, us);   // 1 MHz counter: level = pulse width in us
    s.servo_us = us;
}

static void servoDetach(Station &s) {
    if (s.servo_us == 0) return;
    pwm_set_gpio_level(s.sol_pin, 0);    // 0% duty = ngat xung giu (giong myServo.detach())
    s.servo_us = 0;
}

static void servoApplyStage(Station &s) {
    const auto st = s.servo_gate.stage;
    if (st == TrialStage::Extended) {
        servoWrite(s, s.servo_push_us);
    } else if (st == TrialStage::Recovery) {
        servoWrite(s, s.servo_home_us);
    } else if (servoUsesDetach(s)) {
        servoDetach(s);
    } else {
        servoWrite(s, s.servo_home_us);
    }
}

static void servoInit(Station &s) {
    gpio_set_function(s.sol_pin, GPIO_FUNC_PWM);
    const uint slice = pwm_gpio_to_slice_num(s.sol_pin);
    static bool pwm_ready = false;  // GP10/GP11 chung slice: khong reset servo da khoi tao.
    if (!pwm_ready) {
        pwm_config config = pwm_get_default_config();
        pwm_config_set_clkdiv(&config, static_cast<float>(clock_get_hz(clk_sys)) / 1000000.0f);
        pwm_config_set_wrap(&config, 19999);  // 20 ms period (50 Hz)
        pwm_init(slice, &config, false);
        pwm_ready = true;
    }
    pwm_set_gpio_level(s.sol_pin, s.servo_home_us);
    pwm_set_enabled(slice, true);
    s.servo_us = s.servo_home_us;
    if (servoUsesDetach(s)) {
        // Buoc 1 khoi dong: dua thanh day ve sat goc (600us), doi 250ms roi ngat xung giu
        sleep_ms(SERVO_INIT_MS);
        servoDetach(s);
    }
    s.servo_gate = Station1TrialGate{};
    s.servo_gate.position_stable_ms = LM393_STABLE_MS;
    s.servo_gate.wait_limit_ms = LM393_WAIT_LIMIT_MS;
    s.servo_gate.push_ms = s.on_time_ms;
    s.servo_gate.recover_ms = SERVO_RECOVER_MS;
    s.servo_gate.clear_ms = LM393_CLEAR_MS;
    gpio_init(s.position_pin);
    gpio_set_dir(s.position_pin, GPIO_IN);
    gpio_pull_up(s.position_pin);
    s.position_last_raw = gpio_get(s.position_pin);
}

static bool lm393Raw(const Station &s) { return gpio_get(s.position_pin); }
static bool lm393Active(const Station &s) { return lm393Raw(s) == (LM393_ACTIVE_LOW ? false : true); }

// Back to HOME and forget any pending item (pause, fault, E-stop, calib, sensor loss).
static void servoAbort(Station &s) {
    const bool was_active = (s.servo_us != 0);
    s.servo_gate.reset();
    s.measurement_ticket = 0;
    if (servoUsesDetach(s)) {
        if (was_active) {
            servoWrite(s, s.servo_home_us);
            sleep_ms(SERVO_RECOVER_MS);
            servoDetach(s);
        }
    } else {
        servoWrite(s, s.servo_home_us);
    }
}

static const char *sizeName(sorter_core::Size sz);

static void servoQueuePush(Station &s, bool push, uint32_t now) {
    auto &g = s.servo_gate;
    if (g.q_count == Station1TrialGate::QUEUE_MAX) {
        g.q_head = (g.q_head + 1) % Station1TrialGate::QUEUE_MAX;
        --g.q_count;
        printf("[%s] Hang doi day -> bo ve cu nhat\n", s.name);
    }
    const std::size_t idx = (g.q_head + g.q_count) % Station1TrialGate::QUEUE_MAX;
    g.q_push[idx] = push;
    g.q_ready[idx] = true;
    g.q_time[idx] = now;
    ++g.q_count;
}

static void servoStep(Station &s, uint32_t now, bool enabled) {
    const bool raw = lm393Raw(s);
    const bool active = lm393Active(s);
    if (raw != s.position_last_raw) {
        s.position_last_raw = raw;
        printf("[%s LM393] DO=%u -> %s (t=%lu ms)\n", s.name, (unsigned)raw,
               active ? "CO VAT" : "KHONG VAT", (unsigned long)now);
    }

    auto &g = s.servo_gate;
    if (!enabled) {
        g.reset();
        g.need_release = active;  // Neu LM393 dang dinh san luc bat, cho nha ve trong
        servoApplyStage(s);
        return;
    }

    // 1. Chu ky servo: Extended (PUSH) -> Recovery (HOME) -> Idle (DETACH cho ca 2 tram)
    if (g.stage == TrialStage::Extended && now - g.stage_since >= g.push_ms) {
        g.stage = TrialStage::Recovery;
        g.stage_since = now;
        printf(">>> [%s] SERVO GP%u THU VE GOC: PWM=%u us trong %lu ms\n",
               s.name, (unsigned)s.sol_pin, (unsigned)s.servo_home_us, (unsigned long)g.recover_ms);
    } else if (g.stage == TrialStage::Recovery && now - g.stage_since >= g.recover_ms) {
        g.stage = TrialStage::Idle;
        printf(">>> [%s] SERVO GP%u DAY THANH CONG -> NGAT XUNG GIU (PWM=0 us, DETACH) | Tong da day: %lu\n",
               s.name, (unsigned)s.sol_pin, (unsigned long)s.push_count);
    }

    // 2. Tu bo ve qua han (khong khoa tram)
    while (g.q_count > 0 && now - g.q_time[g.q_head] >= g.wait_limit_ms) {
        g.q_head = (g.q_head + 1) % Station1TrialGate::QUEUE_MAX;
        --g.q_count;
        printf("[%s] Bo ve qua han >%lums (con %u ve)\n",
               s.name, (unsigned long)g.wait_limit_ms, (unsigned)g.q_count);
    }

    // 3. Cho LM393 nha ve trong truoc khi nhan phoi tiep theo
    if (g.need_release) {
        if (active) {
            g.release_timing = false;
        } else {
            if (!g.release_timing) { g.release_timing = true; g.release_since = now; }
            if (now - g.release_since >= g.clear_ms) g.need_release = false;
        }
        servoApplyStage(s);
        return;
    }

    // 4. Canh len LM393 (TRONG -> CO VAT, on dinh >= position_stable_ms)
    if (!active) {
        g.position_timing = false;
    } else {
        if (!g.position_timing) { g.position_timing = true; g.position_since = now; }
        if (now - g.position_since >= g.position_stable_ms) {
            g.position_timing = false;
            g.need_release = true;
            g.release_timing = false;
            if (g.q_count == 0) {
                const auto early = s.detector.finish_early();
                if (early.ready &&
                    (early.action == sorter_core::Action::Push || early.action == sorter_core::Action::Pass)) {
                    const bool push = early.action == sorter_core::Action::Push;
                    printf(">>> [%s] CHOT SOM TAI LM393: mau=%u (giu %u) | d_med=%.1f mm | h=%.1f mm => SIZE %s -> %s\n",
                           s.name, (unsigned)early.total_samples, (unsigned)early.kept_samples,
                           early.median_distance_mm, early.height_mm, sizeName(early.size),
                           push ? "DAY (PUSH)" : "CHO QUA (PASS)");
                    if (push) ++s.total_count;
                    if (s.detector_cfg.station == 2 && early.size == sorter_core::Size::M) ++tray_m_count;
                    servoQueuePush(s, push, now);
                }
            }
            if (g.q_count == 0) {
                printf(">>> [%s] LM393 THAY VAT -> KHONG DAY (hang doi rong, ToF chua chot kip)\n", s.name);
            } else {
                const bool push = g.q_push[g.q_head];
                g.q_head = (g.q_head + 1) % Station1TrialGate::QUEUE_MAX;
                --g.q_count;
                if (!push) {
                    printf(">>> [%s] LM393 THAY VAT -> KHONG DAY (CHO QUA) | hang doi con %u ve\n",
                           s.name, (unsigned)g.q_count);
                } else if (g.stage != TrialStage::Idle) {
                    printf(">>> [%s] LM393 THAY VAT -> KHONG DAY (SERVO GP%u DANG BAN)\n",
                           s.name, (unsigned)s.sol_pin);
                } else {
                    g.stage = TrialStage::Extended;
                    g.stage_since = now;
                    ++s.push_count;
                    printf(">>> [%s] LM393 THAY VAT -> KICH SERVO GP%u DAY RA: PWM=%u us trong %lu ms (lan #%lu)\n",
                           s.name, (unsigned)s.sol_pin, (unsigned)s.servo_push_us,
                           (unsigned long)g.push_ms, (unsigned long)s.push_count);
                }
            }
        }
    }

    servoApplyStage(s);
}

// Stop whatever the station is doing (solenoid release or servo home).
static void stationRelease(Station &s, uint32_t now) {
    if (s.servo) { servoAbort(s); return; }
    const auto off_tick = s.actuator.tick(now, false);
    if (off_tick.changed) solenoidSet(s, false);
}

#if USE_SORTER_ENGINE
static const char *fsmStateName(sorter_core::SystemState st);

static inline bool estopTriggered() {
    return ESTOP_ACTIVE_LOW ? !gpio_get(ESTOP_PIN) : gpio_get(ESTOP_PIN);
}

static void relayUpdate() {
    if (RELAY_ACTIVE_LEVEL < 0) return;
    static int last_level = -1;
    const bool run = system_fsm.state() == sorter_core::SystemState::Running &&
                     !gpio_get(STOP_PIN) && !estopTriggered();
    const int level = run ? !RELAY_ACTIVE_LEVEL : RELAY_ACTIVE_LEVEL;
    gpio_set_dir(RELAY_PIN, GPIO_OUT);
    gpio_put(RELAY_PIN, level);
    if (level != last_level) {
        last_level = level;
        printf("[RELAY] GP%u=%d (%s)\n", (unsigned)RELAY_PIN, level,
               run ? "RUN: 3.3V nha relay -> NC dong" : "STOP: 0V hut relay -> NC ho");
    }
}

static const char *controlBlockReason() {
    if (estopTriggered() || system_fsm.state() == sorter_core::SystemState::Estop) return "ESTOP_ACTIVE";
    if (gpio_get(STOP_PIN)) return "STOP_ACTIVE";
    if (RELAY_ACTIVE_LEVEL < 0) return "RELAY_UNCONFIGURED";
    return nullptr;
}

static void pollControls(uint32_t now) {
    static bool raw_start = true, stable_start = true, armed = false;
    static bool last_estop = false, last_stop = false;
    static uint32_t changed_at = 0;
    const bool estop = estopTriggered();
    const bool stop = gpio_get(STOP_PIN);
    if (estop != last_estop || stop != last_stop) {
        printf("[CONTROL] STOP=%s E-STOP=%s\n", stop ? "OPEN" : "OK", estop ? "ACTIVE" : "OK");
        last_estop = estop;
        last_stop = stop;
    }
    system_fsm.update_inputs(stop, estop);
    if (estop || stop) {
        sorting_enabled = false;
        relayUpdate();
        for (Station &s : stations) {
            s.manual_test_active = false;
            stationRelease(s, now);
            s.detector.reset();
            s.state = ST_IDLE;
        }
    }
    const bool current = gpio_get(START_PIN);
    if (current != raw_start) { raw_start = current; changed_at = now; }
    if (raw_start != stable_start && now - changed_at >= 30) {
        stable_start = raw_start;
        printf("[CONTROL] GP%u(START)=%u (%s)\n", (unsigned)START_PIN,
               (unsigned)stable_start, stable_start ? "NHA" : "NHAN");
        if (stable_start) armed = true;
        else if (armed) {
            armed = false;
            if (!estop && !stop && RELAY_ACTIVE_LEVEL >= 0) {
                if (system_fsm.state() == sorter_core::SystemState::Estop) system_fsm.request_reset(true);
                else system_fsm.request_start();
                sorting_enabled = system_fsm.state() == sorter_core::SystemState::Running;
                printf("[CONTROL] START: %s\n", sorting_enabled ? "RUN" : "RESET / IDLE");
            } else {
                printf("[CONTROL] START blocked: %s\n", controlBlockReason());
            }
        }
    } else if (stable_start && current && now >= 30) {
        armed = true;
    }
    relayUpdate();
}
#endif

static bool stationActive(const Station &s) {
    return s.servo ? s.servo_gate.stage == TrialStage::Extended : s.actuator.is_on();
}

// Bench push. Caller has already checked FSM is Idle and sensor_ok.
static void stationManualTest(Station &s, uint32_t now) {
    if (s.servo) {
        if (!s.servo_gate.force_push(now)) return;
        s.manual_test_active = true;
        ++s.push_count;
        servoWrite(s, s.servo_push_us);
        printf("[%s] TEST: servo day (%u us)\n", s.name, (unsigned)s.servo_push_us);
    } else if (s.actuator.trigger_manual(now, s.on_time_ms)) {
        s.manual_test_active = true;
        const auto on_tick = s.actuator.tick(now, true);
        if (on_tick.changed && on_tick.solenoid_on) solenoidSet(s, true);
    }
    s.state = stationActive(s) ? ST_ACTIVE : ST_IDLE;
}

// =====================================================================
//                       HAM CAM BIEN ToF (VL53L1X)
// =====================================================================
static void i2cBusRecover(uint sda_pin, uint scl_pin) {
    gpio_init(sda_pin);
    gpio_set_dir(sda_pin, GPIO_IN);
    gpio_pull_up(sda_pin);

    gpio_init(scl_pin);
    gpio_set_dir(scl_pin, GPIO_OUT);
    gpio_put(scl_pin, 1);
    sleep_us(5);

    if (!gpio_get(sda_pin)) {
        for (int pulse = 0; pulse < 9 && !gpio_get(sda_pin); ++pulse) {
            gpio_put(scl_pin, 0);
            sleep_us(5);
            gpio_put(scl_pin, 1);
            sleep_us(5);
        }
        // Phat STOP condition de nha slave bi treo sau soft reset.
        gpio_set_dir(sda_pin, GPIO_OUT);
        gpio_put(sda_pin, 0);
        sleep_us(5);
        gpio_put(scl_pin, 1);
        sleep_us(5);
        gpio_set_dir(sda_pin, GPIO_IN);
        gpio_pull_up(sda_pin);
        sleep_us(5);
    }
}

static void i2cBusInit(Station &s) {
    i2cBusRecover(s.sda_pin, s.scl_pin);
    i2c_init(s.i2c, I2C_BAUDRATE);
    gpio_set_function(s.sda_pin, GPIO_FUNC_I2C);
    gpio_set_function(s.scl_pin, GPIO_FUNC_I2C);
    gpio_pull_up(s.sda_pin);
    gpio_pull_up(s.scl_pin);
}

static bool tofInit(Station &s) {
    s.last_retry_ms = millis();
    i2cBusInit(s);
    sleep_us(10);
    const unsigned sda_idle = gpio_get(s.sda_pin);
    const unsigned scl_idle = gpio_get(s.scl_pin);
    s.sensor_initialized = s.tof->init(true);
    if (!s.sensor_initialized) {
        printf("[%s] LOI: Khong khoi tao duoc VL53L1X tren %s (SDA=GP%u, SCL=GP%u)\n",
               s.name, s.i2c == i2c0 ? "I2C0" : "I2C1", s.sda_pin, s.scl_pin);
        const auto &diag = s.tof->initDiagnostic();
        printf("[%s] TOF_DIAG stage=%s model=0x%04X boot=0x%02X SDA_IDLE=%u SCL_IDLE=%u",
               s.name, diag.stage, (unsigned)diag.model_id, (unsigned)diag.boot_status, sda_idle, scl_idle);
        if (diag.i2c_operation) {
            const char *reason = "SHORT_TRANSFER";
            if (diag.i2c_result == PICO_ERROR_GENERIC) reason = "NO_ACK_OR_BUS_ERROR";
            else if (diag.i2c_result == PICO_ERROR_TIMEOUT) reason = "I2C_TIMEOUT";
            printf(" i2c=%s op=%s reg=0x%04X rc=%d",
                   reason, diag.i2c_operation, (unsigned)diag.i2c_register, diag.i2c_result);
        }
        printf("\n");
        s.sensor_ok = false;
        return false;
    }
    s.tof->setDistanceMode(VL53L1X::Medium);       // Che do tam trung (chinh xac cho bang tai)
    s.tof->setMeasurementTimingBudget(15000);     // Requested budget; verify actual dataReady cadence.
    s.tof->startContinuous(15);                   // Current driver ignores period_ms: back-to-back mode.
    s.sensor_ok        = true;
    s.has_valid_target = false;
    s.hit_count        = 0;
    s.clear_count      = 0;
    s.last_data_ms     = millis();
    printf("[%s] VL53L1X khoi tao THANH CONG!\n", s.name);
    return true;
}

static bool tofRead(Station &s) {
    if (!s.sensor_ok) return false;

    if (s.tof->dataReady()) {
        uint16_t d = s.tof->read(false);
        uint8_t status = s.tof->ranging_data.range_status;
        s.raw_mm = d;
        s.raw_status = status;
        if (!(status == 0 && d > 0 && d < MAX_VALID_RANGE_MM)) ++s.bad_samples;

        // Status == 0 la ket qua do hop le va tin cay
        if (status == 0 && d > 0 && d < MAX_VALID_RANGE_MM) {
            s.distance_mm = d;
            s.has_valid_target = true;
        } else {
            // Khong co vat the hoac ngoai tam do
            s.distance_mm = MAX_VALID_RANGE_MM;
            s.has_valid_target = false;
        }

        s.last_data_ms = millis();
        return true;
    }

    if (millis() - s.last_data_ms > SENSOR_TIMEOUT_MS) {
        printf("[%s] LOI: Mat tin hieu cam bien!\n", s.name);
        s.sensor_ok = false;
    }
    return false;
}

// =====================================================================
//                       LOGIC MOI TRAM (STATE MACHINE)
// =====================================================================
#if USE_SORTER_ENGINE
static const char *sizeName(sorter_core::Size sz) {
    switch (sz) {
    case sorter_core::Size::S: return "S";
    case sorter_core::Size::M: return "M";
    case sorter_core::Size::L: return "L";
    default: return "?";
    }
}

static void stationUpdate(Station &s) {
    const uint32_t now = millis();

    // 1. Kiem tra ket noi cam bien
    if (!s.sensor_ok) {
        if (s.servo) servoStep(s, now, system_fsm.state() == sorter_core::SystemState::Running);
        else stationRelease(s, now);
        s.manual_test_active = false;
        s.measurement_ticket = 0;
        s.detector.reset();
        s.state = ST_IDLE;
        if (now - s.last_retry_ms > SENSOR_RETRY_MS) tofInit(s);
        return;
    }

    // 2. Do chieu cao ToF; chi xep hang doi khi phep do hop le (khong xep nhieu 1 mau)
    if (tofRead(s)) {
        const double sample_mm = s.has_valid_target ? static_cast<double>(s.distance_mm) : -1.0;
        const auto res = s.detector.feed(sample_mm, now);
        const bool running = system_fsm.state() == sorter_core::SystemState::Running;
        if (res.started) {
            printf(">>> [%s] PHAT HIEN VAT qua ToF (d=%u mm, H0=%.1f mm)\n",
                   s.name, (unsigned)s.distance_mm, s.detector_cfg.h0_mm);
        }
        if (res.ready) {
            const char *act_str =
                res.action == sorter_core::Action::Push ? "DAY (PUSH)" :
                res.action == sorter_core::Action::Missed ? "BO LO (MISSED)" :
                res.action == sorter_core::Action::Invalid ? "LOI DO (INVALID)" : "CHO QUA (PASS)";
            printf(">>> [%s] DO XONG: mau=%u (giu %u) | d_med=%.1f mm | CHIEU CAO h=%.1f mm => QUYET DINH: SIZE %s -> %s [st=%u]%s\n",
                   s.name, (unsigned)res.total_samples, (unsigned)res.kept_samples,
                   res.median_distance_mm, res.height_mm, sizeName(res.size),
                   act_str, (unsigned)res.status, running ? "" : " (CHI LOG - CHUA CHAY)");
            if (running) {
                const bool valid_decision =
                    res.action == sorter_core::Action::Push || res.action == sorter_core::Action::Pass;
                const bool push = res.action == sorter_core::Action::Push;
                if (s.servo) {
                    if (valid_decision) {
                        servoQueuePush(s, push, now);
                        printf(">>> [%s] HANG DOI: da xep lenh [%s] cho LM393 (dang cho: %u ve)\n",
                               s.name, push ? "DAY" : "CHO QUA", (unsigned)s.servo_gate.q_count);
                    }
                } else if (push) {
                    if (!s.actuator.enqueue(res.t0_ms, sorter_core::compute_trigger_delay_ms(s.geometry)))
                        printf("[%s] CANH BAO: Hang doi hen gio day!\n", s.name);
                }
                if (push) ++s.total_count;
                if (res.action == sorter_core::Action::Missed || res.action == sorter_core::Action::Invalid)
                    ++s.missed_count;
                if (s.detector_cfg.station == 2 && res.size == sorter_core::Size::M)
                    ++tray_m_count;
            }
            s.measurement_ticket = 0;
        }
    }

    // 3. Cap nhat actuator FIFO + chan nhiet duty cycle
    const bool allow_actuation =
        system_fsm.outputs_allowed() ||
        (system_fsm.state() == sorter_core::SystemState::Idle && s.manual_test_active);
    if (s.servo) {
        servoStep(s, now, allow_actuation);
        if (s.servo_gate.stage == TrialStage::Idle) s.manual_test_active = false;
    } else {
    const sorter_core::ActuatorTick tick = s.actuator.tick(now, allow_actuation);
    if (tick.changed) solenoidSet(s, tick.solenoid_on);
    if (!tick.solenoid_on) s.manual_test_active = false;
    if (tick.overheat_fault) {
        system_fsm.trigger_fault(sorter_core::FaultCode::Overheat);
        sorting_enabled = false;
        printf("[%s] LOI QUA NHIET SOLENOID (duty=%.0f%%) -> FAULT!\n",
               s.name, tick.duty_ratio * 100.0);
    }
    }

    // 4. Dong bo trang thai hien thi Serial / Dashboard
    if (stationActive(s)) {
        s.state = ST_ACTIVE;
    } else if (s.detector.state() == sorter_core::ItemDetector::State::WaitClear) {
        s.state = ST_WAIT_CLEAR;
    } else {
        s.state = ST_IDLE;
    }
}
#else
static void stationUpdate(Station &s) {
    uint32_t now = millis();

    // 1. Kiem tra ket noi cam bien
    if (!s.sensor_ok) {
        if (s.state != ST_IDLE) {
            solenoidSet(s, false);
            s.state = ST_IDLE;
        }
        if (now - s.last_retry_ms > SENSOR_RETRY_MS) tofInit(s);
        return;
    }

    // 2. Doc cam bien & loc nhieu (debounce)
    if (tofRead(s)) {
        bool matches_size = s.has_valid_target &&
                            s.distance_mm >= s.min_mm && s.distance_mm <= s.max_mm;
        if (matches_size) { if (s.hit_count < 255) s.hit_count++; }
        else       { s.hit_count = 0; }

        // Chi phep nhan phoi moi khi thay mat bang tai, khong dung mau loi de mo khoa.
        if (s.has_valid_target && s.distance_mm >= BELT_CLEAR_MIN_MM) {
            if (s.clear_count < 255) s.clear_count++;
        } else {
            s.clear_count = 0;
        }
    }

    bool object_detected = (s.hit_count >= DEBOUNCE_COUNT);
    bool object_cleared  = (s.clear_count >= CLEAR_COUNT);

    // 3. Phat hien vat moi -> dua vao hang doi (Queue)
    if (sorting_enabled && s.state == ST_IDLE && object_detected) {
        s.total_count++;
        printf("[%s] PHAT HIEN VAT #%lu: d=%u mm (size %u-%u mm)\n",
               s.name, (unsigned long)s.total_count, s.distance_mm, s.min_mm, s.max_mm);

        if (s.travel_delay_ms == 0) {
            solenoidSet(s, true);
            s.state = ST_ACTIVE;
            s.state_start_ms = now;
        } else {
            queuePush(s, now + s.travel_delay_ms);
            printf("[%s] Cho vat chay toi tay gat sau %lu ms...\n", s.name, (unsigned long)s.travel_delay_ms);
            s.state = ST_WAIT_CLEAR;
        }
    }

    // 4. Kiem tra hang doi tre di chuyen
    if (s.state != ST_ACTIVE && s.q_count > 0) {
        uint32_t target_time = 0;
        if (queuePeek(s, &target_time) && (int32_t)(now - target_time) >= 0) {
            queuePop(s, &target_time);
            solenoidSet(s, true);
            s.state = ST_ACTIVE;
            s.state_start_ms = now;
        }
    }

    // 5. Quan ly thoi gian Solenoid dang hut
    if (s.state == ST_ACTIVE) {
        uint32_t elapsed = now - s.state_start_ms;

        if (s.mode == MODE_PULSE) {
            if (elapsed >= s.on_time_ms) {
                solenoidSet(s, false);
                s.state = ST_WAIT_CLEAR;
            }
        } else { // MODE_HOLD
            if (object_cleared) {
                printf("[%s] Vat da qua tay gat sau %lu ms\n", s.name, (unsigned long)elapsed);
                solenoidSet(s, false);
                s.state = ST_IDLE;
                s.hit_count = 0;
            } else if (elapsed >= s.on_time_ms) {
                printf("[%s] CANH BAO: Hut qua %lu ms, nha de bao ve cuon day!\n",
                       s.name, (unsigned long)s.on_time_ms);
                solenoidSet(s, false);
                s.state = ST_WAIT_CLEAR;
            }
        }
    }

    // 6. Cho vat roi khoi vung do truoc khi nhan vat tiep theo
    if (s.state == ST_WAIT_CLEAR) {
        if (object_cleared) {
            s.state = ST_IDLE;
            s.hit_count = 0;
        }
    }
}
#endif

// =====================================================================
//                       DEBUG SERIAL
// =====================================================================
static const char *stateName(StationState st) {
    switch (st) {
    case ST_IDLE:       return "IDLE";
    case ST_ACTIVE:     return "ACTIVE";
    case ST_WAIT_CLEAR: return "WAIT_CLR";
    }
    return "?";
}

static const char *sensorErrorName(const Station &s) {
    const auto &diag = s.tof->initDiagnostic();
    if (diag.i2c_operation) {
        if (diag.i2c_result == PICO_ERROR_TIMEOUT) return "I2C_TIMEOUT";
        if (diag.i2c_result == PICO_ERROR_GENERIC) return "NO_ACK_OR_BUS_ERROR";
        return "SHORT_TRANSFER";
    }
    return s.sensor_initialized ? "MAT_DU_LIEU" : "LOI_KHOI_TAO";
}

static void printStatus() {
    for (int i = 0; i < 2; i++) {
        Station &s = stations[i];
        if (!s.sensor_ok) {
            printf("%s: %s !          ", s.name, sensorErrorName(s));
        } else if (!s.has_valid_target || s.distance_mm >= MAX_VALID_RANGE_MM) {
            printf("%s: d= ----mm [%-8s | #%2lu] ", s.name, stateName(s.state), (unsigned long)s.total_count);
        } else {
            printf("%s: d=%4umm [%-8s | #%2lu] ", s.name, s.distance_mm, stateName(s.state), (unsigned long)s.total_count);
        }
    }
#if USE_SORTER_ENGINE
    printf("| FSM=%-7s G18=%u ST19=%u SP20=%u ES21=%u G22=%u RL26=%u | LM1=%u(%c) LM2=%u(%c) | SV=(%u,%u) Q=(%u,%u)",
           fsmStateName(system_fsm.state()),
           (unsigned)gpio_get(18),
           (unsigned)gpio_get(START_PIN),
           (unsigned)gpio_get(STOP_PIN),
           (unsigned)gpio_get(ESTOP_PIN),
           (unsigned)gpio_get(22),
           RELAY_ACTIVE_LEVEL >= 0 ? (unsigned)gpio_get_out_level(RELAY_PIN) : 0u,
           (unsigned)lm393Raw(stations[0]), lm393Active(stations[0]) ? 'V' : '-',
           (unsigned)lm393Raw(stations[1]), lm393Active(stations[1]) ? 'V' : '-',
           (unsigned)stations[0].servo_us,
           (unsigned)stations[1].servo_us,
           (unsigned)stations[0].servo_gate.q_count,
           (unsigned)stations[1].servo_gate.q_count);
#endif
    printf("\n");
}

// =====================================================================
//                       MAIN
// =====================================================================
// Only core 0 reads the stations or changes an output. Core 1 owns CYW43/lwIP.
static queue_t snapshot_queue, command_queue, ack_queue;
alignas(32) static uint32_t network_stack[2048];

// One rule set for both the permissions shown on the web and the checks before acting.
static const char *denyReason(pegatron_action action, unsigned station) {
#if USE_SORTER_ENGINE
    if (action == PEGATRON_ENABLE || action == PEGATRON_TEST) {
        if (const char *reason = controlBlockReason()) return reason;
    }
    switch (action) {
    case PEGATRON_ENABLE:
        if (system_fsm.state() == sorter_core::SystemState::Running) return "ALREADY_SORTING";
        if (system_fsm.estop_active() || system_fsm.state() == sorter_core::SystemState::Estop) {
            return "ESTOP_ACTIVE";
        }
        return nullptr;
    case PEGATRON_PAUSE:
        return system_fsm.state() == sorter_core::SystemState::Running ? nullptr : "ALREADY_PAUSED";
    case PEGATRON_TEST: {
        const Station &s = stations[station - 1];
        if (system_fsm.state() == sorter_core::SystemState::Running) return "SORTING_ACTIVE";
        if (system_fsm.estop_active() || system_fsm.state() == sorter_core::SystemState::Estop) {
            return "ESTOP_ACTIVE";
        }
        if (system_fsm.state() == sorter_core::SystemState::Fault) return "FAULT_ACTIVE";
        if (!s.sensor_ok) return "TOF_UNAVAILABLE";
        return s.state == ST_IDLE ? nullptr : "STATION_BUSY";
    }
    default:                 return "NOT_SUPPORTED";
    }
#else
    switch (action) {
    case PEGATRON_ENABLE:    return sorting_enabled ? "ALREADY_SORTING" : nullptr;
    case PEGATRON_PAUSE:     return sorting_enabled ? nullptr : "ALREADY_PAUSED";
    case PEGATRON_TEST: {
        const Station &s = stations[station - 1];
        if (sorting_enabled) return "SORTING_ACTIVE";
        if (!s.sensor_ok) return "TOF_UNAVAILABLE";
        return s.state == ST_IDLE ? nullptr : "STATION_BUSY";
    }
    default:                 return "NOT_SUPPORTED";
    }
#endif
}

static void answerCommand(const pegatron_command &command, const char *reason) {
    pegatron_ack ack{};
    snprintf(ack.request_id, sizeof ack.request_id, "%s", command.request_id);
    snprintf(ack.reason, sizeof ack.reason, "%s", reason ? reason : "OK");
    ack.executed = reason == nullptr;
    ack.mode = sorting_enabled ? PEGATRON_SORTING : PEGATRON_IDLE;  // Read back after acting.
    queue_try_add(&ack_queue, &ack);
}

static void runCommands() {
    pegatron_command command;
    while (queue_try_remove(&command_queue, &command)) {
        const char *reason = denyReason(command.action, command.station);
        if (!reason) {
            const uint32_t now = millis();
#if USE_SORTER_ENGINE
            if (command.action == PEGATRON_ENABLE) {
                if (system_fsm.state() == sorter_core::SystemState::Fault) {
                    system_fsm.request_reset(false);
                }
                system_fsm.request_start();
                sorting_enabled = (system_fsm.state() == sorter_core::SystemState::Running);
            } else if (command.action == PEGATRON_PAUSE) {
                system_fsm.request_stop();
                sorting_enabled = false;
                for (Station &s : stations) {  // Release at once, drop pending delayed pushes.
                    s.manual_test_active = false;
                    stationRelease(s, now);
                    s.detector.reset();
                    s.state = ST_IDLE;
                }
            } else {  // PEGATRON_TEST: one normal pulse; the state machine ends it.
                Station &s = stations[command.station - 1];
                stationManualTest(s, now);
            }
#else
            if (command.action == PEGATRON_ENABLE) {
                sorting_enabled = true;
            } else if (command.action == PEGATRON_PAUSE) {
                sorting_enabled = false;
                for (Station &s : stations) {  // Release at once, drop pending delayed pushes.
                    s.q_count = s.q_head = s.q_tail = 0;
                    if (s.state == ST_ACTIVE) {
                        solenoidSet(s, false);
                        s.state = ST_WAIT_CLEAR;
                    }
                }
            } else {  // PEGATRON_TEST: one normal pulse; the state machine ends it.
                Station &s = stations[command.station - 1];
                solenoidSet(s, true);
                s.state = ST_ACTIVE;
                s.state_start_ms = now;
            }
#endif
        }
        answerCommand(command, reason);
    }
}

static pegatron_snapshot makeSnapshot() {
    const int references[] = {PEGATRON_REFERENCE_1_MM, PEGATRON_REFERENCE_2_MM};
    pegatron_snapshot snapshot{};
#if USE_SORTER_ENGINE
    snapshot.estop = estopTriggered() ? PEGATRON_TRUE : PEGATRON_FALSE;
    const auto fsm_state = system_fsm.state();
    snapshot.mode = (fsm_state == sorter_core::SystemState::Fault ||
                     fsm_state == sorter_core::SystemState::Estop)
                        ? PEGATRON_FAULT
                        : (sorting_enabled ? PEGATRON_SORTING : PEGATRON_IDLE);
#else
    snapshot.mode = sorting_enabled ? PEGATRON_SORTING : PEGATRON_IDLE;
#endif
    snapshot.deny[0] = denyReason(PEGATRON_ENABLE, 0);
    snapshot.deny[1] = denyReason(PEGATRON_PAUSE, 0);
    snapshot.deny[2] = denyReason(PEGATRON_TEST, 1);
    snapshot.deny[3] = denyReason(PEGATRON_TEST, 2);
    snapshot.deny[4] = snapshot.deny[5] = denyReason(PEGATRON_CALIBRATE, 0);
    for (unsigned i = 0; i < 2; ++i) {
        const Station &source = stations[i];
        pegatron_station &target = snapshot.station[i];
        const bool valid = source.sensor_ok && source.has_valid_target;
#if USE_SORTER_ENGINE
        const int ref = source.detector_cfg.h0_mm > 0.0
                            ? static_cast<int>(source.detector_cfg.h0_mm + 0.5)
                            : references[i];
#else
        const int ref = references[i];
#endif
        const int height = ref - source.distance_mm;
        target.sample_valid = target.distance_known = valid;
        target.distance_mm = source.distance_mm;
        target.push_count = source.push_count;
        target.reference_known = ref > 0;
        target.reference_mm = ref;
        target.height_known = valid && target.reference_known && height >= -2000 && height <= 2000;
        target.height_mm = target.height_known ? height : 0;
        snapshot.tof_unavailable[i] = !source.sensor_ok;
    }
#if USE_SORTER_ENGINE
    snapshot.tray_confirmed[0] = true;
    snapshot.tray_confirmed[1] = true;
    snapshot.tray_confirmed[2] = true;
    snapshot.tray_count[0] = stations[1].total_count;
    snapshot.tray_count[1] = tray_m_count;
    snapshot.tray_count[2] = stations[0].total_count;
#endif
    return snapshot;
}

static void networkWorker() {
    const pegatron_settings settings = {
        "pico-01", PEGATRON_WIFI_SSID, PEGATRON_WIFI_PASSWORD, PEGATRON_BROKER_IPV4,
        "pico-01", PEGATRON_MQTT_PASSWORD, 1883, &command_queue, &ack_queue
    };
    if (!pegatron_monitor_init(&settings)) {
        while (true) sleep_ms(1000);
    }
    pegatron_snapshot snapshot{};
    pegatron_snapshot next{};
    bool have_snapshot = false;
    uint32_t last_blink = 0;
    bool led_state = false;
    while (true) {
        bool changed = false;
        while (queue_try_remove(&snapshot_queue, &next)) {
            changed |= !have_snapshot || next.mode != snapshot.mode || next.estop != snapshot.estop ||
                next.tof_unavailable[0] != snapshot.tof_unavailable[0] ||
                next.tof_unavailable[1] != snapshot.tof_unavailable[1];
            snapshot = next;
            have_snapshot = true;
        }
        if (have_snapshot) pegatron_monitor_tick(&snapshot, changed);
        const uint32_t now = millis();
        if (now - last_blink >= 500) {
            last_blink = now;
            led_state = !led_state;
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_state);
        }
        sleep_ms(2);
    }
}

#if USE_SORTER_ENGINE
static const char *fsmStateName(sorter_core::SystemState st) {
    switch (st) {
    case sorter_core::SystemState::Init:    return "INIT";
    case sorter_core::SystemState::Idle:    return "IDLE";
    case sorter_core::SystemState::Running: return "RUNNING";
    case sorter_core::SystemState::Manual:  return "MANUAL";
    case sorter_core::SystemState::Calib:   return "CALIB";
    case sorter_core::SystemState::Fault:   return "FAULT";
    case sorter_core::SystemState::Estop:   return "ESTOP";
    }
    return "?";
}

static void runCalibC1() {
    const bool was_running = (system_fsm.state() == sorter_core::SystemState::Running);
    if (was_running) {
        system_fsm.request_stop();
        sorting_enabled = false;
    }
    const uint32_t t_now = millis();
    for (Station &s : stations) {
        s.manual_test_active = false;
        stationRelease(s, t_now);
    }

    printf("[CLI] Dang thu 30 mau bang tai trong cho C1 (toi da 2.5s)...\n");
    constexpr std::size_t C1_TARGET = 30;
    double samples[2][C1_TARGET]{};
    std::size_t count[2]{};
    const uint32_t t_start = millis();

    while ((count[0] < C1_TARGET || count[1] < C1_TARGET) &&
           (millis() - t_start < 2500)) {
        for (int i = 0; i < 2; ++i) {
            if (stations[i].sensor_ok && count[i] < C1_TARGET && tofRead(stations[i]) &&
                stations[i].has_valid_target) {
                samples[i][count[i]++] = static_cast<double>(stations[i].distance_mm);
            }
        }
        sleep_ms(2);
    }

    for (int i = 0; i < 2; ++i) {
        Station &s = stations[i];
        if (count[i] == 0) {
            printf("[%s] CALIB C1 BO QUA: khong co du lieu ToF\n", s.name);
            continue;
        }
        const auto res = sorter_core::compute_c1_baseline(samples[i], count[i], 1.0);
        if (res.h0_mm < 75.0 || res.h0_mm > 120.0) {
            printf("[%s] CALIB C1 BO QUA: H0=%.1fmm bat thuong (co vat che ToF luc bat nguon?) -> giu H0=%.1fmm\n",
                   s.name, res.h0_mm, s.detector_cfg.h0_mm);
            continue;
        }
        printf("[%s] CALIB C1 (%u mau): H0=%.1fmm, sigma0=%.2fmm -> %s\n",
               s.name, (unsigned)res.valid_count, res.h0_mm, res.sigma_mm,
               res.ok ? "DAT" : "NHIEU (>1mm)");
        s.detector_cfg.h0_mm = res.h0_mm;
        s.detector.set_config(s.detector_cfg);
    }

    if (was_running && !controlBlockReason()) {
        system_fsm.request_start();
        sorting_enabled = (system_fsm.state() == sorter_core::SystemState::Running);
    }
}

static bool applyStationConfig(double l_min, double l_max, double m_min, double m_max,
                               uint32_t s1_on, uint32_t s1_td,
                               uint32_t s2_on, uint32_t s2_td, uint32_t min_off) {
    if (system_fsm.state() != sorter_core::SystemState::Idle ||
        !sorter_core::is_finite(l_min) || !sorter_core::is_finite(l_max) ||
        !sorter_core::is_finite(m_min) || !sorter_core::is_finite(m_max) ||
        l_min < 0.0 || l_min >= l_max || l_max > 300.0 ||
        m_min <= S_HEIGHT_MIN_MM || m_min >= m_max || m_max > 300.0 ||
        s1_on < 50 || s1_on > 600 ||
        s2_on < 50 || s2_on > 600 ||
        s1_td != 0 || s2_td != 0 ||  // LM393 xac nhan vi tri; do tre ToF khong dung
        min_off < 40 || min_off > 500) {
        return false;
    }
    const uint32_t on_vals[2] = { s1_on, s2_on };
    const uint32_t td_vals[2] = { s1_td, s2_td };
    for (int i = 0; i < 2; ++i) {
        Station &s = stations[i];
        stationRelease(s, millis());
        s.on_time_ms = on_vals[i];
        if (s.servo) s.servo_gate.push_ms = on_vals[i];
        s.travel_delay_ms = td_vals[i];
        s.geometry.fallback_delay_ms = td_vals[i];
        s.detector_cfg.use_ranges = true;
        s.detector_cfg.l_min_mm = l_min;
        s.detector_cfg.l_max_mm = l_max;
        s.detector_cfg.m_min_mm = m_min;
        s.detector_cfg.m_max_mm = m_max;
        s.detector.set_config(s.detector_cfg);
        s.actuator.set_config(sorter_core::SolenoidConfig{
            s.on_time_ms, SOLENOID_MAX_ON_MS, 60000, SOLENOID_RATED_DUTY, 0.80, min_off});
        if (i == 0) {
            s.min_mm = s.detector_cfg.h0_mm > l_max ? static_cast<uint16_t>(s.detector_cfg.h0_mm - l_max) : 0;
            s.max_mm = s.detector_cfg.h0_mm > l_min ? static_cast<uint16_t>(s.detector_cfg.h0_mm - l_min) : 0;
        } else {
            s.min_mm = s.detector_cfg.h0_mm > m_min ? static_cast<uint16_t>(s.detector_cfg.h0_mm - m_min) : 0;
            s.max_mm = s.detector_cfg.h0_mm > s.detector_cfg.s_min_mm ? static_cast<uint16_t>(s.detector_cfg.h0_mm - s.detector_cfg.s_min_mm) : 0;
        }
    }
    printf("[CFG] OK L=[%.1f,%.1f] M=[%.1f,%.1f] S1=(on=%lu,td=%lu) S2=(on=%lu,td=%lu) OFF=%lu\n",
           l_min, l_max, m_min, m_max,
           (unsigned long)s1_on, (unsigned long)s1_td,
           (unsigned long)s2_on, (unsigned long)s2_td, (unsigned long)min_off);
    return true;
}

static void handleSerialCommands() {
    static bool reading_cfg = false;
    static char cfg_buf[128]{};
    static size_t cfg_len = 0;

    if (reading_cfg) {
        for (;;) {
            const int ch = getchar_timeout_us(0);
            if (ch == PICO_ERROR_TIMEOUT) return;
            if (ch == '\r' || ch == '\n') {
                cfg_buf[cfg_len] = '\0';
                double v1 = 0.0, v2 = 0.0, v3 = 0.0, v4 = 0.0;
                unsigned long a = 0, b = 0, c = 0, d = 0, e = 0;
                bool ok = false;
                if (sscanf(cfg_buf, "%lf %lf %lf %lf %lu %lu %lu %lu %lu",
                           &v1, &v2, &v3, &v4, &a, &b, &c, &d, &e) == 9) {
                    ok = applyStationConfig(v1, v2, v3, v4,
                                            (uint32_t)a, (uint32_t)b,
                                            (uint32_t)c, (uint32_t)d, (uint32_t)e);
                } else if (sscanf(cfg_buf, "%lf %lf %lu %lu %lu %lu %lu",
                                  &v1, &v2, &a, &b, &c, &d, &e) == 7 && v1 > 0.0 && v2 > v1) {
                    ok = applyStationConfig(v2, 200.0, v1, v2,
                                            (uint32_t)a, (uint32_t)b,
                                            (uint32_t)c, (uint32_t)d, (uint32_t)e);
                }
                if (!ok) printf("[CFG] ERR: %s\n", cfg_buf);
                reading_cfg = false;
                cfg_len = 0;
                return;
            }
            if (cfg_len + 1 < sizeof cfg_buf) {
                cfg_buf[cfg_len++] = static_cast<char>(ch);
            }
        }
    }

    const int ch = getchar_timeout_us(0);
    if (ch == PICO_ERROR_TIMEOUT || ch == '\r' || ch == '\n') return;

    const uint32_t now = millis();
    if (ch == 'k' || ch == 'K') {
        reading_cfg = true;
        cfg_len = 0;
        return;
    }
    if (ch == 'c' || ch == 'C') {
        runCalibC1();
    } else if (ch == 'p' || ch == 'P') {
        if (system_fsm.state() == sorter_core::SystemState::Running) {
            system_fsm.request_stop();
            sorting_enabled = false;
            for (Station &s : stations) {
                s.manual_test_active = false;
                stationRelease(s, now);
                s.detector.reset();
                s.state = ST_IDLE;
            }
            printf("[CLI] -> PAUSE (IDLE)\n");
        } else {
            if (const char *reason = controlBlockReason()) {
                printf("[CLI] START TU CHOI: %s\n", reason);
                return;
            }
            if (system_fsm.state() == sorter_core::SystemState::Fault) {
                system_fsm.request_reset(false);
            }
            const auto r = system_fsm.request_start();
            sorting_enabled = (system_fsm.state() == sorter_core::SystemState::Running);
            printf("[CLI] -> START: %s (%s)\n", r.ok ? "RUNNING" : "TU CHOI", r.reason);
        }
    } else if (ch == '1' || ch == '2') {
        if (const char *reason = controlBlockReason()) {
            printf("[CLI] TEST TU CHOI: %s\n", reason);
            return;
        }
        if (system_fsm.state() != sorter_core::SystemState::Idle) {
            printf("[CLI] Dang %s - nhan 'p' de tam dung truoc khi test Solenoid.\n",
                   fsmStateName(system_fsm.state()));
            return;
        }
        Station &s = stations[(ch == '1') ? 0 : 1];
        if (!s.sensor_ok) {
            printf("[%s] Bo qua test: TOF_UNAVAILABLE\n", s.name);
            return;
        }
        stationManualTest(s, now);
    } else if (ch == 's' || ch == 'S') {
        printf("[STATUS] FSM=%s | H0=(%.1f, %.1f) L=[%.1f,%.1f] M=[%.1f,%.1f] | S1=(on=%lu,td=%lu) S2=(on=%lu,td=%lu) | ",
               fsmStateName(system_fsm.state()),
               stations[0].detector_cfg.h0_mm, stations[1].detector_cfg.h0_mm,
               stations[0].detector_cfg.l_min_mm, stations[0].detector_cfg.l_max_mm,
               stations[0].detector_cfg.m_min_mm, stations[0].detector_cfg.m_max_mm,
               (unsigned long)stations[0].on_time_ms, (unsigned long)stations[0].travel_delay_ms,
               (unsigned long)stations[1].on_time_ms, (unsigned long)stations[1].travel_delay_ms);
        printf("T1(tot=%lu,push=%lu) T2(tot=%lu,push=%lu,miss=%lu)\n",
               (unsigned long)stations[0].total_count, (unsigned long)stations[0].push_count,
               (unsigned long)stations[1].total_count, (unsigned long)stations[1].push_count,
               (unsigned long)stations[1].missed_count);
        printf("[TOF1] raw=%u status=%u bad=%lu det_state=%u H0=%.1f missed=%lu\n",
               (unsigned)stations[0].raw_mm, (unsigned)stations[0].raw_status,
               (unsigned long)stations[0].bad_samples, (unsigned)stations[0].detector.state(),
               stations[0].detector_cfg.h0_mm, (unsigned long)stations[0].missed_count);
        for (const Station &s : stations) {
            printf("[%s SERVO] LM393_DO=%u active=%u stage=%u pulse=%u us (HOME=%u PUSH=%u) push_ms=%lu miss=%lu\n",
                   s.name, (unsigned)lm393Raw(s), (unsigned)lm393Active(s), (unsigned)s.servo_gate.stage,
                   (unsigned)s.servo_us, (unsigned)s.servo_home_us, (unsigned)s.servo_push_us,
                   (unsigned long)s.servo_gate.push_ms, (unsigned long)s.missed_count);
        }
    } else if (ch == '?' || ch == 'h' || ch == 'H') {
        printf("[CLI] c=Calib H0 | p=Pause/Run | 1/2=Test servo | s=Status | k <l_min> <l_max> <m_min> <m_max> <s1_on> 0 <s2_on> 0 <off>\n");
    }
}
#endif

int main() {
    stdio_init_all();

#if USE_SORTER_ENGINE
    const uint input_pins[] = {18, START_PIN, STOP_PIN, ESTOP_PIN, 22};
    for (const uint pin : input_pins) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_IN);
        gpio_pull_up(pin);
    }
    if (RELAY_ACTIVE_LEVEL >= 0) {
        gpio_init(RELAY_PIN);
        gpio_put(RELAY_PIN, RELAY_ACTIVE_LEVEL);
        gpio_set_dir(RELAY_PIN, GPIO_OUT);
    }
#endif

    for (int i = 0; i < 30 && !stdio_usb_connected(); i++) sleep_ms(100);
    printf("\n=======================================================\n");
    printf("   BANG TAI PHAN LOAI TU DONG - Raspberry Pi Pico 2 W   \n");
    printf("=======================================================\n");
#if USE_SORTER_ENGINE
    printf("[CLI] c=Calib H0 | p=Pause/Run | 1/2=Test servo | s=Status\n");
#endif

    for (int i = 0; i < 2; i++) {
        Station &s = stations[i];
        s.state = ST_IDLE;
        s.distance_mm = MAX_VALID_RANGE_MM;
        s.has_valid_target = false;
        s.total_count = 0;
        s.q_head = s.q_tail = s.q_count = 0;
#if USE_SORTER_ENGINE
        const int configured_ref = (i == 0) ? PEGATRON_REFERENCE_1_MM : PEGATRON_REFERENCE_2_MM;
        const double ref_mm = configured_ref > 0 ? static_cast<double>(configured_ref) : DEFAULT_H0_MM;
        sorter_core::StationConfig cfg{};
        cfg.station = static_cast<unsigned>(i + 1);
        cfg.h0_mm = ref_mm;
        cfg.detect_delta_mm = DETECT_DELTA_MM;
        cfg.clear_delta_mm = CLEAR_DELTA_MM;
        cfg.threshold_sm_mm = THRESHOLD_SM_MM;
        cfg.threshold_ml_mm = THRESHOLD_ML_MM;
        cfg.use_ranges = true;
        cfg.l_min_mm = L_HEIGHT_MIN_MM;
        cfg.l_max_mm = L_HEIGHT_MAX_MM;
        cfg.m_min_mm = M_HEIGHT_MIN_MM;
        cfg.m_max_mm = M_HEIGHT_MAX_MM;
        cfg.s_min_mm = S_HEIGHT_MIN_MM;
        cfg.min_valid_samples = MIN_VALID_SAMPLES;
        cfg.clear_samples = 2;
        cfg.max_valid_distance_mm = static_cast<double>(MAX_VALID_RANGE_MM);
        s.detector_cfg = cfg;
        if (!s.detector.set_config(s.detector_cfg)) {
            printf("[%s] LOI CAU HINH ItemDetector (h0=%.1f min_samples=%u)!\n",
                   s.name, s.detector_cfg.h0_mm, (unsigned)s.detector_cfg.min_valid_samples);
        }
        s.actuator = sorter_core::SolenoidActuator(
            sorter_core::SolenoidConfig{s.on_time_ms, SOLENOID_MAX_ON_MS, 60000, SOLENOID_RATED_DUTY, 0.80, SOLENOID_MIN_OFF_MS});
        s.geometry = sorter_core::TriggerGeometry{0.0, 0.0, 0.0, 0.0, 0.0, s.travel_delay_ms};
        s.missed_count = 0;
        s.manual_test_active = false;
        s.servo = true;
        s.position_pin = (i == 0) ? LM393_1_PIN : LM393_2_PIN;
        s.servo_home_us = (i == 0) ? SERVO1_HOME_US : SERVO2_HOME_US;
        s.servo_push_us = (i == 0) ? SERVO1_PUSH_US : SERVO2_PUSH_US;
#endif
        if (s.servo) servoInit(s); else solenoidInit(s.sol_pin);
        tofInit(s);
    }
#if USE_SORTER_ENGINE
    system_fsm.init_ok();
    sorting_enabled = (system_fsm.state() == sorter_core::SystemState::Running);
    // No fixed reference -> measure H0 on the EMPTY belt at boot (82 mm default is rarely right).
    if (PEGATRON_REFERENCE_1_MM <= 0 || PEGATRON_REFERENCE_2_MM <= 0) {
        printf("[BOOT] De BANG TAI TRONG: tu do H0...\n");
        runCalibC1();
    }
#if AUTO_START_ON_BOOT
    {
        const bool estop = estopTriggered();
        const bool stop = gpio_get(STOP_PIN);
        system_fsm.update_inputs(stop, estop);
        if (!estop && !stop && RELAY_ACTIVE_LEVEL >= 0) {
            system_fsm.request_start();
        }
        sorting_enabled = (system_fsm.state() == sorter_core::SystemState::Running);
        relayUpdate();
        printf("[BOOT] AUTO-START: %s\n",
               sorting_enabled ? "RUNNING" : (controlBlockReason() ? controlBlockReason() : "IDLE"));
    }
#endif
#endif

    const bool network_enabled = (ENABLE_WIFI != 0) && (PEGATRON_WIFI_SSID[0] != '\0');
    if (network_enabled) {
        queue_init(&snapshot_queue, sizeof(pegatron_snapshot), 4);
        queue_init(&command_queue, sizeof(pegatron_command), 4);
        queue_init(&ack_queue, sizeof(pegatron_ack), 4);
        flash_safe_execute_core_init();  // Lets core 1 save Wi-Fi credentials to flash.
        multicore_launch_core1_with_stack(networkWorker, network_stack, sizeof network_stack);
    }
    uint32_t last_print = 0, last_snapshot = 0;

    while (true) {
#if USE_SORTER_ENGINE
        pollControls(millis());
        handleSerialCommands();
#endif
        stationUpdate(stations[0]);
        stationUpdate(stations[1]);

        uint32_t now = millis();
        if (now - last_print >= PRINT_PERIOD_MS) {
            last_print = now;
            printStatus();
        }
        if (network_enabled) runCommands();
#if USE_SORTER_ENGINE
        pollControls(millis());
#endif
        if (network_enabled && now - last_snapshot >= 100) {
            last_snapshot = now;
            const auto snapshot = makeSnapshot();
            queue_try_add(&snapshot_queue, &snapshot);
        }
    }
}
