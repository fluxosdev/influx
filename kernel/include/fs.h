#pragma once
#include "kernel.h"
#include "list.h"
#include "sched.h"

#define VN_FILE 1
#define VN_DIR  2
#define VN_DEV  3      /* character device */
#define VN_BLK  4      /* block device */
#define VN_LNK  5      /* symbolic link: its data is the target path */

#define O_RDONLY  0x0000
#define O_WRONLY  0x0001
#define O_RDWR    0x0002
#define O_ACCMODE 0x0003
#define O_CREAT   0x0040
#define O_EXCL    0x0080
#define O_NOCTTY  0x0100
#define O_NONBLOCK 0x0800
#define O_CLOEXEC 0x80000
#define O_NOFOLLOW 0x20000
#define O_TRUNC   0x0200
#define O_APPEND  0x0400
#define O_DIRECTORY 0x10000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define NAME_MAX 255
#define PATH_MAX 1024

struct vnode;
struct file;
struct dirent { char name[NAME_MAX + 1]; int type; uint64_t size; uint64_t mtime; uint64_t ino; };
struct stat { int type; uint64_t size; uint64_t mtime; uint32_t mode; uint64_t ino; uint32_t nlink; };

struct vnode_ops {
    int64_t (*read)(struct vnode *v, uint64_t off, void *buf, size_t n);
    int64_t (*write)(struct vnode *v, uint64_t off, const void *buf, size_t n);
    int (*truncate)(struct vnode *v, uint64_t size);
    struct vnode *(*lookup)(struct vnode *dir, const char *name);
    int (*readdir)(struct vnode *dir, int index, struct dirent *out);   /* 0 ok, -1 end */
    struct vnode *(*create)(struct vnode *dir, const char *name, int type);
    int (*unlink)(struct vnode *dir, const char *name);
    int (*ioctl)(struct vnode *v, int cmd, void *arg);
    void (*release)(struct vnode *v);
    int (*sync)(struct vnode *v);
    /* optional: write at the current end of file, atomically with respect to other writers (O_APPEND);
     * *end receives the offset just past the written data */
    int64_t (*append)(struct vnode *v, const void *buf, size_t n, uint64_t *end);
    /* optional: which of POLLIN/POLLOUT/POLLHUP/POLLERR hold now (no poll: always readable and writable).
     * Objects with a poll must call poll_notify() whenever the answer may have changed. */
    int (*poll)(struct vnode *v, int events);
};

#define POLLIN   0x001
#define POLLPRI  0x002
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020
#define POLLRDNORM 0x040
#define POLLWRNORM 0x100
int vfs_poll(struct vnode *v, int events);
void poll_notify(void);                           /* some pollable object changed state */
uint64_t poll_generation(void);
/* sleeps until poll_notify() moves the generation past gen, the timeout passes (ms < 0: none)
 * or a signal arrives; false on timeout or signal */
bool poll_wait(uint64_t gen, int64_t ms);

struct vnode {
    char name[NAME_MAX + 1];
    int type;
    uint64_t size;
    uint64_t mtime;
    uint32_t mode;
    uint64_t ino;
    const struct vnode_ops *ops;
    void *priv;
    void *fs;
    int refcount;
    struct vnode *mount_root;     /* if a filesystem is mounted on this directory */
    struct vnode *parent;
    const char *fs_name;
    uint64_t fs_total, fs_free;   /* only on fs roots */
};

struct file { struct vnode *vn; uint64_t pos; int flags; int refcount; void *priv; char *path; /* absolute path it was opened by (NULL: pipe, socket...) */ };

void vfs_init(void);
struct vnode *vfs_root(void);
int vfs_mount(const char *path, struct vnode *root);
int vfs_unmount(const char *path);
struct vnode *vfs_lookup(const char *path);                 /* returns a referenced vnode or NULL */
struct vnode *vfs_lookup_rel(const char *cwd, const char *path);
void vnode_ref(struct vnode *v);
void vnode_unref(struct vnode *v);
int vfs_open(const char *path, int flags, struct file **out);
int vfs_open_rel(const char *cwd, const char *path, int flags, struct file **out);
int64_t vfs_read(struct file *f, void *buf, size_t n);
int64_t vfs_write(struct file *f, const void *buf, size_t n);
int64_t vfs_pread(struct vnode *v, uint64_t off, void *buf, size_t n);
int64_t vfs_pwrite(struct vnode *v, uint64_t off, const void *buf, size_t n);
int64_t vfs_seek(struct file *f, int64_t off, int whence);
int vfs_close(struct file *f);                              /* returns the sync error, if any; f is released regardless */
struct file *vfs_dup(struct file *f);
int vfs_stat(const char *path, struct stat *st);
int vfs_stat_rel(const char *cwd, const char *path, struct stat *st);
int vfs_readdir(struct vnode *dir, int index, struct dirent *d);
int vfs_mkdir(const char *path);
int vfs_create(const char *path);
int vfs_unlink(const char *path);
int vfs_rename(const char *oldp, const char *newp);
int vfs_symlink(const char *target, const char *linkpath);
int vfs_readlink(const char *path, char *buf, size_t n);   /* bytes, not NUL-terminated */
int vfs_lstat(const char *path, struct stat *st);
struct vnode *vfs_lookup_nofollow(const char *path);
int vfs_truncate(struct vnode *v, uint64_t size);           /* regular files only */
int vfs_sync(void);
int vfs_normalize(const char *cwd, const char *path, char *out, size_t n);   /* 0, -ENOENT (empty path) or -ENAMETOOLONG; out is "" on error */
int vfs_read_file(const char *path, void **data, size_t *size);   /* kmalloc'd, NUL terminated */
int vfs_write_file(const char *path, const void *data, size_t size);
const char *vfs_basename(const char *path);
void vfs_dirname(const char *path, char *out, size_t n);
#define VFS_MAX_MOUNTS 16
int vfs_mount_count(void);
struct vnode *vfs_mount_at(int i, char *path, size_t n);

/* tmpfs */
struct vnode *tmpfs_create_root(const char *fsname);
/* initrd */
void initrd_unpack(const void *tar, size_t size, const char *dest);
/* devfs */
void devfs_init(void);
struct vnode *devfs_add(const char *name, int type, const struct vnode_ops *ops, void *priv, uint64_t size);
/* errno-ish return codes */
#define ENOENT   2
#define EIO      5
#define EBADF    9
#define ENOMEM   12
#define EACCES   13
#define EEXIST   17
#define ENOTDIR  20
#define EISDIR   21
#define EINVAL   22
#define EFBIG    27
#define ENOSPC   28
#define EROFS    30
#define ENOTEMPTY 39
#define ENOSYS   38
#define ENAMETOOLONG 36
#define ENOEXEC  8
#define ENOTTY_  25
#define ECHILD   10
#define ESRCH    3
#ifndef EBUSY
#define EBUSY    16
#endif
#define ENODEV   19
#define ENOTSUP  95
#define EPIPE    32
#define EAGAIN   11
#define EINTR    4
#define ERESTARTSYS 512
#define EFAULT   14
#define ESPIPE   29
#define EMFILE   24
#define ENOTSOCK 88
#define EPERM    1
#define EXDEV    18
#define ELOOP    40
#define ERANGE   34
#define EOVERFLOW 75
#ifndef ETIMEDOUT
#define ETIMEDOUT 110
#endif
