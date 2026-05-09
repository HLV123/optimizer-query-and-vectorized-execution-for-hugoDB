# HugoDB — Báo cáo phát triển Vectorized Execution

> **Mục tiêu dự án:** Phát triển HugoDB theo hướng vectorized execution để đạt tốc độ truy vấn nhanh nhất có thể, không thay đổi API bên ngoài, không phá vỡ code hiện có.

---

## Phân tích điểm xuất phát

Trước khi bắt đầu, codebase HugoDB ở stage31 đang dùng mô hình **Volcano with materialization**: mỗi operator nhận `Document**`, xử lý row-by-row, trả về `Document**` mới. `phys_executor.c` là executor chính.

**5 bottleneck được xác định từ code thực tế:**

| # | Vấn đề | Vị trí trong code | Impact |
|---|--------|-------------------|--------|
| 1 | `doc_get_field()` là O(n) linked-list walk mỗi row | `executor_disk.c`, `phys_executor.c` | Rất cao — gọi trong mọi operation |
| 2 | Filter row-by-row với branch per condition | `pe_eval_condition()` | Cao — không vectorize được |
| 3 | `AggBucket` linked list, `calloc` mỗi group | `exec_hash_aggregate()` | Trung bình |
| 4 | `qsort` với global state + indirect callback | `pe_compare_docs()` | Trung bình |
| 5 | `ddb_read_doc()` = 1 `pread(4KB)` per document | `ddb_scan()` | **Cực cao** — chiếm 97.8% thời gian |

**Nguyên tắc thiết kế được chọn:**

> Làm việc trên `double[]` / `int[]` càng lâu càng tốt — chỉ rebuild `Document*` ở bước cuối cùng sau khi đã filter/sort/agg xong.

---

## Giai đoạn 1 — ColBatch + Vectorized Filter

### Làm gì

Tạo `col_batch.h/c` và `vec_filter.h/c` trong `src/vec/`.

**ColBatch** — extract tất cả fields cần thiết từ Document linked-list ra flat arrays một lần duy nhất khi scan:

- `double *num_data[MAX_COLS][N]` — numeric fields
- `char **str_data[MAX_COLS][N]` — string fields (pointer vào KVPair gốc, không copy)
- `uint8_t *null_mask[MAX_COLS][N]` — null flags
- `uint8_t *alive[N]` — bitmap, 1 = row chưa bị filter loại

Bug quan trọng tìm được: `v.str` trong `doc_get_field` là stack copy bị overwrite sau mỗi iteration — phải lấy pointer trực tiếp từ `KVPair` trong Document.

**vec_filter** — các loop đơn giản trên `double[]`, không branch, không function call:

```c
// branch-free, compiler autovectorize thành AVX2
for (int i = 0; i < n; i++)
    alive[i] &= (!null_mask[i]) & (col[i] > val);
```

Hỗ trợ đầy đủ: `=, !=, <, >, <=, >=` cho số; `=, !=, contains` cho string; `AND, OR, NOT, IN, EXISTS`.

### Thu được gì

6 test cases, tất cả pass trên Linux.

| Phép đo | Kết quả |
|---------|---------|
| Filter 100k rows (`score >= 50`) | **0.079ms** với `-O3 -march=native` |
| So với doc_get_field loop | Không có baseline cùng điều kiện, nhưng filter là O(n) thuần túy |

### Kết luận giai đoạn 1

`ColBatch` là nền tảng của toàn bộ pipeline. Sau bước extract một lần, tất cả operations tiếp theo làm việc trên integer/float arrays — compiler có thể autovectorize thành AVX2 instructions mà không cần viết intrinsics thủ công.

---

## Giai đoạn 2 — Vectorized Aggregation + Sort

### Làm gì

**`vec_agg.h/c`** — thay `AggBucket` linked list bằng open-addressing hash table trên flat arrays trong Arena:

- Không `calloc` trong aggregation loop
- Parallel accumulator arrays: `sum[]`, `cnt[]`, `min_[]`, `max_[]` — mỗi array indexed trực tiếp bằng slot
- Hỗ trợ COUNT, SUM, AVG, MIN, MAX với GROUP BY

**`vec_sort.h/c`** — sort trên `int32_t perm[]` (permutation index) thay vì sort `Document**`:

- Comparator so sánh `num_data[col][perm[a]]` vs `num_data[col][perm[b]]` — không pointer chase
- Không dùng global state (loại bỏ `g_pe_sort_head` thread-unsafe)
- Với LIMIT nhỏ: heap select top-k thay full sort (nhanh hơn khi k << n)
- Introsort: quicksort + heapsort fallback khi depth limit đạt

### Thu được gì

8 test cases, tất cả pass.

| Phép đo (N=100k, `-O3`) | vec | Ghi chú |
|-------------------------|-----|---------|
| GROUP BY 4 groups (agg) | 11ms | Open-addr hash trong arena |
| SORT score DESC | 16ms | Introsort trên perm[] |
| SORT + GROUP BY total | 27ms | Combined |

### Kết luận giai đoạn 2

Sort trên `perm[]` tránh được việc move `Document*` (424 bytes/struct) — chỉ move `int32_t` (4 bytes). GROUP BY với open-addressing hash table loại bỏ hoàn toàn `malloc` trong inner loop. Cả hai đều fit trong L1/L2 cache tốt hơn model cũ.

---

## Giai đoạn 3 — vec_exec: Pipeline Executor

### Làm gì

**`vec_exec.h/c`** — drop-in replacement cho `phys_executor.c`, nhận `PhysicalPlan*` + `DiskDB*`, trả `HugoResult*` giống hệt API cũ.

Pipeline:
1. Walk `PhysicalPlan*` để thu thập tất cả fields cần thiết (field collector)
2. Scan DiskDB một lần → extract vào `ColBatch`
3. `vec_filter_apply()` trên ColBatch
4. Nếu có GROUP BY → `vec_agg_run()` → materialize
5. Nếu có ORDER BY → `vec_sort_full()` hoặc `vec_sort_topk()`
6. `vec_sort_apply_limit()` → slice perm[]
7. Rebuild `Document*` chỉ với rows còn lại → `HugoResult`

**Wire vào `executor_disk.c`:** chỉ thay 1 dòng:
```c
// Trước:  int rc = phys_exec_run(db, plan, r, arena);
// Sau:    int rc = vec_exec_run(db, plan, r, arena);
```

**Makefile:** thêm `make vec_test`, `make vec_bench`, `make hugodb`.

### Thu được gì

Benchmark thuần execution (no disk I/O) vs row model:

| Query | N=100k vec | N=100k row | Speedup |
|-------|-----------|-----------|---------|
| Filter | 0.09ms | 9.2ms | **~100x** |
| SUM | 0.16ms | 8.9ms | **~55x** |
| GROUP BY | 9.1ms | 15.8ms | 1.7x |
| SORT | 15.5ms | 88.7ms | 5.8x |

### Kết luận giai đoạn 3

Filter và SUM đạt speedup lớn nhất (55–100x) vì compiler autovectorize loop `double[]` thành AVX2 — xử lý 4 doubles/cycle thay vì 1. GROUP BY ít hơn (1.7x) vì string hash (`strcmp`) không vectorize được. Sort cải thiện rõ (5.8x) nhờ cache-friendly perm[].

---

## Giai đoạn 4 — String Interning + Scan Cache

### Làm gì

**`vec_str_intern.h/c`** — thay `strcmp` per row trong GROUP BY inner loop bằng intern pass:

1. Pass 1: map từng string unique → `int32_t id` (open-addressing hashtable, 1 lần)
2. Pass 2: accumulate chỉ dùng `id[i]` làm index — không hash, không strcmp

**`vec_scan_cache.h/c`** — cache `Document*` array in-memory theo collection:

- Key: collection name
- Invalidation: fingerprint = `count XOR next_id` — tự động stale khi insert/update/delete
- Lifetime: docs sống trong heap, độc lập với DiskDB open/close
- Wire: `executor_disk.c` gọi `scan_cache_invalidate()` sau mỗi write operation

**`vec_agg_run_fast()`** — phiên bản agg dùng intern:

```c
// Thay vì:  find_or_create_str(t, str_col[i])  mỗi row
// Dùng:     slot = ids[i]  — direct index, không hash
```

### Thu được gì

Đo tác động intern (N=100k, 4 groups):

| Method | Time | vs hash |
|--------|------|---------|
| Hash per row | 12ms | baseline |
| Intern + direct index | 9ms | **1.3x** nhanh hơn |

Đo tác động scan cache (N=30k, same query repeated):

| Mode | Time/query |
|------|------------|
| `phys_exec` (no cache) | ~375ms |
| `vec_exec` cold (lần đầu) | ~428ms |
| `vec_exec` warm (có cache) | **6–7ms** |
| **Speedup warm** | **~60x** |

### Kết luận giai đoạn 4

String interning cho thấy cải tiến khiêm tốn (1.3x) vì với ít groups, hash table đã fit trong L1 cache. Impact thực sự lớn hơn khi số unique groups lớn (>256). Scan cache là cải tiến **có impact lớn nhất** trong toàn bộ dự án — warm query gần như loại bỏ hoàn toàn disk I/O, đây là bottleneck chiếm 97.8% thời gian.

---

## Giai đoạn 5 — Bulk Scan: Loại bỏ N syscalls

### Làm gì

**Phân tích bottleneck cold scan:**

```
Stage breakdown (N=20000):
  disk read+clone : 246.91 ms  (97.8%)
  col extract     :   4.12 ms   (1.6%)
  vec_filter      :   0.02 ms   (0.0%)
  vec_sort        :   1.30 ms   (0.5%)
```

97.8% thời gian là disk I/O — mỗi `ddb_read_doc()` = 1 `pread(4096 bytes)` riêng lẻ. 30k docs = 30k syscalls.

**`vec_bulk_scan.h/c`** — thay N lần `pread(4KB)` bằng 1 lần đọc toàn bộ range:

1. Tìm `pid_min`, `pid_max` từ `doc_page_ids[]`
2. `hugo_read(file, buf, n_pages × 4096, pid_min × 4096)` — 1 syscall
3. Deserialize tất cả documents từ memory buffer
4. `__builtin_prefetch` cho page tiếp theo trong deserialization loop

Wire vào `vec_scan_cache.c`: thay manual `ddb_read_doc` loop bằng `vec_bulk_scan()`.

### Thu được gì

| Dataset | phys_exec (per query) | vec cold (bulk) | vec warm (cache) |
|---------|----------------------|-----------------|------------------|
| N=5k | 62ms | **8ms (7.6x)** | 0.77ms (79x) |
| N=20k | 247ms | **79ms (3.1x)** | 5.5ms (45x) |
| N=50k | 646ms | **259ms (2.5x)** | 19ms (33x) |

**Profile cold scan sau bulk:**

| Stage | Before bulk | After bulk |
|-------|-------------|------------|
| Disk I/O | 30k × pread | 1 × pread |
| N=50k cold | ~444ms | ~259ms |
| Improvement | — | **1.7x** |

Giới hạn: cold scan bị giới hạn bởi disk throughput vật lý (~1GB/s). Trên environment này (1 CPU core), parallel scan không có lợi.

### Kết luận giai đoạn 5

Bulk read giảm syscall overhead từ O(N) xuống O(1). Cải thiện cold scan 1.7–6x tùy dataset size. Tuy nhiên bottleneck thực sự sau bulk là **deserialization** và **disk throughput** — không phải số syscalls. Warm cache vẫn là kịch bản quan trọng nhất trong thực tế (workload read-heavy).

---

## Giai đoạn 6 — Benchmark Hoàn Chỉnh

### Làm gì

**`bench/bench_final.c`** — executable tự chạy, đo 3 phần:

- **Part 1:** Pure execution (no disk) — vec vs row model
- **Part 2:** Full pipeline (DiskDB) — phys vs vec cold vs vec warm
- **Part 3:** Summary speedup table

Trong quá trình build benchmark, phát hiện và fix **bug nghiêm trọng** trong `vec_exec.c`: AGG early-return path thiếu check `owned_raw` trước khi `doc_free` → double-free khi scan cache active. Phát hiện qua AddressSanitizer (`-fsanitize=address`).

### Kết quả benchmark (gcc -O3 -march=native, Intel Xeon 2.8GHz)

**Part 1 — Pure execution:**

| Query | N=5k speedup | N=20k speedup | N=50k speedup |
|-------|:---:|:---:|:---:|
| Filter | 72x | 136x | **360x** |
| Sort + limit 10 | 6x | 13x | 25x |
| GROUP BY + SUM | 0.5x | 1.6x | 3.4x |
| Combined | 1.9x | 2.2x | 3.9x |

*GROUP BY N=5k âm là do setup overhead của hash table lớn hơn benefit khi N nhỏ.*

**Part 2 — Full pipeline:**

| Mode | N=5k | N=20k | N=50k |
|------|:---:|:---:|:---:|
| phys_exec (baseline) | 62ms | 247ms | 646ms |
| vec cold (bulk scan) | 8ms | 79ms | 259ms |
| vec warm (cache) | **0.3–0.9ms** | **1.2–6ms** | **6–20ms** |
| **Warm speedup** | **73–212x** | **41–211x** | **34–113x** |

---

## Kết luận cuối cùng

### Về kiến trúc

Hướng vectorized execution đã được triển khai thành công theo 3 tầng độc lập, mỗi tầng giải quyết một bottleneck khác nhau:

1. **Execution layer** (`col_batch`, `vec_filter`, `vec_agg`, `vec_sort`) — loại bỏ pointer-chasing trong inner loops, cho phép compiler autovectorize thành SIMD instructions
2. **I/O layer** (`vec_bulk_scan`) — giảm cold scan từ N syscalls xuống 1 syscall
3. **Cache layer** (`vec_scan_cache`) — loại bỏ hoàn toàn disk I/O cho warm queries

Ba tầng này **cộng hưởng**: execution nhanh không có ý nghĩa nếu I/O vẫn chiếm 97% thời gian. Bulk scan + scan cache giải quyết I/O, sau đó execution speedup mới phát huy tác dụng.

### Về benchmark

**Warm query (kịch bản thực tế nhất — workload read-heavy):** speedup **34–212x** so với `phys_exec` baseline, tùy query type và dataset size. Sort + limit đạt speedup cao nhất (212x) vì topk heap select + cache hit.

**Cold query (lần đầu mở DB):** speedup **2.5–8x**. Giới hạn bởi disk throughput vật lý, không phải CPU. Trên SSD NVMe multi-queue, speedup sẽ cao hơn với parallel I/O.

**Pure execution (no disk):** Filter đạt **360x** ở N=50k — đây là con số lý thuyết khi data đã nằm trong RAM (sau warm-up). Thực tế phản ánh mức độ SIMD utilization của compiler với `-O3 -march=native`.

### Về phương pháp

- **Không thay API** — `vec_exec_run` là drop-in replacement, CLI `hugodb` chạy bình thường
- **Non-invasive** — chỉ thêm `src/vec/`, chỉnh 2 dòng trong `executor_disk.c`
- **Backward compatible** — khi optimizer tắt hoặc query không phải FUNDEN/GOMAIL, pipeline cũ vẫn chạy
- **Cross-platform** — Makefile hỗ trợ cả `gcc` (Linux/macOS) và `cl.exe /O2 /arch:AVX2` (Windows MSVC)
- **Bug được tìm ra qua test/ASAN** — double-free trong AGG path sẽ không bao giờ được phát hiện nếu không có test suite + ASAN build

### Bottleneck còn lại

Sau tất cả các tối ưu, bottleneck duy nhất còn lại là **cold scan trên file lớn** — giới hạn hoàn toàn bởi disk throughput. Hướng cải tiến tiếp theo nếu cần:
- **Parallel cold scan** với `pthread` khi chạy trên multi-core + NVMe
- **Memory-mapped I/O** (`mmap`) thay `pread` để OS quản lý page cache tự động
- **KVPair pool allocator** trong deserialize để giảm `calloc` overhead (~8ms cho N=30k)

---

*Dự án: HugoDB Vectorized Execution | Platform: Linux x86-64 | Compiler: gcc 13 -O3 -march=native*
