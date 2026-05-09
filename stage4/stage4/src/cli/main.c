/* main.c — HUGO DB CLI REPL
 *
 * Prompt: hugo>
 * Commands:
 *   \help          — show help
 *   \q, exit, quit — exit
 *   \timing        — toggle timing display
 *   \open <path>   — open/create database file
 *   \save          — save current DB
 *   \close         — close DB (auto-save if dirty)
 *   <HugoQL>       — execute query
 *
 * MVP: line editing = fgets thuần (no history/arrow keys).
 * Phase 7.b sẽ tích hợp linenoise.
 */
#include "../query/tokenizer.h"
#include "../query/parser.h"
#include "../query/executor.h"
#include "../core/collection.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

static void print_banner(void) {
    printf("HUGO DB — HugoQL CLI (MVP)\n");
    printf("Type \\help for commands, \\q to quit.\n\n");
}

static void print_help(void) {
    printf("Meta commands:\n");
    printf("  \\help            show this help\n");
    printf("  \\q, exit, quit   quit the REPL\n");
    printf("  \\timing          toggle display of query time\n");
    printf("  \\open <path>     open or create DB file\n");
    printf("  \\save            save current DB\n");
    printf("  \\close           close DB (auto-save if dirty)\n");
    printf("\nHugoQL examples:\n");
    printf("  madeco users\n");
    printf("  vietinfo users { name: \"Alice\", age: 20 }\n");
    printf("  funden users haar age $bh 18 orange bi age desc lime 10\n");
    printf("  cochin users haar id $bg 1 $quy age 21\n");
    printf("  demlet users haar id $bg 1\n");
    printf("  skill\n");
    printf("\n");
}

static int ends_with(const char *s, char c) {
    size_t n = strlen(s);
    return n > 0 && s[n-1] == c;
}

/* Strip trailing whitespace/newline */
static void rstrip(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r' ||
                     s[n-1] == ' '  || s[n-1] == '\t')) {
        s[--n] = 0;
    }
}

static int is_blank(const char *s) {
    while (*s) { if (*s != ' ' && *s != '\t') return 0; s++; }
    return 1;
}

int main(int argc, char **argv) {
#ifdef _WIN32
    /* UTF-8 output cho Windows console */
    SetConsoleOutputCP(65001);
#endif

    print_banner();

    HugoDatabase db;
    db_init(&db, "default");
    char db_path[512] = {0};

    /* Optional: argv[1] = path to open */
    if (argc >= 2) {
        strncpy(db_path, argv[1], sizeof(db_path)-1);
        if (db_load(&db, db_path) == 0) {
            printf("opened %s\n", db_path);
        } else {
            printf("creating new db at %s\n", db_path);
        }
    }

    int timing = 0;

    char buf[4096];
    char multi[16384] = {0};
    int in_multi = 0;

    while (1) {
        printf("%s ", in_multi ? "...  >" : "hugo>");
        fflush(stdout);

        if (!fgets(buf, sizeof(buf), stdin)) {
            printf("\n");
            break;
        }
        rstrip(buf);

        /* Meta commands */
        if (!in_multi) {
            if (is_blank(buf)) continue;
            if (strcmp(buf, "\\q") == 0 || strcmp(buf, "exit") == 0 ||
                strcmp(buf, "quit") == 0) {
                break;
            }
            if (strcmp(buf, "\\help") == 0) { print_help(); continue; }
            if (strcmp(buf, "\\timing") == 0) {
                timing = !timing;
                printf("timing %s\n", timing ? "ON" : "OFF");
                continue;
            }
            if (strncmp(buf, "\\open ", 6) == 0) {
                db_free(&db); db_init(&db, "default");
                strncpy(db_path, buf + 6, sizeof(db_path)-1);
                if (db_load(&db, db_path) == 0) printf("opened %s\n", db_path);
                else printf("creating new db at %s\n", db_path);
                continue;
            }
            if (strcmp(buf, "\\save") == 0) {
                if (db_path[0] == 0) {
                    printf("err NO_PATH \"no db path set; use \\open <path>\"\n");
                } else if (db_save(&db, db_path) == 0) {
                    printf("ok saved %s\n", db_path);
                } else {
                    printf("err IO \"save failed\"\n");
                }
                continue;
            }
            if (strcmp(buf, "\\close") == 0) {
                if (db.dirty && db_path[0]) db_save(&db, db_path);
                db_free(&db); db_init(&db, "default");
                db_path[0] = 0;
                printf("ok closed\n");
                continue;
            }
        }

        /* Multi-line: query ends when line ends with ';' or blank line */
        if (in_multi) {
            strncat(multi, " ", sizeof(multi) - strlen(multi) - 1);
            strncat(multi, buf, sizeof(multi) - strlen(multi) - 1);
            if (is_blank(buf) || ends_with(buf, ';')) {
                /* Process */
            } else {
                continue;
            }
        } else {
            if (!ends_with(buf, ';') && !strchr(buf, '}')) {
                /* Single-line: proceed directly (most queries don't need ;) */
            }
            strncpy(multi, buf, sizeof(multi)-1);
        }

        /* Remove trailing ; */
        size_t mlen = strlen(multi);
        if (mlen > 0 && multi[mlen-1] == ';') multi[mlen-1] = 0;

        /* Tokenize + parse + execute */
        clock_t t0 = clock();

        TokenList tl;
        if (hugo_tokenize(multi, &tl) != 0) {
            printf("err TOKEN \"%s\"\n", tl.error_msg);
            multi[0] = 0; in_multi = 0; continue;
        }

        Query q;
        if (hugo_parse(&tl, &q) != 0) {
            printf("err PARSE \"%s\"\n", q.error);
            query_free(&q);
            multi[0] = 0; in_multi = 0; continue;
        }

        HugoResult r;
        hugo_execute(&db, &q, &r);
        result_print(&r);

        query_free(&q);

        clock_t t1 = clock();
        if (timing) {
            double ms = 1000.0 * (t1 - t0) / CLOCKS_PER_SEC;
            printf("time: %.3f ms\n", ms);
        }

        multi[0] = 0; in_multi = 0;
    }

    if (db.dirty && db_path[0]) {
        db_save(&db, db_path);
        printf("auto-saved %s\n", db_path);
    }
    db_free(&db);
    return 0;
}
