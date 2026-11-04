#pragma once
/*
 * User address spaces: a process's memory is a sorted list of areas (VMAs),
 * each with its protection. Anonymous memory is demand-zero (a page is
 * allocated on first touch); fork shares every private page copy-on-write.
 * The page fault handler only allocates and copies pages, so it never sleeps.
 */
#include "kernel.h"
#include "list.h"

struct process;

#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_NORESERVE 0x4000
#define MAP_POPULATE  0x8000
#define MAP_STACK     0x20000
#define MAP_FIXED_NOREPLACE 0x100000
#define MAP_FAILED_ERR(e) ((int64_t)-(e))

#define VMA_SHARED 0x1     /* fork shares the pages instead of copying them */
#define VMA_STACK  0x2
#define VMA_HEAP   0x4

struct vma {
    struct list_head link;
    uint64_t start, end;   /* page aligned, end exclusive */
    uint32_t prot, flags;
};

#define USER_MMAP_TOP   0x00007f0000000000ULL   /* mmap hands out addresses below this, top down */
#define USER_MMAP_FLOOR 0x0000200000000000ULL

void uvm_init(struct process *p);
/* addr is a hint unless MAP_FIXED; returns the address or -errno */
int64_t uvm_map(struct process *p, uint64_t addr, uint64_t len, uint32_t prot, uint32_t flags);
int uvm_unmap(struct process *p, uint64_t addr, uint64_t len);
int uvm_protect(struct process *p, uint64_t addr, uint64_t len, uint32_t prot);
/* allocate every page of [start, end) now (ELF segments, MAP_POPULATE) */
int uvm_populate(struct process *p, uint64_t start, uint64_t end);
/* a fault at addr: 0 once the page is mapped, -EFAULT (no access: SIGSEGV), -ENOMEM (SIGBUS) */
int uvm_fault(struct process *p, uint64_t addr, bool write, bool exec);
int uvm_fork(struct process *parent, struct process *child);
void uvm_clear(struct process *p);          /* unmap everything (execve) */
int64_t uvm_brk(struct process *p, uint64_t new_brk);   /* 0: query; returns the (new) break */
bool uvm_user_page_present(struct process *p, uint64_t addr);
uint64_t uvm_resident_bytes(struct process *p);
