/* Disk Manager: disks, partitions and volumes. Mount and unmount volumes, erase a volume or a
 * whole disk (new GPT with one volume), add a partition in unallocated space, delete a partition. */
#include "ui.h"
#include "blockdev.h"
#include "fs.h"
#include "mm.h"
#include "string.h"

int fat32_format(struct blockdev *d, const char *label);
int btrfs_mkfs(struct blockdev *d, const char *label);

#define SIDEBAR_W 230
#define TOOLBAR_H 44
#define DISK_ROW_H 30
#define VOL_ROW_H 26
#define MAX_ITEMS 48
#define MAX_SEGS 40
#define MARGIN 24
#define MAP_H 46

/* filesystem minimums (bytes): FAT32 needs 65525 clusters, btrfs_mkfs 64 MiB */
#define FAT32_MIN (36ull << 20)
#define BTRFS_MIN (64ull << 20)
enum { FMT_FAT32, FMT_BTRFS };
enum { FS_NONE, FS_FAT32, FS_BTRFS };

struct item { struct blockdev *dev; bool disk; int fs; int y, h; };
struct seg { uint64_t first, count; struct blockdev *vol; };   /* vol NULL: unallocated */
struct diskinfo { struct blockdev *disk; int table; uint64_t lo, hi; };   /* lo..hi: where partitions may go */

enum { SHEET_NONE, SHEET_ERASE, SHEET_ADD };
enum { B_MOUNT = 1, B_ERASE, B_ADD, B_DELETE, B_CANCEL, B_OK, B_FMT0, B_FMT1 };

struct dm {
    struct window *w;
    struct item items[MAX_ITEMS]; int nitems;
    struct diskinfo dinfo[16]; int ndinfo;
    struct blockdev *seen[32]; int nseen;      /* the device list at the last reload (hotplug detection) */
    struct blockdev *sel;
    int pressed;
    /* the erase / add-partition sheet */
    int sheet; bool sheet_disk;                /* erase: the whole disk rather than one volume */
    struct textfield name, size; int fmt;
    char err[160]; const char *busy;
    uint64_t add_first, add_last;              /* the free range a new partition goes into */
};

enum { DCMD_MOUNT = CMD_APP + 1, DCMD_ERASE, DCMD_ADD, DCMD_DELETE, DCMD_REFRESH };
/* not const: the items are greyed out (and Mount renamed) to match the selection, see update_menus */
static struct menu_item disk_items[] = {
    MENU("Mount", DCMD_MOUNT, 'm', MOD_CMD | MOD_SHIFT), MENU("Erase…", DCMD_ERASE, 'e', MOD_CMD | MOD_SHIFT), MENU_SEP,
    MENU("Add Partition…", DCMD_ADD, 'p', MOD_CMD | MOD_SHIFT), MENU("Delete Partition…", DCMD_DELETE, KEY_BACKSPACE, MOD_CMD), MENU_SEP,
    MENU("Refresh", DCMD_REFRESH, 'r', MOD_CMD), MENU_SEP,
    MENU("Close", CMD_CLOSE, 'w', MOD_CMD), MENU("Quit Disk Manager", CMD_QUIT, 'q', MOD_CMD),
};
static const struct menu menus[] = { { "Disk", disk_items, ARRAY_LEN(disk_items) } };
static const struct app_menus dm_menus = { menus, ARRAY_LEN(menus) };

/* ------------------------------------------------------------------ helpers */
static void format_size(uint64_t b, char *out, size_t n) {
    if (b >= 1ull << 40) snprintf(out, n, "%lu.%lu TB", b >> 40, ((b >> 30) % 1024) / 103);
    else if (b >= 1ull << 30) snprintf(out, n, "%lu.%lu GB", b >> 30, ((b >> 20) % 1024) / 103);
    else if (b >= 1ull << 20) snprintf(out, n, "%lu MB", b >> 20);
    else snprintf(out, n, "%lu KB", b >> 10);
}
static const char *err_text(int r) {
    switch (-r) {
    case ENOSPC: return "There is not enough space.";
    case EIO: return "The disk reported an I/O error.";
    case ENODEV: return "The disk is no longer connected.";
    case EBUSY: return "The volume is in use.";
    case EEXIST: return "The space is already taken by another partition.";
    case ENOMEM: return "The system is out of memory.";
    default: return "The operation failed.";
    }
}
static uint64_t dev_bytes(struct blockdev *d) { return d->sectors * d->sector_size; }
static struct blockdev *disk_of(struct blockdev *v) { return v->parent ? v->parent : v; }
static uint64_t align_sectors(struct blockdev *d) { return MAX(1u, (1u << 20) / d->sector_size); }   /* 1 MiB */

static bool has_partitions(struct blockdev *d) {
    for (int i = 0; i < blockdev_count(); i++) { struct blockdev *p = blockdev_at(i); if (p && p->parent == d && !p->removed) return true; }
    return false;
}
/* a volume: a partition, or a whole disk that has no partitions (and maybe a filesystem) */
static bool is_volume(struct blockdev *d) { return d->parent || !has_partitions(d); }

static struct vnode *mount_root(struct blockdev *v, char *mp, size_t n) {
    for (int i = 0; i < vfs_mount_count(); i++) {
        char p[64]; struct vnode *r = vfs_mount_at(i, p, sizeof p);
        if (r && r->priv == v && strcmp(p, "/")) { if (mp) strlcpy(mp, p, n); return r; }
    }
    return NULL;
}
/* the installed system's disk: its /home volume can't be unmounted, nothing on it may be erased */
static bool is_system_disk(struct blockdev *d) {
    for (int i = 0; i < vfs_mount_count(); i++) {
        char p[64]; struct vnode *r = vfs_mount_at(i, p, sizeof p);
        if (!r || strcmp(p, "/home")) continue;
        /* r->priv is only a blockdev for disk volumes: match it against the disk's devices, don't dereference it */
        for (int j = 0; j < blockdev_count(); j++) { struct blockdev *b = blockdev_at(j); if (b && (void *)b == r->priv && (b == d || b->parent == d)) return true; }
    }
    return false;
}

static int probe_fs(struct blockdev *v) {
    uint8_t *buf = kmalloc(4096);
    int fs = FS_NONE;
    if (blockdev_read_bytes(v, 65536 + 64, buf, 8) == 0 && !memcmp(buf, "_BHRfS_M", 8)) fs = FS_BTRFS;
    else if (blockdev_read_bytes(v, 0, buf, 512) == 0 && buf[510] == 0x55 && buf[511] == 0xAA && !memcmp(buf + 82, "FAT32   ", 8)) fs = FS_FAT32;
    kfree(buf);
    return fs;
}
static const char *fs_label(int fs) { return fs == FS_BTRFS ? "btrfs" : fs == FS_FAT32 ? "FAT32" : NULL; }

static struct item *item_of(struct dm *m, struct blockdev *d) { for (int i = 0; i < m->nitems; i++) if (m->items[i].dev == d) return &m->items[i]; return NULL; }
static struct diskinfo *info_of(struct dm *m, struct blockdev *d) { for (int i = 0; i < m->ndinfo; i++) if (m->dinfo[i].disk == d) return &m->dinfo[i]; return NULL; }

static void vol_name(struct blockdev *v, char *out, size_t n) {
    struct vnode *r = mount_root(v, NULL, 0);
    if (r && r->name[0] && strcmp(r->name, v->name)) { strlcpy(out, r->name, n); return; }
    if (v->parent && v->model[0] && strncmp(v->model, "MBR type", 8)) { strlcpy(out, v->model, n); return; }
    if (v->esp) { strlcpy(out, "EFI System", n); return; }
    if (!v->parent) { strlcpy(out, v->model[0] ? v->model : v->name, n); return; }
    snprintf(out, n, "Untitled (%s)", v->name);
}
static void item_name(struct blockdev *d, char *out, size_t n) {
    if (is_volume(d) && d->parent) vol_name(d, out, n);
    else strlcpy(out, d->model[0] ? d->model : "Disk", n);
}

/* ------------------------------------------------------------------ device list */
static void reload(struct dm *m) {
    m->nitems = 0; m->ndinfo = 0; m->nseen = 0;
    struct blockdev *all[32]; int n = 0;
    for (int i = 0; i < blockdev_count() && n < 32; i++) { struct blockdev *b = blockdev_at(i); if (b && !b->removed) all[n++] = b; }
    memcpy(m->seen, all, n * sizeof all[0]); m->nseen = n;
    for (int i = 0; i < n && m->nitems < MAX_ITEMS; i++) {
        struct blockdev *d = all[i];
        if (d->parent) continue;
        struct item *it = &m->items[m->nitems++];
        *it = (struct item){ .dev = d, .disk = true };
        /* partitions in disk order */
        struct blockdev *parts[32]; int np = 0;
        for (int j = 0; j < n; j++) if (all[j]->parent == d) parts[np++] = all[j];
        for (int a = 1; a < np; a++) { struct blockdev *k = parts[a]; int b = a - 1; while (b >= 0 && parts[b]->start_lba > k->start_lba) { parts[b + 1] = parts[b]; b--; } parts[b + 1] = k; }
        if (!np) it->fs = probe_fs(d);
        for (int j = 0; j < np && m->nitems < MAX_ITEMS; j++) m->items[m->nitems++] = (struct item){ .dev = parts[j], .fs = probe_fs(parts[j]) };
        if (m->ndinfo < 16) {
            struct diskinfo *di = &m->dinfo[m->ndinfo++];
            di->disk = d; di->table = blockdev_ptable(d);
            di->lo = 0; di->hi = d->sectors ? d->sectors - 1 : 0;
            if (di->table == PTABLE_GPT && gpt_usable_range(d, &di->lo, &di->hi) < 0) di->table = PTABLE_NONE;
            else if (di->table == PTABLE_MBR) di->lo = 1;
        }
    }
    if (m->sel && !item_of(m, m->sel)) m->sel = NULL;
    if (!m->sel && m->nitems) m->sel = m->items[0].dev;
}
static bool devices_changed(struct dm *m) {
    int n = 0;
    for (int i = 0; i < blockdev_count(); i++) {
        struct blockdev *b = blockdev_at(i);
        if (!b || b->removed) continue;
        if (n >= m->nseen || m->seen[n] != b) return true;
        n++;
    }
    return n != m->nseen;
}

/* the disk's layout: partitions and the unallocated gaps (>= 1 MiB) between them */
static int disk_segments(struct dm *m, struct blockdev *d, struct seg *out, int max) {
    struct diskinfo *di = info_of(m, d);
    int n = 0;
    if (!has_partitions(d)) { out[n++] = (struct seg){ 0, d->sectors, is_volume(d) && item_of(m, d) && item_of(m, d)->fs != FS_NONE ? d : NULL }; return n; }
    uint64_t pos = di ? di->lo : 0, end = di ? di->hi + 1 : d->sectors, al = align_sectors(d);
    for (int i = 0; i < m->nitems && n < max - 1; i++) {
        struct blockdev *p = m->items[i].dev;
        if (p->parent != d) continue;
        if (p->start_lba > pos && p->start_lba - pos >= al) out[n++] = (struct seg){ pos, p->start_lba - pos, NULL };
        out[n++] = (struct seg){ p->start_lba, p->sectors, p };
        pos = MAX(pos, p->start_lba + p->sectors);
    }
    if (end > pos && end - pos >= al && n < max) out[n++] = (struct seg){ pos, end - pos, NULL };
    return n;
}
/* the largest unallocated range of a GPT disk, 1 MiB aligned */
static bool largest_free(struct dm *m, struct blockdev *d, uint64_t *first, uint64_t *last) {
    struct diskinfo *di = info_of(m, d);
    if (!di || di->table != PTABLE_GPT) return false;
    struct seg segs[MAX_SEGS]; int n = disk_segments(m, d, segs, MAX_SEGS);
    uint64_t al = align_sectors(d), best = 0;
    for (int i = 0; i < n; i++) {
        if (segs[i].vol) continue;
        uint64_t f = ALIGN_UP(segs[i].first, al), e = segs[i].first + segs[i].count;   /* exclusive */
        if (e <= f) continue;
        uint64_t l = e - 1;
        if (e - f >= al) l = f + ALIGN_DOWN(e - f, al) - 1;
        if (l - f + 1 > best) { best = l - f + 1; *first = f; *last = l; }
    }
    return best * d->sector_size >= FAT32_MIN;
}

/* ------------------------------------------------------------------ what can be done now */
static bool can_mount(struct dm *m) {
    if (!m->sel || !is_volume(m->sel)) return false;
    char mp[64];
    if (mount_root(m->sel, mp, sizeof mp)) return strcmp(mp, "/home") != 0;
    struct item *it = item_of(m, m->sel);
    return it && it->fs != FS_NONE;
}
static bool can_erase(struct dm *m) { return m->sel && !is_system_disk(disk_of(m->sel)); }
static bool can_add(struct dm *m) { uint64_t f, l; return m->sel && !is_system_disk(disk_of(m->sel)) && largest_free(m, disk_of(m->sel), &f, &l); }
static bool can_delete(struct dm *m) {
    if (!m->sel || !m->sel->parent || is_system_disk(m->sel->parent)) return false;
    struct diskinfo *di = info_of(m, m->sel->parent);
    return di && di->table == PTABLE_GPT;
}
static void update_menus(struct dm *m) {
    disk_items[0].label = m->sel && mount_root(m->sel, NULL, 0) ? "Unmount" : "Mount";
    disk_items[0].disabled = !can_mount(m);
    disk_items[1].disabled = !can_erase(m);
    disk_items[3].disabled = !can_add(m);
    disk_items[4].disabled = !can_delete(m);
}

/* ------------------------------------------------------------------ layout */
struct tbtn { int id; const char *label; struct rect r; bool enabled; };
static int toolbar(struct dm *m, struct tbtn *b) {
    int x = SIDEBAR_W + 14, y = TITLEBAR_H + 8, n = 0;
    const char *ml = m->sel && mount_root(m->sel, NULL, 0) ? "Unmount" : "Mount";
    struct { int id; const char *l; bool en; } defs[] = { { B_MOUNT, ml, can_mount(m) }, { B_ERASE, "Erase…", can_erase(m) }, { B_ADD, "Add Partition…", can_add(m) }, { B_DELETE, "Delete", can_delete(m) } };
    for (int i = 0; i < 4; i++) {
        int w = font_text_width(font_ui, defs[i].l) + 28;
        b[n++] = (struct tbtn){ defs[i].id, defs[i].l, rect_make(x, y, w, 28), defs[i].en };
        x += w + 8;
    }
    return n;
}
static void layout_sidebar(struct dm *m) {
    int y = TITLEBAR_H + 36;
    for (int i = 0; i < m->nitems; i++) {
        struct item *it = &m->items[i];
        if (it->disk && i) y += 8;
        it->y = y; it->h = it->disk ? DISK_ROW_H : VOL_ROW_H;
        y += it->h;
    }
}
static struct rect map_rect(struct dm *m) { int W = m->w->frame.w; return rect_make(SIDEBAR_W + MARGIN, TITLEBAR_H + TOOLBAR_H + 104, W - SIDEBAR_W - 2 * MARGIN, MAP_H); }

/* segment x extents: proportional, but every segment at least 10 px wide */
static void seg_extents(struct seg *s, int n, struct rect r, int *x0, int *x1) {
    uint64_t total = 0; for (int i = 0; i < n; i++) total += s[i].count;
    int minw = 10, spare = MAX(0, r.w - n * minw), x = r.x;
    uint64_t acc = 0;
    for (int i = 0; i < n; i++) {
        x0[i] = x;
        acc += s[i].count;
        int end = r.x + (int)(total ? (uint64_t)spare * acc / total : 0) + (i + 1) * minw;
        x1[i] = i == n - 1 ? r.x + r.w : MIN(end, r.x + r.w);
        x = x1[i];
    }
}
static const color_t seg_colors[] = { RGB(0x0A, 0x84, 0xFF), RGB(0x30, 0xB0, 0x50), RGB(0xFF, 0x9F, 0x0A), RGB(0xAF, 0x52, 0xDE), RGB(0x32, 0xAD, 0xE6), RGB(0xFF, 0x37, 0x5F) };

struct sheet_layout { struct rect panel, name, size, fmt0, fmt1, cancel, ok; };
static struct sheet_layout sheet_layout(struct dm *m) {
    struct sheet_layout L; int W = m->w->frame.w;
    int pw = 440, ph = m->sheet == SHEET_ADD ? 290 : 250;
    L.panel = rect_make((W - pw) / 2, TITLEBAR_H + 6, pw, ph);
    int x = L.panel.x + 110, y = L.panel.y + 104;
    L.name = rect_make(x, y, pw - 130, 26);
    L.fmt0 = rect_make(x, y + 40, 90, 26); L.fmt1 = rect_make(x + 90, y + 40, 90, 26);
    L.size = rect_make(x, y + 80, 100, 26);
    L.ok = rect_make(L.panel.x + pw - 20 - 100, L.panel.y + ph - 46, 100, 28);
    L.cancel = rect_make(L.ok.x - 110, L.ok.y, 100, 28);
    return L;
}

/* ------------------------------------------------------------------ drawing */
static void draw_hatch(struct canvas *c, struct rect r, color_t bg, color_t line) {
    gfx_fill_rect(c, r.x, r.y, r.w, r.h, bg);
    struct canvas sub = canvas_sub(c, r);
    for (int x = -r.h; x < r.w; x += 7) gfx_line(&sub, x, r.h, x + r.h, 0, line);
}

static void draw_info_rows(struct canvas *c, struct rect r, const char *const *k, char v[][96], int n) {
    ui_panel(c, r, theme.bg_secondary);
    int cols = 2, rows = (n + 1) / 2, cw = r.w / cols;
    for (int i = 0; i < n; i++) {
        int col = i / rows, row = i % rows;
        int x = r.x + 16 + col * cw, y = r.y + 12 + row * 24;
        font_draw(c, font_ui, x, y, theme.text_secondary, k[i]);
        font_draw_ellipsized(c, font_ui, x + 108, y, cw - 128, theme.text, v[i]);
    }
}

static void draw(struct dm *m) {
    update_menus(m);
    layout_sidebar(m);
    struct canvas *c = window_begin_draw(m->w);
    int W = c->w, H = c->h;
    gfx_fill(c, theme.bg);
    char buf[160], name[96], sz[32];

    /* sidebar */
    gfx_fill_rect(c, 0, 0, SIDEBAR_W, H, theme.sidebar_bg);
    gfx_vline(c, SIDEBAR_W - 1, 0, H, theme.separator);
    font_draw(c, font_ui_semibold, 18, TITLEBAR_H + 14, theme.text_tertiary, "Disks");
    for (int i = 0; i < m->nitems; i++) {
        struct item *it = &m->items[i];
        bool sel = it->dev == m->sel;
        int indent = it->disk ? 0 : 18;
        if (sel) gfx_fill_rounded_rect(c, 8, it->y, SIDEBAR_W - 16, it->h, 6, theme.accent);
        color_t tc = sel ? theme.accent_text : theme.text, sc = sel ? theme.accent_text : theme.text_tertiary;
        bool usb = it->dev->bus && !strcmp(it->dev->bus, "USB");
        struct canvas *ic = icon_get(usb ? "drive-removable" : "drive", 32);
        int isz = it->disk ? 20 : 16;
        if (ic) gfx_blit_scaled(c, 16 + indent, it->y + (it->h - isz) / 2, isz, isz, ic);
        item_name(it->dev, name, sizeof name);
        format_size(dev_bytes(it->dev), buf, sizeof buf);
        int sw = font_text_width(font_ui_small, buf);
        int tx = 16 + indent + isz + 8;
        font_draw_ellipsized(c, it->disk ? font_ui_semibold : font_ui, tx, it->y + (it->h - font_height(font_ui)) / 2, SIDEBAR_W - 16 - tx - sw - 10, tc, name);
        font_draw(c, font_ui_small, SIDEBAR_W - 16 - sw, it->y + (it->h - font_height(font_ui_small)) / 2, sc, buf);
    }
    if (!m->nitems) font_draw(c, font_ui, 18, TITLEBAR_H + 40, theme.text_tertiary, "No disks found");

    /* toolbar */
    gfx_fill_rect(c, SIDEBAR_W, 0, W - SIDEBAR_W, TITLEBAR_H + TOOLBAR_H, theme.titlebar);
    gfx_hline(c, SIDEBAR_W, TITLEBAR_H + TOOLBAR_H - 1, W - SIDEBAR_W, theme.titlebar_line);
    struct tbtn tb[4]; int ntb = toolbar(m, tb);
    for (int i = 0; i < ntb; i++) ui_button(c, tb[i].r, tb[i].label, BTN_NORMAL, m->pressed == tb[i].id, tb[i].enabled && !m->sheet);

    struct blockdev *s = m->sel;
    if (s) {
        struct blockdev *disk = disk_of(s);
        struct diskinfo *di = info_of(m, disk);
        struct item *sit = item_of(m, s);
        bool vol = is_volume(s);
        char mp[64]; struct vnode *root = vol ? mount_root(s, mp, sizeof mp) : NULL;
        int x = SIDEBAR_W + MARGIN, y = TITLEBAR_H + TOOLBAR_H + 18;
        /* header */
        bool usb = s->bus && !strcmp(s->bus, "USB");
        struct canvas *ic = icon_get(usb ? "drive-removable" : "drive", 64);
        if (ic) gfx_blit_scaled(c, x, y, 64, 64, ic);
        item_name(s, name, sizeof name);
        font_draw_ellipsized(c, font_ui_title, x + 80, y + 4, W - x - 80 - MARGIN, theme.text, name);
        format_size(dev_bytes(s), sz, sizeof sz);
        const char *tbl = !di ? "" : di->table == PTABLE_GPT ? "GUID partition map" : di->table == PTABLE_MBR ? "Master boot record" : "No partition map";
        if (!s->parent && has_partitions(s)) snprintf(buf, sizeof buf, "%s %s disk  ·  %s", sz, s->bus ? s->bus : "", tbl);
        else {
            const char *fl = root ? (!strcmp(root->fs_name, "fat32") ? "FAT32" : root->fs_name) : sit ? fs_label(sit->fs) : NULL;
            snprintf(buf, sizeof buf, "%s %s  ·  %s", sz, fl ? fl : "Unformatted", root ? "" : "Not mounted");
            if (root) { size_t o = strlen(buf); snprintf(buf + o, sizeof buf - o, "Mounted at %s", mp); }
        }
        font_draw_ellipsized(c, font_ui, x + 80, y + 40, W - x - 80 - MARGIN, theme.text_secondary, buf);

        /* layout map */
        struct seg segs[MAX_SEGS]; int nseg = disk_segments(m, disk, segs, MAX_SEGS);
        int x0[MAX_SEGS], x1[MAX_SEGS];
        struct rect mr = map_rect(m);
        seg_extents(segs, nseg, mr, x0, x1);
        int ci = 0;
        for (int i = 0; i < nseg; i++) {
            int mask = (i == 0 ? 1 | 4 : 0) | (i == nseg - 1 ? 2 | 8 : 0);
            struct rect r = rect_make(x0[i], mr.y, x1[i] - x0[i] - (i < nseg - 1 ? 2 : 0), mr.h);
            if (r.w <= 0) continue;
            bool hl = segs[i].vol && segs[i].vol == s;
            if (segs[i].vol) {
                color_t col = seg_colors[ci++ % ARRAY_LEN(seg_colors)];
                if (mask) gfx_fill_rounded_rect_corners(c, r.x, r.y, r.w, r.h, 8, col, mask); else gfx_fill_rect(c, r.x, r.y, r.w, r.h, col);
                if (r.w > 60) {
                    vol_name(segs[i].vol, name, sizeof name);
                    font_draw_ellipsized(c, font_ui_semibold, r.x + 8, r.y + 6, r.w - 16, RGB(255, 255, 255), name);
                    format_size(segs[i].count * disk->sector_size, sz, sizeof sz);
                    font_draw_ellipsized(c, font_ui_small, r.x + 8, r.y + 25, r.w - 16, RGBA(255, 255, 255, 0xD0), sz);
                }
            } else {
                color_t bg = theme.dark ? RGB(0x2C, 0x2C, 0x2E) : RGB(0xEC, 0xEC, 0xEE), ln = theme.dark ? RGB(0x3A, 0x3A, 0x3C) : RGB(0xDA, 0xDA, 0xDE);
                struct rect hr = r; if (mask & 1) { hr.x += 6; hr.w -= 6; } if (mask & 2) hr.w -= 6;
                if (mask) gfx_fill_rounded_rect_corners(c, r.x, r.y, r.w, r.h, 8, bg, mask);
                if (hr.w > 0) draw_hatch(c, hr, bg, ln);
                if (r.w > 70) {
                    font_draw_ellipsized(c, font_ui_semibold, r.x + 8, r.y + 6, r.w - 16, theme.text_secondary, "Free space");
                    format_size(segs[i].count * disk->sector_size, sz, sizeof sz);
                    font_draw_ellipsized(c, font_ui_small, r.x + 8, r.y + 25, r.w - 16, theme.text_tertiary, sz);
                }
            }
            if (hl) { gfx_draw_rounded_rect(c, r.x - 2, r.y - 2, r.w + 4, r.h + 4, 9, theme.text); gfx_draw_rounded_rect(c, r.x - 3, r.y - 3, r.w + 6, r.h + 6, 10, theme.text); }
        }

        /* details */
        int iy = mr.y + mr.h + 22, iw = W - SIDEBAR_W - 2 * MARGIN;
        static const char *dk[] = { "Device", "Connection", "Capacity", "Sector size", "Partition map", "Volumes", "Unallocated", "Model" };
        static const char *vk[] = { "Device", "Format", "Capacity", "Mount point", "Used", "Available", "Partition", "Disk" };
        char v[8][96];
        if (!s->parent && has_partitions(s)) {
            int nvol = 0; uint64_t free = 0;
            for (int i = 0; i < nseg; i++) if (segs[i].vol) nvol++; else free += segs[i].count * s->sector_size;
            snprintf(v[0], 96, "/dev/%s", s->name); strlcpy(v[1], s->bus ? s->bus : "—", 96);
            format_size(dev_bytes(s), sz, sizeof sz); snprintf(v[2], 96, "%s (%lu sectors)", sz, s->sectors);
            snprintf(v[3], 96, "%u bytes", s->sector_size); strlcpy(v[4], di && di->table == PTABLE_GPT ? "GPT" : di && di->table == PTABLE_MBR ? "MBR" : "None", 96);
            snprintf(v[5], 96, "%d", nvol); format_size(free, v[6], 96); strlcpy(v[7], s->model[0] ? s->model : "—", 96);
            draw_info_rows(c, rect_make(x, iy, iw, 4 * 24 + 20), dk, v, 8);
        } else {
            snprintf(v[0], 96, "/dev/%s", s->name);
            const char *fl = root ? (!strcmp(root->fs_name, "fat32") ? "FAT32" : root->fs_name) : sit ? fs_label(sit->fs) : NULL;
            strlcpy(v[1], fl ? fl : "Unknown", 96);
            format_size(dev_bytes(s), v[2], 96);
            strlcpy(v[3], root ? mp : "Not mounted", 96);
            if (root && root->fs_total) { format_size(root->fs_total - MIN(root->fs_free, root->fs_total), v[4], 96); format_size(root->fs_free, v[5], 96); }
            else { strlcpy(v[4], "—", 96); strlcpy(v[5], "—", 96); }
            if (s->parent) snprintf(v[6], 96, "%s, from %lu MB", s->esp ? "EFI System" : di && di->table == PTABLE_MBR ? "MBR" : "GPT", s->start_lba * s->sector_size >> 20);
            else strlcpy(v[6], "Whole disk", 96);
            snprintf(v[7], 96, "%s (/dev/%s)", disk->model[0] ? disk->model : "Disk", disk->name);
            draw_info_rows(c, rect_make(x, iy, iw, 4 * 24 + 20), vk, v, 8);
            /* usage bar */
            if (root && root->fs_total) {
                int by = iy + 4 * 24 + 36;
                uint64_t used = root->fs_total - MIN(root->fs_free, root->fs_total);
                gfx_fill_rounded_rect(c, x, by, iw, 10, 5, theme.dark ? RGB(0x3A, 0x3A, 0x3E) : RGB(0xDD, 0xDD, 0xE0));
                gfx_fill_rounded_rect(c, x, by, MAX(10, (int)((uint64_t)iw * used / root->fs_total)), 10, 5, theme.accent);
                snprintf(buf, sizeof buf, "%lu%% used", used * 100 / root->fs_total);
                font_draw(c, font_ui_small, x, by + 16, theme.text_secondary, buf);
            }
        }
        if (is_system_disk(disk)) font_draw(c, font_ui_small, x, H - 28, theme.text_tertiary, "This disk holds the running inFlux system. It can’t be erased or repartitioned.");
    }

    /* sheet */
    if (m->sheet) {
        gfx_fill_rect(c, 0, TITLEBAR_H, W, H - TITLEBAR_H, RGBA(0, 0, 0, theme.dark ? 0x60 : 0x30));
        struct sheet_layout L = sheet_layout(m);
        struct rect p = L.panel;
        gfx_shadow_outside(c, p, 10, 18, 90, 6);
        gfx_fill_rounded_rect(c, p.x, p.y, p.w, p.h, 10, theme.dark ? RGB(0x2C, 0x2C, 0x2E) : RGB(0xF6, 0xF6, 0xF8));
        gfx_draw_rounded_rect(c, p.x, p.y, p.w, p.h, 10, theme.separator);
        item_name(m->sheet == SHEET_ADD || m->sheet_disk ? disk_of(s) : s, name, sizeof name);
        if (m->sheet == SHEET_ERASE) snprintf(buf, sizeof buf, "Erase “%s”?", name); else snprintf(buf, sizeof buf, "Add a partition to “%s”", name);
        font_draw_ellipsized(c, font_ui_semibold_large, p.x + 20, p.y + 18, p.w - 40, theme.text, buf);
        const char *l1, *l2;
        if (m->sheet == SHEET_ADD) { l1 = "The new volume goes into the largest unallocated space."; l2 = "Other volumes on this disk are briefly unmounted."; }
        else if (m->sheet_disk) { l1 = "Erasing creates a new GUID partition map with one volume."; l2 = "All data on the disk will be lost. This can’t be undone."; }
        else { l1 = "Erasing creates an empty filesystem on this volume."; l2 = "All data on the volume will be lost. This can’t be undone."; }
        font_draw(c, font_ui, p.x + 20, p.y + 50, theme.text_secondary, l1);
        font_draw(c, font_ui, p.x + 20, p.y + 68, theme.text_secondary, l2);
        int lx = p.x + 20;
        font_draw_aligned(c, font_ui, rect_make(lx, L.name.y, 80, L.name.h), theme.text, "Name", 2);
        textfield_draw(c, &m->name, L.name);
        font_draw_aligned(c, font_ui, rect_make(lx, L.fmt0.y, 80, L.fmt0.h), theme.text, "Format", 2);
        gfx_fill_rounded_rect(c, L.fmt0.x, L.fmt0.y, L.fmt0.w + L.fmt1.w, L.fmt0.h, 6, theme.dark ? RGBA(255, 255, 255, 0x14) : RGBA(0, 0, 0, 0x0C));
        struct rect act = m->fmt == FMT_FAT32 ? L.fmt0 : L.fmt1;
        gfx_fill_rounded_rect(c, act.x + 2, act.y + 2, act.w - 4, act.h - 4, 5, theme.control_bg);
        gfx_draw_rounded_rect(c, act.x + 2, act.y + 2, act.w - 4, act.h - 4, 5, theme.control_border);
        font_draw_centered(c, m->fmt == FMT_FAT32 ? font_ui_semibold : font_ui, L.fmt0, theme.text, "FAT32");
        font_draw_centered(c, m->fmt == FMT_BTRFS ? font_ui_semibold : font_ui, L.fmt1, theme.text, "btrfs");
        font_draw(c, font_ui_small, L.fmt1.x + L.fmt1.w + 12, L.fmt0.y + 6, theme.text_tertiary, m->fmt == FMT_FAT32 ? "Readable everywhere" : "Copy-on-write");
        if (m->sheet == SHEET_ADD) {
            font_draw_aligned(c, font_ui, rect_make(lx, L.size.y, 80, L.size.h), theme.text, "Size", 2);
            textfield_draw(c, &m->size, L.size);
            format_size((m->add_last - m->add_first + 1) * disk_of(s)->sector_size, sz, sizeof sz);
            snprintf(buf, sizeof buf, "MB   (up to %s)", sz);
            font_draw(c, font_ui, L.size.x + L.size.w + 8, L.size.y + (L.size.h - font_height(font_ui)) / 2, theme.text_secondary, buf);
        }
        if (m->err[0]) font_draw_ellipsized(c, font_ui_small, p.x + 20, L.ok.y - 22, p.w - 40, RGB(0xFF, 0x3B, 0x30), m->err);
        if (m->busy) font_draw(c, font_ui, p.x + 20, L.ok.y + (28 - font_height(font_ui)) / 2, theme.text_secondary, m->busy);
        ui_button(c, L.cancel, "Cancel", BTN_NORMAL, m->pressed == B_CANCEL, !m->busy);
        ui_button(c, L.ok, m->sheet == SHEET_ADD ? "Add" : "Erase", m->sheet == SHEET_ADD ? BTN_DEFAULT : BTN_DESTRUCTIVE, m->pressed == B_OK, !m->busy);
    }
    window_end_draw(m->w, NULL);
}

/* ------------------------------------------------------------------ operations */
static void show_error(const char *what, int r) {
    char msg[200]; snprintf(msg, sizeof msg, "%s\n%s (error %d)", what, err_text(r), r);
    dialog_message("Disk Manager", msg);
}
static struct blockdev *child_at(struct blockdev *d, uint64_t first) {
    for (int i = 0; i < blockdev_count(); i++) { struct blockdev *p = blockdev_at(i); if (p && p->parent == d && !p->removed && p->start_lba == first) return p; }
    return NULL;
}
/* table edits rescan the disk, which retires every partition struct: unmount all of its
 * volumes first, and remount the ones that were mounted (found again by start LBA) */
struct mounted { uint64_t start[32]; int n; };
static void remember_mounts(struct blockdev *d, struct mounted *mt) {
    mt->n = 0;
    for (int i = 0; i < blockdev_count() && mt->n < 32; i++) { struct blockdev *p = blockdev_at(i); if (p && p->parent == d && !p->removed && mount_root(p, NULL, 0)) mt->start[mt->n++] = p->start_lba; }
}
static void remount(struct blockdev *d, const struct mounted *mt, uint64_t skip) {
    for (int i = 0; i < mt->n; i++) { if (mt->start[i] == skip) continue; struct blockdev *p = child_at(d, mt->start[i]); if (p) storage_mount_volume(p); }
}

static void toggle_mount(struct dm *m) {
    if (!can_mount(m)) return;
    struct blockdev *v = m->sel;
    if (mount_root(v, NULL, 0)) { int r = storage_unmount_volume(v); if (r < 0) show_error("The volume couldn’t be unmounted.", r); }
    else { int r = storage_mount_volume(v); if (r < 0) show_error(r == -EINVAL ? "The volume couldn’t be mounted: it has no FAT32 or btrfs filesystem." : "The volume couldn’t be mounted.", r); }
}

static void open_sheet(struct dm *m, int kind) {
    if (kind == SHEET_ERASE && !can_erase(m)) return;
    if (kind == SHEET_ADD && !(can_add(m) && largest_free(m, disk_of(m->sel), &m->add_first, &m->add_last))) return;
    m->sheet = kind; m->err[0] = 0; m->busy = NULL;
    m->sheet_disk = kind == SHEET_ERASE && !m->sel->parent;   /* a whole disk always gets a new GPT */
    /* erasing a volume keeps its name */
    char nm[96] = "Untitled";
    if (kind == SHEET_ERASE && m->sel->parent) {
        vol_name(m->sel, nm, sizeof nm);
        if (!strncmp(nm, "Untitled", 8)) strlcpy(nm, "Untitled", sizeof nm);
        else if (!strcmp(nm, "EFI System")) strlcpy(nm, "EFI", sizeof nm);
    }
    textfield_init(&m->name, nm); m->name.placeholder = "Untitled"; m->name.sel_anchor = 0; m->name.focused = true;
    uint64_t bytes = kind == SHEET_ADD ? (m->add_last - m->add_first + 1) * disk_of(m->sel)->sector_size : dev_bytes(m->sel);
    struct item *it = item_of(m, m->sel);
    m->fmt = it && it->fs == FS_BTRFS ? FMT_BTRFS : it && it->fs == FS_FAT32 ? FMT_FAT32 : bytes >= (1ull << 30) ? FMT_BTRFS : FMT_FAT32;
    if (bytes < BTRFS_MIN) m->fmt = FMT_FAT32;
    char s[32]; snprintf(s, sizeof s, "%lu", bytes >> 20);
    textfield_init(&m->size, s); m->size.focused = false;
}

static int format_volume(struct blockdev *v, int fmt, const char *label) {
    uint64_t b = dev_bytes(v);
    if (fmt == FMT_FAT32) { if (v->sector_size != 512) return -EINVAL; if (b < FAT32_MIN) return -ENOSPC; return fat32_format(v, label); }
    if (b < BTRFS_MIN) return -ENOSPC;
    return btrfs_mkfs(v, label);
}

/* runs the sheet's operation; false (with m->err set) keeps the sheet open */
static bool sheet_commit(struct dm *m) {
    struct blockdev *s = m->sel, *disk = disk_of(s);
    const char *label = m->name.len ? m->name.buf : "Untitled";
    uint64_t min = m->fmt == FMT_FAT32 ? FAT32_MIN : BTRFS_MIN;
    if (m->fmt == FMT_FAT32 && disk->sector_size != 512) { strlcpy(m->err, "FAT32 needs a disk with 512-byte sectors.", sizeof m->err); return false; }
    uint64_t first = 0, last = 0;
    if (m->sheet == SHEET_ADD) {
        const char *p = m->size.buf; uint64_t mb = 0; bool ok = *p != 0;
        for (; *p; p++) { if (*p < '0' || *p > '9') { ok = false; break; } mb = mb * 10 + (*p - '0'); if (mb > (1ull << 32)) break; }
        uint64_t maxs = m->add_last - m->add_first + 1, al = align_sectors(disk);
        uint64_t want = mb * ((1ull << 20) / disk->sector_size);
        if (!ok || !mb) { strlcpy(m->err, "Enter the size in MB.", sizeof m->err); return false; }
        if (want > maxs) want = maxs;
        if (maxs - want < al) want = maxs;   /* don't leave a sliver behind */
        if (want * disk->sector_size < min) { snprintf(m->err, sizeof m->err, "A %s volume needs at least %lu MB.", m->fmt == FMT_FAT32 ? "FAT32" : "btrfs", min >> 20); return false; }
        first = m->add_first; last = first + want - 1;
    } else if (m->sheet_disk) {
        if (dev_bytes(disk) < min + (2ull << 20)) { snprintf(m->err, sizeof m->err, "The disk is too small for %s.", m->fmt == FMT_FAT32 ? "FAT32" : "btrfs"); return false; }
    } else if (dev_bytes(s) < min) { snprintf(m->err, sizeof m->err, "A %s volume needs at least %lu MB.", m->fmt == FMT_FAT32 ? "FAT32" : "btrfs", min >> 20); return false; }
    if (is_system_disk(disk)) { strlcpy(m->err, "This disk holds the running system.", sizeof m->err); return false; }

    m->err[0] = 0;
    m->busy = m->sheet == SHEET_ADD ? "Creating the partition…" : "Erasing…";
    draw(m);
    const uint8_t *type = m->fmt == FMT_FAT32 ? GPT_TYPE_BASIC_DATA : GPT_TYPE_LINUX_FS;
    struct blockdev *target = NULL; int r = 0;
    const char *what = "The operation failed.";
    if (m->sheet == SHEET_ADD) {
        struct mounted mt; remember_mounts(disk, &mt);
        storage_unmount(disk);
        r = gpt_add_partition(disk, first, last, type, label);
        blockdev_rescan(disk);
        if (r == 0 && !(target = child_at(disk, first))) r = -EIO;
        if (r == 0) { what = "The new partition couldn’t be formatted."; r = format_volume(target, m->fmt, label); }
        else what = "The partition couldn’t be created.";
        remount(disk, &mt, first);
    } else if (m->sheet_disk) {
        storage_unmount(disk);
        uint64_t lo, hi, al = align_sectors(disk);
        what = "The disk couldn’t be partitioned.";
        r = gpt_create(disk);
        if (r == 0) r = gpt_usable_range(disk, &lo, &hi);
        if (r == 0) { first = ALIGN_UP(lo, al); last = ALIGN_DOWN(hi + 1, al) - 1; r = gpt_add_partition(disk, first, last, type, label); }
        blockdev_rescan(disk);
        if (r == 0 && !(target = child_at(disk, first))) r = -EIO;
        if (r == 0) { what = "The new volume couldn’t be formatted."; r = format_volume(target, m->fmt, label); }
    } else {
        target = s;
        if (mount_root(s, NULL, 0) && (r = storage_unmount_volume(s)) < 0) what = "The volume couldn’t be unmounted.";
        else { what = "The volume couldn’t be erased."; r = format_volume(s, m->fmt, label); }
    }
    if (r == 0 && target) {
        int mr = storage_mount_volume(target);
        if (mr < 0 && mr != -EBUSY) kprintf("diskmgr: mounting the new %s volume failed (%d)\n", target->name, mr);
    }
    m->busy = NULL;
    reload(m);
    if (target && item_of(m, target)) m->sel = target;
    else if (item_of(m, disk)) m->sel = disk;
    if (r < 0) { m->sheet = SHEET_NONE; show_error(what, r); return true; }
    return true;
}

static void delete_partition(struct dm *m) {
    if (!can_delete(m)) return;
    struct blockdev *v = m->sel, *disk = v->parent;
    char nm[96], msg[256]; vol_name(v, nm, sizeof nm);
    snprintf(msg, sizeof msg, "Delete the partition “%s” (/dev/%s)?\nAll data on it will be lost. Other volumes on this disk are briefly unmounted.", nm, v->name);
    if (!dialog_confirm("Delete Partition", msg, "Delete", true)) return;
    uint64_t first = v->start_lba;
    struct mounted mt; remember_mounts(disk, &mt);
    storage_unmount(disk);
    int r = gpt_delete_partition(disk, first);
    blockdev_rescan(disk);
    remount(disk, &mt, first);
    reload(m);
    m->sel = disk;
    if (r < 0) show_error("The partition couldn’t be deleted.", r);
}

/* ------------------------------------------------------------------ input */
static void select_step(struct dm *m, int dir) {
    if (!m->nitems) return;
    int idx = -1; for (int i = 0; i < m->nitems; i++) if (m->items[i].dev == m->sel) idx = i;
    idx = CLAMP(idx + dir, 0, m->nitems - 1);
    m->sel = m->items[idx].dev;
}
static int hit_toolbar(struct dm *m, int x, int y) {
    struct tbtn tb[4]; int n = toolbar(m, tb);
    for (int i = 0; i < n; i++) if (tb[i].enabled && rect_contains(tb[i].r, x, y)) return tb[i].id;
    return 0;
}
static void run_button(struct dm *m, int id) {
    switch (id) {
    case B_MOUNT: toggle_mount(m); break;
    case B_ERASE: open_sheet(m, SHEET_ERASE); break;
    case B_ADD: open_sheet(m, SHEET_ADD); break;
    case B_DELETE: delete_partition(m); break;
    }
}
static void click_main(struct dm *m, int x, int y) {
    if (x < SIDEBAR_W) {
        layout_sidebar(m);
        for (int i = 0; i < m->nitems; i++) if (y >= m->items[i].y && y < m->items[i].y + m->items[i].h) { m->sel = m->items[i].dev; return; }
        return;
    }
    if ((m->pressed = hit_toolbar(m, x, y))) return;
    struct rect mr = map_rect(m);
    if (m->sel && rect_contains(mr, x, y)) {
        struct blockdev *disk = disk_of(m->sel);
        struct seg segs[MAX_SEGS]; int n = disk_segments(m, disk, segs, MAX_SEGS);
        int x0[MAX_SEGS], x1[MAX_SEGS]; seg_extents(segs, n, mr, x0, x1);
        for (int i = 0; i < n; i++) if (x >= x0[i] && x < x1[i]) { m->sel = segs[i].vol ? segs[i].vol : disk; break; }
    }
}
static void click_sheet(struct dm *m, int x, int y, bool down) {
    struct sheet_layout L = sheet_layout(m);
    if (down) {
        m->name.focused = m->size.focused = false;
        if (rect_contains(L.name, x, y)) textfield_mouse(&m->name, L.name, x, y);
        else if (m->sheet == SHEET_ADD && rect_contains(L.size, x, y)) textfield_mouse(&m->size, L.size, x, y);
        else if (rect_contains(L.fmt0, x, y)) m->fmt = FMT_FAT32;
        else if (rect_contains(L.fmt1, x, y)) m->fmt = FMT_BTRFS;
        else if (rect_contains(L.cancel, x, y)) m->pressed = B_CANCEL;
        else if (rect_contains(L.ok, x, y)) m->pressed = B_OK;
        if (!m->name.focused && !m->size.focused) m->name.focused = true;
        return;
    }
    int p = m->pressed; m->pressed = 0;
    if (p == B_CANCEL && rect_contains(L.cancel, x, y)) m->sheet = SHEET_NONE;
    else if (p == B_OK && rect_contains(L.ok, x, y) && sheet_commit(m)) m->sheet = SHEET_NONE;
}

void diskmgr_main(void *arg) {
    struct dm *m = kzalloc(sizeof *m);
    m->w = window_create("Disk Manager", 860, 560, WIN_STANDARD | WIN_FULL_TITLE);
    window_set_min_size(m->w, 720, 480);
    window_set_menus(m->w, &dm_menus);
    reload(m);
    draw(m);
    window_show(m->w);
    window_set_timer(m->w, 500);   /* caret blink, hotplug, usage */
    struct event ev; bool quit = false;
    while (!quit && window_wait_event(m->w, &ev, -1)) {
        switch (ev.type) {
        case EV_QUIT: case EV_CLOSE: quit = true; break;
        case EV_OPEN: kfree(ev.path); break;
        case EV_TIMER: if (!m->sheet && devices_changed(m)) reload(m); break;
        case EV_MENU:
            if (ev.cmd == CMD_QUIT || ev.cmd == CMD_CLOSE) quit = true;
            else if (m->sheet) break;
            else if (ev.cmd == DCMD_MOUNT) toggle_mount(m);
            else if (ev.cmd == DCMD_ERASE) open_sheet(m, SHEET_ERASE);
            else if (ev.cmd == DCMD_ADD) open_sheet(m, SHEET_ADD);
            else if (ev.cmd == DCMD_DELETE) delete_partition(m);
            else if (ev.cmd == DCMD_REFRESH) reload(m);
            break;
        case EV_MOUSE_DOWN:
            if (ev.button != BTN_LEFT) break;
            if (m->sheet) click_sheet(m, ev.x, ev.y, true); else click_main(m, ev.x, ev.y);
            break;
        case EV_MOUSE_UP:
            if (m->sheet) click_sheet(m, ev.x, ev.y, false);
            else { int p = m->pressed; m->pressed = 0; if (p && hit_toolbar(m, ev.x, ev.y) == p) { draw(m); run_button(m, p); } }
            break;
        case EV_KEY_DOWN:
            if (m->sheet) {
                if (m->busy) break;
                if (ev.key == KEY_ESC) m->sheet = SHEET_NONE;
                else if (ev.key == KEY_ENTER) { if (sheet_commit(m)) m->sheet = SHEET_NONE; }
                else if (ev.key == KEY_TAB && m->sheet == SHEET_ADD) { bool n = m->name.focused; m->name.focused = !n; m->size.focused = n; }
                else if (m->name.focused) textfield_key(&m->name, &ev);
                else if (m->size.focused) textfield_key(&m->size, &ev);
            }
            else if (ev.key == KEY_UP) select_step(m, -1);
            else if (ev.key == KEY_DOWN) select_step(m, 1);
            break;
        default: break;
        }
        if (!quit) draw(m);
    }
    window_destroy(m->w);
    kfree(m);
}
