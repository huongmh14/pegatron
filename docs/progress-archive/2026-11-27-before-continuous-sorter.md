# Tiến độ dự án BangTaiPhanLoai

## Servo đẩy trạm 2 (`D-15`) — 2026-11-26

- [x] Firmware C++ dùng servo GP11 + LM393 GP13 cho trạm 2, giữ GP10/GP12 cho trạm 1; cấu hình PWM chung slice và FIFO riêng từng trạm.
- [x] Dashboard chỉ cho chỉnh thời gian đẩy hai servo; độ trễ gạt bằng 0, ẩn cấu hình hồi solenoid cũ; README có sơ đồ chân và quy trình hiệu chỉnh.
- [x] Kiểm tra: build firmware bằng Pico SDK, biên dịch `static_assert` C++ bằng ARM toolchain, `node --check` và 20 test dashboard Python.
- [~] Nạp UF2, đấu dây, kiểm tra cực tính LM393, hành trình HOME/PUSH và phôi S/M/L trên băng tải: người dùng sẽ tự thực hiện.

## Đợt M4 + USB Bridge + Web Config (`D-12`..`D-14`) — 2026-11-25

- [x] **Sửa gạt mẫu gần 2–3 cm & I2C ToF:** `SolenoidActuator` thêm `min_off_time_ms` (120 ms) bảo đảm nhả hồi giữa 2 nhịp; `vl53l1x.cpp` xóa cờ `i2c_error` sau soft-reset `0x0000`; ToF chạy `15 ms`, `CLEAR_COUNT=1`, `DETECT=6 mm`, `CLEAR=5 mm`.
- [x] **`D-12` + `D-13` + `D-14`:** Bỏ ước lượng tốc độ băng tải trong runtime; đổi thứ tự phân loại `L → M → S` (Trạm 1 gạt `L`, Trạm 2 gạt `M`, `S` đi thẳng); tách 2 khoảng chiều cao độc lập `L=[22,100]` và `M=[7,20]` hỗ trợ khe đệm chống nhiễu ToF (`classify_ranges`).
- [x] **Cầu nối USB Serial & Cấu hình tay trên Web:** Thêm `dashboard/serial_bridge.py` đọc `COM6` -> MQTT (`pico-01`), API `GET/POST /api/config` (`k <9 tham số>`), và form **Cấu hình hai trạm (nhập tay)** (`index.html`, `dashboard.js`).
- [x] **Xác minh:** C++ `g++ -fsyntax-only` (3 file test), `node --check`, `20` unit test Python (`unittest`) đều đạt; build xong `build/BangTaiPhanLoai.uf2` (837,632 bytes).
- [~] **Điểm dừng & Bước tiếp theo:** Kéo thả `build/BangTaiPhanLoai.uf2` vào ổ `RP2350` (`BOOTSEL`), bấm `F5` tại `http://127.0.0.1:8000/` để kiểm thử thực tế `L → M → S` và lưu cấu hình trực tiếp từ Web.

## Đợt M3 + M5 + M6 — 2026-11-25

- [x] **M3 (`SORT-AC01`, `SORT-AC06`, `SORT-R01`, `SORT-R06`, `SORT-R07`):** Tạo `sorter_fsm.h` (pure C++17 `constexpr`, 0 cấp phát động) gồm `SystemFsm` 7 trạng thái (`Init`, `Idle`, `Running`, `Manual`, `Calib`, `Fault`, `Estop`), `compute_trigger_delay_ms` (công thức hình học động + fallback), và `SolenoidActuator` (hàng đợi FIFO 8 phần tử, chặn trên `t_on_max`, bảo vệ quá nhiệt cửa sổ trượt 60 s `D_rated` + cảnh báo 80%). Tạo `tests/test_sorter_fsm.cpp` và xác minh bằng `g++ -std=c++17 -Wall -Wextra -Werror -fsyntax-only`.
- [x] **M5 + M6 (`CALIB-AC02..06`, gỡ kẹt I²C, Serial CLI):**
  - Tạo `calib_math.h` (C1 `compute_c1_baseline`, C2 `fit_c2_linear`, hợp nhất tốc độ 4 nguồn `VelocityEstimator` loại ngoại lai $\pm 20\%$, bù áp `compute_compensated_duty`, kiểm tra trôi `evaluate_h0_drift`) + `tests/test_calib_math.cpp` xanh bằng `static_assert`.
  - Bổ sung `i2cBusRecover` (9 xung SCL + STOP) trong `main.cpp` để chống treo bus I²C khi reset không ngắt nguồn cảm biến, tích hợp engine `USE_SORTER_ENGINE=1`, và thêm Serial CLI (`c` đo C1 cập nhật $H_0$, `p` tạm dừng/chạy, `1`/`2` test solenoid khi dừng, `s` xem thống kê). Build thành công `build/BangTaiPhanLoai.uf2` (95,744 bytes).

**Điểm dừng & Bước tiếp theo:**
- Nạp `build/BangTaiPhanLoai.uf2` xuống Pico 2 W (`COM6`), bấm `c` trên Serial Monitor khi băng trống để đo C1 ($H_0, \sigma_0$), rồi thả phôi mẫu **S / M / L** để chạy **M4 (Chặng 1 & 2)** và chốt $T_{SM}, T_{ML}, t_d$.

## Đợt M0 + M1 + M2 — 2026-11-25

- [x] **M0 (`.gitignore`):** Mở theo dõi toàn bộ `docs/` (spec, decisions, glossary, assets, biên bản verify, progress), chặn `.omc/`, `__pycache__/`, `*.pyc`, `dashboard.zip`, `docs/*.docx`.
- [x] **M1 (chạy test C++ vượt chặn WDAC EXE):** Chuyển toàn bộ `sorter_core.h` và bộ kiểm thử `tests/test_sorter_core.cpp` sang `constexpr` + `static_assert`, xác minh thực thi 100% lúc biên dịch bằng `g++ -std=c++17 -Wall -Wextra -Werror -fsyntax-only tests/test_sorter_core.cpp` (vẫn giữ `main()` + `std::nextafter` cho Linux/WSL/CI).
- [x] **M2 (`SORT-AC02`, `SORT-R02..R05`):** Bổ sung vào `sorter_core.h` (thuần C++17, 0 cấp phát động): `calibrate_height`, `median_of` (lọc nhiễu 1 điểm), và `ItemDetector` (neo `t0_ms`, bỏ mẫu mép đầu, trung vị $N$ mẫu, phân loại & định tuyến `D-11`, hysteresis `WaitClear` không mở khóa bởi mẫu lỗi `D-03`, chống nhiễu glitch).
- [x] **HW-A (phần cứng đang cắm ở `COM6`):** Hai cảm biến VL53L1X đã đọc ổn định trên `COM6` (`TRAM1: d= 80..84mm`, `TRAM2: d= 81..85mm`); `monitor.bat` tự chọn `COM6`, Serial Monitor đã cài vào Antigravity IDE, dev broker (`1883`) và dashboard (`8000`) đã chạy.

---

## Đợt 2026-11-25 & 2026-11-23 — Spec Markdown & Wi‑Fi Dashboard

- [x] Chuyển `.docx` → [`phan-tich-thiet-ke-he-thong.md`](phan-tich-thiet-ke-he-thong.md) + 7 ảnh; lập [`glossary.md`](glossary.md), `specs/00..05`, [`bien-ban-ban-giao-verify.md`](bien-ban-ban-giao-verify.md).
- [x] Dashboard dùng Windows WLAN API thật cho simulator (`windows` vs `device`); 14 test Python + `node --check` đạt.
