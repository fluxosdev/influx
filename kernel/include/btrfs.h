#pragma once
#include "kernel.h"

uint32_t crc32c(uint32_t seed, const void *data, size_t n);
uint32_t crc32_ieee(const void *data, size_t n);

#define BTRFS_MAGIC "_BHRfS_M"
#define BTRFS_SUPER_OFFSET 0x10000
#define BTRFS_CSUM_SIZE 32

/* object ids */
#define BTRFS_ROOT_TREE_OBJECTID 1ULL
#define BTRFS_EXTENT_TREE_OBJECTID 2ULL
#define BTRFS_CHUNK_TREE_OBJECTID 3ULL
#define BTRFS_DEV_TREE_OBJECTID 4ULL
#define BTRFS_FS_TREE_OBJECTID 5ULL
#define BTRFS_ROOT_TREE_DIR_OBJECTID 6ULL
#define BTRFS_CSUM_TREE_OBJECTID 7ULL
#define BTRFS_FREE_SPACE_TREE_OBJECTID 10ULL
#define BTRFS_BLOCK_GROUP_TREE_OBJECTID 11ULL
#define BTRFS_FIRST_FREE_OBJECTID 256ULL
#define BTRFS_LAST_FREE_OBJECTID (-256ULL)
#define BTRFS_FIRST_CHUNK_TREE_OBJECTID 256ULL
#define BTRFS_EXTENT_CSUM_OBJECTID (-10ULL)

/* key types */
#define BTRFS_INODE_ITEM_KEY 1
#define BTRFS_INODE_REF_KEY 12
#define BTRFS_INODE_EXTREF_KEY 13
#define BTRFS_XATTR_ITEM_KEY 24
#define BTRFS_DIR_ITEM_KEY 84
#define BTRFS_DIR_INDEX_KEY 96
#define BTRFS_EXTENT_DATA_KEY 108
#define BTRFS_EXTENT_CSUM_KEY 128
#define BTRFS_ROOT_ITEM_KEY 132
#define BTRFS_EXTENT_ITEM_KEY 168
#define BTRFS_METADATA_ITEM_KEY 169
#define BTRFS_TREE_BLOCK_REF_KEY 176
#define BTRFS_EXTENT_DATA_REF_KEY 178
#define BTRFS_SHARED_BLOCK_REF_KEY 182
#define BTRFS_SHARED_DATA_REF_KEY 184
#define BTRFS_BLOCK_GROUP_ITEM_KEY 192
#define BTRFS_DEV_EXTENT_KEY 204
#define BTRFS_DEV_ITEM_KEY 216
#define BTRFS_CHUNK_ITEM_KEY 228

#define BTRFS_BLOCK_GROUP_DATA 1
#define BTRFS_BLOCK_GROUP_SYSTEM 2
#define BTRFS_BLOCK_GROUP_METADATA 4
#define BTRFS_BLOCK_GROUP_RAID0 8
#define BTRFS_BLOCK_GROUP_RAID1 0x10
#define BTRFS_BLOCK_GROUP_DUP 0x20
#define BTRFS_BLOCK_GROUP_RAID10 0x40
#define BTRFS_BLOCK_GROUP_RAID5 0x80
#define BTRFS_BLOCK_GROUP_RAID6 0x100
#define BTRFS_BLOCK_GROUP_RAID1C3 0x200
#define BTRFS_BLOCK_GROUP_RAID1C4 0x400

#define BTRFS_EXTENT_FLAG_DATA 1
#define BTRFS_EXTENT_FLAG_TREE_BLOCK 2
#define BTRFS_HEADER_FLAG_WRITTEN 1
#define BTRFS_HEADER_FLAG_MIXED_BACKREF (1ULL << 56)   /* backref revision 1 */

#define BTRFS_FEATURE_INCOMPAT_MIXED_GROUPS 0x4
#define BTRFS_FEATURE_INCOMPAT_EXTENDED_IREF 0x40
#define BTRFS_FEATURE_INCOMPAT_RAID56 0x80
#define BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA 0x100
#define BTRFS_FEATURE_INCOMPAT_NO_HOLES 0x200
#define BTRFS_FEATURE_INCOMPAT_METADATA_UUID 0x400
#define BTRFS_FEATURE_INCOMPAT_ZONED 0x1000
#define BTRFS_FEATURE_INCOMPAT_EXTENT_TREE_V2 0x2000
#define BTRFS_FEATURE_INCOMPAT_BLOCK_GROUP_TREE 0x4000
#define BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE 0x1
#define BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE_VALID 0x2

#define BTRFS_FT_REG_FILE 1
#define BTRFS_FT_DIR 2
#define BTRFS_FT_SYMLINK 7
#define BTRFS_INODE_NODATASUM 1

struct btrfs_key { uint64_t objectid; uint8_t type; uint64_t offset; } PACKED;
struct btrfs_timespec { uint64_t sec; uint32_t nsec; } PACKED;
struct btrfs_dev_item {
    uint64_t devid, total_bytes, bytes_used; uint32_t io_align, io_width, sector_size; uint64_t type, generation, start_offset;
    uint32_t dev_group; uint8_t seek_speed, bandwidth; uint8_t uuid[16], fsid[16];
} PACKED;
struct btrfs_root_backup { uint64_t tree_root, tree_root_gen, chunk_root, chunk_root_gen, extent_root, extent_root_gen, fs_root, fs_root_gen, dev_root, dev_root_gen, csum_root, csum_root_gen, total_bytes, bytes_used, num_devices, unused_64[4]; uint8_t tree_root_level, chunk_root_level, extent_root_level, fs_root_level, dev_root_level, csum_root_level, unused_8[10]; } PACKED;
struct btrfs_super_block {
    uint8_t csum[32]; uint8_t fsid[16]; uint64_t bytenr, flags; char magic[8];
    uint64_t generation, root, chunk_root, log_root, log_root_transid, total_bytes, bytes_used, root_dir_objectid, num_devices;
    uint32_t sectorsize, nodesize, leafsize, stripesize, sys_chunk_array_size;
    uint64_t chunk_root_generation, compat_flags, compat_ro_flags, incompat_flags;
    uint16_t csum_type; uint8_t root_level, chunk_root_level, log_root_level;
    struct btrfs_dev_item dev_item;
    char label[256];
    uint64_t cache_generation, uuid_tree_generation;
    uint8_t metadata_uuid[16];
    uint64_t nr_global_roots, reserved[27];
    uint8_t sys_chunk_array[2048];
    struct btrfs_root_backup super_roots[4];
    uint8_t padding[565];
} PACKED;
struct btrfs_header { uint8_t csum[32]; uint8_t fsid[16]; uint64_t bytenr, flags; uint8_t chunk_tree_uuid[16]; uint64_t generation, owner; uint32_t nritems; uint8_t level; } PACKED;
struct btrfs_item { struct btrfs_key key; uint32_t offset, size; } PACKED;
struct btrfs_key_ptr { struct btrfs_key key; uint64_t blockptr, generation; } PACKED;
struct btrfs_stripe { uint64_t devid, offset; uint8_t dev_uuid[16]; } PACKED;
struct btrfs_chunk { uint64_t length, owner, stripe_len, type; uint32_t io_align, io_width, sector_size; uint16_t num_stripes, sub_stripes; struct btrfs_stripe stripe[]; } PACKED;
struct btrfs_inode_item {
    uint64_t generation, transid, size, nbytes, block_group; uint32_t nlink, uid, gid, mode; uint64_t rdev, flags, sequence, reserved[4];
    struct btrfs_timespec atime, ctime, mtime, otime;
} PACKED;
struct btrfs_inode_ref { uint64_t index; uint16_t name_len; } PACKED;
struct btrfs_dir_item { struct btrfs_key location; uint64_t transid; uint16_t data_len, name_len; uint8_t type; } PACKED;
struct btrfs_file_extent_item { uint64_t generation, ram_bytes; uint8_t compression, encryption; uint16_t other_encoding; uint8_t type; uint64_t disk_bytenr, disk_num_bytes, offset, num_bytes; } PACKED;
#define BTRFS_FILE_EXTENT_INLINE_DATA_START 21
struct btrfs_root_item {
    struct btrfs_inode_item inode; uint64_t generation, root_dirid, bytenr, byte_limit, bytes_used, last_snapshot, flags; uint32_t refs;
    struct btrfs_key drop_progress; uint8_t drop_level, level; uint64_t generation_v2; uint8_t uuid[16], parent_uuid[16], received_uuid[16];
    uint64_t ctransid, otransid, stransid, rtransid; struct btrfs_timespec ctime, otime, stime, rtime; uint64_t reserved[8];
} PACKED;
struct btrfs_extent_item { uint64_t refs, generation, flags; } PACKED;
struct btrfs_tree_block_info { struct btrfs_key key; uint8_t level; } PACKED;
struct btrfs_extent_data_ref { uint64_t root, objectid, offset; uint32_t count; } PACKED;
struct btrfs_block_group_item { uint64_t used, chunk_objectid, flags; } PACKED;
struct btrfs_dev_extent { uint64_t chunk_tree, chunk_objectid, chunk_offset, length; uint8_t chunk_tree_uuid[16]; } PACKED;

_Static_assert(sizeof(struct btrfs_super_block) == 4096, "super block size");
_Static_assert(sizeof(struct btrfs_header) == 101, "header size");
_Static_assert(sizeof(struct btrfs_inode_item) == 160, "inode item size");
_Static_assert(sizeof(struct btrfs_root_item) == 439, "root item size");
_Static_assert(sizeof(struct btrfs_file_extent_item) == 53, "file extent item size");
_Static_assert(sizeof(struct btrfs_dir_item) == 30, "dir item size");
