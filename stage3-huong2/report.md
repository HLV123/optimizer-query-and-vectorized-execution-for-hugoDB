# Báo cáo: Hugo DB Stage 3 — Retrofit MVCC (Hướng 2)

> **Base code:** Hugo DB Stage 2 (2PL, WAL, B-tree, HugoQL)  
> **Mục tiêu:** Retrofit MVCC vào Hugo DB, giữ nguyên 2PL, switchable runtime

---

## 1. Xuất phát điểm — Stage 2 đã có gì

Stage 2 của Hugo DB là một document database viết bằng C với các thành phần:

| Component | File | Mô tả |
|-----------|------|-------|
| B-tree | `btree.c`, `dbtree.c` | Index structure |
| Page manager | `page.c` | 4KB pages, serializer |
| Buffer pool | `buffer_pool.c` | LRU cache |
| WAL | `wal.c` | ARIES-style, before/after image, CRC32 |
| Lock Manager | `lock_manager.c` | 2PL, S/X locks, deadlock detection |
| Disk DB | `disk_db.c`, `executor_disk.c` | On-disk DB với transaction support |
| Query engine | `src/query/` | HugoQL parser + executor |
| HTTP server | `http_server.c` | REST API + embedded HTML UI |


---

## 2. Những gì đã phát triển thêm cho Stage32

### 2.1. Files mới tạo

| File | Phase | Mô tả |
|------|-------|-------|
| `src/core/ts_oracle.h/.c` | 1 | Monotonic timestamp counter dùng C11 `_Atomic` |
| `src/core/doc_version.h/.c` | 2 | DocVersion struct + serialize/deserialize |
| `src/core/mvcc_tx.h/.c` | 3 | MvccTx, MvccTxRegistry, MvccCommittedTable |
| `src/core/mvcc_read.h/.c` | 3 | Visibility check algorithm, mvcc_find_doc, mvcc_scan |
| `src/core/mvcc_write.h/.c` | 4 | INSERT/UPDATE/DELETE tạo version chain, commit/abort |
| `src/core/mvcc_recovery.h/.c` | 5 | WAL recovery rebuild version chains sau crash |
| `src/core/mvcc_vacuum.h/.c` | 6 | Garbage collection — reclaim old versions |
| `tests/test_mvcc_phase3.c` | 3 | 15 test cases |
| `tests/test_mvcc_phase5.c` | 5 | 4 test cases WAL recovery |
| `tests/test_mvcc_phase6.c` | 6 | 4 test cases vacuum |
| `bench/bench_mvcc.c` | 7 | Benchmark 2PL vs MVCC, 5 workloads |
| `docs/mvcc.md` | 7 | Architectural overview |
| `docs/2pl-vs-mvcc.md` | 7 | Comparison + analysis |

### 2.2. Files modified

| File | Thay đổi |
|------|----------|
| `disk_db.h` | Thêm `IsolationMode` enum, `PAGE_TYPE_MVCC_VERSION`, MVCC fields vào `DiskDB` struct |
| `disk_db.c` | Init TsOracle, MvccTxRegistry, MvccCommittedTable khi open; gọi `mvcc_recover()` |
| `executor_disk.c` | Dispatch tất cả DML verbs (INSERT/UPDATE/DELETE/SELECT) qua MVCC path khi `mode==HUGO_MODE_MVCC`; GINAN/COMETI/TULABERK tạo/commit/abort MvccTx |
| `wal.h/.c` | Thêm `WAL_MVCC_BEGIN`, `WAL_MVCC_COMMIT`, `WAL_MVCC_VERSION` record types |
| `src/query/executor.c` | Fix cross-platform `strtok_s` → `strtok_r` |

---

## 3. Kiến trúc MVCC đã implement

### 3.1. Version chain

Thay vì update in-place, mỗi write tạo version mới và chain về version cũ:

```
doc_page_ids[id] → V3(latest) → V2 → V1 → NULL
                   created_ts=30  20   10
```

**DocVersion struct (44-byte header on-disk):**
```
version_id       u64  — unique, monotonic
created_ts       u64  — commit timestamp (0 = uncommitted)
deleted_ts       u64  — 0 = alive; nonzero = deleted
created_tx       u64  — tx_id của transaction tạo version
prev_version_ptr u64  — pointer page_id+offset về version cũ
data_size        u32  — bytes document data
data[]           bytes
```

### 3.2. Visibility algorithm (5 rules)

Khi transaction T đọc doc, walk version chain từ latest → oldest:

| Rule | Condition | Kết quả |
|------|-----------|---------|
| 1 | Creator bị abort | SKIP |
| 2 | Creator = chính T (own write) | VISIBLE |
| 3 | Creator trong active_set của T | SKIP (in-flight) |
| 4 | V.created_ts > T.begin_ts | SKIP (committed sau snapshot) |
| 5a | V.deleted_ts == 0 | VISIBLE |
| 5b | V.deleted_ts > T.begin_ts | VISIBLE (delete chưa visible) |
| 5c | V.deleted_ts ≤ T.begin_ts | NOT_FOUND |

### 3.3. WAL extensions

```
WAL_MVCC_BEGIN   (6): tx_id + begin_ts
WAL_MVCC_COMMIT  (7): tx_id + commit_ts  
WAL_MVCC_VERSION (8): tx_id + doc_id + version_ptr + coll_name
```

### 3.4. Vacuum algorithm

```
oldest_ts = min(begin_ts of all active transactions)
for each (coll, doc_id):
    walk chain → tìm anchor (created_ts ≤ oldest_ts)
    cắt chain tại anchor → versions cũ hơn mark PAGE_TYPE_FREE
```

---

## 4. Lệnh PowerShell và kết quả

### 4.1. Compile và chạy test Phase 3 (MVCC Read Path)

```powershell
gcc -Wall -O2 -std=c11 -I src/core -I src/query `
    tests/test_mvcc_phase3.c `
    src/core/checksum.c src/core/hugo_io_win.c src/core/page.c `
    src/core/wal.c src/core/disk_db.c src/core/collection.c `
    src/core/executor_disk.c src/core/ts_oracle.c src/core/mvcc_tx.c `
    src/core/doc_version.c src/core/mvcc_read.c src/core/mvcc_write.c `
    src/core/mvcc_recovery.c src/core/mvcc_vacuum.c `
    src/query/tokenizer.c src/query/parser.c src/query/executor.c `
    -o test_mvcc_phase3.exe -lws2_32

.\test_mvcc_phase3.exe
```

**Kết quả:**
```
=== Hugo DB Stage 3: MVCC Read Path Tests ===

--- Unit tests ---
  [  OK ] ts_oracle_monotonic
  [  OK ] doc_version_roundtrip
  [  OK ] mvcc_tx_active_set
  [  OK ] mvcc_registry
  [  OK ] version_ptr_encoding
  [  OK ] committed_table

--- Integration tests ---
  [  OK ] mvcc_db_open
  [  OK ] ddb_open_mode
  [  OK ] read_own_writes
  [  OK ] snapshot_isolation
  [  OK ] abort_not_visible
  [  OK ] ww_conflict
  [  OK ] concurrent_readers_no_block
  [  OK ] writer_not_blocking_readers
  [  OK ] delete_visibility

=== Results: 15/15 passed ===
```

**Kết luận từ Phase 3:**
- `ts_oracle_monotonic` → Timestamp counter tăng đơn điệu, không bao giờ lặp lại
- `snapshot_isolation` → T2 begin trước T3 update, T2 vẫn thấy giá trị cũ dù T3 đã commit — đây chính xác là Snapshot Isolation của PostgreSQL
- `abort_not_visible` → Dữ liệu của aborted tx hoàn toàn không visible với bất kỳ tx nào khác
- `ww_conflict` → First-committer-wins: T1 commit trước thắng, T2 nhận `MVCC_ERR_CONFLICT`
- `writer_not_blocking_readers` → Writer uncommitted không block readers — đây là lợi thế cốt lõi của MVCC

---

### 4.2. Compile và chạy test Phase 5 (WAL Recovery)

```powershell
.\test_mvcc_phase5.exe
```

**Kết quả:**
```
=== Hugo DB Stage 3: Phase 5 (WAL Recovery) Tests ===

  [  OK ] wal_mvcc_records
  [  OK ] recovery_committed
  [  OK ] recovery_loser_tx
  [  OK ] ts_oracle_advances

=== Results: 4/4 passed ===
```

**Kết luận từ Phase 5:**
- `wal_mvcc_records` → WAL ghi đúng 3 record types: MVCC_BEGIN, MVCC_COMMIT, MVCC_VERSION
- `recovery_committed` → Sau crash + reopen, data của committed tx vẫn còn nguyên
- `recovery_loser_tx` → Tx chưa commit khi crash (loser) bị rollback khi recovery — data không tồn tại sau reopen
- `ts_oracle_advances` → Oracle advance past pre-close timestamp sau recovery → timestamps không bao giờ reuse

---

### 4.3. Compile và chạy test Phase 6 (Vacuum GC)

```powershell
.\test_mvcc_phase6.exe
```

**Kết quả:**
```
=== Hugo DB Stage 3: Phase 6 (Vacuum) Tests ===

  [  OK ] oldest_visible_ts
  [  OK ] vacuum_removes_old_versions
        (removed=5 versions, freed=5 pages)
  [  OK ] vacuum_preserves_active_snapshot
        oldest_ts=5 → removed=0 (long tx đang giữ snapshot)
        oldest_ts=9 → removed=1 (sau khi long tx commit)
  [  OK ] vacuum_stats
        (removed=10 freed=10 oldest_ts=34)

=== Results: 4/4 passed ===
```

**Kết luận từ Phase 6:**
- `vacuum_removes_old_versions` → Sau 5 lần update doc, vacuum xóa được 5 versions cũ, freed 5 pages
- `vacuum_preserves_active_snapshot` → **Quan trọng nhất**: khi long-running tx đang active với snapshot cũ, vacuum KHÔNG xóa version đó. Chỉ sau khi tx commit mới xóa được — đây chính xác là lý do PostgreSQL cần AUTOVACUUM và tại sao long transaction gây bloat
- `vacuum_stats` → 10 updates → 10 old versions bị thu hồi sau vacuum

---

### 4.4. Benchmark 2PL vs MVCC

```powershell
# Fix clock_gettime cho Windows trước, rồi compile
.\bench_mvcc.exe 5000 500
```

**Kết quả thực tế trên máy (Windows, MinGW gcc 15.2.0):**

```
Mode   Workload         ops/sec    elapsed   p50µs   p99µs  aborts
------ --------------- -------- ---------- ------- ------- ------
2PL    W1_reads          111,118    0.045s     8.6    15.0       0
MVCC   W1_reads              754    6.633s  1185.9  2379.5       0

2PL    W2_hotwrite           784    6.376s  1190.8  2342.2       0
MVCC   W2_hotwrite           379   13.183s  2427.8  4482.5       0

2PL    W3_multiwrite         794    6.299s  1186.4  2139.8       0
MVCC   W3_multiwrite         343   14.562s  2691.8  5049.0       0

2PL    W4_mixed            1,463    3.418s   992.0  2439.3       0
MVCC   W4_mixed              487   10.271s  2027.7  4662.6       0

2PL    W5_longreader         786    6.360s  1200.5  2543.7       0
MVCC   W5_longreader         401   12.482s  2407.4  4772.3       0
```

**Phân tích kết quả benchmark:**

| Workload | Winner | Lý do |
|----------|--------|-------|
| W1: reads | **2PL** (147x) | MVCC phải malloc MvccTx + walk version chain cho mỗi read. 2PL đọc thẳng page |
| W2: hot write | **2PL** (2x) | MVCC alloc page mới + serialize DocVersion header 44 bytes + WAL_MVCC_VERSION mỗi write |
| W3: multi write | **2PL** (2.3x) | Cùng lý do W2, overhead version creation |
| W4: mixed | **2PL** (3x) | 2PL raw read nhanh bù đắp |
| W5: long reader | **2PL** (2x) | Single-thread không thể hiện lợi thế MVCC |

**Kết luận quan trọng từ benchmark:**

> Benchmark này chạy **single-threaded** nên không đo được lợi thế thực sự của MVCC. Trong môi trường production multi-threaded:
> - **2PL**: Reader giữ S-lock → Writer phải chờ → throughput giảm tuyến tính theo contention
> - **MVCC**: Reader đọc snapshot → Writer tạo version mới song song → throughput scale với số cores
>
> Đây chính xác là lý do **PostgreSQL, CockroachDB, TiDB** đều chọn MVCC.

**Overhead của MVCC write giải thích:**
> Tại sao `UPDATE` trên PostgreSQL chậm hơn MySQL (single row, single thread):
> PostgreSQL phải: alloc dead tuple + write new tuple + update visibility map + WAL
> MySQL InnoDB in-place update + undo log entry (ít I/O hơn cho single writer)

---

## 5. Demo qua HugoQL — Kết quả thực tế trên DB

### 5.1. Setup

```powershell
# Embed UI
python scripts/embed_ui.py
# Output: embedded 40656 bytes → src/core/ui_html.h

# Compile server
gcc -O2 -std=c11 -I src/core -I src/query src/cli/hugo_serve.c [sources] -o hugo_serve.exe -lws2_32

# Chạy server với 5000 documents sample
.\hugo_serve.exe demo.hugo 7777
```

Truy cập `http://localhost:7777` → UI Hugo DB với collection `employees` (5000 docs).

### 5.2. Demo MVCC Abort — Tab Batch

Chạy batch sau trên 5000 documents thực tế:

```sql
funden employees haar id $bg 1 lime 1
ginan
cochin employees haar id $bg 1 $quy status "se_bi_huy"
funden employees haar id $bg 1 lime 1
tulaberk
funden employees haar id $bg 1 lime 1
```

**Kết quả: 6/6 OK · 0 err · 0.284s total**

| # | Query | Time | Result |
|---|-------|------|--------|
| 1 | funden... lime 1 | 50ms | ✅ 1 doc — status = **on_leave** |
| 2 | ginan | 16ms | ✅ transaction begin (MVCC, begin_ts assigned) |
| 3 | cochin... $quy status "se_bi_huy" | 64ms | ✅ updated 1 — version mới tạo trong chain |
| 4 | funden... lime 1 | 80ms | ✅ 1 doc — status = **se_bi_huy** |
| 5 | tulaberk | 0ms | ✅ transaction abort — doc_page_ids restored |
| 6 | funden... lime 1 | 74ms | ✅ 1 doc — status = **on_leave** |

**Kết luận từ demo:**

- **Query 2→3**: `ginan` assign begin_ts từ TsOracle, snapshot active set
- **Query 3→4**: `cochin` tạo DocVersion mới với `created_tx = tx_id`, `created_ts = 0` (uncommitted). `funden` thấy vì Rule 2 (own write visible)
- **Query 5**: `tulaberk` gọi `mvcc_abort_tx()` → walk write_set → restore `doc_page_ids[1]` về `prev_version_ptr` → version "se_bi_huy" bị orphan, sẽ được vacuum dọn
- **Query 6**: `funden` thấy status = `on_leave` — thay đổi biến mất hoàn toàn, **Atomicity đảm bảo**

---

## 6. Tổng kết — Tất cả test results

| Phase | Test file | Kết quả | Nội dung |
|-------|-----------|---------|---------|
| 3 | `test_mvcc_phase3.exe` | **15/15 passed** | Read path, visibility check, snapshot isolation |
| 5 | `test_mvcc_phase5.exe` | **4/4 passed** | WAL recovery, crash safety |
| 6 | `test_mvcc_phase6.exe` | **4/4 passed** | Vacuum GC, snapshot preservation |
| 2 (cũ) | `test_phase8.exe` | **36/36 passed** | Backward compat — 2PL vẫn hoạt động |
| **TOTAL** | | **59/59 passed** | Zero regression |

---

## 7. Kết luận học thuật

Qua việc build MVCC engine bằng C, đã hiểu rõ:

### Tại sao PostgreSQL cần VACUUM?
Mỗi UPDATE không xóa version cũ mà append version mới. Version cũ tích lũy trong chain. Vacuum phải scan để tìm versions không còn ai cần (older than `oldest_visible_ts`) và reclaim pages. Test `vacuum_preserves_active_snapshot` chứng minh: **long-running transaction block vacuum** — đây là nguyên nhân gây table bloat trong production PostgreSQL.

### Tại sao PostgreSQL write chậm hơn MySQL single-row UPDATE?
Benchmark W2/W3 đo được overhead: mỗi MVCC write phải alloc page mới, serialize DocVersion header 44 bytes, ghi WAL_MVCC_VERSION, sync. MySQL InnoDB update in-place + undo log entry → ít I/O hơn cho single writer không có concurrency.

### Tại sao distributed DB chọn MVCC?
Không cần distributed lock coordinator. Readers không block writers. Timestamps (begin_ts/commit_ts) compatible với distributed clock (HLC của CockroachDB, TrueTime của Spanner).

### Snapshot Isolation có an toàn tuyệt đối không?
**Không.** Write skew vẫn có thể xảy ra với SI. Fix cần Serializable Snapshot Isolation (SSI) — PostgreSQL 9.1+ implement SSI, Hugo DB để làm stretch goal.

### 2PL vs MVCC — khi nào dùng gì?
- **2PL tốt hơn**: Single writer, low concurrency, write-heavy workload không có concurrent reads
- **MVCC tốt hơn**: Nhiều concurrent readers + writers, analytics query chạy song song với OLTP, distributed systems

---

