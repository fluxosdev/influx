/* wget: fetch a URL over HTTP or HTTPS (the Browser's HTTP client and BearSSL) into a file or stdout. */
#include "net.h"
#include <stdio.h>
#include <string.h>

static void usage(void) { fprintf(stderr, "usage: wget [-q] [-O file|-] URL\n"); }

int main(int argc, char **argv) {
    const char *out = NULL, *url = NULL;
    bool quiet = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-q")) quiet = true;
        else if (!strcmp(argv[i], "-O") && i + 1 < argc) out = argv[++i];
        else if (argv[i][0] == '-' && argv[i][1]) { usage(); return 2; }
        else url = argv[i];
    }
    if (!url) { usage(); return 2; }
    char full[2048];
    snprintf(full, sizeof full, "%s%s", strstr(url, "://") ? "" : "http://", url);
    if (!out) { const char *s = strrchr(full, '/'); out = s && s[1] && s > strstr(full, "://") + 2 ? s + 1 : "index.html"; }
    struct http_response r;
    int e = http_get(full, &r, NULL);
    if (e < 0) { fprintf(stderr, "wget: %s\n", r.error[0] ? r.error : "failed"); http_response_free(&r); return 1; }
    if (!quiet) fprintf(stderr, "%s: HTTP %d %s, %zu bytes\n", r.final_url, r.status, r.status_text, r.body_len);
    FILE *f = strcmp(out, "-") ? fopen(out, "wb") : stdout;
    if (!f) { perror(out); http_response_free(&r); return 1; }
    fwrite(r.body, 1, r.body_len, f);
    if (f != stdout) fclose(f);
    int status = r.status >= 400 ? 1 : 0;
    http_response_free(&r);
    return status;
}
