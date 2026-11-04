/*
 * Kernel log and crash reports.
 *
 * Everything kprintf prints also goes into a 128 KiB ring (dmesg). On an
 * installed system, crashlog_init() reserves a 256 KiB file, crash.log, on
 * the system's EFI partition (FAT32, which has no data checksums) and records
 * the disk sectors it occupies. When the kernel panics, the panic handler
 * writes the tail of the log straight to those sectors through the polled
 * AHCI/NVMe driver, bypassing the filesystem, then shows the message on
 * screen. At the next boot crashlog_init() finds the report, copies it to
 * ~/Crash Reports and clears it, and the desktop says so.
 */
#include "kernel.h"
#include "arch.h"
#include "io.h"
#include "fs.h"
#include "blockdev.h"
#include "gfx.h"
#include "mm.h"
#include "string.h"
#include "sched.h"

#define RING_SIZE (128 * 1024)
static char ring[RING_SIZE];
static uint64_t ring_total;            /* bytes ever written */

volatile bool panicking;

void klog_write(const char *s, size_t n) {   /* print_lock held */
    for (size_t i = 0; i < n; i++) ring[(ring_total + i) % RING_SIZE] = s[i];
    ring_total += n;
}

/* copies the last (up to) max bytes of the log; returns the count */
size_t klog_snapshot(char *out, size_t max) {
    uint64_t total = __atomic_load_n(&ring_total, __ATOMIC_ACQUIRE);
    size_t n = (size_t)MIN((uint64_t)max, MIN(total, (uint64_t)RING_SIZE));
    uint64_t start = total - n;
    for (size_t i = 0; i < n; i++) out[i] = ring[(start + i) % RING_SIZE];
    return n;
}

/* ---- crash log on the EFI partition ---- */
#define CRASH_BYTES (256 * 1024)
#define CRASH_MAGIC "INFLUX-CRASH-1\n"
struct crash_hdr { char magic[16]; uint64_t time; uint32_t len, fresh; char reason[200]; } PACKED;
struct extent { uint64_t lba; uint32_t count; };
static struct { struct blockdev *dev; struct extent ext[16]; int next; uint32_t sectors, ss; uint8_t *buf; } crash;

int fat32_extents(struct vnode *file, struct blockdev **dev, uint64_t *lba, uint32_t *count, int max);

static void save_previous(const struct crash_hdr *h, const char *text, size_t len) {
    struct rtc_time t; unix_to_datetime(h->time + tz_offset_minutes * 60, &t);
    vfs_mkdir("/home/flux/Crash Reports");
    char path[PATH_MAX];
    snprintf(path, sizeof path, "/home/flux/Crash Reports/%04d-%02d-%02d %02d.%02d.%02d.log", t.year, t.month, t.day, t.hour, t.minute, t.second);
    struct file *f;
    if (vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC, &f) < 0) return;
    char head[320];
    int n = snprintf(head, sizeof head, "inFlux kernel panic: %s\n\n", h->reason);
    vfs_write(f, head, n);
    vfs_write(f, text, len);
    vfs_close(f);
    kprintf("crashlog: a crash report from the previous boot was saved to %s\n", path);
    extern char crashlog_previous[PATH_MAX];
    strlcpy(crashlog_previous, path, PATH_MAX);
}
char crashlog_previous[PATH_MAX];   /* path of the report saved at this boot ("" = none) */

/* the ESP of the disk that holds /home (an installed inFlux) */
static bool find_esp(char *mp, size_t n) {
    struct blockdev *home = NULL;
    for (int i = 0; i < vfs_mount_count(); i++) {
        char p[PATH_MAX]; struct vnode *r = vfs_mount_at(i, p, sizeof p);
        if (r && !strcmp(p, "/home")) home = r->priv;
    }
    if (!home || !home->parent) return false;
    for (int i = 0; i < vfs_mount_count(); i++) {
        char p[PATH_MAX]; struct vnode *r = vfs_mount_at(i, p, sizeof p);
        struct blockdev *d = r ? r->priv : NULL;
        if (!r || !r->fs_name || strcmp(r->fs_name, "fat32") || !d) continue;
        bool ours = false;
        for (int j = 0; j < blockdev_count(); j++) if (blockdev_at(j) == d && d->esp && d->parent == home->parent) ours = true;
        if (!ours) continue;
        char k[PATH_MAX]; snprintf(k, sizeof k, "%s/boot/influx.elf", p);
        struct stat st;
        if (vfs_stat(k, &st) < 0) continue;
        strlcpy(mp, p, n);
        return true;
    }
    return false;
}

void crashlog_init(void) {
    char mp[PATH_MAX];
    if (!find_esp(mp, sizeof mp)) return;
    char path[PATH_MAX]; snprintf(path, sizeof path, "%s/crash.log", mp);
    void *data = NULL; size_t sz = 0;
    if (vfs_read_file(path, &data, &sz) == 0 && sz >= 512) {
        struct crash_hdr *h = data;
        if (!memcmp(h->magic, CRASH_MAGIC, sizeof CRASH_MAGIC - 1) && h->fresh == 1 && h->len <= sz - 512) {
            h->reason[sizeof h->reason - 1] = 0;
            save_previous(h, (char *)data + 512, h->len);
        }
    }
    if (data) kfree(data);
    if (sz != CRASH_BYTES) {
        /* (re)create it full size, so the panic handler never has to allocate */
        uint8_t *z = kzalloc(CRASH_BYTES);
        if (!z) return;
        int r = vfs_write_file(path, z, CRASH_BYTES);
        kfree(z);
        if (r < 0) { kprintf("crashlog: cannot create %s (%d)\n", path, r); return; }
    } else {
        /* mark the stored report as read */
        struct crash_hdr h = { 0 };
        struct file *f;
        if (vfs_open(path, O_WRONLY, &f) == 0) { vfs_write(f, &h, sizeof h); vfs_close(f); }
    }
    struct vnode *esp_root = NULL;
    for (int i = 0; i < vfs_mount_count(); i++) { char p[PATH_MAX]; struct vnode *r = vfs_mount_at(i, p, sizeof p); if (r && !strcmp(p, mp)) esp_root = r; }
    if (esp_root && esp_root->ops->sync) esp_root->ops->sync(esp_root);
    struct vnode *v = esp_root ? esp_root->ops->lookup(esp_root, "crash.log") : NULL;
    if (!v) return;
    uint64_t lba[16]; uint32_t cnt[16]; struct blockdev *dev = NULL;
    int ne = fat32_extents(v, &dev, lba, cnt, 16);
    vnode_unref(v);
    if (ne <= 0 || !dev) { kprintf("crashlog: crash.log is too fragmented\n"); return; }
    uint32_t total = 0;
    for (int i = 0; i < ne; i++) { crash.ext[i].lba = lba[i]; crash.ext[i].count = cnt[i]; total += cnt[i]; }
    crash.ss = dev->sector_size ? dev->sector_size : 512;
    if (total * crash.ss < CRASH_BYTES) return;
    crash.buf = P2V(pmm_alloc_pages(CRASH_BYTES / PAGE_SIZE));   /* physically contiguous, for DMA */
    crash.next = ne; crash.sectors = CRASH_BYTES / crash.ss;
    crash.dev = dev;
    kprintf("crashlog: panics will be recorded in %s (%d extent%s on %s)\n", path, ne, ne == 1 ? "" : "s", dev->name);
}

int blockdev_write_panic(struct blockdev *d, uint64_t lba, uint32_t count, const void *buf);
int blockdev_flush_panic(struct blockdev *d);

static void crashlog_write(const char *reason) {
    if (!crash.dev) return;
    struct crash_hdr *h = (struct crash_hdr *)crash.buf;
    memset(crash.buf, 0, CRASH_BYTES);
    memcpy(h->magic, CRASH_MAGIC, sizeof CRASH_MAGIC - 1);
    h->time = time_unix(); h->fresh = 1;
    strlcpy(h->reason, reason, sizeof h->reason);
    h->len = (uint32_t)klog_snapshot((char *)crash.buf + 512, CRASH_BYTES - 512);
    uint32_t sectors = (uint32_t)((512 + h->len + crash.ss - 1) / crash.ss);
    uint32_t done = 0;
    for (int i = 0; i < crash.next && done < sectors; i++) {
        uint32_t n = MIN(crash.ext[i].count, sectors - done);
        int r = blockdev_write_panic(crash.dev, crash.ext[i].lba, n, crash.buf + (size_t)done * crash.ss);
        if (r < 0) { kprintf("crashlog: writing the report failed (%d)\n", r); return; }
        done += n;
    }
    blockdev_flush_panic(crash.dev);
    kprintf("crashlog: report written to the EFI partition\n");
}

/* ---- panic ---- */
/* symbol lookup in /boot/influx.map ("addr type name" lines, nm output), packed into the initrd */
static const void *initrd_find(const char *name, size_t *size) {
    const uint8_t *p = boot_info.initrd, *end = p + boot_info.initrd_size;
    while (p && p + 512 <= end && p[0]) {
        const char *n = (const char *)p;
        if (n[0] == '.' && n[1] == '/') n += 2;
        uint64_t sz = 0; for (int i = 0; i < 11; i++) sz = sz * 8 + (p[124 + i] - '0');
        if (!strcmp(n, name)) { *size = sz; return p + 512; }
        p += 512 + ALIGN_UP(sz, 512);
    }
    return NULL;
}
static void symbolize(uint64_t addr, char *out, size_t n) {
    size_t sz; const char *m = initrd_find("boot/influx.map", &sz);
    snprintf(out, n, "%016lx", addr);
    if (!m) return;
    uint64_t best = 0; const char *bname = NULL; int blen = 0;
    const char *p = m, *end = m + sz;
    while (p < end) {
        const char *nl = p; while (nl < end && *nl != '\n') nl++;
        uint64_t a = 0; const char *q = p;
        while (q < nl && ((*q >= '0' && *q <= '9') || (*q >= 'a' && *q <= 'f'))) { a = a * 16 + (uint64_t)(*q <= '9' ? *q - '0' : *q - 'a' + 10); q++; }
        if (q + 3 < nl && (q[1] == 'T' || q[1] == 't') && a <= addr && a > best) { best = a; bname = q + 3; blen = (int)(nl - bname); }
        p = nl + 1;
    }
    if (bname && addr - best < 0x100000) snprintf(out, n, "%016lx %.*s+0x%lx", addr, blen, bname, addr - best);
}

void panic_backtrace(uint64_t rip, uint64_t rbp) {
    char s[160];
    if (rip) { symbolize(rip, s, sizeof s); kprintf("  at %s\n", s); }
    for (int i = 0; i < 24 && rbp >= 0xffff800000000000ull && !(rbp & 7); i++) {
        uint64_t *fp = (uint64_t *)rbp;
        uint64_t ret = fp[1];
        if (ret < 0xffff800000000000ull) break;
        symbolize(ret, s, sizeof s); kprintf("  from %s\n", s);
        if (fp[0] <= rbp) break;   /* frames grow up the stack */
        rbp = fp[0];
    }
}

static void panic_screen(const char *reason) {
    extern struct canvas *screen;
    extern struct font *fb_panic_font(bool mono);
    extern void fb_flush_all(void);
    struct font *f = fb_panic_font(false), *mono = fb_panic_font(true);
    if (!screen || !f) return;
    int w = screen->w, h = screen->h;
    gfx_fill(screen, RGB(0x12, 0x12, 0x14));
    int y = h / 6, x = MAX(24, w / 10);
    font_draw(screen, f, x, y, RGB(0xFF, 0x6B, 0x60), "inFlux stopped because of a problem and needs to restart.");
    y += 28;
    font_draw(screen, f, x, y, RGB(0xE0, 0xE0, 0xE4), reason);
    y += 22;
    font_draw(screen, f, x, y, RGB(0x9a, 0x9a, 0xa0), crash.dev ? "A report was saved and will be in ~/Crash Reports after the restart. Hold the power button to turn the computer off."
                                                             : "Hold the power button to turn the computer off. The kernel log below is also on the serial port.");
    y += 36;
    /* the tail of the log: as many lines as fit */
    static char tail[16384];
    size_t n = klog_snapshot(tail, sizeof tail - 1); tail[n] = 0;
    int lh = font_height(mono) + 1, rows = (h - y - 24) / lh;
    if (rows <= 0) { fb_flush_all(); fb_flush_all(); return; }
    int lines = 0; for (size_t i = 0; i < n; i++) if (tail[i] == '\n') lines++;
    char *p = tail; int skip = lines - rows;
    while (skip > 0 && *p) { if (*p++ == '\n') skip--; }
    while (*p) {
        char *nl = strchr(p, '\n'); if (nl) *nl = 0;
        /* drop ANSI colour sequences */
        char line[256]; size_t o = 0;
        for (char *q = p; *q && o < sizeof line - 1; q++) { if (*q == 033) { while (*q && *q != 'm') q++; if (!*q) break; continue; } line[o++] = *q; }
        line[o] = 0;
        font_draw(screen, mono, x, y, RGB(0x8a, 0x8a, 0x90), line);
        y += lh;
        if (!nl) break;
        p = nl + 1;
    }
    fb_flush_all(); fb_flush_all();   /* both buffers of a page-flipping GPU */
}

void NORETURN panic_finish(const char *reason) {
    crashlog_write(reason);
    panic_screen(reason);
    for (;;) hlt();
}
