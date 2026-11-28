# BangTaiPhanLoai

Bản firmware C++ (`main.cpp`) dùng Pico 2 W, hai ToF VL53L1X, hai servo đẩy
và hai LM393 xác nhận phôi đến vị trí đẩy. Trạm 1 đẩy L, trạm 2 đẩy S;
M đi thẳng. Bản MicroPython (`main_micropython.py`) dùng VL53L0X và sơ đồ
chân riêng; không dùng sơ đồ dưới đây cho bản đó.

## Sơ đồ chân firmware C++

| Pico 2 W | Thiết bị | Tín hiệu |
| --- | --- | --- |
| GP0 (chân 1) | ToF trạm 1 | SDA, I2C0 |
| GP1 (chân 2) | ToF trạm 1 | SCL, I2C0 |
| GP6 (chân 9) | ToF trạm 2 | SDA, I2C1 |
| GP7 (chân 10) | ToF trạm 2 | SCL, I2C1 |
| GP10 (chân 14) | Servo trạm 1 | Tín hiệu PWM 50 Hz |
| GP11 (chân 15) | Servo trạm 2 | Tín hiệu PWM 50 Hz; tháo dây L298N IN3 cũ |
| GP12 (chân 16) | LM393 trạm 1 | DO |
| GP13 (chân 17) | LM393 trạm 2 | DO |
| 3V3(OUT) (chân 36) | Hai ToF, LM393 nếu tương thích 3,3 V | VCC |
| GND (ví dụ chân 13/18) | Tất cả cảm biến và nguồn servo | Mass chung |

Hai servo dùng nguồn 5 V ngoài đủ dòng, **không** lấy điện từ GP10/GP11 hoặc
3V3 của Pico. Nối GND nguồn servo với GND Pico. Nếu LM393 cấp 5 V và DO có
thể lên 5 V, phải hạ mức DO xuống 3,3 V trước GP12/GP13; không đưa 5 V vào
GPIO Pico. Chân LPN/XSHUT của ToF giữ HIGH ở 3,3 V như cấu hình hiện tại.

ToF thu trọn lượt phôi, bỏ 20% mẫu đầu/cuối theo thứ tự thời gian rồi lấy
trung vị phần giữa. FIFO giữ chỗ từ lúc bắt đầu đo; mỗi lượt LM393 tiêu thụ
một vé, chỉ đẩy khi vé đã chốt đúng cỡ. Xem [thuật toán và giới hạn vận hành](docs/thuat-toan-phan-loai.md).
Mặc định servo HOME=1500 µs, PUSH=2500 µs, giữ PUSH 220 ms,
hồi 150 ms. Hiệu chỉnh riêng `SERVO1_HOME_US`/`SERVO1_PUSH_US` và
`SERVO2_HOME_US`/`SERVO2_PUSH_US` trong `main.cpp` khi chưa gắn tay đẩy;
đảm bảo cơ cấu không chạm điểm chặn. Nếu LM393 tác động mức HIGH, đổi
`LM393_ACTIVE_LOW` thành 0. Thời gian đẩy chỉnh trên dashboard 50–600 ms;
độ trễ gạt luôn 0 vì LM393 xác nhận vị trí. Khi dừng hoặc mất ToF, servo
được ra lệnh về HOME; firmware không có cảm biến xác nhận servo đã hồi.

## Build và nạp

Build bằng task **Compile Project** của VS Code với Raspberry Pi Pico SDK.
Firmware được tạo tại `build/BangTaiPhanLoai.uf2`; bản sao ở thư mục gốc
là `BangTaiPhanLoai.uf2`. Giữ BOOTSEL khi cắm Pico rồi chép firmware mới
vào ổ BOOTSEL để áp dụng cấu hình.

Thử riêng từng servo khi tháo tay đẩy, rồi thử phôi S, M, L trên băng tải để
chỉnh ngưỡng chiều cao và thời gian đẩy. Test logic gate trên máy tính:
`g++ -std=c++17 -Wall -Wextra -Werror -fsyntax-only tests/test_station1_servo_trial.cpp`.
