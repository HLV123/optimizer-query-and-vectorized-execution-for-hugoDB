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
    LIBS   =
endif

CORE_SRC  = src/core/checksum.c $(IO_SRC) src/core/page.c
BTREE_SRC = src/core/btree.c
DBTREE_SRC = src/core/dbtree.c
BP_SRC = src/core/buffer_pool.c
WAL_SRC = src/core/wal.c
LM_SRC = src/core/lock_manager.c
QUERY_SRC = src/query/tokenizer.c src/query/parser.c
EXEC_SRC = src/query/executor.c src/core/collection.c
DDB_SRC = src/core/disk_db.c src/core/executor_disk.c src/core/wal.c
HTTP_SRC = src/core/http_server.c src/core/hugo_api.c
BULK_SRC = src/core/bulk_import.c src/core/batch_runner.c

# ===== Stage 3: MVCC sources =====
MVCC_SRC = src/core/ts_oracle.c \
           src/core/mvcc_tx.c \
           src/core/doc_version.c \
           src/core/mvcc_read.c \
           src/core/mvcc_write.c \
           src/core/mvcc_recovery.c \
           src/core/mvcc_vacuum.c

test_phase1$(EXE): tests/test_phase1.c $(CORE_SRC)
	$(CC) $(CFLAGS) $^ -o $@

test_phase2$(EXE): tests/test_phase2.c $(BTREE_SRC)
	$(CC) $(CFLAGS) $^ -o $@

test_phase2_stress$(EXE): tests/test_phase2_stress.c $(BTREE_SRC)
	$(CC) $(CFLAGS) $^ -o $@

test_phase2b$(EXE): tests/test_phase2b.c $(CORE_SRC) $(BTREE_SRC) $(DBTREE_SRC)
	$(CC) $(CFLAGS) $^ -o $@

test_phase3$(EXE): tests/test_phase3.c $(CORE_SRC) $(BP_SRC)
	$(CC) $(CFLAGS) $^ -o $@

test_phase4$(EXE): tests/test_phase4.c $(CORE_SRC) $(WAL_SRC)
	$(CC) $(CFLAGS) $^ -o $@

test_phase4_crash$(EXE): tests/test_phase4_crash.c $(CORE_SRC) $(WAL_SRC)
	$(CC) $(CFLAGS) $^ -o $@

test_phase5$(EXE): tests/test_phase5.c $(LM_SRC)
	$(CC) $(CFLAGS) $^ -o $@

test_phase6$(EXE): tests/test_phase6.c $(QUERY_SRC)
	$(CC) $(CFLAGS) -I src/query $^ -o $@

test_phase7$(EXE): tests/test_phase7.c $(QUERY_SRC) $(EXEC_SRC)
	$(CC) $(CFLAGS) -I src/query -I src/core $^ -o $@

hugo$(EXE): src/cli/main.c $(QUERY_SRC) $(EXEC_SRC)
	$(CC) $(CFLAGS) -I src/query -I src/core $^ -o $@

test_phase8$(EXE): tests/test_phase8.c $(CORE_SRC) $(DDB_SRC) src/core/collection.c src/query/executor.c
	$(CC) $(CFLAGS) -I src/query -I src/core $^ -o $@

test_phase8_exec$(EXE): tests/test_phase8_exec.c $(CORE_SRC) $(QUERY_SRC) $(EXEC_SRC) $(DDB_SRC)
	$(CC) $(CFLAGS) -I src/query -I src/core $^ -o $@

test_phase8b$(EXE): tests/test_phase8b.c $(CORE_SRC) $(QUERY_SRC) $(EXEC_SRC) $(DDB_SRC)
	$(CC) $(CFLAGS) -I src/query -I src/core $^ -o $@

hugo_disk$(EXE): src/cli/main_disk.c $(CORE_SRC) $(QUERY_SRC) $(EXEC_SRC) $(DDB_SRC) $(BULK_SRC) src/core/hugo_api.c
	$(CC) $(CFLAGS) -I src/query -I src/core $^ -o $@

test_phase9$(EXE): tests/test_phase9.c $(CORE_SRC) $(QUERY_SRC) $(EXEC_SRC) $(DDB_SRC) $(HTTP_SRC)
	$(CC) $(CFLAGS) -I src/query -I src/core $^ -o $@ $(LIBS)

hugo_serve$(EXE): src/cli/hugo_serve.c $(CORE_SRC) $(QUERY_SRC) $(EXEC_SRC) $(DDB_SRC) $(HTTP_SRC) $(BULK_SRC)
	$(CC) $(CFLAGS) -I src/query -I src/core $^ -o $@ $(LIBS)

test_phase10$(EXE): tests/test_phase10.c $(CORE_SRC) $(QUERY_SRC) $(EXEC_SRC) $(DDB_SRC) $(HTTP_SRC) $(BULK_SRC)
	$(CC) $(CFLAGS) -I src/query -I src/core $^ -o $@ $(LIBS)

test_stage2$(EXE): tests/test_stage2.c $(QUERY_SRC) $(EXEC_SRC)
	$(CC) $(CFLAGS) -I src/query -I src/core $^ -o $@

# ===== Stage 3: MVCC test =====
test_mvcc_phase3$(EXE): tests/test_mvcc_phase3.c \
    $(CORE_SRC) $(QUERY_SRC) $(EXEC_SRC) $(DDB_SRC) $(MVCC_SRC)
	$(CC) $(CFLAGS) -I src/query -I src/core $^ -o $@

test_mvcc_phase5$(EXE): tests/test_mvcc_phase5.c \
    $(CORE_SRC) $(QUERY_SRC) $(EXEC_SRC) $(DDB_SRC) $(MVCC_SRC)
	$(CC) $(CFLAGS) -I src/query -I src/core $^ -o $@

test_mvcc_phase6$(EXE): tests/test_mvcc_phase6.c \
    $(CORE_SRC) $(QUERY_SRC) $(EXEC_SRC) $(DDB_SRC) $(MVCC_SRC)
	$(CC) $(CFLAGS) -I src/query -I src/core $^ -o $@

bench_mvcc$(EXE): bench/bench_mvcc.c \
    $(CORE_SRC) $(QUERY_SRC) $(EXEC_SRC) $(DDB_SRC) $(MVCC_SRC)
	$(CC) $(CFLAGS) -I src/query -I src/core $^ -o $@

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

test_mvcc3: test_mvcc_phase3$(EXE)
	./test_mvcc_phase3$(EXE)

test_mvcc5: test_mvcc_phase5$(EXE)
	./test_mvcc_phase5$(EXE)

test_mvcc6: test_mvcc_phase6$(EXE)
	./test_mvcc_phase6$(EXE)

bench: bench_mvcc$(EXE)
	./bench_mvcc$(EXE) 5000 500

cli: hugo$(EXE)
	@echo "Built hugo$(EXE) (in-memory MVP). Run: ./hugo$(EXE)"

cli_disk: hugo_disk$(EXE)
	@echo "Built hugo_disk$(EXE) (disk-backed). Run: ./hugo_disk$(EXE) mydb.hugo"

serve: hugo_serve$(EXE)
	@echo "Built hugo_serve$(EXE) (web UI). Run: ./hugo_serve$(EXE) [db.hugo] [port]"
	@echo "Then open http://127.0.0.1:7777"

test: test1 test2 stress2 test2b test3 test4 crash4 test5 test6 test7 test8 test8exec test8b test9 test10 test_mvcc3 test_mvcc5 test_mvcc6

clean:
	$(RM) test_phase1$(EXE) test_phase2$(EXE) test_phase2_stress$(EXE) test_phase2b$(EXE) test_phase3$(EXE) test_phase4$(EXE) test_phase4_crash$(EXE) test_phase5$(EXE) test_phase6$(EXE) test_phase7$(EXE) test_phase8$(EXE) test_phase8_exec$(EXE) test_phase8b$(EXE) test_phase9$(EXE) test_phase10$(EXE) test_stage2$(EXE) test_mvcc_phase3$(EXE) test_mvcc_phase5$(EXE) test_mvcc_phase6$(EXE) bench_mvcc$(EXE) hugo$(EXE) hugo_disk$(EXE) hugo_serve$(EXE) *.o *.hugo *.hugolog

.PHONY: test test1 test2 stress2 test2b test3 test4 crash4 test5 test6 test7 test8 test8exec test8b test9 test10 test_s2 test_mvcc3 test_mvcc5 test_mvcc6 bench cli cli_disk serve clean
