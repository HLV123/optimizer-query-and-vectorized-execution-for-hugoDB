/* http_server.c — Minimal HTTP/1.1 server */
#include "http_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef int socklen_t;
  #define CLOSE_SOCK(s) closesocket(s)
  #define strncasecmp _strnicmp
  static int sock_init(void) {
      WSADATA wsa;
      return WSAStartup(MAKEWORD(2,2), &wsa) == 0 ? 0 : -1;
  }
  static void sock_cleanup(void) { WSACleanup(); }
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <errno.h>
  #include <strings.h>
  #define CLOSE_SOCK(s) close(s)
  #define INVALID_SOCKET (-1)
  #define SOCKET_ERROR   (-1)
  typedef int SOCKET;
  static int  sock_init(void)    { return 0; }
  static void sock_cleanup(void) { }
#endif

/* ===== Basic helpers ===== */
int http_server_init(HttpServer *s, int port) {
    memset(s, 0, sizeof(*s));
    s->port = port;
    if (sock_init() != 0) return -1;

    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) return -1;

    /* Allow reuse */
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  /* only localhost */
    addr.sin_port        = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        CLOSE_SOCK(fd);
        return -2;
    }
    if (listen(fd, 16) == SOCKET_ERROR) {
        CLOSE_SOCK(fd);
        return -3;
    }
    s->listen_fd = (int)fd;
    return 0;
}

void http_server_add_route(HttpServer *s, const char *method, const char *path,
                           HttpHandler handler) {
    if (s->n_routes >= HTTP_MAX_ROUTES) return;
    HttpRoute *r = &s->routes[s->n_routes++];
    strncpy(r->method, method, sizeof(r->method)-1);
    strncpy(r->path_prefix, path, sizeof(r->path_prefix)-1);
    r->handler = handler;
}

/* ===== Parse request ===== */
static int parse_request(const char *buf, size_t buf_len, HttpRequest *req,
                         size_t *header_end_out) {
    memset(req, 0, sizeof(*req));
    /* Find \r\n\r\n */
    const char *hend = NULL;
    for (size_t i = 0; i + 3 < buf_len; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' &&
            buf[i+2] == '\r' && buf[i+3] == '\n') {
            hend = buf + i + 4;
            break;
        }
    }
    if (!hend) return -1;  /* incomplete headers */
    *header_end_out = (size_t)(hend - buf);

    /* Parse first line: METHOD PATH HTTP/1.x */
    const char *p = buf;
    const char *sp1 = memchr(p, ' ', buf_len);
    if (!sp1) return -2;
    size_t mlen = (size_t)(sp1 - p);
    if (mlen >= sizeof(req->method)) return -2;
    memcpy(req->method, p, mlen);
    req->method[mlen] = 0;

    p = sp1 + 1;
    const char *sp2 = memchr(p, ' ', (size_t)(buf + buf_len - p));
    if (!sp2) return -3;
    size_t plen = (size_t)(sp2 - p);
    if (plen >= sizeof(req->path)) return -3;
    memcpy(req->path, p, plen);
    req->path[plen] = 0;

    /* Parse Content-Length header */
    long content_length = 0;
    const char *hdr = memchr(buf, '\n', buf_len);
    if (hdr && hdr < hend) {
        hdr++;
        while (hdr < hend) {
            const char *line_end = memchr(hdr, '\r', (size_t)(hend - hdr));
            if (!line_end) break;
            if (line_end - hdr > 16 &&
                (strncasecmp(hdr, "Content-Length:", 15) == 0)) {
                content_length = strtol(hdr + 15, NULL, 10);
            }
            hdr = line_end + 2;  /* skip \r\n */
        }
    }
    req->body_len = (content_length > 0) ? (size_t)content_length : 0;
    return 0;
}

/* ===== Response helpers ===== */
void http_respond_json(HttpResponse *resp, int status, const char *json) {
    http_respond_text(resp, status, json, "application/json");
}

void http_respond_text(HttpResponse *resp, int status, const char *text,
                       const char *content_type) {
    size_t n = strlen(text);
    http_respond_bytes(resp, status, text, n, content_type);
}

void http_respond_bytes(HttpResponse *resp, int status, const void *bytes,
                        size_t len, const char *content_type) {
    resp->status = status;
    resp->content_type = content_type;
    resp->body = (char*)malloc(len);
    if (resp->body) {
        memcpy(resp->body, bytes, len);
        resp->body_len = len;
    }
}

static const char* status_text(int code) {
    switch (code) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 500: return "Internal Server Error";
    default:  return "Unknown";
    }
}

static void send_response(int client_fd, const HttpResponse *resp) {
    char hdr[512];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Connection: close\r\n"
        "\r\n",
        resp->status, status_text(resp->status),
        resp->content_type ? resp->content_type : "text/plain",
        resp->body_len);
    send(client_fd, hdr, n, 0);
    if (resp->body && resp->body_len > 0) {
        size_t sent = 0;
        while (sent < resp->body_len) {
            int k = send(client_fd, resp->body + sent,
                         (int)(resp->body_len - sent), 0);
            if (k <= 0) break;
            sent += k;
        }
    }
}

/* ===== Route dispatch ===== */
static HttpHandler find_handler(const HttpServer *s, const char *method,
                                const char *path) {
    /* Strip query string for matching: "/import?coll=x" → "/import" */
    char clean_path[HTTP_MAX_PATH];
    const char *q = strchr(path, '?');
    if (q) {
        size_t n = (size_t)(q - path);
        if (n >= sizeof(clean_path)) n = sizeof(clean_path) - 1;
        memcpy(clean_path, path, n);
        clean_path[n] = 0;
    } else {
        strncpy(clean_path, path, sizeof(clean_path)-1);
        clean_path[sizeof(clean_path)-1] = 0;
    }

    for (int i = 0; i < s->n_routes; i++) {
        if (strcmp(s->routes[i].method, method) != 0) continue;
        if (strcmp(s->routes[i].path_prefix, clean_path) == 0)
            return s->routes[i].handler;
    }
    return NULL;
}

/* ===== Serve single connection ===== */
static void handle_connection(HttpServer *s, int client_fd, void *ctx) {
    /* Read request: headers + body up to Content-Length */
    static char buf[HTTP_MAX_HEADERS + HTTP_MAX_BODY];
    size_t total = 0;
    size_t header_end = 0;
    HttpRequest req;
    memset(&req, 0, sizeof(req));

    /* Read until headers complete */
    while (total < sizeof(buf)) {
        int n = recv(client_fd, buf + total, (int)(sizeof(buf) - total), 0);
        if (n <= 0) break;
        total += n;
        if (parse_request(buf, total, &req, &header_end) == 0) break;
    }
    if (header_end == 0) {
        CLOSE_SOCK(client_fd);
        return;
    }

    /* Read more if body incomplete */
    size_t body_have = total - header_end;
    while (body_have < req.body_len && total < sizeof(buf)) {
        int n = recv(client_fd, buf + total, (int)(sizeof(buf) - total), 0);
        if (n <= 0) break;
        total += n;
        body_have = total - header_end;
    }
    req.body = (req.body_len > 0) ? buf + header_end : NULL;

    /* CORS preflight */
    HttpResponse resp;
    memset(&resp, 0, sizeof(resp));
    if (strcmp(req.method, "OPTIONS") == 0) {
        resp.status = 204;
        resp.content_type = "text/plain";
    } else {
        HttpHandler h = find_handler(s, req.method, req.path);
        if (h) {
            h(&req, &resp, ctx);
        } else {
            http_respond_text(&resp, 404, "not found\n", "text/plain");
        }
    }

    send_response(client_fd, &resp);
    free(resp.body);
    CLOSE_SOCK(client_fd);
}

/* ===== Main loop ===== */
int http_server_run(HttpServer *s, void *ctx) {
    s->running = 1;
    printf("HTTP server listening on http://127.0.0.1:%d\n", s->port);
    fflush(stdout);
    while (s->running) {
        struct sockaddr_in ca;
        socklen_t cal = sizeof(ca);
        SOCKET cfd = accept(s->listen_fd, (struct sockaddr*)&ca, &cal);
        if (cfd == INVALID_SOCKET) {
            if (!s->running) break;
            continue;
        }
        handle_connection(s, (int)cfd, ctx);
    }
    CLOSE_SOCK(s->listen_fd);
    sock_cleanup();
    return 0;
}

void http_server_stop(HttpServer *s) {
    s->running = 0;
    /* shutdown() để accept() trả về trên thread khác — chưa implement */
}
