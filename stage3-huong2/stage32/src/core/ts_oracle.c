/* ts_oracle.c — Monotonic Timestamp Oracle implementation */
#include "ts_oracle.h"

void ts_oracle_init(TsOracle *oracle, uint64_t initial_ts) {
    uint64_t start = (initial_ts >= TS_MIN) ? initial_ts : TS_MIN;
    atomic_store_explicit(&oracle->current_ts, start, memory_order_relaxed);
}

uint64_t ts_oracle_next(TsOracle *oracle) {
    /* fetch_add trả về giá trị CŨ, ta cộng 1 nên giá trị mới = old + 1 */
    return atomic_fetch_add_explicit(&oracle->current_ts, 1, memory_order_acq_rel) + 1;
}

uint64_t ts_oracle_current(TsOracle *oracle) {
    return atomic_load_explicit(&oracle->current_ts, memory_order_acquire);
}

void ts_oracle_advance(TsOracle *oracle, uint64_t min_ts) {
    uint64_t cur = atomic_load_explicit(&oracle->current_ts, memory_order_acquire);
    while (cur < min_ts) {
        /* CAS: nếu current vẫn là cur, set thành min_ts; nếu không thì retry */
        if (atomic_compare_exchange_weak_explicit(
                &oracle->current_ts, &cur, min_ts,
                memory_order_acq_rel, memory_order_acquire)) {
            break;
        }
        /* cur được cập nhật tự động bởi CAS nếu thất bại */
    }
}
