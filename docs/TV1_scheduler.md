# TV1 – Scheduler MLQ: đã code gì, để làm gì, hoạt động thế nào

## 1. Tóm tắt

Phần scheduler đã chạy đúng chính sách MLQ trong đặc tả (mục 2.1). Các test `sched`, `sched_0`, `sched_1` chạy hết, không treo, không mất process. Riêng `sched` và `sched_0` đã chạy lặp 30 lần mỗi test và không lần nào lỗi.

| File | Thay đổi |
| --- | --- |
| `src/queue.c` | Viết `enqueue`, `dequeue`, `purgequeue` |
| `src/sched.c` | Viết `get_mlq_proc` theo chính sách slot; sửa `put/add_mlq_proc`, bản không MLQ, `queue_empty`; thêm `finish_proc` |
| `include/sched.h` | Đổi include guard `QUEUE_H` → `SCHED_H`; khai báo `finish_proc` |
| `src/os.c` | `cpu_routine`: gọi `finish_proc(proc)` trước `free(proc)` |
| `input/sched_mlq_slot`, `input/proc/t_long` | Test mới kiểm tra giới hạn slot |
| `tools/gantt.py` | Script vẽ Gantt chart từ output (phục vụ report) |

## 2. `src/queue.c` – hàng đợi PCB

`struct queue_t` là một mảng `proc[MAX_QUEUE_SIZE]` (50 phần tử) kèm biến `size`.

- **`enqueue(q, proc)`**: thêm PCB vào cuối mảng. Nếu hàng đợi đầy thì báo lỗi ra `stderr` và bỏ qua, không ghi tràn mảng.
- **`dequeue(q)`**: lấy PCB có độ ưu tiên cao nhất (giá trị `prio` nhỏ nhất), sau đó dồn các phần tử phía sau lên một ô.
  - Nếu nhiều PCB cùng độ ưu tiên thì lấy PCB vào trước.
  - Trong MLQ, mỗi hàng đợi chỉ chứa PCB cùng một `prio`, nên `dequeue` hoạt động như FIFO. Kết quả là các process cùng mức chạy xoay vòng (round robin).
  - Ở chế độ không MLQ, nó so sánh trường `priority` (priority cũ ghi trong file chương trình).
- **`purgequeue(q, proc)`**: tìm đúng con trỏ `proc` trong hàng đợi và xóa nó ra. Hàm này dùng để gỡ process khỏi `running_list`.

Các hàm này **không tự khóa**. Hàm gọi trong `sched.c` luôn giữ `queue_lock` khi gọi chúng, nên không có hai CPU cùng sửa một hàng đợi.

## 3. `src/sched.c` – chính sách MLQ

### 3.1 Dữ liệu

- `mlq_ready_queue[MAX_PRIO]`: 140 hàng đợi, mỗi mức `prio` (0 đến 139) một hàng đợi.
- `slot[prio]`: số lượt dispatch hàng đợi `prio` còn được dùng trong vòng hiện tại. Ban đầu `slot[prio] = MAX_PRIO - prio`, nên prio 0 có 140 lượt và prio 139 có 1 lượt.
- `running_list`: các process đang chạy trên CPU.
- `queue_lock`: một mutex bảo vệ tất cả các hàng đợi trên.

### 3.2 `get_mlq_proc()` – chọn process cho CPU

```
khóa queue_lock
lặp tối đa 2 lần:
    duyệt prio = 0 → 139:
        nếu hàng đợi prio không rỗng và slot[prio] > 0:
            lấy process ra, slot[prio]--, dừng
    nếu không lấy được process nào:
        nạp lại slot của mọi hàng đợi (bắt đầu vòng mới) rồi quét lại
nếu có process: đưa vào running_list
mở khóa
```

Ý nghĩa:

- **Luôn ưu tiên mức cao.** Mỗi lần CPU rảnh, scheduler quét lại từ prio 0. Process mức cao vừa hết time slice sẽ được lấy lại ngay, giống output mẫu `output/sched.output`.
- **Slot chống "chết đói".** Hàng đợi `prio` chỉ được cấp tối đa `MAX_PRIO - prio` lượt mỗi vòng. Dùng hết thì phải nhường hàng đợi kế tiếp, kể cả khi vẫn còn việc. Đặc tả gọi là: "left the remaining work for future slot".
- **Nạp lại slot.** Khi mọi hàng đợi còn process đều đã hết slot thì một vòng kết thúc: nạp lại slot cho tất cả rồi quét lại. Vòng lặp tối đa 2 lần nên hàm luôn dừng.
- **Không còn process nào** thì trả về `NULL`, CPU sẽ chờ sang time slot sau.

Ví dụ trong test `sched_mlq_slot`: có 2 process prio 138 (slot = 2) và 1 process prio 139 (slot = 1), time slice = 1. Thứ tự dispatch thu được là `1 2 3 1 2 3 …`. Hai process prio 138 được 2/3 thời gian CPU, process prio 139 được 1/3 và không bị bỏ đói.

Sửa lỗi có sẵn: bản gốc gọi `pthread_mutex_lock` nhưng thiếu `unlock`. CPU thứ hai gọi `get_proc` sẽ bị treo vĩnh viễn.

### 3.3 `put_mlq_proc()` và `add_mlq_proc()`

- **`put_mlq_proc`** được gọi khi process hết time slice: gỡ process khỏi `running_list` (`purgequeue`) rồi đưa về `mlq_ready_queue[proc->prio]`.
- **`add_mlq_proc`** được loader gọi khi có process mới: chỉ đưa vào hàng đợi theo `prio`, vì process chưa từng chạy.
- Cả hai dùng hàm `valid_prio`: nếu `prio` trong file input ≥ 140 thì kẹp về 139, tránh ghi ra ngoài mảng.

### 3.4 `finish_proc()` và `running_list`

Ở bản gốc, process chạy xong bị `free()` nhưng vẫn nằm trong `running_list`. Sau đó, ai duyệt danh sách này (ví dụ syscall tìm process theo PID của TV3) sẽ đọc vào vùng nhớ đã giải phóng.

Bây giờ `cpu_routine` gọi `finish_proc(proc)` trước `free(proc)`, nên `running_list` luôn chỉ chứa process đang thật sự chạy.

### 3.5 Các sửa nhỏ khác

- **`queue_empty()`** ở bản gốc trả `-1` (tức là "đúng") khi còn process, nên ý nghĩa bị ngược. Giờ hàm trả 1 khi rỗng, 0 khi còn process, và có khóa khi đọc.
- **`include/sched.h`** dùng cùng include guard `QUEUE_H` với `queue.h`. File nào include `queue.h` trước `sched.h` sẽ mất khai báo `get_proc()`, và trình biên dịch tự coi `get_proc()` trả về `int`, làm hỏng con trỏ trên máy 64-bit. Đã đổi guard thành `SCHED_H`.
- **Bản không MLQ** (khi tắt `MLQ_SCHED`) cũng đã được hoàn thiện. Process lấy từ `ready_queue`; khi `ready_queue` rỗng thì chuyển các process trong `run_queue` (đã dùng hết time slice) sang lại. Đặc tả nói `run_queue` đã lỗi thời, nên phần này chỉ giữ để tương thích.

## 4. Đồng bộ

| Tài nguyên dùng chung | Ai truy cập | Bảo vệ bằng |
| --- | --- | --- |
| `mlq_ready_queue[]`, `slot[]`, `ready_queue`, `run_queue` | Loader (`add_proc`), mọi CPU (`get_proc`, `put_proc`) | `queue_lock` |
| `running_list` | Mọi CPU; sau này syscall của TV3 | `queue_lock` |

Lưu ý cho TV3: `running_list` và `queue_lock` là biến `static` trong `sched.c`. Nếu syscall cần tìm process theo PID thì nên thêm một hàm trong `sched.c` để duyệt danh sách trong khi giữ `queue_lock`, thay vì đọc thẳng qua `krnl->running_list` mà không khóa.

## 5. Cách chạy test

Các file `input/sched*` không có dòng cấu hình bộ nhớ, nên khi chạy chúng phải **tạm** bật `MM_FIXED_MEMSZ` trong `include/os-cfg.h`:

```c
#define MM_FIXED_MEMSZ      // bỏ "//" khi test scheduler, nhớ comment lại trước khi commit
```

```bash
make clean && make os
./os sched           > out_sched.txt
./os sched_mlq_slot  > out_slot.txt
python3 tools/gantt.py out_sched.txt     # in Gantt chart theo CPU và time slot
```

Ví dụ Gantt chart của test `sched` (2 CPU, time slice 4):

```
slot    0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24
CPU0    1  1  1  1  1  1  1  1  1  1  1  1  3  3  3  3  2  2  2  2  2  2  .  .  .
CPU1    .  .  2  2  2  2  4  4  4  4  4  4  4  4  4  4  4  3  3  3  3  3  3  3  3
```

PID 1 và 4 (prio 0) chạy trước, PID 2 và 3 (prio 1) chạy sau. Kết quả có thể lệch vài slot giữa các lần chạy vì loader và CPU là các thread song song, đúng như đặc tả đã nói.

## 6. Giới hạn và việc còn lại

- **Các test có lệnh `alloc`** (ví dụ `os_1_singleCPU_mlq`) hiện bị segfault tại `get_free_vmrg_area` trong `src/libmem.c`. Đây là phần bộ nhớ của TV2/TV4 chưa code, không phải lỗi scheduler.
- **Một slot = một lần dispatch** (một lần CPU lấy process ra chạy hết một time slice), không phải một đơn vị thời gian. Nếu giảng viên hiểu khác thì chỉ cần sửa chỗ `slot[prio]--` trong `get_mlq_proc`.

## 7. Gợi ý cho report – câu hỏi mục 2.1

"Lợi ích của từng chính sách trong MLQ":

- **Mỗi mức ưu tiên một hàng đợi**: chọn process chỉ cần quét tối đa 140 hàng đợi, không phải sắp xếp lại toàn bộ danh sách.
- **Quét từ prio cao xuống**: process quan trọng (prio nhỏ) có thời gian phản hồi ngắn.
- **Slot = MAX_PRIO - prio**: chia CPU theo tỉ lệ ưu tiên; mức thấp vẫn chắc chắn được chạy trong mỗi vòng, nên không bị bỏ đói như priority scheduling thuần.
- **Round robin trong cùng mức**: công bằng giữa các process có cùng prio.
- **Không có feedback**: process không bị đổi mức, nên hành vi dễ dự đoán và dễ kiểm chứng bằng Gantt chart. Đổi lại, hệ thống không tự thích nghi với process I/O-bound hay CPU-bound.
