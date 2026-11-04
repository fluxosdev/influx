#include "tty.h"
#include "mm.h"
#include "string.h"
#include "sched.h"
#include "signal.h"
#include "uaccess.h"

static int64_t tty_vn_read(struct vnode *v, uint64_t off, void *buf, size_t n) { return tty_read(v->priv, buf, n); }
static int64_t tty_vn_write(struct vnode *v, uint64_t off, const void *buf, size_t n) { tty_write(v->priv, buf, n); return n; }
/* Linux termios, as far as a terminal emulator with a line editor needs: ICANON selects cooked
 * (line editing, echo) or raw input; the other flags are kept and reported back unchanged. */
struct ktermios { uint32_t c_iflag, c_oflag, c_cflag, c_lflag; uint8_t c_line, c_cc[19]; };
struct kwinsize { uint16_t rows, cols, xpixel, ypixel; };
#define ICANON 0x0002
#define TCGETS 0x5401
#define TCSETS 0x5402
#define TCSETSW 0x5403
#define TCSETSF 0x5404
#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define FIONREAD 0x541B
#define FIONBIO 0x5421
static struct ktermios default_termios(bool raw) {
    struct ktermios k = { 0 };
    k.c_iflag = 0x100 | 0x400;            /* ICRNL IXON */
    k.c_oflag = 0x1 | 0x4;                /* OPOST ONLCR */
    k.c_cflag = 0xBF;                     /* CS8 CREAD B38400 */
    k.c_lflag = raw ? 0 : (0x1 | ICANON | 0x8 | 0x10 | 0x20 | 0x8000);   /* ISIG ICANON ECHO ECHOE ECHOK IEXTEN */
    static const uint8_t cc[19] = { 3, 28, 127, 21, 4, 0, 1, 0, 17, 19, 26, 0, 18, 15, 23, 22 };
    memcpy(k.c_cc, cc, sizeof cc);
    return k;
}
/* arg is a kernel pointer for the old numeric commands and a user pointer for the Linux ones (from the syscall layer) */
static int tty_vn_ioctl(struct vnode *v, int cmd, void *arg) {
    struct tty *t = v->priv;
    switch (cmd) {
    case 1: tty_set_raw(t, arg != NULL); return 0;   /* inFlux TIOCSRAW */
    case 2: { int *wh = arg; wh[0] = t->cols; wh[1] = t->rows; return 0; }
    case 3: t->fg_pid = (int)(intptr_t)arg; return 0;
    case TCGETS: { struct ktermios k = t->have_termios ? *(struct ktermios *)t->termios : default_termios(t->raw); return copy_to_user((uint64_t)arg, &k, sizeof k) < 0 ? -EFAULT : 0; }
    case TCSETS: case TCSETSW: case TCSETSF: {
        struct ktermios k;
        if (copy_from_user(&k, (uint64_t)arg, sizeof k) < 0) return -EFAULT;
        memcpy(t->termios, &k, sizeof k); t->have_termios = true;
        bool raw = !(k.c_lflag & ICANON);
        if (raw != t->raw) tty_set_raw(t, raw);
        if (cmd == TCSETSF) { spin_lock(&t->lock); t->in_head = t->in_tail = 0; t->cooked_len = t->cooked_pos = 0; spin_unlock(&t->lock); }
        return 0; }
    case TIOCGWINSZ: { struct kwinsize w = { (uint16_t)t->rows, (uint16_t)t->cols, 0, 0 }; return copy_to_user((uint64_t)arg, &w, sizeof w) < 0 ? -EFAULT : 0; }
    case TIOCSWINSZ: return 0;
    case TIOCGPGRP: { int pg = t->fg_pid > 0 ? t->fg_pid : current_thread->proc->pgid; return copy_to_user((uint64_t)arg, &pg, sizeof pg) < 0 ? -EFAULT : 0; }
    case TIOCSPGRP: { int pg; if (copy_from_user(&pg, (uint64_t)arg, sizeof pg) < 0) return -EFAULT; t->fg_pid = pg; return 0; }
    case FIONREAD: {
        spin_lock(&t->lock);
        int n = t->raw ? (t->in_tail - t->in_head + TTY_IN_SIZE) % TTY_IN_SIZE : t->cooked_len - t->cooked_pos;
        spin_unlock(&t->lock);
        return copy_to_user((uint64_t)arg, &n, sizeof n) < 0 ? -EFAULT : 0; }
    }
    return -ENOTTY_;
}
static int tty_vn_poll(struct vnode *v, int ev) {
    struct tty *t = v->priv; int r = POLLOUT | POLLWRNORM;
    spin_lock(&t->lock);
    if (t->closed) r |= POLLHUP | POLLIN;
    else if (t->raw ? t->in_head != t->in_tail : (t->cooked_pos < t->cooked_len || t->eof_pending)) r |= POLLIN | POLLRDNORM;
    spin_unlock(&t->lock);
    return r;
}
static const struct vnode_ops tty_ops = { tty_vn_read, tty_vn_write, NULL, NULL, NULL, NULL, NULL, tty_vn_ioctl, NULL, NULL, NULL, tty_vn_poll };

/* the foreground job: the process (group) the terminal's shell made foreground */
static void signal_foreground(struct tty *t, int sig) {
    if (t->fg_pid <= 0) return;
    struct process *list[64];
    procs_lock();   /* recursive: held across the walk so none of them can be freed meanwhile */
    int n = process_list(list, 64);
    for (int i = 0; i < n; i++) {
        struct process *p = list[i];
        if (p->is_user && !p->exited && (p->pid == t->fg_pid || p->pgid == t->fg_pid)) signal_send(p, sig);
    }
    procs_unlock();
}
static int tty_counter;

struct tty *tty_create(void (*output)(void *, const char *, size_t), void *priv) {
    struct tty *t = kzalloc(sizeof *t);
    t->output = output; t->priv = priv; t->raw = false; t->cols = 80; t->rows = 24;
    t->lock = (spinlock_t)SPINLOCK_INIT;
    waitq_init(&t->in_wq);
    char name[16]; snprintf(name, sizeof name, "tty%d", tty_counter++);
    t->vn = devfs_add(name, VN_DEV, &tty_ops, t, 0);
    return t;
}
void tty_destroy(struct tty *t) {
    spin_lock(&t->lock);   /* readers check it under the lock before waiting */
    t->closed = true;
    spin_unlock(&t->lock);
    waitq_wake_all(&t->in_wq);
    poll_notify();
    if (t->vn) { char p[32]; snprintf(p, sizeof p, "/dev/%s", t->vn->name); vfs_unlink(p); }
    /* the struct is leaked intentionally if readers are still blocked; small and rare */
}
struct file *tty_file(struct tty *t) {
    struct file *f = kzalloc(sizeof *f);
    f->vn = t->vn; vnode_ref(t->vn); f->flags = O_RDWR; f->refcount = 1;
    return f;
}
void tty_set_raw(struct tty *t, bool raw) { spin_lock(&t->lock); t->raw = raw; t->line_len = 0; t->eof_pending = false; spin_unlock(&t->lock); }
void tty_write(struct tty *t, const char *s, size_t n) { if (t->output && !t->closed) t->output(t->priv, s, n); }

static void push_in(struct tty *t, char c) {
    int next = (t->in_tail + 1) % TTY_IN_SIZE;
    if (next == t->in_head) return;
    t->in[t->in_tail] = c; t->in_tail = next;
}
static void cooked_push_line(struct tty *t) {
    /* move the edited line into the cooked buffer */
    if (t->cooked_len + t->line_len + 1 > (int)sizeof t->cooked) { t->cooked_len = 0; t->cooked_pos = 0; }
    memcpy(t->cooked + t->cooked_len, t->line, t->line_len);
    t->cooked_len += t->line_len;
    t->cooked[t->cooked_len++] = '\n';
    t->line_len = 0;
}

void tty_input(struct tty *t, const char *s, size_t n) {
    bool wake = false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == 3 && !t->raw) {   /* Ctrl+C in cooked mode: interrupt the foreground job */
            tty_write(t, "^C\n", 3);
            /* a builtin blocked reading the terminal sees EOF; one that loops checks the flag */
            /* intr_flag points into the shell, which clears it under this lock before it frees itself */
            spin_lock(&t->lock); t->line_len = 0; t->eof_pending = true; if (t->intr_flag) *t->intr_flag = true; spin_unlock(&t->lock); wake = true;
            signal_foreground(t, SIGINT);
            if (t->on_interrupt) t->on_interrupt(t->priv);
            continue;
        }
        if (c == 28 && !t->raw) { tty_write(t, "^\\\n", 3); signal_foreground(t, SIGQUIT); continue; }
        if (c == 3) { spin_lock(&t->lock); if (t->intr_flag) *t->intr_flag = true; spin_unlock(&t->lock); }
        /* echo after dropping the spinlock: the output callback may sleep on the terminal's mutex */
        const char *echo = NULL; int elen = 0, erep = 1;
        spin_lock(&t->lock);
        if (t->raw) { push_in(t, c); wake = true; }
        else {
            if (c == '\r' || c == '\n') { echo = "\n"; elen = 1; cooked_push_line(t); wake = true; }
            else if (c == 127 || c == 8) { if (t->line_len) { t->line_len--; echo = "\b \b"; elen = 3; } }
            else if (c == 4) { if (t->line_len) cooked_push_line(t); t->eof_pending = true; wake = true; }   /* Ctrl+D */
            else if (c == 21) { erep = t->line_len; t->line_len = 0; echo = "\b \b"; elen = 3; }
            else if ((unsigned char)c >= 32 || c == '\t') { if (t->line_len < (int)sizeof t->line - 1) { t->line[t->line_len++] = c; echo = &c; elen = 1; } }
            else if (c == 27) { /* swallow escape sequences in cooked mode */ while (i + 1 < n && (s[i + 1] == '[' || isalnum(s[i + 1]) || s[i + 1] == '~' || s[i + 1] == ';')) { i++; if (isalpha(s[i]) || s[i] == '~') break; } }
        }
        spin_unlock(&t->lock);
        if (echo) for (int k = 0; k < erep; k++) tty_write(t, echo, elen);
    }
    if (wake) { waitq_wake_all(&t->in_wq); poll_notify(); }
}

int64_t tty_read(struct tty *t, void *buf, size_t n) {
    char *out = buf; size_t got = 0;
    if (n == 0) return 0;   /* read(fd, buf, 0) must not block (and must not consume a pending EOF) */
    spin_lock(&t->lock);
    for (;;) {
        if (t->closed) { spin_unlock(&t->lock); return 0; }
        if (t->raw) {
            while (got < n && t->in_head != t->in_tail) { out[got++] = t->in[t->in_head]; t->in_head = (t->in_head + 1) % TTY_IN_SIZE; }
            if (got) break;
        } else {
            if (t->cooked_pos < t->cooked_len) {
                while (got < n && t->cooked_pos < t->cooked_len) { char c = t->cooked[t->cooked_pos++]; out[got++] = c; if (c == '\n') break; }
                if (t->cooked_pos == t->cooked_len) { t->cooked_len = 0; t->cooked_pos = 0; }
                break;
            }
            if (t->eof_pending) { t->eof_pending = false; break; }   /* return 0 = EOF */
        }
        if (signal_interrupted()) { spin_unlock(&t->lock); return got ? (int64_t)got : -ERESTARTSYS; }
        waitq_wait(&t->in_wq, &t->lock);
    }
    spin_unlock(&t->lock);
    return got;
}
