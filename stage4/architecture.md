# HugoDB Vectorized Execution — Kiến trúc

---

## Tổng quan: Query execution flow

```
HugoQL query string
        │
        ▼
  ┌─────────────┐
  │   Parser    │  tokenizer.c → parser.c → Query (AST)
  └──────┬──────┘
         │
         ▼
  ┌─────────────┐
  │  Optimizer  │  optimizer.c → PhysicalPlan*
  └──────┬──────┘
         │
         ▼
  ┌──────────────────────────────────────────────────┐
  │              executor_disk.c                     │
  │  FUNDEN/GOMAIL → vec_exec_run()                  │
  │  writes/DDL   → hugo_execute_disk() (unchanged)  │
  └──────┬───────────────────────────────────────────┘
         │
         ▼
  ┌──────────────────────────────────────────────────┐
  │              vec_exec.c  (src/vec/)              │
  │                                                  │
  │  1. Field collector  (walk PhysicalPlan)         │
  │  2. Scan             (vec_scan_cache / bulk)     │
  │  3. ColBatch extract (doc → flat arrays)         │
  │  4. vec_filter_apply (bitmap on double[])        │
  │  5. vec_agg_run_fast (intern + direct index)     │
  │  6. vec_sort_full / vec_sort_topk (perm[])       │
  │  7. Materialize      (clone surviving rows)      │
  └──────────────────────────────────────────────────┘
         │
         ▼
   HugoResult (Document*[])
```

---

## Cấu trúc thư mục `src/vec/`

```
src/vec/
├── col_batch.h / col_batch.c         ← tầng 1: data representation
├── vec_filter.h / vec_filter.c       ← tầng 1: bitmap filter
├── vec_agg.h / vec_agg.c             ← tầng 1: aggregation
├── vec_sort.h / vec_sort.c           ← tầng 1: sort
├── vec_str_intern.h / vec_str_intern.c  ← tầng 1: string interning
├── vec_bulk_scan.h / vec_bulk_scan.c    ← tầng 2: bulk I/O
├── vec_scan_cache.h / vec_scan_cache.c  ← tầng 3: in-memory cache
├── vec_exec.h / vec_exec.c              ← entry point (pipeline)
├── test_col_batch.c                  ← 6 unit tests
├── test_vec_agg_sort.c               ← 10 unit tests
├── test_vec_exec.c                   ← 2 integration tests
└── bench_pure.c                      ← pure execution benchmark
```

---

## Tầng 1 — Data Representation: ColBatch

### Sơ đồ cấu trúc

```
Document (linked list — row model, input)
  │
  │  col_batch_add_doc() — extract 1 lần, O(fields)
  ▼

ColBatch (flat arrays — column model, for execution)
┌─────────────────────────────────────────────────────┐
│  n_rows = 50000                                     │
│                                                     │
│  num_data[0]  →  [d0, d1, d2, ..., d49999]  double[]│  "score"
│  num_data[1]  →  [d0, d1, d2, ..., d49999]  double[]│  "age"
│  null_mask[0] →  [0,  0,  1,  ..., 0     ]  byte[]  │
│                                                     │
│  str_data[0]  →  [*s0, *s1, *s2, ..., *s49999]      │  "region"
│  null_mask[16]→  [0,   0,   0,  ..., 0    ]         │
│                                                     │
│  alive[]      →  [1, 1, 1, ..., 1]  byte[]          │  ← filter bitmap
│  perm[]       →  [i0, i1, i2, ..., ik]  int32[]     │  ← sort order
│  docs[]       →  [*d0, *d1, ..., *d49999]           │  borrowed ptrs
└─────────────────────────────────────────────────────┘
```

### Chi tiết

- `MAX_COLS = 32`, `MAX_ROWS = 200000`
- String columns: `char*` pointer trỏ thẳng vào `KVPair.value.str` trong Document gốc — zero-copy, không `strdup`
- Numeric null: `null_mask[col_idx][row]` — riêng biệt với numeric data
- String null: `null_mask[COL_BATCH_MAX_COLS/2 + col_idx][row]`
- `alive[]` và `perm[]` alloc từ `Arena*` — reset cùng query

---

## Tầng 1 — vec_filter: Bitmap Filter

### Sơ đồ hoạt động

```
Condition tree (from PhysicalPlan)
         COND_AND
        /        \
   COND_CMP    COND_CMP
  score > 500  region = "north"

         │
         │ vec_filter_apply(b, cond)  — recursive dispatch
         ▼

COND_CMP numeric (score > 500):
  for i in 0..N:
    alive[i] &= (!null[i]) & (num_data[0][i] > 500.0)
         ↑ branch-free, autovectorize → AVX2 (4 doubles/cycle)

COND_CMP string (region = "north"):
  for i in 0..N:
    alive[i] = alive[i] ? strcmp(str_data[0][i], "north")==0 : 0

Result: alive[] updated in-place, 0 = filtered out
```

### Chi tiết

- `AND`: apply cả 2 nhánh tuần tự — rows die sớm ở nhánh 1 không cần check nhánh 2
- `OR`: copy `alive[]` → apply left → restore → apply right → merge với `|`
- `NOT`: apply child → flip với `^= 1`
- `IN` / `NOT IN`: loop qua values list per row
- `EXISTS`: check `null_mask`
- `CONTAINS` (`$xau`): `strstr()` per row
- `vec_filter_compact()`: copy surviving rows về đầu arrays — dùng trước agg để shrink working set

---

## Tầng 1 — vec_agg: Open-Addressing Hash Table

### Sơ đồ cấu trúc

```
VecAggTable (trong Arena — zero malloc trong loop)
┌──────────────────────────────────────────────────┐
│  capacity = 4096 (power-of-2)                    │
│  n_groups = 4                                    │
│                                                  │
│  slot_used[]  →  [0,1,0,0,1,0,0,1,0,0,1,0,...]   │
│  key_str[]    →  [-, "north", -, -, "south", ...]│
│                                                  │
│  sum[0][]     →  [-, 12345.0, -, -, 9876.0, ...] │  SUM(score)
│  cnt[0][]     →  [-, 12500,   -, -, 12500,  ...] │  COUNT(*)
│  min_[0][]    →  [-, 0.0,     -, -, 0.0,    ...] │  MIN
│  max_[0][]    →  [-, 999.0,   -, -, 999.0,  ...] │  MAX
└──────────────────────────────────────────────────┘

vec_agg_run_fast():
  Pass 1: str_intern_build() → ids[row] = 0..3
  Pass 2: for i in 0..N: sum[0][ids[i]] += num_data[col][i]
              ↑ integer index — no hash, no strcmp → vectorizable
```

### Chi tiết

- `vec_agg_run()` — standard path: FNV hash + strcmp per row trong probe loop
- `vec_agg_run_fast()` — fast path: intern strings trước, accumulate dùng `int32_t id[]`
- `vec_agg_new()` tự chọn capacity = smallest power-of-2 ≥ `n_rows × 2`, cap ở `VEC_AGG_MAX_GROUPS = 65536`
- MIN/MAX init: `+DBL_MAX` / `-DBL_MAX` per slot
- `vec_agg_materialize()`: duyệt slots, tạo `Document*` cho mỗi group có `slot_used=1`
- Simple aggregates (no GROUP BY): `vec_agg_count_star()`, `sum()`, `avg()`, `min()`, `max()` — single-pass loops, fully vectorizable

---

## Tầng 1 — vec_sort: Introsort trên Permutation Array

### Sơ đồ hoạt động

```
Input: ColBatch với alive[] đã filter

build_perm():
  perm[] = [i | alive[i] == 1]  ← chỉ alive rows

introsort(perm[], n, depth_limit=2*log2(n)):
  ┌─────────────────────────────────────────────────┐
  │  n > 16: quicksort (median-of-3 pivot)          │
  │          │                                      │
  │          ├─ depth_limit == 0 → heapsort         │
  │          └─ recurse on smaller half             │
  │  n ≤ 16: insertion sort                         │
  └─────────────────────────────────────────────────┘

Comparator:
  cmp_rows(ctx, perm[a], perm[b]):
    va = num_data[col][perm[a]]    ← no pointer chase
    vb = num_data[col][perm[b]]    ← no pointer chase
    return desc ? vb-va : va-vb

vec_sort_topk(k):
  Build max-heap size k từ first k elements
  For remaining: if new < heap[0] → replace + sift_down
  → O(n log k), nhanh hơn full sort khi k << n

vec_sort_apply_limit(skip, limit):
  perm[skip..skip+limit-1] → valid result slice
```

### Chi tiết

- `SortCtx` được pass by pointer — không dùng global state, thread-safe
- Pre-resolve column indices trong `build_ctx()` — tránh `col_batch_find_col` trong comparator
- Null handling: null sorts last (ascending) / null sorts first (descending)
- Multi-key sort: loop qua `n_keys` trong comparator, short-circuit trên first non-equal key

---

## Tầng 1 — vec_str_intern: String Interning

### Sơ đồ

```
str_col[]   = ["north", "south", "north", "east", "south", ...]
                │        │         │         │       │
                ▼        ▼         ▼         ▼       ▼
InternHT (open-addressing, size=131072, load~0.5)
  hash → slot → gid

ids[]       = [0,       1,        0,        2,      1,      ...]
                ↑ int32, direct index into accumulator arrays

keys[]      = ["north", "south", "east"]   (n_unique = 3)
```

### Chi tiết

- Hash: FNV-1a trên string bytes
- `hash == 0` reserved cho empty slot → `if (h == 0) h = 1`
- Probe: linear probing
- `InternHT` alloc trên heap (131072 × ~24 bytes = ~3MB) → `free()` sau build
- `StrIntern` result alloc trong `Arena*` — sống cùng lifetime với query
- Null/missing rows → `ids[i] = -1` — được check và skip trong accumulate loop

---

## Tầng 2 — vec_bulk_scan: Bulk Page Read

### Sơ đồ

```
DiskColl.doc_page_ids[]  →  [0, 2, 3, 4, 5, ..., 30001]
                                 ↑              ↑
                               pid_min        pid_max

Before (N syscalls):
  for each doc_id: pread(fd, buf_4k, 4096, pid * 4096)  ← N=30000 calls

After (1 syscall):
  n_pages = pid_max - pid_min + 1
  hugo_read(file, buf, n_pages * 4096, pid_min * 4096)  ← 1 call
                         ↑
                    up to 512MB cap

Deserialize loop:
  for each doc_id:
    page_buf = buf + (doc_page_ids[id] - pid_min) * 4096
    __builtin_prefetch(next_page, 0, 1)  ← hide memory latency
    doc = deser_doc_from_page(page_buf)
```

### Chi tiết

- Page header offsets hardcoded: `PH_TYPE_OFF=4`, `PH_DATA_OFF=19` (= `HUGO_PAGE_HDR_SIZE`)
- Fallback khi `read_size > 512MB`: dùng loop `ddb_read_doc()` bình thường
- `deser_doc_from_page()` là bản copy của `disk_db.c::deserialize_doc()` — độc lập, không cần DiskDB internal
- Ownership: `vec_bulk_scan` malloc `Document**` → transfer cho `vec_scan_cache`

---

## Tầng 3 — vec_scan_cache: In-Memory Scan Cache

### Sơ đồ vòng đời

```
Query 1 (cold):                    Query 2 (warm):
  scan_cache_get()                   scan_cache_get()
       │                                  │
       ├─ valid=0 → miss                  ├─ valid=1
       │                                  │  fp match → HIT
       ▼                                  ▼
  vec_bulk_scan()              return e->docs (borrowed)
       │                                  │
       ▼                                  ▼
  e->docs = bulk_docs         vec_exec uses cached docs
  e->valid = 1                (owned_raw = 0, no free)
  e->fingerprint = fp

Write operation (insert/update/delete):
  executor_disk.c calls scan_cache_invalidate()
       │
       ▼
  entry_free() → doc_free(docs[i]) → free(docs) → valid=0
```

### Chi tiết

- `fingerprint = (count << 20) XOR next_id XOR (capacity * 31)`
- `SCAN_CACHE_MAX_COLLS = 64`, `SCAN_CACHE_MAX_DOCS = 200000`
- Eviction khi đầy: evict slot 0 + `memmove` (FIFO)
- `vec_get_scan_cache()` trả global singleton — khởi tạo lazy khi lần đầu gọi
- Thread safety: single-threaded, phù hợp với HugoDB MVP
- Write invalidation: `VERB_VIETINFO`, `VERB_COCHIN`, `VERB_DEMLET` trigger `scan_cache_invalidate()` sau khi `r->ok`

---

## vec_exec.c — Pipeline Entry Point

### Field Collector (walk PhysicalPlan)

```
PhysicalPlan tree:
  LIMIT
    └─ SORT (field="score")          → register "score" COL_TYPE_NUM
         └─ FILTER (score > 500)     → register "score" COL_TYPE_NUM
              └─ AGG (group="region") → register "region" COL_TYPE_STR
                   └─ SEQ_SCAN       →  (no fields to register)

Result: FieldCollector { fields=["score","region"], types=[NUM,STR], n=2 }
```

### Chi tiết `vec_exec_run()`

- `owned_raw = 0` khi docs borrowed từ cache → không `doc_free` sau query
- `owned_raw = 1` khi cache miss fallback → phải `doc_free` sau query
- Bug fix: AGG early-return path cũng phải check `owned_raw` (tìm qua ASAN)
- JOIN: fallback sang `do_hash_join()` (port từ `phys_executor.c`) — ColBatch chưa hỗ trợ 2 tables
- `scan_cache_destroy()` + `scan_cache_init()` trong benchmark cold path để tránh stale state

---

## Makefile targets

| Target | Mô tả |
|--------|-------|
| `make vec_test` | Build + chạy 3 test suites (24 tests) |
| `make vec_bench` | Build + chạy benchmark pure execution |
| `make hugodb` | Build CLI với `vec_exec_run` active |
| `make bench_final` | Build + chạy full benchmark (Parts 1–3) |

**VEC_CORE** (không cần DiskDB): `col_batch`, `vec_filter`, `vec_agg`, `vec_sort`, `vec_str_intern`

**VEC_SRC** (cần DiskDB): `VEC_CORE` + `vec_bulk_scan` + `vec_scan_cache` + `vec_exec`

---

## Tích hợp với codebase gốc

### Các file được sửa đổi (chỉ 1 file)

**`src/core/executor_disk.c`** — 3 thay đổi nhỏ:
1. `#include "../vec/vec_exec.h"` và `#include "../vec/vec_scan_cache.h"`
2. `phys_exec_run(...)` → `vec_exec_run(...)` (1 dòng)
3. `scan_cache_invalidate()` sau `VERB_VIETINFO`, `VERB_COCHIN`, `VERB_DEMLET`

### Các file không thay đổi

`phys_executor.c`, `optimizer.c`, `disk_db.c`, `page.c`, `wal.c`, `parser.c`, `tokenizer.c`, tất cả tests gốc — hoàn toàn nguyên vẹn.

---

*HugoDB vec/ — ~3600 lines C across 8 implementation files + 3 test files + 2 benchmark files*
