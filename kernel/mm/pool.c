/* SPDX-License-Identifier: MIT */
/*
 * Mm - kernel pool (ExAllocatePoolWithTag / ExFreePoolWithTag).
 *
 * Small blocks (up to 2 KiB including the header) come from per-size free
 * lists that are refilled one page at a time. Bigger blocks get whole pages
 * of their own, which are returned to the page allocator when freed.
 *
 * Every block starts with a 16-byte header holding the tag, the size and a
 * magic value. Freeing checks the header, so a double free or an overwritten
 * header stops the system (BAD_POOL_CALLER / BAD_POOL_HEADER) right where it
 * happens instead of corrupting memory silently. Freed small blocks are filled
 * with a pattern that is checked when the block is handed out again, which
 * catches writes after free.
 */
#include "mm.h"
#include "ke.h"
#include "rtl.h"

#define POOL_MAGIC_LIVE   0x504Cu      /* "LP" */
#define POOL_MAGIC_FREE   0x4650u      /* "PF" */
#define POOL_FREE_FILL    0xDF
#define LARGE_CLASS       0xFFFF
#define NUM_CLASSES       7            /* 32 .. 2048 */
#define MAX_TAGS          128

typedef struct POOL_HEADER {
    uint32_t Tag;
    uint16_t Class;          /* size class index, or LARGE_CLASS */
    uint16_t Magic;
    uint32_t Size;           /* bytes requested */
    uint32_t Pages;          /* LARGE_CLASS only */
} POOL_HEADER;

_Static_assert(sizeof(POOL_HEADER) == 16, "pool header must stay 16 bytes");

typedef struct FREE_BLOCK {
    POOL_HEADER Header;
    struct FREE_BLOCK *Next;
} FREE_BLOCK;

static const uint32_t g_class_size[NUM_CLASSES] = { 32, 64, 128, 256, 512, 1024, 2048 };
static FREE_BLOCK *g_free[NUM_CLASSES];
static KSPIN_LOCK g_lock;
static POOL_STATS g_stats;
static POOL_TAG_INFO g_tags[MAX_TAGS];
static unsigned g_tag_count;
static bool g_check_free_fill = true;

static void tag_account(uint32_t tag, int64_t bytes, int count)
{
    for (unsigned i = 0; i < g_tag_count; i++) {
        if (g_tags[i].Tag == tag) {
            g_tags[i].Bytes += bytes;
            g_tags[i].Allocations += count;
            return;
        }
    }
    if (count > 0 && g_tag_count < MAX_TAGS)
        g_tags[g_tag_count++] = (POOL_TAG_INFO){ .Tag = tag, .Allocations = 1, .Bytes = (uint64_t)bytes };
}

static bool refill(int cls)
{
    uint64_t pa = MmAllocatePhysicalPages(1, MM_ANY_ADDRESS, 0);
    if (!pa)
        return false;
    uint8_t *page = MmPhysToVirt(pa);
    uint32_t size = g_class_size[cls];
    for (uint32_t off = 0; off + size <= PAGE_SIZE; off += size) {
        FREE_BLOCK *b = (FREE_BLOCK *)(page + off);
        memset(b, POOL_FREE_FILL, size);
        b->Header = (POOL_HEADER){ .Tag = 0, .Class = (uint16_t)cls, .Magic = POOL_MAGIC_FREE };
        b->Next = g_free[cls];
        g_free[cls] = b;
    }
    g_stats.PagesInPool++;
    return true;
}

static void check_free_fill(FREE_BLOCK *b, uint32_t size)
{
    const uint8_t *p = (const uint8_t *)b + sizeof(FREE_BLOCK);
    for (uint32_t i = 0; i < size - sizeof(FREE_BLOCK); i++) {
        if (p[i] != POOL_FREE_FILL) {
            /* Somebody wrote to this block after freeing it. */
            KeBugCheckEx(BAD_POOL_HEADER, 0x20, (uint64_t)b, i + sizeof(FREE_BLOCK), b->Header.Tag);
        }
    }
}

void *ExAllocatePoolWithTag(POOL_TYPE type, size_t size, uint32_t tag)
{
    if (size == 0 || size > 0xFFFFFFF0u)
        return NULL;
    size_t need = size + sizeof(POOL_HEADER);
    POOL_HEADER *h;

    if (need > g_class_size[NUM_CLASSES - 1]) {
        size_t pages = BYTES_TO_PAGES(need);
        uint64_t pa = MmAllocatePhysicalPages(pages, MM_ANY_ADDRESS, 0);
        if (!pa)
            return NULL;
        h = MmPhysToVirt(pa);
        *h = (POOL_HEADER){ .Tag = tag, .Class = LARGE_CLASS, .Magic = POOL_MAGIC_LIVE,
                            .Size = (uint32_t)size, .Pages = (uint32_t)pages };
        KIRQL_STATE s = KeAcquireSpinLock(&g_lock);
        g_stats.PagesInPool += pages;
        g_stats.LiveAllocations++;
        g_stats.LiveBytes += size;
        g_stats.TotalAllocations++;
        tag_account(tag, (int64_t)size, 1);
        KeReleaseSpinLock(&g_lock, s);
        return h + 1;               /* large blocks come back zeroed already */
    }

    int cls = 0;
    while (g_class_size[cls] < need)
        cls++;

    KIRQL_STATE s = KeAcquireSpinLock(&g_lock);
    if (!g_free[cls] && !refill(cls)) {
        KeReleaseSpinLock(&g_lock, s);
        return NULL;
    }
    FREE_BLOCK *b = g_free[cls];
    if (b->Header.Magic != POOL_MAGIC_FREE || b->Header.Class != cls)
        KeBugCheckEx(BAD_POOL_HEADER, 0x21, (uint64_t)b, b->Header.Magic, cls);
    if (g_check_free_fill)
        check_free_fill(b, g_class_size[cls]);
    g_free[cls] = b->Next;
    h = &b->Header;
    *h = (POOL_HEADER){ .Tag = tag, .Class = (uint16_t)cls, .Magic = POOL_MAGIC_LIVE, .Size = (uint32_t)size };
    g_stats.LiveAllocations++;
    g_stats.LiveBytes += size;
    g_stats.TotalAllocations++;
    tag_account(tag, (int64_t)size, 1);
    KeReleaseSpinLock(&g_lock, s);

    if (type == NonPagedPoolZeroed)
        memset(h + 1, 0, size);
    return h + 1;
}

void ExFreePoolWithTag(void *p, uint32_t tag)
{
    if (!p)
        return;
    POOL_HEADER *h = (POOL_HEADER *)p - 1;
    if (((uint64_t)p & 15) || (uint64_t)p < MmDirectMapBase)
        KeBugCheckEx(BAD_POOL_CALLER, 0x01, (uint64_t)p, 0, 0);
    if (h->Magic == POOL_MAGIC_FREE)
        KeBugCheckEx(BAD_POOL_CALLER, 0x07, (uint64_t)p, h->Tag, 0);   /* already freed */
    if (h->Magic != POOL_MAGIC_LIVE || (h->Class >= NUM_CLASSES && h->Class != LARGE_CLASS))
        KeBugCheckEx(BAD_POOL_HEADER, 0x01, (uint64_t)p, h->Magic, h->Class);
    if (tag && tag != h->Tag)
        KeBugCheckEx(BAD_POOL_CALLER, 0x0A, (uint64_t)p, h->Tag, tag);  /* wrong tag */

    KIRQL_STATE s = KeAcquireSpinLock(&g_lock);
    g_stats.LiveAllocations--;
    g_stats.LiveBytes -= h->Size;
    g_stats.TotalFrees++;
    tag_account(h->Tag, -(int64_t)h->Size, -1);

    if (h->Class == LARGE_CLASS) {
        uint32_t pages = h->Pages;
        g_stats.PagesInPool -= pages;
        h->Magic = POOL_MAGIC_FREE;
        KeReleaseSpinLock(&g_lock, s);
        MmFreePhysicalPages(MmVirtToPhysDirect(h), pages);
        return;
    }

    int cls = h->Class;
    FREE_BLOCK *b = (FREE_BLOCK *)h;
    memset(b, POOL_FREE_FILL, g_class_size[cls]);
    b->Header = (POOL_HEADER){ .Tag = 0, .Class = (uint16_t)cls, .Magic = POOL_MAGIC_FREE };
    b->Next = g_free[cls];
    g_free[cls] = b;
    KeReleaseSpinLock(&g_lock, s);
}

void ExQueryPoolStats(POOL_STATS *st)
{
    KIRQL_STATE s = KeAcquireSpinLock(&g_lock);
    *st = g_stats;
    KeReleaseSpinLock(&g_lock, s);
}

unsigned ExQueryPoolTags(POOL_TAG_INFO *out, unsigned max)
{
    KIRQL_STATE s = KeAcquireSpinLock(&g_lock);
    unsigned n = 0;
    for (unsigned i = 0; i < g_tag_count && n < max; i++)
        if (g_tags[i].Allocations)
            out[n++] = g_tags[i];
    KeReleaseSpinLock(&g_lock, s);
    return n;
}

void ExInitializePool(void)
{
    memset(&g_stats, 0, sizeof(g_stats));
}
