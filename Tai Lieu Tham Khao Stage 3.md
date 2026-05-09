## Tài liệu tham khảo cho stage 3 hướng 1

Đọc trước:

1. **"Access Path Selection in a Relational Database Management System" (Selinger et al. 1979)** — System R paper. **20 trang, đọc trước Phase 7**. Paper mở đầu cost-based optimization.

2. **"Query Optimization" chapter of Database System Concepts (Silberschatz, Korth, Sudarshan)** — textbook reference, chapter 16. Accessible.

3. **"Query Optimizers: Time to Rethink the Contract?" (Cao et al. VLDB 2018)** — modern perspective trên cardinality estimation issues.

4. **PostgreSQL planner documentation** (postgresql.org/docs/current/planner-optimizer.html) — overview of production planner.

5. **"How Good Are Query Optimizers, Really?" (Leis et al. 2015)** — empirical analysis, cardinality estimation often wrong.

6. **Apache Calcite** (calcite.apache.org) — reference open source optimizer framework. Java-based, architecture pedagogical.

Existing codebases để đọc (không copy):
- SQLite optimizer (`src/where.c`, `src/whereexpr.c`) — simpler, C code
- PostgreSQL `src/backend/optimizer/` — production quality nhưng dense
- DuckDB optimizer (C++) — modern, analytical focus
- Apache Calcite — Java, framework approach


## Tài liệu tham khảo cho stage 3 hướng 2

Đọc trước khi bắt đầu:

1. **PostgreSQL MVCC documentation** (postgresql.org/docs/current/mvcc.html) — reference cho semantic chính xác
2. **"An Empirical Evaluation of In-Memory Multi-Version Concurrency Control" (Wu et al., VLDB 2017)** — 9 trang, compare multiple MVCC implementations, insight về design choices
3. **"Serializable Snapshot Isolation in PostgreSQL" (Ports & Grittner 2012)** — cho stretch goal SSI
4. **"Concurrency Control in Distributed Database Systems" (Bernstein & Goodman 1981)** — textbook foundations, chapter on MVCC
5. **Transaction Processing book (Gray & Reuter 1993)** — reference khi stuck
6. **Existing MVCC codebases (READ ONLY, không copy)**:
   - PostgreSQL `src/backend/access/heap/heapam_visibility.c` — visibility logic reference
   - BadgerDB (Go) — simpler MVCC implementation
   - SiloDB — OCC với MVCC variant


## Tài liệu tham khảo cho stage 3 hướng 3

Đọc trước khi bắt đầu:

1. **LevelDB documentation** (github.com/google/leveldb/blob/main/doc/index.md) — concise, ~30 trang. Reference chính.
2. **"The Log-Structured Merge-Tree" (O'Neil et al. 1996)** — paper gốc LSM, 30 trang. Foundation theory.
3. **RocksDB Wiki** (github.com/facebook/rocksdb/wiki) — production LSM details. Đọc selectively.
4. **"Constructing and Analyzing the LSM Compaction Design Space" (VLDB 2018)** — hệ thống hóa compaction strategies.
5. **"Bloom Filters - the math" (llimllib.github.io/bloomfilter-tutorial/)** — hiểu bloom math.
6. **"Skiplist: A Probabilistic Alternative to Balanced Trees" (Pugh 1990)** — skiplist paper.

Existing LSM codebases (READ ONLY, không copy):
- LevelDB (C++): smallest production LSM, ~20K lines. Best learning reference.
- BadgerDB (Go): cleaner Go implementation
- Pebble (Go): RocksDB-compatible, used by CockroachDB
- TiKV's RocksDB usage patterns (Rust)