#include "mm.h"
#include "string.h"
#include "io.h"
#include "spinlock.h"
#include "arch.h"
#include <limine.h>

extern volatile struct limine_memmap_request memmap_request;
extern char __kernel_start[], __kernel_end[], __text_start[], __text_end[], __rodata_start[], __rodata_end[];

static uint64_t kernel_pml4_phys;
static spinlock_t lock = SPINLOCK_INIT;

static uint64_t *table_of(uint64_t phys) { return P2V(phys & PTE_ADDR_MASK); }

/* Walk/allocate down to the page table for virt; returns pointer to the PTE */
static uint64_t *walk(uint64_t pml4, uint64_t virt, bool create, uint64_t table_flags) {
    uint64_t *t = table_of(pml4);
    for (int level = 3; level > 0; level--) {
        uint64_t idx = (virt >> (12 + level * 9)) & 0x1FF;
        if (!(t[idx] & PTE_PRESENT)) {
            if (!create) return NULL;
            uint64_t p = pmm_alloc_page();
            t[idx] = p | PTE_PRESENT | PTE_WRITE | table_flags;
        } else if (t[idx] & PTE_HUGE) {
            return &t[idx];   /* caller must handle huge pages */
        } else if (table_flags & PTE_USER) {
            t[idx] |= PTE_USER;
        }
        t = table_of(t[idx]);
    }
    return &t[(virt >> 12) & 0x1FF];
}

static void map_2m(uint64_t pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *t = table_of(pml4);
    for (int level = 3; level > 1; level--) {
        uint64_t idx = (virt >> (12 + level * 9)) & 0x1FF;
        if (!(t[idx] & PTE_PRESENT)) t[idx] = pmm_alloc_page() | PTE_PRESENT | PTE_WRITE;
        t = table_of(t[idx]);
    }
    t[(virt >> 21) & 0x1FF] = phys | flags | PTE_HUGE | PTE_PRESENT;
}

/* Replacing or removing a present mapping must reach every CPU's TLB. User
 * processes are single-threaded and every CR3 load flushes their entries,
 * but the other CPUs may hold stale (global) kernel-half entries. */
static void flush_after(uint64_t virt, uint64_t pages, bool was_present) {
    if (was_present && virt >= 0xFFFF800000000000ULL) tlb_shootdown(virt, pages);
    else tlb_flush_local(virt, pages);
}

void vmm_map(uint64_t pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    spin_lock(&lock);
    uint64_t *pte = walk(pml4, virt, true, flags & PTE_USER);
    bool was = *pte & PTE_PRESENT;
    *pte = (phys & PTE_ADDR_MASK) | flags | PTE_PRESENT;
    spin_unlock(&lock);
    flush_after(virt, 1, was);
}

void vmm_unmap(uint64_t pml4, uint64_t virt) {
    spin_lock(&lock);
    uint64_t *pte = walk(pml4, virt, false, 0);
    bool was = pte && (*pte & PTE_PRESENT);
    if (pte) *pte = 0;
    spin_unlock(&lock);
    flush_after(virt, 1, was);
}

uint64_t vmm_get_phys(uint64_t pml4, uint64_t virt) {
    uint64_t *t = table_of(pml4);
    for (int level = 3; level >= 0; level--) {
        uint64_t idx = (virt >> (12 + level * 9)) & 0x1FF;
        if (!(t[idx] & PTE_PRESENT)) return 0;
        if (level == 0) return (t[idx] & PTE_ADDR_MASK) | (virt & 0xFFF);
        if (t[idx] & PTE_HUGE) {
            uint64_t sz = 1ULL << (12 + level * 9);
            return (t[idx] & PTE_ADDR_MASK & ~(sz - 1)) | (virt & (sz - 1));
        }
        t = table_of(t[idx]);
    }
    return 0;
}

void vmm_init(void) {
    kernel_pml4_phys = pmm_alloc_page();
    uint64_t *pml4 = table_of(kernel_pml4_phys);
    /* Pre-populate every kernel-half PML4 entry so later kernel mappings are
     * visible in every address space (they all share these tables). */
    for (int i = 256; i < 512; i++) pml4[i] = pmm_alloc_page() | PTE_PRESENT | PTE_WRITE;

    /* HHDM: first 4 GiB plus every memory map region, 2 MiB pages */
    struct limine_memmap_response *mm = memmap_request.response;
    uint64_t top = 4ULL * 1024 * 1024 * 1024;
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        uint64_t e = mm->entries[i]->base + mm->entries[i]->length;
        if (e > top) top = e;
    }
    top = ALIGN_UP(top, 2 * 1024 * 1024);
    for (uint64_t p = 0; p < top; p += 2 * 1024 * 1024) map_2m(kernel_pml4_phys, hhdm_offset + p, p, PTE_WRITE | PTE_GLOBAL);
    /* framebuffer regions: write-combining (PAT index 1 via PWT) */
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type != LIMINE_MEMMAP_FRAMEBUFFER) continue;
        for (uint64_t p = ALIGN_DOWN(e->base, 2 * 1024 * 1024); p < ALIGN_UP(e->base + e->length, 2 * 1024 * 1024); p += 2 * 1024 * 1024)
            map_2m(kernel_pml4_phys, hhdm_offset + p, p, PTE_WRITE | PTE_GLOBAL | PTE_PWT);
    }
    /* kernel image, 4 KiB pages with proper permissions */
    uint64_t kv = boot_info.kernel_virt, kp = boot_info.kernel_phys;
    for (uint64_t v = (uint64_t)__kernel_start; v < PAGE_ALIGN_UP((uint64_t)__kernel_end); v += PAGE_SIZE) {
        uint64_t flags = PTE_GLOBAL;
        if (v >= (uint64_t)__text_start && v < (uint64_t)__text_end) flags |= 0;
        else if (v >= (uint64_t)__rodata_start && v < (uint64_t)__rodata_end) flags |= PTE_NX;
        else flags |= PTE_WRITE | PTE_NX;
        uint64_t *pte = walk(kernel_pml4_phys, v, true, 0);
        *pte = (v - kv + kp) | flags | PTE_PRESENT;
    }
    write_cr3(kernel_pml4_phys);
    kprintf("vmm: kernel page tables active, HHDM covers %lu MiB\n", top / (1024 * 1024));
}

uint64_t vmm_kernel_pml4(void) { return kernel_pml4_phys; }

uint64_t vmm_new_pml4(void) {
    uint64_t p = pmm_alloc_page();
    uint64_t *t = table_of(p), *k = table_of(kernel_pml4_phys);
    for (int i = 256; i < 512; i++) t[i] = k[i];
    return p;
}

static void free_level(uint64_t table, int level) {
    uint64_t *t = table_of(table);
    for (int i = 0; i < 512; i++) {
        if (!(t[i] & PTE_PRESENT)) continue;
        if (level > 0 && !(t[i] & PTE_HUGE)) free_level(t[i] & PTE_ADDR_MASK, level - 1);
        else pmm_free_page(t[i] & PTE_ADDR_MASK);
    }
    pmm_free_page(table);
}

void vmm_free_pml4(uint64_t pml4) {
    uint64_t *t = table_of(pml4);
    for (int i = 0; i < 256; i++) if (t[i] & PTE_PRESENT) free_level(t[i] & PTE_ADDR_MASK, 2);
    pmm_free_page(pml4);
}

static uint64_t count_level(uint64_t table, int level) {
    uint64_t *t = table_of(table), n = 0;
    for (int i = 0; i < 512; i++) {
        if (!(t[i] & PTE_PRESENT)) continue;
        if (level > 0 && !(t[i] & PTE_HUGE)) n += count_level(t[i] & PTE_ADDR_MASK, level - 1);
        else n++;
    }
    return n;
}
uint64_t vmm_user_pages(uint64_t pml4) {
    uint64_t *t = table_of(pml4), n = 0;
    for (int i = 0; i < 256; i++) if (t[i] & PTE_PRESENT) n += count_level(t[i] & PTE_ADDR_MASK, 2);
    return n;
}

void vmm_switch(uint64_t pml4) { if (read_cr3() != pml4) write_cr3(pml4); }

static void *map_mmio(uint64_t phys, size_t size, uint64_t cache_bits);
void *vmm_map_mmio(uint64_t phys, size_t size) { return map_mmio(phys, size, PTE_PCD); }
void *vmm_map_wc(uint64_t phys, size_t size) { return map_mmio(phys, size, PTE_PWT); }
static void *map_mmio(uint64_t phys, size_t size, uint64_t cache_bits) {
    uint64_t start = PAGE_ALIGN_DOWN(phys), end = PAGE_ALIGN_UP(phys + size);
    bool changed = false;   /* replaced a live translation (HHDM RAM, a split 2 MiB page) */
    for (uint64_t p = start; p < end; p += PAGE_SIZE) {
        /* If the HHDM already covers this via a 2 MiB page, remap that 2 MiB
         * region with 4 KiB uncached pages. */
        spin_lock(&lock);
        uint64_t *t = table_of(kernel_pml4_phys);
        uint64_t v = hhdm_offset + p;
        uint64_t *pdpte = &t[(v >> 39) & 0x1FF];
        if (!(*pdpte & PTE_PRESENT)) *pdpte = pmm_alloc_page() | PTE_PRESENT | PTE_WRITE;
        uint64_t *pde = &table_of(*pdpte)[(v >> 30) & 0x1FF];
        if (!(*pde & PTE_PRESENT)) *pde = pmm_alloc_page() | PTE_PRESENT | PTE_WRITE;
        uint64_t *pdee = &table_of(*pde)[(v >> 21) & 0x1FF];
        if (!(*pdee & PTE_PRESENT)) *pdee = pmm_alloc_page() | PTE_PRESENT | PTE_WRITE;
        if (*pdee & PTE_HUGE) {
            changed = true;
            uint64_t base = *pdee & PTE_ADDR_MASK & ~0x1FFFFFULL;
            uint64_t pt = pmm_alloc_page();
            uint64_t *ptt = table_of(pt);
            for (int i = 0; i < 512; i++) ptt[i] = (base + i * PAGE_SIZE) | PTE_PRESENT | PTE_WRITE | PTE_GLOBAL;
            *pdee = pt | PTE_PRESENT | PTE_WRITE;
        }
        uint64_t *pte = &table_of(*pdee)[(v >> 12) & 0x1FF];
        uint64_t nv = p | PTE_PRESENT | PTE_WRITE | PTE_GLOBAL | cache_bits | PTE_NX;
        if ((*pte & PTE_PRESENT) && *pte != nv) changed = true;
        *pte = nv;
        spin_unlock(&lock);
    }
    flush_after(hhdm_offset + start, (end - start) / PAGE_SIZE, changed);
    return P2V(phys);
}
