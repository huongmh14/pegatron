# Dashboard IoT PEGATRON

## Console USB và điều khiển máy thật

Chạy `run-dashboard.bat` để mở dashboard máy thật, broker và chương trình thu log USB (mặc định COM6; sửa cổng trong file nếu khác). Console web hiển thị 200 dòng gần nhất; các dòng nhận khi dashboard và broker đang kết nối được lưu trong `dashboard/data/pico-01.sqlite3`. Không lưu được những dòng phát ra khi USB, broker hoặc dashboard ngắt kết nối. Đóng cửa sổ USB Console trước khi dùng Serial Monitor hoặc nạp firmware. Điều khiển web dùng Wi-Fi/MQTT của Pico, không dùng đường USB log; cấu hình Wi-Fi và broker theo `docs/dashboard.md`. Máy thật cần firmware mới và cấu hình `RELAY_ACTIVE_LEVEL` theo kiểm tra thực tế. Nút E-STOP vật lý vẫn phải cắt nguồn động lực độc lập.

Dashboard tiếng Việt chạy trên laptop. Dùng MQTT để nhận dữ liệu hai trạm, FastAPI để phục vụ giao diện và SQLite để lưu lịch sử. Mặc định kết nối thiết bị giả lập. Mỗi ID thiết bị có một tệp dữ liệu riêng.

## Chạy thử trên Windows

Cài Python 3.12. Chỉ cần làm bước cài đặt này một lần, từ thư mục gốc dự án:

```powershell
cd dashboard
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements-test.txt
```

Sau đó, nhấp đúp `run-dashboard.bat` ở thư mục gốc để mở dashboard máy thật trong trình duyệt tại [dashboard localhost](http://127.0.0.1:8000). Giữ các cửa sổ broker, dashboard và USB Console mở trong lúc sử dụng; nhấn Ctrl+C trong từng cửa sổ để dừng. Chạy `run-dashboard.bat --sim` từ terminal để dùng simulator. Nếu dashboard đã chạy, nhấp đúp sẽ mở lại phiên hiện tại, kể cả khi phiên đó là giả lập.

Hoặc mở ba cửa sổ PowerShell trong thư mục `dashboard` và chạy mỗi lệnh ở một cửa sổ:

```powershell
.\.venv\Scripts\python.exe broker/dev_broker.py
.\.venv\Scripts\python.exe -m uvicorn app:app --host 127.0.0.1 --port 8000
.\.venv\Scripts\python.exe simulator.py --interactive
```

Kiểm tra nhãn **GIẢ LẬP** rồi bắt đầu một buổi chạy. Bấm **Bật phân loại**, xem số S/M/L tăng, sau đó **Tạm dừng phân loại** và tải CSV.

Nút **Wi‑Fi** trong chế độ giả lập đọc mạng thật từ card Wi‑Fi của máy Windows chạy dashboard, tối đa 16 mạng mạnh nhất, gồm cả 2,4 và 5 GHz. Hộp thoại ghi rõ **Wi‑Fi của máy tính** và chỉ cho xem; không mô phỏng thông báo kết nối thành công. RSSI được ước tính từ chất lượng tín hiệu Windows. Khi Windows từ chối quyền đọc, kiểm tra quyền Vị trí trong Cài đặt Windows rồi quét lại; không dùng mạng mẫu để thay kết quả lỗi.

Khi `PEGATRON_DEVICE_ID=pico-01`, hộp thoại **Wi‑Fi của thiết bị** gửi yêu cầu quét/kết nối qua MQTT tới Pico như phần cấu hình máy thật bên dưới. Kết quả lấy từ Pico, chỉ gồm mạng 2,4 GHz mà bo nhìn thấy, nên có thể khác danh sách Windows.

Trong cửa sổ giả lập, nhập `m` rồi Enter để đổi giữa tạm dừng và bảo trì. Dùng `a` để cấp lại quyền, `p` để tạm dừng tại chỗ, `e` để mô phỏng E‑STOP, `r` để xóa lỗi và `h` để bật/tắt heartbeat. Các phím này chỉ có tác dụng trong giả lập.

Thử lại với `--scenario sss`, `mmm`, `lll`, `missing-tray`, `home-fault`, `heartbeat-loss`, `slow-ack`, `lost-ack`, `maintenance` hoặc `noisy-calibration`. Ví dụ `home-fault` bắt đầu ở chế độ bảo trì và lần thử gạt sẽ báo quá hạn HOME. `heartbeat-loss` ngừng phát telemetry sau 12 giây.

Chạy kiểm thử:

```powershell
.\.venv\Scripts\python.exe -m unittest discover -s tests -v
```

## Kết nối MQTT cho máy thật

Broker `dev_broker.py` chỉ lắng nghe loopback và dành cho thử nghiệm. Dùng Eclipse Mosquitto cho mạng demo. Tạo tài khoản từ thư mục dự án:

```powershell
mosquitto_passwd -c broker/passwords dashboard
mosquitto_passwd broker/passwords simulator
mosquitto_passwd broker/passwords pico-01
mosquitto -c broker/mosquitto.conf -v
```

Dùng `-c` chỉ khi tạo tệp mật khẩu lần đầu. Để Pico vào được broker, sửa địa chỉ listener từ `127.0.0.1` sang IPv4 của laptop trên Wi‑Fi demo 2,4 GHz. Giới hạn TCP 1883 trong Windows Firewall cho subnet demo. MQTT bản này dùng TCP trong LAN riêng. Trang dashboard luôn bind `127.0.0.1`.

Cấu hình môi trường trong cửa sổ FastAPI:

```powershell
$env:PEGATRON_DEVICE_ID = 'pico-01'
$env:PEGATRON_MQTT_HOST = '<IPv4 của laptop trên Wi-Fi demo>'
$env:PEGATRON_MQTT_USER = 'dashboard'
$env:PEGATRON_MQTT_PASSWORD = '<mật khẩu dashboard>'
.\.venv\Scripts\python.exe -m uvicorn app:app --host 127.0.0.1 --port 8000
```

Nếu chạy giả lập qua Mosquitto, dùng ID `sorter-sim` cho backend, đặt `PEGATRON_SIM_USER=simulator` và `PEGATRON_SIM_PASSWORD` trong cửa sổ giả lập. Hai chương trình đọc `PEGATRON_MQTT_PORT` nếu dùng cổng khác 1883. Có thể đặt `PEGATRON_DB` để chọn tệp SQLite riêng cho một thiết bị.

## Tích hợp firmware

Xem [hướng dẫn firmware và hợp đồng MQTT](firmware/README.md). Bản vá dành cho firmware C++/Pico SDK của repo BangTaiPhanLoai. Máy thật hiện chỉ giám sát. Các thao tác trên dashboard hoạt động trong giả lập và quyền phát lệnh tới Pico thật được khóa ở cả backend lẫn ACL mặc định.

