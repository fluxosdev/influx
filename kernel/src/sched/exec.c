/*
 * Program loading: static ELF64 executables (fixed address, or position
 * independent ones loaded at a base), into a fresh address space with the
 * System V initial stack (argc, argv, envp, auxiliary vector), as Linux lays
 * it out, so C runtimes written for Linux start unchanged.
 */
#include "sched.h"
#include "fs.h"
#include "mm.h"
#include "uvm.h"
#include "uaccess.h"
#include "string.h"
#include "simd.h"
#include "random.h"

struct elf64_ehdr { uint8_t ident[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff; uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } PACKED;
struct elf64_phdr { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } PACKED;
#define PT_LOAD 1
#define PT_INTERP 3
#define PT_PHDR 6
#define PT_GNU_STACK 0x6474e551
#define PF_X 1
#define PF_W 2
#define PF_R 4
#define ET_EXEC 2
#define ET_DYN 3
#define PIE_BASE 0x0000000100000000ULL   /* 4 GiB: below the mmap area, clear of fixed-address executables at 4 MiB */
#define USER_PROCS_MAX 64
#define STACK_RESERVE (8ULL << 20)     /* demand-paged main stack */
#define ARG_MAX_BYTES (256 * 1024)
#ifndef EAGAIN
#define EAGAIN 11
#endif
#ifndef E2BIG
#define E2BIG 7
#endif
#define ELIBBAD 80

/* writes into p's memory through its page tables (p need not be the running process); pages must be present */
static void poke(struct process *p, uint64_t dst, const void *src, size_t n) {
    const uint8_t *s = src;
    while (n) {
        uint64_t phys = vmm_get_phys(p->pml4, dst);
        size_t chunk = MIN(n, PAGE_SIZE - (dst & 0xFFF));
        memcpy(P2V(phys), s, chunk);
        dst += chunk; s += chunk; n -= chunk;
    }
}

struct loaded { uint64_t entry, phdr, phnum, base, brk; char interp[256]; };

#define INTERP_BASE 0x00007f8000000000ULL   /* the dynamic linker: above the mmap area, below the stack */
#define HDR_MAX (64 * 1024)                 /* ELF header plus program headers */
#define XFER (64 * 1024)                    /* segments are copied in from the file in pieces of this size */

/* An executable being loaded: the file stays open and its segments are read straight into the new
 * address space, so loading never needs one kernel buffer the size of the program (cc1 is ~30 MB). */
struct image { struct vnode *vn; uint64_t size; uint8_t *hdr; size_t hlen; };
static void image_close(struct image *im) { if (im->vn) vnode_unref(im->vn); kfree(im->hdr); im->vn = NULL; im->hdr = NULL; }
static int image_read(struct image *im, uint64_t off, void *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        int64_t r = vfs_pread(im->vn, off + got, (uint8_t *)buf + got, n - got);
        if (r <= 0) return r < 0 ? (int)r : -EIO;
        got += r;
    }
    return 0;
}

/* opens path and reads its first bytes; a regular file with an execute bit (need_x) */
static int image_open(const char *path, struct image *im, bool need_x) {
    memset(im, 0, sizeof *im);
    struct vnode *v = vfs_lookup(path);
    if (!v) return -ENOENT;
    if (v->type != VN_FILE) { vnode_unref(v); return -EACCES; }
    if (need_x && !(v->mode & 0111)) { vnode_unref(v); return -EACCES; }
    im->vn = v; im->size = v->size;
    im->hlen = (size_t)MIN(im->size, (uint64_t)4096);
    im->hdr = kmalloc(im->hlen + 1);
    if (!im->hdr) { image_close(im); return -ENOMEM; }
    int e = image_read(im, 0, im->hdr, im->hlen);
    if (e < 0) { image_close(im); return e; }
    im->hdr[im->hlen] = 0;
    return 0;
}
/* makes the program headers part of im->hdr (they usually are already) */
static int image_load_phdrs(struct image *im) {
    const struct elf64_ehdr *eh = (const void *)im->hdr;
    uint64_t need = eh->phoff + (uint64_t)eh->phnum * eh->phentsize;
    if (need <= im->hlen) return 0;
    if (need > HDR_MAX || need > im->size) return -ENOEXEC;
    uint8_t *h = kmalloc(need);
    if (!h) return -ENOMEM;
    int e = image_read(im, 0, h, need);
    if (e < 0) { kfree(h); return e; }
    kfree(im->hdr); im->hdr = h; im->hlen = need;
    return 0;
}

/* checks an executable (or, interp, the dynamic linker: position independent, loaded at INTERP_BASE) */
static int validate_at(struct image *im, bool interp) {
    const struct elf64_ehdr *eh = (const void *)im->hdr;
    uint64_t size = im->size;
    if (im->hlen < sizeof *eh || memcmp(eh->ident, "\x7f" "ELF", 4) || eh->ident[4] != 2 || eh->ident[5] != 1 || eh->machine != 62) return -ENOEXEC;
    if (eh->type != ET_DYN && (interp || eh->type != ET_EXEC)) return -ENOEXEC;
    if (!eh->phnum || eh->phentsize < sizeof(struct elf64_phdr) || eh->phoff > size || (uint64_t)eh->phnum * eh->phentsize > size - eh->phoff) return -ENOEXEC;
    int e = image_load_phdrs(im);
    if (e < 0) return e;
    eh = (const void *)im->hdr;
    uint64_t base = interp ? INTERP_BASE : eh->type == ET_DYN ? PIE_BASE : 0;
    uint64_t lo = interp ? INTERP_BASE : PAGE_SIZE, hi = interp ? USER_STACK_TOP - (64ULL << 20) : USER_MMAP_FLOOR;
    bool any = false;
    for (int i = 0; i < eh->phnum; i++) {
        const struct elf64_phdr *ph = (const void *)(im->hdr + eh->phoff + (uint64_t)i * eh->phentsize);
        if (ph->type == PT_INTERP) {
            if (interp || ph->filesz < 2 || ph->filesz > 255 || ph->offset > size || ph->filesz > size - ph->offset) return -ENOEXEC;
            continue;
        }
        if (ph->type != PT_LOAD || ph->memsz == 0) continue;
        uint64_t va = ph->vaddr + base;
        if (ph->filesz > ph->memsz || va < lo || va >= hi || ph->memsz > hi - va || ph->offset > size || ph->filesz > size - ph->offset) return -ENOEXEC;
        any = true;
    }
    return any ? 0 : -ENOEXEC;
}

/* copies n bytes at file offset off into p's memory at dst (populated) */
static int copy_in(struct process *p, struct image *im, uint64_t dst, uint64_t off, uint64_t n) {
    uint8_t *buf = kmalloc((size_t)MIN(n, (uint64_t)XFER));
    if (!buf && n) return -ENOMEM;
    int e = 0;
    while (n) {
        size_t c = (size_t)MIN(n, (uint64_t)XFER);
        if ((e = image_read(im, off, buf, c)) < 0) break;
        poke(p, dst, buf, c);
        dst += c; off += c; n -= c;
    }
    kfree(buf);
    return e;
}

/* maps the segments into p's address space at base (0 for a fixed-address executable) */
static int load_segments(struct process *p, struct image *im, uint64_t base, struct loaded *out) {
    const struct elf64_ehdr *eh = (const void *)im->hdr;
    uint64_t mapped_end = 0, brk = 0, first_va = ~0ULL, first_off = 0;
    out->phdr = 0; out->interp[0] = 0;
    for (int i = 0; i < eh->phnum; i++) {
        const struct elf64_phdr *ph = (const void *)(im->hdr + eh->phoff + (uint64_t)i * eh->phentsize);
        if (ph->type == PT_PHDR) out->phdr = ph->vaddr + base;
        if (ph->type == PT_INTERP) {
            int e = image_read(im, ph->offset, out->interp, (size_t)ph->filesz);
            if (e < 0) return e;
            if (out->interp[ph->filesz - 1]) return -ENOEXEC;
        }
        if (ph->type != PT_LOAD || ph->memsz == 0) continue;
        uint64_t va = ph->vaddr + base;
        uint32_t prot = (ph->flags & PF_R ? PROT_READ : 0) | (ph->flags & PF_W ? PROT_WRITE : 0) | (ph->flags & PF_X ? PROT_EXEC : 0);
        uint64_t s = PAGE_ALIGN_DOWN(va), e = PAGE_ALIGN_UP(va + ph->memsz);
        if (va < first_va) { first_va = va; first_off = ph->offset; }
        /* a page shared with the previous segment keeps both permissions */
        if (s < mapped_end) {
            if (uvm_protect(p, s, PAGE_SIZE, prot | PROT_READ | PROT_WRITE | PROT_EXEC) < 0) return -ENOMEM;
            s += PAGE_SIZE;
        }
        if (e > s) {
            int64_t r = uvm_map(p, s, e - s, prot, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS);
            if (r < 0) return (int)r;
        }
        if (uvm_populate(p, PAGE_ALIGN_DOWN(va), PAGE_ALIGN_UP(va + ph->memsz)) < 0) return -ENOMEM;
        int ce = copy_in(p, im, va, ph->offset, ph->filesz);
        if (ce < 0) return ce;
        if (e > mapped_end) mapped_end = e;
        if (va + ph->memsz > brk) brk = va + ph->memsz;
    }
    if (!out->phdr && eh->phoff >= first_off) out->phdr = first_va + (eh->phoff - first_off);
    out->phnum = eh->phnum;
    out->entry = eh->entry + base;
    out->base = 0;
    out->brk = PAGE_ALIGN_UP(brk);
    return 0;
}

/* ---- interpreter scripts ----
 * "#!interpreter [argument]" on the first line runs the interpreter with argv = { interpreter,
 * [argument,] script path, argv[1], ... }, as Linux does: the argument is the rest of the line
 * (one word, spaces and all). Interpreters may themselves be scripts, up to SCRIPT_DEPTH deep. */
#define SHEBANG_MAX 256
#define SCRIPT_DEPTH 4
static int parse_shebang(const uint8_t *buf, size_t n, char *interp, char *arg) {
    if (n < 3 || buf[0] != '#' || buf[1] != '!') return 0;
    size_t end = 2;
    while (end < n && end < SHEBANG_MAX && buf[end] != '\n') end++;
    if (end == SHEBANG_MAX && (end >= n || buf[end] != '\n')) {
        /* a long line is fine as long as the interpreter's name ended inside the buffer */
        size_t i = 2; while (i < end && (buf[i] == ' ' || buf[i] == '\t')) i++;
        while (i < end && buf[i] != ' ' && buf[i] != '\t') i++;
        if (i == end) return -ENOEXEC;
    }
    size_t i = 2;
    while (i < end && (buf[i] == ' ' || buf[i] == '\t')) i++;
    size_t is = i;
    while (i < end && buf[i] != ' ' && buf[i] != '\t' && buf[i] != '\r') i++;
    if (i == is) return -ENOEXEC;
    memcpy(interp, buf + is, i - is); interp[i - is] = 0;
    while (i < end && (buf[i] == ' ' || buf[i] == '\t')) i++;
    size_t ae = end;
    while (ae > i && (buf[ae - 1] == ' ' || buf[ae - 1] == '\t' || buf[ae - 1] == '\r')) ae--;
    memcpy(arg, buf + i, ae - i); arg[ae - i] = 0;
    return 1;
}

/* one kmalloc'd block: the pointer array, then the strings */
static char **build_argv(const char *interp, const char *arg, const char *script, int argc, char *const *argv, int *out_argc) {
    int n = 1 + (arg[0] ? 1 : 0) + 1 + (argc > 1 ? argc - 1 : 0);
    size_t bytes = (size_t)(n + 1) * sizeof(char *) + strlen(interp) + 1 + strlen(arg) + 1 + strlen(script) + 1;
    for (int i = 1; i < argc; i++) bytes += strlen(argv[i]) + 1;
    if (bytes > 2 * ARG_MAX_BYTES) return NULL;
    char **v = kmalloc(bytes);
    if (!v) return NULL;
    char *s = (char *)(v + n + 1);
    int k = 0;
    #define PUT(str) do { size_t l_ = strlen(str) + 1; memcpy(s, (str), l_); v[k++] = s; s += l_; } while (0)
    PUT(interp);
    if (arg[0]) PUT(arg);
    PUT(script);
    for (int i = 1; i < argc; i++) PUT(argv[i]);
    #undef PUT
    v[k] = NULL;
    *out_argc = k;
    return v;
}

/* Opens what execve(path, argv) really runs: path itself, or the interpreter named by its #! line
 * (recursively). On success *im is the validated ELF image; when scripts were involved argv and argc
 * point to a new vector, which *owned holds (kfree it). */
static int exec_open(const char *path, struct image *im, int *argc, char *const **argv, char **owned) {
    char cur[PATH_MAX];
    strlcpy(cur, path, sizeof cur);
    *owned = NULL;
    for (int depth = 0; ; depth++) {
        int e = image_open(cur, im, true);
        if (e < 0) goto fail;
        char interp[SHEBANG_MAX], arg[SHEBANG_MAX];
        e = parse_shebang(im->hdr, im->hlen, interp, arg);
        if (e == 0) {
            if ((e = validate_at(im, false)) < 0) { image_close(im); goto fail; }
            return 0;
        }
        image_close(im);
        if (e < 0) goto fail;
        if (depth >= SCRIPT_DEPTH) { e = -ELOOP; goto fail; }
        int nargc;
        char **nv = build_argv(interp, arg, cur, *argc, *argv, &nargc);
        if (!nv) { e = -E2BIG; goto fail; }
        kfree(*owned);
        *owned = (char *)nv; *argv = nv; *argc = nargc;
        /* the interpreter's path is taken as absolute (Linux would resolve a relative one against the cwd) */
        if (strlcpy(cur, interp, sizeof cur) >= sizeof cur) { e = -ENAMETOOLONG; goto fail; }
        continue;
    fail:
        kfree(*owned); *owned = NULL;
        return e;
    }
}

#define AT_NULL 0
#define AT_PHDR 3
#define AT_PHENT 4
#define AT_PHNUM 5
#define AT_PAGESZ 6
#define AT_BASE 7
#define AT_FLAGS 8
#define AT_ENTRY 9
#define AT_UID 11
#define AT_EUID 12
#define AT_GID 13
#define AT_EGID 14
#define AT_PLATFORM 15
#define AT_HWCAP 16
#define AT_CLKTCK 17
#define AT_SECURE 23
#define AT_RANDOM 25
#define AT_HWCAP2 26
#define AT_EXECFN 31

/* the initial stack; returns the stack pointer for _start */
static int64_t build_stack(struct process *p, const struct loaded *L, const char *path, int argc, char *const *argv, int envc, char *const *envp) {
    uint64_t top = USER_STACK_TOP;
    int64_t r = uvm_map(p, top - STACK_RESERVE, STACK_RESERVE, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK);
    if (r < 0) return r;
    size_t strs = strlen(path) + 1 + 16 + 8;   /* execfn, AT_RANDOM bytes, "x86_64" */
    for (int i = 0; i < argc; i++) strs += strlen(argv[i]) + 1;
    for (int i = 0; i < envc; i++) strs += strlen(envp[i]) + 1;
    size_t vec = (size_t)(1 + argc + 1 + envc + 1 + 2 * 20) * 8;
    if (strs + vec > ARG_MAX_BYTES) return -E2BIG;
    uint64_t need = PAGE_ALIGN_UP(strs + vec + 64);
    if (uvm_populate(p, top - need, top) < 0) return -ENOMEM;
    /* strings at the top */
    uint64_t sp = top - 16, o;
    uint64_t *av = kmalloc((argc + envc + 2) * 8);
    if (!av) return -ENOMEM;
    o = sp - strlen(path) - 1; poke(p, o, path, strlen(path) + 1); uint64_t execfn = o;
    o -= 7; poke(p, o, "x86_64", 7); uint64_t platform = o;
    uint8_t rnd[16]; random_bytes(rnd, sizeof rnd);
    o = (o - 16) & ~15ULL; poke(p, o, rnd, 16); uint64_t random_at = o;
    for (int i = envc - 1; i >= 0; i--) { size_t l = strlen(envp[i]) + 1; o -= l; poke(p, o, envp[i], l); av[argc + 1 + i] = o; }
    for (int i = argc - 1; i >= 0; i--) { size_t l = strlen(argv[i]) + 1; o -= l; poke(p, o, argv[i], l); av[i] = o; }
    av[argc] = 0;
    /* then argc, argv[], 0, envp[], 0, auxv, 16-byte aligned at argc */
    uint64_t aux[] = {
        AT_PHDR, L->phdr, AT_PHENT, sizeof(struct elf64_phdr), AT_PHNUM, L->phnum, AT_PAGESZ, PAGE_SIZE,
        AT_BASE, L->base, AT_FLAGS, 0, AT_ENTRY, L->entry, AT_UID, (uint64_t)p->uid, AT_EUID, (uint64_t)p->euid,
        AT_GID, (uint64_t)p->gid, AT_EGID, (uint64_t)p->egid, AT_HWCAP, 0x178bfbff, AT_CLKTCK, 100, AT_SECURE, 0,
        AT_RANDOM, random_at, AT_PLATFORM, platform, AT_EXECFN, execfn, AT_NULL, 0,
    };
    size_t words = 1 + argc + 1 + envc + 1 + ARRAY_LEN(aux);
    sp = (o - words * 8) & ~15ULL;
    uint64_t w = sp;
    uint64_t argc64 = (uint64_t)argc;
    poke(p, w, &argc64, 8); w += 8;
    poke(p, w, av, (size_t)(argc + 1) * 8); w += (uint64_t)(argc + 1) * 8;
    for (int i = 0; i < envc; i++) { poke(p, w, &av[argc + 1 + i], 8); w += 8; }
    uint64_t zero = 0; poke(p, w, &zero, 8); w += 8;
    poke(p, w, aux, sizeof aux);
    kfree(av);
    return (int64_t)sp;
}

/* loads the image into p (whose address space is empty) and fills *r with the entry context;
 * path is what execve was asked to run (AT_EXECFN and the process name) */
static int exec_into(struct process *p, const char *path, struct image *im, int argc, char *const *argv, int envc, char *const *envp, struct regs *r) {
    struct loaded L = { 0 };
    const struct elf64_ehdr *eh = (const void *)im->hdr;
    int e = load_segments(p, im, eh->type == ET_DYN ? PIE_BASE : 0, &L);
    if (e < 0) return e;
    p->brk_start = p->brk = L.brk + PAGE_SIZE;   /* a guard page after the image */
    uint64_t start = L.entry;
    if (L.interp[0]) {
        /* dynamically linked: the interpreter (musl's ld.so) runs first and loads the rest; it learns
         * where it sits from AT_BASE and finds the program through AT_PHDR/AT_ENTRY */
        struct image ii;
        if ((e = image_open(L.interp, &ii, false)) < 0) return e == -EACCES ? -ELIBBAD : e;
        struct loaded I = { 0 };
        if ((e = validate_at(&ii, true)) == 0) e = load_segments(p, &ii, INTERP_BASE, &I);
        image_close(&ii);
        if (e < 0) return e;
        L.base = INTERP_BASE;
        start = I.entry;
    }
    int64_t sp = build_stack(p, &L, path, argc, argv, envc, envp);
    if (sp < 0) return (int)sp;
    memset(r, 0, sizeof *r);
    r->rip = start; r->rsp = (uint64_t)sp; r->rflags = 0x202;
    r->cs = USER_CS; r->ss = USER_DS; r->int_no = (uint64_t)-1;
    strlcpy(p->name, vfs_basename(path), sizeof p->name);
    return 0;
}

static char *const default_env[] = { "PATH=/usr/bin:/bin", "HOME=/home/flux", "TERM=xterm-256color", "USER=flux", "SHELL=/bin/sh", NULL };

static int spawn(const char *path, int argc, char **argv, char *const *envp, struct file *stdio[3], const char *cwd, int app_id) {
    struct image im; char *owned; char *const *av = argv;
    int r = exec_open(path, &im, &argc, &av, &owned);
    if (r < 0) return r;
    if (process_user_count() >= USER_PROCS_MAX) { image_close(&im); kfree(owned); return -EAGAIN; }
    struct process *p = process_create(vfs_basename(path), true);
    p->app_id = app_id;
    strlcpy(p->cwd, cwd ? cwd : "/", sizeof p->cwd);
    if (!envp) envp = default_env;
    int envc = 0; while (envp[envc]) envc++;
    struct regs regs;
    r = exec_into(p, path, &im, argc, av, envc, envp, &regs);
    image_close(&im); kfree(owned);
    if (r < 0) {
        /* no thread ever ran: mark it exited (waking anyone already waiting) and reap it here */
        process_mark_exited(p);
        process_wait(p->pid, NULL);
        return r;
    }
    for (int i = 0; i < 3; i++) p->fds[i] = stdio && stdio[i] ? vfs_dup(stdio[i]) : NULL;
    p->execed = true;
    p->pgid = p->pid;   /* a job of its own: the terminal's Ctrl+C reaches it and what it starts */
    thread_create_user_regs(p, p->name, &regs, 0, 0, NULL);
    return p->pid;
}

int process_spawn_env(const char *path, int argc, char **argv, char *const *envp, struct file *stdio[3], const char *cwd) {
    return spawn(path, argc, argv, envp, stdio, cwd, -1);
}
int process_spawn(const char *path, int argc, char **argv, struct file *stdio[3], const char *cwd) {
    return spawn(path, argc, argv, NULL, stdio, cwd, -1);
}
/* a desktop app that is a user program: output to the console (serial), its windows tagged with app_id */
int process_spawn_app(const char *path, int argc, char **argv, const char *cwd, int app_id) {
    struct file *con = NULL;
    vfs_open("/dev/console", O_RDWR, &con);
    struct file *stdio[3] = { con, con, con };
    int r = spawn(path, argc, argv, NULL, stdio, cwd, app_id);
    if (con) vfs_close(con);
    return r;
}

/* execve in the calling process: argv/envp/path are kernel copies. On success r holds the new
 * image's entry context; on a failure after the old image is gone, the process is killed. */
int process_execve(const char *path, int argc, char *const *argv, int envc, char *const *envp, struct regs *r) {
    struct thread *t = current_thread;
    struct process *p = t->proc;
    if (p->nthreads > 1) return -EBUSY;   /* TODO: de-thread like Linux; single-threaded callers only */
    struct image im; char *owned;
    int e = exec_open(path, &im, &argc, &argv, &owned);
    if (e < 0) return e;
    /* point of no return: the old image goes */
    uvm_clear(p);
    struct regs nr;
    e = exec_into(p, path, &im, argc, argv, envc, envp, &nr);
    image_close(&im); kfree(owned);
    if (e < 0) {
        kprintf("[proc %d] execve %s: loading failed (%d) after the old image was released\n", p->pid, path, e);
        p->exit_signal = SIGKILL;
        process_exit(128 + SIGKILL);
    }
    *r = nr;
    /* close-on-exec descriptors, caught signals back to default, a clean thread state */
    for (int i = 0; i < MAX_FDS; i++) if ((p->fd_cloexec & (1ULL << i)) && p->fds[i]) { vfs_close(p->fds[i]); p->fds[i] = NULL; }
    p->fd_cloexec = 0;
    for (int s = 1; s <= NSIG; s++) { if (p->sigact[s].handler != SIG_IGN) p->sigact[s].handler = SIG_DFL; p->sigact[s].flags = 0; p->sigact[s].mask = 0; p->sigact[s].restorer = 0; }
    t->alt_sp = t->alt_size = 0;
    t->fs_base = 0;
    wrmsr(0xC0000100, 0);
    t->clear_child_tid = 0;
    void *m; uint8_t *fresh = fpu_state_alloc(&m);
    memcpy(t->fpu, fresh, fpu_state_size);
    kfree(m);
    uint64_t fl = irq_save(); fpu_restore(t->fpu); irq_restore(fl);
    strlcpy(t->name, p->name, sizeof t->name);
    p->execed = true;
    t->iret_return = true;
    return 0;
}
