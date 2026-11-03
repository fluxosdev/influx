/*
 * Browser: a web browser for HTTP pages. Pages load in a worker thread (fetch
 * and parse), the window thread lays them out and paints them; images are
 * fetched afterwards by a second worker and the page reflows as they arrive.
 * Supports links, history, forms (GET and POST), fragments, text and image
 * documents, local files (file://), about:home and View Source.
 */
#include "ui.h"
#include "net.h"
#include "html.h"
#include "fs.h"
#include "mm.h"
#include "string.h"

#define TOOLBAR_H 40
#define CHROME_H (TITLEBAR_H + TOOLBAR_H)
#define STATUS_H 22
#define SCROLLBAR_W 12
#define HIST_MAX 100
#define VISITED_MAX 512
#define HOME_URL "about:home"
#define SEARCH_URL "http://wiby.me/?q="
#define MAX_IMAGES 80
#define IMAGE_PX_BUDGET (64u << 20)   /* decoded pixels kept per page */

enum { JOB_PAGE, JOB_IMAGES };

struct browser;
struct job {
    int refs, kind, gen;
    volatile bool cancel;
    struct browser *b;
    char url[2048];
    char *post_body; size_t post_len;
    /* progress, written by the worker */
    volatile const char *stage; volatile size_t done, total;
    /* page result */
    struct html_doc *doc;
    char final_url[2048];
    /* image results */
    char **urls; int nurls;
    struct canvas **imgs; bool *finished, *applied;
    size_t px_left;                 /* remaining decoded-pixel budget */
};

struct browser {
    struct window *w;
    struct mutex lock;              /* job hand-off between the workers and the window thread */
    struct html_doc *doc;
    struct layout *lay;
    int scroll_y, lay_width;
    struct textfield addr;
    struct form_field *focus;
    char *hist[HIST_MAX]; int hist_scroll[HIST_MAX]; int hist_len, hist_pos;
    struct job *page_job, *img_job;
    struct job *page_ready;         /* finished page waiting to be installed */
    bool event_posted;
    int gen, workers;
    int hover_link, pressed;
    bool dragging_scroll; int drag_off;
    bool relayout_pending; uint64_t last_layout;
    char *visited[VISITED_MAX]; int nvisited, visited_next;
    char status[256];
    char pending_fragment[256];
    uint64_t load_start;
    int restore_scroll;
};


enum { BCMD_RELOAD = CMD_APP + 1, BCMD_STOP, BCMD_BACK, BCMD_FORWARD, BCMD_HOME, BCMD_LOCATION, BCMD_SOURCE, BCMD_OPEN_FILE, BCMD_SAVE };
static const struct menu_item file_items[] = {
    MENU("New Window", CMD_NEW_WINDOW, 'n', MOD_CMD), MENU("Open Location…", BCMD_LOCATION, 'l', MOD_CMD), MENU("Open File…", CMD_OPEN, 'o', MOD_CMD),
    MENU_SEP, MENU("Save Page As…", BCMD_SAVE, 's', MOD_CMD), MENU_SEP, MENU("Close Window", CMD_CLOSE, 'w', MOD_CMD), MENU("Quit Browser", CMD_QUIT, 'q', MOD_CMD) };
static const struct menu_item edit_items[] = { MENU("Cut", CMD_CUT, 'x', MOD_CMD), MENU("Copy", CMD_COPY, 'c', MOD_CMD), MENU("Paste", CMD_PASTE, 'v', MOD_CMD), MENU("Select All", CMD_SELECT_ALL, 'a', MOD_CMD) };
static const struct menu_item view_items[] = { MENU("Reload Page", BCMD_RELOAD, 'r', MOD_CMD), MENU("Stop", BCMD_STOP, '.', MOD_CMD), MENU_SEP, MENU("View Source", BCMD_SOURCE, 'u', MOD_CMD) };
static const struct menu_item history_items[] = { MENU("Back", BCMD_BACK, '[', MOD_CMD), MENU("Forward", BCMD_FORWARD, ']', MOD_CMD), MENU("Home", BCMD_HOME, 'h', MOD_CMD | MOD_SHIFT) };
static const struct menu menus[] = { { "File", file_items, ARRAY_LEN(file_items) }, { "Edit", edit_items, ARRAY_LEN(edit_items) }, { "View", view_items, ARRAY_LEN(view_items) }, { "History", history_items, ARRAY_LEN(history_items) } };
static const struct app_menus browser_menus = { menus, ARRAY_LEN(menus) };

/* ---- geometry ---- */
static struct rect page_rect(struct browser *b) { return rect_make(0, CHROME_H, b->w->frame.w - SCROLLBAR_W, b->w->frame.h - CHROME_H - STATUS_H); }
static struct rect btn_rect(int i) { return rect_make(10 + i * 34, TITLEBAR_H + 6, 30, 28); }
static struct rect addr_rect(struct browser *b) { return rect_make(10 + 3 * 34 + 6, TITLEBAR_H + 6, b->w->frame.w - (10 + 3 * 34 + 6) - 12, 28); }
static int max_scroll(struct browser *b) { return b->lay ? MAX(0, b->lay->height - page_rect(b).h) : 0; }
static void clamp_scroll(struct browser *b) { b->scroll_y = CLAMP(b->scroll_y, 0, max_scroll(b)); }

/* ---- visited links ---- */
static bool is_visited(void *ctx, const char *url) {
    struct browser *b = ctx;
    for (int i = 0; i < b->nvisited; i++) if (!strcmp(b->visited[i], url)) return true;
    return false;
}
static void mark_visited(struct browser *b, const char *url) {
    for (int i = 0; i < b->nvisited; i++) if (!strcmp(b->visited[i], url)) return;
    if (b->nvisited < VISITED_MAX) b->visited[b->nvisited++] = kstrdup(url);
    else { kfree(b->visited[b->visited_next]); b->visited[b->visited_next] = kstrdup(url); b->visited_next = (b->visited_next + 1) % VISITED_MAX; }
}

/* ---- jobs ---- */
static void job_release(struct browser *b, struct job *j) {
    if (!j) return;
    mutex_lock(&b->lock);
    bool last = --j->refs == 0;
    mutex_unlock(&b->lock);
    if (!last) return;
    if (j->doc) html_free(j->doc);
    for (int i = 0; i < j->nurls; i++) { kfree(j->urls[i]); if (j->imgs && !j->applied[i]) canvas_destroy(j->imgs[i]); }
    kfree(j->urls); kfree(j->imgs); kfree(j->finished); kfree(j->applied);
    kfree(j->post_body);
    kfree(j);
}
static void post_update(struct browser *b) {
    /* caller holds b->lock */
    if (b->event_posted) return;
    b->event_posted = true;
    struct event e = { .type = EV_USER };
    window_post_event(b->w, &e);
}
static void job_progress(void *ctx, const char *stage, size_t done, size_t total) {
    struct job *j = ctx;
    j->stage = stage; j->done = done; j->total = total;
}

static void html_escape(const char *in, char *out, size_t outsz) {
    size_t o = 0;
    for (; *in && o + 7 < outsz; in++) {
        const char *rep = *in == '<' ? "&lt;" : *in == '>' ? "&gt;" : *in == '&' ? "&amp;" : *in == '"' ? "&quot;" : NULL;
        if (rep) { size_t l = strlen(rep); memcpy(out + o, rep, l); o += l; } else out[o++] = *in;
    }
    out[o] = 0;
}

static struct html_doc *error_page(const char *url, const char *title, const char *message, bool offer_proxy) {
    char *html = kmalloc(8192), eurl[2200], emsg[600], proxy[4800] = "";
    html_escape(url, eurl, sizeof eurl);
    html_escape(message, emsg, sizeof emsg);
    if (offer_proxy) {
        char enc[2048], ehost[2200]; url_encode_component(url, enc, sizeof enc);
        html_escape(strstr(url, "://") ? strstr(url, "://") + 3 : url, ehost, sizeof ehost);
        snprintf(proxy, sizeof proxy, "<p>Many sites only answer over HTTPS. A text-only proxy such as "
                 "<a href=\"http://frogfind.com/read.php?a=%s\">FrogFind</a> can fetch the page for you, "
                 "or try the plain <a href=\"http://%s\">http://</a> address.</p>", enc, ehost);
    }
    snprintf(html, 8192,
             "<html><head><title>%s</title></head><body bgcolor=\"#F5F5F7\"><br><br>"
             "<table align=center width=560 cellpadding=24 cellspacing=0 bgcolor=\"#FFFFFF\" border=0><tr><td>"
             "<h2>%s</h2><p>%s</p>%s<p><font color=\"#86868B\" size=2>%s</font></p>"
             "</td></tr></table></body></html>", title, title, emsg, proxy, eurl);
    struct html_doc *d = html_parse(html, strlen(html), "utf-8", url);
    kfree(html);
    return d;
}

static struct html_doc *home_page(void) {
    char *html = kmalloc(8192), net[256];
    struct netif *n = netif_default();
    if (!n) strcpy(net, "<font color=\"#C0392B\">No network adapter was found.</font> Start QEMU with <tt>-device ne2k_pci,netdev=n0 -netdev user,id=n0</tt>.");
    else if (!n->configured) snprintf(net, sizeof net, "%s (%s) is waiting for an address from DHCP…", n->name, n->driver);
    else { char ip[16]; ip_to_str(n->ip, ip); snprintf(net, sizeof net, "Connected through <b>%s</b> (%s), address %s.", n->name, n->driver, ip); }
    snprintf(html, 8192,
             "<html><head><title>Start Page</title></head><body bgcolor=\"#F5F5F7\">"
             "<br><center><h1>inFlux Browser</h1>"
             "<form action=\"" SEARCH_URL "\" method=get><input type=text name=q size=48 placeholder=\"Search the small web with wiby.me\"> <input type=submit value=Search></form>"
             "<p><font color=\"#6E6E73\" size=2>%s</font></p></center><br>"
             "<table align=center width=620 cellpadding=18 cellspacing=0 bgcolor=\"#FFFFFF\"><tr><td>"
             "<h3>Places to visit</h3><ul>"
             "<li><a href=\"http://info.cern.ch/hypertext/WWW/TheProject.html\">The World Wide Web project</a> — the first web site, CERN 1991</li>"
             "<li><a href=\"http://68k.news/\">68k.news</a> — headlines for vintage computers</li>"
             "<li><a href=\"http://textfiles.com/\">textfiles.com</a> — BBS-era text files</li>"
             "<li><a href=\"http://wiby.me/\">wiby.me</a> — a search engine for classic web pages</li>"
             "<li><a href=\"http://danluu.com/\">danluu.com</a> — essays, in plain HTML</li>"
             "<li><a href=\"http://example.com/\">example.com</a></li>"
             "</ul><h3>About</h3><p>This browser speaks HTTP/1.1 over inFlux's own TCP/IP stack and NE2000 driver. "
             "It lays out HTML with tables, lists, forms and images (PNG, JPEG, GIF, BMP). "
             "Secure (HTTPS) sites, JavaScript and style sheets are not supported.</p>"
             "<p><font size=2 color=\"#6E6E73\">⌘L address bar · ⌘R reload · ⌘[ back · ⌘] forward · ⌘U view source</font></p>"
             "</td></tr></table><br></body></html>", net);
    struct html_doc *d = html_parse(html, strlen(html), "utf-8", HOME_URL);
    kfree(html);
    return d;
}

static void basename_for_url(const char *url, char *out, size_t n) {
    struct url u;
    const char *name = "download";
    char tmp[256];
    if (url_parse(url, &u)) {
        strlcpy(tmp, u.path, sizeof tmp);
        char *q = strchr(tmp, '?'); if (q) *q = 0;
        char *s = strrchr(tmp, '/');
        if (s && s[1]) name = s + 1;
    }
    strlcpy(out, name, n);
}

/* turns a finished HTTP response into a document */
static struct html_doc *doc_from_response(struct job *j, struct http_response *r) {
    const char *ct = r->content_type;
    char charset[32] = "";
    const char *cs = strstr(ct, "charset=");
    if (cs) { strlcpy(charset, cs + 8, sizeof charset); for (char *p = charset; *p; p++) if (*p == ';' || *p == ' ' || *p == '"') { *p = 0; break; } }
    const char *url = r->final_url;
    bool html = !strncmp(ct, "text/html", 9) || !strncmp(ct, "application/xhtml", 17);
    if (!ct[0]) {
        /* no type: sniff */
        const char *p = (const char *)r->body; size_t i = 0;
        while (i < r->body_len && i < 512 && isspace(p[i])) i++;
        html = i < r->body_len && p[i] == '<';
    }
    if (html) return html_parse((const char *)r->body, r->body_len, charset[0] ? charset : NULL, url);
    if (!strncmp(ct, "image/", 6) || !ct[0]) {
        struct canvas *img = image_decode(r->body, r->body_len);
        if (img) {
            char *h = kmalloc(2400), e[2100];
            html_escape(url, e, sizeof e);
            snprintf(h, 2400, "<html><body bgcolor=\"#2B2B2E\"><center><img src=\"%s\"></center></body></html>", e);
            struct html_doc *d = html_parse(h, strlen(h), "utf-8", url);
            kfree(h);
            if (d->images) d->images->img = img; else canvas_destroy(img);
            basename_for_url(url, d->title, sizeof d->title);
            snprintf(d->title + strlen(d->title), sizeof d->title - strlen(d->title), " (%d × %d)", img->w, img->h);
            return d;
        }
        if (ct[0]) return error_page(url, "Unsupported image", "The image could not be decoded (PNG, JPEG, GIF and BMP are supported).", false);
    }
    if (!strncmp(ct, "text/", 5) || !strncmp(ct, "application/json", 16) || !strncmp(ct, "application/javascript", 22) || !ct[0])
        return html_from_text((const char *)r->body, r->body_len, url);
    /* anything else: save it to ~/Downloads */
    char name[200], path[PATH_MAX], msg[512];
    basename_for_url(url, name, sizeof name);
    vfs_mkdir("/home/flux/Downloads");
    snprintf(path, sizeof path, "/home/flux/Downloads/%s", name);
    if (vfs_write_file(path, r->body, r->body_len) == 0) {
        snprintf(msg, sizeof msg, "The file (%zu bytes, %s) was saved to %s.", r->body_len, ct, path);
        return error_page(url, "Download complete", msg, false);
    }
    snprintf(msg, sizeof msg, "This %s file cannot be displayed, and saving it to %s failed.", ct, path);
    return error_page(url, "Cannot display file", msg, false);
}

static void page_worker(void *arg) {
    struct job *j = arg;
    struct browser *b = j->b;
    struct html_doc *doc = NULL;
    const char *url = j->url;
    bool source = !strncmp(url, "view-source:", 12);
    if (source) url += 12;
    strlcpy(j->final_url, url, sizeof j->final_url);
    if (!strcmp(url, "about:blank")) doc = html_parse("", 0, "utf-8", url);
    else if (!strcmp(url, HOME_URL)) doc = home_page();
    else if (!strncmp(url, "file://", 7)) {
        void *data; size_t sz;
        const char *path = url + 7;
        if (vfs_read_file(path, &data, &sz) < 0) doc = error_page(url, "File not found", "The file could not be opened.", false);
        else {
            const char *ext = strrchr(path, '.');
            bool html = ext && (!strcasecmp(ext, ".html") || !strcasecmp(ext, ".htm"));
            struct canvas *img = !html ? image_decode(data, sz) : NULL;
            if (img) { canvas_destroy(img); struct http_response r = { .body = data, .body_len = sz }; strlcpy(r.final_url, url, sizeof r.final_url); strcpy(r.content_type, "image/x"); doc = doc_from_response(j, &r); }
            else if (html && !source) doc = html_parse(data, sz, NULL, url);
            else doc = html_from_text(data, sz, url);
            kfree(data);
        }
    } else {
        struct http_request req = { .url = url, .method = j->post_body ? "POST" : "GET", .body = j->post_body, .body_len = j->post_len,
                                    .cancel = &j->cancel, .max_redirects = -1, .progress = job_progress, .progress_ctx = j };
        struct http_response r;
        int rc = http_request(&req, &r);
        if (r.final_url[0]) strlcpy(j->final_url, r.final_url, sizeof j->final_url);
        if (j->cancel) { http_response_free(&r); goto done; }
        if (rc < 0) {
            bool https = rc == -EPROTO && (strstr(r.error, "HTTPS") != NULL);
            const char *title = https ? "Secure connection required" : rc == -ENOENT ? "Server not found" : "Can’t open the page";
            doc = error_page(j->final_url, title, r.error[0] ? r.error : "The page could not be loaded.", https);
        } else if (source) {
            doc = html_from_text((const char *)r.body, r.body_len, j->final_url);
            snprintf(doc->title, sizeof doc->title, "Source of %s", j->final_url);
        } else doc = doc_from_response(j, &r);
        http_response_free(&r);
    }
    if (source && doc && strncmp(doc->title, "Source of", 9)) {
        struct html_doc *s = html_from_text(doc->source ? doc->source : "", doc->source_len, url);
        snprintf(s->title, sizeof s->title, "Source of %s", url);
        html_free(doc); doc = s;
    }
    if (source && doc) { char tmp[2048]; snprintf(tmp, sizeof tmp, "view-source:%s", j->final_url); strlcpy(j->final_url, tmp, sizeof j->final_url); }
done:
    struct job *stale = NULL;
    mutex_lock(&b->lock);
    if (!j->cancel && doc) { j->doc = doc; doc = NULL; j->refs++; stale = b->page_ready; b->page_ready = j; post_update(b); }
    b->workers--;
    mutex_unlock(&b->lock);
    if (doc) html_free(doc);
    job_release(b, stale);
    job_release(b, j);
}

static void image_worker(void *arg) {
    struct job *j = arg;
    struct browser *b = j->b;
    for (int i = 0; i < j->nurls && !j->cancel; i++) {
        struct canvas *img = NULL;
        const char *u = j->urls[i];
        if (!strncmp(u, "file://", 7)) {
            void *data; size_t sz;
            if (vfs_read_file(u + 7, &data, &sz) == 0) { img = image_decode(data, sz); kfree(data); }
        } else if (!strncmp(u, "http://", 7)) {
            struct http_request req = { .url = u, .cancel = &j->cancel, .max_redirects = 4, .max_body = 4 * 1024 * 1024 };
            struct http_response r;
            if (http_request(&req, &r) == 0 && r.status == 200 && r.body_len) img = image_decode(r.body, r.body_len);
            http_response_free(&r);
        }
        if (img) {
            size_t px = (size_t)img->w * img->h;
            if (px > j->px_left) { canvas_destroy(img); img = NULL; } else j->px_left -= px;
        }
        mutex_lock(&b->lock);
        j->imgs[i] = img; j->finished[i] = true;
        j->done = i + 1;
        if (!j->cancel) post_update(b);
        mutex_unlock(&b->lock);
    }
    mutex_lock(&b->lock);
    b->workers--;
    mutex_unlock(&b->lock);
    job_release(b, j);
}

static void cancel_job(struct browser *b, struct job **jp) {
    struct job *j = *jp;
    if (!j) return;
    j->cancel = true;
    *jp = NULL;
    job_release(b, j);
}

static void start_images(struct browser *b) {
    cancel_job(b, &b->img_job);
    int n = 0;
    for (struct html_image *im = b->doc->images; im; im = im->next) if (!im->img && !im->failed) n++;
    if (!n) return;
    n = MIN(n, MAX_IMAGES);
    struct job *j = kzalloc(sizeof *j);
    j->kind = JOB_IMAGES; j->b = b; j->gen = b->gen; j->refs = 2;
    size_t used = 0;
    for (struct html_image *im = b->doc->images; im; im = im->next) if (im->img) used += (size_t)im->img->w * im->img->h;
    j->px_left = used < IMAGE_PX_BUDGET ? IMAGE_PX_BUDGET - used : 0;
    j->urls = kcalloc(n, sizeof(char *)); j->imgs = kcalloc(n, sizeof(struct canvas *));
    j->finished = kcalloc(n, sizeof(bool)); j->applied = kcalloc(n, sizeof(bool));
    for (struct html_image *im = b->doc->images; im && j->nurls < n; im = im->next) if (!im->img && !im->failed) j->urls[j->nurls++] = kstrdup(im->url);
    j->total = j->nurls;
    b->img_job = j;
    mutex_lock(&b->lock); b->workers++; mutex_unlock(&b->lock);
    thread_create(current_thread->proc, "browser-img", image_worker, j);
}

/* ---- navigation ---- */
static void relayout(struct browser *b) {
    struct layout *old = b->lay;
    b->lay = b->doc ? html_layout(b->doc, page_rect(b).w) : NULL;
    b->lay_width = page_rect(b).w;
    layout_free(old);
    b->last_layout = time_ms();
    b->relayout_pending = false;
    b->hover_link = -1;
    clamp_scroll(b);
}

static void set_address(struct browser *b, const char *url) {
    if (b->addr.focused) return;
    textfield_set(&b->addr, url);
}

static void start_load(struct browser *b, const char *url, const char *post, size_t post_len) {
    cancel_job(b, &b->page_job);
    struct job *j = kzalloc(sizeof *j);
    j->kind = JOB_PAGE; j->b = b; j->gen = ++b->gen; j->refs = 2;
    strlcpy(j->url, url, sizeof j->url);
    if (post) { j->post_body = kmalloc(post_len + 1); memcpy(j->post_body, post, post_len); j->post_body[post_len] = 0; j->post_len = post_len; }
    j->stage = "Loading";
    b->page_job = j;
    snprintf(b->status, sizeof b->status, "Loading %s…", url);
    b->load_start = time_ms();
    mutex_lock(&b->lock); b->workers++; mutex_unlock(&b->lock);
    thread_create(current_thread->proc, "browser-load", page_worker, j);
}

static const char *current_url(struct browser *b) { return b->hist_pos >= 0 && b->hist_pos < b->hist_len ? b->hist[b->hist_pos] : ""; }

static bool same_document(const char *a, const char *b) {
    const char *ha = strchr(a, '#'), *hb = strchr(b, '#');
    size_t la = ha ? (size_t)(ha - a) : strlen(a), lb = hb ? (size_t)(hb - b) : strlen(b);
    return la == lb && !strncmp(a, b, la);
}

static void scroll_to_fragment(struct browser *b, const char *frag) {
    if (!b->lay || !frag || !*frag) return;
    int y = layout_anchor_y(b->lay, frag);
    if (y >= 0) { b->scroll_y = y - 8; clamp_scroll(b); }
}

static void push_history(struct browser *b, const char *url) {
    if (b->hist_pos >= 0 && b->hist_pos < b->hist_len) b->hist_scroll[b->hist_pos] = b->scroll_y;
    for (int i = b->hist_pos + 1; i < b->hist_len; i++) kfree(b->hist[i]);
    b->hist_len = b->hist_pos + 1;
    if (b->hist_len == HIST_MAX) { kfree(b->hist[0]); memmove(b->hist, b->hist + 1, (HIST_MAX - 1) * sizeof b->hist[0]); memmove(b->hist_scroll, b->hist_scroll + 1, (HIST_MAX - 1) * sizeof b->hist_scroll[0]); b->hist_len--; }
    b->hist[b->hist_len] = kstrdup(url);
    b->hist_scroll[b->hist_len] = 0;
    b->hist_pos = b->hist_len++;
}

static void navigate(struct browser *b, const char *url, const char *post, size_t post_len) {
    const char *hash = strchr(url, '#');
    if (!post && hash && b->doc && same_document(url, current_url(b))) {
        /* in-page link: no reload */
        push_history(b, url);
        set_address(b, url);
        scroll_to_fragment(b, hash + 1);
        return;
    }
    push_history(b, url);
    strlcpy(b->pending_fragment, hash ? hash + 1 : "", sizeof b->pending_fragment);
    b->restore_scroll = -1;
    set_address(b, url);
    start_load(b, url, post, post_len);
}

static void go_history(struct browser *b, int delta) {
    int np = b->hist_pos + delta;
    if (np < 0 || np >= b->hist_len) return;
    b->hist_scroll[b->hist_pos] = b->scroll_y;
    bool same = b->doc && same_document(b->hist[np], current_url(b));
    b->hist_pos = np;
    const char *url = b->hist[np];
    set_address(b, url);
    if (same) { b->scroll_y = b->hist_scroll[np]; clamp_scroll(b); return; }
    const char *hash = strchr(url, '#');
    strlcpy(b->pending_fragment, hash ? hash + 1 : "", sizeof b->pending_fragment);
    b->restore_scroll = b->hist_scroll[np];
    start_load(b, url, NULL, 0);
}

static void stop_loading(struct browser *b) {
    cancel_job(b, &b->page_job);
    cancel_job(b, &b->img_job);
    strcpy(b->status, "Stopped");
}

static void install_page(struct browser *b, struct job *j) {
    if (b->doc) {
        /* the old document's images are freed with it; drop references held by the old layout first */
        layout_free(b->lay); b->lay = NULL;
        html_free(b->doc);
    }
    cancel_job(b, &b->img_job);
    b->doc = j->doc; j->doc = NULL;
    b->focus = NULL;
    /* the address after redirects replaces the history entry */
    if (b->hist_pos >= 0 && strcmp(b->hist[b->hist_pos], j->final_url)) {
        const char *hash = strchr(b->hist[b->hist_pos], '#');
        char full[2048];
        snprintf(full, sizeof full, "%s%s", j->final_url, hash && !strchr(j->final_url, '#') ? hash : "");
        kfree(b->hist[b->hist_pos]); b->hist[b->hist_pos] = kstrdup(full);
    }
    set_address(b, current_url(b));
    mark_visited(b, current_url(b));
    mark_visited(b, j->final_url);
    b->scroll_y = 0;
    relayout(b);
    if (b->restore_scroll >= 0) { b->scroll_y = b->restore_scroll; clamp_scroll(b); }
    else if (b->pending_fragment[0]) scroll_to_fragment(b, b->pending_fragment);
    window_set_title(b->w, b->doc->title[0] ? b->doc->title : current_url(b));
    if (b->page_job == j) cancel_job(b, &b->page_job);
    if (b->doc->source_len >= 1024) snprintf(b->status, sizeof b->status, "Done — %zu KB in %lu ms", b->doc->source_len / 1024, time_ms() - b->load_start);
    else snprintf(b->status, sizeof b->status, "Done — %lu ms", time_ms() - b->load_start);
    start_images(b);
}

/* applies decoded images; returns true if any arrived */
static bool apply_images(struct browser *b) {
    struct job *j = b->img_job;
    if (!j || !b->doc) return false;
    bool any = false;
    mutex_lock(&b->lock);
    for (int i = 0; i < j->nurls; i++) {
        if (!j->finished[i] || j->applied[i]) continue;
        j->applied[i] = true;
        for (struct html_image *im = b->doc->images; im; im = im->next) {
            if (strcmp(im->url, j->urls[i])) continue;
            if (j->imgs[i]) { im->img = j->imgs[i]; j->imgs[i] = NULL; } else im->failed = true;
            any = true;
            break;
        }
        canvas_destroy(j->imgs[i]); j->imgs[i] = NULL;
    }
    mutex_unlock(&b->lock);
    return any;
}

/* the address bar: URLs, bare host names, local paths or a search */
static void open_address(struct browser *b, const char *in) {
    while (isspace(*in)) in++;
    char url[2048];
    size_t l = strlen(in);
    while (l && isspace(in[l - 1])) l--;
    char t[2048]; memcpy(t, in, MIN(l, sizeof t - 1)); t[MIN(l, sizeof t - 1)] = 0;
    if (!t[0]) return;
    if (strstr(t, "://") || !strncmp(t, "about:", 6) || !strncmp(t, "view-source:", 12)) strlcpy(url, t, sizeof url);
    else if (t[0] == '/') snprintf(url, sizeof url, "file://%s", t);
    else if (!strchr(t, ' ') && (strchr(t, '.') || strchr(t, ':') || !strcmp(t, "localhost"))) snprintf(url, sizeof url, "http://%s", t);
    else { char q[1500]; url_encode_component(t, q, sizeof q); snprintf(url, sizeof url, SEARCH_URL "%s", q); }
    /* host-only URLs get a trailing slash so redirects and relative links resolve */
    struct url u;
    if (url_parse(url, &u) && u.host[0] && !strstr(strstr(url, "://") + 3, "/")) strlcat(url, "/", sizeof url);
    b->addr.focused = false;
    navigate(b, url, NULL, 0);
}

/* ---- forms ---- */
static void append_param(char *out, size_t outsz, const char *name, const char *value) {
    char en[400], ev[1600];
    url_encode_component(name, en, sizeof en);
    url_encode_component(value, ev, sizeof ev);
    if (out[0]) strlcat(out, "&", outsz);
    strlcat(out, en, outsz); strlcat(out, "=", outsz); strlcat(out, ev, outsz);
}

static void submit_form(struct browser *b, struct html_node *form, struct form_field *submitter) {
    char *data = kzalloc(8192);
    for (struct form_field *f = b->doc->fields; f; f = f->next) {
        if (f->form != form || !f->name[0] || html_attr(f->node, "disabled")) continue;
        switch (f->kind) {
        case FF_TEXT: case FF_PASSWORD: case FF_HIDDEN: case FF_TEXTAREA: append_param(data, 8192, f->name, f->value); break;
        case FF_CHECKBOX: case FF_RADIO: if (f->checked) append_param(data, 8192, f->name, f->value); break;
        case FF_SELECT: {
            char text[256]; const char *v;
            select_option_text(f->node, f->selected, text, sizeof text, &v);
            append_param(data, 8192, f->name, v ? v : text);
            break; }
        case FF_SUBMIT: case FF_IMAGE: if (f == submitter) append_param(data, 8192, f->name, f->value); break;
        default: break;
        }
    }
    const char *action = form ? html_attr(form, "action") : NULL;
    const char *method = form ? html_attr(form, "method") : NULL;
    char target[2048];
    url_resolve(b->doc->base, action && *action ? action : b->doc->url, target, sizeof target);
    char *hash = strchr(target, '#'); if (hash) *hash = 0;
    if (method && !strcasecmp(method, "post")) navigate(b, target, data, strlen(data));
    else {
        char *q = strchr(target, '?'); if (q) *q = 0;
        char url[4096];
        snprintf(url, sizeof url, "%s?%s", target, data);
        navigate(b, url, NULL, 0);
    }
    kfree(data);
}

static void click_field(struct browser *b, struct form_field *f, bool shift) {
    switch (f->kind) {
    case FF_TEXT: case FF_PASSWORD: case FF_TEXTAREA: b->focus = f; b->addr.focused = false; return;
    case FF_CHECKBOX: f->checked = !f->checked; break;
    case FF_RADIO:
        for (struct form_field *o = b->doc->fields; o; o = o->next) if (o->kind == FF_RADIO && o->form == f->form && !strcmp(o->name, f->name)) o->checked = false;
        f->checked = true;
        break;
    case FF_SELECT: if (f->noptions) f->selected = (f->selected + (shift ? f->noptions - 1 : 1)) % f->noptions; break;
    case FF_SUBMIT: case FF_IMAGE: submit_form(b, f->form, f); return;
    case FF_RESET:
        for (struct form_field *o = b->doc->fields; o; o = o->next) {
            if (o->form != f->form) continue;
            const char *v = html_attr(o->node, "value");
            if (o->kind == FF_TEXT || o->kind == FF_PASSWORD) strlcpy(o->value, v ? v : "", sizeof o->value);
            if (o->kind == FF_CHECKBOX || o->kind == FF_RADIO) o->checked = html_attr(o->node, "checked") != NULL;
        }
        break;
    }
    b->focus = NULL;
}

static bool field_key(struct browser *b, struct event *ev) {
    struct form_field *f = b->focus;
    if (!f) return false;
    size_t l = strlen(f->value);
    switch (ev->key) {
    case KEY_ESC: b->focus = NULL; return true;
    case KEY_BACKSPACE:
        while (l > 0 && (f->value[l - 1] & 0xC0) == 0x80) l--;
        if (l > 0) f->value[l - 1] = 0;
        return true;
    case KEY_ENTER:
        if (f->kind == FF_TEXTAREA) { if (l + 1 < sizeof f->value) { f->value[l] = '\n'; f->value[l + 1] = 0; } return true; }
        /* implicit submission: the form's first submit button counts as the submitter */
        { struct form_field *sub = NULL;
          for (struct form_field *o = b->doc->fields; o; o = o->next) if (o->form == f->form && (o->kind == FF_SUBMIT || o->kind == FF_IMAGE)) { sub = o; break; }
          b->focus = NULL; submit_form(b, f->form, sub); }
        return true;
    case KEY_TAB: {
        /* next text field */
        struct form_field *start = f->next, *o = start;
        for (; o; o = o->next) if (o->kind == FF_TEXT || o->kind == FF_PASSWORD || o->kind == FF_TEXTAREA) break;
        if (!o) for (o = b->doc->fields; o && o != f; o = o->next) if (o->kind == FF_TEXT || o->kind == FF_PASSWORD || o->kind == FF_TEXTAREA) break;
        if (o) b->focus = o;
        return true; }
    }
    if ((ev->mods & (MOD_CMD | MOD_CTRL)) && (ev->key == 'v' || ev->key == 'V')) {
        char *s = clipboard_get();
        if (s) { for (char *p = s; *p && l + 1 < sizeof f->value; p++) if (*p != '\n' || f->kind == FF_TEXTAREA) f->value[l++] = *p; f->value[l] = 0; kfree(s); }
        return true;
    }
    if (ev->mods & (MOD_CMD | MOD_CTRL)) return false;
    if (ev->ch >= 32) {
        char enc[8]; int n = utf8_encode(ev->ch, enc);
        if (l + n < sizeof f->value) { memcpy(f->value + l, enc, n); f->value[l + n] = 0; }
        return true;
    }
    return true;
}

/* ---- drawing ---- */
static void draw_glyph_button(struct canvas *c, struct rect r, const char *glyph, bool enabled, bool pressed, struct font *f) {
    if (pressed) gfx_fill_rounded_rect(c, r.x, r.y, r.w, r.h, 6, theme.dark ? RGBA(255, 255, 255, 0x30) : RGBA(0, 0, 0, 0x18));
    font_draw_centered(c, f, r, enabled ? theme.text_secondary : theme.text_disabled, glyph);
}

static void draw_reload_icon(struct canvas *c, struct rect r, bool loading, bool pressed) {
    if (pressed) gfx_fill_rounded_rect(c, r.x, r.y, r.w, r.h, 6, theme.dark ? RGBA(255, 255, 255, 0x30) : RGBA(0, 0, 0, 0x18));
    color_t col = theme.text_secondary;
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    if (loading) {
        /* stop: an X */
        for (int d = -1; d <= 0; d++) { gfx_line(c, cx - 5, cy - 5 + d, cx + 5, cy + 5 + d, col); gfx_line(c, cx - 5, cy + 5 + d, cx + 5, cy - 5 + d, col); }
        return;
    }
    /* reload: an open circle with an arrow head */
    int rad = 7;
    for (int a = 40; a < 360; a += 4) {
        /* integer approximation of a circle from a small sine table */
        static const int8_t s16[] = { 0, 17, 34, 50, 64, 77, 87, 94, 98, 100 };
        int ang = a % 360, q = ang / 90, k = (ang % 90) / 10;
        int sx = q == 0 ? s16[k] : q == 1 ? s16[9 - k] : q == 2 ? -s16[k] : -s16[9 - k];
        int cyv = q == 0 ? s16[9 - k] : q == 1 ? -s16[k] : q == 2 ? -s16[9 - k] : s16[k];
        gfx_fill_rect(c, cx + sx * rad / 100, cy - cyv * rad / 100, 2, 2, col);
    }
    int ax = cx + 1, ay = cy - rad;
    gfx_line(c, ax, ay, ax - 4, ay - 3, col); gfx_line(c, ax, ay, ax - 4, ay + 3, col);
    gfx_line(c, ax, ay + 1, ax - 4, ay - 2, col); gfx_line(c, ax, ay + 1, ax - 4, ay + 4, col);
}

static void draw(struct browser *b) {
    struct canvas *c = window_begin_draw(b->w);
    int W = c->w, H = c->h;
    bool loading = b->page_job != NULL;
    /* title row + toolbar */
    gfx_fill_rect(c, 0, 0, W, CHROME_H, theme.titlebar);
    gfx_hline(c, 0, CHROME_H - 1, W, theme.titlebar_line);
    draw_glyph_button(c, btn_rect(0), "‹", b->hist_pos > 0, b->pressed == 1, font_ui_semibold_xl);
    draw_glyph_button(c, btn_rect(1), "›", b->hist_pos < b->hist_len - 1, b->pressed == 2, font_ui_semibold_xl);
    draw_reload_icon(c, btn_rect(2), loading, b->pressed == 3);
    struct rect ar = addr_rect(b);
    textfield_draw(c, &b->addr, ar);
    if (loading || (b->img_job && b->img_job->done < (size_t)b->img_job->nurls)) {
        struct job *j = loading ? b->page_job : b->img_job;
        int pw;
        if (!loading) pw = (int)((int64_t)ar.w * j->done / MAX(1, j->nurls));
        else if (j->total) pw = (int)((int64_t)ar.w * j->done / j->total);
        else pw = ar.w / 6 + (int)((time_ms() / 8) % (ar.w * 5 / 6));
        if (!loading || j->total) gfx_fill_rect(c, ar.x + 4, ar.y + ar.h - 3, MAX(0, MIN(pw, ar.w) - 8), 2, theme.accent);
        else gfx_fill_rect(c, ar.x + MIN(pw, ar.w - ar.w / 6) , ar.y + ar.h - 3, ar.w / 6, 2, theme.accent);
    }
    /* page */
    struct rect pr = page_rect(b);
    struct canvas page = canvas_sub(c, pr);
    if (b->lay) {
        layout_paint(b->lay, &page, b->scroll_y, b->focus, is_visited, b);
    } else gfx_fill(&page, RGB(255, 255, 255));
    gfx_fill_rect(c, W - SCROLLBAR_W, pr.y, SCROLLBAR_W, pr.h, b->lay ? b->lay->bg : RGB(255, 255, 255));
    if (b->lay && b->lay->height > pr.h) ui_scrollbar_v(c, rect_make(W - SCROLLBAR_W, pr.y, SCROLLBAR_W, pr.h), b->lay->height, pr.h, b->scroll_y);
    /* status bar */
    gfx_fill_rect(c, 0, H - STATUS_H, W, STATUS_H, theme.bg_secondary);
    gfx_hline(c, 0, H - STATUS_H, W, theme.separator);
    char st[300];
    if (b->hover_link >= 0 && b->lay && b->hover_link < b->lay->nlinks) strlcpy(st, b->lay->links[b->hover_link], sizeof st);
    else if (loading) {
        struct job *j = b->page_job;
        const char *stage = (const char *)j->stage;
        if (j->done) snprintf(st, sizeof st, "%s %s… %zu KB", stage ? stage : "Loading", j->url, j->done / 1024);
        else snprintf(st, sizeof st, "%s %s…", stage ? stage : "Loading", j->url);
    } else if (b->img_job && b->img_job->done < (size_t)b->img_job->nurls) snprintf(st, sizeof st, "Loading images (%zu of %d)…", b->img_job->done, b->img_job->nurls);
    else strlcpy(st, b->status, sizeof st);
    font_draw_ellipsized(c, font_ui_small, 10, H - STATUS_H + (STATUS_H - font_height(font_ui_small)) / 2, W - 20, theme.text_secondary, st);
    window_end_draw(b->w, NULL);
}

/* ---- input ---- */
static void page_click(struct browser *b, int x, int y, bool shift) {
    if (!b->lay) return;
    int dx = x, dy = y - CHROME_H + b->scroll_y;
    struct display_item *fi = layout_field_at(b->lay, dx, dy);
    if (fi && fi->node->field) { click_field(b, fi->node->field, shift); return; }
    b->focus = NULL;
    int link = layout_link_at(b->lay, dx, dy);
    if (link >= 0) {
        char url[2048];
        strlcpy(url, b->lay->links[link], sizeof url);
        mark_visited(b, url);
        navigate(b, url, NULL, 0);
    }
}

static void scrollbar_drag(struct browser *b, int y) {
    struct rect pr = page_rect(b);
    if (!b->lay || b->lay->height <= pr.h) return;
    int thumb = MAX(30, pr.h * pr.h / b->lay->height);
    int track = pr.h - thumb;
    int pos = y - pr.y - b->drag_off;
    b->scroll_y = track > 0 ? (int)((int64_t)pos * max_scroll(b) / track) : 0;
    clamp_scroll(b);
}

static void handle_command(struct browser *b, int cmd, bool *quit) {
    switch (cmd) {
    case CMD_QUIT: case CMD_CLOSE: *quit = true; break;
    case CMD_NEW_WINDOW: app_launch(APP_BROWSER, NULL); break;
    case BCMD_LOCATION: b->addr.focused = true; b->focus = NULL; b->addr.sel_anchor = 0; b->addr.cursor = b->addr.len; break;
    case BCMD_RELOAD: if (b->hist_len) { b->restore_scroll = b->scroll_y; b->pending_fragment[0] = 0; start_load(b, current_url(b), NULL, 0); } break;
    case BCMD_STOP: stop_loading(b); break;
    case BCMD_BACK: go_history(b, -1); break;
    case BCMD_FORWARD: go_history(b, 1); break;
    case BCMD_HOME: navigate(b, HOME_URL, NULL, 0); break;
    case BCMD_SOURCE: {
        const char *u = current_url(b);
        if (!strncmp(u, "view-source:", 12)) break;
        char s[2100]; snprintf(s, sizeof s, "view-source:%s", u);
        navigate(b, s, NULL, 0);
        break; }
    case CMD_OPEN: {
        char p[PATH_MAX];
        if (dialog_prompt("Open File", "Path of the HTML, text or image file to open:", "/home/flux/Documents/", p, sizeof p) && p[0]) {
            char norm[PATH_MAX], url[PATH_MAX + 8];
            vfs_normalize("/home/flux", p, norm, sizeof norm);
            snprintf(url, sizeof url, "file://%s", norm);
            navigate(b, url, NULL, 0);
        }
        break; }
    case BCMD_SAVE: {
        if (!b->doc || !b->doc->source) break;
        char name[200], def[PATH_MAX], p[PATH_MAX];
        basename_for_url(b->doc->url, name, sizeof name);
        if (!strchr(name, '.') || !strcmp(name, "download")) strlcat(name, ".html", sizeof name);
        vfs_mkdir("/home/flux/Downloads");
        snprintf(def, sizeof def, "/home/flux/Downloads/%s", name);
        if (!dialog_prompt("Save Page As", "Save the page source to:", def, p, sizeof p) || !p[0]) break;
        char norm[PATH_MAX]; vfs_normalize("/home/flux", p, norm, sizeof norm);
        if (vfs_write_file(norm, b->doc->source, b->doc->source_len) < 0) dialog_message("Browser", "The page could not be saved.");
        else snprintf(b->status, sizeof b->status, "Saved to %s", norm);
        break; }
    case CMD_COPY: case CMD_CUT: case CMD_PASTE: case CMD_SELECT_ALL: {
        static const int keys[] = { [CMD_CUT - CMD_CUT] = 'x', [CMD_COPY - CMD_CUT] = 'c', [CMD_PASTE - CMD_CUT] = 'v', [CMD_SELECT_ALL - CMD_CUT] = 'a' };
        struct event e = { .type = EV_KEY_DOWN, .key = keys[cmd - CMD_CUT], .mods = MOD_CMD };
        if (b->addr.focused) textfield_key(&b->addr, &e);
        else if (b->focus) field_key(b, &e);
        else if (cmd == CMD_COPY) clipboard_set(current_url(b), strlen(current_url(b)));
        break; }
    }
}

static void handle_key(struct browser *b, struct event *ev, bool *quit) {
    if (b->addr.focused) {
        if (ev->key == KEY_ENTER) { char t[1024]; strlcpy(t, b->addr.buf, sizeof t); open_address(b, t); return; }
        if (ev->key == KEY_ESC) { b->addr.focused = false; textfield_set(&b->addr, current_url(b)); return; }
        textfield_key(&b->addr, ev);
        return;
    }
    if (b->focus && field_key(b, ev)) return;
    int page_h = page_rect(b).h;
    bool alt = ev->mods & MOD_ALT, shift = ev->mods & MOD_SHIFT;
    switch (ev->key) {
    case KEY_DOWN: b->scroll_y += 40; break;
    case KEY_UP: b->scroll_y -= 40; break;
    case KEY_PGDN: b->scroll_y += page_h - 40; break;
    case KEY_PGUP: b->scroll_y -= page_h - 40; break;
    case KEY_SPACE: b->scroll_y += shift ? -(page_h - 40) : page_h - 40; break;
    case KEY_HOME: b->scroll_y = 0; break;
    case KEY_END: b->scroll_y = max_scroll(b); break;
    case KEY_LEFT: if (alt) go_history(b, -1); break;
    case KEY_RIGHT: if (alt) go_history(b, 1); break;
    case KEY_BACKSPACE: go_history(b, shift ? 1 : -1); break;
    case KEY_ESC: if (b->page_job) stop_loading(b); break;
    case KEY_F5: handle_command(b, BCMD_RELOAD, quit); break;
    }
    clamp_scroll(b);
}

void browser_main(void *arg) {
    struct browser *b = kzalloc(sizeof *b);
    mutex_init(&b->lock);
    b->hist_pos = -1; b->hover_link = -1; b->restore_scroll = -1;
    textfield_init(&b->addr, "");
    b->addr.placeholder = "Search or enter address";
    strcpy(b->status, "Ready");
    b->w = window_create("Browser", 940, 640, WIN_STANDARD | WIN_FULL_TITLE);
    window_set_min_size(b->w, 420, 260);
    window_set_menus(b->w, &browser_menus);
    window_set_timer(b->w, 100);
    const char *start = arg && ((const char *)arg)[0] ? arg : HOME_URL;
    if (start[0] == '/') { char u[PATH_MAX + 8]; snprintf(u, sizeof u, "file://%s", start); navigate(b, u, NULL, 0); }
    else navigate(b, start, NULL, 0);
    draw(b);
    window_show(b->w);
    bool quit = false;
    struct event ev;
    while (!quit && window_wait_event(b->w, &ev, -1)) {
        switch (ev.type) {
        case EV_QUIT: case EV_CLOSE: quit = true; break;
        case EV_MENU: handle_command(b, ev.cmd, &quit); break;
        case EV_OPEN: if (ev.path) { open_address(b, ev.path); kfree(ev.path); } break;
        case EV_RESIZE: if (b->doc && page_rect(b).w != b->lay_width) relayout(b); clamp_scroll(b); break;
        case EV_USER: {
            mutex_lock(&b->lock);
            b->event_posted = false;
            struct job *ready = b->page_ready;
            b->page_ready = NULL;
            mutex_unlock(&b->lock);
            if (ready) { if (ready == b->page_job) install_page(b, ready); job_release(b, ready); }
            if (apply_images(b)) {
                if (time_ms() - b->last_layout > 250) relayout(b);
                else b->relayout_pending = true;
            }
            break; }
        case EV_TIMER:
            if (b->relayout_pending && time_ms() - b->last_layout > 250) relayout(b);
            if (b->img_job && b->img_job->done == (size_t)b->img_job->nurls && !b->relayout_pending) { apply_images(b); cancel_job(b, &b->img_job); }
            break;
        case EV_MOUSE_SCROLL: b->scroll_y += ev.scroll * 48; clamp_scroll(b); break;
        case EV_MOUSE_DOWN: {
            if (ev.button != BTN_LEFT) break;
            struct rect pr = page_rect(b);
            b->pressed = 0;
            for (int i = 0; i < 3; i++) if (rect_contains(btn_rect(i), ev.x, ev.y)) b->pressed = i + 1;
            if (b->pressed) break;
            struct rect ar = addr_rect(b);
            if (rect_contains(ar, ev.x, ev.y)) {
                bool was = b->addr.focused;
                textfield_mouse(&b->addr, ar, ev.x, ev.y);
                if (!was) { b->addr.sel_anchor = 0; b->addr.cursor = b->addr.len; }
                b->focus = NULL;
                break;
            }
            b->addr.focused = false;
            if (ev.x >= b->w->frame.w - SCROLLBAR_W && ev.y >= pr.y && ev.y < pr.y + pr.h && b->lay && b->lay->height > pr.h) {
                int thumb = MAX(30, pr.h * pr.h / b->lay->height);
                int ty = pr.y + (int)((int64_t)(pr.h - thumb) * b->scroll_y / MAX(1, max_scroll(b)));
                b->drag_off = (ev.y >= ty && ev.y < ty + thumb) ? ev.y - ty : thumb / 2;
                b->dragging_scroll = true;
                scrollbar_drag(b, ev.y);
                break;
            }
            if (rect_contains(pr, ev.x, ev.y)) page_click(b, ev.x, ev.y, ev.mods & MOD_SHIFT);
            break; }
        case EV_MOUSE_UP:
            if (b->pressed && rect_contains(btn_rect(b->pressed - 1), ev.x, ev.y)) {
                if (b->pressed == 1) go_history(b, -1);
                else if (b->pressed == 2) go_history(b, 1);
                else if (b->page_job) stop_loading(b);
                else handle_command(b, BCMD_RELOAD, &quit);
            }
            b->pressed = 0;
            b->dragging_scroll = false;
            break;
        case EV_MOUSE_MOVE:
            if (b->dragging_scroll && (ev.buttons & BTN_LEFT)) { scrollbar_drag(b, ev.y); break; }
            if (b->lay && rect_contains(page_rect(b), ev.x, ev.y)) b->hover_link = layout_link_at(b->lay, ev.x, ev.y - CHROME_H + b->scroll_y);
            else b->hover_link = -1;
            break;
        case EV_MOUSE_LEAVE: b->hover_link = -1; break;
        case EV_KEY_DOWN: handle_key(b, &ev, &quit); break;
        default: break;
        }
        if (!quit) draw(b);
    }
    /* stop the workers before the window goes away */
    mutex_lock(&b->lock);
    if (b->page_job) b->page_job->cancel = true;
    if (b->img_job) b->img_job->cancel = true;
    mutex_unlock(&b->lock);
    cancel_job(b, &b->page_job);
    cancel_job(b, &b->img_job);
    for (int i = 0; i < 300; i++) {
        mutex_lock(&b->lock); int n = b->workers; mutex_unlock(&b->lock);
        if (!n) break;
        thread_sleep_ms(10);
    }
    if (b->page_ready) job_release(b, b->page_ready);
    window_destroy(b->w);
    layout_free(b->lay);
    html_free(b->doc);
    for (int i = 0; i < b->hist_len; i++) kfree(b->hist[i]);
    for (int i = 0; i < b->nvisited; i++) kfree(b->visited[i]);
    if (!b->workers) kfree(b);   /* a worker that outlived the wait still references b */
}
