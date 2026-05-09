/* ts_oracle.h — Monotonic Timestamp Oracle cho MVCC
 *
 * Cung cấp monotonic counter dùng làm transaction timestamps.
 * Thread-safe qua C11 stdatomic.
 *
 * Mỗi transaction nhận:
 *   begin_ts  — snapshot tại thời điểm begin
 *   commit_ts — gán tại thời điểm commit
 *
 * Timestamp 0 là giá trị invalid/chưa set.
 * Counter bắt đầu từ 1, tăng đơn điệu, không bao giờ lặp lại.
 *
 * Persistence: ts hiện tại được save vào WAL checkpoint để sau restart
 * counter không bị lùi về 0 (gây reuse timestamps cũ).
 */
#ifndef HUGO_TS_ORACLE_H
#define HUGO_TS_ORACLE_H

#include <stdint.h>
#include <stdatomic.h>

#define TS_INVALID  0   /* timestamp chưa set hoặc không hợp lệ */
#define TS_MIN      1   /* giá trị hợp lệ nhỏ nhất */

typedef struct {
    _Atomic uint64_t current_ts;   /* atomic counter, thread-safe */
} TsOracle;

/* Khởi tạo oracle với giá trị ban đầu (thường là 1 hoặc giá trị restore từ WAL) */
void     ts_oracle_init   (TsOracle *oracle, uint64_t initial_ts);

/* Lấy timestamp tiếp theo (tăng counter và trả về giá trị mới) */
uint64_t ts_oracle_next   (TsOracle *oracle);

/* Đọc timestamp hiện tại mà không tăng (dùng cho oldest_visible_ts calc) */
uint64_t ts_oracle_current(TsOracle *oracle);

/* Advance oracle đến ít nhất min_ts (dùng khi recovery từ WAL).
 * Nếu current < min_ts, set current = min_ts. */
void     ts_oracle_advance(TsOracle *oracle, uint64_t min_ts);

#endif
