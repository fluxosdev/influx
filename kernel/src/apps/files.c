/* Files: a sidebar + icon/list view file manager. */
#include "ui.h"
#include "fs.h"
#include "mm.h"
#include "string.h"

#define SIDEBAR_W 190
#define TOOLBAR_H 44
#define STATUS_H 24
#define LIST_ROW_H 24
#define LIST_HEADER_H 24
#define CELL_W 100
#define CELL_H 96

struct entry { char name[NAME_MAX + 1]; int type; uint64_t size, mtime; };
struct side_item { const char *label; const char *icon; char path[PATH_MAX]; bool header; };

struct files {
    struct window *w;
    char path[PATH_MAX];
    char history[32][PATH_MAX]; int hist_len, hist_pos;
    struct entry *entries; int nentries;
    int selected, anchor;          /* selected index, -1 none */
    bool *sel;                     /* multi-selection bitmap */
    bool list_view, show_hidden;
    int scroll;
    int sort_col; bool sort_desc;
    struct side_item side[24]; int nside;
    uint64_t last_click_time; int last_click_idx;
    int pressed_btn, hover_btn;
    char clip_path[PATH_MAX];
    int dragging_sel;
};

enum { FCMD_NEW_WINDOW = CMD_APP + 1, FCMD_RENAME, FCMD_TRASH, FCMD_INFO, FCMD_ICONS, FCMD_LIST, FCMD_HIDDEN, FCMD_REFRESH,
       FCMD_BACK, FCMD_FORWARD, FCMD_UP, FCMD_HOME, FCMD_COMPUTER, FCMD_GOTO, FCMD_OPEN_TERMINAL, FCMD_EMPTY_TRASH, FCMD_DUPLICATE };

static const struct menu_item file_items[] = {
    MENU("New Files Window", FCMD_NEW_WINDOW, 'n', MOD_CMD), MENU("New Folder", CMD_NEW_FOLDER, 'n', MOD_CMD | MOD_SHIFT), MENU("New Document", CMD_NEW, 'n', MOD_CMD | MOD_ALT), MENU_SEP,
    MENU("Open", CMD_OPEN, 'o', MOD_CMD), MENU("Open in Terminal", FCMD_OPEN_TERMINAL, 't', MOD_CMD | MOD_SHIFT), MENU("Get Info", FCMD_INFO, 'i', MOD_CMD), MENU("Rename…", FCMD_RENAME, KEY_ENTER, 0), MENU("Duplicate", FCMD_DUPLICATE, 'd', MOD_CMD), MENU_SEP,
    MENU("Move to Trash", FCMD_TRASH, KEY_BACKSPACE, MOD_CMD), MENU("Empty Trash…", FCMD_EMPTY_TRASH, KEY_BACKSPACE, MOD_CMD | MOD_SHIFT), MENU_SEP,
    MENU("Close Window", CMD_CLOSE, 'w', MOD_CMD), MENU("Quit Files", CMD_QUIT, 'q', MOD_CMD),
};
static const struct menu_item edit_items[] = { MENU("Copy", CMD_COPY, 'c', MOD_CMD), MENU("Paste", CMD_PASTE, 'v', MOD_CMD), MENU_SEP, MENU("Select All", CMD_SELECT_ALL, 'a', MOD_CMD) };
static const struct menu_item view_items[] = { MENU("as Icons", FCMD_ICONS, '1', MOD_CMD), MENU("as List", FCMD_LIST, '2', MOD_CMD), MENU_SEP, MENU("Show Hidden Files", FCMD_HIDDEN, '.', MOD_CMD | MOD_SHIFT), MENU("Refresh", FCMD_REFRESH, 'r', MOD_CMD) };
static const struct menu_item go_items[] = { MENU("Back", FCMD_BACK, '[', MOD_CMD), MENU("Forward", FCMD_FORWARD, ']', MOD_CMD), MENU("Enclosing Folder", FCMD_UP, KEY_UP, MOD_CMD), MENU_SEP, MENU("Home", FCMD_HOME, 'h', MOD_CMD | MOD_SHIFT), MENU("Computer", FCMD_COMPUTER, 'c', MOD_CMD | MOD_SHIFT), MENU_SEP, MENU("Go to Folder…", FCMD_GOTO, 'g', MOD_CMD | MOD_SHIFT) };
static const struct menu_item help_items[] = { MENU("inFlux Help", CMD_HELP, '?', MOD_CMD) };
static const struct menu menus[] = { { "File", file_items, ARRAY_LEN(file_items) }, { "Edit", edit_items, ARRAY_LEN(edit_items) }, { "View", view_items, ARRAY_LEN(view_items) }, { "Go", go_items, ARRAY_LEN(go_items) }, { "Help", help_items, ARRAY_LEN(help_items) } };
static struct app_menus files_menus = { menus, ARRAY_LEN(menus) };

static void format_size(uint64_t sz, char *out, size_t n) {
    if (sz < 1000) snprintf(out, n, "%lu bytes", sz);
    else if (sz < 1000 * 1000) snprintf(out, n, "%lu KB", (sz + 500) / 1000);
    else if (sz < 1000ULL * 1000 * 1000) snprintf(out, n, "%lu.%lu MB", sz / 1000000, (sz % 1000000) / 100000);
    else snprintf(out, n, "%lu.%lu GB", sz / 1000000000, (sz % 1000000000) / 100000000);
}
static void format_date(uint64_t t, char *out, size_t n) {
    static const char *mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    struct rtc_time d; unix_to_datetime(t + tz_offset_minutes * 60, &d);
    int h12 = d.hour % 12; if (!h12) h12 = 12;
    snprintf(out, n, "%s %d, %d at %d:%02d %s", mon[(d.month - 1) % 12], d.day, d.year, h12, d.minute, d.hour >= 12 ? "PM" : "AM");
}
static const char *kind_of(const struct entry *e) {
    if (e->type == VN_DIR) return "Folder";
    if (e->type == VN_BLK) return "Block device";
    if (e->type == VN_DEV) return "Device";
    const char *icon = icon_for_file(e->name, e->type);
    if (!strcmp(icon, "file-text")) return "Plain Text";
    if (!strcmp(icon, "file-markdown")) return "Markdown";
    if (!strcmp(icon, "file-c") || !strcmp(icon, "file-h")) return "C source";
    if (!strcmp(icon, "file-image")) return "Image";
    if (!strcmp(icon, "file-audio")) return "Audio";
    if (!strcmp(icon, "file-exec")) return "Executable";
    if (!strcmp(icon, "file-script")) return "Shell script";
    return "Document";
}

static int cmp_entries(const struct entry *a, const struct entry *b, int col) {
    if ((a->type == VN_DIR) != (b->type == VN_DIR) && col == 0) return a->type == VN_DIR ? -1 : 1;
    switch (col) {
    case 1: return a->mtime < b->mtime ? -1 : a->mtime > b->mtime;
    case 2: return a->size < b->size ? -1 : a->size > b->size;
    case 3: { int k = strcmp(kind_of(a), kind_of(b)); if (k) return k; }
    }
    return strcasecmp(a->name, b->name);
}

static void load_dir(struct files *f) {
    kfree(f->entries); kfree(f->sel);
    f->entries = NULL; f->nentries = 0; f->sel = NULL;
    struct vnode *d = vfs_lookup(f->path);
    if (!d) return;
    int cap = 64; f->entries = kmalloc(cap * sizeof(struct entry));
    struct dirent de;
    for (int i = 0; vfs_readdir(d, i, &de) == 0; i++) {
        if (!f->show_hidden && de.name[0] == '.') continue;
        if (f->nentries == cap) { cap *= 2; f->entries = krealloc(f->entries, cap * sizeof(struct entry)); }
        struct entry *e = &f->entries[f->nentries++];
        strlcpy(e->name, de.name, sizeof e->name); e->type = de.type; e->size = de.size; e->mtime = de.mtime;
    }
    vnode_unref(d);
    /* insertion sort (small directories) */
    for (int i = 1; i < f->nentries; i++) {
        struct entry t = f->entries[i]; int j = i - 1;
        while (j >= 0 && (cmp_entries(&f->entries[j], &t, f->sort_col) > 0) != f->sort_desc && cmp_entries(&f->entries[j], &t, f->sort_col) != 0) { f->entries[j + 1] = f->entries[j]; j--; }
        f->entries[j + 1] = t;
    }
    f->sel = kzalloc(f->nentries ? f->nentries : 1);
    f->selected = -1; f->scroll = 0;
}

static void build_sidebar(struct files *f) {
    f->nside = 0;
    #define SIDE(l, ic, p, h) do { struct side_item *s = &f->side[f->nside++]; s->label = l; s->icon = ic; strlcpy(s->path, p, PATH_MAX); s->header = h; } while (0)
    SIDE("Favorites", NULL, "", true);
    SIDE("flux", "folder-home", "/home/flux", false);
    SIDE("Desktop", "desktop", "/home/flux/Desktop", false);
    SIDE("Documents", "folder-documents", "/home/flux/Documents", false);
    SIDE("Downloads", "folder-downloads", "/home/flux/Downloads", false);
    SIDE("Pictures", "folder-pictures", "/home/flux/Pictures", false);
    SIDE("Locations", NULL, "", true);
    SIDE("inFlux", "computer", "/", false);
    for (int i = 0; i < vfs_mount_count() && f->nside < 22; i++) {
        char mp[PATH_MAX]; struct vnode *root = vfs_mount_at(i, mp, sizeof mp);
        if (!root || strncmp(mp, "/mnt", 4) != 0) continue;
        if (!f->show_hidden && vfs_basename(mp)[0] == '.') continue;
        SIDE(root->name[0] ? root->name : vfs_basename(mp), "drive", mp, false);
    }
    SIDE("Trash", "trash", "/home/flux/.Trash", false);
}

static void navigate(struct files *f, const char *path, bool push) {
    char norm[PATH_MAX]; vfs_normalize("/", path, norm, sizeof norm);
    struct stat st;
    if (vfs_stat(norm, &st) < 0 || st.type != VN_DIR) { dialog_message("Files", "The folder can’t be found."); return; }
    strlcpy(f->path, norm, sizeof f->path);
    if (push) {
        if (f->hist_pos < f->hist_len - 1) f->hist_len = f->hist_pos + 1;
        if (f->hist_len == 32) { memmove(f->history[0], f->history[1], 31 * PATH_MAX); f->hist_len--; }
        strlcpy(f->history[f->hist_len++], norm, PATH_MAX);
        f->hist_pos = f->hist_len - 1;
    }
    load_dir(f);
    const char *title = !strcmp(norm, "/") ? "inFlux" : !strcmp(norm, "/home/flux") ? "flux" : !strcmp(norm, "/home/flux/.Trash") ? "Trash" : vfs_basename(norm);
    window_set_title(f->w, title);
}

/* ---- geometry ---- */
static struct rect content_rect(struct files *f) { return rect_make(SIDEBAR_W, TITLEBAR_H + TOOLBAR_H, f->w->frame.w - SIDEBAR_W, f->w->frame.h - TITLEBAR_H - TOOLBAR_H - STATUS_H); }
static int cols_per_row(struct files *f) { int c = (content_rect(f).w - 16) / CELL_W; return c < 1 ? 1 : c; }
static int content_height(struct files *f) {
    if (f->list_view) return LIST_HEADER_H + f->nentries * LIST_ROW_H + 8;
    int c = cols_per_row(f); return ((f->nentries + c - 1) / c) * CELL_H + 16;
}
static struct rect item_rect(struct files *f, int i) {
    struct rect cr = content_rect(f);
    if (f->list_view) return rect_make(cr.x, cr.y + LIST_HEADER_H + i * LIST_ROW_H - f->scroll, cr.w, LIST_ROW_H);
    int c = cols_per_row(f);
    return rect_make(cr.x + 8 + (i % c) * CELL_W, cr.y + 8 + (i / c) * CELL_H - f->scroll, CELL_W, CELL_H);
}
static int item_at(struct files *f, int x, int y) {
    struct rect cr = content_rect(f);
    if (!rect_contains(cr, x, y)) return -1;
    for (int i = 0; i < f->nentries; i++) {
        struct rect r = item_rect(f, i);
        if (!f->list_view) r = rect_make(r.x + 10, r.y + 4, r.w - 20, r.h - 10);
        if (rect_contains(r, x, y)) return i;
    }
    return -1;
}
static int clamp_scroll(struct files *f) { int max = content_height(f) - content_rect(f).h; if (max < 0) max = 0; f->scroll = CLAMP(f->scroll, 0, max); return max; }

/* ---- drawing ---- */
static struct rect btn_back(struct files *f) { return rect_make(SIDEBAR_W + 12, TITLEBAR_H + 8, 30, 28); }
static struct rect btn_fwd(struct files *f) { return rect_make(SIDEBAR_W + 44, TITLEBAR_H + 8, 30, 28); }
static struct rect btn_view(struct files *f, int which) { return rect_make(f->w->frame.w - 12 - 72 + which * 36, TITLEBAR_H + 8, 36, 28); }
static struct rect btn_newfolder(struct files *f) { return rect_make(f->w->frame.w - 12 - 72 - 10 - 40, TITLEBAR_H + 8, 40, 28); }

static void draw(struct files *f) {
    struct canvas *c = window_begin_draw(f->w);
    int W = c->w, H = c->h;
    gfx_fill(c, theme.bg);
    /* sidebar */
    gfx_fill_rect(c, 0, 0, SIDEBAR_W, H, theme.sidebar_bg);
    gfx_vline(c, SIDEBAR_W - 1, 0, H, theme.separator);
    int y = TITLEBAR_H + 10;
    for (int i = 0; i < f->nside; i++) {
        struct side_item *s = &f->side[i];
        if (s->header) { font_draw(c, font_ui_semibold, 18, y + 4, theme.text_tertiary, s->label); y += 24; continue; }
        bool sel = !strcmp(s->path, f->path);
        if (sel) gfx_fill_rounded_rect(c, 8, y, SIDEBAR_W - 16, 26, 6, theme.dark ? RGBA(255, 255, 255, 0x22) : RGBA(0, 0, 0, 0x14));
        struct canvas *ic = icon_get(s->icon, 32);
        if (ic) gfx_blit_scaled(c, 18, y + 4, 18, 18, ic);
        font_draw_ellipsized(c, font_ui, 44, y + (26 - font_height(font_ui)) / 2, SIDEBAR_W - 60, theme.text, s->label);
        y += 26;
    }
    /* toolbar */
    gfx_fill_rect(c, SIDEBAR_W, 0, W - SIDEBAR_W, TITLEBAR_H + TOOLBAR_H, theme.dark ? theme.titlebar : theme.titlebar);
    gfx_hline(c, SIDEBAR_W, TITLEBAR_H + TOOLBAR_H - 1, W - SIDEBAR_W, theme.titlebar_line);
    struct rect bb = btn_back(f), bf = btn_fwd(f);
    bool can_back = f->hist_pos > 0, can_fwd = f->hist_pos < f->hist_len - 1;
    if (f->pressed_btn == 1) gfx_fill_rounded_rect(c, bb.x, bb.y, bb.w, bb.h, 6, theme.dark ? RGBA(255, 255, 255, 0x30) : RGBA(0, 0, 0, 0x18));
    if (f->pressed_btn == 2) gfx_fill_rounded_rect(c, bf.x, bf.y, bf.w, bf.h, 6, theme.dark ? RGBA(255, 255, 255, 0x30) : RGBA(0, 0, 0, 0x18));
    font_draw_centered(c, font_ui_semibold_xl, bb, can_back ? theme.text_secondary : theme.text_disabled, "‹");
    font_draw_centered(c, font_ui_semibold_xl, bf, can_fwd ? theme.text_secondary : theme.text_disabled, "›");
    font_draw_ellipsized(c, font_ui_semibold_large, SIDEBAR_W + 86, TITLEBAR_H + 8 + (28 - font_height(font_ui_semibold_large)) / 2, W - SIDEBAR_W - 86 - 180, theme.text, f->w->title);
    /* view switcher (segmented) */
    struct rect v0 = btn_view(f, 0), v1 = btn_view(f, 1);
    gfx_fill_rounded_rect(c, v0.x, v0.y, 72, 28, 6, theme.dark ? RGBA(255, 255, 255, 0x14) : RGBA(0, 0, 0, 0x0C));
    struct rect act = f->list_view ? v1 : v0;
    gfx_fill_rounded_rect(c, act.x + 2, act.y + 2, act.w - 4, act.h - 4, 5, theme.control_bg);
    gfx_draw_rounded_rect(c, act.x + 2, act.y + 2, act.w - 4, act.h - 4, 5, theme.control_border);
    /* icons glyph: 2x2 squares; list glyph: 3 lines */
    int gx = v0.x + 11, gy = v0.y + 8; color_t gc = theme.text_secondary;
    gfx_fill_rounded_rect(c, gx, gy, 5, 5, 1, gc); gfx_fill_rounded_rect(c, gx + 8, gy, 5, 5, 1, gc); gfx_fill_rounded_rect(c, gx, gy + 8, 5, 5, 1, gc); gfx_fill_rounded_rect(c, gx + 8, gy + 8, 5, 5, 1, gc);
    gx = v1.x + 11; for (int i = 0; i < 3; i++) gfx_fill_rect(c, gx, gy + 1 + i * 5, 14, 2, gc);
    struct rect nf = btn_newfolder(f);
    if (f->pressed_btn == 3) gfx_fill_rounded_rect(c, nf.x, nf.y, nf.w, nf.h, 6, theme.dark ? RGBA(255, 255, 255, 0x30) : RGBA(0, 0, 0, 0x18));
    struct canvas *fic = icon_get("folder", 32);
    if (fic) gfx_blit_scaled(c, nf.x + 8, nf.y + 5, 18, 18, fic);
    font_draw(c, font_ui_semibold, nf.x + 27, nf.y + 6, theme.text_secondary, "+");

    /* content */
    struct rect cr = content_rect(f);
    struct canvas sub = canvas_sub(c, cr);
    if (f->list_view) {
        gfx_fill_rect(&sub, 0, 0, cr.w, LIST_HEADER_H, theme.bg);
        int colx[4] = { 10, cr.w - 400, cr.w - 230, cr.w - 130 };
        const char *cols[4] = { "Name", "Date Modified", "Size", "Kind" };
        for (int k = 0; k < 4; k++) {
            char hdr[40]; snprintf(hdr, sizeof hdr, "%s%s", cols[k], f->sort_col == k ? (f->sort_desc ? " ▼" : " ▲") : "");
            font_draw(&sub, font_ui_medium_small, colx[k] + (k ? 0 : 26), 6, theme.text_secondary, hdr);
            if (k) gfx_vline(&sub, colx[k] - 8, 4, LIST_HEADER_H - 8, theme.separator);
        }
        gfx_hline(&sub, 0, LIST_HEADER_H - 1, cr.w, theme.separator);
        struct canvas rows = canvas_sub(c, rect_make(cr.x, cr.y + LIST_HEADER_H, cr.w, cr.h - LIST_HEADER_H));
        for (int i = 0; i < f->nentries; i++) {
            struct entry *e = &f->entries[i];
            int ry = i * LIST_ROW_H - f->scroll;
            if (ry + LIST_ROW_H < 0 || ry > rows.h) continue;
            bool sel = f->sel[i];
            if (i % 2 == 1 && !sel) gfx_fill_rect(&rows, 0, ry, cr.w, LIST_ROW_H, theme.dark ? RGBA(255, 255, 255, 0x08) : RGBA(0, 0, 0, 0x06));
            if (sel) gfx_fill_rect(&rows, 0, ry, cr.w, LIST_ROW_H, theme.selection);
            color_t tc = sel ? theme.accent_text : theme.text, sc = sel ? theme.accent_text : theme.text_secondary;
            struct canvas *ic = icon_get(icon_for_file(e->name, e->type), 32);
            if (ic) gfx_blit_scaled(&rows, colx[0], ry + 3, 18, 18, ic);
            font_draw_ellipsized(&rows, font_ui, colx[0] + 26, ry + (LIST_ROW_H - font_height(font_ui)) / 2, colx[1] - colx[0] - 40, tc, e->name);
            char buf[64]; format_date(e->mtime, buf, sizeof buf);
            font_draw_ellipsized(&rows, font_ui, colx[1], ry + 5, colx[2] - colx[1] - 12, sc, buf);
            if (e->type == VN_DIR) strcpy(buf, "--"); else format_size(e->size, buf, sizeof buf);
            font_draw_aligned(&rows, font_ui, rect_make(colx[2], ry, colx[3] - colx[2] - 14, LIST_ROW_H), sc, buf, 2);
            font_draw_ellipsized(&rows, font_ui, colx[3], ry + 5, cr.w - colx[3] - 10, sc, kind_of(e));
        }
    } else {
        for (int i = 0; i < f->nentries; i++) {
            struct entry *e = &f->entries[i];
            struct rect r = item_rect(f, i); r.x -= cr.x; r.y -= cr.y;
            if (r.y + r.h < 0 || r.y > cr.h) continue;
            bool sel = f->sel[i];
            struct canvas *ic = icon_get(icon_for_file(e->name, e->type), 64);
            if (sel) gfx_fill_rounded_rect(&sub, r.x + 20, r.y + 2, 60, 60, 6, theme.dark ? RGBA(255, 255, 255, 0x20) : RGBA(0, 0, 0, 0x12));
            if (ic) gfx_blit_scaled(&sub, r.x + 26, r.y + 8, 48, 48, ic);
            /* label: up to 2 lines */
            int lw = font_text_width(font_ui, e->name);
            int ty = r.y + 62;
            if (lw <= CELL_W - 12) {
                if (sel) gfx_fill_rounded_rect(&sub, r.x + (CELL_W - lw) / 2 - 4, ty - 1, lw + 8, font_height(font_ui) + 3, 4, theme.selection);
                font_draw(&sub, font_ui, r.x + (CELL_W - lw) / 2, ty, sel ? theme.accent_text : theme.text, e->name);
            } else {
                int n1 = font_fit_chars(font_ui, e->name, CELL_W - 12);
                int w1 = font_text_width_n(font_ui, e->name, n1);
                if (sel) gfx_fill_rounded_rect(&sub, r.x + 2, ty - 1, CELL_W - 4, font_height(font_ui) * 2 + 4, 4, theme.selection);
                font_draw_n(&sub, font_ui, r.x + (CELL_W - w1) / 2, ty, sel ? theme.accent_text : theme.text, e->name, n1);
                const char *rest = e->name + n1;
                int rw = font_text_width(font_ui, rest); if (rw > CELL_W - 12) rw = CELL_W - 12;
                font_draw_ellipsized(&sub, font_ui, r.x + (CELL_W - rw) / 2, ty + font_height(font_ui) + 1, CELL_W - 12, sel ? theme.accent_text : theme.text, rest);
            }
        }
    }
    if (f->nentries == 0) font_draw_centered(&sub, font_ui_large, rect_make(0, 0, cr.w, cr.h), theme.text_tertiary, "No items");
    ui_scrollbar_v(c, rect_make(cr.x + cr.w - 12, cr.y + (f->list_view ? LIST_HEADER_H : 0), 12, cr.h - (f->list_view ? LIST_HEADER_H : 0)), content_height(f), cr.h, f->scroll);
    /* status bar */
    gfx_fill_rect(c, SIDEBAR_W, H - STATUS_H, W - SIDEBAR_W, STATUS_H, theme.bg_secondary);
    gfx_hline(c, SIDEBAR_W, H - STATUS_H, W - SIDEBAR_W, theme.separator);
    int nsel = 0; for (int i = 0; i < f->nentries; i++) if (f->sel[i]) nsel++;
    char status[128];
    char avail[32]; format_size(pmm_free_bytes(), avail, sizeof avail);
    if (nsel) snprintf(status, sizeof status, "%d of %d selected, %s available", nsel, f->nentries, avail);
    else snprintf(status, sizeof status, "%d item%s, %s available", f->nentries, f->nentries == 1 ? "" : "s", avail);
    font_draw_centered(c, font_ui_small, rect_make(SIDEBAR_W, H - STATUS_H, W - SIDEBAR_W, STATUS_H), theme.text_secondary, status);
    window_end_draw(f->w, NULL);
}

/* ---- actions ---- */
static void select_only(struct files *f, int i) { memset(f->sel, 0, f->nentries); if (i >= 0) f->sel[i] = true; f->selected = i; f->anchor = i; }
static int first_selected(struct files *f) { for (int i = 0; i < f->nentries; i++) if (f->sel[i]) return i; return -1; }
static void ensure_visible(struct files *f, int i) {
    if (i < 0) return;
    struct rect cr = content_rect(f), r = item_rect(f, i);
    int top = cr.y + (f->list_view ? LIST_HEADER_H : 0);
    if (r.y < top) f->scroll -= top - r.y;
    else if (r.y + r.h > cr.y + cr.h) f->scroll += r.y + r.h - (cr.y + cr.h);
    clamp_scroll(f);
}
static void full_path(struct files *f, const char *name, char *out, size_t n) { snprintf(out, n, "%s/%s", !strcmp(f->path, "/") ? "" : f->path, name); }

static void open_entry(struct files *f, int i) {
    if (i < 0 || i >= f->nentries) return;
    char p[PATH_MAX]; full_path(f, f->entries[i].name, p, sizeof p);
    if (f->entries[i].type == VN_DIR) navigate(f, p, true);
    else app_open_path(p);
}

static int copy_tree(const char *src, const char *dst) {
    struct stat st; if (vfs_stat(src, &st) < 0) return -1;
    if (st.type == VN_DIR) {
        vfs_mkdir(dst);
        struct vnode *d = vfs_lookup(src); if (!d) return -1;
        struct dirent de;
        for (int i = 0; vfs_readdir(d, i, &de) == 0; i++) {
            char a[PATH_MAX], b[PATH_MAX];
            snprintf(a, sizeof a, "%s/%s", src, de.name); snprintf(b, sizeof b, "%s/%s", dst, de.name);
            copy_tree(a, b);
        }
        vnode_unref(d);
        return 0;
    }
    void *data; size_t sz;
    if (vfs_read_file(src, &data, &sz) < 0) return -1;
    int r = vfs_write_file(dst, data, sz);
    kfree(data);
    return r;
}
static int remove_tree(const char *path) {
    struct stat st; if (vfs_stat(path, &st) < 0) return -1;
    if (st.type == VN_DIR) {
        struct vnode *d = vfs_lookup(path); if (!d) return -1;
        struct dirent de;
        while (vfs_readdir(d, 0, &de) == 0) { char a[PATH_MAX]; snprintf(a, sizeof a, "%s/%s", path, de.name); if (remove_tree(a) < 0) break; }
        vnode_unref(d);
    }
    return vfs_unlink(path);
}
static void unique_dest(const char *dir, const char *name, char *out, size_t n) {
    snprintf(out, n, "%s/%s", dir, name);
    struct stat st; int k = 2;
    while (vfs_stat(out, &st) == 0 && k < 100) { snprintf(out, n, "%s/%s copy%s%d", dir, name, k == 2 ? "" : " ", k); if (k == 2) snprintf(out, n, "%s/%s copy", dir, name); k++; }
}

static void trash_selected(struct files *f) {
    int n = 0; for (int i = 0; i < f->nentries; i++) if (f->sel[i]) n++;
    if (!n) return;
    bool in_trash = !strcmp(f->path, "/home/flux/.Trash");
    if (in_trash) {
        if (!dialog_confirm("Delete", n == 1 ? "Are you sure you want to delete this item permanently?" : "Are you sure you want to delete these items permanently?", "Delete", true)) return;
    }
    for (int i = 0; i < f->nentries; i++) {
        if (!f->sel[i]) continue;
        char p[PATH_MAX]; full_path(f, f->entries[i].name, p, sizeof p);
        if (in_trash) remove_tree(p);
        else {
            char dst[PATH_MAX]; unique_dest("/home/flux/.Trash", f->entries[i].name, dst, sizeof dst);
            if (copy_tree(p, dst) == 0) remove_tree(p);
        }
    }
    load_dir(f);
}

static void new_folder(struct files *f) {
    char name[NAME_MAX + 1];
    if (!dialog_prompt("New Folder", "Name of the new folder:", "untitled folder", name, sizeof name) || !name[0]) return;
    char p[PATH_MAX]; full_path(f, name, p, sizeof p);
    int r = vfs_mkdir(p);
    if (r < 0) dialog_message("Files", r == -EEXIST ? "An item with that name already exists." : "The folder could not be created.");
    load_dir(f);
    for (int i = 0; i < f->nentries; i++) if (!strcmp(f->entries[i].name, name)) { select_only(f, i); ensure_visible(f, i); }
}
static void new_document(struct files *f) {
    char name[NAME_MAX + 1];
    if (!dialog_prompt("New Document", "Name of the new text file:", "untitled.txt", name, sizeof name) || !name[0]) return;
    char p[PATH_MAX]; full_path(f, name, p, sizeof p);
    if (vfs_write_file(p, "", 0) < 0) dialog_message("Files", "The file could not be created.");
    load_dir(f);
}
static void rename_selected(struct files *f) {
    int i = first_selected(f); if (i < 0) return;
    char name[NAME_MAX + 1];
    if (!dialog_prompt("Rename", "Enter a new name:", f->entries[i].name, name, sizeof name) || !name[0] || !strcmp(name, f->entries[i].name)) return;
    char a[PATH_MAX], b[PATH_MAX]; full_path(f, f->entries[i].name, a, sizeof a); full_path(f, name, b, sizeof b);
    struct stat st;
    if (vfs_stat(b, &st) == 0) { dialog_message("Files", "An item with that name already exists."); return; }
    if (f->entries[i].type == VN_DIR) { if (copy_tree(a, b) == 0) remove_tree(a); }
    else vfs_rename(a, b);
    load_dir(f);
    for (int k = 0; k < f->nentries; k++) if (!strcmp(f->entries[k].name, name)) select_only(f, k);
}
static void get_info(struct files *f) {
    int i = first_selected(f);
    char msg[512];
    if (i < 0) {
        struct vnode *v = vfs_lookup(f->path);
        snprintf(msg, sizeof msg, "%s\n\nKind: Folder\nItems: %d\nFile system: %s", f->path, f->nentries, v && v->fs_name ? v->fs_name : "?");
        if (v) vnode_unref(v);
    } else {
        struct entry *e = &f->entries[i];
        char sz[32], dt[64]; format_size(e->size, sz, sizeof sz); format_date(e->mtime, dt, sizeof dt);
        char p[PATH_MAX]; full_path(f, e->name, p, sizeof p);
        snprintf(msg, sizeof msg, "%s\n\nKind: %s\nSize: %s\nModified: %s\nWhere: %s", e->name, kind_of(e), e->type == VN_DIR ? "--" : sz, dt, f->path);
    }
    dialog_message("Info", msg);
}
static void duplicate_selected(struct files *f) {
    int i = first_selected(f); if (i < 0) return;
    char a[PATH_MAX], b[PATH_MAX]; full_path(f, f->entries[i].name, a, sizeof a);
    unique_dest(f->path, f->entries[i].name, b, sizeof b);
    copy_tree(a, b);
    load_dir(f);
}
static void paste(struct files *f) {
    if (!f->clip_path[0]) { char *s = clipboard_get(); if (s) { struct stat st; if (s[0] == '/' && vfs_stat(s, &st) == 0) strlcpy(f->clip_path, s, PATH_MAX); kfree(s); } }
    if (!f->clip_path[0]) return;
    char dst[PATH_MAX]; unique_dest(f->path, vfs_basename(f->clip_path), dst, sizeof dst);
    copy_tree(f->clip_path, dst);
    load_dir(f);
}
static void empty_trash(struct files *f) {
    if (!dialog_confirm("Empty Trash", "Are you sure you want to permanently erase the items in the Trash?", "Empty Trash", true)) return;
    struct vnode *d = vfs_lookup("/home/flux/.Trash"); if (!d) return;
    struct dirent de;
    while (vfs_readdir(d, 0, &de) == 0) { char a[PATH_MAX]; snprintf(a, sizeof a, "/home/flux/.Trash/%s", de.name); if (remove_tree(a) < 0) break; }
    vnode_unref(d);
    load_dir(f);
}

static void move_selection(struct files *f, int delta, bool extend) {
    if (!f->nentries) return;
    int cur = f->selected < 0 ? -1 : f->selected;
    int nxt = CLAMP(cur + delta, 0, f->nentries - 1);
    if (cur < 0) nxt = delta > 0 ? 0 : f->nentries - 1;
    if (extend) { int a = MIN(f->anchor < 0 ? nxt : f->anchor, nxt), b = MAX(f->anchor < 0 ? nxt : f->anchor, nxt); memset(f->sel, 0, f->nentries); for (int i = a; i <= b; i++) f->sel[i] = true; f->selected = nxt; }
    else select_only(f, nxt);
    ensure_visible(f, nxt);
}

static void handle_command(struct files *f, int cmd, bool *quit, bool *close) {
    switch (cmd) {
    case CMD_QUIT: *quit = true; break;
    case CMD_CLOSE: *close = true; break;
    case FCMD_NEW_WINDOW: app_launch(APP_FILES, f->path); break;
    case CMD_NEW_FOLDER: new_folder(f); break;
    case CMD_NEW: new_document(f); break;
    case CMD_OPEN: { int i = first_selected(f); if (i >= 0) open_entry(f, i); break; }
    case FCMD_OPEN_TERMINAL: { int i = first_selected(f); char p[PATH_MAX]; if (i >= 0 && f->entries[i].type == VN_DIR) full_path(f, f->entries[i].name, p, sizeof p); else strlcpy(p, f->path, sizeof p); char arg[PATH_MAX + 8]; snprintf(arg, sizeof arg, "cd:%s", p); app_launch(APP_TERMINAL, arg); break; }
    case FCMD_INFO: get_info(f); break;
    case FCMD_RENAME: rename_selected(f); break;
    case FCMD_DUPLICATE: duplicate_selected(f); break;
    case FCMD_TRASH: trash_selected(f); break;
    case FCMD_EMPTY_TRASH: empty_trash(f); break;
    case CMD_COPY: { int i = first_selected(f); if (i >= 0) { full_path(f, f->entries[i].name, f->clip_path, PATH_MAX); clipboard_set(f->clip_path, strlen(f->clip_path)); } break; }
    case CMD_PASTE: paste(f); break;
    case CMD_SELECT_ALL: memset(f->sel, 1, f->nentries); break;
    case FCMD_ICONS: f->list_view = false; clamp_scroll(f); break;
    case FCMD_LIST: f->list_view = true; clamp_scroll(f); break;
    case FCMD_HIDDEN: f->show_hidden = !f->show_hidden; load_dir(f); build_sidebar(f); break;
    case FCMD_REFRESH: load_dir(f); build_sidebar(f); break;
    case FCMD_BACK: if (f->hist_pos > 0) { f->hist_pos--; navigate(f, f->history[f->hist_pos], false); } break;
    case FCMD_FORWARD: if (f->hist_pos < f->hist_len - 1) { f->hist_pos++; navigate(f, f->history[f->hist_pos], false); } break;
    case FCMD_UP: { char d[PATH_MAX]; vfs_dirname(f->path, d, sizeof d); if (strcmp(d, f->path)) navigate(f, d, true); break; }
    case FCMD_HOME: navigate(f, "/home/flux", true); break;
    case FCMD_COMPUTER: navigate(f, "/", true); break;
    case FCMD_GOTO: { char p[PATH_MAX]; if (dialog_prompt("Go to Folder", "Enter a path:", f->path, p, sizeof p) && p[0]) navigate(f, p, true); break; }
    case CMD_HELP: dialog_message("inFlux Help", "Double-click to open items. Use the Go menu or ⌘↑ to go up.\nDrag windows by their title bar; ⌘Q quits an app, ⌘W closes a window.\n\nThe Terminal runs a zsh-like shell: type help for commands."); break;
    }
}

void files_main(void *arg) {
    struct files *f = kzalloc(sizeof *f);
    f->selected = -1; f->anchor = -1; f->pressed_btn = 0; f->last_click_idx = -1;
    f->w = window_create("Files", 860, 520, WIN_STANDARD | WIN_FULL_TITLE);
    window_set_min_size(f->w, 520, 300);
    window_set_menus(f->w, &files_menus);
    build_sidebar(f);
    navigate(f, arg && ((const char *)arg)[0] ? (const char *)arg : "/home/flux", true);
    draw(f);
    window_show(f->w);
    bool quit = false, close = false;
    struct event ev;
    while (!quit && !close && window_wait_event(f->w, &ev, -1)) {
        switch (ev.type) {
        case EV_QUIT: quit = true; break;
        case EV_CLOSE: close = true; break;
        case EV_RESIZE: clamp_scroll(f); break;
        case EV_OPEN: navigate(f, ev.path, true); kfree(ev.path); break;
        case EV_MENU: handle_command(f, ev.cmd, &quit, &close); break;
        case EV_MOUSE_SCROLL: f->scroll += ev.scroll * 40; clamp_scroll(f); break;
        case EV_MOUSE_DOWN: {
            if (ev.button != BTN_LEFT) break;
            if (rect_contains(btn_back(f), ev.x, ev.y)) { f->pressed_btn = 1; break; }
            if (rect_contains(btn_fwd(f), ev.x, ev.y)) { f->pressed_btn = 2; break; }
            if (rect_contains(btn_newfolder(f), ev.x, ev.y)) { f->pressed_btn = 3; break; }
            if (rect_contains(btn_view(f, 0), ev.x, ev.y)) { f->list_view = false; clamp_scroll(f); break; }
            if (rect_contains(btn_view(f, 1), ev.x, ev.y)) { f->list_view = true; clamp_scroll(f); break; }
            if (ev.x < SIDEBAR_W) {
                int y = TITLEBAR_H + 10;
                for (int i = 0; i < f->nside; i++) {
                    int h = f->side[i].header ? 24 : 26;
                    if (!f->side[i].header && ev.y >= y && ev.y < y + h) { navigate(f, f->side[i].path, true); break; }
                    y += h;
                }
                break;
            }
            struct rect cr = content_rect(f);
            if (f->list_view && rect_contains(rect_make(cr.x, cr.y, cr.w, LIST_HEADER_H), ev.x, ev.y)) {
                int colx[4] = { 10, cr.w - 400, cr.w - 230, cr.w - 130 }; int rel = ev.x - cr.x, col = 0;
                for (int k = 1; k < 4; k++) if (rel >= colx[k] - 8) col = k;
                if (f->sort_col == col) f->sort_desc = !f->sort_desc; else { f->sort_col = col; f->sort_desc = false; }
                load_dir(f); break;
            }
            int i = item_at(f, ev.x, ev.y);
            if (i >= 0) {
                bool dbl = (i == f->last_click_idx) && (system_ticks - f->last_click_time < 450);
                if (ev.mods & MOD_CMD) f->sel[i] = !f->sel[i], f->selected = i;
                else if ((ev.mods & MOD_SHIFT) && f->anchor >= 0) { int a = MIN(f->anchor, i), b = MAX(f->anchor, i); memset(f->sel, 0, f->nentries); for (int k = a; k <= b; k++) f->sel[k] = true; f->selected = i; }
                else select_only(f, i);
                if (dbl) { f->last_click_idx = -1; open_entry(f, i); }
                else { f->last_click_idx = i; f->last_click_time = system_ticks; }
            } else if (rect_contains(cr, ev.x, ev.y)) { select_only(f, -1); f->last_click_idx = -1; }
            break; }
        case EV_MOUSE_UP:
            if (f->pressed_btn == 1 && rect_contains(btn_back(f), ev.x, ev.y)) handle_command(f, FCMD_BACK, &quit, &close);
            if (f->pressed_btn == 2 && rect_contains(btn_fwd(f), ev.x, ev.y)) handle_command(f, FCMD_FORWARD, &quit, &close);
            if (f->pressed_btn == 3 && rect_contains(btn_newfolder(f), ev.x, ev.y)) { f->pressed_btn = 0; draw(f); new_folder(f); }
            f->pressed_btn = 0;
            break;
        case EV_KEY_DOWN: {
            bool cmd = ev.mods & (MOD_CMD | MOD_CTRL);
            int cols = f->list_view ? 1 : cols_per_row(f);
            if (ev.key == KEY_DOWN) move_selection(f, cols, ev.mods & MOD_SHIFT);
            else if (ev.key == KEY_UP) { if (cmd) handle_command(f, FCMD_UP, &quit, &close); else move_selection(f, -cols, ev.mods & MOD_SHIFT); }
            else if (ev.key == KEY_LEFT && !f->list_view) move_selection(f, -1, ev.mods & MOD_SHIFT);
            else if (ev.key == KEY_RIGHT && !f->list_view) move_selection(f, 1, ev.mods & MOD_SHIFT);
            else if (ev.key == KEY_ENTER) { if (cmd) rename_selected(f); else { int i = first_selected(f); if (i >= 0) open_entry(f, i); } }
            else if (ev.key == KEY_BACKSPACE && cmd) trash_selected(f);
            else if (ev.key == KEY_DELETE) trash_selected(f);
            else if (ev.key == KEY_ESC) select_only(f, -1);
            else if (ev.key == KEY_PGDN) { f->scroll += content_rect(f).h; clamp_scroll(f); }
            else if (ev.key == KEY_PGUP) { f->scroll -= content_rect(f).h; clamp_scroll(f); }
            else if (ev.key == KEY_HOME) { f->scroll = 0; }
            else if (ev.key == KEY_END) { f->scroll = 1 << 20; clamp_scroll(f); }
            else if (ev.ch >= 32 && ev.ch < 127 && !cmd) {
                /* type-to-select */
                for (int i = 0; i < f->nentries; i++) if (tolower(f->entries[i].name[0]) == tolower((int)ev.ch)) { select_only(f, i); ensure_visible(f, i); break; }
            }
            break; }
        default: break;
        }
        if (!quit && !close) draw(f);
    }
    window_destroy(f->w);
    kfree(f->entries); kfree(f->sel); kfree(f);
}
