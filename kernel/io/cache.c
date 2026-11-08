/* SPDX-License-Identifier: MIT */
/*
 * Cc - block cache for disks.
 *
 * The cache holds 4 KiB "lines" of disk data, found through a hash table
 * and recycled in least-recently-used order. Every read and write of a disk
 * goes through its cache, whether it comes from a partition, a file system
 * or raw access to the whole disk, so all of them always see the same data.
 *
 * Fixed disks are write-back: changed lines are written by the lazy writer
 * thread within about a second, on a flush, or at shutdown. Removable disks
 * (floppies, USB sticks) are write-through, so pulling the medium out never
 * loses data that a write already reported as done.
 *
 * Large requests (128 KiB or more) go straight to the disk after dirty lines
 * in the range are written, and update any cached copy, so streaming a big
 * file does not push everything else out of the cache.
 *
 * Phase 1.3 uses one cache-wide mutex, held during disk I/O.
 */
#include "iop.h"
#include "ps.h"
#include "mm.h"
#include "rtl.h"
#include "console.h"

#define HASH_BUCKETS   1024
#define BYPASS_BYTES   (128 * 1024)
#define RUN_LINES      32               /* lines filled or flushed with one disk request */

typedef struct LINE {
    LIST_ENTRY HashEntry;
    LIST_ENTRY LruEntry;            /* most recent at the tail */
    LIST_ENTRY CacheEntry;          /* lines of the same cache */
    struct CACHE *Owner;
    uint64_t LineNo;
    uint32_t Valid;                 /* bytes in this line that exist on the disk */
    bool Dirty;
    uint8_t *Data;
} LINE;

typedef struct CACHE {
    DEVICE_OBJECT *Disk;
    bool WriteThrough;
    LIST_ENTRY Lines;
    uint64_t DirtyCount;
    LIST_ENTRY AllEntry;
} CACHE;

static KMUTEX g_lock;
static LIST_ENTRY g_hash[HASH_BUCKETS];
static LIST_ENTRY g_lru;
static LIST_ENTRY g_caches;
static LINE *g_free;                /* unused line descriptors with pages (singly linked via Owner) */
static uint64_t g_lines, g_capacity;
static CC_STATS g_stats;

#define LOCK()   KeWaitForSingleObject(&g_lock, KE_INFINITE)
#define UNLOCK() KeReleaseMutex(&g_lock)

static inline unsigned bucket(CACHE *c, uint64_t line)
{
    return (unsigned)(((uintptr_t)c >> 6) ^ (line * 2654435761u)) % HASH_BUCKETS;
}

static uint64_t disk_bytes(CACHE *c)
{
    DISK_EXTENSION *x = c->Disk->DeviceExtension;
    return x->SectorCount * x->BytesPerSector;
}

static LINE *find(CACHE *c, uint64_t line)
{
    LIST_ENTRY *h = &g_hash[bucket(c, line)];
    for (LIST_ENTRY *e = h->Flink; e != h; e = e->Flink) {
        LINE *l = CONTAINING_RECORD(e, LINE, HashEntry);
        if (l->Owner == c && l->LineNo == line)
            return l;
    }
    return NULL;
}

static NTSTATUS write_line(LINE *l)
{
    NTSTATUS st = DiskRawWrite(l->Owner->Disk, l->LineNo * CC_LINE_SIZE, l->Valid, l->Data);
    if (NT_SUCCESS(st)) {
        l->Dirty = false;
        l->Owner->DirtyCount--;
        g_stats.WritesToDisk++;
    }
    return st;
}

static void unlink_line(LINE *l)
{
    RemoveEntryList(&l->HashEntry);
    RemoveEntryList(&l->LruEntry);
    RemoveEntryList(&l->CacheEntry);
    if (l->Dirty)
        l->Owner->DirtyCount--;
    l->Owner = NULL;
}

/* Get a free line: a new one if under capacity, else the least recently
   used clean line (writing back a dirty one if that is all there is). */
static LINE *alloc_line(void)
{
    if (g_free) {
        LINE *l = g_free;
        g_free = (LINE *)l->Owner;
        l->Owner = NULL;
        return l;
    }
    if (g_lines < g_capacity) {
        LINE *l = ExAllocatePoolWithTag(NonPagedPoolZeroed, sizeof(LINE), 'CcLn');
        uint64_t pa = l ? MmAllocatePhysicalPages(1, MM_ANY_ADDRESS, 0) : 0;
        if (l && pa) {
            l->Data = MmPhysToVirt(pa);
            g_lines++;
            return l;
        }
        if (l)
            ExFreePoolWithTag(l, 'CcLn');
    }
    for (LIST_ENTRY *e = g_lru.Flink; e != &g_lru; e = e->Flink) {
        LINE *l = CONTAINING_RECORD(e, LINE, LruEntry);
        if (!l->Dirty) {
            unlink_line(l);
            return l;
        }
    }
    if (!IsListEmpty(&g_lru)) {
        LINE *l = CONTAINING_RECORD(g_lru.Flink, LINE, LruEntry);
        if (NT_SUCCESS(write_line(l))) {
            unlink_line(l);
            return l;
        }
    }
    return NULL;
}

static void release_line(LINE *l)
{
    l->Owner = (CACHE *)g_free;
    g_free = l;
}

/* Find or load a line. If 'fill' is false the caller overwrites it entirely. */
static NTSTATUS get_line(CACHE *c, uint64_t line, bool fill, LINE **out)
{
    LINE *l = find(c, line);
    if (l) {
        g_stats.Hits++;
        RemoveEntryList(&l->LruEntry);
        InsertTailList(&g_lru, &l->LruEntry);
        *out = l;
        return STATUS_SUCCESS;
    }
    g_stats.Misses++;
    l = alloc_line();
    if (!l)
        return STATUS_INSUFFICIENT_RESOURCES;
    uint64_t start = line * CC_LINE_SIZE, size = disk_bytes(c);
    l->Valid = (uint32_t)MIN((uint64_t)CC_LINE_SIZE, size - start);
    l->LineNo = line;
    l->Dirty = false;
    if (fill) {
        NTSTATUS st = DiskRawRead(c->Disk, start, l->Valid, l->Data);
        if (!NT_SUCCESS(st)) {
            release_line(l);
            return st;
        }
        g_stats.ReadsFromDisk++;
    }
    l->Owner = c;
    InsertTailList(&g_hash[bucket(c, line)], &l->HashEntry);
    InsertTailList(&g_lru, &l->LruEntry);
    InsertTailList(&c->Lines, &l->CacheEntry);
    *out = l;
    return STATUS_SUCCESS;
}

/* Write back the dirty lines of c in [first, last]. Consecutive dirty lines
   are written with one disk request. */
static NTSTATUS flush_range(CACHE *c, uint64_t first, uint64_t last)
{
    if (!c->DirtyCount)
        return STATUS_SUCCESS;
    uint64_t n = 0;
    for (LIST_ENTRY *e = c->Lines.Flink; e != &c->Lines; e = e->Flink) {
        LINE *l = CONTAINING_RECORD(e, LINE, CacheEntry);
        n += l->Dirty && l->LineNo >= first && l->LineNo <= last;
    }
    if (!n)
        return STATUS_SUCCESS;
    LINE **v = ExAllocatePoolWithTag(NonPagedPool, n * sizeof(LINE *), 'CcFl');
    uint8_t *buf = ExAllocatePoolWithTag(NonPagedPool, RUN_LINES * CC_LINE_SIZE, 'CcFl');
    NTSTATUS result = STATUS_SUCCESS;
    if (!v || !buf) {
        /* Low on memory: one line at a time. */
        for (LIST_ENTRY *e = c->Lines.Flink; e != &c->Lines; e = e->Flink) {
            LINE *l = CONTAINING_RECORD(e, LINE, CacheEntry);
            if (l->Dirty && l->LineNo >= first && l->LineNo <= last) {
                NTSTATUS st = write_line(l);
                if (!NT_SUCCESS(st))
                    result = st;
            }
        }
        goto out;
    }
    uint64_t k = 0;
    for (LIST_ENTRY *e = c->Lines.Flink; e != &c->Lines; e = e->Flink) {
        LINE *l = CONTAINING_RECORD(e, LINE, CacheEntry);
        if (l->Dirty && l->LineNo >= first && l->LineNo <= last)
            v[k++] = l;
    }
    /* Sort by line number (insertion sort: the list is mostly in order). */
    for (uint64_t i = 1; i < n; i++) {
        LINE *x = v[i];
        uint64_t j = i;
        while (j > 0 && v[j - 1]->LineNo > x->LineNo) {
            v[j] = v[j - 1];
            j--;
        }
        v[j] = x;
    }
    for (uint64_t i = 0; i < n;) {
        uint64_t j = i + 1;
        while (j < n && j - i < RUN_LINES && v[j]->LineNo == v[j - 1]->LineNo + 1 && v[j - 1]->Valid == CC_LINE_SIZE)
            j++;
        uint32_t bytes = 0;
        for (uint64_t m = i; m < j; m++) {
            memcpy(buf + bytes, v[m]->Data, v[m]->Valid);
            bytes += v[m]->Valid;
        }
        NTSTATUS st = DiskRawWrite(c->Disk, v[i]->LineNo * CC_LINE_SIZE, bytes, buf);
        if (NT_SUCCESS(st)) {
            for (uint64_t m = i; m < j; m++) {
                v[m]->Dirty = false;
                c->DirtyCount--;
            }
            g_stats.WritesToDisk++;
        } else {
            result = st;
        }
        i = j;
    }
out:
    if (v) ExFreePoolWithTag(v, 'CcFl');
    if (buf) ExFreePoolWithTag(buf, 'CcFl');
    return result;
}

/* Read a run of missing lines [line, line + count) with one disk request. */
static NTSTATUS fill_run(CACHE *c, uint64_t line, uint32_t count)
{
    uint64_t size = disk_bytes(c), start = line * CC_LINE_SIZE;
    uint32_t bytes = (uint32_t)MIN((uint64_t)count * CC_LINE_SIZE, size - start);
    uint8_t *buf = ExAllocatePoolWithTag(NonPagedPool, bytes, 'CcRd');
    if (!buf)
        return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS st = DiskRawRead(c->Disk, start, bytes, buf);
    if (NT_SUCCESS(st)) {
        g_stats.ReadsFromDisk++;
        for (uint32_t i = 0; i < count && NT_SUCCESS(st); i++) {
            LINE *l;
            st = get_line(c, line + i, false, &l);
            if (NT_SUCCESS(st))
                memcpy(l->Data, buf + (uint64_t)i * CC_LINE_SIZE, l->Valid);
        }
    }
    ExFreePoolWithTag(buf, 'CcRd');
    return st;
}

NTSTATUS CcRead(CACHE *c, uint64_t offset, uint32_t length, void *buffer)
{
    uint8_t *out = buffer;
    LOCK();
    NTSTATUS st = STATUS_SUCCESS;
    if (length >= BYPASS_BYTES) {
        /* Make the disk current for this range, then read it directly. */
        st = flush_range(c, offset / CC_LINE_SIZE, (offset + length - 1) / CC_LINE_SIZE);
        if (NT_SUCCESS(st))
            st = DiskRawRead(c->Disk, offset, length, buffer);
        g_stats.BypassReads++;
        UNLOCK();
        return st;
    }
    while (length) {
        uint64_t line = offset / CC_LINE_SIZE;
        uint32_t in = (uint32_t)(offset % CC_LINE_SIZE);
        uint32_t n = MIN(length, CC_LINE_SIZE - in);
        LINE *l;
        if (!find(c, line)) {
            /* Fill this and the following missing lines of the request at once. */
            uint64_t last = (offset + length - 1) / CC_LINE_SIZE;
            uint32_t run = 1;
            while (run < RUN_LINES && line + run <= last && !find(c, line + run))
                run++;
            if (run > 1) {
                st = fill_run(c, line, run);
                if (!NT_SUCCESS(st))
                    break;
            }
        }
        st = get_line(c, line, true, &l);
        if (!NT_SUCCESS(st))
            break;
        memcpy(out, l->Data + in, n);
        out += n;
        offset += n;
        length -= n;
    }
    UNLOCK();
    return st;
}

NTSTATUS CcWrite(CACHE *c, uint64_t offset, uint32_t length, const void *buffer)
{
    const uint8_t *in = buffer;
    LOCK();
    NTSTATUS st = STATUS_SUCCESS;
    if (length >= BYPASS_BYTES) {
        st = DiskRawWrite(c->Disk, offset, length, buffer);
        g_stats.BypassWrites++;
        if (NT_SUCCESS(st)) {
            /* Keep cached copies of the range up to date. */
            for (LIST_ENTRY *e = c->Lines.Flink; e != &c->Lines; e = e->Flink) {
                LINE *l = CONTAINING_RECORD(e, LINE, CacheEntry);
                uint64_t ls = l->LineNo * CC_LINE_SIZE, le = ls + l->Valid;
                uint64_t s = MAX(ls, offset), en = MIN(le, offset + length);
                if (s < en)
                    memcpy(l->Data + (s - ls), in + (s - offset), en - s);
            }
        }
        UNLOCK();
        return st;
    }
    while (length) {
        uint64_t line = offset / CC_LINE_SIZE;
        uint32_t at = (uint32_t)(offset % CC_LINE_SIZE);
        uint32_t n = MIN(length, CC_LINE_SIZE - at);
        uint64_t size = disk_bytes(c);
        uint32_t valid = (uint32_t)MIN((uint64_t)CC_LINE_SIZE, size - line * CC_LINE_SIZE);
        LINE *l;
        st = get_line(c, line, !(at == 0 && n >= valid), &l);
        if (!NT_SUCCESS(st))
            break;
        memcpy(l->Data + at, in, n);
        if (!l->Dirty) {
            l->Dirty = true;
            c->DirtyCount++;
        }
        if (c->WriteThrough) {
            st = write_line(l);
            if (!NT_SUCCESS(st))
                break;
        }
        in += n;
        offset += n;
        length -= n;
    }
    UNLOCK();
    return st;
}

NTSTATUS CcFlush(CACHE *c)
{
    LOCK();
    NTSTATUS st = flush_range(c, 0, UINT64_MAX);
    UNLOCK();
    return st;
}

void CcInvalidate(CACHE *c)
{
    LOCK();
    while (!IsListEmpty(&c->Lines)) {
        LINE *l = CONTAINING_RECORD(c->Lines.Flink, LINE, CacheEntry);
        unlink_line(l);
        release_line(l);
    }
    c->DirtyCount = 0;
    UNLOCK();
}

CACHE *CcCreate(DEVICE_OBJECT *disk, bool write_through)
{
    CACHE *c = ExAllocatePoolWithTag(NonPagedPoolZeroed, sizeof(CACHE), 'CcCa');
    if (!c)
        return NULL;
    c->Disk = disk;
    c->WriteThrough = write_through;
    InitializeListHead(&c->Lines);
    LOCK();
    InsertTailList(&g_caches, &c->AllEntry);
    UNLOCK();
    return c;
}

NTSTATUS CcFlushAll(void)
{
    NTSTATUS result = STATUS_SUCCESS;
    LOCK();
    for (LIST_ENTRY *e = g_caches.Flink; e != &g_caches; e = e->Flink) {
        CACHE *c = CONTAINING_RECORD(e, CACHE, AllEntry);
        if (c->DirtyCount) {
            NTSTATUS st = flush_range(c, 0, UINT64_MAX);
            if (!NT_SUCCESS(st))
                result = st;
        }
    }
    UNLOCK();
    return result;
}

void CcQueryStats(CC_STATS *s)
{
    LOCK();
    *s = g_stats;
    s->Lines = g_lines;
    s->CapacityLines = g_capacity;
    s->DirtyLines = 0;
    for (LIST_ENTRY *e = g_caches.Flink; e != &g_caches; e = e->Flink)
        s->DirtyLines += CONTAINING_RECORD(e, CACHE, AllEntry)->DirtyCount;
    UNLOCK();
}

static void lazy_writer(void *ctx)
{
    UNUSED(ctx);
    for (;;) {
        KeSleepMs(1000);
        NTSTATUS st = CcFlushAll();
        if (!NT_SUCCESS(st))
            DbgPrint("Cc: lazy writer could not write some data back: %s", RtlStatusName(st));
    }
}

void CcStartLazyWriter(void)
{
    if (!NT_SUCCESS(PsCreateSystemThread(NULL, lazy_writer, NULL, 9, "CcLazyWriter", NULL)))
        DbgPrint("Cc: could not start the lazy writer; dirty data is written on flush only");
}

void CcInitialize(void)
{
    KeInitializeMutex(&g_lock);
    for (int i = 0; i < HASH_BUCKETS; i++)
        InitializeListHead(&g_hash[i]);
    InitializeListHead(&g_lru);
    InitializeListHead(&g_caches);
    MM_STATS m;
    MmQueryStats(&m);
    /* 1/16 of RAM, between 1 MiB and 64 MiB. */
    uint64_t bytes = m.TotalPages * PAGE_SIZE / 16;
    bytes = MAX(bytes, 1ull << 20);
    bytes = MIN(bytes, 64ull << 20);
    g_capacity = bytes / CC_LINE_SIZE;
}
