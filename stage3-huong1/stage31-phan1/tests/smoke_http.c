/* smoke_http.c — quick smoke test for http_server */
#include "../src/core/http_server.h"
#include <stdio.h>
#include <string.h>

static void handle_health(const HttpRequest *req, HttpResponse *resp, void *ctx) {
    (void)req; (void)ctx;
    http_respond_json(resp, 200, "{\"status\":\"ok\"}");
}

static void handle_echo(const HttpRequest *req, HttpResponse *resp, void *ctx) {
    (void)ctx;
    http_respond_text(resp, 200, req->body ? req->body : "(empty)", "text/plain");
}

int main(void) {
    HttpServer s;
    if (http_server_init(&s, 17777) != 0) {
        fprintf(stderr, "init failed\n"); return 1;
    }
    http_server_add_route(&s, "GET",  "/health", handle_health);
    http_server_add_route(&s, "POST", "/echo",   handle_echo);
    http_server_run(&s, NULL);
    return 0;
}
