/*
 * mkinitrd: packs directory trees into the ustar archive the kernel unpacks as its root
 * filesystem (kernel/src/fs/initrd.c). Replaces the GNU tar invocation the build used, so the
 * build needs no GNU-only tool, and the output is reproducible: entries are sorted (a directory
 * always before what it contains), owners are 0, and every timestamp is the given epoch.
 *
 *   mkinitrd [-t epoch] out.tar root[=prefix] ...
 *
 * Later roots overlay earlier ones; root=prefix puts a tree under prefix/ in the archive.
 * Regular files, directories, symbolic links and hard links (files with the same inode
 * become link entries) are stored; anything else is an error.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct entry { char *rel; char *src; int root; };
static struct entry *ents; static size_t nents, cap;
struct hl { dev_t dev; ino_t ino; char *rel; };
static struct hl *links; static size_t nlinks, lcap;

static void die(const char *fmt, const char *arg) { fprintf(stderr, "mkinitrd: "); fprintf(stderr, fmt, arg); fprintf(stderr, "\n"); exit(1); }

static void add(const char *rel, const char *src, int root) {
    if (nents == cap) { cap = cap ? cap * 2 : 1024; ents = realloc(ents, cap * sizeof *ents); if (!ents) die("%s", "out of memory"); }
    ents[nents++] = (struct entry){ strdup(rel), strdup(src), root };
}

static void walk(const char *src, const char *rel, int root) {
    add(rel, src, root);
    struct stat st;
    if (lstat(src, &st) < 0) die("cannot stat %s", src);
    if (!S_ISDIR(st.st_mode)) return;
    DIR *d = opendir(src);
    if (!d) die("cannot open %s", src);
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char *s, *r;
        if (asprintf(&s, "%s/%s", src, de->d_name) < 0 || asprintf(&r, "%s%s%s", rel, rel[0] ? "/" : "", de->d_name) < 0) die("%s", "out of memory");
        walk(s, r, root);
        free(s); free(r);
    }
    closedir(d);
}

/* parents sort before children ("a" < "a/b"); among equal paths the later root comes first */
static int cmp(const void *x, const void *y) {
    const struct entry *a = x, *b = y;
    int c = strcmp(a->rel, b->rel);
    return c ? c : b->root - a->root;
}

static FILE *out;
static void octal(char *dst, size_t n, uint64_t v) { snprintf(dst, n, "%0*llo", (int)n - 1, (unsigned long long)v); }

static void header(const char *name, char type, uint32_t mode, uint64_t size, uint64_t mtime, const char *linkname) {
    unsigned char h[512];
    memset(h, 0, sizeof h);
    size_t l = strlen(name);
    if (l <= 100) memcpy(h, name, l);
    else {
        /* ustar: prefix (up to 155) + '/' + name (up to 100) */
        const char *cut = NULL;
        for (const char *p = name + l - 1; p > name; p--) if (*p == '/' && (size_t)(p - name) <= 155 && l - (size_t)(p - name) - 1 <= 100) { cut = p; break; }
        if (!cut || cut == name + l - 1) die("path too long for ustar: %s", name);
        memcpy(h + 345, name, (size_t)(cut - name));
        memcpy(h, cut + 1, l - (size_t)(cut - name) - 1);
    }
    octal((char *)h + 100, 8, mode & 07777);
    octal((char *)h + 108, 8, 0);
    octal((char *)h + 116, 8, 0);
    octal((char *)h + 124, 12, size);
    octal((char *)h + 136, 12, mtime);
    h[156] = (unsigned char)type;
    if (linkname) { if (strlen(linkname) > 100) die("link target too long for ustar: %s", linkname); memcpy(h + 157, linkname, strlen(linkname)); }
    memcpy(h + 257, "ustar", 6); memcpy(h + 263, "00", 2);
    memcpy(h + 265, "root", 4); memcpy(h + 297, "root", 4);
    memset(h + 148, ' ', 8);
    unsigned sum = 0; for (int i = 0; i < 512; i++) sum += h[i];
    snprintf((char *)h + 148, 8, "%06o", sum); h[155] = ' ';
    if (fwrite(h, 1, 512, out) != 512) die("%s", "write failed");
}

int main(int argc, char **argv) {
    uint64_t epoch = 1767225600;   /* 2026-01-01T00:00:00Z */
    int i = 1;
    if (i + 1 < argc && !strcmp(argv[i], "-t")) { epoch = strtoull(argv[i + 1], NULL, 10); i += 2; }
    if (argc - i < 2) { fprintf(stderr, "usage: mkinitrd [-t epoch] out.tar root[=prefix] ...\n"); return 2; }
    const char *outpath = argv[i++];
    for (int r = 0; i < argc; i++, r++) {
        char *spec = strdup(argv[i]), *eq = strchr(spec, '=');
        const char *prefix = "";
        if (eq) { *eq = 0; prefix = eq + 1; while (*prefix == '/') prefix++; }
        /* the directories leading to the prefix (unless a root provides them), then the tree,
         * whose top directory becomes the prefix itself */
        if (prefix[0]) {
            char *pp = strdup(prefix);
            for (char *s = pp; *s; s++) if (*s == '/') { *s = 0; add(pp, spec, -1); *s = '/'; }
            free(pp);
        }
        walk(spec, prefix, r);
        free(spec);
    }
    qsort(ents, nents, sizeof *ents, cmp);
    out = fopen(outpath, "wb");
    if (!out) die("cannot create %s", outpath);
    for (size_t k = 0; k < nents; k++) {
        if (k && !strcmp(ents[k].rel, ents[k - 1].rel)) continue;   /* overlaid by a later root */
        struct entry *e = &ents[k];
        char name[4096];
        struct stat st;
        if (e->root < 0) {   /* a directory leading to a prefix */
            snprintf(name, sizeof name, "./%s/", e->rel);
            header(name, '5', 0755, 0, epoch, NULL);
            continue;
        }
        if (lstat(e->src, &st) < 0) die("cannot stat %s", e->src);
        if (S_ISDIR(st.st_mode)) {
            snprintf(name, sizeof name, "./%s%s", e->rel, e->rel[0] ? "/" : "");
            header(name, '5', st.st_mode, 0, epoch, NULL);
        } else if (S_ISLNK(st.st_mode)) {
            char t[4096]; ssize_t n = readlink(e->src, t, sizeof t - 1);
            if (n < 0) die("cannot read link %s", e->src);
            t[n] = 0;
            snprintf(name, sizeof name, "./%s", e->rel);
            header(name, '2', 0777, 0, epoch, t);
        } else if (S_ISREG(st.st_mode)) {
            snprintf(name, sizeof name, "./%s", e->rel);
            if (st.st_nlink > 1) {
                size_t j; for (j = 0; j < nlinks && !(links[j].dev == st.st_dev && links[j].ino == st.st_ino); j++) {}
                if (j < nlinks) { char t[4096]; snprintf(t, sizeof t, "./%s", links[j].rel); header(name, '1', st.st_mode, 0, epoch, t); continue; }
                if (nlinks == lcap) { lcap = lcap ? lcap * 2 : 64; links = realloc(links, lcap * sizeof *links); }
                links[nlinks++] = (struct hl){ st.st_dev, st.st_ino, strdup(e->rel) };
            }
            header(name, '0', st.st_mode, (uint64_t)st.st_size, epoch, NULL);
            FILE *in = fopen(e->src, "rb");
            if (!in) die("cannot read %s", e->src);
            char buf[65536]; size_t n, total = 0;
            while ((n = fread(buf, 1, sizeof buf, in)) > 0) { if (fwrite(buf, 1, n, out) != n) die("%s", "write failed"); total += n; }
            fclose(in);
            if (total != (size_t)st.st_size) die("%s changed while being packed", e->src);
            static const char zero[512];
            if (total % 512) fwrite(zero, 1, 512 - total % 512, out);
        } else die("not a file, directory or link: %s", e->src);
    }
    static const char end[1024];
    fwrite(end, 1, sizeof end, out);
    if (fclose(out) != 0) die("%s", "write failed");
    return 0;
}
