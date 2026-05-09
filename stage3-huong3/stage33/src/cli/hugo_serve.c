/* hugo_serve.c — HUGO DB HTTP server with embedded Web UI
 *
 * Usage:
 *   hugo_serve [path.hugo] [port]
 *
 * Default: hugo.hugo, port 7777
 *
 * Endpoints:
 *   GET  /           — web UI (HTML)
 *   GET  /health     — server status (JSON)
 *   GET  /collections— list collections (JSON)
 *   POST /query      — execute HugoQL (body=text, resp=JSON)
 */
#include "../core/http_server.h"
#include "../core/hugo_api.h"
#include "../core/disk_db.h"
#include "../core/ui_html.h"
#include "../core/bulk_import.h"
#include "../core/batch_runner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* Context passed to handlers = DiskDB pointer */

static void handle_index(const HttpRequest *req, HttpResponse *resp, void *ctx) {
    (void)req; (void)ctx;
    http_respond_text(resp, 200, UI_HTML, "text/html; charset=utf-8");
}

static void handle_health(const HttpRequest *req, HttpResponse *resp, void *ctx) {
    (void)req;
    DiskDB *db = (DiskDB*)ctx;
    char *j = hugo_api_health(db);
    if (j) {
        http_respond_json(resp, 200, j);
        free(j);
    } else {
        http_respond_text(resp, 500, "oom\n", "text/plain");
    }
}

static void handle_collections(const HttpRequest *req, HttpResponse *resp, void *ctx) {
    (void)req;
    DiskDB *db = (DiskDB*)ctx;
    char *j = hugo_api_list_collections(db);
    if (j) {
        http_respond_json(resp, 200, j);
        free(j);
    } else {
        http_respond_text(resp, 500, "oom\n", "text/plain");
    }
}

static void handle_query(const HttpRequest *req, HttpResponse *resp, void *ctx) {
    DiskDB *db = (DiskDB*)ctx;
    if (!req->body || req->body_len == 0) {
        http_respond_json(resp, 400,
            "{\"ok\":false,\"err_code\":\"EMPTY\",\"err_msg\":\"empty body\"}");
        return;
    }
    /* Make null-terminated copy */
    char *sql = (char*)malloc(req->body_len + 1);
    memcpy(sql, req->body, req->body_len);
    sql[req->body_len] = 0;

    char *j = hugo_api_exec(db, sql);
    free(sql);
    if (j) {
        http_respond_json(resp, 200, j);
        free(j);
    } else {
        http_respond_text(resp, 500, "oom\n", "text/plain");
    }
}

/* Extract query param value: "/import?coll=users" → "users". Basic (no unescape). */
static int extract_query_param(const char *path, const char *key,
                               char *out, size_t out_cap) {
    const char *q = strchr(path, '?');
    if (!q) return -1;
    q++;
    size_t klen = strlen(key);
    while (*q) {
        if (strncmp(q, key, klen) == 0 && q[klen] == '=') {
            q += klen + 1;
            const char *end = strchr(q, '&');
            size_t vlen = end ? (size_t)(end - q) : strlen(q);
            if (vlen >= out_cap) vlen = out_cap - 1;
            memcpy(out, q, vlen);
            out[vlen] = 0;
            return 0;
        }
        const char *amp = strchr(q, '&');
        if (!amp) break;
        q = amp + 1;
    }
    return -1;
}

/* POST /import?coll=<name> — body is JSONL */
static void handle_import(const HttpRequest *req, HttpResponse *resp, void *ctx) {
    DiskDB *db = (DiskDB*)ctx;
    char coll[64] = {0};
    if (extract_query_param(req->path, "coll", coll, sizeof(coll)) != 0) {
        http_respond_json(resp, 400,
            "{\"ok\":false,\"err_code\":\"NO_COLL\",\"err_msg\":\"missing ?coll= param\"}");
        return;
    }
    if (!req->body || req->body_len == 0) {
        http_respond_json(resp, 400,
            "{\"ok\":false,\"err_code\":\"EMPTY\",\"err_msg\":\"empty body\"}");
        return;
    }
    BulkStats stats;
    int rc = bulk_import_buffer(db, coll, req->body, req->body_len, &stats);
    char buf[512];
    if (rc != 0) {
        snprintf(buf, sizeof(buf),
            "{\"ok\":false,\"err_code\":\"IMPORT_FAILED\",\"err_msg\":\"rc=%d\"}", rc);
        http_respond_json(resp, 500, buf);
        return;
    }
    snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"coll\":\"%s\",\"lines_read\":%llu,\"docs_inserted\":%llu,"
        "\"parse_errors\":%llu,\"insert_errors\":%llu,\"elapsed_sec\":%.3f,"
        "\"bytes_read\":%llu}",
        coll,
        (unsigned long long)stats.lines_read,
        (unsigned long long)stats.docs_inserted,
        (unsigned long long)stats.parse_errors,
        (unsigned long long)stats.insert_errors,
        stats.elapsed_sec,
        (unsigned long long)stats.bytes_read);
    http_respond_json(resp, 200, buf);
}

/* POST /batch — body is multiple HugoQL queries (1 per line) */
static void handle_batch(const HttpRequest *req, HttpResponse *resp, void *ctx) {
    DiskDB *db = (DiskDB*)ctx;
    if (!req->body || req->body_len == 0) {
        http_respond_json(resp, 400,
            "{\"ok\":false,\"err_code\":\"EMPTY\",\"err_msg\":\"empty body\"}");
        return;
    }
    BatchStats stats;
    char *results_json = NULL;
    batch_run_buffer(db, req->body, req->body_len, 0, NULL, &stats, &results_json);

    /* Compose full response: stats + results */
    size_t n = results_json ? strlen(results_json) : 2;
    char *full = (char*)malloc(n + 512);
    snprintf(full, n + 512,
        "{\"ok\":true,\"total\":%llu,\"ok_count\":%llu,\"err_count\":%llu,"
        "\"total_sec\":%.3f,\"avg_ms\":%.3f,\"min_ms\":%.3f,\"max_ms\":%.3f,"
        "\"p50_ms\":%.3f,\"p95_ms\":%.3f,\"p99_ms\":%.3f,\"results\":%s}",
        (unsigned long long)stats.queries_total,
        (unsigned long long)stats.queries_ok,
        (unsigned long long)stats.queries_err,
        stats.total_sec, stats.avg_ms, stats.min_ms, stats.max_ms,
        stats.p50_ms, stats.p95_ms, stats.p99_ms,
        results_json ? results_json : "[]");
    http_respond_json(resp, 200, full);
    free(full);
    free(results_json);
}

int main(int argc, char **argv) {
#ifdef _WIN32
    SetConsoleOutputCP(65001);
#endif

    const char *db_path = (argc >= 2) ? argv[1] : "hugo.hugo";
    int port            = (argc >= 3) ? atoi(argv[2]) : 7777;

    printf("HUGO DB server starting...\n");
    printf("  Database:  %s\n", db_path);
    printf("  Port:      %d\n", port);

    DiskDB db;
    if (ddb_open(&db, db_path) != 0) {
        if (ddb_create(&db, "default", db_path) != 0) {
            fprintf(stderr, "ERROR: cannot create/open %s\n", db_path);
            return 1;
        }
        printf("  (created new database)\n");
    } else {
        printf("  (opened existing database — %d collection(s))\n", db.n_colls);
    }
    printf("\n");

    HttpServer s;
    if (http_server_init(&s, port) != 0) {
        fprintf(stderr, "ERROR: cannot bind port %d (already in use?)\n", port);
        ddb_close(&db);
        return 1;
    }

    http_server_add_route(&s, "GET",  "/",            handle_index);
    http_server_add_route(&s, "GET",  "/health",      handle_health);
    http_server_add_route(&s, "GET",  "/collections", handle_collections);
    http_server_add_route(&s, "POST", "/query",       handle_query);
    http_server_add_route(&s, "POST", "/import",      handle_import);
    http_server_add_route(&s, "POST", "/batch",       handle_batch);

    printf("Open browser: http://127.0.0.1:%d\n", port);
    printf("Press Ctrl+C to stop.\n\n");
    fflush(stdout);

    http_server_run(&s, &db);

    ddb_close(&db);
    return 0;
}
