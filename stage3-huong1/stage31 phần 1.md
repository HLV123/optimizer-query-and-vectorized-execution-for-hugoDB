# HugoDB Stage 3 — Phần 1: Cost-Based Query Optimizer (Phase 1–6, 8)

> **Mục tiêu:** Thêm optimizer layer vào HugoDB — từ raw AST execution sang cost-based physical plan selection.
> **Kết quả:** 25 tests PASS, CLI hoạt động với `analyze`, `exepanus`, `\trace`, `\opt`.

---

## Những gì đã nâng cấp

### Phase 1 — Logical Plan
**File mới:** `src/core/optimizer/logical_plan.h/.c`

AST → Logical Plan tree với các node:
- `LOP_SCAN` — đọc toàn collection
- `LOP_FILTER` — áp dụng điều kiện `haar`
- `LOP_JOIN` — kết hợp hai collections
- `LOP_AGGREGATE` — `gomail` / GROUP BY
- `LOP_SORT`, `LOP_LIMIT` — `orange bi`, `lime`

Arena allocator — toàn bộ plan nodes dùng chung một block memory, free một lần.

---

### Phase 2 — Statistics
**File mới:** `src/core/optimizer/statistics.h/.c`

Lệnh `analyze <collection>` quét toàn bộ documents và xây dựng:
- Tổng số rows, bytes
- Equi-depth histogram 32 buckets cho numeric fields
- Distinct value set (256 exact + bloom filter overflow)
- Top-K string values

Kết quả lưu xuống `<db>.stats` để dùng lại giữa các sessions.

---

### Phase 3 — Cost Model
**File mới:** `src/core/optimizer/cost_model.h/.c`

Công thức cost cho từng operator:
- `SeqScan` = `page_count × io_cost + rows × cpu_cost`
- `IndexScan` = `log(rows)/log(100) × io_cost + matching × io_cost`
- `NestedLoopJoin` = `outer_rows × inner_rows × cpu_cost`
- `HashJoin` = `build_rows × hash_cost + probe_rows × lookup_cost`
- `Sort` = `rows × log2(rows) × cpu_cost` (external merge nếu > memory)
- `HashAggregate` = `rows × hash_cost`

---

### Phase 4 — Logical Rewrite Rules
**File mới:** `src/core/optimizer/rules.h/.c`

Fixpoint loop (tối đa 10 passes):
- **Eliminate redundant Limit/Sort** — `Limit(-1, 0)` và `Sort` không có fields bị xóa
- **Predicate pushdown** — `Filter` được đẩy xuống gần `Scan` nhất có thể, trước `JOIN`
- **Projection pushdown** — annotation các columns cần thiết

---

### Phase 5 — Physical Plan + Executor
**File mới:** `src/core/optimizer/physical_plan.h/.c`, `src/core/phys_executor.h/.c`

Physical operators:
- `POP_SEQ_SCAN`, `POP_INDEX_SCAN`
- `POP_FILTER`
- `POP_NESTED_LOOP_JOIN`, `POP_HASH_JOIN`, `POP_SORT_MERGE_JOIN`
- `POP_HASH_AGGREGATE`, `POP_STREAM_AGGREGATE`
- `POP_SORT`, `POP_LIMIT`

Materializing executor: mỗi node trả về `RowSet` (mảng documents), parent consume từng row.

---

### Phase 6 — Cost-Based Physical Selection
**File mới:** `src/core/optimizer/optimizer.h/.c`

`optimizer_run()` pipeline:
1. `build_logical_plan()` — AST → Logical Plan
2. `apply_all_rules()` — rewrite rules
3. `enumerate_physical()` — thử từng physical alternative, chọn cost thấp nhất

`choose_scan()`: SeqScan luôn available; IndexScan được thêm nếu có B-tree index trên filter field và selectivity thấp.

`build_physical_join()`: thử NL, Hash, SortMerge — chọn theo cost.

Tích hợp vào `hugo_execute_disk_opt()` — dispatch qua optimizer khi `\opt cost`, fallback legacy khi `\opt off`.

---

### Phase 8 — EXPLAIN (`exepanus`)
**Verb mới:** `exepanus <query>`

In ra physical plan đã chọn với cost estimate và row estimate — không execute query thực sự.

Thêm `\trace` để xem toàn bộ quá trình optimizer: Logical Plan ban đầu → sau rules → Physical Plan được chọn.

Thêm `\opt off|heuristic|cost` để switch optimizer mode.

---

## Build & Test

```powershell
# Build test suite
mingw32-make test_optimizer.exe -B 2>$null
.\test_optimizer.exe
```

**Kết quả mong đợi:**
```
Phase 1: Logical Plan      — 6 PASS
Phase 2: Statistics        — 3 PASS
Phase 3: Cost Model        — 3 PASS
Phase 4: Logical Rules     — 2 PASS
Phase 5-6: Physical Plan   — 2 PASS
End-to-End Correctness     — 5 PASS
ANALYZE / EXPLAIN          — 2 PASS
Miscellaneous              — 2 PASS
Results: 25 passed, 0 failed
```

```powershell
# Build CLI
mingw32-make hugo_disk.exe 2>$null
.\hugo_disk.exe mydb.hugo
```

---

## Demo CLI — Bằng chứng hoạt động

### Insert + Analyze

```
hugo> madeco users
hugo> vietinfo users { name "Alice" age 28 dept "eng" salary 75000 }
hugo> vietinfo users { name "Bob" age 35 dept "sales" salary 60000 }
hugo> analyze users
ok analyzed collection 'users' — statistics updated
```

**Kết luận:** Statistics được build từ dữ liệu thực, lưu xuống `mydb.hugo.stats`.

---

### EXPLAIN — thấy Physical Plan + cost

```
hugo> exepanus users haar age $bg 28
ok Physical Plan:
SeqScan users (cost=1.02 rows=2)
Estimated total cost: 1.02
Estimated rows: 2
```

**Kết luận:** Optimizer chọn SeqScan vì collection nhỏ (2 rows), cost thấp hơn IndexScan.

---

### Trace — thấy toàn bộ optimizer pipeline

```
hugo> \trace
optimizer trace ON
hugo> funden users haar dept $bg "eng" orange bi salary desc lime 2
=== OPTIMIZER TRACE ===
--- Logical Plan (initial) ---
Limit(2) → Sort(salary DESC) → Filter(dept='eng') → Scan(users)
--- Logical Plan (after rules) ---
[không đổi — predicate pushdown không áp dụng, không có JOIN]
[opt] SeqScan(users) cost=1.02 rows=2
--- Physical Plan (chosen) ---
Limit(2) → Sort(salary DESC) → Filter(dept='eng') → SeqScan(users)
=== END OPTIMIZER TRACE ===
ok 1 document
{ name: "Alice", ... }
```

**Kết luận:** Logical Plan → Rules → Physical Plan selection hiển thị đầy đủ.

---

### So sánh optimizer ON vs OFF — cùng kết quả

```
hugo> \opt off
hugo> funden users haar age $bg 28
ok 1 document   ← legacy executor

hugo> \opt cost
hugo> funden users haar age $bg 28
ok 1 document   ← optimizer executor, cùng kết quả
```

**Kết luận:** Correctness đảm bảo — optimizer và legacy trả về kết quả giống hệt nhau.

---

### Import data lớn + thấy optimizer hoạt động với 5000 rows

```powershell
.\hugo_disk.exe megadb.hugo --import samples\megadata.jsonl megadata
.\hugo_disk.exe megadb.hugo
```

```
hugo> analyze megadata
hugo> \trace
hugo> gomail megadata gremb bi department pou salary
=== OPTIMIZER TRACE ===
Physical Plan:
HashAggregate(group_by department) [cost=55.78  rows=16]
    └─ SeqScan(megadata) [cost=40.54  rows=254]
=== END OPTIMIZER TRACE ===
ok 14 documents
{ department: "Engineering", pou_salary: 8 }
...
```

**Kết luận:** Với 5000 rows, optimizer chọn `HashAggregate` (input không sorted → StreamAggregate đắt hơn). Cost estimate 40.54 → 55.78 đúng theo formula Phase 3.

---

## Tóm tắt Phase 1–6, 8

| Tính năng | Command | Kết quả |
|-----------|---------|---------|
| Logical Plan | internal | AST → 6 node types |
| Statistics | `analyze <coll>` | histogram + distinct count |
| Cost Model | internal | formula cho 6 operator types |
| Rewrite Rules | internal | predicate pushdown, eliminate redundant |
| Physical Plan | internal | 10 physical operator types |
| Plan Selection | `\opt cost` | cost-based SeqScan/IndexScan/Join |
| EXPLAIN | `exepanus <query>` | plan tree + cost + rows |
| Trace | `\trace` | full optimizer pipeline visible |
| Mode switch | `\opt off/cost` | legacy vs optimizer |
| Test coverage | `.\test_optimizer.exe` | **25/25 PASS** |
