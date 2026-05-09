/* http_server.h — Minimal HTTP/1.1 server (blocking, single-thread)
 *
 * Đủ cho local dev UI. KHÔNG dùng cho production (no TLS, no concurrency,
 * no keep-alive). Cross-platform: BSD sockets trên POSIX, Winsock trên Windows.
 *
 * Usage:
 *   HttpServer s;
 *   http_server_init(&s, 7777);
 *   http_server_add_route(&s, "GET",  "/health",      handle_health);
 *   http_server_add_route(&s, "POST", "/query",       handle_query);
 *   http_server_run(&s, &user_ctx);   // blocks, serves forever
 */
#ifndef HUGO_HTTP_SERVER_H
#define HUGO_HTTP_SERVER_H

#include <stddef.h>
#include <stdint.h>

#define HTTP_MAX_ROUTES       32
#define HTTP_MAX_PATH         256
#define HTTP_MAX_METHOD       8
#define HTTP_MAX_HEADERS      4096
#define HTTP_MAX_BODY         (16 * 1024 * 1024)   /* 16MB for bulk import */

typedef struct {
    char   method[HTTP_MAX_METHOD];
    char   path[HTTP_MAX_PATH];
    char  *body;               /* pointer into request buffer */
    size_t body_len;
    int    keep_alive;         /* unused in MVP */
} HttpRequest;

typedef struct {
    int    status;             /* 200, 404, 500, ... */
    const char *content_type;  /* "application/json", "text/html", ... */
    char  *body;               /* malloc'd; server frees */
    size_t body_len;
} HttpResponse;

typedef void (*HttpHandler)(const HttpRequest *req, HttpResponse *resp, void *ctx);

typedef struct {
    char         method[HTTP_MAX_METHOD];
    char         path_prefix[HTTP_MAX_PATH];  /* exact match trong MVP */
    HttpHandler  handler;
} HttpRoute;

typedef struct {
    int         port;
    int         listen_fd;
    HttpRoute   routes[HTTP_MAX_ROUTES];
    int         n_routes;
    int         running;
} HttpServer;

int  http_server_init     (HttpServer *s, int port);
void http_server_add_route(HttpServer *s, const char *method, const char *path,
                           HttpHandler handler);
int  http_server_run      (HttpServer *s, void *ctx);   /* blocks */
void http_server_stop     (HttpServer *s);

/* Helper: respond với JSON body (malloc'd, server sẽ free) */
void http_respond_json  (HttpResponse *resp, int status, const char *json);
void http_respond_text  (HttpResponse *resp, int status, const char *text,
                         const char *content_type);
void http_respond_bytes (HttpResponse *resp, int status, const void *bytes,
                         size_t len, const char *content_type);

#endif
