/*
 * TLS for the HTTP client in user programs, with BearSSL (third_party/bearssl).
 *
 * Trust anchors are the Mozilla root certificates in
 * /etc/ssl/certs/ca-certificates.crt, decoded once (PEM, then X.509 for the
 * subject name and public key). A connection runs BearSSL's full client
 * profile (TLS 1.0-1.2, every supported cipher suite) with the standard
 * X.509 chain validation against those roots and the server name; the
 * certificates' validity periods are checked against the system clock,
 * which SNTP keeps right. Records go over the TCP connection from
 * net_shim.c, with the HTTP client's timeouts and cancel flag.
 */
#include <bearssl.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CA_BUNDLE "/etc/ssl/certs/ca-certificates.crt"

struct tcp_conn;
int64_t tcp_send(struct tcp_conn *c, const void *data, size_t len, int timeout_ms, volatile bool *cancel);
int64_t tcp_recv(struct tcp_conn *c, void *buf, size_t len, int timeout_ms, volatile bool *cancel);

/* ---- trust anchors ---- */
static br_x509_trust_anchor *anchors;
static size_t nanchors;
static pthread_once_t anchors_once = PTHREAD_ONCE_INIT;

struct bytes { unsigned char *d; size_t len, cap; };
static void bytes_append(void *ctx, const void *data, size_t len) {
    struct bytes *b = ctx;
    if (b->len + len > b->cap) {
        size_t nc = (b->len + len) * 2;
        unsigned char *nd = realloc(b->d, nc);
        if (!nd) return;
        b->d = nd; b->cap = nc;
    }
    memcpy(b->d + b->len, data, len);
    b->len += len;
}
static void *dup_bytes(const void *p, size_t n) { void *d = malloc(n ? n : 1); if (d) memcpy(d, p, n); return d; }

/* one DER certificate -> a trust anchor (its subject name and public key) */
static void add_anchor(const unsigned char *der, size_t len) {
    struct bytes dn = { 0 };
    br_x509_decoder_context dc;
    br_x509_decoder_init(&dc, bytes_append, &dn);
    br_x509_decoder_push(&dc, der, len);
    br_x509_pkey *pk = br_x509_decoder_get_pkey(&dc);
    if (!pk || !dn.len) { free(dn.d); return; }
    br_x509_trust_anchor *na = realloc(anchors, (nanchors + 1) * sizeof *anchors);
    if (!na) { free(dn.d); return; }
    anchors = na;
    br_x509_trust_anchor *ta = &anchors[nanchors];
    memset(ta, 0, sizeof *ta);
    ta->dn.data = dn.d; ta->dn.len = dn.len;
    ta->flags = br_x509_decoder_isCA(&dc) ? BR_X509_TA_CA : 0;
    ta->pkey.key_type = pk->key_type;
    if (pk->key_type == BR_KEYTYPE_RSA) {
        ta->pkey.key.rsa.n = dup_bytes(pk->key.rsa.n, pk->key.rsa.nlen); ta->pkey.key.rsa.nlen = pk->key.rsa.nlen;
        ta->pkey.key.rsa.e = dup_bytes(pk->key.rsa.e, pk->key.rsa.elen); ta->pkey.key.rsa.elen = pk->key.rsa.elen;
    } else if (pk->key_type == BR_KEYTYPE_EC) {
        ta->pkey.key.ec.curve = pk->key.ec.curve;
        ta->pkey.key.ec.q = dup_bytes(pk->key.ec.q, pk->key.ec.qlen); ta->pkey.key.ec.qlen = pk->key.ec.qlen;
    } else { free(dn.d); return; }
    nanchors++;
}

static void load_anchors(void) {
    FILE *f = fopen(CA_BUNDLE, "rb");
    if (!f) { fprintf(stderr, "tls: %s: %s\n", CA_BUNDLE, strerror(errno)); return; }
    br_pem_decoder_context pc;
    br_pem_decoder_init(&pc);
    struct bytes der = { 0 };
    bool in_cert = false;
    unsigned char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        unsigned char *p = buf;
        while (n) {
            size_t k = br_pem_decoder_push(&pc, p, n);
            p += k; n -= k;
            switch (br_pem_decoder_event(&pc)) {
            case BR_PEM_BEGIN_OBJ:
                in_cert = !strcmp(br_pem_decoder_name(&pc), "CERTIFICATE");
                der.len = 0;
                br_pem_decoder_setdest(&pc, in_cert ? bytes_append : NULL, &der);
                break;
            case BR_PEM_END_OBJ:
                if (in_cert && der.len) add_anchor(der.d, der.len);
                in_cert = false;
                break;
            case BR_PEM_ERROR:
                br_pem_decoder_init(&pc);   /* skip the damaged object */
                break;
            }
        }
    }
    fclose(f);
    free(der.d);
}

/* ---- connections ---- */
struct tls {
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    br_sslio_context io;
    struct tcp_conn *c;
    int timeout;
    volatile bool *cancel;
    int64_t io_err;                  /* what the socket last reported (for messages) */
    unsigned char buf[BR_SSL_BUFSIZE_BIDI];
};

static int low_read(void *ctx, unsigned char *buf, size_t len) {
    struct tls *t = ctx;
    int64_t n = tcp_recv(t->c, buf, len, t->timeout, t->cancel);
    if (n <= 0) { t->io_err = n ? n : -EPIPE; return -1; }
    return (int)n;
}
static int low_write(void *ctx, const unsigned char *buf, size_t len) {
    struct tls *t = ctx;
    int64_t n = tcp_send(t->c, buf, len, t->timeout, t->cancel);
    if (n <= 0) { t->io_err = n ? n : -EPIPE; return -1; }
    return (int)n;
}

static const char *describe(int err) {
    switch (err) {
    case BR_ERR_X509_NOT_TRUSTED: return "the certificate is not signed by a trusted authority";
    case BR_ERR_X509_BAD_SERVER_NAME: return "the certificate is for a different site";
    case BR_ERR_X509_EXPIRED: return "the certificate has expired or is not valid yet (is the clock right?)";
    case BR_ERR_X509_NOT_CA: return "the certificate chain is not valid";
    case BR_ERR_X509_FORBIDDEN_KEY_USAGE: case BR_ERR_X509_WEAK_PUBLIC_KEY: return "the certificate's key is not acceptable";
    case BR_ERR_BAD_VERSION: return "the server needs a TLS version this browser does not speak";
    case BR_ERR_BAD_CIPHER_SUITE: case BR_ERR_UNSUPPORTED_VERSION: return "the server offered no cipher suite in common";
    case BR_ERR_IO: return "the connection was closed during the handshake";
    }
    if (err >= BR_ERR_RECV_FATAL_ALERT && err < BR_ERR_RECV_FATAL_ALERT + 256) return "the server rejected the handshake";
    return "the TLS handshake failed";
}

bool http_tls_available(void) {
    pthread_once(&anchors_once, load_anchors);
    return nanchors > 0;
}

void *http_tls_open(struct tcp_conn *c, const char *host, int timeout_ms, volatile bool *cancel, char *why, size_t whysz) {
    pthread_once(&anchors_once, load_anchors);
    if (!nanchors) { snprintf(why, whysz, "no trusted root certificates (%s)", CA_BUNDLE); return NULL; }
    struct tls *t = calloc(1, sizeof *t);
    if (!t) { snprintf(why, whysz, "out of memory"); return NULL; }
    t->c = c; t->timeout = timeout_ms; t->cancel = cancel;
    br_ssl_client_init_full(&t->sc, &t->xc, anchors, nanchors);
    br_ssl_engine_set_buffer(&t->sc.eng, t->buf, sizeof t->buf, 1);
    if (!br_ssl_client_reset(&t->sc, host, 0)) { snprintf(why, whysz, "%s", describe(br_ssl_engine_last_error(&t->sc.eng))); free(t); return NULL; }
    br_sslio_init(&t->io, &t->sc.eng, low_read, t, low_write, t);
    /* flushing with nothing queued runs the handshake to completion */
    if (br_sslio_flush(&t->io) < 0) {
        int err = br_ssl_engine_last_error(&t->sc.eng);
        if (err == BR_ERR_IO && t->io_err == -ETIMEDOUT) snprintf(why, whysz, "the server did not answer");
        else if (err == BR_ERR_IO && t->io_err == -125) snprintf(why, whysz, "cancelled");
        else snprintf(why, whysz, "%s (BearSSL error %d)", describe(err), err);
        free(t);
        return NULL;
    }
    return t;
}

int64_t http_tls_send(void *tp, const void *data, size_t len, int timeout_ms, volatile bool *cancel) {
    struct tls *t = tp;
    t->timeout = timeout_ms; t->cancel = cancel;
    if (br_sslio_write_all(&t->io, data, len) < 0 || br_sslio_flush(&t->io) < 0) return t->io_err ? t->io_err : -EPROTO;
    return (int64_t)len;
}

int64_t http_tls_recv(void *tp, void *buf, size_t len, int timeout_ms, volatile bool *cancel) {
    struct tls *t = tp;
    t->timeout = timeout_ms; t->cancel = cancel;
    int n = br_sslio_read(&t->io, buf, len);
    if (n > 0) return n;
    int err = br_ssl_engine_last_error(&t->sc.eng);
    /* close_notify, or (as many servers do) the TCP connection simply closing: end of the stream */
    if (err == BR_ERR_OK || (err == BR_ERR_IO && t->io_err == -EPIPE)) return 0;
    return t->io_err && t->io_err != -EPIPE ? t->io_err : -EPROTO;
}

void http_tls_close(void *tp, bool clean) {
    struct tls *t = tp;
    if (!t) return;
    if (clean) { t->timeout = 1000; br_sslio_close(&t->io); }
    free(t);
}
