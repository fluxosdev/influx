#pragma once
#include "kernel.h"
#include "list.h"
#include "sched.h"

#define VN_FILE 1
#define VN_DIR  2
#define VN_DEV  3      /* character device */
#define VN_BLK  4      /* block device */

#define O_RDONLY  0x0000
#define O_WRONLY  0x0001
#define O_RDWR    0x0002
#define O_ACCMODE 0x0003
#define O_CREAT   0x0040
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
};

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

struct file { struct vnode *vn; uint64_t pos; int flags; int refcount; void *priv; };

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
void vfs_close(struct file *f);
struct file *vfs_dup(struct file *f);
int vfs_stat(const char *path, struct stat *st);
int vfs_stat_rel(const char *cwd, const char *path, struct stat *st);
int vfs_readdir(struct vnode *dir, int index, struct dirent *d);
int vfs_mkdir(const char *path);
int vfs_create(const char *path);
int vfs_unlink(const char *path);
int vfs_rename(const char *oldp, const char *newp);
int vfs_truncate(struct vnode *v, uint64_t size);
int vfs_sync(void);
void vfs_normalize(const char *cwd, const char *path, char *out, size_t n);
int vfs_read_file(const char *path, void **data, size_t *size);   /* kmalloc'd, NUL terminated */
int vfs_write_file(const char *path, const void *data, size_t size);
const char *vfs_basename(const char *path);
void vfs_dirname(const char *path, char *out, size_t n);
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
#define ENOSPC   28
#define EROFS    30
#define ENOTEMPTY 39
#define ENOSYS   38
#define ENOEXEC  8
#define ENOTTY_  25
#define ECHILD   10
#define ESRCH    3
