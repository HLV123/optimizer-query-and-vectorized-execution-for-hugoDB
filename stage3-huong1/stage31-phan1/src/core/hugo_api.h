/* hugo_api.h — Bridge DiskDB + executor_disk → JSON for HTTP API
 *
 * Core function: hugo_api_exec(db, hugoql) → malloc'd JSON string.
 * Caller free. Response format (on success):
 *
 *   { "ok": true, "count": 2, "info": "...",
 *     "docs": [ {...}, {...} ] }
 *
 * On error:
 *   { "ok": false, "err_code": "...", "err_msg": "..." }
 */
#ifndef HUGO_API_H
#define HUGO_API_H

#include "disk_db.h"

/* Execute HugoQL string, return JSON response (caller must free). */
char* hugo_api_exec(DiskDB *db, const char *hugoql);

/* List collections as JSON: { "collections": [ {"name":..,"count":..}, ... ] } */
char* hugo_api_list_collections(DiskDB *db);

/* Health: {"status":"ok","db":"...","collections":N} */
char* hugo_api_health(DiskDB *db);

#endif
