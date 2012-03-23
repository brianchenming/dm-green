/*
 * Copyright (C) 2012, Ming Chen
 * 
 * A target to save energy by directing reads/writes to different physical
 * disks based on energy characteristics. 
 *
 * This file is released under the GPL.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/log2.h>
#include <linux/dm-io.h>
#include <linux/dm-kcopyd.h>
#include <linux/vmalloc.h>
#include <linux/jiffies.h>
#include <linux/bitmap.h>
#include <linux/list.h>
#include <linux/spinlock.h>

#include <linux/device-mapper.h>

#define DM_MSG_PREFIX "energy"

/* Define this macro if compile before Linux 3.0 */
#define DME_OLD_KERNEL

/*
 * Magic for persistent energy header: "EnEg"
 */
#define ENERGY_MAGIC 0x45614567
#define ENERGY_VERSION 52
#define ENERGY_DAEMON "kenergyd"

/* The first disk is prime disk. */
#define PRIME_DISK 0

#define SECTOR_SIZE (1 << SECTOR_SHIFT)

#define count_sector(x) (((x) + SECTOR_SIZE - 1) >> SECTOR_SHIFT)

/* Return metadata's size in sector. */
#define header_size() \
    count_sector(sizeof(struct energy_header_disk))

#define table_size(ec) \
    count_sector(ec->header.capacity * sizeof(struct vextent_disk))

/* Return size of bitmap array */
#define bitmap_size(len) dm_round_up(len, sizeof(unsigned long))

#define extent_size(ec) (ec->header.ext_size)
#define vdisk_size(ec) (ec->header.capacity)
#define prime_size(ec) (ec->disks[PRIME_DISK].capacity)
#define fdisk_nr(ec) (ec->header.ndisk)

/* 
 * When requesting a new bio, the number of requested bvecs has to be
 * less than BIO_MAX_PAGES. Otherwise, null is returned. In dm-io.c,
 * this return value is not checked and kernel Oops may happen. We set
 * the limit here to avoid such situations. (2 additional bvecs are
 * required by dm-io for bookeeping.) (From dm-cache)
 */
#define MAX_SECTORS ((BIO_MAX_PAGES - 2) * (PAGE_SIZE >> SECTOR_SHIFT))

/* Size of reserved free extent on prime disk */
#define EXTENT_FREE 8
#define EXTENT_LOW 4

#define array_too_big(fixed, obj, num) \
	((num) > (UINT_MAX - (fixed)) / (obj))

typedef uint64_t extent_t;

/*
 * Header in memory, contained in energy context (energy_c).
 */
struct energy_header {
    uint32_t magic;
    uint32_t version;
    uint32_t ndisk;
    uint32_t ext_size;
    extent_t capacity;          /* capacity in extent */
};

/*
 * Header on disk, followed by metadata of mapping table.
 */
struct energy_header_disk {
    __le32 magic;
    __le32 version;
    __le32 ndisk;
    __le32 ext_size;
    __le64 capacity;
} __packed;

/* Virtual extent states */
#define VES_PRESENT 0x01
#define VES_ACCESS  0x02
#define VES_MIGRATE 0x04

/*
 * Virtual extent in memory.
 */
struct vextent {
    extent_t eid;               /* physical extent id */
    uint32_t state;             
    uint32_t counter;           /* how many times are accessed */
    uint64_t tick;              /* timestamp of latest access */
    uint32_t exflags; 
};

/*
 * Extent metadata on disk.
 */
struct vextent_disk {
    __le64 eid;
    __le32 state;
    __le32 counter;
} __packed;

/*
 * Physical extent.
 */
struct extent {
    struct vextent *vext;       /* virtual extent */
    struct list_head list;
};

/*
 * Ring buffer of physical extent.
 */
struct extent_buffer {
    extent_t data[EXTENT_FREE]; /* array of physical extent id */
    unsigned capacity;          
    unsigned cursor;            /* cursor of first entent id */
    unsigned count;             /* number of valid extents */
};

struct mapped_disk {
    struct dm_dev *dev;
    extent_t capacity;          /* capacity in extent */
    extent_t free_nr;           /* number of free extents */
    extent_t offset;            /* offset within virtual disk in extent */
};

struct energy_c {
    struct dm_target *ti;

    struct energy_header header;
    uint32_t flags;
    uint32_t ext_shift;

    struct mapped_disk *disks;

    struct vextent *table;       /* mapping table */

    struct extent   *prime_extents; /* physical extents on prime disk */
    struct list_head prime_free;    /* free extents on prime disk */
    struct list_head prime_use;     /* in-use extents on prime disk */

    struct list_head prime_active;  	/* active extents on prime disk */	
    struct list_head prime_inactive;	/* inactive extents on prime disk */

    unsigned long *bitmap;      /* bitmap of extent, '0' for free extent */
    spinlock_t lock;            /* protect table, free and bitmap */

    struct dm_io_client *io_client;
    struct dm_kcopyd_client *kcp_client;

    struct work_struct eviction_work;   /* work of evicting prime extent */
    struct extent *eviction_cursor;
    bool eviction_running;
};
