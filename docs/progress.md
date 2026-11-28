# Tiến độ BangTaiPhanLoai — 2026-11-27

## Nhật ký build và nạp — 2026-11-28

- Người dùng xác nhận nối IN của module relay xuống GND làm relay hút. Firmware đặt `RELAY_ACTIVE_LEVEL=0`; tiếp điểm NC của module điều khiển cuộn MY4. E-STOP có tiếp điểm NC độc lập cắt cuộn MY4 theo xác nhận của người dùng.
- Object `main.cpp` biên dịch thành công. Build target `BangTaiPhanLoai` dừng mã 4551 vì Windows Device Guard chặn `C:\Users\huong\.pico-sdk\picotool\2.3.1\picotool\picotool.exe`; thử lại sau khi người dùng đồng ý vẫn bị chặn. Chưa có UF2 mới và chưa nạp thay đổi lên Pico; UF2 trong `build` ngày 26/11 là bản cũ.
- Pico xuất hiện ở COM6, không thấy ổ BOOTSEL. Dashboard kết nối broker nhưng báo Pico offline; log USB gần nhất ghi thử GP10/GP11 servo, chưa xác nhận firmware phân loại đang chạy.
- Cấu hình firmware được sinh ra có SSID Wi-Fi, mật khẩu Wi-Fi, IP broker và mật khẩu MQTT đều rỗng; không ghi thông tin bí mật vào nhật ký.
- Tiếp theo: nhờ quản trị viên cho phép picotool, cấu hình mạng tại máy theo `dashboard/firmware/README.md`, build lại; ngắt nguồn động lực trước khi vào BOOTSEL và nạp. Khi ngắt tải, xác minh mức 3,3 V GP26 làm relay nhả và kiểm tra START/STOP/E-STOP, cả khi bật/mất nguồn, trước khi vận hành.

## Đo liên tục và phân loại L/S/M (D-16..D-18)

- [x] ToF đo trọn lượt; cắt đầu/cuối theo thời gian 20/60/20 rồi trung vị, giữ vị trí mẫu lỗi, kiểm tra thiếu mẫu/timeout/tràn bộ đệm.
- [x] Trạm 1 đẩy L, trạm 2 đẩy S, M đi thẳng; ngoài khoảng/khe đệm không được đẩy như S.
- [x] FIFO giữ vé từ đầu phiên; chốt đúng vé, bỏ kết quả muộn, tiêu thụ cả vé 0/pending/busy. Quá hạn/tràn/không có vé/mất ToF khóa đẩy đến khi dừng và dọn băng tải.
- [x] Tích hợp firmware chính và bản thử trạm 1; sửa ánh xạ bộ đếm S/M, chỉ nhận đổi ngưỡng khi IDLE. Có tài liệu [thuật toán](thuat-toan-phan-loai.md), README và kiểm tra hồi quy.
- [x] Kiểm tra C++17 static_assert bằng g++ native và ARM với `-Wall -Wextra -Werror -fsyntax-only`: sorter_core, station1_servo_trial, sorter_fsm. Native cần thêm `C:/msys64/ucrt64/bin` vào PATH để compiler con tìm được DLL.
- [x] Pico SDK biên dịch hai target và liên kết ra ELF.
- [x] Tránh sao chép mảng 256 double lên stack 2 KB: median tại chỗ, detector thử static, cập nhật cấu hình không dựng detector tạm; có kiểm tra trung vị 256 mẫu.
- [~] Toàn bộ lệnh build chưa thành công: `picotool.exe` bị Windows Device Guard chặn (4551) ở bước hậu xử lý. Chưa có UF2 mới được xác nhận; không dùng UF2 cũ làm bản thuật toán mới.
- [~] Chưa nạp firmware hoặc chạy cơ cấu trong đợt sửa này. Servo trạm 1 không quay ở lần thử trước vẫn chưa rõ nguyên nhân phần cứng.

## Điểm dừng và việc tiếp theo

Source đã sửa, chưa commit. Hoàn tất đóng gói UF2 trong môi trường được phép chạy Pico SDK/picotool. Sau đó kiểm tra từng servo/LM393, đo H0 trên băng trống, hiệu chỉnh ngưỡng và nhịp đọc ToF thực tế, chạy mẫu L/S/M cùng các ca đến sớm và phôi sát nhau. Driver hiện bỏ qua `period_ms` của `startContinuous`, không giả định đúng 15 ms/mẫu. Tối thiểu 10 mẫu hợp lệ và thời hạn vé 1.500 ms phải phù hợp tốc độ thực tế. FIFO không phát hiện chắc chắn trường hợp ToF bỏ sót/đếm đôi khi vẫn có vé chờ.

Lịch sử trước đợt này: [bản lưu](progress-archive/2026-11-27-before-continuous-sorter.md). Các tuyến L/M/S và trạng thái build trong lịch sử đã được thay thế bởi nội dung trên.
