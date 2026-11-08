/* SPDX-License-Identifier: MIT */
/*
 * Portable FAT12/16/32 file system core.
 *
 * This file and fatcore.c have no operating system dependencies: they are
 * compiled into the XenonOS kernel (wrapped by the fastfat driver) and also
 * into a Linux program (tests/host/fat/xfat) that checks them against
 * mkfs.fat, fsck.fat and mtools. The environment supplies sector I/O through
 * fat_dev and three hooks (memory and the current time).
 *
 * Not thread safe: the caller serialises all calls for one volume.
 * Names are UTF-8; separators are '/' or '\\'; lookups ignore case.
 */
#ifndef FATCORE_H
#define FATCORE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Errors (negative). */
#define FAT_OK            0
#define FAT_EIO          (-1)
#define FAT_ENOENT       (-2)
#define FAT_EEXIST       (-3)
#define FAT_ENOTDIR      (-4)
#define FAT_EISDIR       (-5)
#define FAT_ENOTEMPTY    (-6)
#define FAT_ENOSPC       (-7)
#define FAT_EINVAL       (-8)     /* bad name or argument */
#define FAT_ENOMEM       (-9)
#define FAT_EROFS        (-10)
#define FAT_ECORRUPT     (-11)
#define FAT_EBUSY        (-12)
#define FAT_EFBIG        (-13)
#define FAT_ENAMETOOLONG (-14)
#define FAT_ENOTFAT      (-15)    /* no FAT file system on the device */

/* Attributes */
#define FAT_ATTR_READONLY  0x01
#define FAT_ATTR_HIDDEN    0x02
#define FAT_ATTR_SYSTEM    0x04
#define FAT_ATTR_VOLUME    0x08
#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_ARCHIVE   0x20
#define FAT_ATTR_LFN       0x0F

typedef struct fat_dev {
    int (*read)(void *ctx, uint64_t lba, uint32_t count, void *buf);         /* 0 or < 0 */
    int (*write)(void *ctx, uint64_t lba, uint32_t count, const void *buf);
    void *ctx;
    uint32_t sector_size;
    uint64_t sectors;
} fat_dev;

typedef struct fat_time {
    uint16_t year;                /* 1980-2107 */
    uint8_t month, day, hour, minute, second;
    uint16_t ms;
} fat_time;

/* Supplied by the environment. */
void *fat_env_alloc(size_t size);       /* zeroed memory, or NULL */
void fat_env_free(void *p);
void fat_env_now(fat_time *t);

typedef struct fat_volume fat_volume;
typedef struct fat_node fat_node;

#define FAT_NAME_MAX (255 * 3)          /* UTF-8 bytes of a long name */

typedef struct fat_info {
    char name[FAT_NAME_MAX + 1];        /* long name, or short name with case flags applied */
    char short_name[13];                /* "NAME.EXT" */
    uint8_t attr;
    uint32_t size;
    uint32_t first_cluster;
    fat_time created, modified, accessed;
} fat_info;

typedef struct fat_volinfo {
    int type;                           /* 12, 16 or 32 */
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t cluster_bytes;
    uint32_t total_clusters;
    uint32_t free_clusters;
    uint32_t serial;
    char label[12];                     /* trimmed, "" if none */
    bool read_only;
} fat_volinfo;

int  fat_mount(const fat_dev *dev, bool read_only, fat_volume **out);
int  fat_unmount(fat_volume *v);        /* writes everything, marks the volume clean, frees it */
void fat_discard(fat_volume *v);        /* the medium is gone or changed: free without writing anything */
int  fat_mark_clean(fat_volume *v);     /* write FSInfo and clear the dirty flag (until the next change) */
int  fat_sync(fat_volume *v);           /* writes FSInfo; the volume stays "in use" */
void fat_volume_info(fat_volume *v, fat_volinfo *info);
int  fat_open_count(fat_volume *v);     /* open nodes other than the root */

fat_node *fat_root(fat_volume *v);      /* referenced; release with fat_close */
int  fat_open(fat_volume *v, const char *path, fat_node **out);
int  fat_lookup(fat_node *dir, const char *name, fat_node **out);
int  fat_create(fat_node *dir, const char *name, bool directory, fat_node **out);
void fat_ref(fat_node *n);
void fat_close(fat_node *n);
fat_volume *fat_node_volume(fat_node *n);
bool fat_is_dir(fat_node *n);
fat_node *fat_parent(fat_node *n);      /* not referenced; NULL for the root */

int  fat_read(fat_node *n, uint64_t offset, void *buf, uint32_t len, uint32_t *done);
int  fat_write(fat_node *n, uint64_t offset, const void *buf, uint32_t len, uint32_t *done);
int  fat_truncate(fat_node *n, uint64_t size);
int  fat_stat(fat_node *n, fat_info *info);
int  fat_set_attr(fat_node *n, uint8_t attr);
int  fat_set_times(fat_node *n, const fat_time *created, const fat_time *modified, const fat_time *accessed);
int  fat_flush(fat_node *n);            /* write the directory entry */

/* Directory listing: *cookie starts at 0. Returns 1 with an entry, 0 at the
   end, < 0 on error. "." and ".." are included (except in the root). */
int  fat_readdir(fat_node *dir, uint32_t *cookie, fat_info *info);

/* Remove a file or an empty directory. An open file disappears from the
   directory at once; its clusters are freed when the last handle closes. */
int  fat_unlink(fat_node *dir, const char *name);
int  fat_rename(fat_node *src_dir, const char *src, fat_node *dst_dir, const char *dst, bool replace);
/* Remove the entry of an open node (file or empty directory). Its clusters
   are freed when the last reference is closed. */
int  fat_remove(fat_node *n);
int  fat_set_label(fat_volume *v, const char *label);

const char *fat_strerror(int err);

#endif
