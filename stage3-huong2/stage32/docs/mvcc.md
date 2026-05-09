# Hugo DB MVCC — Architectural Overview

## Mục tiêu

Hugo DB Stage 3 retrofit **MVCC (Multi-Version Concurrency Control)** vào engine 2PL hiện có. Hai modes cùng tồn tại và switchable runtime:

```c
DiskDB db;
ddb_open_mode(&db, "mydb.hugo", HUGO_MODE_2PL);   // behavior như stage 1-2
ddb_open_mode(&db, "mydb.hugo", HUGO_MODE_MVCC);  // MVCC mode
```

---

## Core concepts

### 1. Version chain

Mỗi document không còn lưu in-place. Thay vào đó, mỗi update tạo một **DocVersion mới**, chain về version cũ:

```
doc_page_ids[id] → V3 → V2 → V1 → NULL
                 (latest)         (oldest)
```

`DocVersion` struct (on-disk, 44-byte header):

```
version_id       u64  — unique, monotonic
created_ts       u64  — commit timestamp của tx tạo (0 = uncommitted)
deleted_ts       u64  — 0 = alive; nonzero = deleted at this ts
created_tx       u64  — tx_id (cho visibility check uncommitted)
prev_version_ptr u64  — pointer page_id+offset về version cũ hơn
data_size        u32  — bytes document data
data[]           bytes
```

**Version pointer encoding** (1 x u64):
```
bits [63..16] = page_id (48 bits)
bits [15..0]  = offset  (16 bits)
```

### 2. Timestamp oracle

Global monotonic counter, thread-safe (`_Atomic uint64_t`):

```c
uint64_t begin_ts  = ts_oracle_next(&db->mvcc_oracle); // khi BEGIN
uint64_t commit_ts = ts_oracle_next(&db->mvcc_oracle); // khi COMMIT
```

Oracle được advance sau restart để không reuse timestamps cũ.

### 3. Transaction state

```c
typedef struct MvccTx {
    uint64_t  tx_id;
    uint64_t  begin_ts;
    uint64_t  commit_ts;
    uint64_t *active_set;    // tx_id list đang active tại begin
    size_t    n_active;
    MvccWriteEntry *write_set; // versions ta đã tạo
    size_t    n_writes;
    MvccTxState state;       // ACTIVE / COMMITTED / ABORTED
} MvccTx;
```

### 4. Isolation level: Snapshot Isolation

Mỗi transaction đọc **snapshot tại begin_ts**. Visibility của version V với transaction T:

| Rule | Condition | Result |
|------|-----------|--------|
| 1 | Creator của V bị abort | SKIP |
| 2 | Creator = T (own write) | VISIBLE |
| 3 | Creator trong active_set của T | SKIP |
| 4 | V.created_ts > T.begin_ts | SKIP |
| 5 | V.deleted_ts == 0 | VISIBLE |
| 5b | V.deleted_ts > T.begin_ts | VISIBLE |
| 5c | V.deleted_ts ≤ T.begin_ts, deleter committed | NOT_FOUND |

**Known limitation**: Write skew anomaly vẫn có thể xảy ra với SI (không phải Serializable). SSI là stretch goal.

### 5. Write-write conflict: First-committer-wins

```
T1 và T2 cùng update doc D:
  T1 update → commit (thành công)
  T2 update → CONFLICT khi commit vì latest version committed sau T2.begin_ts
```

T2 nhận `MVCC_ERR_CONFLICT`, caller phải abort và retry.

---

## File structure

```
src/core/
  ts_oracle.h/.c       — Timestamp oracle (atomic counter)
  doc_version.h/.c     — DocVersion struct + serialization
  mvcc_tx.h/.c         — MvccTx, MvccTxRegistry, MvccCommittedTable
  mvcc_read.h/.c       — Visibility check, mvcc_find_doc, mvcc_scan
  mvcc_write.h/.c      — INSERT/UPDATE/DELETE/COMMIT/ABORT
  mvcc_recovery.h/.c   — WAL recovery rebuild version chains
  mvcc_vacuum.h/.c     — Garbage collection
```

### Modified files

| File | Thay đổi |
|------|----------|
| `disk_db.h` | Thêm `IsolationMode`, `PAGE_TYPE_MVCC_VERSION`, MVCC fields vào `DiskDB` |
| `disk_db.c` | Init MVCC infrastructure; call `mvcc_recover()` khi open |
| `executor_disk.c` | Dispatch tất cả DML verbs qua MVCC path khi `mode==HUGO_MODE_MVCC` |
| `wal.h/.c` | Thêm `WAL_MVCC_BEGIN/COMMIT/VERSION` record types |

---

## WAL extension (Phase 5)

3 record types mới:

```
WAL_MVCC_BEGIN   (6): tx_id + begin_ts (encoded in page_id field)
WAL_MVCC_COMMIT  (7): tx_id + commit_ts
WAL_MVCC_VERSION (8): tx_id + doc_id + version_ptr + coll_name (in before[] field)
```

### Recovery algorithm

1. **Pass 1**: Scan WAL, build `tx_map` (tx_id → state) và `ver_log` (version creation events)
2. **Pass 2**: Populate `committed_table` — committed tx ghi commit_ts; loser/aborted ghi aborted=1
3. **Pass 3**: Rebuild `doc_page_ids`:
   - Committed version → set pointer tới version
   - Aborted/loser version → restore về `prev_version_ptr`
4. **Pass 4**: Advance `TsOracle` qua max timestamp đã thấy

---

## Garbage Collection (Phase 6)

```c
VacuumStats stats;
mvcc_vacuum(&db, &stats);
// stats.versions_removed, stats.pages_freed, stats.oldest_visible_ts
```

### Algorithm

```
oldest_ts = min(tx.begin_ts for tx in active_transactions)
           (nếu không có active tx: oldest_ts = current_ts)

for each (collection, doc_id):
    walk chain từ latest → oldest
    tìm anchor = version đầu tiên có created_ts ≤ oldest_ts
    chain sau anchor (older) → safe to remove
    → cắt chain tại anchor (set anchor.prev = NULL)
    → mark dead pages as PAGE_TYPE_FREE
```

**Guarantee**: vacuum không bao giờ xóa version mà active transaction cần.

---

## Design decisions

### Tại sao append-only thay vì in-place + undo log?

- Đơn giản hơn (PostgreSQL/CockroachDB style)
- Không cần undo log riêng
- Recovery dễ hơn (forward-only scan)
- Trade-off: cần vacuum để reclaim space (như PostgreSQL VACUUM)

### Tại sao 1 version/page?

- Đơn giản hoá page layout, không cần compaction logic trong write path
- Trade-off: space waste, nhiều page I/O hơn
- Phase 6b có thể optimize với multiple versions/page

### Tại sao Snapshot Isolation, không phải Serializable?

- SI là standard của PostgreSQL, Oracle, MySQL (Repeatable Read)
- Serializable cần SSI (detect read-write conflicts) — stretch goal
- SI đủ cho hầu hết use cases, tránh deadlocks
