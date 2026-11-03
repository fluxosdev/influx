/* zsh-like interactive shell running on a tty. */
#include "ui.h"
#include "tty.h"
#include "fs.h"
#include "mm.h"
#include "string.h"
#include "arch.h"
#include "blockdev.h"
#include "gpu.h"
#include "installer.h"
#include "net.h"
#include "acpi.h"
#include "audio.h"
#include "usb.h"

#define MAX_ARGS 128
#define MAX_HIST 500
#define LINE_MAX 2048
#define WALK_DEPTH 64   /* directory levels cp -r, rm -r and du descend: their frames are small (struct walk is on the heap) */
#define ETOODEEP 40     /* ELOOP: the tree nests deeper than WALK_DEPTH */

struct shell;
struct ctx { struct shell *sh; struct file *in, *out, *err; char rbuf[512]; int rlen, rpos; };
struct shell {
    struct tty *tty; struct file *tty_f;
    struct process *proc;
    char *env[64]; int nenv;
    struct { char *name, *value; } aliases[64]; int naliases;
    char *history[MAX_HIST]; int nhist;
    int last_status;
    volatile bool cancel;
    char line[LINE_MAX]; int len, cursor;
    int hist_pos; char saved_line[LINE_MAX];
    int tab_count;
    int cur_row, cur_col;   /* where the terminal cursor sits within the wrapped prompt + line */
    bool exiting;
    int depth;
    int bg_jobs;            /* detached builtin threads (cmd &) still using this struct */
};
struct shell_start { struct tty *tty; char *cwd; char *cmd; };

/* Ctrl+C (sh->cancel), or the thread was killed: the Terminal closed or the shell is exiting.
 * Every builtin loop that can run long checks this; calls that take a cancel pointer get
 * &sh->cancel, which Terminal and shell_main also raise when they stop the shell */
static bool cancelled(struct ctx *c) { return c->sh->cancel || thread_should_exit(); }
static void cwrite(struct ctx *c, const char *s, size_t n) { if (c->out) vfs_write(c->out, s, n); }
static void cputs(struct ctx *c, const char *s) { cwrite(c, s, strlen(s)); }
static void cprintf(struct ctx *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void cprintf(struct ctx *c, const char *fmt, ...) { char buf[1024]; va_list ap; va_start(ap, fmt); int n = vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap); cwrite(c, buf, MIN(n, (int)sizeof buf - 1)); }
static void eprintf(struct ctx *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void eprintf(struct ctx *c, const char *fmt, ...) { char buf[512]; va_list ap; va_start(ap, fmt); int n = vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap); if (c->err) vfs_write(c->err, buf, MIN(n, (int)sizeof buf - 1)); }
static int cgetc(struct ctx *c) {
    if (c->rpos >= c->rlen) { if (!c->in || cancelled(c)) return -1; int64_t n = vfs_read(c->in, c->rbuf, sizeof c->rbuf); if (n <= 0) return -1; c->rlen = n; c->rpos = 0; }
    return (unsigned char)c->rbuf[c->rpos++];
}
static bool cgetline(struct ctx *c, char *buf, size_t n) {
    size_t i = 0; int ch;
    while ((ch = cgetc(c)) >= 0) { if (ch == '\n') break; if (i < n - 1) buf[i++] = ch; }
    buf[i] = 0;
    return i > 0 || ch == '\n';
}

/* ---- environment ---- */
static const char *env_get(struct shell *sh, const char *name) {
    size_t l = strlen(name);
    for (int i = 0; i < sh->nenv; i++) if (!strncmp(sh->env[i], name, l) && sh->env[i][l] == '=') return sh->env[i] + l + 1;
    return NULL;
}
static void env_set(struct shell *sh, const char *name, const char *value) {
    size_t l = strlen(name);
    char *kv = kmalloc(l + strlen(value) + 2); snprintf(kv, l + strlen(value) + 2, "%s=%s", name, value);
    for (int i = 0; i < sh->nenv; i++) if (!strncmp(sh->env[i], name, l) && sh->env[i][l] == '=') { kfree(sh->env[i]); sh->env[i] = kv; return; }
    if (sh->nenv < 64) sh->env[sh->nenv++] = kv; else kfree(kv);
}
static void env_unset(struct shell *sh, const char *name) {
    size_t l = strlen(name);
    for (int i = 0; i < sh->nenv; i++) if (!strncmp(sh->env[i], name, l) && sh->env[i][l] == '=') { kfree(sh->env[i]); sh->env[i] = sh->env[--sh->nenv]; return; }
}
static const char *alias_get(struct shell *sh, const char *name) { for (int i = 0; i < sh->naliases; i++) if (!strcmp(sh->aliases[i].name, name)) return sh->aliases[i].value; return NULL; }
static void alias_set(struct shell *sh, const char *name, const char *value) {
    for (int i = 0; i < sh->naliases; i++) if (!strcmp(sh->aliases[i].name, name)) { kfree(sh->aliases[i].value); sh->aliases[i].value = strdup(value); return; }
    if (sh->naliases < 64) { sh->aliases[sh->naliases].name = strdup(name); sh->aliases[sh->naliases].value = strdup(value); sh->naliases++; }
}
static void history_add(struct shell *sh, const char *line) {
    if (!line[0] || (sh->nhist && !strcmp(sh->history[sh->nhist - 1], line))) return;
    if (sh->nhist == MAX_HIST) { kfree(sh->history[0]); memmove(sh->history, sh->history + 1, (MAX_HIST - 1) * sizeof(char *)); sh->nhist--; }
    sh->history[sh->nhist++] = strdup(line);
    struct file *f;
    if (vfs_open("/home/flux/.zsh_history", O_WRONLY | O_CREAT | O_APPEND, &f) == 0) { vfs_write(f, line, strlen(line)); vfs_write(f, "\n", 1); vfs_close(f); }
}
static void history_load(struct shell *sh) {
    void *data; size_t sz;
    if (vfs_read_file("/home/flux/.zsh_history", &data, &sz) < 0) return;
    char *save, *tok = strtok_r(data, "\n", &save);
    while (tok) { if (strlen(tok) >= LINE_MAX) tok[LINE_MAX - 1] = 0; if (tok[0]) { if (sh->nhist == MAX_HIST) { kfree(sh->history[0]); memmove(sh->history, sh->history + 1, (MAX_HIST - 1) * sizeof(char *)); sh->nhist--; } sh->history[sh->nhist++] = strdup(tok); } tok = strtok_r(NULL, "\n", &save); }
    kfree(data);
}

/* ---- tokenizer ---- */
enum tok { T_WORD, T_PIPE, T_SEMI, T_AND, T_OR, T_GT, T_GTGT, T_LT, T_ERRGT, T_ERR2OUT, T_AMP, T_END };
struct token { enum tok type; char *text; bool quoted; };

static int tokenize(const char *s, struct token *toks, int max) {
    int n = 0;
    while (*s && n < max - 1) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#') break;
        struct token *t = &toks[n];
        t->text = NULL; t->quoted = false;
        if (*s == '|') { if (s[1] == '|') { t->type = T_OR; s += 2; } else { t->type = T_PIPE; s++; } n++; continue; }
        if (*s == '&') { if (s[1] == '&') { t->type = T_AND; s += 2; } else { t->type = T_AMP; s++; } n++; continue; }
        if (*s == ';') { t->type = T_SEMI; s++; n++; continue; }
        if (*s == '>') { if (s[1] == '>') { t->type = T_GTGT; s += 2; } else { t->type = T_GT; s++; } n++; continue; }
        if (*s == '<') { t->type = T_LT; s++; n++; continue; }
        if (*s == '2' && s[1] == '>') { if (s[2] == '&' && s[3] == '1') { t->type = T_ERR2OUT; s += 4; } else { t->type = T_ERRGT; s += 2; } n++; continue; }
        /* word */
        char buf[LINE_MAX]; int l = 0;
        while (*s && !strchr(" \t|&;<>", *s)) {
            if (*s == '\'') { s++; t->quoted = true; while (*s && *s != '\'' && l < LINE_MAX - 1) buf[l++] = *s++; if (*s == '\'') s++; }
            else if (*s == '"') { s++; t->quoted = true; while (*s && *s != '"' && l < LINE_MAX - 1) { if (*s == '\\' && (s[1] == '"' || s[1] == '\\' || s[1] == '$')) s++; buf[l++] = *s++; } if (*s == '"') s++; }
            else if (*s == '\\' && s[1]) { s++; buf[l++] = *s++; }
            else { if (*s == '>' && l > 0 && buf[l - 1] == '2') { l--; break; } buf[l++] = *s++; }
            if (l >= LINE_MAX - 1) break;
        }
        buf[l] = 0;
        t->type = T_WORD; t->text = strdup(buf);
        n++;
    }
    toks[n].type = T_END; toks[n].text = NULL;
    return n;
}

/* ---- expansion ---- */
static void expand_vars(struct shell *sh, const char *in, char *out, size_t n) {
    size_t o = 0;
    for (const char *p = in; *p && o < n - 1; ) {
        if (*p == '$') {
            p++;
            char name[64]; int l = 0;
            if (*p == '{') { p++; while (*p && *p != '}' && l < 63) name[l++] = *p++; if (*p == '}') p++; }
            else if (*p == '?') { name[0] = '?'; l = 1; p++; }
            else if (*p == '$') { name[0] = '$'; l = 1; p++; }
            else while ((isalnum(*p) || *p == '_') && l < 63) name[l++] = *p++;
            name[l] = 0;
            char tmp[32]; const char *v = NULL;
            if (!strcmp(name, "?")) { snprintf(tmp, sizeof tmp, "%d", sh->last_status); v = tmp; }
            else if (!strcmp(name, "$")) { snprintf(tmp, sizeof tmp, "%d", sh->proc->pid); v = tmp; }
            else if (!strcmp(name, "PWD")) v = sh->proc->cwd;
            else if (l) v = env_get(sh, name);
            else { out[o++] = '$'; continue; }
            if (v) { size_t vl = strlen(v); if (o + vl >= n) vl = n - 1 - o; memcpy(out + o, v, vl); o += vl; }
        } else out[o++] = *p++;
    }
    out[o] = 0;
}
static bool glob_match(const char *pat, const char *s) {
    while (*pat) {
        if (*pat == '*') { pat++; if (!*pat) return true; for (const char *q = s; ; q++) { if (glob_match(pat, q)) return true; if (!*q) return false; } }
        if (*pat == '?') { if (!*s) return false; pat++; s++; continue; }
        if (*pat == '[') { const char *e = strchr(pat, ']'); if (e) { bool ok = false, neg = pat[1] == '!' || pat[1] == '^'; for (const char *q = pat + 1 + neg; q < e; q++) { if (q[1] == '-' && q + 2 < e) { if (*s >= q[0] && *s <= q[2]) ok = true; q += 2; } else if (*q == *s) ok = true; } if (ok == neg || !*s) return false; pat = e + 1; s++; continue; } }
        if (*pat != *s) return false;
        pat++; s++;
    }
    return !*s;
}
static int expand_word(struct shell *sh, struct token *t, char **argv, int argc) {
    char buf[LINE_MAX];
    expand_vars(sh, t->text, buf, sizeof buf);
    char tilde[LINE_MAX];
    if (!t->quoted && buf[0] == '~' && (buf[1] == '/' || buf[1] == 0)) { snprintf(tilde, sizeof tilde, "/home/flux%s", buf + 1); strcpy(buf, tilde); }
    bool has_glob = !t->quoted && (strchr(buf, '*') || strchr(buf, '?') || strchr(buf, '['));
    if (!has_glob) { if (argc < MAX_ARGS - 1) argv[argc++] = strdup(buf); return argc; }
    /* glob the last path component only */
    char dir[PATH_MAX], pat[NAME_MAX + 1];
    char *slash = strrchr(buf, '/');
    if (slash && (size_t)(slash - buf) >= sizeof dir) { if (argc < MAX_ARGS - 1) argv[argc++] = strdup(buf); return argc; }
    if (slash) { size_t dl = slash - buf; memcpy(dir, buf, dl); dir[dl] = 0; if (!dl) strcpy(dir, "/"); strlcpy(pat, slash + 1, sizeof pat); }
    else { strcpy(dir, "."); strlcpy(pat, buf, sizeof pat); }
    struct vnode *d = vfs_lookup_rel(sh->proc->cwd, dir);
    int start = argc;
    if (d) {
        struct dirent de;
        for (int i = 0; vfs_readdir(d, i, &de) == 0 && argc < MAX_ARGS - 1; i++) {
            if (de.name[0] == '.' && pat[0] != '.') continue;
            if (!glob_match(pat, de.name)) continue;
            char full[PATH_MAX];
            if (slash) snprintf(full, sizeof full, "%s%s%s", dir, !strcmp(dir, "/") ? "" : "/", de.name); else strlcpy(full, de.name, sizeof full);
            argv[argc++] = strdup(full);
        }
        vnode_unref(d);
    }
    if (argc == start) { if (argc < MAX_ARGS - 1) argv[argc++] = strdup(buf); }   /* no match: keep literal (zsh would error) */
    else for (int i = start + 1; i < argc; i++) { char *k = argv[i]; int j = i - 1; while (j >= start && strcmp(argv[j], k) > 0) { argv[j + 1] = argv[j]; j--; } argv[j + 1] = k; }
    return argc;
}

/* ---- builtins ---- */
typedef int (*builtin_fn)(struct ctx *c, int argc, char **argv);
struct builtin { const char *name; builtin_fn fn; const char *help; };
static const struct builtin builtins[];
static int nbuiltins;
static const struct builtin *find_builtin(const char *name) { for (int i = 0; i < nbuiltins; i++) if (!strcmp(builtins[i].name, name)) return &builtins[i]; return NULL; }
static int run_line(struct shell *sh, struct ctx *c, const char *line);

static void resolve(struct shell *sh, const char *p, char *out, size_t n) { vfs_normalize(sh->proc->cwd, p, out, n); }
static const char *errstr(int e) {
    switch (-e) { case ENOENT: return "no such file or directory"; case EEXIST: return "file exists"; case ENOTDIR: return "not a directory"; case EISDIR: return "is a directory"; case ENOTEMPTY: return "directory not empty"; case EROFS: return "read-only file system"; case EACCES: return "permission denied"; case ENOSPC: return "no space left on device"; case EIO: return "input/output error"; case EBUSY: return "device or resource busy"; case EFBIG: return "file too large"; case ENAMETOOLONG: return "file name too long"; case ECANCELED: return "interrupted"; case ETOODEEP: return "too deeply nested"; default: return "error"; }
}
static void format_size_h(uint64_t sz, char *out, size_t n) {
    if (sz < 1024) snprintf(out, n, "%luB", sz);
    else if (sz < 1024 * 1024) snprintf(out, n, "%lu.%luK", sz / 1024, (sz % 1024) * 10 / 1024);
    else if (sz < 1024ULL * 1024 * 1024) snprintf(out, n, "%lu.%luM", sz >> 20, ((sz >> 10) % 1024) * 10 / 1024);
    else snprintf(out, n, "%lu.%luG", sz >> 30, ((sz >> 20) % 1024) * 10 / 1024);
}

static int b_cd(struct ctx *c, int argc, char **argv) {
    const char *target = argc > 1 ? argv[1] : "/home/flux";
    if (!strcmp(target, "-")) { const char *o = env_get(c->sh, "OLDPWD"); target = o ? o : "/home/flux"; }
    char p[PATH_MAX]; resolve(c->sh, target, p, sizeof p);
    struct stat st;
    if (vfs_stat(p, &st) < 0) { eprintf(c, "cd: no such file or directory: %s\n", target); return 1; }
    if (st.type != VN_DIR) { eprintf(c, "cd: not a directory: %s\n", target); return 1; }
    env_set(c->sh, "OLDPWD", c->sh->proc->cwd);
    strlcpy(c->sh->proc->cwd, p, sizeof c->sh->proc->cwd);
    env_set(c->sh, "PWD", p);
    return 0;
}
static int b_pwd(struct ctx *c, int argc, char **argv) { cprintf(c, "%s\n", c->sh->proc->cwd); return 0; }
static int b_echo(struct ctx *c, int argc, char **argv) {
    bool nl = true, esc = false; int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) { bool ok = true; for (const char *p = argv[i] + 1; *p; p++) if (*p != 'n' && *p != 'e') ok = false; if (!ok) break; if (strchr(argv[i], 'n')) nl = false; if (strchr(argv[i], 'e')) esc = true; }
    for (; i < argc; i++) {
        if (esc) { char buf[LINE_MAX]; int o = 0; for (const char *p = argv[i]; *p && o < LINE_MAX - 1; p++) { if (*p == '\\' && p[1]) { p++; buf[o++] = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p == 'e' ? 27 : *p == 'a' ? 7 : *p; } else buf[o++] = *p; } buf[o] = 0; cputs(c, buf); }
        else cputs(c, argv[i]);
        if (i < argc - 1) cputs(c, " ");
    }
    if (nl) cputs(c, "\n");
    return 0;
}
static int b_printf(struct ctx *c, int argc, char **argv) {
    if (argc < 2) return 1;
    int ai = 2; char out[LINE_MAX]; int o = 0;
    for (const char *p = argv[1]; *p && o < LINE_MAX - 64; p++) {
        if (*p == '\\' && p[1]) { p++; out[o++] = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p; continue; }
        if (*p != '%') { out[o++] = *p; continue; }
        p++; if (*p == '%') { out[o++] = '%'; continue; }
        char spec[16]; int sl = 0; spec[sl++] = '%';
        int nd = 0;   /* drop '*' (it would pull an extra va_arg) and cap widths at 4 digits */
        while (*p && !isalpha(*p) && sl < 12) { nd = isdigit(*p) ? nd + 1 : 0; if (*p != '*' && nd <= 4) spec[sl++] = *p; p++; }
        char conv = *p; spec[sl++] = conv == 'd' || conv == 'i' ? 'l' : conv; if (conv == 'd' || conv == 'i') spec[sl++] = 'd'; spec[sl] = 0;
        const char *arg = ai < argc ? argv[ai++] : ""; int r = 0;
        if (conv == 's') r = snprintf(out + o, LINE_MAX - o, spec, arg);
        else if (conv == 'd' || conv == 'i') r = snprintf(out + o, LINE_MAX - o, spec, strtol(arg, NULL, 0));
        else if (conv == 'x' || conv == 'X' || conv == 'u' || conv == 'o') { spec[sl - 1] = conv; spec[sl] = 0; char s2[16]; snprintf(s2, sizeof s2, "%.*sl%c", sl - 1, spec, conv); r = snprintf(out + o, LINE_MAX - o, s2, strtoul(arg, NULL, 0)); }
        else if (conv == 'c') r = snprintf(out + o, LINE_MAX - o, "%c", arg[0]);
        o += CLAMP(r, 0, LINE_MAX - 1 - o);   /* snprintf returns the untruncated length */
        if (!*p) break;
    }
    cwrite(c, out, o);
    return 0;
}
static int b_ls(struct ctx *c, int argc, char **argv) {
    bool lng = false, all = false, human = false; int npaths = 0; char *paths[MAX_ARGS];
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) { for (const char *p = argv[i] + 1; *p; p++) { if (*p == 'l') lng = true; else if (*p == 'a') all = true; else if (*p == 'h') human = true; } }
        else paths[npaths++] = argv[i];
    }
    if (!npaths) paths[npaths++] = ".";
    bool tty_out = c->out && c->out->vn == c->sh->tty->vn;   /* pipes are VN_DEV too */
    int status = 0;
    for (int pi = 0; pi < npaths; pi++) {
        char p[PATH_MAX]; resolve(c->sh, paths[pi], p, sizeof p);
        struct stat st;
        if (vfs_stat(p, &st) < 0) { eprintf(c, "ls: %s: no such file or directory\n", paths[pi]); status = 1; continue; }
        if (npaths > 1) cprintf(c, "%s%s:\n", pi ? "\n" : "", paths[pi]);
        struct dirent *ents = kmalloc(sizeof(struct dirent) * 512); int n = 0;
        if (st.type == VN_DIR) {
            struct vnode *d = vfs_lookup(p);
            for (int i = 0; d && n < 512 && vfs_readdir(d, i, &ents[n]) == 0; i++) if (all || ents[n].name[0] != '.') n++;
            if (d) vnode_unref(d);
        } else { strlcpy(ents[0].name, vfs_basename(p), NAME_MAX + 1); ents[0].type = st.type; ents[0].size = st.size; ents[0].mtime = st.mtime; n = 1; }
        for (int i = 1; i < n; i++) { struct dirent t = ents[i]; int j = i - 1; while (j >= 0 && strcasecmp(ents[j].name, t.name) > 0) { ents[j + 1] = ents[j]; j--; } ents[j + 1] = t; }
        if (lng) {
            for (int i = 0; i < n; i++) {
                struct dirent *e = &ents[i];
                char mode[11] = "-rw-r--r--"; if (e->type == VN_DIR) { mode[0] = 'd'; mode[3] = 'x'; mode[6] = 'x'; mode[9] = 'x'; } else if (e->type == VN_DEV) mode[0] = 'c'; else if (e->type == VN_BLK) mode[0] = 'b';
                struct rtc_time dt; unix_to_datetime(e->mtime + tz_offset_minutes * 60, &dt);
                static const char *mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
                char sz[16]; if (human) format_size_h(e->size, sz, sizeof sz); else snprintf(sz, sizeof sz, "%lu", e->size);
                const char *col = e->type == VN_DIR ? "\033[1;34m" : (strstr(e->name, ".sh") || !strncmp(p, "/bin", 4)) ? "\033[1;32m" : "";
                cprintf(c, "%s  1 flux  staff %8s %s %2d %02d:%02d %s%s%s%s\n", mode, sz, mon[(dt.month - 1) % 12], dt.day, dt.hour, dt.minute, tty_out ? col : "", e->name, tty_out && col[0] ? "\033[0m" : "", e->type == VN_DIR ? "/" : "");
            }
        } else {
            int maxw = 0; for (int i = 0; i < n; i++) { int l = strlen(ents[i].name) + (ents[i].type == VN_DIR ? 1 : 0); if (l > maxw) maxw = l; }
            int colw = maxw + 2, cols = tty_out ? MAX(1, c->sh->tty->cols / colw) : 1;
            int rows = (n + cols - 1) / cols;
            for (int r = 0; r < rows; r++) {
                for (int col = 0; col < cols; col++) {
                    int i = col * rows + r; if (i >= n) break;
                    struct dirent *e = &ents[i];
                    const char *colr = e->type == VN_DIR ? "\033[1;34m" : !strncmp(p, "/bin", 4) ? "\033[1;32m" : "";
                    char name[NAME_MAX + 2]; snprintf(name, sizeof name, "%s%s", e->name, e->type == VN_DIR ? "/" : "");
                    if (tty_out && colr[0]) cprintf(c, "%s%s\033[0m", colr, name); else cputs(c, name);
                    if (col < cols - 1 && i + rows < n) for (int k = strlen(name); k < colw; k++) cputs(c, " ");
                }
                cputs(c, "\n");
            }
        }
        kfree(ents);
    }
    return status;
}
static int b_cat(struct ctx *c, int argc, char **argv) {
    bool number = false; int status = 0, nfiles = 0;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "-n")) number = true; else nfiles++;
    int ln = 1;
    if (!nfiles) { char line[LINE_MAX]; while (cgetline(c, line, sizeof line)) { if (number) cprintf(c, "%6d\t", ln++); cputs(c, line); cputs(c, "\n"); } return 0; }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n")) continue;
        char p[PATH_MAX]; resolve(c->sh, argv[i], p, sizeof p);
        struct file *f; int r = vfs_open(p, O_RDONLY, &f);
        if (r < 0) { eprintf(c, "cat: %s: %s\n", argv[i], errstr(r)); status = 1; continue; }
        if (f->vn->type == VN_DIR) { eprintf(c, "cat: %s: is a directory\n", argv[i]); vfs_close(f); status = 1; continue; }
        char *buf = kmalloc(8192); int64_t n;
        if (number) { struct ctx fc = { c->sh, f, c->out, c->err }; char line[LINE_MAX]; while (cgetline(&fc, line, sizeof line)) { cprintf(c, "%6d\t%s\n", ln++, line); } }
        else while (!cancelled(c) && (n = vfs_read(f, buf, 8192)) > 0) cwrite(c, buf, n);
        kfree(buf); vfs_close(f);
    }
    return status;
}
static int b_mkdir(struct ctx *c, int argc, char **argv) {
    bool parents = false; int status = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p")) { parents = true; continue; }
        char p[PATH_MAX]; resolve(c->sh, argv[i], p, sizeof p);
        if (parents) { for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; vfs_mkdir(p); *s = '/'; } }
        int r = vfs_mkdir(p);
        if (r < 0 && !(parents && r == -EEXIST)) { eprintf(c, "mkdir: %s: %s\n", argv[i], errstr(r)); status = 1; }
    }
    return status;
}
/* Recursive walks keep their paths and dirent on the heap and stop at WALK_DEPTH levels: with
 * PATH_MAX buffers in every frame, ~50 nested directories would overflow the kernel stack */
struct walk { char a[PATH_MAX], b[PATH_MAX]; struct dirent de; };
static bool path_join(char *out, const char *dir, const char *name) { return (size_t)snprintf(out, PATH_MAX, "%s/%s", !strcmp(dir, "/") ? "" : dir, name) < PATH_MAX; }   /* false: too long (a cut path names another file) */
static int remove_recursive(struct ctx *c, const char *p, int depth) {
    struct stat st; if (vfs_stat(p, &st) < 0) return -ENOENT;
    if (st.type == VN_DIR) {
        if (depth >= WALK_DEPTH) return -ETOODEEP;
        struct vnode *d = vfs_lookup(p); struct walk *w = kmalloc(sizeof *w); int r = 0;
        while (d && !r && vfs_readdir(d, 0, &w->de) == 0) r = cancelled(c) ? -ECANCELED : !path_join(w->a, p, w->de.name) ? -ENAMETOOLONG : remove_recursive(c, w->a, depth + 1);
        kfree(w); if (d) vnode_unref(d);
        if (r < 0) return r;
    }
    return vfs_unlink(p);
}
static int b_rm(struct ctx *c, int argc, char **argv) {
    bool rec = false, force = false; int status = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') { if (strchr(argv[i], 'r') || strchr(argv[i], 'R')) rec = true; if (strchr(argv[i], 'f')) force = true; continue; }
        char p[PATH_MAX]; resolve(c->sh, argv[i], p, sizeof p);
        struct stat st;
        if (vfs_stat(p, &st) < 0) { if (!force) { eprintf(c, "rm: %s: no such file or directory\n", argv[i]); status = 1; } continue; }
        if (st.type == VN_DIR && !rec) { eprintf(c, "rm: %s: is a directory\n", argv[i]); status = 1; continue; }
        int r = rec ? remove_recursive(c, p, 0) : vfs_unlink(p);
        if (r < 0) { eprintf(c, "rm: %s: %s\n", argv[i], errstr(r)); status = 1; }
    }
    return status;
}
static int b_rmdir(struct ctx *c, int argc, char **argv) {
    int status = 0;
    for (int i = 1; i < argc; i++) { char p[PATH_MAX]; resolve(c->sh, argv[i], p, sizeof p); struct stat st; if (vfs_stat(p, &st) < 0 || st.type != VN_DIR) { eprintf(c, "rmdir: %s: not a directory\n", argv[i]); status = 1; continue; } int r = vfs_unlink(p); if (r < 0) { eprintf(c, "rmdir: %s: %s\n", argv[i], errstr(r)); status = 1; } }
    return status;
}
static int b_touch(struct ctx *c, int argc, char **argv) {
    int status = 0;
    for (int i = 1; i < argc; i++) {
        char p[PATH_MAX]; resolve(c->sh, argv[i], p, sizeof p);
        struct file *f; int r = vfs_open(p, O_WRONLY | O_CREAT, &f);
        if (r < 0) { eprintf(c, "touch: %s: %s\n", argv[i], errstr(r)); status = 1; continue; }
        f->vn->mtime = time_unix(); vfs_close(f);
    }
    return status;
}
static int copy_path(struct ctx *c, const char *src, const char *dst, bool rec, int depth) {
    struct stat st; if (vfs_stat(src, &st) < 0) return -ENOENT;
    if (st.type == VN_DIR) {
        if (!rec) return -EISDIR;
        if (depth >= WALK_DEPTH) return -ETOODEEP;
        int r = vfs_mkdir(dst); if (r < 0 && r != -EEXIST) return r;
        struct vnode *d = vfs_lookup(src); struct walk *w = kmalloc(sizeof *w); int res = d ? 0 : -ENOENT;
        for (int i = 0; d && vfs_readdir(d, i, &w->de) == 0; i++) {
            if (cancelled(c)) { res = -ECANCELED; break; }
            int e = path_join(w->a, src, w->de.name) && path_join(w->b, dst, w->de.name) ? copy_path(c, w->a, w->b, true, depth + 1) : -ENAMETOOLONG;
            if (e < 0) res = e;
        }
        kfree(w); if (d) vnode_unref(d);
        return res;
    }
    void *data; size_t sz; int r = vfs_read_file(src, &data, &sz); if (r < 0) return r;
    r = vfs_write_file(dst, data, sz); kfree(data); return r;
}
/* the (normalized) target is the source itself or lies inside it */
static bool path_within(const char *p, const char *base) { size_t l = strlen(base); return !strcmp(base, "/") || (!strncmp(p, base, l) && (p[l] == 0 || p[l] == '/')); }
static bool copy_target_ok(struct ctx *c, const char *cmd, const char *arg, const char *src, char *target) {
    char tn[PATH_MAX]; vfs_normalize("/", target, tn, sizeof tn); strlcpy(target, tn, PATH_MAX);
    if (!strcmp(tn, src)) { eprintf(c, "%s: %s and %s are the same file\n", cmd, arg, tn); return false; }
    if (path_within(tn, src)) { eprintf(c, "%s: cannot %s %s into itself\n", cmd, !strcmp(cmd, "mv") ? "move" : "copy", arg); return false; }
    return true;
}
static int b_cp(struct ctx *c, int argc, char **argv) {
    bool rec = false; char *args[MAX_ARGS]; int n = 0;
    for (int i = 1; i < argc; i++) { if (argv[i][0] == '-') { if (strchr(argv[i], 'r') || strchr(argv[i], 'R')) rec = true; } else args[n++] = argv[i]; }
    if (n < 2) { eprintf(c, "usage: cp [-r] source... target\n"); return 1; }
    char dst[PATH_MAX]; resolve(c->sh, args[n - 1], dst, sizeof dst);
    struct stat dst_st; bool dst_dir = vfs_stat(dst, &dst_st) == 0 && dst_st.type == VN_DIR;
    int status = 0;
    for (int i = 0; i < n - 1; i++) {
        char src[PATH_MAX], target[PATH_MAX]; resolve(c->sh, args[i], src, sizeof src);
        if (dst_dir) snprintf(target, sizeof target, "%s/%s", dst, vfs_basename(src)); else strlcpy(target, dst, sizeof target);
        if (!copy_target_ok(c, "cp", args[i], src, target)) { status = 1; continue; }
        int r = copy_path(c, src, target, rec, 0);
        if (r < 0) { eprintf(c, "cp: %s: %s\n", args[i], errstr(r)); status = 1; }
    }
    return status;
}
static int b_mv(struct ctx *c, int argc, char **argv) {
    if (argc < 3) { eprintf(c, "usage: mv source... target\n"); return 1; }
    char dst[PATH_MAX]; resolve(c->sh, argv[argc - 1], dst, sizeof dst);
    struct stat dst_st; bool dst_dir = vfs_stat(dst, &dst_st) == 0 && dst_st.type == VN_DIR;
    int status = 0;
    for (int i = 1; i < argc - 1; i++) {
        char src[PATH_MAX], target[PATH_MAX]; resolve(c->sh, argv[i], src, sizeof src);
        if (dst_dir) snprintf(target, sizeof target, "%s/%s", dst, vfs_basename(src)); else strlcpy(target, dst, sizeof target);
        if (!copy_target_ok(c, "mv", argv[i], src, target)) { status = 1; continue; }
        int r = copy_path(c, src, target, true, 0);
        if (r == 0) r = remove_recursive(c, src, 0);
        if (r < 0) { eprintf(c, "mv: %s: %s\n", argv[i], errstr(r)); status = 1; }
    }
    return status;
}
static int head_tail(struct ctx *c, int argc, char **argv, bool tail) {
    int n = 10; const char *file = NULL;
    for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "-n") && i + 1 < argc) n = atoi(argv[++i]); else if (argv[i][0] == '-' && isdigit(argv[i][1])) n = atoi(argv[i] + 1); else file = argv[i]; }
    struct ctx fc = *c; struct file *f = NULL;
    if (file) { char p[PATH_MAX]; resolve(c->sh, file, p, sizeof p); int r = vfs_open(p, O_RDONLY, &f); if (r < 0) { eprintf(c, "%s: %s: %s\n", tail ? "tail" : "head", file, errstr(r)); return 1; } fc.in = f; fc.rlen = fc.rpos = 0; }
    char line[LINE_MAX];
    if (tail && n > 65536) n = 65536;
    if (n <= 0) { if (f) vfs_close(f); return 0; }
    if (!tail) { for (int i = 0; i < n && cgetline(&fc, line, sizeof line); i++) { cputs(c, line); cputs(c, "\n"); } }
    else { char **ring = kzalloc(sizeof(char *) * n); int cnt = 0; while (cgetline(&fc, line, sizeof line)) { kfree(ring[cnt % n]); ring[cnt % n] = strdup(line); cnt++; } int start = cnt > n ? cnt - n : 0; for (int i = start; i < cnt; i++) { cputs(c, ring[i % n]); cputs(c, "\n"); } for (int i = 0; i < n; i++) kfree(ring[i]); kfree(ring); }
    if (f) vfs_close(f);
    return 0;
}
static int b_head(struct ctx *c, int argc, char **argv) { return head_tail(c, argc, argv, false); }
static int b_tail(struct ctx *c, int argc, char **argv) { return head_tail(c, argc, argv, true); }
static int b_wc(struct ctx *c, int argc, char **argv) {
    const char *file = argc > 1 && argv[argc - 1][0] != '-' ? argv[argc - 1] : NULL;
    bool ol = false, ow = false, oc = false;
    for (int i = 1; i < argc; i++) if (argv[i][0] == '-') { if (strchr(argv[i], 'l')) ol = true; if (strchr(argv[i], 'w')) ow = true; if (strchr(argv[i], 'c')) oc = true; }
    if (!ol && !ow && !oc) ol = ow = oc = true;
    struct ctx fc = *c; struct file *f = NULL;
    if (file) { char p[PATH_MAX]; resolve(c->sh, file, p, sizeof p); if (vfs_open(p, O_RDONLY, &f) < 0) { eprintf(c, "wc: %s: no such file\n", file); return 1; } fc.in = f; fc.rlen = fc.rpos = 0; }
    uint64_t lines = 0, words = 0, chars = 0; bool in = false; int ch;
    while ((ch = cgetc(&fc)) >= 0) { chars++; if (ch == '\n') lines++; if (isspace(ch)) in = false; else if (!in) { in = true; words++; } }
    if (f) vfs_close(f);
    if (ol) cprintf(c, "%8lu", lines); if (ow) cprintf(c, "%8lu", words); if (oc) cprintf(c, "%8lu", chars);
    cprintf(c, "%s%s\n", file ? " " : "", file ? file : "");
    return 0;
}
static int b_grep(struct ctx *c, int argc, char **argv) {
    bool icase = false, invert = false, count = false, lineno = false; const char *pat = NULL; char *files[MAX_ARGS]; int nf = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] && !pat) { for (const char *p = argv[i] + 1; *p; p++) { if (*p == 'i') icase = true; else if (*p == 'v') invert = true; else if (*p == 'c') count = true; else if (*p == 'n') lineno = true; } }
        else if (!pat) pat = argv[i]; else files[nf++] = argv[i];
    }
    if (!pat) { eprintf(c, "usage: grep [-ivcn] pattern [file...]\n"); return 2; }
    char lpat[256]; strlcpy(lpat, pat, sizeof lpat); if (icase) for (char *p = lpat; *p; p++) *p = tolower(*p);
    int found = 0;
    for (int fi = 0; fi < (nf ? nf : 1); fi++) {
        struct ctx fc = *c; struct file *f = NULL;
        if (nf) { char p[PATH_MAX]; resolve(c->sh, files[fi], p, sizeof p); if (vfs_open(p, O_RDONLY, &f) < 0) { eprintf(c, "grep: %s: no such file\n", files[fi]); continue; } fc.in = f; fc.rlen = fc.rpos = 0; }
        char line[LINE_MAX]; int ln = 0, cnt = 0;
        while (cgetline(&fc, line, sizeof line)) {
            ln++;
            bool m;
            if (icase) { char l2[LINE_MAX]; strlcpy(l2, line, sizeof l2); for (char *p = l2; *p; p++) *p = tolower(*p); m = strstr(l2, lpat) != NULL; } else m = strstr(line, pat) != NULL;
            if (m != invert) {
                found++; cnt++;
                if (count) continue;
                bool tty = c->out && c->out->vn == c->sh->tty->vn;   /* colour only on the terminal, not into pipes or files */
                if (nf > 1) cprintf(c, tty ? "\033[35m%s\033[0m:" : "%s:", files[fi]);
                if (lineno) cprintf(c, tty ? "\033[32m%d\033[0m:" : "%d:", ln);
                /* highlight matches */
                if (!invert && !icase && pat[0] && tty) { const char *p = line, *q; while ((q = strstr(p, pat))) { cwrite(c, p, q - p); cprintf(c, "\033[1;31m%s\033[0m", pat); p = q + strlen(pat); } cputs(c, p); cputs(c, "\n"); }
                else { cputs(c, line); cputs(c, "\n"); }
            }
        }
        if (count) { if (nf > 1) cprintf(c, "%s:", files[fi]); cprintf(c, "%d\n", cnt); }
        if (f) vfs_close(f);
    }
    return found ? 0 : 1;
}
static int b_sort(struct ctx *c, int argc, char **argv) {
    bool rev = false, num = false, uniq = false;
    for (int i = 1; i < argc; i++) if (argv[i][0] == '-') { if (strchr(argv[i], 'r')) rev = true; if (strchr(argv[i], 'n')) num = true; if (strchr(argv[i], 'u')) uniq = true; }
    char **lines = kmalloc(sizeof(char *) * 4096); int n = 0; char line[LINE_MAX];
    while (n < 4096 && cgetline(c, line, sizeof line)) lines[n++] = strdup(line);
    for (int i = 1; i < n; i++) { char *k = lines[i]; int j = i - 1; while (j >= 0) { int cmp = num ? (strtol(lines[j], NULL, 10) > strtol(k, NULL, 10)) - (strtol(lines[j], NULL, 10) < strtol(k, NULL, 10)) : strcmp(lines[j], k); if (rev) cmp = -cmp; if (cmp <= 0) break; lines[j + 1] = lines[j]; j--; } lines[j + 1] = k; }
    for (int i = 0; i < n; i++) { if (uniq && i && !strcmp(lines[i], lines[i - 1])) continue; cputs(c, lines[i]); cputs(c, "\n"); }
    for (int i = 0; i < n; i++) kfree(lines[i]); kfree(lines);
    return 0;
}
static int b_uniq(struct ctx *c, int argc, char **argv) {
    bool count = argc > 1 && !strcmp(argv[1], "-c");
    char line[LINE_MAX], prev[LINE_MAX]; bool have = false; int cnt = 0;
    while (cgetline(c, line, sizeof line)) { if (have && !strcmp(line, prev)) { cnt++; continue; } if (have) { if (count) cprintf(c, "%7d ", cnt); cputs(c, prev); cputs(c, "\n"); } strcpy(prev, line); have = true; cnt = 1; }
    if (have) { if (count) cprintf(c, "%7d ", cnt); cputs(c, prev); cputs(c, "\n"); }
    return 0;
}
static int b_rev(struct ctx *c, int argc, char **argv) { char line[LINE_MAX]; while (cgetline(c, line, sizeof line)) { for (int i = strlen(line) - 1; i >= 0; i--) cwrite(c, &line[i], 1); cputs(c, "\n"); } return 0; }
static int b_tee(struct ctx *c, int argc, char **argv) {
    struct file *f = NULL; if (argc > 1) { char p[PATH_MAX]; resolve(c->sh, argv[argc - 1], p, sizeof p); vfs_open(p, O_WRONLY | O_CREAT | (argc > 2 && !strcmp(argv[1], "-a") ? O_APPEND : O_TRUNC), &f); }
    char line[LINE_MAX]; while (cgetline(c, line, sizeof line)) { cputs(c, line); cputs(c, "\n"); if (f) { vfs_write(f, line, strlen(line)); vfs_write(f, "\n", 1); } }
    if (f) vfs_close(f); return 0;
}
static int b_seq(struct ctx *c, int argc, char **argv) {
    long a = 1, b = 0, step = 1;
    if (argc == 2) b = strtol(argv[1], NULL, 10); else if (argc == 3) { a = strtol(argv[1], NULL, 10); b = strtol(argv[2], NULL, 10); } else if (argc >= 4) { a = strtol(argv[1], NULL, 10); step = strtol(argv[2], NULL, 10); b = strtol(argv[3], NULL, 10); }
    if (step == 0) return 1;
    for (long i = a; step > 0 ? i <= b : i >= b; i += step) { if (cancelled(c)) break; cprintf(c, "%ld\n", i); }
    return 0;
}
static int b_clear(struct ctx *c, int argc, char **argv) { cputs(c, "\033[H\033[2J\033[3J"); return 0; }
static int b_ps(struct ctx *c, int argc, char **argv) {
    bool threads = argc > 1 && (strstr(argv[1], "T") || strstr(argv[1], "t"));
    sched_sample_cpu();
    /* format under the procs lock so the reaper can't free a process mid-walk; write once it is dropped */
    size_t cap = 32768, o = 0; char *out = kmalloc(cap);
    #define PS_OUT(...) do { int r_ = snprintf(out + o, cap - o, __VA_ARGS__); o += CLAMP(r_, 0, (int)(cap - 1 - o)); } while (0)
    procs_lock();
    struct process *list[64]; int n = process_list(list, 64);
    PS_OUT("  PID %s  %%CPU     MEM STAT  CMD\n", threads ? "  TID" : "     ");
    for (int i = 0; i < n; i++) {
        struct process *p = list[i];
        uint64_t mem = p->heap_bytes + (p->pml4 ? vmm_user_pages(p->pml4) * PAGE_SIZE : 0);
        int cpu = 0, nt = 0; struct thread *t; list_for_each_entry(t, &p->threads, proc_link) { cpu += t->cpu_percent; nt++; }
        char memstr[16]; format_size_h(mem, memstr, sizeof memstr);
        PS_OUT("%5d %s %4d%% %7s %-4s  %s%s\n", p->pid, threads ? "     " : "", cpu, memstr, p->exited ? "Z" : "S", p->name, p->is_user ? "" : nt > 1 ? "" : "");
        if (threads) list_for_each_entry(t, &p->threads, proc_link) {
            const char *st = t->state == THREAD_RUNNING ? "R" : t->state == THREAD_READY ? "R" : t->state == THREAD_SLEEPING ? "S" : t->state == THREAD_BLOCKED ? "D" : "Z";
            PS_OUT("      %5d %4d%%         %-4s    \u2514 %s\n", t->tid, t->cpu_percent, st, t->name);
        }
    }
    procs_unlock();
    #undef PS_OUT
    cwrite(c, out, o); kfree(out);
    return 0;
}
static int b_kill(struct ctx *c, int argc, char **argv) {
    int sig = 15, status = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') { sig = atoi(argv[i] + 1); if (!strcmp(argv[i], "-KILL") || !strcmp(argv[i], "-9")) sig = 9; continue; }
        int pid = atoi(argv[i]);
        if (pid == c->sh->proc->pid) { c->sh->exiting = true; continue; }
        if (!process_kill(pid, sig)) { eprintf(c, "kill: kill %s failed: no such process\n", argv[i]); status = 1; }
    }
    return status;
}
static int b_uname(struct ctx *c, int argc, char **argv) {
    bool all = argc > 1 && strchr(argv[1], 'a');
    if (!all && argc > 1) { if (strchr(argv[1], 'r')) cprintf(c, "%s\n", INFLUX_VERSION); else if (strchr(argv[1], 'm')) cputs(c, "x86_64\n"); else if (strchr(argv[1], 'n')) cprintf(c, "%s\n", system_hostname); else cputs(c, "inFlux\n"); return 0; }
    if (all) cprintf(c, "inFlux %s %s #1 x86_64 inFlux\n", system_hostname, INFLUX_VERSION); else cputs(c, "inFlux\n");
    return 0;
}
static int b_free(struct ctx *c, int argc, char **argv) {
    bool h = argc > 1 && strchr(argv[1], 'h');
    uint64_t total = pmm_total_bytes(), used = pmm_used_bytes(), fr = pmm_free_bytes();
    if (h) { char a[16], b[16], d[16]; format_size_h(total, a, 16); format_size_h(used, b, 16); format_size_h(fr, d, 16); cprintf(c, "              total        used        free\nMem:    %11s %11s %11s\n", a, b, d); }
    else cprintf(c, "              total        used        free\nMem:    %11lu %11lu %11lu\n", total / 1024, used / 1024, fr / 1024);
    cprintf(c, "Heap:   %11lu %11lu\n", heap_total_bytes() / 1024, heap_used_bytes() / 1024);
    return 0;
}
static int b_df(struct ctx *c, int argc, char **argv) {
    bool h = argc > 1 && strchr(argv[1], 'h');
    cprintf(c, "%-16s %10s %10s %10s %5s  %s\n", "Filesystem", "Size", "Used", "Avail", "Use%", "Mounted on");
    char a[16], b[16], d[16];
    uint64_t t = pmm_total_bytes(), u = heap_used_bytes();
    if (h) { format_size_h(t, a, 16); format_size_h(u, b, 16); format_size_h(t - u, d, 16); } else { snprintf(a, 16, "%lu", t / 1024); snprintf(b, 16, "%lu", u / 1024); snprintf(d, 16, "%lu", (t - u) / 1024); }
    cprintf(c, "%-16s %10s %10s %10s %4lu%%  /\n", "tmpfs", a, b, d, u * 100 / t);
    for (int i = 0; i < vfs_mount_count(); i++) {
        char mp[PATH_MAX]; struct vnode *r = vfs_mount_at(i, mp, sizeof mp);
        if (!r) continue;
        uint64_t tot = r->fs_total, fre = r->fs_free, use = tot > fre ? tot - fre : 0;
        if (h) { format_size_h(tot, a, 16); format_size_h(use, b, 16); format_size_h(fre, d, 16); } else { snprintf(a, 16, "%lu", tot / 1024); snprintf(b, 16, "%lu", use / 1024); snprintf(d, 16, "%lu", fre / 1024); }
        cprintf(c, "%-16s %10s %10s %10s %4lu%%  %s\n", r->fs_name ? r->fs_name : "?", a, b, d, tot ? use * 100 / tot : 0, mp);
    }
    return 0;
}
static int b_mount(struct ctx *c, int argc, char **argv) {
    cprintf(c, "tmpfs on / type tmpfs (rw)\n");
    for (int i = 0; i < vfs_mount_count(); i++) { char mp[PATH_MAX]; struct vnode *r = vfs_mount_at(i, mp, sizeof mp); if (!r) continue; cprintf(c, "%s on %s type %s (rw)\n", r->name[0] ? r->name : "?", mp, r->fs_name ? r->fs_name : "?"); }
    return 0;
}
static int b_lsblk(struct ctx *c, int argc, char **argv) {
    cprintf(c, "%-10s %4s %10s %-5s %-24s %s\n", "NAME", "TYPE", "SIZE", "FS", "MODEL", "MOUNTPOINT");
    for (int i = 0; i < blockdev_count(); i++) {
        struct blockdev *b = blockdev_at(i); if (!b) continue;
        char sz[16]; format_size_h(b->sectors * b->sector_size, sz, sizeof sz);
        char mp[PATH_MAX] = ""; for (int m = 0; m < vfs_mount_count(); m++) { char p[PATH_MAX]; struct vnode *r = vfs_mount_at(m, p, sizeof p); if (r && r->priv == b) strlcpy(mp, p, sizeof mp); }
        cprintf(c, "%s%-9s %4s %10s %-5s %-24s %s\n", b->parent ? "\u2514\u2500" : "", b->name, b->parent ? "part" : "disk", sz, b->fs_type ? b->fs_type : "", b->parent ? "" : b->model, mp);
    }
    return 0;
}
static int b_lsgpu(struct ctx *c, int argc, char **argv) {
    for (int i = 0; i < gpu_count(); i++) {
        struct gpu_device *g = gpu_at(i);
        cprintf(c, "%s%s\n  driver: %s   chip: %s   bus: %s\n", g->name, g == gpu_primary() ? "  (primary)" : "", g->driver, g->chip, g->bus[0] ? g->bus : "-");
        if (g->vram_bytes) cprintf(c, "  VRAM: %lu MiB at %lx, registers at %lx\n", g->vram_bytes >> 20, g->vram_base, g->mmio_base);
        if (g->bios_message[0]) cprintf(c, "  BIOS: %s %s\n", g->bios_message, g->bios_part);
        if (g->engine_clock_khz) {
            const char *units = !strcmp(g->driver, "i915") ? "execution units" : "stream processors";
            if (g->memory_clock_khz) cprintf(c, "  clocks: %u MHz engine, %u MHz memory, %d %s\n", g->engine_clock_khz / 1000, g->memory_clock_khz / 1000, g->stream_processors, units);
            else cprintf(c, "  clocks: %u MHz engine, %d %s\n", g->engine_clock_khz / 1000, g->stream_processors, units);
        }
        cprintf(c, "  features: %s%s%s\n", g->scanout_verified ? "scanout " : "", g->page_flip ? "page-flip " : "", g->hw_cursor ? "hw-cursor" : "");
    }
    return 0;
}
static void install_progress(void *ctx, int pct, const char *m) { cprintf((struct ctx *)ctx, "[%3d%%] %s\n", pct, m); }
static int b_install(struct ctx *c, int argc, char **argv) {
    bool yes = false; const char *dev = NULL;
    for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "-y")) yes = true; else dev = argv[i]; }
    if (!dev) { cputs(c, "usage: install [-y] /dev/<disk>   (erases the whole disk after asking; -y: don't ask. See also the Install inFlux app)\n"); return 1; }
    const char *n = dev; if (!strncmp(n, "/dev/", 5)) n += 5;
    struct blockdev *d = blockdev_find(n);
    if (!d || d->parent) { eprintf(c, "install: %s is not a whole disk\n", dev); return 1; }
    if (install_is_system_disk(d)) { eprintf(c, "install: %s holds the running system (mounted at /home); choose another disk\n", dev); return 1; }
    if (!yes) {   /* read the answer like any builtin reads its input: a line from the terminal (cooked) or a pipe */
        char sz[16], ans[16]; format_size_h(d->sectors * d->sector_size, sz, sizeof sz);
        cprintf(c, "Everything on /dev/%s (%s, %s) will be erased.\nType yes to install inFlux on it: ", d->name, d->model[0] ? d->model : "disk", sz);
        if (!cgetline(c, ans, sizeof ans) || strcmp(ans, "yes")) { eprintf(c, "install: cancelled, nothing was written\n"); return 1; }
    }
    int r = install_run(d, install_progress, c);
    c->sh->last_status = r < 0 ? 1 : 0;
    return r < 0 ? 1 : 0;
}
static int b_display(struct ctx *c, int argc, char **argv) {
    struct gpu_device *g = gpu_primary();
    if (argc < 2) {
        cprintf(c, "current: %dx%d (%s%s)\n", wm_screen_w(), wm_screen_h(), g ? g->name : "?", g && g->modeset ? ", runtime mode setting" : ", boot-time only");
        struct display_mode m[16]; int n = gpu_list_modes(m, 16);
        cputs(c, "modes:"); for (int i = 0; i < n; i++) cprintf(c, " %dx%d", m[i].w, m[i].h); cputs(c, "\n");
        return 0;
    }
    int w = atoi(argv[1]); const char *x = strchr(argv[1], 'x'); int h = x ? atoi(x + 1) : 0;
    if (w < 320 || h < 200) { eprintf(c, "display: usage: display WIDTHxHEIGHT\n"); return 1; }
    bool live = g && g->modeset && wm_set_resolution(w, h);
    int saved = display_persist_resolution(w, h);
    if (live) cprintf(c, "switched to %dx%d%s\n", w, h, saved == 0 ? " (saved as boot resolution)" : "");
    else if (saved == 0) cprintf(c, "saved %dx%d as the boot resolution (takes effect at next boot)\n", w, h);
    else { eprintf(c, "display: cannot change mode at runtime and no installed limine.conf found\n"); return 1; }
    return 0;
}
static int b_lspci(struct ctx *c, int argc, char **argv) {
    for (int i = 0; i < pci_device_count(); i++) { struct pci_device *d = pci_device_at(i); cprintf(c, "%02x:%02x.%d %s [%02x%02x]: %04x:%04x\n", d->bus, d->slot, d->func, pci_class_name(d->class_code, d->subclass), d->class_code, d->subclass, d->vendor, d->device); }
    return 0;
}
/* ---- ACPI ---- */
static void acpi_line(void *ctx, const char *s) { cprintf(ctx, "%s\n", s); }
static int b_acpi(struct ctx *c, int argc, char **argv) {
    if (argc < 2 || !strcmp(argv[1], "info")) { acpi_print_info(acpi_line, c); return 0; }
    if (!strcmp(argv[1], "irq")) { acpi_print_irqs(acpi_line, c); return 0; }
    if (!strcmp(argv[1], "tree")) {
        if (acpi_print_tree(argc > 2 ? argv[2] : NULL, acpi_line, c) == 0) return 0;
        eprintf(c, "acpi: %s: no such object\n", argc > 2 ? argv[2] : "\\"); return 1;
    }
    if (!strcmp(argv[1], "eval") && argc > 2) {
        uint64_t args[7]; int n = 0;
        for (int i = 3; i < argc && n < 7; i++) args[n++] = strtoul(argv[i], NULL, 0);
        int r = acpi_print_eval(argv[2], n, args, acpi_line, c);
        if (r == -ENOENT) eprintf(c, "acpi: %s: no such object\n", argv[2]);
        else if (r < 0) eprintf(c, "acpi: evaluating %s failed (see the kernel log)\n", argv[2]);
        return r < 0;
    }
    cputs(c, "usage: acpi [info]          ACPI tables, mode, SCI, sleep states\n"
             "       acpi tree [path]     dump the AML namespace (e.g. acpi tree _SB.PCI0)\n"
             "       acpi eval path [n..] evaluate an object or run a method (e.g. acpi eval _S5)\n"
             "       acpi irq             PCI interrupt routing from _PRT\n");
    return 1;
}
/* ---- USB ---- */
static int b_lsusb(struct ctx *c, int argc, char **argv) {
    bool verbose = argc > 1 && !strcmp(argv[1], "-v");
    if (argc > 1 && !verbose) { cputs(c, "usage: lsusb [-v]\n"); return 1; }
    usb_print(verbose, acpi_line, c);
    return 0;
}
/* ---- sound ---- */
static int b_play(struct ctx *c, int argc, char **argv) {
    if (argc < 2) { cputs(c, "usage: play file.wav ...\n"); return 1; }
    int status = 0;
    for (int i = 1; i < argc && !cancelled(c); i++) {
        char p[PATH_MAX]; resolve(c->sh, argv[i], p, sizeof p);
        struct vnode *v = vfs_lookup(p);
        if (!v) { eprintf(c, "play: %s: no such file or directory\n", argv[i]); status = 1; continue; }
        struct wav_info w;
        int r = wav_parse(v, &w);
        if (r < 0) { eprintf(c, "play: %s: %s\n", argv[i], r == -ENOTSUP ? "unsupported WAV encoding (PCM or float only)" : "not a WAV file"); vnode_unref(v); status = 1; continue; }
        int err;
        struct audio_stream *s = audio_open(w.rate, w.channels, w.bits, w.is_float, &err);
        if (!s) { eprintf(c, "play: %s\n", err == -ENODEV ? "no sound hardware" : err == -EBUSY ? "the audio device is in use" : "unsupported format"); vnode_unref(v); return 1; }
        char desc[64]; wav_describe(&w, desc, sizeof desc);
        uint64_t total = w.frames / w.rate;
        cprintf(c, "%s: %s, %lu:%02lu\n", vfs_basename(p), desc, total / 60, total % 60);
        size_t fb = w.channels * w.bits / 8, cap = 65536 / fb * fb;
        uint8_t *buf = kmalloc(cap);
        uint64_t shown = ~0ULL;
        bool gone = false;
        for (uint64_t off = 0; off < w.data_len && !cancelled(c); ) {
            int64_t got = vfs_pread(v, w.data_off + off, buf, MIN((uint64_t)cap, w.data_len - off));
            if (got <= 0) break;
            got -= got % fb;
            if (!got) break;
            if (audio_stream_write(s, buf, got / fb, &c->sh->cancel) < 0) { gone = true; break; }   /* USB device unplugged */
            off += got;
            uint64_t sec = audio_stream_position(s) / w.rate;
            if (sec != shown) { cprintf(c, "\r  %lu:%02lu / %lu:%02lu", sec / 60, sec % 60, total / 60, total % 60); shown = sec; }
        }
        if (gone) { cputs(c, "\n"); eprintf(c, "play: the audio device was disconnected"); status = 1; }
        else {
            if (!cancelled(c)) audio_stream_drain(s, &c->sh->cancel);
            if (cancelled(c)) audio_stream_stop(s);
            else cprintf(c, "\r  %lu:%02lu / %lu:%02lu", total / 60, total % 60, total / 60, total % 60);
        }
        cputs(c, "\n");
        audio_stream_close(s);
        kfree(buf);
        vnode_unref(v);
    }
    return status;
}
static int b_volume(struct ctx *c, int argc, char **argv) {
    struct audio_device *d = audio_device();
    if (!d) { eprintf(c, "volume: no sound hardware\n"); return 1; }
    if (argc > 1) {
        if (!isdigit(argv[1][0])) { cputs(c, "usage: volume [0-100]\n"); return 1; }
        audio_set_volume(atoi(argv[1]));
    }
    cprintf(c, "%s\n%s\nvolume %d%%\n", d->name, d->detail, audio_volume());
    return 0;
}
/* ---- networking ---- */
static int b_ifconfig(struct ctx *c, int argc, char **argv) {
    if (argc >= 3) {
        /* ifconfig <if> <ip> [netmask <m>] [gw <g>] [dns <d>] | ifconfig <if> up|down */
        struct netif *n = netif_find(argv[1]);
        if (!n) { eprintf(c, "ifconfig: %s: no such interface\n", argv[1]); return 1; }
        if (!strcmp(argv[2], "up") || !strcmp(argv[2], "down")) { n->up = !strcmp(argv[2], "up"); return 0; }
        uint32_t ip, mask = 0xFFFFFF00, gw = 0, dns = 0;
        if (!str_to_ip(argv[2], &ip)) { eprintf(c, "ifconfig: %s: bad address\n", argv[2]); return 1; }
        for (int i = 3; i + 1 < argc; i += 2) {
            uint32_t v;
            if (!str_to_ip(argv[i + 1], &v)) { eprintf(c, "ifconfig: %s: bad address\n", argv[i + 1]); return 1; }
            if (!strcmp(argv[i], "netmask")) mask = v; else if (!strcmp(argv[i], "gw")) gw = v; else if (!strcmp(argv[i], "dns")) dns = v;
            else { eprintf(c, "ifconfig: unknown option %s\n", argv[i]); return 1; }
        }
        mutex_lock(&net_lock);
        n->ip = ip; n->netmask = mask; n->gateway = gw; n->dns = dns ? dns : gw; n->configured = true; n->dhcp = false; n->up = true;
        mutex_unlock(&net_lock);
        dns_flush();
        return 0;
    }
    if (!netif_count()) { cputs(c, "no network adapters (QEMU: -device ne2k_pci,netdev=n0 -netdev user,id=n0)\n"); return 1; }
    for (int i = 0; i < netif_count(); i++) {
        struct netif *n = netif_at(i);
        if (argc == 2 && strcmp(argv[1], n->name)) continue;
        char mac[18], ip[16], mask[16], gw[16], dns[16];
        mac_to_str(n->mac, mac); ip_to_str(n->ip, ip); ip_to_str(n->netmask, mask); ip_to_str(n->gateway, gw); ip_to_str(n->dns, dns);
        cprintf(c, "%s: <%s%s> %s\n", n->name, n->up ? "UP" : "DOWN", n->configured ? (n->dhcp ? ",DHCP" : ",STATIC") : ",UNCONFIGURED", n->driver);
        cprintf(c, "    ether %s\n", mac);
        if (n->configured) cprintf(c, "    inet %s  netmask %s  gateway %s  dns %s\n", ip, mask, gw, dns);
        cprintf(c, "    RX packets %lu  bytes %lu  errors %lu  dropped %lu\n", n->rx_packets, n->rx_bytes, n->rx_errors, n->rx_dropped);
        cprintf(c, "    TX packets %lu  bytes %lu  errors %lu\n", n->tx_packets, n->tx_bytes, n->tx_errors);
    }
    return 0;
}
static int b_dhcp(struct ctx *c, int argc, char **argv) {
    struct netif *n = argc > 1 ? netif_find(argv[1]) : netif_at(0);
    if (!n) { eprintf(c, "dhcp: no such interface\n"); return 1; }
    cprintf(c, "%s: requesting an address...\n", n->name);
    mutex_lock(&net_lock); n->configured = false; n->ip = 0; mutex_unlock(&net_lock);
    int r = dhcp_configure(n, 10000);
    if (r < 0) { eprintf(c, "dhcp: no lease (%s)\n", r == -ETIMEDOUT ? "no answer" : r == -ECONNREFUSED ? "refused" : "error"); return 1; }
    char ip[16], gw[16]; ip_to_str(n->ip, ip); ip_to_str(n->gateway, gw);
    cprintf(c, "%s: %s, gateway %s\n", n->name, ip, gw);
    return 0;
}
static int b_ping(struct ctx *c, int argc, char **argv) {
    int count = 4; const char *host = NULL;
    for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "-c") && i + 1 < argc) count = atoi(argv[++i]); else host = argv[i]; }
    if (!host) { cputs(c, "usage: ping [-c count] host\n"); return 1; }
    uint32_t ip;
    int r = dns_resolve(host, &ip, 5000, &c->sh->cancel);
    if (r < 0) { eprintf(c, "ping: %s: %s\n", host, r == -ENOENT ? "unknown host" : r == -ENETUNREACH ? "network unreachable" : "lookup failed"); return 1; }
    char ips[16]; ip_to_str(ip, ips);
    cprintf(c, "PING %s (%s): 56 data bytes\n", host, ips);
    int sent = 0, received = 0; int64_t min = -1, max = 0, total = 0;
    for (int seq = 0; count <= 0 || seq < count; seq++) {
        if (cancelled(c)) break;
        uint64_t start = time_ms();
        int ttl = 0;
        int64_t us = icmp_ping(ip, seq, 56, 2000, &ttl);
        sent++;
        if (us >= 0) {
            received++; total += us; if (min < 0 || us < min) min = us; if (us > max) max = us;
            cprintf(c, "64 bytes from %s: icmp_seq=%d ttl=%d time=%ld.%03ld ms\n", ips, seq, ttl, us / 1000, us % 1000);
        } else if (us == -ETIMEDOUT) cprintf(c, "Request timeout for icmp_seq %d\n", seq);
        else { eprintf(c, "ping: sendto: %s\n", us == -ENETUNREACH ? "network unreachable" : "error"); break; }
        while (time_ms() - start < 1000 && !cancelled(c) && (count <= 0 || seq + 1 < count)) thread_sleep_ms(20);
    }
    cprintf(c, "--- %s ping statistics ---\n%d packets transmitted, %d packets received, %d%% packet loss\n", host, sent, received, sent ? (sent - received) * 100 / sent : 0);
    if (received) cprintf(c, "round-trip min/avg/max = %ld.%03ld/%ld.%03ld/%ld.%03ld ms\n", min / 1000, min % 1000, total / received / 1000, total / received % 1000, max / 1000, max % 1000);
    return received ? 0 : 1;
}
static int b_nslookup(struct ctx *c, int argc, char **argv) {
    if (argc < 2) { cprintf(c, "usage: %s name\n", argv[0]); return 1; }
    struct netif *n = netif_default();
    if (n && n->configured) { char d[16]; ip_to_str(n->dns, d); cprintf(c, "Server:  %s\n\n", d); }
    uint32_t ip;
    int r = dns_resolve(argv[1], &ip, 8000, &c->sh->cancel);
    if (r < 0) { eprintf(c, "%s: %s: %s\n", argv[0], argv[1], r == -ENOENT ? "NXDOMAIN" : r == -ETIMEDOUT ? "timed out" : r == -ENETUNREACH ? "network unreachable" : "lookup failed"); return 1; }
    char ips[16]; ip_to_str(ip, ips);
    cprintf(c, "Name:    %s\nAddress: %s\n", argv[1], ips);
    return 0;
}
static void curl_progress(void *ctx, const char *stage, size_t done, size_t total) { UNUSED(ctx); UNUSED(stage); UNUSED(done); UNUSED(total); }
static int b_curl(struct ctx *c, int argc, char **argv) {
    const char *url = NULL, *out = NULL; bool head = false, include = false, location = true;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-I")) head = true;
        else if (!strcmp(argv[i], "-i")) include = true;
        else if (!strcmp(argv[i], "-L")) location = true;
        else if (!strcmp(argv[i], "-s")) ;
        else url = argv[i];
    }
    if (!url) { cputs(c, "usage: curl [-I] [-i] [-o file] http://host/path\n"); return 1; }
    char full[1024];
    if (!strstr(url, "://")) snprintf(full, sizeof full, "http://%s", url); else strlcpy(full, url, sizeof full);
    struct http_request req = { .url = full, .method = head ? "HEAD" : "GET", .cancel = &c->sh->cancel, .max_redirects = location ? -1 : 0, .progress = curl_progress };
    struct http_response resp;
    int r = http_request(&req, &resp);
    if (r < 0) { eprintf(c, "curl: %s\n", resp.error); http_response_free(&resp); return 1; }
    if (head || include) { cprintf(c, "HTTP/1.1 %d %s\n", resp.status, resp.status_text); for (char *p = resp.headers; p && *p; p++) if (*p != '\r') cwrite(c, p, 1); }
    if (!head) {
        if (out) {
            char p[PATH_MAX]; resolve(c->sh, out, p, sizeof p);
            if (vfs_write_file(p, resp.body, resp.body_len) < 0) { eprintf(c, "curl: cannot write %s\n", out); http_response_free(&resp); return 1; }
            cprintf(c, "%zu bytes saved to %s\n", resp.body_len, p);
        } else cwrite(c, (const char *)resp.body, resp.body_len);
    }
    http_response_free(&resp);
    return 0;
}
static int b_arp(struct ctx *c, int argc, char **argv) {
    struct arp_entry_info e[64]; int n = arp_list(e, 64);
    for (int i = 0; i < n; i++) { char ip[16], mac[18]; ip_to_str(e[i].ip, ip); mac_to_str(e[i].mac, mac); cprintf(c, "%-15s  %s  %lus\n", ip, e[i].resolved ? mac : "(incomplete)    ", e[i].age_ms / 1000); }
    return 0;
}
static int b_netstat(struct ctx *c, int argc, char **argv) {
    struct tcp_conn_info t[64]; int n = tcp_list(t, 64);
    cputs(c, "Proto Recv-Q Send-Q Local Address          Foreign Address        State\n");
    for (int i = 0; i < n; i++) {
        char l[32], r[32], a[16];
        ip_to_str(t[i].lip, a); snprintf(l, sizeof l, "%s:%u", a, t[i].lport);
        ip_to_str(t[i].rip, a); snprintf(r, sizeof r, "%s:%u", a, t[i].rport);
        cprintf(c, "tcp   %6u %6u %-22s %-22s %s\n", t[i].rx_queued, t[i].tx_queued, l, r, t[i].state);
    }
    return 0;
}
static int b_date(struct ctx *c, int argc, char **argv) {
    static const char *wd[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" }; static const char *mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    uint64_t t = time_unix(); struct rtc_time d; unix_to_datetime(t + tz_offset_minutes * 60, &d);
    if (argc > 1 && !strcmp(argv[1], "+%s")) { cprintf(c, "%lu\n", t); return 0; }
    cprintf(c, "%s %s %2d %02d:%02d:%02d %s%02ld%02ld %d\n", wd[day_of_week(d.year, d.month, d.day)], mon[(d.month - 1) % 12], d.day, d.hour, d.minute, d.second, tz_offset_minutes < 0 ? "-" : "+", (tz_offset_minutes < 0 ? -tz_offset_minutes : tz_offset_minutes) / 60, (tz_offset_minutes < 0 ? -tz_offset_minutes : tz_offset_minutes) % 60, d.year);
    return 0;
}
static int b_uptime(struct ctx *c, int argc, char **argv) {
    uint64_t s = system_ticks / 1000; struct rtc_time d; unix_to_datetime(time_unix() + tz_offset_minutes * 60, &d);
    sched_sample_cpu();
    cprintf(c, "%02d:%02d:%02d  up %lu day%s, %lu:%02lu, 1 user, cpu %d%%\n", d.hour, d.minute, d.second, s / 86400, s / 86400 == 1 ? "" : "s", (s % 86400) / 3600, (s % 3600) / 60, 100 - sched_idle_percent());
    return 0;
}
static int b_sleep(struct ctx *c, int argc, char **argv) {
    if (argc < 2) return 1;
    long ms = strtol(argv[1], NULL, 10) * 1000; char *dot = strchr(argv[1], '.'); if (dot) { int f = atoi(dot + 1); while (f >= 1000) f /= 10; int digits = strlen(dot + 1); if (digits == 1) f *= 100; else if (digits == 2) f *= 10; ms += f; }
    uint64_t end = system_ticks + ms;
    while (system_ticks < end && !cancelled(c)) thread_sleep_ms(MIN(50, end - system_ticks));
    return 0;
}
static int b_history(struct ctx *c, int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "-c")) { for (int i = 0; i < c->sh->nhist; i++) kfree(c->sh->history[i]); c->sh->nhist = 0; vfs_write_file("/home/flux/.zsh_history", "", 0); return 0; }
    int start = 0; if (argc > 1) { int n = atoi(argv[1]); if (n > 0 && n < c->sh->nhist) start = c->sh->nhist - n; }
    for (int i = start; i < c->sh->nhist; i++) cprintf(c, "%5d  %s\n", i + 1, c->sh->history[i]);
    return 0;
}
static int b_alias(struct ctx *c, int argc, char **argv) {
    if (argc == 1) { for (int i = 0; i < c->sh->naliases; i++) cprintf(c, "%s='%s'\n", c->sh->aliases[i].name, c->sh->aliases[i].value); return 0; }
    for (int i = 1; i < argc; i++) { char *eq = strchr(argv[i], '='); if (!eq) { const char *v = alias_get(c->sh, argv[i]); if (v) cprintf(c, "%s='%s'\n", argv[i], v); else { eprintf(c, "alias: %s not found\n", argv[i]); } continue; } *eq = 0; alias_set(c->sh, argv[i], eq + 1); }
    return 0;
}
static int b_unalias(struct ctx *c, int argc, char **argv) {
    for (int i = 1; i < argc; i++) for (int j = 0; j < c->sh->naliases; j++) if (!strcmp(c->sh->aliases[j].name, argv[i])) { kfree(c->sh->aliases[j].name); kfree(c->sh->aliases[j].value); c->sh->aliases[j] = c->sh->aliases[--c->sh->naliases]; break; }
    return 0;
}
static int b_export(struct ctx *c, int argc, char **argv) {
    if (argc == 1) { for (int i = 0; i < c->sh->nenv; i++) cprintf(c, "%s\n", c->sh->env[i]); return 0; }
    for (int i = 1; i < argc; i++) { char *eq = strchr(argv[i], '='); if (eq) { *eq = 0; env_set(c->sh, argv[i], eq + 1); } else if (!env_get(c->sh, argv[i])) env_set(c->sh, argv[i], ""); }
    return 0;
}
static int b_unset(struct ctx *c, int argc, char **argv) { for (int i = 1; i < argc; i++) env_unset(c->sh, argv[i]); return 0; }
static int b_env(struct ctx *c, int argc, char **argv) { for (int i = 0; i < c->sh->nenv; i++) cprintf(c, "%s\n", c->sh->env[i]); return 0; }
static int b_which(struct ctx *c, int argc, char **argv) {
    int status = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = alias_get(c->sh, argv[i]);
        if (a) { cprintf(c, "%s: aliased to %s\n", argv[i], a); continue; }
        if (find_builtin(argv[i])) { cprintf(c, "%s: shell built-in command\n", argv[i]); continue; }
        char p[PATH_MAX]; snprintf(p, sizeof p, "/bin/%s", argv[i]); struct stat st;
        if (vfs_stat(p, &st) == 0) cprintf(c, "%s\n", p); else { cprintf(c, "%s not found\n", argv[i]); status = 1; }
    }
    return status;
}
static int b_true(struct ctx *c, int argc, char **argv) { return 0; }
static int b_false(struct ctx *c, int argc, char **argv) { return 1; }
static int b_exit(struct ctx *c, int argc, char **argv) { c->sh->exiting = true; return argc > 1 ? atoi(argv[1]) : c->sh->last_status; }
static int b_hexdump(struct ctx *c, int argc, char **argv) {
    const char *file = NULL; long limit = -1;
    for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "-n") && i + 1 < argc) limit = strtol(argv[++i], NULL, 0); else if (strcmp(argv[i], "-C")) file = argv[i]; }
    struct ctx fc = *c; struct file *f = NULL;
    if (file) { char p[PATH_MAX]; resolve(c->sh, file, p, sizeof p); int r = vfs_open(p, O_RDONLY, &f); if (r < 0) { eprintf(c, "hexdump: %s: %s\n", file, errstr(r)); return 1; } fc.in = f; fc.rlen = fc.rpos = 0; }
    unsigned char row[16]; int n = 0, ch; uint64_t off = 0;
    for (;;) {
        n = 0; while (n < 16 && (limit < 0 || (long)(off + n) < limit) && (ch = cgetc(&fc)) >= 0) row[n++] = ch;
        if (!n) break;
        cprintf(c, "%08lx  ", off);
        for (int i = 0; i < 16; i++) { if (i < n) cprintf(c, "%02x ", row[i]); else cputs(c, "   "); if (i == 7) cputs(c, " "); }
        cputs(c, " |"); for (int i = 0; i < n; i++) cprintf(c, "%c", isprint(row[i]) ? row[i] : '.'); cputs(c, "|\n");
        off += n; if (n < 16) break;
    }
    if (f) vfs_close(f); return 0;
}
static int b_stat(struct ctx *c, int argc, char **argv) {
    int status = 0;
    for (int i = 1; i < argc; i++) {
        char p[PATH_MAX]; resolve(c->sh, argv[i], p, sizeof p); struct stat st;
        if (vfs_stat(p, &st) < 0) { eprintf(c, "stat: %s: no such file or directory\n", argv[i]); status = 1; continue; }
        struct rtc_time d; unix_to_datetime(st.mtime + tz_offset_minutes * 60, &d);
        cprintf(c, "  File: %s\n  Size: %-12lu Type: %s\n Inode: %-12lu Mode: %04o\nModify: %04d-%02d-%02d %02d:%02d:%02d\n", p, st.size, st.type == VN_DIR ? "directory" : st.type == VN_FILE ? "regular file" : st.type == VN_BLK ? "block device" : "character device", st.ino, st.mode, d.year, d.month, d.day, d.hour, d.minute, d.second);
    }
    return status;
}
static int b_open(struct ctx *c, int argc, char **argv) {
    if (argc < 2) { app_launch(APP_FILES, c->sh->proc->cwd); return 0; }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-a") && i + 2 < argc) { const char *app = argv[i + 1]; char p[PATH_MAX]; resolve(c->sh, argv[i + 2], p, sizeof p); for (int k = 0; k < APP_COUNT; k++) if (!strcasecmp(app_table[k].name, app)) app_launch(k, p); i += 2; continue; }
        if (!strcmp(argv[i], "-a") && i + 1 < argc) { for (int k = 0; k < APP_COUNT; k++) if (!strcasecmp(app_table[k].name, argv[i + 1])) app_launch(k, NULL); i++; continue; }
        char p[PATH_MAX]; resolve(c->sh, argv[i], p, sizeof p); struct stat st;
        if (vfs_stat(p, &st) < 0) { eprintf(c, "open: %s: no such file or directory\n", argv[i]); return 1; }
        app_open_path(p);
    }
    return 0;
}
static int b_sync(struct ctx *c, int argc, char **argv) { vfs_sync(); return 0; }
static int b_reboot(struct ctx *c, int argc, char **argv) { power_restart(); }
static int b_shutdown(struct ctx *c, int argc, char **argv) { power_shutdown(); }
static int b_basename(struct ctx *c, int argc, char **argv) { if (argc < 2) return 1; char p[PATH_MAX]; strlcpy(p, argv[1], sizeof p); size_t l = strlen(p); while (l > 1 && p[l - 1] == '/') p[--l] = 0; cprintf(c, "%s\n", vfs_basename(p)); return 0; }
static int b_dirname(struct ctx *c, int argc, char **argv) { if (argc < 2) return 1; char d[PATH_MAX]; vfs_dirname(argv[1], d, sizeof d); cprintf(c, "%s\n", d); return 0; }
static void tree_walk(struct ctx *c, const char *path, const char *prefix, int depth, int *dirs, int *files) {
    struct vnode *d = vfs_lookup(path); if (!d) return;
    struct dirent *ents = kmalloc(256 * sizeof *ents); int n = 0;   /* 75 KiB: not on the stack of a recursive function */
    for (int i = 0; n < 256 && vfs_readdir(d, i, &ents[n]) == 0; i++) if (ents[n].name[0] != '.') n++;
    vnode_unref(d);
    for (int i = 0; i < n && !cancelled(c); i++) {
        bool last = i == n - 1;
        cprintf(c, "%s%s%s%s%s\n", prefix, last ? "\u2514\u2500\u2500 " : "\u251c\u2500\u2500 ", ents[i].type == VN_DIR ? "\033[1;34m" : "", ents[i].name, ents[i].type == VN_DIR ? "\033[0m" : "");
        if (ents[i].type == VN_DIR) { (*dirs)++; if (depth < 8) { char sub[PATH_MAX], np[256]; snprintf(sub, sizeof sub, "%s/%s", !strcmp(path, "/") ? "" : path, ents[i].name); snprintf(np, sizeof np, "%s%s", prefix, last ? "    " : "\u2502   "); tree_walk(c, sub, np, depth + 1, dirs, files); } }
        else (*files)++;
    }
    kfree(ents);
}
static int b_tree(struct ctx *c, int argc, char **argv) {
    char p[PATH_MAX]; resolve(c->sh, argc > 1 ? argv[1] : ".", p, sizeof p);
    int dirs = 0, files = 0; cprintf(c, "\033[1;34m%s\033[0m\n", argc > 1 ? argv[1] : "."); tree_walk(c, p, "", 0, &dirs, &files);
    cprintf(c, "\n%d director%s, %d file%s\n", dirs, dirs == 1 ? "y" : "ies", files, files == 1 ? "" : "s");
    return 0;
}
static void find_walk(struct ctx *c, const char *path, const char *pat, int depth) {
    struct vnode *d = vfs_lookup(path); if (!d) return; struct dirent de;
    for (int i = 0; vfs_readdir(d, i, &de) == 0; i++) {
        char sub[PATH_MAX]; snprintf(sub, sizeof sub, "%s/%s", !strcmp(path, "/") ? "" : path, de.name);
        if (!pat || glob_match(pat, de.name)) cprintf(c, "%s\n", sub);
        if (de.type == VN_DIR && depth < 16 && strcmp(de.name, ".") && strcmp(de.name, "..")) find_walk(c, sub, pat, depth + 1);
        if (cancelled(c)) break;
    }
    vnode_unref(d);
}
static int b_find(struct ctx *c, int argc, char **argv) {
    const char *start = "."; const char *pat = NULL;
    for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "-name") && i + 1 < argc) pat = argv[++i]; else if (argv[i][0] != '-') start = argv[i]; }
    char p[PATH_MAX]; resolve(c->sh, start, p, sizeof p);
    if (!pat) cprintf(c, "%s\n", start);
    find_walk(c, p, pat, 0); return 0;
}
static int du_walk(struct ctx *c, const char *path, int depth, uint64_t *total) {
    struct stat st; if (vfs_stat(path, &st) < 0) return 0;
    if (st.type != VN_DIR) { *total += st.size; return 0; }
    if (depth >= WALK_DEPTH) return -ETOODEEP;
    struct vnode *d = vfs_lookup(path); struct walk *w = kmalloc(sizeof *w); int r = 0;
    for (int i = 0; d && !r && vfs_readdir(d, i, &w->de) == 0; i++) r = cancelled(c) ? -ECANCELED : !path_join(w->a, path, w->de.name) ? -ENAMETOOLONG : du_walk(c, w->a, depth + 1, total);
    kfree(w); if (d) vnode_unref(d);
    return r;
}
static int du_one(struct ctx *c, const char *path, const char *shown, bool h) {
    uint64_t t = 0; int r = du_walk(c, path, 0, &t);
    if (r < 0) eprintf(c, "du: %s: %s\n", shown, errstr(r));
    char s[16]; if (h) format_size_h(t, s, 16); else snprintf(s, 16, "%lu", (t + 1023) / 1024);
    cprintf(c, "%s\t%s\n", s, shown);
    return r < 0;
}
static int b_du(struct ctx *c, int argc, char **argv) {
    bool h = false; int n = 0, status = 0;
    for (int i = 1; i < argc; i++) { if (argv[i][0] == '-') { if (strchr(argv[i], 'h')) h = true; continue; } char p[PATH_MAX]; resolve(c->sh, argv[i], p, sizeof p); status |= du_one(c, p, argv[i], h); n++; }
    if (!n) status = du_one(c, c->sh->proc->cwd, ".", h);
    return status;
}
static int b_whoami(struct ctx *c, int argc, char **argv) { cputs(c, "flux\n"); return 0; }
static int b_hostname(struct ctx *c, int argc, char **argv) { if (argc > 1) strlcpy(system_hostname, argv[1], sizeof system_hostname); else cprintf(c, "%s\n", system_hostname); return 0; }
static int b_id(struct ctx *c, int argc, char **argv) { cputs(c, "uid=501(flux) gid=20(staff) groups=20(staff),80(admin)\n"); return 0; }
static int b_nproc(struct ctx *c, int argc, char **argv) { cputs(c, "1\n"); return 0; }
static int b_source(struct ctx *c, int argc, char **argv) {
    if (argc < 2) return 1;
    char p[PATH_MAX]; resolve(c->sh, argv[1], p, sizeof p); void *data; size_t sz;
    if (vfs_read_file(p, &data, &sz) < 0) { eprintf(c, "source: no such file: %s\n", argv[1]); return 1; }
    if (c->sh->depth >= 3) { kfree(data); eprintf(c, "source: nested too deeply\n"); return 1; }   /* each level costs a run_line frame on the 128 KiB stack */
    c->sh->depth++;
    char *save, *line = strtok_r(data, "\n", &save); int st = 0;
    while (line && !cancelled(c)) { st = run_line(c->sh, c, line); line = strtok_r(NULL, "\n", &save); }
    c->sh->depth--;
    kfree(data); return st;
}
static int b_fetch(struct ctx *c, int argc, char **argv) {
    char brand[64]; cpu_get_brand(brand, sizeof brand);
    uint64_t s = system_ticks / 1000;
    struct process *list[64]; int np = process_list(list, 64);
    const char *logo[] = {
        "\033[1;36m  ████████████  \033[0m", "\033[1;36m  ██        ██  \033[0m", "\033[1;36m  ██   ██   ██  \033[0m", "\033[1;36m  ██  ████  ██  \033[0m",
        "\033[1;36m  ██  ██ ██  ██ \033[0m", "\033[1;36m  ██  ██  ██ ██ \033[0m", "\033[1;36m  ████     ████ \033[0m", "                 " };
    char lines[8][128];
    snprintf(lines[0], 128, "\033[1;36mflux\033[0m@\033[1;36m%s\033[0m", system_hostname);
    snprintf(lines[1], 128, "\033[1mOS:\033[0m inFlux %s x86_64", INFLUX_VERSION);
    snprintf(lines[2], 128, "\033[1mKernel:\033[0m inFlux %s (Limine)", INFLUX_VERSION);
    snprintf(lines[3], 128, "\033[1mUptime:\033[0m %lu min, %lu sec", s / 60, s % 60);
    snprintf(lines[4], 128, "\033[1mShell:\033[0m zsh (inFlux)  \033[1mProcs:\033[0m %d", np);
    snprintf(lines[5], 128, "\033[1mResolution:\033[0m %dx%d", wm_screen_w(), wm_screen_h());
    snprintf(lines[6], 128, cpu_count > 1 ? "\033[1mCPU:\033[0m %s (%d)" : "\033[1mCPU:\033[0m %s", brand, cpu_count);
    snprintf(lines[7], 128, "\033[1mMemory:\033[0m %lu MiB / %lu MiB", pmm_used_bytes() >> 20, pmm_total_bytes() >> 20);
    for (int i = 0; i < 8; i++) cprintf(c, "%s  %s\n", logo[i], lines[i]);
    cputs(c, "\n                   ");
    for (int i = 0; i < 8; i++) cprintf(c, "\033[4%dm   ", i); cputs(c, "\033[0m\n                   ");
    for (int i = 0; i < 8; i++) cprintf(c, "\033[10%dm   ", i); cputs(c, "\033[0m\n");
    return 0;
}
static int b_help(struct ctx *c, int argc, char **argv) {
    if (argc > 1) { const struct builtin *b = find_builtin(argv[1]); if (b) cprintf(c, "%s: %s\n", b->name, b->help); else cprintf(c, "help: no help for %s\n", argv[1]); return 0; }
    cputs(c, "\033[1minFlux shell\033[0m \u2014 zsh-like built-ins:\n\n");
    for (int i = 0; i < nbuiltins; i++) cprintf(c, "  \033[1;32m%-10s\033[0m %s\n", builtins[i].name, builtins[i].help);
    cputs(c, "\nExternal programs are run from /bin. Supports pipes (|), redirection (>, >>, <, 2>),\n&& and || chaining, aliases, $VAR expansion, globbing (*, ?), Tab completion and \u2191/\u2193 history.\n");
    return 0;
}
static int b_type(struct ctx *c, int argc, char **argv) { return b_which(c, argc, argv); }

static const struct builtin builtins[] = {
    { "acpi", b_acpi, "ACPI information, namespace dump and evaluation" }, { "alias", b_alias, "define or list aliases: alias ll='ls -l'" }, { "arp", b_arp, "show the ARP cache" }, { "basename", b_basename, "strip directory from a path" },
    { "cat", b_cat, "concatenate files (-n numbers lines)" }, { "cd", b_cd, "change directory (cd -, cd ~)" },
    { "clear", b_clear, "clear the screen" }, { "cp", b_cp, "copy files (-r for directories)" }, { "curl", b_curl, "fetch a URL over HTTP (-I headers, -i, -o file)" },
    { "date", b_date, "show the current date and time" }, { "df", b_df, "show filesystem usage (-h)" }, { "dhcp", b_dhcp, "request an address for an adapter" },
    { "dirname", b_dirname, "strip the last component from a path" }, { "display", b_display, "show or set the display resolution" }, { "du", b_du, "disk usage (-h)" },
    { "echo", b_echo, "print arguments (-n, -e)" }, { "env", b_env, "list environment variables" },
    { "exit", b_exit, "exit the shell" }, { "export", b_export, "set environment variables" },
    { "false", b_false, "return failure" }, { "fetch", b_fetch, "show system information" },
    { "find", b_find, "list files recursively (-name pattern)" }, { "free", b_free, "show memory usage (-h)" },
    { "grep", b_grep, "search for a pattern (-i -v -c -n)" }, { "head", b_head, "first lines of input (-n N)" },
    { "help", b_help, "this list" }, { "hexdump", b_hexdump, "hex dump of a file (-n bytes)" },
    { "history", b_history, "show command history (-c clears)" }, { "host", b_nslookup, "look up a host name" }, { "hostname", b_hostname, "print the host name" },
    { "id", b_id, "print user identity" }, { "ifconfig", b_ifconfig, "show or configure network adapters" }, { "install", b_install, "install inFlux to a disk, erasing it (asks first; -y doesn't)" }, { "kill", b_kill, "terminate a process: kill [-9] pid" },
    { "ls", b_ls, "list directory contents (-l -a -h)" }, { "lsblk", b_lsblk, "list block devices" }, { "lsgpu", b_lsgpu, "show graphics adapters" },
    { "lspci", b_lspci, "list PCI devices" }, { "lsusb", b_lsusb, "list USB devices (-v: interfaces and endpoints)" }, { "mkdir", b_mkdir, "create directories (-p)" },
    { "mount", b_mount, "list mounted filesystems" }, { "mv", b_mv, "move or rename files" }, { "netstat", b_netstat, "list TCP connections" },
    { "nproc", b_nproc, "number of processors" }, { "nslookup", b_nslookup, "query DNS for a host name" }, { "open", b_open, "open a file or folder in the desktop (-a App)" },
    { "ping", b_ping, "send ICMP echo requests (-c count)" }, { "play", b_play, "play WAV files (Ctrl+C stops)" }, { "printf", b_printf, "formatted output" }, { "ps", b_ps, "list processes (-T shows threads)" },
    { "pwd", b_pwd, "print the working directory" }, { "reboot", b_reboot, "restart the computer" },
    { "rev", b_rev, "reverse each line" }, { "rm", b_rm, "remove files (-r -f)" },
    { "rmdir", b_rmdir, "remove empty directories" }, { "seq", b_seq, "print a sequence of numbers" },
    { "shutdown", b_shutdown, "power off the computer" }, { "sleep", b_sleep, "pause for N seconds" },
    { "sort", b_sort, "sort lines (-r -n -u)" }, { "source", b_source, "run commands from a file" },
    { "stat", b_stat, "show file status" }, { "sync", b_sync, "flush filesystem buffers to disk" },
    { "tail", b_tail, "last lines of input (-n N)" }, { "tee", b_tee, "copy input to a file and stdout" },
    { "touch", b_touch, "create files or update timestamps" }, { "tree", b_tree, "show a directory tree" },
    { "true", b_true, "return success" }, { "type", b_type, "describe a command" },
    { "unalias", b_unalias, "remove an alias" }, { "volume", b_volume, "show the sound output, set the volume (0-100)" }, { "uname", b_uname, "system information (-a)" },
    { "uniq", b_uniq, "filter adjacent duplicate lines (-c)" }, { "unset", b_unset, "remove environment variables" },
    { "uptime", b_uptime, "show uptime and load" }, { "wc", b_wc, "count lines, words and bytes" },
    { "which", b_which, "locate a command" }, { "whoami", b_whoami, "print the current user" },
};

/* ---- execution ---- */
struct stage { int argc; char *argv[MAX_ARGS]; char *redir_out, *redir_in, *redir_err; bool append, err2out; };
struct job { struct ctx ctx; const struct builtin *b; int argc; char **argv; struct semaphore done; int status; bool detached; };

static void job_close(struct job *j) { if (j->ctx.in) vfs_close(j->ctx.in); if (j->ctx.out) vfs_close(j->ctx.out); if (j->ctx.err) vfs_close(j->ctx.err); j->ctx.in = j->ctx.out = j->ctx.err = NULL; }
static void job_thread(void *p) {
    struct job *j = p;
    j->status = j->b->fn(&j->ctx, j->argc, j->argv);
    job_close(j);
    if (!j->detached) { sem_post(&j->done); return; }
    /* background job: nobody waits, it owns its copy of the job and argv */
    struct shell *sh = j->ctx.sh;
    for (int i = 0; i < j->argc; i++) kfree(j->argv[i]);
    kfree(j->argv); kfree(j);
    __atomic_sub_fetch(&sh->bg_jobs, 1, __ATOMIC_SEQ_CST);   /* last access to sh: shell_main frees it once no job is left */
}

static int run_pipeline(struct shell *sh, struct ctx *base, struct stage *st, int n, bool background) {
    struct file *in = base->in ? vfs_dup(base->in) : NULL;
    struct job *jobs = kzalloc(16 * sizeof *jobs); int pids[16]; int njobs = 0; int status = 0; const char *lastname = NULL;   /* heap: keeps nested `source` off the stack */
    if (!background) tty_set_raw(sh->tty, false);   /* builtins and programs reading the terminal get line editing, echo and ^C */
    for (int i = 0; i < n; i++) {
        struct stage *s = &st[i];
        if (!s->argc) continue;
        struct file *out = NULL, *err = base->err ? vfs_dup(base->err) : NULL, *next_in = NULL;
        bool last = i == n - 1;
        if (!last) { struct file *r, *w; pipe_create(&r, &w); out = w; next_in = r; }
        else out = base->out ? vfs_dup(base->out) : NULL;
        if (s->redir_in) { char p[PATH_MAX]; resolve(sh, s->redir_in, p, sizeof p); struct file *f; int r = vfs_open(p, O_RDONLY, &f); if (r < 0) { eprintf(base, "zsh: no such file or directory: %s\n", s->redir_in); status = 1; if (out) vfs_close(out); if (err) vfs_close(err); if (next_in) vfs_close(next_in); break; } if (in) vfs_close(in); in = f; }
        if (s->redir_out) { char p[PATH_MAX]; resolve(sh, s->redir_out, p, sizeof p); struct file *f; int r = vfs_open(p, O_WRONLY | O_CREAT | (s->append ? O_APPEND : O_TRUNC), &f); if (r < 0) { eprintf(base, "zsh: %s: %s\n", s->redir_out, errstr(r)); status = 1; if (out) vfs_close(out); if (err) vfs_close(err); if (next_in) vfs_close(next_in); break; } if (out) vfs_close(out); out = f; }
        if (s->redir_err) { char p[PATH_MAX]; resolve(sh, s->redir_err, p, sizeof p); struct file *f; if (vfs_open(p, O_WRONLY | O_CREAT | O_TRUNC, &f) == 0) { if (err) vfs_close(err); err = f; } }
        if (s->err2out && out) { if (err) vfs_close(err); err = vfs_dup(out); }
        const struct builtin *b = find_builtin(s->argv[0]);
        if (b) {
            struct job *j = &jobs[njobs];
            j->ctx = (struct ctx){ sh, in, out, err }; j->b = b; j->argc = s->argc; j->argv = s->argv;
            sem_init(&j->done, 0); pids[njobs] = 0;
            in = out = err = NULL;   /* the job owns these now */
            if (n == 1 && !background) { j->status = b->fn(&j->ctx, s->argc, s->argv); job_close(j); sem_post(&j->done); }
            else if (background) {
                /* run_pipeline's frame and the stage argv die before the job does: give it its own copies */
                struct job *bj = kmalloc(sizeof *bj); *bj = *j; bj->detached = true; sem_init(&bj->done, 0);
                bj->argv = kzalloc((s->argc + 1) * sizeof(char *));
                for (int k = 0; k < s->argc; k++) bj->argv[k] = strdup(s->argv[k]);
                j->ctx.in = j->ctx.out = j->ctx.err = NULL; j->b = NULL;
                __atomic_add_fetch(&sh->bg_jobs, 1, __ATOMIC_SEQ_CST);   /* it uses sh: shell_main waits for it */
                thread_create(sh->proc, b->name, job_thread, bj);
            }
            else { thread_create(sh->proc, b->name, job_thread, j); }
            lastname = s->argv[0]; njobs++;
        } else {
            char p[PATH_MAX];
            if (strchr(s->argv[0], '/')) resolve(sh, s->argv[0], p, sizeof p); else snprintf(p, sizeof p, "/bin/%s", s->argv[0]);
            struct stat stt;
            if (vfs_stat(p, &stt) < 0 || stt.type != VN_FILE) {
                eprintf(base, "zsh: command not found: %s\n", s->argv[0]); status = 127;
                if (in) vfs_close(in); if (out) vfs_close(out); if (err) vfs_close(err);
                in = next_in; continue;
            }
            struct file *stdio[3] = { in, out, err };
            int pid = process_spawn(p, s->argc, s->argv, stdio, sh->proc->cwd);
            if (pid < 0) { eprintf(base, "zsh: %s: %s\n", s->argv[0], pid == -ENOEXEC ? "exec format error" : errstr(pid)); status = 126; }
            else { if (!background && last) sh->tty->fg_pid = pid; pids[njobs] = pid; jobs[njobs].b = NULL; lastname = s->argv[0]; njobs++; }
        }
        if (in) vfs_close(in); if (out) vfs_close(out); if (err) vfs_close(err);
        in = next_in;
    }
    if (in) vfs_close(in);
    /* a builtin runs as a thread of the shell and has no pid of its own: name it instead */
    if (background) { if (njobs) { if (pids[njobs - 1]) cprintf(base, "[1] %d\n", pids[njobs - 1]); else cprintf(base, "[1] %s\n", lastname); } kfree(jobs); return 0; }
    for (int i = 0; i < njobs; i++) {
        if (jobs[i].b) { sem_wait(&jobs[i].done); status = jobs[i].status; }
        else { int s2 = 0; process_wait(pids[i], &s2); status = s2; }
    }
    kfree(jobs);
    sh->tty->fg_pid = 0;
    tty_set_raw(sh->tty, true);
    return status;
}

static int run_line(struct shell *sh, struct ctx *c, const char *line) {
    struct token toks[256]; int nt = tokenize(line, toks, 256);
    if (!nt) return sh->last_status;
    int i = 0, status = 0; int connector = 0;   /* 0 none, 1 &&, 2 || */
    bool skip = false;
    struct stage *st = kmalloc(16 * sizeof *st);   /* 17 KiB: heap, so nested `source` fits the stack */
    while (i < nt && !sh->exiting && !thread_should_exit()) {   /* killed: start nothing more */
        int ns = 0; memset(st, 0, 16 * sizeof *st);
        bool background = false;
        /* parse one pipeline */
        while (i < nt && toks[i].type != T_SEMI && toks[i].type != T_AND && toks[i].type != T_OR && toks[i].type != T_AMP) {
            struct token *t = &toks[i];
            if (t->type == T_PIPE) { if (ns < 15) ns++; i++; continue; }
            struct stage *s = &st[ns];
            if (t->type == T_GT || t->type == T_GTGT || t->type == T_LT || t->type == T_ERRGT) {
                if (i + 1 < nt && toks[i + 1].type == T_WORD) { char buf[LINE_MAX]; expand_vars(sh, toks[i + 1].text, buf, sizeof buf); if (buf[0] == '~') { char tb[LINE_MAX]; snprintf(tb, sizeof tb, "/home/flux%s", buf + 1); strcpy(buf, tb); }
                    if (t->type == T_LT) s->redir_in = strdup(buf); else if (t->type == T_ERRGT) s->redir_err = strdup(buf); else { s->redir_out = strdup(buf); s->append = t->type == T_GTGT; } i += 2; }
                else { eprintf(c, "zsh: parse error near redirection\n"); i++; }
                continue;
            }
            if (t->type == T_ERR2OUT) { s->err2out = true; i++; continue; }
            if (t->type == T_WORD) {
                if (s->argc == 0 && !t->quoted) {
                    const char *a = alias_get(sh, t->text);
                    if (a && sh->depth < 8) {
                        struct token at[64]; int na = tokenize(a, at, 64);
                        for (int k = 0; k < na; k++) { if (at[k].type == T_WORD) s->argc = expand_word(sh, &at[k], s->argv, s->argc); kfree(at[k].text); }
                        i++; continue;
                    }
                    /* VAR=value assignment */
                    char *eq = strchr(t->text, '='); if (eq && eq != t->text && (i + 1 >= nt || toks[i + 1].type != T_WORD || true)) { bool valid = true; for (char *p = t->text; p < eq; p++) if (!isalnum(*p) && *p != '_') valid = false; if (valid && s->argc == 0) { char val[LINE_MAX]; expand_vars(sh, eq + 1, val, sizeof val); *eq = 0; env_set(sh, t->text, val); i++; continue; } }
                }
                s->argc = expand_word(sh, t, s->argv, s->argc);
            }
            i++;
        }
        if (i < nt) { if (toks[i].type == T_AMP) background = true; }
        int n = ns + 1;
        if (!skip && (st[0].argc || n > 1)) {
            sh->cancel = false;
            status = run_pipeline(sh, c, st, n, background);
            sh->last_status = status;
        }
        for (int k = 0; k < n; k++) { for (int a = 0; a < st[k].argc; a++) kfree(st[k].argv[a]); kfree(st[k].redir_out); kfree(st[k].redir_in); kfree(st[k].redir_err); }
        if (i < nt) { connector = toks[i].type == T_AND ? 1 : toks[i].type == T_OR ? 2 : 0; i++; }
        skip = (connector == 1 && status != 0) || (connector == 2 && status == 0);
        if (!skip) connector = 0;
    }
    kfree(st);
    for (int k = 0; k < nt; k++) kfree(toks[k].text);
    return status;
}

/* ---- line editing ---- */
static void prompt_string(struct shell *sh, char *out, size_t n) {
    const char *cwd = sh->proc->cwd; char disp[PATH_MAX];
    if (!strncmp(cwd, "/home/flux", 10) && (cwd[10] == 0 || cwd[10] == '/')) snprintf(disp, sizeof disp, "~%s", cwd + 10); else strlcpy(disp, cwd, sizeof disp);
    snprintf(out, n, "\033[1;32mflux@%s\033[0m \033[1;34m%s\033[0m %% ", system_hostname, disp);
}
static int visible_len(const char *s) { int n = 0; while (*s) { if (*s == 27) { while (*s && *s != 'm') s++; if (*s) s++; continue; } s++; n++; } return n; }
static void redraw_line(struct shell *sh) {
    char prompt[PATH_MAX + 64]; prompt_string(sh, prompt, sizeof prompt);
    char buf[LINE_MAX + PATH_MAX + 160]; int o = 0;
    int cols = MAX(sh->tty->cols, 1), plen = visible_len(prompt);
    /* back to the prompt's first row, clear everything below, draw it all and let the terminal wrap */
    if (sh->cur_row > 0) o += snprintf(buf + o, sizeof buf - o, "\033[%dA", sh->cur_row);
    o += snprintf(buf + o, sizeof buf - o, "\r\033[J%s", prompt);
    memcpy(buf + o, sh->line, sh->len); o += sh->len;
    int end = plen + sh->len, pos = plen + sh->cursor;
    if (end % cols == 0) o += snprintf(buf + o, sizeof buf - o, "\r\n");   /* leave the pending wrap at the last column */
    int erow = end / cols, prow = pos / cols, pcol = pos % cols;
    if (erow > prow) o += snprintf(buf + o, sizeof buf - o, "\033[%dA", erow - prow);
    o += snprintf(buf + o, sizeof buf - o, "\r");
    if (pcol) o += snprintf(buf + o, sizeof buf - o, "\033[%dC", pcol);
    sh->cur_row = prow; sh->cur_col = pcol;
    tty_write(sh->tty, buf, o);
}
/* put the terminal cursor on a fresh row below the whole line (sh->cursor is kept) */
static void leave_line(struct shell *sh) {
    int c = sh->cursor; sh->cursor = sh->len; redraw_line(sh); sh->cursor = c;
    if (sh->cur_col) tty_write(sh->tty, "\n", 1);
    sh->cur_row = 0;
}
static void insert_str(struct shell *sh, const char *s, int n) {
    if (sh->len + n >= LINE_MAX - 1) return;
    memmove(sh->line + sh->cursor + n, sh->line + sh->cursor, sh->len - sh->cursor);
    memcpy(sh->line + sh->cursor, s, n); sh->len += n; sh->cursor += n; sh->line[sh->len] = 0;
}

static bool has_cand(char (*c)[NAME_MAX + 2], int n, const char *s) { for (int i = 0; i < n; i++) if (!strcmp(c[i], s)) return true; return false; }
__attribute__((noinline)) static void complete(struct shell *sh) {   /* 33 KiB of candidates: keep them out of shell_main's frame */
    /* find the word under the cursor */
    int ws = sh->cursor; while (ws > 0 && sh->line[ws - 1] != ' ' && sh->line[ws - 1] != '|' && sh->line[ws - 1] != ';') ws--;
    char word[LINE_MAX]; int wl = sh->cursor - ws; memcpy(word, sh->line + ws, wl); word[wl] = 0;
    bool first = true; for (int i = 0; i < ws; i++) if (sh->line[i] != ' ') { first = false; break; }
    for (int i = ws - 1; i >= 0; i--) { if (sh->line[i] == ' ') continue; first = sh->line[i] == '|' || sh->line[i] == ';' || sh->line[i] == '&'; break; }
    char cands[128][NAME_MAX + 2]; int nc = 0;
    char dir[PATH_MAX], prefix[NAME_MAX + 1], dirdisp[PATH_MAX];
    bool path_mode = !first || strchr(word, '/');
    if (!path_mode) {
        for (int i = 0; i < nbuiltins && nc < 128; i++) if (!strncmp(builtins[i].name, word, wl)) strlcpy(cands[nc++], builtins[i].name, NAME_MAX + 2);
        for (int i = 0; i < sh->naliases && nc < 128; i++) if (!strncmp(sh->aliases[i].name, word, wl) && !has_cand(cands, nc, sh->aliases[i].name)) strlcpy(cands[nc++], sh->aliases[i].name, NAME_MAX + 2);
        struct vnode *b = vfs_lookup("/bin"); struct dirent de;
        for (int i = 0; b && nc < 128 && vfs_readdir(b, i, &de) == 0; i++) if (!strncmp(de.name, word, wl) && !has_cand(cands, nc, de.name)) strlcpy(cands[nc++], de.name, NAME_MAX + 2);
        if (b) vnode_unref(b);
        dirdisp[0] = 0;
    } else {
        char w2[LINE_MAX]; strlcpy(w2, word, sizeof w2);
        if (w2[0] == '~' && (w2[1] == '/' || !w2[1])) { char tb[LINE_MAX]; snprintf(tb, sizeof tb, "/home/flux%s", w2 + 1); strcpy(w2, tb); }
        char *slash = strrchr(w2, '/');
        if (slash && ((size_t)(slash - w2) >= sizeof dir || (size_t)(strrchr(word, '/') - word + 1) >= sizeof dirdisp)) return;
        if (slash) { size_t dl = slash - w2; memcpy(dirdisp, word, strrchr(word, '/') - word + 1); dirdisp[strrchr(word, '/') - word + 1] = 0; memcpy(dir, w2, dl); dir[dl] = 0; if (!dl) strcpy(dir, "/"); strlcpy(prefix, slash + 1, sizeof prefix); }
        else { strcpy(dir, "."); dirdisp[0] = 0; strlcpy(prefix, w2, sizeof prefix); }
        struct vnode *d = vfs_lookup_rel(sh->proc->cwd, dir); struct dirent de;
        for (int i = 0; d && nc < 128 && vfs_readdir(d, i, &de) == 0; i++) {
            if (strncmp(de.name, prefix, strlen(prefix))) continue;
            if (de.name[0] == '.' && prefix[0] != '.') continue;
            snprintf(cands[nc++], NAME_MAX + 2, "%s%s", de.name, de.type == VN_DIR ? "/" : "");
        }
        if (d) vnode_unref(d);
    }
    if (!nc) return;
    /* common prefix */
    int common = strlen(cands[0]);
    for (int i = 1; i < nc; i++) { int k = 0; while (k < common && cands[i][k] == cands[0][k]) k++; common = k; }
    int already = path_mode ? strlen(prefix) : wl;
    if (common > already) {
        insert_str(sh, cands[0] + already, common - already);
        if (nc == 1 && cands[0][common - 1] != '/') insert_str(sh, " ", 1);
        sh->tab_count = 0;
        return;
    }
    if (++sh->tab_count >= 2 || nc <= 1) {
        /* list candidates */
        leave_line(sh);
        int maxw = 0; for (int i = 0; i < nc; i++) maxw = MAX(maxw, (int)strlen(cands[i]));
        int colw = maxw + 2, cols = MAX(1, sh->tty->cols / colw);
        for (int i = 0; i < nc; i++) { char buf[NAME_MAX + 8]; int n = snprintf(buf, sizeof buf, "%-*s", colw, cands[i]); tty_write(sh->tty, buf, n); if ((i + 1) % cols == 0 || i == nc - 1) tty_write(sh->tty, "\n", 1); }
        sh->tab_count = 0;
    }
}

static bool read_line(struct shell *sh) {
    sh->len = 0; sh->cursor = 0; sh->line[0] = 0; sh->hist_pos = sh->nhist; sh->tab_count = 0; sh->cur_row = 0;
    redraw_line(sh);
    for (;;) {
        char ch;
        int64_t n = tty_read(sh->tty, &ch, 1);
        if (n <= 0) { if (thread_should_exit() || sh->tty->closed) return false; continue; }
        if (ch != '\t') sh->tab_count = 0;
        if (ch == '\r' || ch == '\n') { leave_line(sh); return true; }
        else if (ch == 27) {
            char seq[8]; int sl = 0;
            while (sl < 7) { if (tty_read(sh->tty, &seq[sl], 1) <= 0) break; sl++; if (isalpha(seq[sl - 1]) || seq[sl - 1] == '~') break; }
            seq[sl] = 0;
            if (!strcmp(seq, "[A") || !strcmp(seq, "[B")) {
                if (seq[1] == 'A' && sh->hist_pos > 0) { if (sh->hist_pos == sh->nhist) strcpy(sh->saved_line, sh->line); sh->hist_pos--; strlcpy(sh->line, sh->history[sh->hist_pos], sizeof sh->line); }
                else if (seq[1] == 'B' && sh->hist_pos < sh->nhist) { sh->hist_pos++; strlcpy(sh->line, sh->hist_pos == sh->nhist ? sh->saved_line : sh->history[sh->hist_pos], sizeof sh->line); }
                sh->len = sh->cursor = strlen(sh->line);
            }
            else if (!strcmp(seq, "[C")) { if (sh->cursor < sh->len) sh->cursor++; }
            else if (!strcmp(seq, "[D")) { if (sh->cursor > 0) sh->cursor--; }
            else if (!strcmp(seq, "[H")) sh->cursor = 0;
            else if (!strcmp(seq, "[F")) sh->cursor = sh->len;
            else if (!strcmp(seq, "[3~")) { if (sh->cursor < sh->len) { memmove(sh->line + sh->cursor, sh->line + sh->cursor + 1, sh->len - sh->cursor); sh->len--; } }
            else if (sl == 1 && (seq[0] == 'b' || seq[0] == 'f')) { /* alt+b / alt+f word moves */
                if (seq[0] == 'b') { while (sh->cursor > 0 && sh->line[sh->cursor - 1] == ' ') sh->cursor--; while (sh->cursor > 0 && sh->line[sh->cursor - 1] != ' ') sh->cursor--; }
                else { while (sh->cursor < sh->len && sh->line[sh->cursor] != ' ') sh->cursor++; while (sh->cursor < sh->len && sh->line[sh->cursor] == ' ') sh->cursor++; }
            }
        }
        else if (ch == 127 || ch == 8) { if (sh->cursor > 0) { memmove(sh->line + sh->cursor - 1, sh->line + sh->cursor, sh->len - sh->cursor + 1); sh->cursor--; sh->len--; } }
        else if (ch == 1) sh->cursor = 0;
        else if (ch == 5) sh->cursor = sh->len;
        else if (ch == 2) { if (sh->cursor > 0) sh->cursor--; }
        else if (ch == 6) { if (sh->cursor < sh->len) sh->cursor++; }
        else if (ch == 11) { sh->len = sh->cursor; sh->line[sh->len] = 0; }
        else if (ch == 21) { memmove(sh->line, sh->line + sh->cursor, sh->len - sh->cursor + 1); sh->len -= sh->cursor; sh->cursor = 0; }
        else if (ch == 23) { int e = sh->cursor, s = e; while (s > 0 && sh->line[s - 1] == ' ') s--; while (s > 0 && sh->line[s - 1] != ' ') s--; memmove(sh->line + s, sh->line + e, sh->len - e + 1); sh->len -= e - s; sh->cursor = s; }
        else if (ch == 12) { tty_write(sh->tty, "\033[H\033[2J", 7); sh->cur_row = 0; }
        else if (ch == 3) { sh->cursor = sh->len; redraw_line(sh); tty_write(sh->tty, "^C\n", 3); sh->cur_row = 0; sh->len = sh->cursor = 0; sh->line[0] = 0; sh->hist_pos = sh->nhist; }
        else if (ch == 4) { if (sh->len == 0) { tty_write(sh->tty, "exit\n", 5); return false; } }
        else if (ch == '\t') { complete(sh); }
        else if ((unsigned char)ch >= 32) insert_str(sh, &ch, 1);
        redraw_line(sh);
    }
}

void shell_main(void *arg) {
    struct shell_start *ss = arg;
    struct shell *sh = kzalloc(sizeof *sh);
    sh->tty = ss->tty; sh->proc = current_thread->proc;
    sh->tty_f = tty_file(sh->tty);
    spin_lock(&sh->tty->lock); sh->tty->intr_flag = &sh->cancel; spin_unlock(&sh->tty->lock);   /* Ctrl+C stops ping, play, sleep, ... */
    env_set(sh, "HOME", "/home/flux"); env_set(sh, "USER", "flux"); env_set(sh, "SHELL", "/bin/zsh"); env_set(sh, "PATH", "/bin");
    env_set(sh, "TERM", "xterm-256color"); env_set(sh, "PWD", sh->proc->cwd); env_set(sh, "HOSTNAME", "influx"); env_set(sh, "EDITOR", "editor");
    alias_set(sh, "ll", "ls -l"); alias_set(sh, "la", "ls -la"); alias_set(sh, "l", "ls -lah"); alias_set(sh, "..", "cd .."); alias_set(sh, "cls", "clear");
    nbuiltins = ARRAY_LEN(builtins);
    history_load(sh);
    struct ctx c = { sh, sh->tty_f, sh->tty_f, sh->tty_f };
    tty_set_raw(sh->tty, true);
    struct stat st;
    if (vfs_stat("/home/flux/.zshrc", &st) == 0) { char *argv[] = { "source", "/home/flux/.zshrc" }; b_source(&c, 2, argv); }
    if (ss->cmd) { run_line(sh, &c, ss->cmd); }
    else {
        void *motd; size_t msz;
        if (vfs_read_file("/etc/motd", &motd, &msz) == 0) { tty_write(sh->tty, motd, msz); kfree(motd); }
    }
    while (!sh->exiting && !thread_should_exit()) {
        /* reap finished background jobs */
        struct { int pid; char name[32]; } done[16]; int nd = 0;
        procs_lock();   /* a process can't be freed mid-scan */
        struct process *plist[64]; int np = process_list(plist, 64);
        for (int i = 0; i < np && nd < 16; i++) if (plist[i]->parent == sh->proc && plist[i]->exited && plist[i]->is_user) { done[nd].pid = plist[i]->pid; strlcpy(done[nd].name, plist[i]->name, sizeof done[nd].name); nd++; }
        procs_unlock();
        for (int i = 0; i < nd; i++) { char msg[64]; int ml = snprintf(msg, sizeof msg, "[1]  + done  %s\n", done[i].name); tty_write(sh->tty, msg, ml); process_wait(done[i].pid, NULL); }
        char title[PATH_MAX + 16]; int tl = snprintf(title, sizeof title, "\033]0;%s\007", sh->proc->cwd); tty_write(sh->tty, title, tl);
        if (!read_line(sh)) break;
        if (sh->len == 0) continue;
        history_add(sh, sh->line);
        c.rlen = c.rpos = 0;
        char line_copy[LINE_MAX]; strcpy(line_copy, sh->line);
        run_line(sh, &c, line_copy);
    }
    /* background builtins are threads of this process and use sh: stop them like a closing Terminal
     * does (raise the cancel flag, kill them out of pipe and terminal reads), wait until they are gone */
    sh->exiting = true; sh->cancel = true;
    procs_lock();
    struct thread *t; list_for_each_entry(t, &sh->proc->threads, proc_link) if (t != current_thread) thread_kill(t);
    procs_unlock();
    while (__atomic_load_n(&sh->bg_jobs, __ATOMIC_SEQ_CST)) thread_sleep_ms(10);
    spin_lock(&sh->tty->lock); sh->tty->intr_flag = NULL; spin_unlock(&sh->tty->lock);   /* Terminal raises it under this lock */
    vfs_close(sh->tty_f);
    for (int i = 0; i < sh->nhist; i++) kfree(sh->history[i]);
    kfree(ss->cwd); kfree(ss->cmd); kfree(ss);
    kfree(sh);
}
