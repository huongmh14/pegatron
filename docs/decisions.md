# Quyết định đã xác nhận

Nguồn: README.md, `main.cpp` (commit `e181c00`), `CMakeLists.txt`, `docs/conflicts-and-bugs.md`, git log. Thêm mục mới theo dạng `D-xx`; không sửa mục cũ, chỉ ghi mục thay thế.

| ID | Quyết định | Nguồn |
| --- | --- | --- |
| D-01 | Trạm 1 đẩy phôi M (đo 65–75 mm), trạm 2 đẩy phôi L (đo 50–60 mm); S nằm ngoài hai khoảng nên đi qua cả hai trạm. | README, commit `c31887c`, `main.cpp:110-111` |
| D-02 | Mặt băng tải không phôi đo 85–90 mm; không kích solenoid. Số đo là khoảng cách cảm biến tới mặt phôi, không phải chiều cao phôi. | README |
| D-03 | Cần 2 mẫu liên tiếp trong khoảng nhận mới kích, không trễ di chuyển. Sau khi kích, phải thấy lại mặt băng tải (2 mẫu hợp lệ liên tiếp từ 80 mm trở lên) mới nhận phôi tiếp; mẫu lỗi không mở khóa. Hai trạm độc lập. | README |
| D-04 | Xung kích 500 ms rồi nhả (`MODE_PULSE`). | README, `main.cpp:110` |
| D-05 | Solenoid qua L298N: GP10→IN1 (trạm 1), GP11→IN3 (trạm 2); IN2/IN4 nối GND; GP HIGH = cấp điện. | README, `main.cpp:40-41` |
| D-06 | Board `pico2_w`. Bản C++ (`main.cpp`) dùng VL53L1X; bản MicroPython (`main_micropython.py`) dùng VL53L0X. Chọn bản đúng với cảm biến thực tế. | README, `CMakeLists.txt:26` |
| D-07 | Chân I2C của bản C++: ToF1 SDA GP0/SCL GP1, ToF2 SDA GP6/SCL GP7. `test.cpp` và `main_micropython.py` dùng ToF2 GP2/GP3 nên khác dây (xem DBG-02). | `main.cpp:33-38`, conflicts DBG-02 |
| D-08 | Dashboard nằm trong repo tại `dashboard/` (đổi mặc định `PEGATRON_IOT_DIR` trong `CMakeLists.txt`). | `CMakeLists.txt:38`, conflicts INT-01 |
| D-09 | Firmware chưa có đầu vào E-STOP nên build với `PEGATRON_ESTOP_ABSENT=1`. Người vận hành chấp nhận rủi ro này; chi tiết ở `docs/dashboard.md`. | conflicts INT-01 |
| D-10 | Trong chế độ mô phỏng, Wi‑Fi hiển thị kết quả Windows thật và chỉ xem. Cấu hình Wi‑Fi Pico dùng ID máy thật qua MQTT; không dùng SSID mẫu thay kết quả quét lỗi. | sửa Wi‑Fi 2026-11-23, conflicts DBG-04 |
| D-11 | Chốt phân công trạm đẩy cho bộ đặc tả triển khai mới giữ đúng theo code hiện trạng (`D-01`, `main.cpp:13-15`): Trạm 1 đẩy `M`, Trạm 2 đẩy `L`, `S` chạy thẳng về cuối băng (ghi đè `.docx §1.1`, `FR-04`, `FR-05`, `FR-07`). Các thông số phần cứng/thể lệ còn lại giữ mở để điền từ kết quả đo thực nghiệm theo kiến trúc tách module. | Người dùng chốt 2026-11-25 |
| D-12 | Dừng xử lý phần tính/ước lượng tốc độ băng tải (không dùng `VelocityEstimator` / `compute_v12_mm_s` trong runtime); giữ gạt trực tiếp theo `travel_delay_ms` cố định/nhập tay từ web (mặc định `0 ms`). | Người dùng chốt 2026-11-25 |
| D-13 | Đổi thứ tự phân loại từ cao xuống thấp (`L → M → S`): Trạm 1 gạt `L`, Trạm 2 gạt `M`, `S` đi thẳng cuối băng không cần đẩy (thay thế `D-11`). | Người dùng chốt 2026-11-25 |
| D-14 | Tách cấu hình hai trạm thành hai khoảng chiều cao vật độc lập (`L=[l_min_mm, l_max_mm]`, `M=[m_min_mm, m_max_mm]`) tính theo `H0` riêng từng cảm biến, không ép hai ô dính nhau hay ràng buộc `H0` trên Web UI, cho phép tạo khe đệm chống nhiễu ToF (vật rơi vào khe đệm/ngoài khoảng trả về `S` đi thẳng). | Người dùng chốt 2026-11-25 |
| D-15 | Thay cơ cấu đẩy trạm 2 bằng servo GP11 + LM393 DO GP13 theo luồng ToF → LM393 → servo của trạm 1; hai servo dùng chung PWM slice nhưng có kênh và gate riêng. Giữ phân loại L/M/S và cách đo ToF từng trạm; độ trễ gạt bằng 0. Thay thế phần đấu solenoid/L298N của D-05 trong firmware C++. | Người dùng yêu cầu 2026-11-26; `main.cpp`, README |

| D-16 | Trạm 1 đẩy L, trạm 2 đẩy S, M đi thẳng; thay thế phân tuyến D-13/D-15. Mỗi trạm đo lại các phôi thực sự đi tới cảm biến của nó. Phép đo ngoài khoảng/khe đệm là Invalid và không đẩy, thay thế quy tắc coi tất cả ngoài L/M là S của D-14. | Người dùng chốt trạm 2 phân loại S; yêu cầu viết lại thuật toán 2026-11-27 |
| D-17 | Thu trọn phiên ToF; bỏ floor(N/5) mẫu mỗi đầu theo thứ tự thu rồi lấy trung vị mẫu hợp lệ ở giữa. Mẫu lỗi giữ vị trí thời gian; thiếu mẫu/timeout/tràn bộ đệm không đẩy. Giữ vé FIFO từ đầu phiên, chốt đúng vé; LM393 đến sớm hoặc servo bận vẫn tiêu thụ vé, không đẩy muộn. | `sorter_core.h`, `station1_servo_trial_logic.h`, kiểm tra C++17 2026-11-27 |
| D-18 | Mất ToF, FIFO đầy/quá hạn hoặc LM393 không có vé sẽ khóa đẩy trạm đến khi dừng, dọn băng tải và khởi động lại. Thay đổi khoảng chiều cao chỉ nhận khi IDLE. Ngưỡng, số mẫu và thời gian hiện tại cần hiệu chỉnh trên máy. | Tích hợp `main.cpp` 2026-11-27; `docs/thuat-toan-phan-loai.md` |

## Chưa xác nhận
- Chạy trên Pico thật với dashboard: mới có build firmware (Pico SDK 2.3.1) và 10 test backend đạt.
- Độ chính xác và loại chip cảm biến thực tế (xem `docs/debug-cam-bien.md`).
