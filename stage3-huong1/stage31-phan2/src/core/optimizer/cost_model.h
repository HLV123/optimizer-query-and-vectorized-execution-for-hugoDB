/* cost_model.h — Cost model for Hugo DB optimizer
 *
 * Simple additive formula: Cost = α × page_reads + β × CPU_tuples
 * All constants centralized here for easy tuning.
 *
 * Phase 3: cost formulas per operator.
 */
#ifndef HUGO_COST_MODEL_H
#define HUGO_COST_MODEL_H

#include <stddef.h>
#include "statistics.h"

#define PAGE_SIZE 4096

/* ===== Cost Model Constants ===== */
typedef struct CostModel {
    double io_cost_per_page;           /* default: 1.0  */
    double cpu_cost_per_tuple;         /* default: 0.01 */
    double cpu_cost_per_operator;      /* default: 0.001 */
    double cpu_cost_per_hash;          /* default: 0.05 */
    double cpu_cost_per_sort_cmp;      /* default: 0.05 */
    size_t work_mem_bytes;             /* default: 64 MB */
} CostModel;

/* Default constants */
#define DEFAULT_IO_COST          1.0
#define DEFAULT_CPU_TUPLE        0.01
#define DEFAULT_CPU_OPERATOR     0.001
#define DEFAULT_CPU_HASH         0.05
#define DEFAULT_CPU_SORT_CMP     0.05
#define DEFAULT_WORK_MEM         (64 * 1024 * 1024)

/* Default fallback when stats not available */
#define DEFAULT_ROWS_ESTIMATE    1000.0
#define DEFAULT_PAGE_ESTIMATE    100.0
#define DEFAULT_SELECTIVITY      0.1

void cost_model_init_default(CostModel *m);

/* ===== Cost formulas ===== */

/* Sequential scan: read all pages + process all tuples */
double cost_seq_scan(const CostModel *m, const CollectionStats *stats);

/* Index scan: log(N) index pages + selectivity * data pages */
double cost_index_scan(const CostModel *m, const CollectionStats *stats,
                       double selectivity);

/* Filter: just CPU per tuple (no I/O, filter is applied on input) */
double cost_filter(const CostModel *m, double input_rows, double selectivity);

/* Sort: CPU = O(N log N) comparisons */
double cost_sort(const CostModel *m, double rows, double avg_row_size);

/* Nested loop join: outer × inner CPU */
double cost_nested_loop_join(const CostModel *m, double outer_rows, double inner_rows);

/* Hash join: build + probe (with memory spill check) */
double cost_hash_join(const CostModel *m,
                      double build_rows, double probe_rows,
                      double build_row_size);

/* Sort-merge join: sort both sides then merge */
double cost_sort_merge_join(const CostModel *m,
                            double left_rows, double right_rows,
                            int left_sorted, int right_sorted,
                            double avg_row_size);

/* Hash aggregate */
double cost_hash_aggregate(const CostModel *m, double input_rows, double groups);

/* Stream aggregate (requires sorted input) */
double cost_stream_aggregate(const CostModel *m, double input_rows, int input_sorted);

#endif
