/* Disk/tree paths must never run: the append regression uses a loaded buffer. */
#include "btrfs_internal.h"
#include <stdlib.h>
void btrfs_op_begin(struct btrfs_fs *fs) { (void)fs; }
int btrfs_search(struct btrfs_fs *fs,struct btrfs_tree *t,const struct btrfs_key *k,struct btrfs_path *p) { abort(); }
int btrfs_next_item(struct btrfs_fs *fs,struct btrfs_path *p) { abort(); }
int btrfs_prev_item(struct btrfs_fs *fs,struct btrfs_path *p) { abort(); }
int btrfs_path_normalize(struct btrfs_fs *fs,struct btrfs_path *p) { abort(); }
struct btrfs_key *btrfs_path_key(struct btrfs_path *p) { abort(); }
void *btrfs_path_data(struct btrfs_path *p) { abort(); }
uint32_t btrfs_path_size(struct btrfs_path *p) { abort(); }
uint64_t btrfs_logical_to_phys(struct btrfs_fs *fs,uint64_t l,int s) { abort(); }
int blockdev_read_bytes(struct blockdev *d,uint64_t off,void *buf,size_t n) { abort(); }
