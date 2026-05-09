/* statistics.h — Collection statistics for cost estimation
 *
 * Stats được build bằng lệnh `analyze <collection>` (scan full table).
 * Persist vào file `<dbname>.stats` (text format, dễ debug).
 * Loaded khi open DB. Stale sau khi data thay đổi nhiều.
 *
 * Defensive: mọi stats access phải handle NULL → fallback defaults.
 */
#ifndef HUGO_STATISTICS_H
#define HUGO_STATISTICS_H

#include <stddef.h>
#include <stdint.h>
#include "../../query/ast.h"
#include "../../core/disk_db.h"

#define HISTOGRAM_BUCKETS 32
#define TOP_K_VALUES      10
#define MAX_STATS_COLUMNS 64

/* ===== Per-column statistics ===== */
typedef struct ColumnStats {
    char     column_name[128];
    uint64_t total_rows;      /* rows where field exists */
    uint64_t null_count;      /* rows where field missing */
    uint64_t distinct_count;  /* unique values estimate */

    /* Numeric range */
    double   min_value;
    double   max_value;
    double   avg_value;

    /* Equi-depth histogram for numeric columns */
    double   histogram_bounds[HISTOGRAM_BUCKETS + 1];
    uint64_t histogram_counts[HISTOGRAM_BUCKETS];
    int      has_histogram;

    /* Top-K frequent string values */
    char     top_k_values[TOP_K_VALUES][128];
    uint64_t top_k_counts[TOP_K_VALUES];
    int      n_top_k;
} ColumnStats;

/* ===== Per-collection statistics ===== */
typedef struct CollectionStats {
    char     collection_name[64];
    uint64_t total_rows;
    uint64_t total_bytes;
    uint64_t page_count;
    uint64_t avg_row_size;

    ColumnStats columns[MAX_STATS_COLUMNS];
    int         n_columns;

    int         is_valid;  /* 0 = not yet analyzed */
} CollectionStats;

/* ===== Stats store (one per DiskDB) ===== */
#define MAX_STATS_COLLECTIONS 64
typedef struct StatsStore {
    CollectionStats entries[MAX_STATS_COLLECTIONS];
    int             n_entries;
    char            stats_path[512];
} StatsStore;

/* ===== API ===== */

/* Init/free stats store */
void stats_store_init(StatsStore *ss, const char *db_path);

/* Scan collection and build statistics (expensive — user-triggered) */
int stats_analyze(DiskDB *db, StatsStore *ss, const char *collection);

/* Get stats for a collection (NULL if not analyzed) */
CollectionStats* stats_get(StatsStore *ss, const char *collection);

/* Find column stats within a collection stats (NULL if not found) */
ColumnStats* stats_find_column(CollectionStats *cs, const char *col_name);

/* Estimate selectivity of a predicate against stats [0.0, 1.0]
 * Returns 0.1 as fallback if stats not available. */
double stats_estimate_selectivity(const CollectionStats *cs, const Condition *pred);

/* Estimate output cardinality of a join */
double stats_estimate_join_cardinality(
    const CollectionStats *left_stats,
    const CollectionStats *right_stats,
    const char *left_col,
    const char *right_col);

/* Persist stats to disk */
int stats_persist(const StatsStore *ss);

/* Load stats from disk */
int stats_load(StatsStore *ss);

#endif
