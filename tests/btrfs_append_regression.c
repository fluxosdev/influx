#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "../kernel/src/fs/btrfs.c"
static pthread_mutex_t host_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_barrier_t start_write;
static struct btrfs_fs testfs;
static bool concurrent;
void mutex_lock(struct mutex *m) { if(m==&testfs.lock) { if(concurrent) pthread_barrier_wait(&start_write); pthread_mutex_lock(&host_lock); } }
void mutex_unlock(struct mutex *m) { if(m==&testfs.lock) pthread_mutex_unlock(&host_lock); }
void *kmalloc_try(size_t n) { return malloc(n); }
void *krealloc_try(void *p,size_t n) { return realloc(p,n); }
void kfree(void *p) { free(p); }
uint64_t time_unix(void) { return 0; }
int64_t time_realtime_ns(void) { return 0; }
int fifo_open(struct vnode *v, int flags, struct file **out) { return -ENOSYS; }
volatile uint64_t system_ticks;
static void *writer(void *f) { int64_t r=vfs_write(f,"X",1); return (void *)(intptr_t)r; }
int main(void) {
    pthread_barrier_init(&start_write,NULL,2);
    struct bnode b={.fs=&testfs,.wbuf=calloc(1,64),.wcap=64};
    const struct vnode_ops ops={.write=b_write,.append=btrfs_ops.append};
    b.vn.ops=&ops; b.vn.type=VN_FILE;
    struct file a={.vn=&b.vn,.flags=O_WRONLY|O_APPEND},c=a;
    pthread_t t1,t2; void *r1,*r2;
    concurrent=true;
    pthread_create(&t1,NULL,writer,&a); pthread_create(&t2,NULL,writer,&c);
    pthread_join(t1,&r1); pthread_join(t2,&r2);
    concurrent=false;
    assert((long)r1==1 && (long)r2==1 && b.vn.size==2);
    assert(b.wsize==2 && memcmp(b.wbuf,"XX",2)==0);
    assert((a.pos==1 && c.pos==2) || (a.pos==2 && c.pos==1));
    /* Appends ignore a preceding seek and continue at EOF. */
    a.pos=0;
    assert(vfs_write(&a,"Y",1)==1 && a.pos==3);
    assert(b.vn.size==3 && memcmp(b.wbuf,"XXY",3)==0);
    assert(vfs_write(&a,NULL,0)==0 && b.vn.size==3);
    /* The refactored normal write still overwrites and zero-fills holes. */
    a.flags=O_WRONLY; a.pos=5;
    assert(vfs_write(&a,"Z",1)==1 && b.vn.size==6);
    assert(b.wbuf[3]==0 && b.wbuf[4]==0 && b.wbuf[5]=='Z');
    testfs.readonly=true;
    a.flags|=O_APPEND;
    assert(vfs_write(&a,"!",1)==-EROFS && b.vn.size==6);
    puts("btrfs append regressions passed");
    free(b.wbuf);
}
