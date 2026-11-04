/* The kernel's HTTP client speaks plain HTTP only: TLS lives in user programs (user/ui/tls.c). */
#include "net.h"
#include "string.h"

bool http_tls_available(void) { return false; }
void *http_tls_open(struct tcp_conn *c, const char *host, int timeout_ms, volatile bool *cancel, char *why, size_t whysz) {
    strlcpy(why, "not supported in the kernel", whysz);
    return NULL;
}
int64_t http_tls_send(void *tls, const void *data, size_t len, int timeout_ms, volatile bool *cancel) { return -EPROTO; }
int64_t http_tls_recv(void *tls, void *buf, size_t len, int timeout_ms, volatile bool *cancel) { return -EPROTO; }
void http_tls_close(void *tls, bool clean) {}
