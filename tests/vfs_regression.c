#include "fs.h"
#include "mm.h"
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
void mutex_init(struct mutex *m) { (void)m; }
void mutex_lock(struct mutex *m) { (void)m; }
void mutex_unlock(struct mutex *m) { (void)m; }
void *kmalloc(size_t n) { return malloc(n); }
void *kmalloc_try(size_t n) { return malloc(n); }
void *kzalloc(size_t n) { return calloc(1,n); }
void *krealloc_try(void *p,size_t n) { return realloc(p,n); }
void kfree(void *p) { free(p); }
uint64_t pmm_free_bytes(void) { return 1ULL<<30; }
uint64_t time_unix(void) { return 0; }
int64_t time_realtime_ns(void) { return 0; }
int fifo_open(struct vnode *v, int flags, struct file **out) { return -ENOSYS; }
void fifo_release_node(struct vnode *v) {}
void kprintf(const char *fmt, ...) { (void)fmt; }
size_t strlcpy(char *d,const char *s,size_t n) { size_t l=strlen(s); if(n) { size_t c=l<n-1?l:n-1; memcpy(d,s,c); d[c]=0; } return l; }
static int64_t fault_read(struct vnode *v,uint64_t off,void *buf,size_t n) {
    (void)v; (void)n;
    if(off==0) { memcpy(buf,"GOOD",4); return 4; }
    return -EIO;
}
static const struct vnode_ops fault_ops={.read=fault_read};
/* Exercise VFS/tmpfs unchanged, replacing only allocation, locking and time.
 * These cases are single-threaded; the append test uses real host threads. */
static int64_t eof_read(struct vnode *v,uint64_t off,void *buf,size_t n) {
    if (off) return 0;
    return fault_read(v,off,buf,n);
}
static const struct vnode_ops eof_ops={.read=eof_read};
static void check_failed_move(const char *name,const struct vnode_ops *ops) {
    char src[64]; snprintf(src,sizeof src,"/dev/%s",name);
    devfs_add(name,VN_FILE,ops,NULL,8);
    assert(vfs_write_file("/destination","original",8)==0);
    assert(vfs_rename(src,"/destination")==-EIO);
    struct vnode *source=vfs_lookup(src);
    assert(source && source->size==8); vnode_unref(source);
    void *data; size_t size;
    assert(vfs_read_file("/destination",&data,&size)==0);
    assert(size==8 && memcmp(data,"original",8)==0); kfree(data);
}
int main(void) {
    vfs_init();
    check_failed_move("fault",&fault_ops);
    check_failed_move("early_eof",&eof_ops);
    assert(vfs_write_file("/source","complete",8)==0);
    assert(vfs_rename("/source","/moved")==0);
    assert(vfs_lookup("/source")==NULL);
    void *data; size_t size;
    assert(vfs_read_file("/moved",&data,&size)==0);
    assert(size==8 && memcmp(data,"complete",8)==0); kfree(data);
    struct file *f;
    assert(vfs_open("/moved",O_WRONLY,&f)==0);
    char buf[16]={0};
    assert(vfs_read(f,buf,6)==-EBADF && f->pos==0);
    vfs_close(f);
    assert(vfs_open("/moved",O_RDONLY,&f)==0);
    assert(vfs_read(f,buf,8)==8 && memcmp(buf,"complete",8)==0);
    vfs_close(f);
    assert(vfs_open("/zero",O_CREAT|O_WRONLY,&f)==0);
    assert(vfs_write(f,NULL,0)==0); /* empty, unallocated backing buffer */
    assert(vfs_seek(f,4096,SEEK_SET)==4096);
    assert(vfs_write(f,NULL,0)==0);
    struct stat st; assert(vfs_stat("/zero",&st)==0 && st.size==0);
    assert(f->pos==4096);
    assert(vfs_write(f,"Z",1)==1);
    assert(vfs_stat("/zero",&st)==0 && st.size==4097);
    vfs_close(f);
    /* hard links: one file under two names; it lives until the last name goes */
    assert(vfs_write_file("/h1","shared",6)==0);
    assert(vfs_link("/h1","/h2")==0);
    assert(vfs_link("/h1","/h2")==-EEXIST);
    assert(vfs_stat("/h1",&st)==0 && st.nlink==2);
    assert(vfs_write_file("/h2","SHARED!",7)==0);
    assert(vfs_read_file("/h1",&data,&size)==0 && size==7 && memcmp(data,"SHARED!",7)==0); kfree(data);
    assert(vfs_unlink("/h1")==0);
    assert(vfs_stat("/h2",&st)==0 && st.nlink==1 && st.size==7);
    assert(vfs_read_file("/h2",&data,&size)==0 && size==7); kfree(data);
    assert(vfs_unlink("/h2")==0 && vfs_lookup("/h2")==NULL);
    assert(vfs_mkdir("/ld")==0 && vfs_link("/ld","/ld2")==-EPERM);
    /* rename(2): replaces the target in one step */
    assert(vfs_write_file("/ra","AAA",3)==0 && vfs_write_file("/rb","B",1)==0);
    struct vnode *ra=vfs_lookup("/ra");
    assert(vfs_rename_posix("/ra","/rb")==0 && vfs_lookup("/ra")==NULL);
    struct vnode *rb=vfs_lookup("/rb");
    assert(rb==ra && rb->size==3);   /* the same node, moved */
    vnode_unref(ra); vnode_unref(rb);
    /* directories move with their contents; checks on what may be replaced */
    assert(vfs_mkdir("/d1")==0 && vfs_mkdir("/d1/sub")==0 && vfs_write_file("/d1/sub/f","x",1)==0);
    assert(vfs_rename_posix("/d1","/d1/sub/inside")==-EINVAL);
    assert(vfs_mkdir("/d2")==0 && vfs_write_file("/d2/keep","k",1)==0);
    assert(vfs_rename_posix("/d1","/d2")==-ENOTEMPTY);
    assert(vfs_rename_posix("/rb","/d2")==-EISDIR);
    assert(vfs_rename_posix("/d1","/rb")==-ENOTDIR);
    assert(vfs_mkdir("/d3")==0 && vfs_rename_posix("/d1","/d3")==0);
    assert(vfs_read_file("/d3/sub/f",&data,&size)==0 && size==1); kfree(data);
    assert(vfs_lookup("/d1")==NULL);
    /* a link entry can itself be renamed and unlinked */
    assert(vfs_link("/rb","/d3/rb-link")==0 && vfs_rename_posix("/d3/rb-link","/rb-link2")==0);
    assert(vfs_stat("/rb",&st)==0 && st.nlink==2);
    assert(vfs_rename_posix("/rb-link2","/rb")==0 && vfs_stat("/rb",&st)==0 && st.nlink==2);   /* same file: nothing happens */
    assert(vfs_unlink("/rb-link2")==0 && vfs_stat("/rb",&st)==0 && st.nlink==1);
    /* modes and times */
    assert(vfs_open_mode("/","/exe",O_CREAT|O_WRONLY,0750,&f)==0); vfs_close(f);
    assert(vfs_stat("/exe",&st)==0 && st.mode==0750);
    struct vnode *ev=vfs_lookup("/exe");
    struct vattr va={ VA_MODE|VA_MTIME, 0644, 1234567890, 987654321 };
    assert(vfs_setattr(ev,&va)==0); vnode_unref(ev);
    assert(vfs_stat("/exe",&st)==0 && st.mode==0644 && st.mtime==1234567890 && st.mtime_nsec==987654321);
    assert(vfs_mkdir_mode("/m700",0700)==0 && vfs_stat("/m700",&st)==0 && st.mode==0700);
    puts("VFS/tmpfs regressions passed");
    return 0;
}
