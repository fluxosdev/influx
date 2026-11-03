/* ELF loader, user process creation and the system call interface. */
#include "sched.h"
#include "fs.h"
#include "mm.h"
#include "string.h"
#include "io.h"
#include "tty.h"
#include "blockdev.h"
#include "ui.h"

struct elf64_ehdr { uint8_t ident[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff; uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } PACKED;
struct elf64_phdr { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } PACKED;
#define PT_LOAD 1
#define USER_MAX 0x0000800000000000ULL
#define USER_LIMIT (USER_STACK_TOP - USER_STACK_SIZE - PAGE_SIZE)   /* image and heap stay below the stack and its guard page */
#define USER_BRK_MAX (1ULL << 32)                                    /* per-process sbrk heap */
#define PMM_RESERVE_PAGES 4096                                       /* left for the kernel when mapping user memory */
#define USER_PROCS_MAX 64                                            /* live (unreaped) user processes */
#define USER_WIN_BYTES_MAX (64ULL << 20)                             /* window pixels per user process */
#ifndef EAGAIN
#define EAGAIN 11
#endif

struct syscall_frame { uint64_t rax, r9, r8, r10, rdx, rsi, rdi, r15, r14, r13, r12, rbp, rbx, rflags, rip, rsp; };

/* ---- user memory helpers ---- */
static bool user_range_ok(struct process *p, uint64_t addr, uint64_t len) {
    if (!len) return true;
    if (addr >= USER_MAX || addr + len > USER_MAX || addr + len < addr) return false;
    for (uint64_t a = PAGE_ALIGN_DOWN(addr); a < addr + len; a += PAGE_SIZE) if (!vmm_get_phys(p->pml4, a)) return false;
    return true;
}
static int copy_user_string(struct process *p, uint64_t addr, char *out, size_t max) {
    for (size_t i = 0; i < max; i++) {
        if (!user_range_ok(p, addr + i, 1)) return -EINVAL;
        out[i] = *(char *)(addr + i);
        if (!out[i]) return 0;
    }
    return -EINVAL;
}

/* map a zeroed page range into the process */
static int map_user_range(struct process *p, uint64_t start, uint64_t end) {
    uint64_t need = (PAGE_ALIGN_UP(end) - PAGE_ALIGN_DOWN(start)) / PAGE_SIZE;
    if (need + need / 512 + PMM_RESERVE_PAGES > pmm_free_bytes() / PAGE_SIZE) return -ENOMEM;
    for (uint64_t a = PAGE_ALIGN_DOWN(start); a < PAGE_ALIGN_UP(end); a += PAGE_SIZE) {
        if (vmm_get_phys(p->pml4, a)) continue;
        uint64_t phys = pmm_alloc_page();
        vmm_map(p->pml4, a, phys, PTE_WRITE | PTE_USER);
    }
    return 0;
}
static void copy_to_user_space(struct process *p, uint64_t dst, const void *src, size_t n) {
    const uint8_t *s = src;
    while (n) {
        uint64_t phys = vmm_get_phys(p->pml4, dst);
        size_t chunk = MIN(n, PAGE_SIZE - (dst & 0xFFF));
        memcpy(P2V(phys), s, chunk);
        dst += chunk; s += chunk; n -= chunk;
    }
}

int process_spawn(const char *path, int argc, char **argv, struct file *stdio[3], const char *cwd) {
    void *data; size_t size;
    int r = vfs_read_file(path, &data, &size);
    if (r < 0) return r;
    struct elf64_ehdr *eh = data;
    if (size < sizeof *eh || memcmp(eh->ident, "\x7f" "ELF", 4) || eh->ident[4] != 2 || eh->machine != 62 || eh->type != 2 ||
        eh->entry < PAGE_SIZE || eh->entry >= USER_LIMIT || (eh->phnum && (eh->phentsize < sizeof(struct elf64_phdr) ||
        eh->phoff > size || (uint64_t)eh->phnum * eh->phentsize > size - eh->phoff))) { kfree(data); return -ENOEXEC; }
    for (int i = 0; i < eh->phnum; i++) {
        struct elf64_phdr *ph = (struct elf64_phdr *)((uint8_t *)data + eh->phoff + (uint64_t)i * eh->phentsize);
        if (ph->type != PT_LOAD || ph->memsz == 0) continue;
        if (ph->filesz > ph->memsz || ph->vaddr < PAGE_SIZE || ph->vaddr >= USER_LIMIT || ph->memsz > USER_LIMIT - ph->vaddr ||
            ph->offset > size || ph->filesz > size - ph->offset) { kfree(data); return -ENOEXEC; }
    }
    if (process_user_count() >= USER_PROCS_MAX) { kfree(data); return -EAGAIN; }
    struct process *p = process_create(vfs_basename(path), true);
    strlcpy(p->cwd, cwd ? cwd : "/", sizeof p->cwd);
    uint64_t brk = 0;
    for (int i = 0; i < eh->phnum; i++) {
        struct elf64_phdr *ph = (struct elf64_phdr *)((uint8_t *)data + eh->phoff + (uint64_t)i * eh->phentsize);
        if (ph->type != PT_LOAD || ph->memsz == 0) continue;
        if (map_user_range(p, ph->vaddr, ph->vaddr + ph->memsz) < 0) goto nomem;
        copy_to_user_space(p, ph->vaddr, (uint8_t *)data + ph->offset, ph->filesz);
        if (ph->vaddr + ph->memsz > brk) brk = ph->vaddr + ph->memsz;
    }
    p->brk_start = p->brk = PAGE_ALIGN_UP(brk) + PAGE_SIZE;
    /* stack with argv */
    uint64_t stack_bottom = USER_STACK_TOP - USER_STACK_SIZE;
    if (map_user_range(p, stack_bottom, USER_STACK_TOP) < 0) goto nomem;
    size_t strs = 0; for (int i = 0; i < argc; i++) strs += strlen(argv[i]) + 1;
    uint64_t sp = USER_STACK_TOP - 16;
    sp -= ALIGN_UP(strs, 16);
    uint64_t str_base = sp;
    uint64_t *ptrs = kmalloc((argc + 2) * 8);
    uint64_t off = 0;
    for (int i = 0; i < argc; i++) { size_t l = strlen(argv[i]) + 1; copy_to_user_space(p, str_base + off, argv[i], l); ptrs[i] = str_base + off; off += l; }
    ptrs[argc] = 0;
    sp -= (argc + 2) * 8; sp &= ~0xFULL;
    uint64_t argc64 = argc;
    copy_to_user_space(p, sp, &argc64, 8);
    copy_to_user_space(p, sp + 8, ptrs, (argc + 1) * 8);
    kfree(ptrs);
    for (int i = 0; i < 3; i++) p->fds[i] = stdio && stdio[i] ? vfs_dup(stdio[i]) : NULL;
    uint64_t entry = eh->entry;
    kfree(data);
    thread_create_user(p, p->name, entry, sp);
    return p->pid;
nomem:
    /* no thread ever ran: mark it exited (waking anyone already waiting) and reap it here */
    kfree(data);
    process_mark_exited(p);
    process_wait(p->pid, NULL);
    return -ENOMEM;
}

/* ---- syscalls ---- */
enum {
    SYS_EXIT = 0, SYS_WRITE, SYS_READ, SYS_OPEN, SYS_CLOSE, SYS_LSEEK, SYS_GETPID, SYS_SBRK, SYS_SLEEP, SYS_YIELD,
    SYS_GETCWD, SYS_CHDIR, SYS_READDIR, SYS_STAT, SYS_MKDIR, SYS_UNLINK, SYS_SPAWN, SYS_WAITPID, SYS_UNAME, SYS_TIME,
    SYS_UPTIME, SYS_IOCTL, SYS_DUP2, SYS_PIPE, SYS_KILL, SYS_SYSINFO, SYS_PS, SYS_FSTAT, SYS_RENAME, SYS_SYNC, SYS_LSBLK,
    SYS_WIN_CREATE, SYS_WIN_PRESENT, SYS_WIN_EVENT, SYS_WIN_DESTROY, SYS_WIN_TITLE,
};
struct user_event { int32_t type, x, y, button, buttons, scroll, key; uint32_t ch; int32_t mods, cmd, w, h; };
struct user_dirent { char name[256]; uint32_t type; uint64_t size, mtime; };
struct user_stat { uint32_t type, mode; uint64_t size, mtime, ino; };
struct user_utsname { char sysname[32], nodename[64], release[32], version[64], machine[32]; };
struct user_sysinfo { uint64_t uptime_ms, total_mem, free_mem, used_mem; uint32_t procs, threads; };
struct user_procinfo { int32_t pid; char name[32]; int32_t cpu; uint64_t mem; int32_t threads; int32_t user; };
struct user_blkinfo { char name[16], model[48], fs[16]; uint64_t bytes; int32_t partition; };

void process_close_windows(struct process *p) { for (int i = 0; i < 8; i++) if (p->uwin[i]) { window_destroy(p->uwin[i]); p->uwin[i] = NULL; } }
static struct window *uwin_get(struct process *p, uint64_t id) { return id < 8 ? p->uwin[id] : NULL; }
static int fd_alloc(struct process *p, struct file *f) { for (int i = 0; i < MAX_FDS; i++) if (!p->fds[i]) { p->fds[i] = f; return i; } vfs_close(f); return -ENOMEM; }
static struct file *fd_get(struct process *p, uint64_t fd) { return fd < MAX_FDS ? p->fds[fd] : NULL; }

static int64_t do_syscall(struct syscall_frame *f) {
    struct process *p = current_thread->proc;
    uint64_t a1 = f->rdi, a2 = f->rsi, a3 = f->rdx;
    switch (f->rax) {
    case SYS_EXIT: process_exit((int)a1);
    case SYS_WRITE: { struct file *fl = fd_get(p, a1); if (!fl) return -EBADF; if (!user_range_ok(p, a2, a3)) return -EINVAL; return vfs_write(fl, (void *)a2, a3); }
    case SYS_READ: { struct file *fl = fd_get(p, a1); if (!fl) return -EBADF; if (!user_range_ok(p, a2, a3)) return -EINVAL; return vfs_read(fl, (void *)a2, a3); }
    case SYS_OPEN: { char path[PATH_MAX]; if (copy_user_string(p, a1, path, sizeof path) < 0) return -EINVAL; struct file *fl; int r = vfs_open_rel(p->cwd, path, (int)a2, &fl); if (r < 0) return r; return fd_alloc(p, fl); }
    case SYS_CLOSE: { struct file *fl = fd_get(p, a1); if (!fl) return -EBADF; p->fds[a1] = NULL; return vfs_close(fl); }   /* reports a failed flush (e.g. btrfs out of space) */
    case SYS_LSEEK: { struct file *fl = fd_get(p, a1); if (!fl) return -EBADF; return vfs_seek(fl, (int64_t)a2, (int)a3); }
    case SYS_GETPID: return p->pid;
    case SYS_SBRK: { int64_t incr = (int64_t)a1; uint64_t old = p->brk;
        if (incr > 0) { if ((uint64_t)incr > USER_BRK_MAX - (p->brk - p->brk_start) || p->brk + incr > USER_LIMIT || map_user_range(p, p->brk, p->brk + incr) < 0) return -ENOMEM; }
        else if (incr < 0 && 0 - (uint64_t)incr > p->brk - p->brk_start) return -EINVAL;
        p->brk += incr; return old; }
    case SYS_SLEEP: thread_sleep_ms(a1); return 0;
    case SYS_YIELD: thread_yield(); return 0;
    case SYS_GETCWD: { size_t l = strlen(p->cwd) + 1; if (a2 < l || !user_range_ok(p, a1, l)) return -EINVAL; memcpy((void *)a1, p->cwd, l); return 0; }
    case SYS_CHDIR: { char path[PATH_MAX], norm[PATH_MAX]; if (copy_user_string(p, a1, path, sizeof path) < 0) return -EINVAL; int nr = vfs_normalize(p->cwd, path, norm, sizeof norm); if (nr < 0) return nr; struct stat st; if (vfs_stat(norm, &st) < 0) return -ENOENT; if (st.type != VN_DIR) return -ENOTDIR; strlcpy(p->cwd, norm, sizeof p->cwd); return 0; }
    case SYS_READDIR: { struct file *fl = fd_get(p, a1); if (!fl) return -EBADF; if (!user_range_ok(p, a3, sizeof(struct user_dirent))) return -EINVAL; struct dirent d; int r = vfs_readdir(fl->vn, (int)a2, &d); if (r < 0) return r == -1 ? 0 : r; struct user_dirent *ud = (void *)a3; strlcpy(ud->name, d.name, sizeof ud->name); ud->type = d.type; ud->size = d.size; ud->mtime = d.mtime; return 1; }
    case SYS_STAT: { char path[PATH_MAX]; if (copy_user_string(p, a1, path, sizeof path) < 0) return -EINVAL; if (!user_range_ok(p, a2, sizeof(struct user_stat))) return -EINVAL; struct stat st; int r = vfs_stat_rel(p->cwd, path, &st); if (r < 0) return r; struct user_stat *us = (void *)a2; us->type = st.type; us->mode = st.mode; us->size = st.size; us->mtime = st.mtime; us->ino = st.ino; return 0; }
    case SYS_FSTAT: { struct file *fl = fd_get(p, a1); if (!fl) return -EBADF; if (!user_range_ok(p, a2, sizeof(struct user_stat))) return -EINVAL; struct user_stat *us = (void *)a2; us->type = fl->vn->type; us->mode = fl->vn->mode; us->size = fl->vn->size; us->mtime = fl->vn->mtime; us->ino = fl->vn->ino; return 0; }
    case SYS_MKDIR: { char path[PATH_MAX], norm[PATH_MAX]; if (copy_user_string(p, a1, path, sizeof path) < 0) return -EINVAL; int nr = vfs_normalize(p->cwd, path, norm, sizeof norm); if (nr < 0) return nr; return vfs_mkdir(norm); }
    case SYS_UNLINK: { char path[PATH_MAX], norm[PATH_MAX]; if (copy_user_string(p, a1, path, sizeof path) < 0) return -EINVAL; int nr = vfs_normalize(p->cwd, path, norm, sizeof norm); if (nr < 0) return nr; return vfs_unlink(norm); }
    case SYS_RENAME: { char a[PATH_MAX], b[PATH_MAX], na[PATH_MAX], nb[PATH_MAX]; if (copy_user_string(p, a1, a, sizeof a) < 0 || copy_user_string(p, a2, b, sizeof b) < 0) return -EINVAL; int nr = vfs_normalize(p->cwd, a, na, sizeof na); if (nr == 0) nr = vfs_normalize(p->cwd, b, nb, sizeof nb); if (nr < 0) return nr; return vfs_rename(na, nb); }
    case SYS_SPAWN: {
        char path[PATH_MAX]; if (copy_user_string(p, a1, path, sizeof path) < 0) return -EINVAL;
        char *argv[64]; int argc = 0;
        if (a2) { for (; argc < 63; argc++) { if (!user_range_ok(p, a2 + argc * 8, 8)) break; uint64_t ptr = ((uint64_t *)a2)[argc]; if (!ptr) break; char buf[1024]; if (copy_user_string(p, ptr, buf, sizeof buf) < 0) break; argv[argc] = strdup(buf); } }
        if (argc == 0) argv[argc++] = strdup(path);
        char full[PATH_MAX]; int nr = 0; if (strchr(path, '/')) nr = vfs_normalize(p->cwd, path, full, sizeof full); else snprintf(full, sizeof full, "/bin/%s", path);
        if (nr < 0) { for (int i = 0; i < argc; i++) kfree(argv[i]); return nr; }
        int r = process_spawn(full, argc, argv, p->fds, p->cwd);
        for (int i = 0; i < argc; i++) kfree(argv[i]);
        return r; }
    case SYS_WAITPID: { int status = 0; int r = process_wait((int)a1, &status); if (r < 0) return -ECHILD; if (a2 && user_range_ok(p, a2, 4)) *(int *)a2 = status; return r; }
    case SYS_UNAME: { if (!user_range_ok(p, a1, sizeof(struct user_utsname))) return -EINVAL; struct user_utsname *u = (void *)a1; strcpy(u->sysname, "inFlux"); strlcpy(u->nodename, system_hostname, sizeof u->nodename); strcpy(u->release, INFLUX_VERSION); strcpy(u->version, "#1 preemptive multithreaded"); strcpy(u->machine, "x86_64"); return 0; }
    case SYS_TIME: return time_unix();
    case SYS_UPTIME: return system_ticks;
    case SYS_IOCTL: { struct file *fl = fd_get(p, a1); if (!fl) return -EBADF; if (!fl->vn->ops->ioctl) return -ENOTTY_; if (a2 == 2 && !user_range_ok(p, a3, 8)) return -EINVAL;
        /* tty foreground pid: Ctrl-C and closing the terminal kill it, so only another user process (or none) */
        if (a2 == 3 && (int)a3) { procs_lock(); struct process *tp = process_find((int)a3); bool ok = tp && tp->is_user && !tp->exited; procs_unlock(); if (!ok) return -EACCES; }
        return fl->vn->ops->ioctl(fl->vn, (int)a2, (void *)a3); }
    case SYS_DUP2: { struct file *fl = fd_get(p, a1); if (!fl || a2 >= MAX_FDS) return -EBADF; if (a1 == a2) return a2; struct file *nf = vfs_dup(fl); if (p->fds[a2]) vfs_close(p->fds[a2]); p->fds[a2] = nf; return a2; }
    case SYS_PIPE: { if (!user_range_ok(p, a1, 8)) return -EINVAL; struct file *r, *w; pipe_create(&r, &w); int fr = fd_alloc(p, r); if (fr < 0) { vfs_close(w); return -ENOMEM; } int fw = fd_alloc(p, w); if (fw < 0) { p->fds[fr] = NULL; vfs_close(r); return -ENOMEM; } ((int *)a1)[0] = fr; ((int *)a1)[1] = fw; return 0; }
    case SYS_KILL: { procs_lock(); struct process *tp = process_find((int)a1); int r = !tp ? -ESRCH : !tp->is_user ? -EACCES : 0; procs_unlock(); if (r) return r; return process_kill((int)a1, (int)a2) ? 0 : -ESRCH; }
    case SYS_SYSINFO: { if (!user_range_ok(p, a1, sizeof(struct user_sysinfo))) return -EINVAL; struct user_sysinfo *si = (void *)a1; si->uptime_ms = system_ticks; si->total_mem = pmm_total_bytes(); si->free_mem = pmm_free_bytes(); si->used_mem = pmm_used_bytes(); struct process *l[64]; si->procs = process_list(l, 64); struct thread *t[128]; si->threads = thread_list(t, 128); return 0; }
    case SYS_PS: { int max = (int)a2; if (max > 64) max = 64; if (!user_range_ok(p, a1, max * sizeof(struct user_procinfo))) return -EINVAL; sched_sample_cpu(); procs_lock(); struct process *l[64]; int n = process_list(l, 64); struct user_procinfo *out = (void *)a1; int k = 0; for (int i = 0; i < n && k < max; i++) { out[k].pid = l[i]->pid; strlcpy(out[k].name, l[i]->name, 32); out[k].user = l[i]->is_user; out[k].mem = l[i]->heap_bytes + (l[i]->pml4 ? vmm_user_pages(l[i]->pml4) * PAGE_SIZE : 0); int cpu = 0, nt = 0; struct thread *t; list_for_each_entry(t, &l[i]->threads, proc_link) { cpu += t->cpu_percent; nt++; } out[k].cpu = l[i]->pid == 1 ? 0 : cpu; out[k].threads = nt; k++; } procs_unlock(); return k; }
    case SYS_SYNC: vfs_sync(); return 0;
    case SYS_WIN_CREATE: {
        char title[128]; if (copy_user_string(p, a3, title, sizeof title) < 0) strcpy(title, p->name);
        int w = (int)a1, h = (int)a2; if (w < 64 || h < 48 || w > 4096 || h > 4096) return -EINVAL;
        int slot = -1; uint64_t used = 0, need = (uint64_t)w * h * 4;
        for (int i = 0; i < 8; i++) if (!p->uwin[i]) { if (slot < 0) slot = i; } else used += (uint64_t)MAX(p->uwin[i]->frame.w, 0) * MAX(p->uwin[i]->frame.h, 0) * 4;
        /* the canvas is one contiguous allocation: keep it well clear of exhausting memory */
        if (slot < 0 || used + need > USER_WIN_BYTES_MAX || need > pmm_free_bytes() / 4) return -ENOMEM;
        struct window *win = window_create_try(title, w, h, WIN_STANDARD);
        if (!win) return -ENOMEM;   /* no contiguous memory for the canvas */
        win->flags |= WIN_NO_CTRL_ALIAS;
        p->uwin[slot] = win;
        window_show(win);
        return slot; }
    case SYS_WIN_PRESENT: {
        struct window *win = uwin_get(p, a1); if (!win) return -EBADF;
        /* a3/r10 = the buffer's width/height (0: legacy, buffer matches the window) */
        uint64_t sw = a3, sh = f->r10;
        if ((sw || sh) && (!sw || !sh || sw > 4096 || sh > 4096)) return -EINVAL;
        struct canvas *cv = window_begin_draw(win);
        if (!sw) { sw = cv->w; sh = cv->h; }
        if (!user_range_ok(p, a2, sw * sh * 4)) { window_end_draw(win, NULL); return -EINVAL; }
        int cw = MIN((int)sw, cv->w), ch = MIN((int)sh, cv->h);
        for (int y = 0; y < ch; y++) memcpy(cv->px + (size_t)y * cv->stride, (uint32_t *)a2 + y * sw, (size_t)cw * 4);
        window_end_draw(win, NULL);
        return 0; }
    case SYS_WIN_EVENT: {
        struct window *win = uwin_get(p, a1); if (!win) return -EBADF;
        if (!user_range_ok(p, a2, sizeof(struct user_event))) return -EINVAL;
        struct event ev; if (!window_wait_event(win, &ev, (int)(int64_t)a3)) return 0;
        struct user_event *ue = (void *)a2;
        ue->type = ev.type; ue->x = ev.x; ue->y = ev.y; ue->button = ev.button; ue->buttons = ev.buttons; ue->scroll = ev.scroll; ue->key = ev.key; ue->ch = ev.ch; ue->mods = ev.mods; ue->cmd = ev.cmd; ue->w = ev.w; ue->h = ev.h;
        if (ev.type == EV_OPEN) kfree(ev.path);
        return 1; }
    case SYS_WIN_DESTROY: { struct window *win = uwin_get(p, a1); if (!win) return -EBADF; p->uwin[a1] = NULL; window_destroy(win); return 0; }
    case SYS_WIN_TITLE: { struct window *win = uwin_get(p, a1); if (!win) return -EBADF; char t[128]; if (copy_user_string(p, a2, t, sizeof t) < 0) return -EINVAL; window_set_title(win, t); return 0; }
    case SYS_LSBLK: { int max = (int)a2; if (max > 32) max = 32; if (!user_range_ok(p, a1, max * sizeof(struct user_blkinfo))) return -EINVAL; struct user_blkinfo *out = (void *)a1; int n = 0; for (int i = 0; i < blockdev_count() && n < max; i++) { struct blockdev *b = blockdev_at(i); if (!b) continue; strlcpy(out[n].name, b->name, 16); strlcpy(out[n].model, b->parent ? b->parent->model : b->model, 48); strlcpy(out[n].fs, b->fs_type ? b->fs_type : "", 16); out[n].bytes = b->sectors * b->sector_size; out[n].partition = b->parent != NULL; n++; } return n; }
    }
    return -ENOSYS;
}

void syscall_dispatch(struct syscall_frame *f) {
    f->rax = (uint64_t)do_syscall(f);
    /* sysret to a non-canonical rip would fault in ring 0 on the user stack */
    if (f->rip >= USER_MAX) current_thread->kill_pending = true;
    if (current_thread->kill_pending) process_exit(current_thread->proc->exit_code);
}

extern void syscall_entry(void);
/* every CPU, at bring-up: no user code runs before syscall_init() anyway */
void syscall_init_cpu(void) {
    wrmsr(0xC0000081, ((uint64_t)0x13 << 48) | ((uint64_t)KERNEL_CS << 32));   /* STAR */
    wrmsr(0xC0000082, (uint64_t)syscall_entry);                                /* LSTAR */
    wrmsr(0xC0000084, 0x47700);                                                /* SFMASK: clear IF, TF, DF, IOPL, NT, AC */
}

void syscall_init(void) { kprintf("syscall: fast system calls enabled on %d CPUs\n", cpu_count); }
