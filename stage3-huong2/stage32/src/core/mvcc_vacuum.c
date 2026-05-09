/* mvcc_vacuum.c — MVCC Garbage Collection implementation */
#include "mvcc_vacuum.h"
#include "mvcc_read.h"
#include "mvcc_tx.h"
#include "doc_version.h"
#include "serializer.h"
#include "page.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ===== mvcc_oldest_visible_ts ===== */

uint64_t mvcc_oldest_visible_ts(DiskDB *db) {
    uint64_t min_ts = ts_oracle_current(&db->mvcc_oracle);

    /* Tìm min begin_ts trong tất cả active transactions */
    for (int i = 0; i < db->mvcc_registry.count; i++) {
        MvccTx *tx = db->mvcc_registry.txs[i];
        if (tx && tx->begin_ts < min_ts) {
            min_ts = tx->begin_ts;
        }
    }
    return min_ts;
}

/* ===== mvcc_vacuum ===== */

int mvcc_vacuum(DiskDB *db, VacuumStats *stats_out) {
    VacuumStats stats;
    memset(&stats, 0, sizeof(stats));

    uint64_t oldest_ts = mvcc_oldest_visible_ts(db);
    stats.oldest_visible_ts = oldest_ts;

    printf("  [vacuum] oldest_visible_ts=%llu\n", (unsigned long long)oldest_ts);

    /* Walk every collection, every doc_id */
    for (int ci = 0; ci < db->n_colls; ci++) {
        DiskColl *c = &db->colls[ci];
        if (!c->doc_page_ids) continue;

        for (uint64_t doc_id = 1; doc_id < c->capacity; doc_id++) {
            uint64_t head_ptr = c->doc_page_ids[doc_id];
            if (head_ptr == VERSION_PTR_NULL || head_ptr == 0) continue;

            /* Walk chain: tìm "anchor" — version đầu tiên với
             * created_ts <= oldest_ts (visible tới mọi active tx)
             * Mọi version sau anchor (prev chain) là dead. */

            uint64_t cur_ptr   = head_ptr;
            uint64_t prev_ptr  = VERSION_PTR_NULL;
            uint64_t anchor_ptr = VERSION_PTR_NULL;  /* ptr trỏ TỚI anchor */
            uint64_t anchor_prev_ptr = VERSION_PTR_NULL;  /* ptr trong anchor.prev */
            int      found_anchor = 0;
            int      chain_depth  = 0;
            int      max_depth    = 100000;

            while (cur_ptr != VERSION_PTR_NULL && chain_depth++ < max_depth) {
                DocVersion v;
                if (mvcc_page_read_version(db,
                                           version_ptr_page(cur_ptr),
                                           version_ptr_offset(cur_ptr),
                                           &v, NULL) != 0) {
                    break;
                }

                /* Phiên bản committed có created_ts <= oldest_ts
                 * → đây là anchor, tất cả version cũ hơn có thể xóa */
                if (v.created_ts > 0 && v.created_ts <= oldest_ts) {
                    found_anchor = 1;
                    anchor_ptr      = cur_ptr;
                    anchor_prev_ptr = v.prev_version_ptr;
                    /* prev_ptr là ptr TỪ version TRƯỚC trỏ tới cur_ptr (anchor) */
                    (void)prev_ptr;
                    break;
                }

                prev_ptr = cur_ptr;
                cur_ptr  = v.prev_version_ptr;
            }

            if (!found_anchor) continue;
            if (anchor_prev_ptr == VERSION_PTR_NULL) continue;  /* Không có gì để xóa */

            /* Cắt chain: set anchor.prev_version_ptr = VERSION_PTR_NULL */
            uint64_t anchor_page   = version_ptr_page(anchor_ptr);
            uint16_t anchor_offset = version_ptr_offset(anchor_ptr);

            HugoPage page;
            if (pm_read_page(&db->pm, anchor_page, &page) != PG_OK) continue;

            /* prev_version_ptr nằm ở bytes [32..39] trong DocVersion header
             * (offset 32 từ đầu header tại anchor_offset) */
            uint16_t prev_field_off = anchor_offset + 32;
            if (prev_field_off + 8 > HUGO_PAGE_DATA_SIZE) continue;
            write_u64_be(page.data + prev_field_off, VERSION_PTR_NULL);
            pm_write_page(&db->pm, &page);

            /* Walk phần chain đã bị cắt, đánh dấu pages là FREE */
            cur_ptr = anchor_prev_ptr;
            int freed = 0;
            while (cur_ptr != VERSION_PTR_NULL && freed < 100000) {
                uint64_t dead_page   = version_ptr_page(cur_ptr);
                uint16_t dead_offset = version_ptr_offset(cur_ptr);

                DocVersion dead_v;
                uint64_t next_prev = VERSION_PTR_NULL;
                if (mvcc_page_read_version(db, dead_page, dead_offset,
                                           &dead_v, NULL) == 0) {
                    next_prev = dead_v.prev_version_ptr;
                }

                /* Mark page as FREE (đơn giản: đổi page_type) */
                HugoPage dead_pg;
                if (pm_read_page(&db->pm, dead_page, &dead_pg) == PG_OK) {
                    if (dead_pg.page_type == PAGE_TYPE_MVCC_VERSION) {
                        dead_pg.page_type = PAGE_TYPE_FREE;
                        pm_write_page(&db->pm, &dead_pg);
                        stats.pages_freed++;
                    }
                }

                stats.versions_removed++;
                freed++;
                cur_ptr = next_prev;
            }
        }
    }

    printf("  [vacuum] removed %llu versions, freed %llu pages\n",
           (unsigned long long)stats.versions_removed,
           (unsigned long long)stats.pages_freed);

    if (stats_out) *stats_out = stats;
    return 0;
}
