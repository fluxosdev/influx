#include "tty.h"
#include "mm.h"
#include "string.h"
#include "sched.h"

static int64_t tty_vn_read(struct vnode *v, uint64_t off, void *buf, size_t n) { return tty_read(v->priv, buf, n); }
static int64_t tty_vn_write(struct vnode *v, uint64_t off, const void *buf, size_t n) { tty_write(v->priv, buf, n); return n; }
static int tty_vn_ioctl(struct vnode *v, int cmd, void *arg) {
    struct tty *t = v->priv;
    switch (cmd) {
    case 1: tty_set_raw(t, arg != NULL); return 0;   /* TIOCSRAW */
    case 2: { int *wh = arg; wh[0] = t->cols; wh[1] = t->rows; return 0; }   /* TIOCGWINSZ */
    case 3: t->fg_pid = (int)(intptr_t)arg; return 0;
    }
    return -EINVAL;
}
static const struct vnode_ops tty_ops = { tty_vn_read, tty_vn_write, NULL, NULL, NULL, NULL, NULL, tty_vn_ioctl, NULL, NULL };
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
            spin_lock(&t->lock); t->line_len = 0; t->eof_pending = true; spin_unlock(&t->lock); wake = true;
            if (t->intr_flag) *t->intr_flag = true;
            if (t->fg_pid > 0) process_kill(t->fg_pid, 2);
            if (t->on_interrupt) t->on_interrupt(t->priv);
            continue;
        }
        if (c == 3 && t->intr_flag) *t->intr_flag = true;
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
    if (wake) waitq_wake_all(&t->in_wq);
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
        if (thread_should_exit()) break;
        waitq_wait(&t->in_wq, &t->lock);
    }
    spin_unlock(&t->lock);
    return got;
}
