# HUGO DB Makefile
# Linux/macOS:        make test
# Windows MinGW:      mingw32-make test
#
# Targets:
#   test1   — Phase 1 (foundation: page manager + CRC)
#   test2   — Phase 2 (B-tree property-based, ~16k ops)
#   stress2 — Phase 2 stress (1 triệu ops)
#   test    — chạy tất cả

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=c11

ifeq ($(OS),Windows_NT)
    IO_SRC = src/core/hugo_io_win.c
    EXE    = .exe
    RM     = del /Q
    LIBS   = -lws2_32
else
    IO_SRC = src/core/hugo_io_posix.c
    EXE    =
    RM     = rm -f
    CFLAGS += -D_POSIX_C_SOURCE=200809L
    LIBS   = -lm
endif

CORE_SRC  = src/core/checksum.c $(IO_SRC) src/core/page.c
BTREE_SRC = src/core/btree.c
DBTREE_SRC = src/core/dbtree.c
BP_SRC = src/core/buffer_pool.c
WAL_SRC = src/core/wal.c
LM_SRC = src/core/lock_manager.c
QUERY_SRC = src/query/tokenizer.c src/query/parser.c
EXEC_SRC = src/query/executor.c src/core/collection.c
OPT_SRC = src/core/optimizer/arena.c \
          src/core/optimizer/logical_plan.c \
          src/core/optimizer/statistics.c \
          src/core/optimizer/cost_model.c \
          src/core/optimizer/rules.c \
          src/core/optimizer/physical_plan.c \
          src/core/optimizer/join_order.c \
          src/core/optimizer/advanced_rules.c \
          src/core/optimizer/optimizer.c \
          src/core/phys_executor.c

DDB_SRC = src/core/disk_db.c src/core/executor_disk.c src/core/wal.c $(OPT_SRC)
HTTP_SRC = src/core/http_server.c src/core/hugo_api.c
BULK_SRC = src/core/bulk_import.c src/core/batch_runner.c

test_phase1$(EXE): tests/test_phase1.c $(CORE_SRC)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

test_phase2$(EXE): tests/test_phase2.c $(BTREE_SRC)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

test_phase2_stress$(EXE): tests/test_phase2_stress.c $(BTREE_SRC)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

test_phase2b$(EXE): tests/test_phase2b.c $(CORE_SRC) $(BTREE_SRC) $(DBTREE_SRC)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

test_phase3$(EXE): tests/test_phase3.c $(CORE_SRC) $(BP_SRC)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

test_phase4$(EXE): tests/test_phase4.c $(CORE_SRC) $(WAL_SRC)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

test_phase4_crash$(EXE): tests/test_phase4_crash.c $(CORE_SRC) $(WAL_SRC)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

test_phase5$(EXE): tests/test_phase5.c $(LM_SRC)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

test_phase6$(EXE): tests/test_phase6.c $(QUERY_SRC)
	$(CC) $(CFLAGS) -I src/query $^ -o $@ $(LIBS)

test_phase7$(EXE): tests/test_phase7.c $(QUERY_SRC) $(EXEC_SRC)
	$(CC) $(CFLAGS) -I src/query -I src/core $^ -o $@ $(LIBS)

hugo$(EXE): src/cli/main.c $(QUERY_SRC) $(EXEC_SRC)
	$(CC) $(CFLAGS) -I src/query -I src/core $^ -o $@ $(LIBS)

test_phase8$(EXE): tests/test_phase8.c $(CORE_SRC) $(DDB_SRC) src/core/collection.c src/query/executor.c
	$(CC) $(CFLAGS) -I src/query -I src/core $^ -o $@ $(LIBS)

test_phase8_exec$(EXE): tests/test_phase8_exec.c $(CORE_SRC) $(QUERY_SRC) $(EXEC_SRC) $(DDB_SRC)
	$(CC) $(CFLAGS) -I src/query -I src/core $^ -o $@ $(LIBS)

test_phase8b$(EXE): tests/test_phase8b.c $(CORE_SRC) $(QUERY_SRC) $(EXEC_SRC) $(DDB_SRC)
	$(CC) $(CFLAGS) -I src/query -I src/core $^ -o $@ $(LIBS)

hugo_disk$(EXE): src/cli/main_disk.c $(CORE_SRC) $(QUERY_SRC) $(EXEC_SRC) $(DDB_SRC) $(BULK_SRC) src/core/hugo_api.c
	$(CC) $(CFLAGS) -I src/query -I src/core $^ -o $@ $(LIBS)

test_phase9$(EXE): tests/test_phase9.c $(CORE_SRC) $(QUERY_SRC) $(EXEC_SRC) $(DDB_SRC) $(HTTP_SRC)
	$(CC) $(CFLAGS) -I src/query -I src/core -I src/core/optimizer $^ -o $@ $(LIBS)

hugo_serve$(EXE): src/cli/hugo_serve.c $(CORE_SRC) $(QUERY_SRC) $(EXEC_SRC) $(DDB_SRC) $(HTTP_SRC) $(BULK_SRC)
	$(CC) $(CFLAGS) -I src/query -I src/core -I src/core/optimizer $^ -o $@ $(LIBS)

test_phase10$(EXE): tests/test_phase10.c $(CORE_SRC) $(QUERY_SRC) $(EXEC_SRC) $(DDB_SRC) $(HTTP_SRC) $(BULK_SRC)
	$(CC) $(CFLAGS) -I src/query -I src/core -I src/core/optimizer $^ -o $@ $(LIBS)

test_stage2$(EXE): tests/test_stage2.c $(QUERY_SRC) $(EXEC_SRC)
	$(CC) $(CFLAGS) -I src/query -I src/core $^ -o $@ $(LIBS)

test1: test_phase1$(EXE)
	./test_phase1$(EXE)

test2: test_phase2$(EXE)
	./test_phase2$(EXE)

stress2: test_phase2_stress$(EXE)
	./test_phase2_stress$(EXE) 1000000 7

test2b: test_phase2b$(EXE)
	./test_phase2b$(EXE)

test3: test_phase3$(EXE)
	./test_phase3$(EXE)

test4: test_phase4$(EXE)
	./test_phase4$(EXE)

crash4: test_phase4_crash$(EXE)
	./test_phase4_crash$(EXE) 200 50

test5: test_phase5$(EXE)
	./test_phase5$(EXE)

test6: test_phase6$(EXE)
	./test_phase6$(EXE)

test7: test_phase7$(EXE)
	./test_phase7$(EXE)

test8: test_phase8$(EXE)
	./test_phase8$(EXE)

test8exec: test_phase8_exec$(EXE)
	./test_phase8_exec$(EXE)

test8b: test_phase8b$(EXE)
	./test_phase8b$(EXE)

test9: test_phase9$(EXE)
	./test_phase9$(EXE)

test10: test_phase10$(EXE)
	./test_phase10$(EXE)

test_s2: test_stage2$(EXE)
	./test_stage2$(EXE)

cli: hugo$(EXE)
	@echo "Built hugo$(EXE) (in-memory MVP). Run: ./hugo$(EXE)"

cli_disk: hugo_disk$(EXE)
	@echo "Built hugo_disk$(EXE) (disk-backed). Run: ./hugo_disk$(EXE) mydb.hugo"

serve: hugo_serve$(EXE)
	@echo "Built hugo_serve$(EXE) (web UI). Run: ./hugo_serve$(EXE) [db.hugo] [port]"
	@echo "Then open http://127.0.0.1:7777"

test: test1 test2 stress2 test2b test3 test4 crash4 test5 test6 test7 test8 test8exec test8b test9 test10

clean:
	$(RM) test_phase1$(EXE) test_phase2$(EXE) test_phase2_stress$(EXE) test_phase2b$(EXE) test_phase3$(EXE) test_phase4$(EXE) test_phase4_crash$(EXE) test_phase5$(EXE) test_phase6$(EXE) test_phase7$(EXE) test_phase8$(EXE) test_phase8_exec$(EXE) test_phase8b$(EXE) test_phase9$(EXE) test_phase10$(EXE) test_stage2$(EXE) hugo$(EXE) hugo_disk$(EXE) hugo_serve$(EXE) *.o *.hugo *.hugolog

.PHONY: test test1 test2 stress2 test2b test3 test4 crash4 test5 test6 test7 test8 test8exec test8b test9 test10 test_s2 cli cli_disk serve clean

# ===== Stage 3: Optimizer =====

ifeq ($(OS),Windows_NT)
    OPT_LIBS = -lm
else
    OPT_LIBS = -lm
endif

test_optimizer$(EXE): tests/test_optimizer.c $(CORE_SRC) $(QUERY_SRC) $(EXEC_SRC) $(DDB_SRC) $(OPT_SRC)
	$(CC) $(CFLAGS) -I src/query -I src/core -I src/core/optimizer $^ -o $@ $(OPT_LIBS)

test_opt: test_optimizer$(EXE)
	./test_optimizer$(EXE)

hugo_disk$(EXE): src/cli/main_disk.c $(CORE_SRC) $(QUERY_SRC) $(EXEC_SRC) $(DDB_SRC) $(BULK_SRC) src/core/hugo_api.c $(OPT_SRC)
	$(CC) $(CFLAGS) -I src/query -I src/core -I src/core/optimizer $^ -o $@ $(OPT_LIBS)

hugo_serve$(EXE): src/cli/hugo_serve.c $(CORE_SRC) $(QUERY_SRC) $(EXEC_SRC) $(DDB_SRC) $(HTTP_SRC) $(BULK_SRC) $(OPT_SRC)
	$(CC) $(CFLAGS) -I src/query -I src/core -I src/core/optimizer $^ -o $@ $(LIBS) $(OPT_LIBS)

# ===== Phase 9: Benchmark =====
bench_optimizer$(EXE): bench/bench_optimizer.c $(CORE_SRC) $(QUERY_SRC) $(EXEC_SRC) $(DDB_SRC)
	$(CC) $(CFLAGS) -I src/query -I src/core -I src/core/optimizer $^ -o $@ $(LIBS)

bench: bench_optimizer$(EXE)
	./bench_optimizer$(EXE)

# ===== Vectorized Execution (src/vec/) =====
#
# Linux:   make vec_test   (gcc -O3 -march=native)
#          make vec_bench  (pure execution benchmark)
#          make hugodb     (full CLI with vec_exec wired in)
# Windows: nmake /f Makefile vec_test
#          (hoặc dùng cl.exe — xem comment trong từng .c file)

VEC_CORE = src/vec/col_batch.c \
           src/vec/vec_filter.c \
           src/vec/vec_agg.c \
           src/vec/vec_sort.c \
           src/vec/vec_str_intern.c

VEC_SRC  = $(VEC_CORE) src/vec/vec_bulk_scan.c src/vec/vec_scan_cache.c src/vec/vec_exec.c

VEC_INC = -I src -I src/core -I src/query -I src/core/optimizer

ifeq ($(OS),Windows_NT)
    VEC_CFLAGS = /O2 /arch:AVX2 /std:c11
    VEC_CC     = cl
else
    VEC_CFLAGS = -O3 -march=native -std=c11 -Wall -D_POSIX_C_SOURCE=200809L
    VEC_CC     = gcc
endif

# --- vec unit tests ---
vec_test_col$(EXE): src/vec/test_col_batch.c $(VEC_CORE) \
                    src/core/collection.c src/core/optimizer/arena.c
	$(VEC_CC) $(VEC_CFLAGS) $(VEC_INC) $^ -o $@ -lm

vec_test_agg$(EXE): src/vec/test_vec_agg_sort.c $(VEC_CORE) \
                    src/core/collection.c src/core/optimizer/arena.c
	$(VEC_CC) $(VEC_CFLAGS) $(VEC_INC) $^ -o $@ -lm

vec_test_exec$(EXE): src/vec/test_vec_exec.c $(VEC_SRC) \
                     $(CORE_SRC) $(QUERY_SRC) $(EXEC_SRC) $(DDB_SRC) $(OPT_SRC)
	$(VEC_CC) $(VEC_CFLAGS) $(VEC_INC) -Dstrtok_s=strtok_r $^ -o $@ -lm

vec_test: vec_test_col$(EXE) vec_test_agg$(EXE) vec_test_exec$(EXE)
	./vec_test_col$(EXE)
	./vec_test_agg$(EXE)
	./vec_test_exec$(EXE)

# --- pure execution benchmark ---
vec_bench_bin$(EXE): src/vec/bench_pure.c $(VEC_CORE) \
                 src/core/collection.c src/core/optimizer/arena.c \
                 src/query/executor.c
	$(VEC_CC) $(VEC_CFLAGS) $(VEC_INC) -Dstrtok_s=strtok_r $^ -o $@ -lm

vec_bench: vec_bench_bin$(EXE)
	./vec_bench_bin$(EXE)

# --- full CLI with vectorized executor wired in ---
hugodb$(EXE): src/cli/main_disk.c \
              $(CORE_SRC) $(QUERY_SRC) $(EXEC_SRC) $(DDB_SRC) \
              $(BULK_SRC) src/core/hugo_api.c $(OPT_SRC) $(VEC_SRC)
	$(VEC_CC) $(VEC_CFLAGS) $(VEC_INC) -Dstrtok_s=strtok_r $^ -o $@ -lm
	@echo "Built hugodb$(EXE) — vec_exec_run() active for FUNDEN/GOMAIL"

.PHONY: vec_test vec_bench vec_bench_bin hugodb

# ===== Final vectorized benchmark =====
bench_final_bin$(EXE): bench/bench_final.c $(CORE_SRC) $(QUERY_SRC) $(EXEC_SRC) \
                      $(DDB_SRC) $(OPT_SRC) $(VEC_SRC)
	$(VEC_CC) $(VEC_CFLAGS) $(VEC_INC) -Dstrtok_s=strtok_r $^ -o $@ -lm
	@echo "Built bench_final_bin$(EXE)"

bench_final: bench_final_bin$(EXE)
	./bench_final_bin$(EXE)

.PHONY: bench_final bench_final_bin
