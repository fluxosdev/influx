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
    puts("VFS/tmpfs regressions passed");
    return 0;
}
