# 2PL vs MVCC — Comparison

## Conceptual differences

| Aspect | 2PL (Two-Phase Locking) | MVCC |
|--------|------------------------|------|
| **Read-write conflict** | Reader blocks writer, writer blocks reader | Readers never block writers |
| **Write-write conflict** | Lock-based wait or deadlock | First-committer-wins, later abort |
| **Deadlock** | Possible (wait-for graph) | Never (no locks) |
| **Abort rate** | Low (only deadlock → abort) | Higher under write-write conflicts |
| **Space overhead** | In-place update | Version chain accumulates |
| **GC needed** | No | Yes (VACUUM) |
| **Read latency** | May block waiting for X locks | Always reads snapshot (no wait) |
| **Write latency** | May block waiting for S/X locks | Never waits, but may abort |

---

## When MVCC wins

### Long-running reads + concurrent writes

```
2PL:  Reader holds S-lock.  Writer wants X-lock → BLOCKS until reader done.
MVCC: Reader uses snapshot. Writer creates new version → NEVER blocks.
```

Classic use case: analytics query (`SELECT COUNT(*)` over large table) vs OLTP writes.

### Read-heavy workloads

MVCC readers đọc snapshot mà không cần acquire bất kỳ lock nào. Throughput tăng tuyến tính với số readers.

### Low write-conflict workloads

Nếu transactions ít khi write cùng document, abort rate thấp. MVCC cấp higher concurrency mà không overhead của lock management.

---

## When 2PL wins (or ties)

### High write-write conflicts

Single hot key được update bởi nhiều transactions concurrent → MVCC abort rate cao, effective throughput thấp hơn 2PL (2PL serialize chờ nhau, không abort).

### Write-only workloads, no concurrency

Single writer: 2PL overhead nhỏ, MVCC có overhead version creation. 2PL có thể nhanh hơn nhẹ.

---

## Benchmark results (sample — Hugo DB, 5000 ops, 1000 seed docs)

> Chạy: `./bench_mvcc 5000 1000 > results.csv`

| Workload | 2PL ops/sec | MVCC ops/sec | Winner | Note |
|----------|-------------|--------------|--------|------|
| W1: 100% reads | ~X | ~X | Tie | MVCC slight overhead từ tx create |
| W2: hot key writes | ~X | ~X | 2PL | MVCC aborts trên concurrent conflicts |
| W3: multi-key writes | ~X | ~X | Tie | MVCC có thêm version alloc overhead |
| W4: 50/50 mixed | ~X | ~X | MVCC | Readers không block writers |
| W5: long reader + writers | ~X | ~X | **MVCC** | Reader không bị block, major win |

*Actual numbers depend on hardware. Run `make bench` để đo thực tế.*

---

## Why PostgreSQL needs VACUUM, MySQL InnoDB doesn't (as much)

**PostgreSQL (MVCC, heap-based)**:
- Old versions (dead tuples) accumulate on-disk pages
- VACUUM scans, marks dead tuples free, updates visibility map
- AUTOVACUUM runs in background

**MySQL InnoDB (MVCC, undo log-based)**:
- Current version stored in-place (clustered index)
- Older versions in separate **undo log** (rollback segments)
- **Purge thread** continuously reclaims undo log records
- Effect tương tự VACUUM nhưng ở level undo log, không heap pages

**Hugo DB** dùng PostgreSQL-style (append-only heap):
- `mvcc_vacuum()` = simplified VACUUM
- Phase 6b sẽ add background vacuum thread

---

## Why distributed DBs (CockroachDB, TiDB, YugabyteDB) choose MVCC

1. **No distributed lock manager**: 2PL distributed → lock coordinator → bottleneck + SPOF
2. **Snapshot reads at any timestamp**: time-travel queries, consistent backups
3. **Serializable via SSI**: Serializable Snapshot Isolation không cần distributed locks
4. **Clock-based ordering**: HLC (Hybrid Logical Clock) provides distributed timestamps
5. **Replication compatibility**: MVCC version chain compatible với Raft log replay

---

## Anomalies still possible with Snapshot Isolation (Hugo DB)

### Write skew

```
T1: reads balance_A=100, balance_B=100
T2: reads balance_A=100, balance_B=100
T1: writes balance_A -= 200  (A = -100)
T2: writes balance_B -= 200  (B = -100)
Both commit: total = -200 (invariant violated!)
```

SI không detect read-write conflicts. Fix: Serializable Snapshot Isolation (SSI) — stretch goal.

Hugo DB SI is sufficient for most use cases. Application-level checks hoặc `FOR UPDATE` (future) có thể mitigate.
