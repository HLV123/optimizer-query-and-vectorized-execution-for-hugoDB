# Storage Engine Benchmark

## Build & Run
```
make bench_storage
./bench_storage
```

## Kết quả mẫu (10K ops, Linux, SSD)

| Workload       | LSM ops/s  | Ghi chú                        |
|----------------|------------|--------------------------------|
| seq_write      | ~1,800     | fsync mỗi record (durability)  |
| rand_write     | ~1,700     | tương đương seq (WAL sequential) |
| seq_read       | ~3,700,000 | từ memtable (in-memory)        |
| rand_read      | ~1,900,000 | bloom filter + index lookup    |

## Nhận xét

- **Write chậm** do `fsync` sau mỗi WAL record (strong durability).  
  Có thể tăng throughput bằng cách batch fsync.
- **Read rất nhanh** khi data còn trong memtable (in-memory path).
- Sau flush SSTable, read cần bloom check + disk read (~10-100K ops/s).
- **Write amplification**: mỗi key được ghi 2 lần (WAL + SSTable) 
  khi flush, rồi N lần khi compaction. Typical 5-10x.
- **Space amplification**: trước compaction cùng key có thể có 
  nhiều versions — thường 1.5-3x.

## So sánh B-tree vs LSM (lý thuyết)

| Metric          | B-tree   | LSM        |
|-----------------|----------|------------|
| Write latency   | cao      | thấp       |
| Read latency    | thấp     | trung bình |
| Write amp.      | 2-4x     | 5-15x      |
| Space amp.      | 1.5-2x   | 1.1-3x     |
| Use case        | OLTP read| write-heavy|
