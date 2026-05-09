/* cost_model.c — Cost formula implementations */
#include "cost_model.h"
#include <math.h>

void cost_model_init_default(CostModel *m) {
    m->io_cost_per_page       = DEFAULT_IO_COST;
    m->cpu_cost_per_tuple     = DEFAULT_CPU_TUPLE;
    m->cpu_cost_per_operator  = DEFAULT_CPU_OPERATOR;
    m->cpu_cost_per_hash      = DEFAULT_CPU_HASH;
    m->cpu_cost_per_sort_cmp  = DEFAULT_CPU_SORT_CMP;
    m->work_mem_bytes         = DEFAULT_WORK_MEM;
}

double cost_seq_scan(const CostModel *m, const CollectionStats *stats) {
    double pages = stats ? (double)stats->page_count : DEFAULT_PAGE_ESTIMATE;
    double rows  = stats ? (double)stats->total_rows  : DEFAULT_ROWS_ESTIMATE;
    if (pages < 1) pages = 1;
    double io  = pages * m->io_cost_per_page;
    double cpu = rows  * m->cpu_cost_per_tuple;
    return io + cpu;
}

double cost_index_scan(const CostModel *m, const CollectionStats *stats,
                       double selectivity) {
    double total_rows = stats ? (double)stats->total_rows : DEFAULT_ROWS_ESTIMATE;
    if (total_rows < 1) total_rows = 1;
    double matching = total_rows * selectivity;

    /* B-tree height ≈ log_100(N) — HugoDB B-tree fanout ~100 keys/node */
    double btree_height = log(total_rows + 1) / log(100.0);
    double index_io  = btree_height * m->io_cost_per_page;

    /* Data pages: 1 random I/O per matching row (worst case, no clustering) */
    double data_io = matching * m->io_cost_per_page;

    double cpu = matching * m->cpu_cost_per_tuple;
    return index_io + data_io + cpu;
}

double cost_filter(const CostModel *m, double input_rows, double selectivity) {
    (void)selectivity;
    /* No I/O — filter is applied over already-loaded tuples */
    return input_rows * m->cpu_cost_per_operator;
}

double cost_sort(const CostModel *m, double rows, double avg_row_size) {
    if (rows < 2) return 0;
    double n_log_n = rows * log(rows);

    /* Check if data fits in memory */
    double total_bytes = rows * avg_row_size;
    double cpu_cost = n_log_n * m->cpu_cost_per_sort_cmp;

    if (total_bytes > (double)m->work_mem_bytes) {
        /* External sort: multiple passes over data */
        int passes = (int)(log(total_bytes / m->work_mem_bytes) / log(2.0)) + 2;
        double pages = total_bytes / PAGE_SIZE;
        double io_cost = pages * passes * m->io_cost_per_page;
        return cpu_cost + io_cost;
    }
    return cpu_cost;
}

double cost_nested_loop_join(const CostModel *m, double outer_rows, double inner_rows) {
    /* For each outer row, scan entire inner — pure CPU */
    return outer_rows * inner_rows * m->cpu_cost_per_tuple;
}

double cost_hash_join(const CostModel *m,
                      double build_rows, double probe_rows,
                      double build_row_size) {
    /* Build phase: hash each build tuple */
    double build_cost = build_rows * (m->cpu_cost_per_tuple + m->cpu_cost_per_hash);

    /* Memory check: if hash table spills to disk, add I/O */
    double hash_table_bytes = build_rows * build_row_size * 1.5; /* overhead factor */
    if (hash_table_bytes > (double)m->work_mem_bytes) {
        double spill_pages = hash_table_bytes / PAGE_SIZE;
        build_cost += spill_pages * m->io_cost_per_page * 2.0; /* write + read back */
    }

    /* Probe phase: hash + lookup per probe tuple */
    double probe_cost = probe_rows * (m->cpu_cost_per_tuple + m->cpu_cost_per_hash);

    return build_cost + probe_cost;
}

double cost_sort_merge_join(const CostModel *m,
                            double left_rows, double right_rows,
                            int left_sorted, int right_sorted,
                            double avg_row_size) {
    double sort_left  = left_sorted  ? 0 : cost_sort(m, left_rows,  avg_row_size);
    double sort_right = right_sorted ? 0 : cost_sort(m, right_rows, avg_row_size);
    /* Merge: linear scan of both */
    double merge_cost = (left_rows + right_rows) * m->cpu_cost_per_tuple;
    return sort_left + sort_right + merge_cost;
}

double cost_hash_aggregate(const CostModel *m, double input_rows, double groups) {
    (void)groups;
    /* Build hash table of groups + scan all input */
    return input_rows * (m->cpu_cost_per_tuple + m->cpu_cost_per_hash);
}

double cost_stream_aggregate(const CostModel *m, double input_rows, int input_sorted) {
    /* Stream: just scan if sorted. Add sort cost if not. */
    double base = input_rows * m->cpu_cost_per_tuple;
    if (!input_sorted)
        base += cost_sort(m, input_rows, 128.0);
    return base;
}
