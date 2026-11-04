/* User address spaces: areas, demand paging, copy-on-write (see uvm.h). */
#include "uvm.h"
#include "uaccess.h"
#include "sched.h"
#include "mm.h"
#include "string.h"
#include "fs.h"
#include "arch.h"
#include "io.h"

#define PMM_RESERVE_PAGES 4096     /* left for the kernel: user memory fails before the kernel heap does */

static uint64_t *table_of(uint64_t e) { return P2V(e & PTE_ADDR_MASK); }

/* the PTE for virt in a user page table, creating the tables on the way when asked */
static uint64_t *pte_of(uint64_t pml4, uint64_t virt, bool create) {
    uint64_t *t = P2V(pml4);
    for (int level = 3; level > 0; level--) {
        uint64_t idx = (virt >> (12 + level * 9)) & 0x1FF;
        if (!(t[idx] & PTE_PRESENT)) {
            if (!create) return NULL;
            uint64_t pg = pmm_alloc_pages_try(1);
            if (!pg) return NULL;
            t[idx] = pg | PTE_PRESENT | PTE_WRITE | PTE_USER;
        }
        t = table_of(t[idx]);
    }
    return &t[(virt >> 12) & 0x1FF];
}

static uint64_t pte_flags(uint32_t prot) {
    uint64_t f = PTE_PRESENT | PTE_USER;
    if (prot & PROT_WRITE) f |= PTE_WRITE;
    if (!(prot & PROT_EXEC)) f |= PTE_NX;
    return f;
}

/* other threads of p may be running on other CPUs with this translation cached */
static void flush(struct process *p, uint64_t virt, uint64_t pages) {
    if (p->nthreads > 1) tlb_shootdown(virt, pages);
    else if (read_cr3() == p->pml4) tlb_flush_local(virt, pages);
}

static bool enough_memory(uint64_t pages) { return pmm_free_bytes() / PAGE_SIZE > pages + PMM_RESERVE_PAGES; }

void uvm_init(struct process *p) {
    list_init(&p->vmas);
    p->vm_lock = (spinlock_t)SPINLOCK_INIT;
}

static struct vma *find(struct process *p, uint64_t addr) {
    struct vma *v;
    list_for_each_entry(v, &p->vmas, link) {
        if (addr < v->start) return NULL;
        if (addr < v->end) return v;
    }
    return NULL;
}

/* insert keeping the list sorted */
static void insert(struct process *p, struct vma *nv) {
    struct vma *v;
    list_for_each_entry(v, &p->vmas, link) if (v->start > nv->start) { list_add_tail(&nv->link, &v->link); return; }
    list_add_tail(&nv->link, &p->vmas);
}

static bool range_free(struct process *p, uint64_t start, uint64_t end) {
    struct vma *v;
    list_for_each_entry(v, &p->vmas, link) if (v->start < end && v->end > start) return false;
    return true;
}

/* highest gap of len bytes below 'top' */
static uint64_t find_gap(struct process *p, uint64_t len, uint64_t top) {
    uint64_t cand = top - len;
    for (;;) {
        if (cand < USER_MMAP_FLOOR || cand > top) return 0;
        struct vma *v, *hit = NULL;
        list_for_each_entry(v, &p->vmas, link) if (v->start < cand + len && v->end > cand) hit = v;
        if (!hit) return cand;
        if (hit->start < len) return 0;
        cand = hit->start - len;
    }
}

/* drop the pages of [start, end) (vm_lock held) */
static void unmap_pages(struct process *p, uint64_t start, uint64_t end) {
    bool any = false;
    for (uint64_t a = start; a < end; a += PAGE_SIZE) {
        uint64_t *pte = pte_of(p->pml4, a, false);
        if (!pte) { a = (a | ((1ULL << 21) - 1)); continue; }   /* no page table here: skip to the next 2 MiB */
        if (!(*pte & PTE_PRESENT)) continue;
        uint64_t e = *pte;
        *pte = 0;
        if (!(e & PTE_SPECIAL)) pmm_page_unref(e & PTE_ADDR_MASK);
        any = true;
    }
    if (any) flush(p, start, (end - start) / PAGE_SIZE);
}

/* split so that no area straddles addr (vm_lock held); false when out of memory */
static bool split_at(struct process *p, uint64_t addr) {
    struct vma *v = find(p, addr);
    if (!v || v->start == addr) return true;
    struct vma *n = kmalloc(sizeof *n);
    if (!n) return false;
    *n = *v;
    n->start = addr;
    v->end = addr;
    list_add(&n->link, &v->link);
    return true;
}

static int unmap_locked(struct process *p, uint64_t start, uint64_t end) {
    if (!split_at(p, start) || !split_at(p, end)) return -ENOMEM;
    struct vma *v, *tmp;
    list_for_each_entry_safe(v, tmp, &p->vmas, link) {
        if (v->end <= start || v->start >= end) continue;
        unmap_pages(p, v->start, v->end);
        list_del(&v->link);
        kfree(v);
    }
    return 0;
}

int64_t uvm_map(struct process *p, uint64_t addr, uint64_t len, uint32_t prot, uint32_t flags) {
    if (!len) return -EINVAL;
    len = PAGE_ALIGN_UP(len);
    if (len == 0 || len >= USER_MMAP_TOP) return -ENOMEM;
    if (addr & (PAGE_SIZE - 1)) { if (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) return -EINVAL; addr = PAGE_ALIGN_DOWN(addr); }
    struct vma *nv = kmalloc(sizeof *nv);
    if (!nv) return -ENOMEM;
    spin_lock(&p->vm_lock);
    bool fixed = flags & (MAP_FIXED | MAP_FIXED_NOREPLACE);
    if (fixed) {
        if (addr < PAGE_SIZE || addr >= USER_STACK_TOP || len > USER_STACK_TOP - addr) { spin_unlock(&p->vm_lock); kfree(nv); return -ENOMEM; }
        if (!range_free(p, addr, addr + len)) {
            if (flags & MAP_FIXED_NOREPLACE) { spin_unlock(&p->vm_lock); kfree(nv); return -EEXIST; }
            int r = unmap_locked(p, addr, addr + len);
            if (r < 0) { spin_unlock(&p->vm_lock); kfree(nv); return r; }
        }
    } else {
        /* the hint, if it is free; else the highest gap below the mmap area's top */
        if (!(addr >= USER_MMAP_FLOOR && addr + len <= USER_MMAP_TOP && addr + len > addr && range_free(p, addr, addr + len)))
            addr = find_gap(p, len, USER_MMAP_TOP);
        if (!addr) { spin_unlock(&p->vm_lock); kfree(nv); return -ENOMEM; }
    }
    nv->start = addr; nv->end = addr + len;
    nv->prot = prot & 7;
    nv->flags = (flags & MAP_SHARED ? VMA_SHARED : 0) | (flags & MAP_STACK ? VMA_STACK : 0);
    insert(p, nv);
    spin_unlock(&p->vm_lock);
    if (flags & MAP_POPULATE) uvm_populate(p, addr, addr + len);
    return (int64_t)addr;
}

int uvm_unmap(struct process *p, uint64_t addr, uint64_t len) {
    if ((addr & (PAGE_SIZE - 1)) || !len || addr >= USER_MAX) return -EINVAL;
    uint64_t end = PAGE_ALIGN_UP(addr + len);
    if (end > USER_MAX || end < addr) return -EINVAL;
    spin_lock(&p->vm_lock);
    int r = unmap_locked(p, addr, end);
    spin_unlock(&p->vm_lock);
    return r;
}

int uvm_protect(struct process *p, uint64_t addr, uint64_t len, uint32_t prot) {
    if (addr & (PAGE_SIZE - 1)) return -EINVAL;
    uint64_t end = PAGE_ALIGN_UP(addr + len);
    if (end > USER_MAX || end < addr) return -ENOMEM;
    if (end == addr) return 0;
    spin_lock(&p->vm_lock);
    /* the whole range must be mapped */
    for (uint64_t a = addr; a < end;) {
        struct vma *v = find(p, a);
        if (!v) { spin_unlock(&p->vm_lock); return -ENOMEM; }
        a = v->end;
    }
    if (!split_at(p, addr) || !split_at(p, end)) { spin_unlock(&p->vm_lock); return -ENOMEM; }
    struct vma *v;
    list_for_each_entry(v, &p->vmas, link) {
        if (v->end <= addr || v->start >= end) continue;
        v->prot = prot & 7;
        for (uint64_t a = v->start; a < v->end; a += PAGE_SIZE) {
            uint64_t *pte = pte_of(p->pml4, a, false);
            if (!pte) { a |= (1ULL << 21) - 1; continue; }
            if (!(*pte & PTE_PRESENT)) continue;
            uint64_t phys = *pte & PTE_ADDR_MASK, keep = *pte & (PTE_COW | PTE_SPECIAL);
            if (prot == PROT_NONE) { *pte = 0; if (!(keep & PTE_SPECIAL)) pmm_page_unref(phys); continue; }
            uint64_t f = pte_flags(prot);
            if (keep & PTE_COW) f &= ~PTE_WRITE;   /* still shared: the next write copies it */
            *pte = phys | f | keep;
        }
    }
    flush(p, addr, (end - addr) / PAGE_SIZE);
    spin_unlock(&p->vm_lock);
    return 0;
}

/* vm_lock held: make the page at addr present (and writable when asked) */
static int fault_locked(struct process *p, struct vma *v, uint64_t addr, bool write) {
    uint64_t *pte = pte_of(p->pml4, addr, true);
    if (!pte) return -ENOMEM;
    uint64_t page = PAGE_ALIGN_DOWN(addr);
    if (!(*pte & PTE_PRESENT)) {
        if (!enough_memory(1)) return -ENOMEM;
        uint64_t phys = pmm_alloc_pages_try(1);   /* zeroed */
        if (!phys) return -ENOMEM;
        *pte = phys | pte_flags(v->prot);
        return 0;   /* was not present: nothing cached to flush */
    }
    if (!write || (*pte & PTE_WRITE)) return 0;   /* another thread got here first */
    if (!(*pte & PTE_COW)) return -EFAULT;
    uint64_t old = *pte & PTE_ADDR_MASK;
    if (pmm_page_shared(old)) {
        if (!enough_memory(1)) return -ENOMEM;
        uint64_t phys = pmm_alloc_pages_try(1);
        if (!phys) return -ENOMEM;
        memcpy(P2V(phys), P2V(old), PAGE_SIZE);
        *pte = phys | pte_flags(v->prot);
        pmm_page_unref(old);
        flush(p, page, 1);   /* other CPUs may still read the old copy */
    } else {
        *pte = old | pte_flags(v->prot);   /* the last sharer: take the page over */
        tlb_flush_local(page, 1);
    }
    return 0;
}

int uvm_fault(struct process *p, uint64_t addr, bool write, bool exec) {
    if (addr >= USER_MAX || !p->is_user) return -EFAULT;
    spin_lock(&p->vm_lock);
    struct vma *v = find(p, addr);
    int r;
    if (!v || v->prot == PROT_NONE || (write && !(v->prot & PROT_WRITE)) || (exec && !(v->prot & PROT_EXEC))) r = -EFAULT;
    else r = fault_locked(p, v, addr, write);
    spin_unlock(&p->vm_lock);
    return r;
}

int uvm_populate(struct process *p, uint64_t start, uint64_t end) {
    start = PAGE_ALIGN_DOWN(start); end = PAGE_ALIGN_UP(end);
    if (!enough_memory((end - start) / PAGE_SIZE)) return -ENOMEM;
    spin_lock(&p->vm_lock);
    int r = 0;
    for (uint64_t a = start; a < end && r == 0; a += PAGE_SIZE) {
        struct vma *v = find(p, a);
        if (!v) { r = -EFAULT; break; }
        r = fault_locked(p, v, a, (v->prot & PROT_WRITE) != 0);
    }
    spin_unlock(&p->vm_lock);
    return r;
}

/* child gets a copy of every area; private pages become copy-on-write in both */
int uvm_fork(struct process *parent, struct process *child) {
    spin_lock(&parent->vm_lock);
    struct vma *v;
    int r = 0;
    list_for_each_entry(v, &parent->vmas, link) {
        struct vma *n = kmalloc(sizeof *n);
        if (!n) { r = -ENOMEM; break; }
        *n = *v;
        list_add_tail(&n->link, &child->vmas);
        for (uint64_t a = v->start; a < v->end; a += PAGE_SIZE) {
            uint64_t *pte = pte_of(parent->pml4, a, false);
            if (!pte) { a |= (1ULL << 21) - 1; continue; }
            if (!(*pte & PTE_PRESENT)) continue;
            uint64_t *cpte = pte_of(child->pml4, a, true);
            if (!cpte) { r = -ENOMEM; break; }
            uint64_t e = *pte;
            if (!(e & PTE_SPECIAL)) pmm_page_ref(e & PTE_ADDR_MASK);
            /* every private page, read-only ones too: a later mprotect must not make a shared page writable */
            if (!(v->flags & VMA_SHARED) && !(e & PTE_SPECIAL)) e = (e & ~PTE_WRITE) | PTE_COW;
            *pte = e; *cpte = e;
        }
        if (r) break;
    }
    /* the parent's writable entries just became read-only */
    if (parent->nthreads > 1) tlb_shootdown(0, ~0ULL); else tlb_flush_local(0, ~0ULL);
    spin_unlock(&parent->vm_lock);
    child->brk_start = parent->brk_start; child->brk = parent->brk;
    return r;
}

void uvm_clear(struct process *p) {
    spin_lock(&p->vm_lock);
    struct vma *v, *tmp;
    list_for_each_entry_safe(v, tmp, &p->vmas, link) {
        unmap_pages(p, v->start, v->end);
        list_del(&v->link);
        kfree(v);
    }
    spin_unlock(&p->vm_lock);
    p->brk_start = p->brk = 0;
}

#define USER_BRK_MAX (64ULL << 30)
int64_t uvm_brk(struct process *p, uint64_t nb) {
    if (!nb || !p->brk_start) return (int64_t)p->brk;
    if (nb < p->brk_start || nb - p->brk_start > USER_BRK_MAX) return (int64_t)p->brk;
    uint64_t old_end = PAGE_ALIGN_UP(p->brk), new_end = PAGE_ALIGN_UP(nb);
    spin_lock(&p->vm_lock);
    struct vma *heap = NULL, *v;
    list_for_each_entry(v, &p->vmas, link) if (v->flags & VMA_HEAP) heap = v;
    if (new_end > old_end) {
        if (!range_free(p, old_end, new_end)) { spin_unlock(&p->vm_lock); return (int64_t)p->brk; }
        if (heap && heap->end == old_end) heap->end = new_end;
        else {
            struct vma *n = kmalloc(sizeof *n);
            if (!n) { spin_unlock(&p->vm_lock); return (int64_t)p->brk; }
            n->start = old_end; n->end = new_end; n->prot = PROT_READ | PROT_WRITE; n->flags = VMA_HEAP;
            insert(p, n);
        }
    } else if (new_end < old_end) {
        unmap_locked(p, new_end, old_end);
    }
    p->brk = nb;
    spin_unlock(&p->vm_lock);
    return (int64_t)nb;
}

bool uvm_user_page_present(struct process *p, uint64_t addr) {
    uint64_t *pte = pte_of(p->pml4, addr, false);
    return pte && (*pte & PTE_PRESENT);
}

uint64_t uvm_resident_bytes(struct process *p) { return p->pml4 ? vmm_user_pages(p->pml4) * PAGE_SIZE : 0; }
