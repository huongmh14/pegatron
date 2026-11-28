# Lỗi và khác biệt cấu hình đã xác nhận

## ALG-01 — Tuyến trạm 2 và kết quả ngoài khoảng (2026-11-27)

- Base SHA: `020163d`; bản sửa chưa commit, thay thế tuyến trong HW-03/D-13.
- Nguyên nhân: code cũ vẫn đẩy M ở trạm 2; mọi chiều cao ngoài L/M được gán S. Khi đổi trạm 2 sang S, phép đo quá cao/khe đệm cũng có thể kích đẩy sai.
- Ảnh hưởng: phân loại và bộ đếm khay hai trạm trong firmware C++; không sửa bản MicroPython.
- Sửa: trạm 1 L, trạm 2 S, M đi thẳng; S có cận dưới riêng, ngoài khoảng là Invalid. L ưu tiên tại biên chung/chồng lấn như hợp đồng cũ.
- Gate 0: native và ARM `-std=c++17 -Wall -Wextra -Werror -fsyntax-only tests/test_sorter_core.cpp`; các static_assert kiểm tra tuyến, biên và khe đệm đạt.

## ALG-02 — Kết quả đến muộn và phôi đến khi servo bận (2026-11-27)

- Base SHA: `020163d`; bản sửa chưa commit.
- Nguyên nhân: chỉ enqueue sau khi đo xong; LM393 tới trước không tiêu thụ được kết quả tương lai. Gate cũ trả sớm khi servo bận nên không hạch toán đầy đủ phôi đến trong nhịp đẩy/hồi.
- Ảnh hưởng: quyết định có thể lệch sang phôi tiếp theo ở cả hai trạm.
- Sửa: giữ vé từ đầu ToF, complete theo mã vé; mọi lượt LM393 tiêu thụ đúng một vé kể cả pending/0/busy. Đầy/quá hạn/không có vé khóa đẩy; phép đo lỗi giữ placeholder 0.
- Gate 0: native và ARM compile-time `tests/test_station1_servo_trial.cpp`; ca pending tới sớm, hoàn thành muộn, servo bận, FIFO tràn/quá hạn và thời gian tràn vòng đạt. FIFO vẫn giả định ToF/LM393 đếm đúng mỗi phôi; chưa có encoder chứng minh danh tính.

## ALG-03 — Mất ToF tự xóa khóa đồng bộ (2026-11-27)

- Base SHA: `020163d`; phát hiện khi tích hợp gate mới, sửa trước khi nạp.
- Nguyên nhân: nhánh mất ToF gọi reset; `stationRelease` tiếp tục reset gate, có thể mở lại khóa khi cảm biến hồi phục dù phôi còn trên băng.
- Ảnh hưởng: hàng đợi hai servo không còn chứng minh đúng thứ tự sau mất cảm biến.
- Sửa: mất ToF giữ khóa và HOME; chỉ thao tác dừng/reset chủ động mới mở khóa. Phải dọn băng trước khi khởi động lại.
- Gate 0: `tof_loss_aborts_and_clears_queue` kiểm tra mất ToF, phục hồi tín hiệu vẫn khóa, từ chối enqueue/manual, rồi disable mới phục hồi; native/ARM đạt.

## ALG-04 — Bộ đệm đo lớn trên stack Pico (2026-11-27)

- Base SHA: `020163d`; phát hiện và sửa trong đợt mở rộng phiên đo, chưa nạp.
- Nguyên nhân: tăng mảng lên 256 double làm bản sao trong median chiếm 2.048 byte; detector local của bản thử và phép gán từ detector tạm cũng tạo frame lớn. Link map hiện chỉ dành 2.048 byte stack chính (`__StackTop - __StackBottom`).
- Ảnh hưởng: có nguy cơ ghi tràn stack khi khởi tạo, hiệu chuẩn hoặc chốt phiên trên Pico; kiểm tra logic host không phát hiện lỗi tài nguyên này.
- Sửa: median sắp xếp trực tiếp mảng đã cắt; detector thử dùng static; firmware cập nhật cấu hình tại chỗ, không dựng detector tạm.
- Gate 0: `full_buffer_median_sorts_in_place` kiểm tra 256 mẫu đảo thứ tự bằng native/ARM; kiểm tra caller `median_of`/`ItemDetector` bằng rg, build hai firmware. Insertion sort giới hạn 256, chưa có benchmark thời gian thực.

## HW-04 — Chu kỳ ToF và servo thực chưa được xác nhận (2026-11-27)

- Base SHA: `020163d`; driver/phần cứng chưa sửa trong đợt này.
- Nguyên nhân xác nhận qua source: `vl53l1x.cpp::startContinuous` bỏ qua `period_ms`; `setDistanceMode` trong header là no-op. Chú thích 15 ms/mẫu trước đây không chứng minh được nhịp lấy mẫu thực.
- Ảnh hưởng: phiên cần 10 mẫu có thể thiếu mẫu khi băng chạy nhanh; ngưỡng phát hiện lớn hơn 6 mm có thể bỏ qua S cao 5–6 mm dù nằm trong khoảng phân loại.
- Triệu chứng phần cứng riêng do người dùng báo: servo trạm 1 không quay khi thử PWM trực tiếp, nguồn tại giắc khoảng 4,8–5,2 V. Chưa xác định nguyên nhân hoặc xác nhận khắc phục; không suy từ số đo tĩnh rằng đủ dòng khi quay.
- Gate 0 tiếp theo: đo nhịp dataReady/N từng phôi, kiểm tra PWM và điện áp dưới tải, hiệu chỉnh H0/ngưỡng/hành trình trên máy. Đợt này không chạy cơ cấu.

## ENV-03 — Compiler native và bước đóng gói UF2 (2026-11-27)

- Base SHA: `020163d`; không thay đổi chính sách Windows, không commit artifact.
- Nguyên nhân: g++ native trả 1 không có chẩn đoán khi thiếu thư mục UCRT trong PATH; thêm `C:/msys64/ucrt64/bin` cho tiến trình kiểm tra thì chạy đạt. Tái hiện ENV-02: Device Guard chặn `picotool.exe` với mã 4551 sau liên kết ELF.
- Ảnh hưởng: không thể coi toàn bộ build là thành công hoặc coi UF2 sẵn có là bản mới. Hai target đã compile/link ELF; UF2 mới chưa xác nhận.
- Gate 0: native và ARM static_assert đạt; `cmake --build build --target BangTaiPhanLoai TestServoTram1 -j 2` dừng đúng tại hậu xử lý picotool. Cần môi trường cho phép chạy tool để đóng gói.
- Lỗi tài liệu trong đợt sửa: Python nhận here-string PowerShell qua encoding ASCII làm mất dấu trong progress; đã viết lại UTF-8 bằng apply_patch. Gate 0 kiểm tra progress có ký tự Unicode, không có dấu `?` thay thế, kích thước dưới 5 KB; bản lưu dùng copy bytes nên không bị ảnh hưởng.

## HW-03 — GP10/GP11 chung PWM slice và hàng đợi trạm 2 (2026-11-26)

- Base commit: `b48a6e8`; bản sửa đang ở working tree.
- Nguyên nhân: khởi tạo PWM riêng trên GP11 reset cả slice của GP10; nhánh `Missed` (L lọt đến trạm 2) không ghi quyết định đi qua vào FIFO, làm lệch phôi kế tiếp.
- Ảnh hưởng: servo trạm 1 có thể giật khi khởi động trạm 2; trạm 2 có thể đẩy nhầm M sau một phôi L lọt qua.
- Sửa: cấu hình PWM slice một lần, mỗi trạm giữ kênh/LM393/gate riêng, đưa `Missed` vào FIFO dưới dạng không đẩy.
- Gate 0: `arm-none-eabi-g++ -std=c++17 -Wall -Wextra -Werror -fsyntax-only tests/test_station1_servo_trial.cpp`; build target `BangTaiPhanLoai`, rồi kiểm tra vật lý từng servo/LM393.

## HW-01 — Hai mẫu cách nhau 2–3 cm chỉ gạt mẫu đầu (2026-11-25)

- Base commit: `f56f416`; đã sửa trong `sorter_fsm.h`, `main.cpp`.
- Nguyên nhân: (1) `SolenoidActuator::tick()` tắt xung cũ và bật ngay xung kế tiếp trong cùng millisecond nên `changed == false`, solenoid không nhả để hồi về; (2) `on_time_ms = 500 ms` + timing budget ToF `40 ms` + `CLEAR_COUNT = 2` không kịp hồi giữa hai mẫu sát nhau.
- Sửa: thêm `min_off_time_ms` (mặc định `120 ms`) và cơ chế tách pha nhả (`has_released_`) trong `SolenoidActuator`; hạ `on_time_ms = 220 ms`, `CLEAR_COUNT = 1`, ToF `15 ms` (~66 Hz), `DETECT_DELTA_MM = 6.0`, `CLEAR_DELTA_MM = 5.0`.
- Gate 0: `g++ -std=c++17 -Wall -Wextra -Werror -fsyntax-only tests/test_sorter_fsm.cpp` (`test_back_to_back_retract_gap`).

## HW-02 — `VL53L4CD` (`0xEBAA`) kẹt cờ `i2c_error` sau soft-reset (2026-11-25)

- Base commit: `f56f416`; đã sửa trong `vl53l1x.cpp`.
- Nguyên nhân: soft-reset (`writeReg(0x0000, 0x00)`) làm slave NACK hợp lệ, nhưng cờ `i2c_error = true` không được xóa trước khi đọc trạng thái boot nên `VL53L1X::init()` trả về `false` dù chip đã boot (`0x03`).
- Sửa: xóa `i2c_error` và thông tin chẩn đoán I2C sau soft-reset và sau khi xác nhận `booted == true`.

## ENV-02 — `picotool.exe` bị Windows Device Guard chặn (2026-11-25)

- Hiện tượng: bước `POST_BUILD` của Ninja gọi `picotool.exe` thất bại với mã `4551` (App Control / Device Guard).
- Xử lý: giữ nguyên chính sách máy; sau khi `BangTaiPhanLoai.elf` liên kết xong, xuất `.bin` qua `arm-none-eabi-objcopy -Obinary` và đóng gói `.uf2` RP2350-E10 bằng Python để nạp `BOOTSEL`.

## UX-01 — Thông số Web phụ thuộc nhau & lệ thuộc mốc `H0` (2026-11-25)

- Nguyên nhân: hai ô nhập ToF cho Trạm 1/2 dùng chung điểm ranh giới `T_ML` và suy ngược từ `H0` chung trên trình duyệt, gây lỗi khi hai cảm biến gắn lệch độ cao hoặc muốn để khe đệm sai số ToF.
- Sửa (`D-14`): chuyển sang cấu hình 2 khoảng chiều cao độc lập (`L=[l_min_mm, l_max_mm]`, `M=[m_min_mm, m_max_mm]`) dùng `H0` riêng từng cảm biến; `classify_ranges()` trả `S` cho khe đệm.
- Gate 0: `tests/test_sorter_core.cpp` + `dashboard/tests/test_serial_bridge.py` (`test_station_config_api_and_serial_format`).

## SPEC-01 — Ví dụ trung vị không khớp AC (2026-11-25)

- Base commit: `f56f416`; bản sửa chưa commit.
- Nguyên nhân: SPEC-SORT §9 ghi median=50.1/h=49.9 và không phân biệt mẫu nền 99 với mẫu mép 75; AC02 lại yêu cầu h=50.
- Ảnh hưởng: test bộ lọc có thể chọn sai cửa sổ mẫu. Sửa §9 thành nền 99, bỏ mép 75, median ba mẫu còn lại=50/h=50; không sửa bản chuyển đổi nguồn.
- Gate 0: đối chiếu SORT-AC02 và SPEC-SORT §9. Test lõi phân loại mới ở `tests/test_sorter_core.cpp` chưa bao phủ bộ lọc.

## SPEC-02 — Ví dụ JSON không parse được (2026-11-25)

- Base commit `f56f416`; chưa commit. Năm block JSON của SPEC-MQTT/CALIB chứa comment hoặc nhiều object nối tiếp, trái kết quả tự kiểm bàn giao.
- Sửa: đưa chú thích ra ngoài fence, tách từng bản tin; không đổi payload. Ảnh hưởng: ví dụ copy/paste để kiểm tra giao thức.
- Gate 0: lấy từng fence json trong `docs/specs/*.md`, parse bằng `ConvertFrom-Json -ErrorAction Stop`: 7/7 đạt sau sửa.

## ENV-01 — Host C++ test bị Application Control chặn (2026-11-25)

- Base commit: `f56f416`; chưa commit. GCC cần `C:\msys64\ucrt64\bin` trong PATH để compile/link.
- Compile/link đạt nhưng Windows Application Control từ chối chạy EXE trong TEMP. `$LASTEXITCODE` giữ 0 của compiler nên không được dùng làm bằng chứng runtime pass.
- Phạm vi: kiểm chứng host mới; không thay đổi chính sách máy. Gate: chạy test trên host được phép, xác nhận tiến trình thực sự khởi chạy và kết thúc 0. Hiện chỉ syntax/warnings và 5 test Python cũ đạt.

Đối chiếu ngày 2026-11-22, mã nguồn tại commit `e181c0066717eb72ece27c525e25bb6a32bf7b2c`. Các mục dưới đây là kết quả chẩn đoán, chưa sửa firmware.

## DBG-01 — Chạy MicroPython bằng Python Windows

- Nguyên nhân: chạy `main_micropython.py` bằng Python trên máy tính; `machine` là API của MicroPython trên bo. Bo đã trả log C++ qua COM6 trong phiên kiểm tra.
- Ảnh hưởng: `ModuleNotFoundError: No module named 'machine'`; lệnh Python không chạy trên bo đang dùng firmware C++.
- Kiểm tra: đối chiếu lệnh gọi Python và `main_micropython.py:17`; đọc COM6 thấy các dòng `TRAM1: d=... TRAM2: d=...` tương ứng `main.cpp`.
- Xử lý: dùng Serial Monitor để xem C++; chỉ dùng REPL khi bo đã nạp MicroPython và driver phù hợp cảm biến. Không cài gói `machine` trên Windows để chữa lỗi này.

## DBG-02 — Chân cảm biến thứ hai khác nhau giữa các bản

- Nguyên nhân: `main.cpp` dùng SDA GP6/SCL GP7; `test.cpp` và `main_micropython.py` dùng SDA GP2/SCL GP3.
- Ảnh hưởng: đổi bản firmware mà giữ nguyên dây có thể làm trạm 2 báo lỗi dù cảm biến vẫn hoạt động.
- Gate 0: `Select-String -Path main.cpp,test.cpp,main_micropython.py -Pattern 'TOF2_SDA_PIN|TOF2_SCL_PIN|i2c1 = I2C'`.
- Xử lý: đối chiếu dây và cấu hình trước khi nạp `TestToF`; chưa đổi chân hoặc nạp firmware trong đợt nghiên cứu này.

## DBG-03 — Cổng COM trong hướng dẫn chạy cũ không trùng thiết bị

- Nguyên nhân: `monitor.bat` cố định COM5; thiết bị USB được xác nhận là COM6 trong phiên này, COM3/COM4 là Bluetooth. Số COM có thể đổi khi cắm lại.
- Ảnh hưởng: không thấy log hoặc mở sai cổng. MicroPico từng kẹt ở “Closing port…”; rút/cắm lại USB đã cho phép đọc COM6.
- Gate 0: `[System.IO.Ports.SerialPort]::GetPortNames()` và `Get-ItemProperty 'HKLM:\HARDWARE\DEVICEMAP\SERIALCOMM'`.
- Xử lý: chọn cổng USB hiện tại, chỉ mở một Serial Monitor/MicroPico tại một thời điểm; xem `debug-cam-bien.md`.

## INT-01 — Tích hợp dashboard (2026-11-23)

- Đường dẫn: bản vá gốc mặc định `PEGATRON_IOT_DIR=../pegatron-iot`; trong repo này dashboard nằm ở `dashboard/`, đã đổi mặc định trong `CMakeLists.txt`.
- Lệnh điều khiển: backend khóa lệnh khi `estop` là `null`; firmware không có đầu vào E‑STOP nên thêm `PEGATRON_ESTOP_ABSENT=1` (rủi ro được người vận hành chấp nhận, xem docs/dashboard.md).
- Chưa thử trên Pico thật: chỉ có build firmware (Pico SDK 2.3.1) và 10 test backend đạt.

## DBG-04 — Danh sách Wi‑Fi mô phỏng bị hiểu là quét thật (2026-11-23)

- Nền mã: `73d363b`; bản sửa đang ở working tree, chưa commit.
- Nguyên nhân: launcher chọn `sorter-sim`; simulator trả ba SSID cố định nhưng hộp thoại ghi “Wi‑Fi của thiết bị”, không cho biết nguồn.
- Ảnh hưởng: tên mạng khác Windows; thao tác giả lập có thể bị hiểu là đã cấu hình Pico thật.
- Xử lý: khi mô phỏng, backend dùng Windows WLAN API để quét thật, trả nguồn `windows`, giao diện chỉ xem và backend từ chối kết nối giả. Thiết bị thật giữ luồng MQTT và nguồn `device`. Xóa danh sách cũ ngay khi quét lại; lỗi không dùng dữ liệu mẫu để thay thế.
- Lỗi phát hiện qua trình duyệt: lần quét Windows đầu tiên có thể chậm hơn giới hạn HTTP 15 giây, khiến giao diện báo lỗi dù backend lấy được mạng. Chuyển quét sang luồng nền, trả mã yêu cầu ngay và chờ kết quả tối đa 45 giây trên giao diện.
- Gate 0: `cd dashboard; .\.venv\Scripts\python.exe -m unittest discover -s tests -v` (14 test đạt); `node --check dashboard/static/wifi.js` từ gốc repo. Test bao gồm nguồn, offline simulator, chặn kết nối giả, lỗi quyền, giữ luồng MQTT máy thật, phản hồi ngay khi Windows quét chậm, gộp SSID, Unicode và giải phóng bộ nhớ native sau timeout.
- Kiểm tra thực tế: trình duyệt tại `http://127.0.0.1:8000` hiển thị năm mạng thật, quét lại thành công, xóa danh sách cũ và giữ chế độ chỉ xem. Chưa xác nhận quét/chuyển mạng trên Pico. Không suy ra phần cứng đã kết nối từ kết quả quét của Windows.
## DBG-05 — Mức kích relay HW-482 và tương thích 3,3 V (2026-11-28)

- Tiếp điểm NC module đang điều khiển cuộn MY4, nên mất nguồn module có thể cấp điện cho MY4. E-STOP NC riêng đã được người dùng xác nhận cắt cuộn MY4.
- Người dùng xác nhận nối IN xuống GND làm relay hút; firmware đặt `RELAY_ACTIVE_LEVEL=0`. Khả năng nhả relay ở mức 3,3 V và trạng thái IN khi thả nổi chưa được xác minh; không coi module là mạch dừng an toàn.
- Gate 0: object firmware biên dịch thành công; trước khi vận hành cần kiểm tra GP26 mức 3,3 V làm relay nhả khi ngắt tải, sau đó kiểm tra cả lúc bật/mất nguồn và các nút trên thiết bị thực tế. E-STOP NC độc lập cắt cuộn MY4.

## DBG-06 — Log USB và trạng thái E-STOP trên web (2026-11-28)

- Bản cũ chưa chuyển dòng console USB vào web; serial bridge cũ báo E-STOP không rõ và xác nhận lệnh sau khi ghi COM, không phải xác nhận từ Pico.
- Thêm chế độ chỉ chuyển log USB, dùng MQTT Wi-Fi trực tiếp cho điều khiển và E-STOP; launcher không còn bật tùy chọn bỏ qua E-STOP. Log chỉ được lưu khi các kết nối đang hoạt động; không dùng serial bridge cũ làm xác nhận an toàn.
- Gate 0: kiểm tra log SQLite và API, test serial bridge, biên dịch object firmware và kiểm tra cú pháp JavaScript. Chưa kiểm thử Pico thực tế.

## DBG-07 — Launcher BAT lỗi cú pháp khi mở dashboard (2026-11-28)

- Dấu ngoặc trong lệnh `echo` nằm trong khối điều kiện BAT gây lỗi cú pháp; thời gian chờ hai giây cũng không đảm bảo web sẵn sàng.
- Đã sửa câu báo, chờ API sẵn sàng và mở lại phiên đang chạy khi nhấp lần nữa. Gate 0: chạy BAT hai lần và kiểm tra API `pico-01` cùng kết nối broker. Chưa xác nhận Pico online.
