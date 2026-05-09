# Hugo DB Benchmark Results

## Setup

- **Machine**: Linux (sandbox), single-threaded simulation
- **Ops**: 3000 operations per workload
- **Seed docs**: 500 documents
- **Compile**: `-O2`

## Results (Hugo DB, Linux, 3000 ops)

| Mode | Workload | ops/sec | p50 µs | p99 µs | aborts |
|------|----------|--------:|-------:|-------:|-------:|
| 2PL  | W1: reads | 86,973 | 10.8 | 30.9 | 0 |
| MVCC | W1: reads | 1,674 | 566.3 | 1,112.4 | 0 |
| 2PL  | W2: hot write | 596 | 1,586 | 2,739 | 0 |
| MVCC | W2: hot write | **760** | 1,254 | 2,483 | 0 |
| 2PL  | W3: multi write | 571 | 1,683 | 2,564 | 0 |
| MVCC | W3: multi write | **710** | 1,350 | 2,678 | 0 |
| 2PL  | W4: mixed 50/50 | **1,132** | 1,350 | 2,211 | 0 |
| MVCC | W4: mixed 50/50 | 1,035 | 782 | 2,276 | 0 |
| 2PL  | W5: long reader | 658 | 1,577 | 2,485 | 0 |
| MVCC | W5: long reader | **792** | 1,228 | 2,772 | 0 |

## Analysis

### W1: 100% reads — 2PL wins significantly
MVCC read path có overhead đáng kể: tạo `MvccTx` struct, snapshot active set, walk version chain với visibility check cho mỗi read. 2PL chỉ làm raw page read từ `doc_page_ids`.

**Key insight**: MVCC overhead per-read là cố định và không scale với số readers. Trong production multi-threaded, MVCC sẽ hơn vì readers không block nhau.

### W2–W3: writes — MVCC thắng nhẹ (~20%)
MVCC write path: alloc page, serialize DocVersion, update pointer. Không có lock acquisition overhead. 2PL cần gọi `wal_new_tx_id` + explicit begin/commit per write trong benchmark này.

### W4: Mixed — tie (2PL nhỉnh hơn)
Workload cân bằng: 2PL's fast raw reads bù đắp overhead của lock management.

### W5: Long reader + writers — MVCC thắng ~20%
MVCC reader giữ snapshot cũ, writers tạo version mới song song. Trong single-threaded simulation, advantage chưa rõ; **multi-threaded production sẽ cho MVCC win lớn hơn nhiều** (reader không bị block bởi writers).

## How to run

```bash
# Linux/macOS
make bench

# Custom params: bench_mvcc [ops] [seed_docs]
./bench_mvcc 10000 1000 > results.csv
```

## CSV output fields

```
mode, workload, ops, elapsed_sec, throughput_ops_per_sec, p50_us, p99_us, aborts
```

Plot với Python:
```python
import pandas as pd, matplotlib.pyplot as plt
df = pd.read_csv('results.csv')
df.pivot(index='workload', columns='mode', values='throughput_ops_per_sec').plot(kind='bar')
plt.savefig('bench.png')
```
