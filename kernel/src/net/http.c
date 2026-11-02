/* HTTP/1.1 client: URLs, redirects, Content-Length / chunked / read-to-close bodies. */
#include "net.h"
#include "mm.h"
#include "string.h"

#define HTTP_UA "Mozilla/5.0 (inFlux " INFLUX_VERSION "; x86_64) InfluxBrowser/1.0"
#define CONNECT_TIMEOUT 10000
#define RECV_TIMEOUT 20000
#define DNS_TIMEOUT 6000

/* ---- URLs ---- */
bool url_parse(const char *s, struct url *u) {
    memset(u, 0, sizeof *u);
    while (isspace(*s)) s++;
    const char *colon = strchr(s, ':');
    const char *slash = strchr(s, '/');
    if (!colon || (slash && slash < colon)) return false;
    size_t sl = colon - s;
    if (sl == 0 || sl >= sizeof u->scheme) return false;
    for (size_t i = 0; i < sl; i++) u->scheme[i] = tolower(s[i]);
    const char *p = colon + 1;
    if (strncmp(p, "//", 2)) {
        /* opaque URL (about:blank, mailto:...) */
        strlcpy(u->path, p, sizeof u->path);
        return true;
    }
    p += 2;
    const char *hend = p;
    while (*hend && *hend != '/' && *hend != '?' && *hend != '#') hend++;
    /* drop user:password@ */
    const char *at = NULL;
    for (const char *q = p; q < hend; q++) if (*q == '@') at = q;
    if (at) p = at + 1;
    const char *pc = NULL;
    for (const char *q = p; q < hend; q++) if (*q == ':') pc = q;
    const char *host_end = pc ? pc : hend;
    size_t hl = host_end - p;
    if (hl >= sizeof u->host) return false;
    for (size_t i = 0; i < hl; i++) u->host[i] = tolower(p[i]);
    if (!strcmp(u->scheme, "https")) u->port = 443;
    else if (!strcmp(u->scheme, "http")) u->port = 80;
    if (pc && pc + 1 < hend) {
        long port = strtol(pc + 1, NULL, 10);
        if (port <= 0 || port > 65535) return false;
        u->port = port;
    }
    const char *path = hend;
    const char *frag = strchr(path, '#');
    size_t pl = frag ? (size_t)(frag - path) : strlen(path);
    if (pl == 0 || path[0] != '/') { u->path[0] = '/'; if (pl >= sizeof u->path - 1) return false; memcpy(u->path + 1, path, pl); u->path[pl + 1] = 0; }
    else { if (pl >= sizeof u->path) return false; memcpy(u->path, path, pl); u->path[pl] = 0; }
    if (u->path[0] == '/' && u->path[1] == '?') { /* "/?q" is fine as is */ }
    return true;
}

static bool has_scheme(const char *s) {
    if (!isalpha(*s)) return false;
    for (const char *p = s; *p; p++) {
        if (*p == ':') return true;
        if (!isalnum(*p) && *p != '+' && *p != '-' && *p != '.') return false;
    }
    return false;
}

/* removes "." and ".." segments from an absolute path (query left untouched) */
static void remove_dots(char *path) {
    char *q = strchr(path, '?');
    char query[2048] = "";
    if (q) { strlcpy(query, q, sizeof query); *q = 0; }
    char out[2048]; size_t ol = 0;
    const char *p = path;
    while (*p) {
        if (!strncmp(p, "/./", 3)) { p += 2; continue; }
        if (!strcmp(p, "/.")) { out[ol++] = '/'; break; }
        if (!strncmp(p, "/../", 4) || !strcmp(p, "/..")) {
            while (ol > 0 && out[ol - 1] != '/') ol--;
            if (ol > 0) ol--;
            p += 3;
            if (!*p) { out[ol++] = '/'; break; }
            continue;
        }
        const char *next = strchr(p + 1, '/');
        size_t n = next ? (size_t)(next - p) : strlen(p);
        if (ol + n >= sizeof out - 1) break;
        memcpy(out + ol, p, n); ol += n; p += n;
    }
    if (ol == 0) out[ol++] = '/';
    out[ol] = 0;
    snprintf(path, 2048, "%s%s", out, query);
}

void url_resolve(const char *base, const char *ref, char *out, size_t outsz) {
    while (isspace(*ref)) ref++;
    if (has_scheme(ref)) { strlcpy(out, ref, outsz); return; }
    struct url b;
    if (!url_parse(base, &b)) { strlcpy(out, ref, outsz); return; }
    char origin[300];
    bool default_port = (!strcmp(b.scheme, "http") && b.port == 80) || (!strcmp(b.scheme, "https") && b.port == 443) || !b.port;
    if (default_port) snprintf(origin, sizeof origin, "%s://%s", b.scheme, b.host);
    else snprintf(origin, sizeof origin, "%s://%s:%u", b.scheme, b.host, b.port);
    if (!strncmp(ref, "//", 2)) { snprintf(out, outsz, "%s:%s", b.scheme, ref); return; }
    char path[2048];
    if (ref[0] == '/') strlcpy(path, ref, sizeof path);
    else if (ref[0] == '?') {
        strlcpy(path, b.path, sizeof path);
        char *q = strchr(path, '?'); if (q) *q = 0;
        strlcat(path, ref, sizeof path);
    } else if (ref[0] == '#' || ref[0] == 0) {
        strlcpy(path, b.path, sizeof path);
        strlcat(path, ref, sizeof path);
        snprintf(out, outsz, "%s%s", origin, path);
        return;
    } else {
        strlcpy(path, b.path, sizeof path);
        char *q = strchr(path, '?'); if (q) *q = 0;
        char *last = strrchr(path, '/');
        if (last) last[1] = 0; else strcpy(path, "/");
        strlcat(path, ref, sizeof path);
    }
    char *frag = strchr(path, '#');
    char fragment[512] = "";
    if (frag) { strlcpy(fragment, frag, sizeof fragment); *frag = 0; }
    remove_dots(path);
    snprintf(out, outsz, "%s%s%s", origin, path, fragment);
}

void url_encode_component(const char *in, char *out, size_t outsz) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < outsz; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') out[o++] = *p;
        else if (*p == ' ') out[o++] = '+';
        else { out[o++] = '%'; out[o++] = hex[*p >> 4]; out[o++] = hex[*p & 15]; }
    }
    out[o] = 0;
}

/* ---- response helpers ---- */
const char *http_header(const struct http_response *r, const char *name, char *out, size_t outsz) {
    if (!r->headers) return NULL;
    size_t nl = strlen(name);
    const char *p = r->headers;
    while (*p) {
        const char *eol = strstr(p, "\r\n");
        size_t ll = eol ? (size_t)(eol - p) : strlen(p);
        if (ll > nl && p[nl] == ':') {
            bool match = true;
            for (size_t i = 0; i < nl; i++) if (tolower(p[i]) != tolower(name[i])) { match = false; break; }
            if (match) {
                const char *v = p + nl + 1;
                while (*v == ' ' || *v == '\t') v++;
                size_t vl = (p + ll) - v;
                while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) vl--;
                if (vl >= outsz) vl = outsz - 1;
                memcpy(out, v, vl); out[vl] = 0;
                return out;
            }
        }
        if (!eol) break;
        p = eol + 2;
    }
    return NULL;
}

void http_response_free(struct http_response *r) {
    kfree(r->headers); kfree(r->body);
    r->headers = NULL; r->body = NULL; r->body_len = 0;
}

struct buf { uint8_t *d; size_t len, cap; };
static bool buf_append(struct buf *b, const void *p, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 16384;
        while (nc < b->len + n + 1) nc *= 2;
        uint8_t *nd = krealloc(b->d, nc);
        if (!nd) return false;
        b->d = nd; b->cap = nc;
    }
    memcpy(b->d + b->len, p, n);
    b->len += n;
    b->d[b->len] = 0;
    return true;
}

static void set_error(struct http_response *r, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(r->error, sizeof r->error, fmt, ap);
    va_end(ap);
}

static const char *strerr(int e) {
    switch (e) {
    case ETIMEDOUT: return "the connection timed out";
    case ECONNREFUSED: return "the server refused the connection";
    case ECONNRESET: return "the connection was reset";
    case ENETUNREACH: return "the network is unreachable (is the adapter configured?)";
    case EHOSTUNREACH: return "the host is unreachable";
    case ECANCELED: return "the request was cancelled";
    case ENOMEM: return "out of memory";
    default: return "network error";
    }
}

/* incremental chunked-body decoder */
struct chunked { size_t pos; size_t remaining; int state; };   /* state: 0 size line, 1 data, 2 CRLF after data, 3 trailers, 4 done */
static void chunked_feed(struct chunked *c, const struct buf *raw, struct buf *body) {
    while (c->state != 4 && c->pos < raw->len) {
        const char *base = (const char *)raw->d;
        if (c->state == 0 || c->state == 2 || c->state == 3) {
            const char *eol = strstr(base + c->pos, "\r\n");
            if (!eol) return;
            size_t line_len = eol - (base + c->pos);
            if (c->state == 0) {
                c->remaining = strtoul(base + c->pos, NULL, 16);
                c->state = c->remaining ? 1 : 3;
            } else if (c->state == 2) c->state = 0;
            else if (line_len == 0) c->state = 4;   /* blank line ends the trailers */
            c->pos = (eol - base) + 2;
        } else {
            size_t n = MIN(c->remaining, raw->len - c->pos);
            buf_append(body, raw->d + c->pos, n);
            c->pos += n; c->remaining -= n;
            if (!c->remaining) c->state = 2;
        }
    }
}

static int fetch_once(const struct http_request *req, const char *url, const char *method, const void *rbody, size_t rbody_len,
                      struct http_response *resp) {
    struct url u;
    if (!url_parse(url, &u) || !u.host[0]) { set_error(resp, "The address “%s” is not valid.", url); return -EINVAL; }
    if (!strcmp(u.scheme, "https")) { set_error(resp, "Secure (HTTPS) connections are not supported."); return -EPROTO; }
    if (strcmp(u.scheme, "http")) { set_error(resp, "The “%s:” scheme is not supported.", u.scheme); return -EPROTO; }
    if (req->progress) req->progress(req->progress_ctx, "Resolving", 0, 0);
    uint32_t ip;
    int r = dns_resolve(u.host, &ip, DNS_TIMEOUT, req->cancel);
    if (r < 0) {
        if (r == -ENOENT) set_error(resp, "The server “%s” could not be found.", u.host);
        else set_error(resp, "Looking up “%s” failed: %s.", u.host, strerr(-r));
        return r;
    }
    if (req->progress) req->progress(req->progress_ctx, "Connecting", 0, 0);
    int err;
    struct tcp_conn *c = tcp_connect(ip, u.port, CONNECT_TIMEOUT, req->cancel, &err);
    if (!c) {
        char ips[16]; ip_to_str(ip, ips);
        set_error(resp, "Could not connect to %s (%s): %s.", u.host, ips, strerr(err));
        return -err;
    }
    char hosthdr[300];
    if (u.port != 80) snprintf(hosthdr, sizeof hosthdr, "%s:%u", u.host, u.port); else strlcpy(hosthdr, u.host, sizeof hosthdr);
    char *head = kmalloc(4096);
    int hl = snprintf(head, 4096, "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: " HTTP_UA "\r\n"
                      "Accept: text/html,application/xhtml+xml,text/plain;q=0.9,image/png,image/jpeg,image/gif,*/*;q=0.5\r\n"
                      "Accept-Encoding: identity\r\nAccept-Language: en\r\nConnection: close\r\n",
                      method, u.path, hosthdr);
    if (rbody) hl += snprintf(head + hl, 4096 - hl, "Content-Type: %s\r\nContent-Length: %zu\r\n", req->content_type ? req->content_type : "application/x-www-form-urlencoded", rbody_len);
    hl += snprintf(head + hl, 4096 - hl, "\r\n");
    int64_t sent = tcp_send(c, head, MIN(hl, 4095));
    kfree(head);
    if (sent >= 0 && rbody && rbody_len) sent = tcp_send(c, rbody, rbody_len);
    if (sent < 0) { tcp_abort(c); set_error(resp, "Sending the request failed: %s.", strerr((int)-sent)); return (int)sent; }

    if (req->progress) req->progress(req->progress_ctx, "Waiting", 0, 0);
    struct buf raw = { 0 }, body = { 0 };
    size_t max_body = req->max_body ? req->max_body : 16 * 1024 * 1024;
    size_t header_end = 0;
    int64_t content_length = -1;
    bool chunked_enc = false, no_body = false, complete = false;
    struct chunked ch = { 0 };
    uint8_t *tmp = kmalloc(8192);
    int result = 0;
    for (;;) {
        int64_t n = tcp_recv(c, tmp, 8192, RECV_TIMEOUT, req->cancel);
        if (n < 0) {
            result = (int)n;
            set_error(resp, "Receiving the response failed: %s.", strerr((int)-n));
            break;
        }
        if (n == 0) { complete = header_end != 0; if (!complete) { result = -EPROTO; set_error(resp, "The server closed the connection without sending a response."); } break; }
        if (!buf_append(&raw, tmp, n)) { result = -ENOMEM; set_error(resp, "The page is too large."); break; }
        if (!header_end) {
            char *e = strstr((char *)raw.d, "\r\n\r\n");
            if (!e) { if (raw.len > 64 * 1024) { result = -EPROTO; set_error(resp, "The server sent malformed headers."); break; } continue; }
            header_end = (uint8_t *)e - raw.d + 4;
            /* status line */
            char *sp = strchr((char *)raw.d, ' ');
            if (strncmp((char *)raw.d, "HTTP/", 5) || !sp) { result = -EPROTO; set_error(resp, "The server did not answer with HTTP."); break; }
            resp->status = atoi(sp + 1);
            char *reason = strchr(sp + 1, ' ');
            char *eol = strstr((char *)raw.d, "\r\n");
            if (reason && reason < eol) { size_t rl = MIN((size_t)(eol - reason - 1), sizeof resp->status_text - 1); memcpy(resp->status_text, reason + 1, rl); resp->status_text[rl] = 0; }
            size_t hlen = header_end - (eol + 2 - (char *)raw.d);
            resp->headers = kmalloc(hlen + 1);
            memcpy(resp->headers, eol + 2, hlen); resp->headers[hlen] = 0;
            char v[256];
            if (http_header(resp, "Content-Length", v, sizeof v)) content_length = strtol(v, NULL, 10);
            if (http_header(resp, "Transfer-Encoding", v, sizeof v) && strstr(v, "chunked")) chunked_enc = true;
            if (http_header(resp, "Content-Type", v, sizeof v)) strlcpy(resp->content_type, v, sizeof resp->content_type);
            if (http_header(resp, "Location", v, sizeof v)) strlcpy(resp->location, v, sizeof resp->location);
            if (resp->status == 100) {   /* interim response: discard it and keep reading */
                memmove(raw.d, raw.d + header_end, raw.len - header_end); raw.len -= header_end;
                header_end = 0; kfree(resp->headers); resp->headers = NULL; resp->location[0] = 0;
                continue;
            }
            no_body = !strcmp(method, "HEAD") || resp->status == 204 || resp->status == 304 || (resp->status >= 100 && resp->status < 200);
            ch.pos = header_end;
        }
        if (no_body) { complete = true; break; }
        if (chunked_enc) {
            chunked_feed(&ch, &raw, &body);
            if (ch.state == 4) { complete = true; break; }
        } else {
            size_t have = raw.len - header_end;
            if (content_length >= 0 && have >= (size_t)content_length) { complete = true; break; }
        }
        size_t got = chunked_enc ? body.len : raw.len - header_end;
        if (got > max_body) { result = -ENOMEM; set_error(resp, "The response is larger than %zu MB.", max_body >> 20); break; }
        if (req->progress) req->progress(req->progress_ctx, "Receiving", got, content_length > 0 ? (size_t)content_length : 0);
    }
    kfree(tmp);
    if (result == 0 && complete) tcp_close(c); else tcp_abort(c);
    if (result == 0 && header_end) {
        if (!chunked_enc && !no_body) {
            size_t blen = raw.len - header_end;
            if (content_length >= 0 && blen > (size_t)content_length) blen = content_length;
            buf_append(&body, raw.d + header_end, blen);
        }
        resp->body = body.d ? body.d : kzalloc(1);
        resp->body_len = body.len;
        body.d = NULL;
    }
    kfree(raw.d); kfree(body.d);
    return result;
}

int http_request(const struct http_request *req, struct http_response *resp) {
    memset(resp, 0, sizeof *resp);
    char *url = kmalloc(2048);
    strlcpy(url, req->url, 2048);
    const char *method = req->method ? req->method : "GET";
    const void *body = req->body; size_t blen = req->body_len;
    int max_redirects = req->max_redirects < 0 ? 8 : req->max_redirects;
    int r;
    for (int hop = 0;; hop++) {
        /* the fragment never goes to the server */
        char *frag = strchr(url, '#'); if (frag) *frag = 0;
        strlcpy(resp->final_url, url, sizeof resp->final_url);
        r = fetch_once(req, url, method, body, blen, resp);
        if (r < 0) break;
        bool redirect = (resp->status == 301 || resp->status == 302 || resp->status == 303 || resp->status == 307 || resp->status == 308) && resp->location[0];
        if (!redirect || hop >= max_redirects) break;
        char *next = kmalloc(2048);
        url_resolve(url, resp->location, next, 2048);
        kfree(url); url = next;
        if (resp->status == 303 || ((resp->status == 301 || resp->status == 302) && strcmp(method, "GET") && strcmp(method, "HEAD"))) { method = "GET"; body = NULL; blen = 0; }
        char err[sizeof resp->error];
        strlcpy(err, resp->error, sizeof err);
        http_response_free(resp);
        memset(resp, 0, sizeof *resp);
        if (!strncmp(url, "https:", 6)) {
            /* the page moved to a secure address we cannot follow */
            strlcpy(resp->final_url, url, sizeof resp->final_url);
            set_error(resp, "The site redirected to a secure (HTTPS) address, which is not supported.");
            r = -EPROTO;
            break;
        }
    }
    kfree(url);
    return r;
}

int http_get(const char *url, struct http_response *resp, volatile bool *cancel) {
    struct http_request req = { .url = url, .cancel = cancel, .max_redirects = -1 };
    return http_request(&req, resp);
}
