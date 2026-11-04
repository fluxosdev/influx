/* The standard character devices: /dev/null, zero, full, random, urandom, console and kmsg. */
#include "fs.h"
#include "random.h"
#include "string.h"
#include "arch.h"
#include "mm.h"

static int64_t null_read(struct vnode *v, uint64_t off, void *buf, size_t n) { return 0; }
static int64_t sink_write(struct vnode *v, uint64_t off, const void *buf, size_t n) { return (int64_t)n; }
static int64_t zero_read(struct vnode *v, uint64_t off, void *buf, size_t n) { memset(buf, 0, n); return (int64_t)n; }
static int64_t full_write(struct vnode *v, uint64_t off, const void *buf, size_t n) { return -ENOSPC; }
static int64_t random_read(struct vnode *v, uint64_t off, void *buf, size_t n) { random_bytes(buf, n); return (int64_t)n; }
static int64_t random_write(struct vnode *v, uint64_t off, const void *buf, size_t n) {
    const uint8_t *b = buf; for (size_t i = 0; i < n; i += 8) { uint64_t x = 0; memcpy(&x, b + i, MIN(8, n - i)); random_add_entropy(x); }
    return (int64_t)n;
}
/* the kernel log and the serial port: what CI and a headless boot see */
static int64_t console_write(struct vnode *v, uint64_t off, const void *buf, size_t n) {
    for (size_t i = 0; i < n; i += 512) kprintf("%.*s", (int)MIN((size_t)512, n - i), (const char *)buf + i);
    return (int64_t)n;
}
static int64_t kmsg_read(struct vnode *v, uint64_t off, void *buf, size_t n) {
    char *tmp = kmalloc(128 * 1024);
    if (!tmp) return -ENOMEM;
    size_t len = klog_snapshot(tmp, 128 * 1024);
    int64_t r = off >= len ? 0 : (int64_t)MIN(n, len - off);
    if (r > 0) memcpy(buf, tmp + off, (size_t)r);
    kfree(tmp);
    return r;
}

static const struct vnode_ops null_ops = { null_read, sink_write };
static const struct vnode_ops zero_ops = { zero_read, sink_write };
static const struct vnode_ops full_ops = { zero_read, full_write };
static const struct vnode_ops random_ops = { random_read, random_write };
static const struct vnode_ops console_ops = { null_read, console_write };
static const struct vnode_ops kmsg_ops = { kmsg_read, console_write };

void devices_init(void) {
    devfs_add("null", VN_DEV, &null_ops, NULL, 0);
    devfs_add("zero", VN_DEV, &zero_ops, NULL, 0);
    devfs_add("full", VN_DEV, &full_ops, NULL, 0);
    devfs_add("random", VN_DEV, &random_ops, NULL, 0);
    devfs_add("urandom", VN_DEV, &random_ops, NULL, 0);
    devfs_add("console", VN_DEV, &console_ops, NULL, 0);
    devfs_add("kmsg", VN_DEV, &kmsg_ops, NULL, 0);
}
