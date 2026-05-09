# Hugo DB - Database System Development (Stage 3 & Stage 4)

Repository này chứa mã nguồn của **Stage 3** và **Stage 4** trong chuỗi dự án phát triển Database engine (Hugo DB) từ con số không bằng ngôn ngữ C. Mã nguồn của Stage 1 và Stage 2 được đặt ở một repo trước, tuy nhiên kiến trúc nền tảng của chúng vẫn được kế thừa và đóng vai trò quan trọng trong dự án này.

## Giới thiệu về Hugo DB

Hugo DB là một document database viết hoàn toàn bằng C. Nó có kiến trúc của một hệ quản trị cơ sở dữ liệu thực thụ với đầy đủ các thành phần như Storage Engine, Buffer Pool, Transaction Manager, Concurrency Control, WAL Recovery, Query Parser và Query Optimizer.

### Stage 1: Nền tảng Storage Engine 
Trong giai đoạn đầu, kiến trúc cơ bản đã được xây dựng và hỗ trợ khoảng 52% các từ khóa truy vấn:
- **Storage & Memory**: Page manager (với CRC32), B-tree (trên RAM và Disk), Buffer pool.
- **Transaction & Recovery**: Lock manager sử dụng giao thức 2PL (Two-Phase Locking), WAL crash recovery (ARIES 3-pass).
- **Query & Interface**: HugoQL parser, Executor cho các lệnh cơ bản (`funden`, `vietinfo`, `cochin`, `demlet`, `madeco`, `delco`, `skill`), CLI REPL disk-backed, Web GUI, và Bulk import JSONL, Batch query runner.
- Ở giai đoạn này, các tính năng phức tạp như Aggregation, Index, Join, Transaction scope được parse nhưng báo lỗi "NOT_IMPLEMENTED".

### Stage 2: Hoàn thiện tính năng truy vấn 
Giai đoạn 2 khắc phục toàn bộ các module còn thiếu, không còn lệnh nào trả về "NOT_IMPLEMENTED" hay "no-op":
- **Aggregation (`gomail`)**: Hỗ trợ nhiều hàm gom nhóm cùng lúc (`pou` - COUNT, `sep` - SUM, `awr` - AVG, `mie` - MIN, `maf` - MAX) kết hợp với `gremb bi` (GROUP BY).
- **Toán tử nâng cao**: `$tg` (IN list), `$ktg` (NOT IN), `$vnot` (NOT condition), mảng (`$don` array push, `$loi` array pull).
- **Dotted path**: Truy cập các field lồng nhau (vd: `doc_get_field("address.city")`).
- **Batch insert & Secondary indexes**: Chèn dữ liệu hàng loạt (`vietinfo collection [{doc1}, {doc2}]`) và xây dựng chỉ mục phụ (`madecoidu collection.field`) để tăng tốc.
- **Join & Query plan**: Hỗ trợ toán tử Join (`$rasoat alias from target on local_field $bg target.field`) và tính năng giải thích kế hoạch truy vấn (`exepanus` hiển thị SCAN hoặc INDEX SCAN).
- **Transactions & Multi-database**: Xử lý logic transaction scope thực thụ (`ginan`, `cometi`, `tulaberk`) và khả năng chuyển đổi qua lại giữa nhiều database (`usf dbname`).

---

## Nội dung của Repository này (Stage 3 & Stage 4)

Repository này bao gồm các cải tiến sâu về mặt hiệu năng và kiến trúc của Hugo DB. **Stage 3** được rẽ nhánh thành 3 hướng phát triển độc lập (dựa trên Stage 2), và **Stage 4** là bản nâng cấp tối ưu hóa cho hướng 1 của Stage 3.

### Stage 3: Phát triển kiến trúc nâng cao theo 3 hướng độc lập 

**1. Hướng 1 (Thư mục `stage3-huong1`): Cost-Based Query Optimizer (CBO)**
Đã hoàn thành CBO (33/33 tests pass) đưa trình tối ưu hóa truy vấn vào pipeline.
- **Cost Model & Statistics**: Thu thập thống kê dữ liệu để ước tính chi phí I/O và CPU cho các toán tử.
- **System R DP (Phase 7)**: Áp dụng thuật toán Dynamic Programming từ System R để tìm thứ tự JOIN tối ưu nhất (giúp plan chọn HashJoin rẻ hơn 30 lần so với Nested Loop Join).
- **Advanced Rules (Phase 10)**: Tối ưu hóa sâu hơn với Constant fold, Split predicates (tách AND cross-table), Push projections, Eliminate joins.
- **Cải tiến Parser**: Sửa lỗi nhận diện cú pháp `$rasoat` để JOIN hoạt động hoàn hảo.
- **Benchmark (Phase 9)**: Vượt qua toàn bộ các bài benchmark với correctness đạt 100% so với executor cũ.

**2. Hướng 2 (Thư mục `stage3-huong2`): Multi-Version Concurrency Control (MVCC)**
Đã retrofit thành công MVCC vào nền tảng (59/59 tests pass, zero regression với 2PL cũ). Database hỗ trợ chạy song song 2 mode: 2PL và MVCC.
- **Visibility & Snapshot Isolation (Phase 3)**: Xây dựng Version Chain với 5 luật Visibility, đảm bảo reader không bao giờ bị block bởi writer (lợi thế cực lớn của MVCC so với 2PL).
- **WAL Recovery & Crash Safety (Phase 5)**: Hệ thống khôi phục trạng thái chuẩn xác kể cả khi crash (bảo vệ các transaction chưa commit).
- **Garbage Collection - Vacuum (Phase 6)**: Thuật toán dọn dẹp (GC) xóa an toàn các phiên bản cũ không còn được transaction nào dùng tới.
- **Benchmark MVCC vs 2PL**: Chứng minh được sự đánh đổi giữa I/O overhead của việc ghi (MVCC chậm hơn khi single-writer) để đổi lấy throughput đọc/ghi song song cực cao.

**3. Hướng 3 (Thư mục `stage3-huong3`): LSM-Tree Storage Engine**
Đã implement thành công LSM-Tree chạy song song, độc lập hoàn toàn với B-Tree Engine cũ (vượt qua 105/105 queries trên giao diện).
- **Memtable & LSM WAL**: Sử dụng Skiplist 12 level trên RAM kết hợp WAL đảm bảo tính durability (fsync-per-write).
- **SSTable & Bloom Filter**: Builder tự động flush dữ liệu xuống các block SSTable kèm Bloom filter (~10 bits/key) hạn chế đọc đĩa thừa.
- **L0 & Leveled Compaction**: Hệ thống k-way merge (min-heap) cho L0 và tự động merge files ở L1-L3 (size ratio 10x) để loại bỏ duplicates và tombstones.
- **Hiệu năng cực khủng**: Test Benchmark chứng minh tốc độ Read của LSM nhanh hơn B-Tree (2PL) từ **2,000x đến 19,000x** (đạt hơn 2.7 triệu ops/s nhờ kiến trúc in-memory memtable).

### Stage 4 (Thư mục `stage4`): Vectorized Execution (Tối ưu hóa AVX2)

Được phát triển kế thừa từ mã nguồn của **Stage 3 - Hướng 1**, Stage 4 thiết kế lại hoàn toàn phần Executor sang cơ chế xử lý dữ liệu theo khối cột (Column-oriented batching) thay vì từng hàng (Row-by-row). Các report benchmark trên cả Windows (MSVC) và Linux (GCC) đều cho thấy kết quả đột phá.
- **Tầng Execution (ColBatch, Filter, Agg, Sort)**: Chuyển đổi Document thành các mảng dẹt (flat arrays). Nhờ thiết kế branch-free và tránh pointer-chasing, trình biên dịch tự động sinh ra các lệnh SIMD/AVX2. Tốc độ pure-execution (không I/O) cho phép filter nhanh hơn đến **360x**. Thay thế hash map chậm bằng Open-Addressing Hash Table (zero malloc trong inner loop) và Introsort trên permutation array `perm[]`.
- **Tầng I/O (Bulk Scan)**: Tăng tốc độ đọc lạnh (cold scan) bằng cách gộp N lời gọi hệ thống (syscalls `pread` nhỏ) thành 1 lần đọc range lớn duy nhất. Giảm thời gian cold scan từ 4 đến 8 lần.
- **Tầng Cache (Scan Cache)**: Maintain In-memory Document Scan Cache, tự động invalidate khi có cập nhật. Đối với các truy vấn nóng (warm queries), disk I/O được loại bỏ hoàn toàn, mang lại mức speedup đo được từ **34x đến 317x** tùy loại truy vấn.
- **Cross-Platform**: Tương thích tốt không phá vỡ API hiện tại. Mã nguồn dịch mượt mà trên cả Linux (`gcc -O3 -march=native`) và Windows (`cl /O2 /arch:AVX2`).
- **File** trong repo đều chạy cho window nếu muốn kiểm tra benchmark chính xác trên linux, hay đọc hướng dẫn thay đổi một số file ở stage 4
