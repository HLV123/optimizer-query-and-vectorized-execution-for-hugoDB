/* main_disk.c — HUGO DB CLI with DiskDB backend (Phase 8.a)
 *
 * Khác main.c: dùng DiskDB + executor_disk. Mỗi thao tác persist xuống
 * page file, có CRC32 check mọi page. Không cần \save thủ công nữa.
 */
#include "../query/tokenizer.h"
#include "../query/parser.h"
#include "../query/executor.h"
#include "../core/executor_disk.h"
#include "../core/disk_db.h"
#include "../core/collection.h"
#include "../core/bulk_import.h"
#include "../core/batch_runner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

static void print_banner(const char *path) {
    printf("HUGO DB — HugoQL CLI (disk-backed)\n");
    if (path) printf("Database: %s\n", path);
    printf("Type \\help for commands, \\q to quit.\n\n");
}

static void print_help(void) {
    printf("Meta commands:\n");
    printf("  \\help              show this help\n");
    printf("  \\q, exit, quit     quit (auto-close DB)\n");
    printf("  \\timing            toggle query timing\n");
    printf("  \\close             close current DB\n");
    printf("\nHugoQL verbs: funden, vietinfo, cochin, demlet, madeco, delco, skill\n");
    printf("Operators: $bg, $kc, $lh, $bh, $lhb, $bhb, $xau, $tntt, $vand, $vor, $quy\n");
    printf("Clauses:   haar, orange bi ... asc/desc, lime N, skopan N\n\n");
}

static void rstrip(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r' ||
                     s[n-1] == ' '  || s[n-1] == '\t')) s[--n] = 0;
}

static int is_blank(const char *s) {
    while (*s) { if (*s != ' ' && *s != '\t') return 0; s++; }
    return 1;
}

int main(int argc, char **argv) {
#ifdef _WIN32
    SetConsoleOutputCP(65001);
#endif

    /* Parse CLI args:
     *   hugo_disk <db.hugo>
     *   hugo_disk <db.hugo> --import <file.jsonl> <coll>
     *   hugo_disk <db.hugo> --run <file.hugoql>
     */
    const char *db_path = (argc >= 2) ? argv[1] : "hugo.hugo";
    const char *import_file = NULL;
    const char *import_coll = NULL;
    const char *run_file = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--import") == 0 && i + 2 < argc) {
            import_file = argv[i+1];
            import_coll = argv[i+2];
            i += 2;
        } else if (strcmp(argv[i], "--run") == 0 && i + 1 < argc) {
            run_file = argv[i+1];
            i += 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: hugo_disk <db.hugo> [--import <file.jsonl> <coll>] [--run <file.hugoql>]\n");
            printf("  (no flags) — enter interactive REPL\n");
            printf("  --import   — bulk load JSONL file into collection\n");
            printf("  --run      — execute HugoQL queries from file with timing\n");
            return 0;
        }
    }

    print_banner(db_path);

    DiskDB db;
    int created_new = 0;
    if (ddb_open(&db, db_path) != 0) {
        if (ddb_create(&db, "default", db_path) != 0) {
            printf("err FATAL \"cannot create/open %s\"\n", db_path);
            return 1;
        }
        created_new = 1;
        printf("(created new database)\n\n");
    } else {
        printf("(opened existing database — %d collection(s))\n\n", db.n_colls);
    }
    (void)created_new;

    /* Non-interactive modes */
    if (import_file && import_coll) {
        printf("Importing %s → collection '%s'...\n", import_file, import_coll);
        BulkStats st;
        int rc = bulk_import_file(&db, import_coll, import_file, &st);
        if (rc != 0) {
            printf("err IMPORT_FAILED (rc=%d)\n", rc);
        } else {
            printf("ok imported %llu/%llu lines in %.3fs (%.0f docs/s, %llu parse_err, %llu insert_err)\n",
                   (unsigned long long)st.docs_inserted,
                   (unsigned long long)st.lines_read,
                   st.elapsed_sec,
                   st.elapsed_sec > 0 ? st.docs_inserted / st.elapsed_sec : 0,
                   (unsigned long long)st.parse_errors,
                   (unsigned long long)st.insert_errors);
        }
    }

    if (run_file) {
        printf("Running queries from %s...\n", run_file);
        BatchStats bs;
        int rc = batch_run_file(&db, run_file, 1 /*verbose*/, stdout, &bs);
        if (rc != 0) {
            printf("err RUN_FAILED (rc=%d)\n", rc);
        } else {
            printf("\n----------------------------------------\n");
            printf("  %llu queries · %llu ok · %llu err · %.3fs total\n",
                   (unsigned long long)bs.queries_total,
                   (unsigned long long)bs.queries_ok,
                   (unsigned long long)bs.queries_err,
                   bs.total_sec);
            printf("  avg=%.3fms  min=%.3fms  max=%.3fms\n",
                   bs.avg_ms, bs.min_ms, bs.max_ms);
            printf("  p50=%.3fms  p95=%.3fms  p99=%.3fms\n",
                   bs.p50_ms, bs.p95_ms, bs.p99_ms);
        }
    }

    /* Exit if any non-interactive flag used */
    if (import_file || run_file) {
        ddb_close(&db);
        printf("\n(saved to %s)\n", db_path);
        return 0;
    }

    int timing = 0;
    char buf[4096];

    while (1) {
        printf("hugo> ");
        fflush(stdout);
        if (!fgets(buf, sizeof(buf), stdin)) { printf("\n"); break; }
        rstrip(buf);
        if (is_blank(buf)) continue;

        if (strcmp(buf, "\\q") == 0 || strcmp(buf, "exit") == 0 ||
            strcmp(buf, "quit") == 0) break;
        if (strcmp(buf, "\\help") == 0) { print_help(); continue; }
        if (strcmp(buf, "\\timing") == 0) {
            timing = !timing;
            printf("timing %s\n", timing ? "ON" : "OFF");
            continue;
        }
        if (strcmp(buf, "\\close") == 0) {
            ddb_close(&db);
            printf("ok closed\n");
            return 0;
        }

        /* Remove trailing ; */
        size_t n = strlen(buf);
        if (n > 0 && buf[n-1] == ';') buf[n-1] = 0;

        clock_t t0 = clock();

        TokenList tl;
        if (hugo_tokenize(buf, &tl) != 0) {
            printf("err TOKEN \"%s\"\n", tl.error_msg);
            continue;
        }
        Query q;
        if (hugo_parse(&tl, &q) != 0) {
            printf("err PARSE \"%s\"\n", q.error);
            query_free(&q);
            continue;
        }

        HugoResult r;
        hugo_execute_disk(&db, &q, &r);
        result_print_disk(&r);
        result_free_disk(&r);
        query_free(&q);

        clock_t t1 = clock();
        if (timing) {
            double ms = 1000.0 * (t1 - t0) / CLOCKS_PER_SEC;
            printf("time: %.3f ms\n", ms);
        }
    }

    ddb_close(&db);
    printf("(saved to %s)\n", db_path);
    return 0;
}
