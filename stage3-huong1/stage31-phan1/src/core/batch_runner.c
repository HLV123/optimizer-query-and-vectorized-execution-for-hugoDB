/* batch_runner.c */
#include "batch_runner.h"
#include "hugo_api.h"
#include "../query/tokenizer.h"
#include "../query/parser.h"
#include "executor_disk.h"
#include "collection.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__MACH__) || defined(__APPLE__)
  #include <mach/mach_time.h>
#elif defined(__linux__) || defined(__unix__)
  #include <time.h>
#endif

static double now_ms(void) {
#if defined(_WIN32)
    /* clock() trên Windows có resolution ~15ms — đủ cho batch-level timing */
    return 1000.0 * (double)clock() / CLOCKS_PER_SEC;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
#endif
}

static int dbl_cmp(const void *a, const void *b) {
    double da = *(const double*)a, db = *(const double*)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

/* Strip leading/trailing whitespace, return pointer into input (modifies in place) */
static char* trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' ||
                     e[-1] == '\r' || e[-1] == '\n' || e[-1] == ';')) {
        *(--e) = 0;
    }
    return s;
}

static int is_blank_or_comment(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    if (*s == 0) return 1;
    if (s[0] == '-' && s[1] == '-') return 1;
    if (s[0] == '#') return 1;
    return 0;
}

/* Execute 1 query, return elapsed ms. ok_out = 1 nếu OK. */
static double run_one(DiskDB *db, const char *sql, int *ok_out, char *info_out, size_t info_cap) {
    double t0 = now_ms();

    TokenList tl;
    int ok = 0;
    if (hugo_tokenize(sql, &tl) != 0) {
        snprintf(info_out, info_cap, "err TOKEN %s", tl.error_msg);
    } else {
        Query q;
        if (hugo_parse(&tl, &q) != 0) {
            snprintf(info_out, info_cap, "err PARSE %s", q.error);
        } else {
            HugoResult r;
            hugo_execute_disk(db, &q, &r);
            if (r.ok) {
                ok = 1;
                if (r.info[0])
                    snprintf(info_out, info_cap, "ok %s", r.info);
                else
                    snprintf(info_out, info_cap, "ok %d doc%s",
                             r.count, r.count == 1 ? "" : "s");
            } else {
                snprintf(info_out, info_cap, "err %s %s", r.err_code, r.err_msg);
            }
            result_free_disk(&r);
        }
        query_free(&q);
    }

    double t1 = now_ms();
    *ok_out = ok;
    return t1 - t0;
}

/* ===== Public: run from FILE ===== */
int batch_run_file(DiskDB *db, const char *path, int verbose,
                   FILE *out_fp, BatchStats *stats) {
    memset(stats, 0, sizeof(*stats));
    stats->min_ms = 1e18;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    double *timings = NULL;
    size_t timings_cap = 0, timings_n = 0;

    double batch_t0 = now_ms();
    char buf[8192];
    while (fgets(buf, sizeof(buf), f)) {
        if (is_blank_or_comment(buf)) continue;
        char *q = trim(buf);
        if (!*q) continue;

        int ok;
        char info[256];
        double ms = run_one(db, q, &ok, info, sizeof(info));

        stats->queries_total++;
        if (ok) stats->queries_ok++; else stats->queries_err++;

        if (timings_n >= timings_cap) {
            timings_cap = timings_cap ? timings_cap * 2 : 256;
            timings = (double*)realloc(timings, timings_cap * sizeof(double));
        }
        timings[timings_n++] = ms;

        if (ms < stats->min_ms) stats->min_ms = ms;
        if (ms > stats->max_ms) stats->max_ms = ms;

        if (verbose && out_fp) {
            fprintf(out_fp, "  [%7.2f ms] %-50.50s → %s\n", ms, q, info);
        }
    }
    fclose(f);
    double batch_t1 = now_ms();

    stats->total_sec = (batch_t1 - batch_t0) / 1000.0;
    if (stats->queries_total > 0) {
        double sum = 0;
        for (size_t i = 0; i < timings_n; i++) sum += timings[i];
        stats->avg_ms = sum / timings_n;

        qsort(timings, timings_n, sizeof(double), dbl_cmp);
        stats->p50_ms = timings[timings_n * 50 / 100];
        stats->p95_ms = timings[timings_n * 95 / 100];
        stats->p99_ms = timings[timings_n > 1 ? (timings_n * 99) / 100 : 0];
    } else {
        stats->min_ms = 0;
    }

    free(timings);
    return 0;
}

/* ===== Public: run from buffer (for HTTP) ===== */
int batch_run_buffer(DiskDB *db, const char *buf, size_t len, int verbose,
                     FILE *out_fp, BatchStats *stats, char **json_results) {
    memset(stats, 0, sizeof(*stats));
    stats->min_ms = 1e18;

    /* Build JSON results array */
    size_t jcap = 4096;
    size_t jlen = 0;
    char *json = (char*)malloc(jcap);
    jlen += snprintf(json + jlen, jcap - jlen, "[");

    double *timings = NULL;
    size_t timings_cap = 0, timings_n = 0;

    double batch_t0 = now_ms();
    const char *p = buf;
    const char *end = buf + len;
    int first = 1;

    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t line_len = nl ? (size_t)(nl - p) : (size_t)(end - p);

        /* Copy to mutable buf */
        char line[8192];
        if (line_len >= sizeof(line)) line_len = sizeof(line) - 1;
        memcpy(line, p, line_len);
        line[line_len] = 0;

        char *q = trim(line);
        if (*q && !is_blank_or_comment(q)) {
            /* Use hugo_api_exec for full JSON result */
            double t0 = now_ms();
            char *r_json = hugo_api_exec(db, q);
            double ms = now_ms() - t0;

            int ok = r_json && strstr(r_json, "\"ok\":true") != NULL;
            stats->queries_total++;
            if (ok) stats->queries_ok++; else stats->queries_err++;

            if (timings_n >= timings_cap) {
                timings_cap = timings_cap ? timings_cap * 2 : 256;
                timings = (double*)realloc(timings, timings_cap * sizeof(double));
            }
            timings[timings_n++] = ms;
            if (ms < stats->min_ms) stats->min_ms = ms;
            if (ms > stats->max_ms) stats->max_ms = ms;

            if (verbose && out_fp) {
                fprintf(out_fp, "  [%7.2f ms] %-50.50s\n", ms, q);
            }

            /* Append to json array */
            size_t need = strlen(r_json) + strlen(q) + 128;
            if (jlen + need >= jcap) {
                while (jlen + need >= jcap) jcap *= 2;
                json = (char*)realloc(json, jcap);
            }
            jlen += snprintf(json + jlen, jcap - jlen, "%s{\"query\":", first ? "" : ",");
            first = 0;
            /* Quote query string (simple — assume no double quotes in input; if any, they'd be inside strings
             * which tokenizer handled; batch input typically safe). Conservative escape: */
            jlen += snprintf(json + jlen, jcap - jlen, "\"");
            for (const char *qq = q; *qq; qq++) {
                if (jlen + 6 >= jcap) { jcap *= 2; json = realloc(json, jcap); }
                if (*qq == '"')      { json[jlen++] = '\\'; json[jlen++] = '"'; }
                else if (*qq == '\\'){ json[jlen++] = '\\'; json[jlen++] = '\\'; }
                else if (*qq < 0x20) { jlen += snprintf(json + jlen, jcap - jlen, "\\u%04x", (unsigned)*qq); }
                else                  json[jlen++] = *qq;
            }
            json[jlen] = 0;
            jlen += snprintf(json + jlen, jcap - jlen, "\",\"ms\":%.3f,\"result\":%s}",
                             ms, r_json);
            free(r_json);
        }
        p = nl ? nl + 1 : end;
    }
    double batch_t1 = now_ms();

    jlen += snprintf(json + jlen, jcap - jlen, "]");

    stats->total_sec = (batch_t1 - batch_t0) / 1000.0;
    if (stats->queries_total > 0) {
        double sum = 0;
        for (size_t i = 0; i < timings_n; i++) sum += timings[i];
        stats->avg_ms = sum / timings_n;
        qsort(timings, timings_n, sizeof(double), dbl_cmp);
        stats->p50_ms = timings[timings_n * 50 / 100];
        stats->p95_ms = timings[timings_n * 95 / 100];
        stats->p99_ms = timings[timings_n > 1 ? (timings_n * 99) / 100 : 0];
    } else {
        stats->min_ms = 0;
    }
    free(timings);

    if (json_results) *json_results = json;
    else              free(json);
    return 0;
}
