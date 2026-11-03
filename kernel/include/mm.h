#pragma once
#include "kernel.h"

/* Physical memory manager */
void pmm_init(void);
uint64_t pmm_alloc_pages(size_t count);     /* physical address, contiguous, zeroed */
uint64_t pmm_alloc_page(void);
uint64_t pmm_alloc_pages_low(size_t count); /* below 4 GiB, zeroed; 0 if none */
void pmm_free_pages(uint64_t phys, size_t count);
void pmm_free_page(uint64_t phys);
uint64_t pmm_total_bytes(void);
uint64_t pmm_free_bytes(void);
uint64_t pmm_used_bytes(void);

/* Virtual memory manager */
#define PTE_PRESENT  (1ULL << 0)
#define PTE_WRITE    (1ULL << 1)
#define PTE_USER     (1ULL << 2)
#define PTE_PWT      (1ULL << 3)
#define PTE_PCD      (1ULL << 4)
#define PTE_HUGE     (1ULL << 7)
#define PTE_GLOBAL   (1ULL << 8)
#define PTE_NX       (1ULL << 63)
#define PTE_ADDR_MASK 0x000ffffffffff000ULL

void vmm_init(void);
uint64_t vmm_kernel_pml4(void);                 /* physical */
uint64_t vmm_new_pml4(void);                    /* physical, kernel half shared */
void vmm_free_pml4(uint64_t pml4);              /* frees user half + tables */
void vmm_map(uint64_t pml4, uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_unmap(uint64_t pml4, uint64_t virt);
uint64_t vmm_get_phys(uint64_t pml4, uint64_t virt);   /* 0 if unmapped */
void vmm_switch(uint64_t pml4);
void *vmm_map_mmio(uint64_t phys, size_t size);  /* returns HHDM pointer, uncached */
void *vmm_map_wc(uint64_t phys, size_t size);    /* write-combining (VRAM apertures) */
uint64_t vmm_user_pages(uint64_t pml4);          /* count mapped user pages */

/* Kernel heap */
void heap_init(void);
void *kmalloc(size_t n);
void *kzalloc(size_t n);
void *kcalloc(size_t a, size_t b);
void *krealloc(void *p, size_t n);
void kfree(void *p);
char *kstrdup(const char *s);
uint64_t heap_used_bytes(void);
uint64_t heap_total_bytes(void);
