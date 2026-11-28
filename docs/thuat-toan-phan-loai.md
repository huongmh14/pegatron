# Thuật toán phân loại liên tục 20/60/20

Áp dụng cho firmware C++ Pico 2 W, hai ToF và hai LM393. Trạm 1 đẩy L,
trạm 2 đẩy S, M đi thẳng. Mỗi trạm đo và quản lý FIFO riêng; trạm 2 chỉ
nhận các phôi thực tế đi tới ToF của nó. Không áp dụng thay đổi này cho bản MicroPython.

## 1. Đo trọn lượt phôi

1. Khi đang trống, một mẫu ToF hợp lệ có `d < H0 - 6 mm` bắt đầu phiên.
   Nếu hệ thống đang RUNNING, giữ ngay một chỗ FIFO với mã vé riêng, trạng thái
   CHƯA CHỐT. `H0` là khoảng cách mặt băng trống của từng cảm biến.
2. Thu liên tục các lần đọc mới theo `dataReady`, giữ nguyên thứ tự thời gian.
   Mẫu lỗi vẫn chiếm vị trí trong chuỗi để không làm dịch phần cắt đầu/cuối,
   nhưng không tham gia trung vị.
3. Hai mẫu hợp lệ liên tiếp có `d >= H0 - 5 mm` xác nhận phôi đã rời vùng đo.
   Loại hai mẫu xác nhận mặt băng này khỏi phiên. Mẫu mặt băng đơn lẻ rồi lại
   thấy phôi chưa kết thúc phiên.
4. Với N vị trí đã thu, bỏ `floor(N/5)` vị trí đầu và cuối, sau đó mới lấy
   các số đo hợp lệ ở phần giữa để tính trung vị. Không sắp xếp trước khi cắt.
   Ví dụ N=10: bỏ mẫu 1–2 và 9–10, tính trung vị mẫu 3–8. Khi N không chia hết
   cho 5, tỷ lệ giữ lại xấp xỉ 60% do làm tròn xuống số mẫu bỏ.
5. Tính chiều cao `h = ((H0 - d_median) - c) / k`, rồi phân loại.
   Nếu số mẫu giữa chẵn, trung vị là trung bình của hai giá trị giữa sau sắp xếp.
6. Chốt quyết định vào đúng mã vé đã giữ. Nếu vé đã bị LM393 tiêu thụ,
   bị hủy hoặc mất đồng bộ, bỏ kết quả đến muộn; không tạo vé thay thế.

Giới hạn mặc định: tối đa 256 vị trí mẫu/phiên, ít nhất 10 mẫu hợp lệ trong
toàn phiên và ít nhất 6 mẫu hợp lệ ở giữa. Phiên quá 2.000 ms hoặc đầy bộ đệm
trả kết quả lỗi một lần rồi chờ hai mẫu mặt băng hợp lệ mới nhận phiên mới.
Phiên thiếu mẫu hoặc chiều cao không hợp lệ cũng chốt quyết định không đẩy.
FIFO có thể khóa vì hết hạn 1.500 ms trước khi phiên chạm timeout 2.000 ms.

## 2. Cỡ phôi và quyết định 0/1

Các ngưỡng khởi tạo trong `main.cpp`:

- S: `5 <= h < 18 mm`; trạm 1 ghi 0, trạm 2 ghi 1.
- M: `18 <= h < 28 mm`; cả hai trạm ghi 0.
- L: `28 <= h <= 100 mm`; trạm 1 ghi 1, trạm 2 ghi 0.
- Phép đo lỗi, thấp hơn S, cao hơn L hoặc trong khe đệm cấu hình: ghi 0.

Code kiểm tra khoảng L trước M, vì vậy biên chung 28 mm thuộc L; nếu người
vận hành đặt hai khoảng chồng nhau thì L được ưu tiên. L lọt trạm 1 được
ghi nhận là Missed ở trạm 2 và cho qua, không đẩy vào khay S.

Ngưỡng S tối thiểu 5 mm là ngưỡng phân loại; ngưỡng bắt đầu phát hiện hiện là
độ giảm khoảng cách lớn hơn 6 mm. Do đó phôi thấp khoảng 5–6 mm có thể chưa
tạo phiên. Cần hiệu chỉnh ngưỡng phát hiện theo phôi nhỏ nhất và nhiễu thực tế.
Các ô L/M trên dashboard vẫn cấu hình khoảng chiều cao; khoảng M giờ là
nhóm đi thẳng, S được suy ra từ 5 mm tới cận dưới M. Chỉ nhận đổi cấu hình khi IDLE.

## 3. LM393 và FIFO

Mỗi vé có mã nhận dạng, thời điểm đầu phiên ToF, trạng thái đã chốt/chưa chốt
và bit quyết định 0/1. Chỉ bit 0/1 là chưa đủ vì còn trường hợp đang đo.
FIFO chứa tối đa 8 vé, kể cả phôi cho qua và phép đo lỗi.

LM393 mặc định active LOW. Khi tín hiệu có phôi liên tục ít nhất 10 ms,
lấy và xóa đúng một vé cũ nhất:

- Đã chốt 1 và servo rảnh: ra lệnh PUSH ngay trong lượt xử lý đó.
- Đã chốt 0: cho qua.
- Chưa chốt: bỏ phôi này, không chờ để đẩy muộn.
- Đã chốt 1 nhưng servo đang đẩy hoặc hồi: bỏ phôi này, không giữ lại cho phôi sau.

Sau mỗi lượt, phải thấy LM393 trống liên tục 30 ms mới nhận lượt tiếp theo.
Vòng lặp vẫn đọc cảm biến và xử lý FIFO khi servo đang chuyển động.

Vé quá 1.500 ms tính từ đầu phiên ToF, tràn FIFO, LM393 không có vé tương ứng
hoặc mất ToF sẽ đưa servo về HOME, xóa hàng đợi và khóa đẩy tự động của trạm.
ToF kết nối lại không tự mở khóa. Để phục hồi: dừng băng tải, chuyển hệ thống
về IDLE (`p` nếu đang RUNNING), dọn hết phôi còn trên băng rồi khởi động lại (`p`).
Phần mềm không điều khiển việc cắt nguồn băng tải hay thay thế E-STOP phần cứng.

FIFO giả định phôi không vượt nhau, mỗi phôi tạo đúng một phiên ToF và một
lượt LM393. Nếu ToF bỏ sót/đếm đôi nhưng vẫn có vé trong cửa sổ thời gian,
FIFO không thể tự chứng minh đúng danh tính; cần hiệu chỉnh vùng đo, khoảng
cách phôi hoặc bổ sung theo dõi vị trí bằng encoder nếu cần bảo đảm mạnh hơn.

## 4. Servo và vị trí cảm biến

Giữ cách đặt LM393 chéo của bạn: thân cảm biến có thể ở sau tay đẩy, còn tia
phải bắt phôi tại vùng tác động mong muốn. Điều kiện là phiên ToF đã kết thúc
và quyết định đã chốt trước thời điểm LM393 được xác nhận. Nếu không đủ
khoảng di chuyển, việc đo hết lượt và đẩy ngay không thể đồng thời đáp ứng.

Mặc định PUSH 220 ms rồi ra lệnh HOME, chờ hồi 150 ms mới sẵn sàng. HOME=1500 µs,
PUSH=2500 µs là giá trị lệnh cần hiệu chỉnh riêng từng servo. Thời gian giữ PUSH
không phải thời gian servo chắc chắn tới vị trí. Tín hiệu LM393 đã lọc mới kích
lệnh; điểm chạm giữa phôi còn phụ thuộc tốc độ băng và thời gian quay thực tế.
Firmware không có phản hồi xác nhận servo đã hồi hoặc phôi đã vào đúng khay.

## 5. Kiểm chứng và triển khai

Logic đo ở `sorter_core.h`; FIFO/servo ở `station1_servo_trial_logic.h`;
`main.cpp` tích hợp hai trạm và `station1_servo_trial.cpp` là bản thử riêng trạm 1.
Trung vị sắp xếp trực tiếp bộ đệm đã cắt; bộ đệm firmware ở vùng nhớ tĩnh để
không sao chép mảng 256 mẫu lên stack Pico. Insertion sort bị chặn ở 256 mẫu.
Không cần bitwise để rút ngắn thời gian đo hay thời gian servo; chưa có số đo
hiệu năng chứng minh phép modulo FIFO là điểm nghẽn.

Chạy kiểm tra compile-time bằng C++17:

```powershell
g++ -std=c++17 -Wall -Wextra -Werror -fsyntax-only tests/test_sorter_core.cpp tests/test_station1_servo_trial.cpp tests/test_sorter_fsm.cpp
```

Driver ToF hiện bỏ qua tham số chu kỳ của `startContinuous(15)` và chạy
back-to-back; không được giả định đúng 15 ms/mẫu. Đo nhịp đọc thực tế và số mẫu
mỗi phôi trước khi chốt tốc độ băng, số mẫu tối thiểu và khoảng cách phôi.

Chưa nạp hay chạy cơ cấu bằng bản sửa này. Lỗi servo trạm 1 không quay trong
lần thử trực tiếp trước đó vẫn cần kiểm tra phần cứng; đổi thuật toán chưa xác
nhận giải quyết được lỗi đó. Bộ đếm phân loại/lệnh PWM không xác nhận phôi thực
sự đã vào khay. Kết quả build và điểm dừng mới nhất ghi tại `progress.md`.
