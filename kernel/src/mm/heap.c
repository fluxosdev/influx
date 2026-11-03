#include "mm.h"
#include "string.h"
#include "spinlock.h"
#include "sched.h"

/* Bucket allocator for small objects, page allocator for large ones. */
#define NBUCKETS 8
static const uint32_t bucket_sizes[NBUCKETS] = { 32, 64, 128, 256, 512, 1024, 2048, 4096 };
#define SLAB_PAGES 8
#define HDR_MAGIC 0x4B48
#define LARGE_MAGIC 0x4C47

struct hdr { uint16_t magic; uint8_t bucket; uint8_t pad; uint16_t pid; uint16_t pad2; uint32_t size; uint32_t npages; };
struct freeblk { struct freeblk *next; };

static struct freeblk *freelist[NBUCKETS];
static spinlock_t lock = SPINLOCK_INIT;
static uint64_t used_bytes, total_bytes;

void heap_init(void) {}

static void account(int pid, int64_t delta) {
    if (pid <= 0) return;
    uint64_t f = irq_save();   /* single CPU: nothing can free p until irqs come back */
    struct process *p = process_find(pid);
    if (p) p->heap_bytes += delta;
    irq_restore(f);
}

static void refill(int b) {
    uint64_t phys = pmm_alloc_pages(SLAB_PAGES);
    total_bytes += SLAB_PAGES * PAGE_SIZE;
    uint8_t *base = P2V(phys);
    uint32_t stride = bucket_sizes[b];
    for (uint32_t off = 0; off + stride <= SLAB_PAGES * PAGE_SIZE; off += stride) {
        struct freeblk *f = (struct freeblk *)(base + off);
        f->next = freelist[b];
        freelist[b] = f;
    }
}

void *kmalloc(size_t n) {
    if (n == 0) n = 1;
    size_t need = n + sizeof(struct hdr);
    int pid = current_thread && current_thread->proc ? current_thread->proc->pid : 0;
    if (need > bucket_sizes[NBUCKETS - 1]) {
        size_t pages = PAGE_ALIGN_UP(need) / PAGE_SIZE;
        uint64_t phys = pmm_alloc_pages(pages);
        struct hdr *h = P2V(phys);
        h->magic = LARGE_MAGIC; h->npages = pages; h->size = n; h->pid = pid;
        spin_lock(&lock); used_bytes += pages * PAGE_SIZE; total_bytes += pages * PAGE_SIZE; spin_unlock(&lock);
        account(pid, pages * PAGE_SIZE);
        return h + 1;
    }
    int b = 0;
    while (bucket_sizes[b] < need) b++;
    spin_lock(&lock);
    if (!freelist[b]) refill(b);
    struct freeblk *f = freelist[b];
    freelist[b] = f->next;
    used_bytes += bucket_sizes[b];
    spin_unlock(&lock);
    struct hdr *h = (struct hdr *)f;
    h->magic = HDR_MAGIC; h->bucket = b; h->size = n; h->pid = pid; h->npages = 0;
    account(pid, bucket_sizes[b]);
    return h + 1;
}

void kfree(void *p) {
    if (!p) return;
    struct hdr *h = (struct hdr *)p - 1;
    if (h->magic == LARGE_MAGIC) {
        size_t pages = h->npages;
        account(h->pid, -(int64_t)(pages * PAGE_SIZE));
        spin_lock(&lock); used_bytes -= pages * PAGE_SIZE; total_bytes -= pages * PAGE_SIZE; spin_unlock(&lock);
        h->magic = 0;
        pmm_free_pages(V2P(h), pages);
        return;
    }
    if (h->magic != HDR_MAGIC) panic("kfree: bad magic %x at %p", h->magic, p);
    int b = h->bucket;
    account(h->pid, -(int64_t)bucket_sizes[b]);
    h->magic = 0;
    spin_lock(&lock);
    struct freeblk *f = (struct freeblk *)h;
    f->next = freelist[b];
    freelist[b] = f;
    used_bytes -= bucket_sizes[b];
    spin_unlock(&lock);
}

void *kzalloc(size_t n) { void *p = kmalloc(n); if (p) memset(p, 0, n); return p; }
void *kcalloc(size_t a, size_t b) { return kzalloc(a * b); }
void *krealloc(void *p, size_t n) {
    if (!p) return kmalloc(n);
    struct hdr *h = (struct hdr *)p - 1;
    size_t cap = h->magic == LARGE_MAGIC ? h->npages * PAGE_SIZE - sizeof(struct hdr) : bucket_sizes[h->bucket] - sizeof(struct hdr);
    if (n <= cap) { h->size = n; return p; }
    void *np = kmalloc(n);
    memcpy(np, p, h->size < n ? h->size : n);
    kfree(p);
    return np;
}
char *kstrdup(const char *s) { return strdup(s); }
uint64_t heap_used_bytes(void) { return used_bytes; }
uint64_t heap_total_bytes(void) { return total_bytes; }
