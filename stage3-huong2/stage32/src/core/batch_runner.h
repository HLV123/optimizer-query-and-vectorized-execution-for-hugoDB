/* batch_runner.h — Execute multiple HugoQL queries, track per-query timing */
#ifndef HUGO_BATCH_RUNNER_H
#define HUGO_BATCH_RUNNER_H

#include "disk_db.h"
#include <stdio.h>

typedef struct {
    uint64_t  queries_total;
    uint64_t  queries_ok;
    uint64_t  queries_err;
    double    total_sec;
    double    min_ms;
    double    max_ms;
    double    avg_ms;
    /* percentiles (computed from per-query timings) */
    double    p50_ms;
    double    p95_ms;
    double    p99_ms;
} BatchStats;

/* Run queries from file (1 query/line, -- for comments, ; allowed at end).
 * If verbose, print each query + result summary.
 * Writes to out_fp the summary at end. */
int batch_run_file(DiskDB *db, const char *path, int verbose,
                   FILE *out_fp, BatchStats *stats);

/* Same but from in-memory buffer (for HTTP /batch endpoint).
 * Also fills json_results (malloc'd JSON array), caller frees. */
int batch_run_buffer(DiskDB *db, const char *buf, size_t len, int verbose,
                     FILE *out_fp, BatchStats *stats,
                     char **json_results);

#endif
