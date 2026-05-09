/* executor_disk.h — Executor cho DiskDB (Phase 8.a integration) */
#ifndef HUGO_EXECUTOR_DISK_H
#define HUGO_EXECUTOR_DISK_H

#include "../query/ast.h"
#include "../query/executor.h"    /* for HugoResult + result_print */
#include "disk_db.h"

int hugo_execute_disk(DiskDB *db, const Query *q, HugoResult *r);

/* Result_print cho disk version: r->docs là heap-allocated clones, cần free. */
void result_print_disk(const HugoResult *r);
void result_free_disk (HugoResult *r);

#endif
