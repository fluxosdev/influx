#include "mm.h"
#include "string.h"
#include "spinlock.h"
#include <limine.h>

extern volatile struct limine_memmap_request memmap_request;

static uint8_t *bitmap;      /* 1 bit per page, 1 = used */
static uint64_t bitmap_pages; /* number of pages tracked */
static uint64_t total_pages, free_pages;
static uint64_t hint;
static spinlock_t lock = SPINLOCK_INIT;

static inline bool bm_test(uint64_t p) { return bitmap[p / 8] & (1 << (p % 8)); }
static inline void bm_set(uint64_t p) { bitmap[p / 8] |= (1 << (p % 8)); }
static inline void bm_clear(uint64_t p) { bitmap[p / 8] &= ~(1 << (p % 8)); }

void pmm_init(void) {
    struct limine_memmap_response *mm = memmap_request.response;
    uint64_t highest = 0;
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE) {
            if (e->base + e->length > highest) highest = e->base + e->length;
            total_pages += e->length / PAGE_SIZE;
        }
    }
    bitmap_pages = highest / PAGE_SIZE;
    uint64_t bm_size = PAGE_ALIGN_UP(bitmap_pages / 8 + 1);
    /* place the bitmap in the first usable region large enough */
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE && e->length >= bm_size) {
            bitmap = P2V(e->base);
            break;
        }
    }
    ASSERT(bitmap);
    memset(bitmap, 0xFF, bm_size);
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;
        for (uint64_t p = e->base / PAGE_SIZE; p < (e->base + e->length) / PAGE_SIZE; p++) { bm_clear(p); free_pages++; }
    }
    /* reserve the bitmap itself and page 0 */
    uint64_t bm_phys = V2P(bitmap);
    for (uint64_t p = bm_phys / PAGE_SIZE; p < (bm_phys + bm_size) / PAGE_SIZE; p++) { bm_set(p); free_pages--; }
    if (!bm_test(0)) { bm_set(0); free_pages--; }
    boot_info.mem_total = total_pages * PAGE_SIZE;
    kprintf("pmm: %lu MiB usable, bitmap %lu KiB at %lx\n", (uint64_t)(total_pages * PAGE_SIZE / (1024 * 1024)), (uint64_t)(bm_size / 1024), bm_phys);
}

uint64_t pmm_alloc_pages(size_t count) {
    if (!count) return 0;
    spin_lock(&lock);
    for (int pass = 0; pass < 2; pass++) {
        uint64_t start = pass == 0 ? hint : 1;
        uint64_t run = 0;
        for (uint64_t p = start; p < bitmap_pages; p++) {
            if (bm_test(p)) { run = 0; continue; }
            if (++run == count) {
                uint64_t first = p + 1 - count;
                for (uint64_t q = first; q <= p; q++) bm_set(q);
                free_pages -= count;
                hint = p + 1;
                spin_unlock(&lock);
                memset(P2V(first * PAGE_SIZE), 0, count * PAGE_SIZE);
                return first * PAGE_SIZE;
            }
        }
    }
    spin_unlock(&lock);
    panic("pmm: out of memory (%zu pages requested, %lu free)", count, free_pages);
}
uint64_t pmm_alloc_page(void) { return pmm_alloc_pages(1); }

void pmm_free_pages(uint64_t phys, size_t count) {
    spin_lock(&lock);
    uint64_t first = phys / PAGE_SIZE;
    for (uint64_t p = first; p < first + count; p++) { if (bm_test(p)) { bm_clear(p); free_pages++; } }
    if (first < hint) hint = first;
    spin_unlock(&lock);
}
void pmm_free_page(uint64_t phys) { pmm_free_pages(phys, 1); }
uint64_t pmm_total_bytes(void) { return total_pages * PAGE_SIZE; }
uint64_t pmm_free_bytes(void) { return free_pages * PAGE_SIZE; }
uint64_t pmm_used_bytes(void) { return (total_pages - free_pages) * PAGE_SIZE; }
