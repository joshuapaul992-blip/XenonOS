/* SPDX-License-Identifier: MIT */
/*
 * Mm - physical page allocator.
 *
 * One bit per 4 KiB page (1 = in use) covering every page up to the highest
 * usable address. General allocations are taken from above 16 MiB first, so
 * that low memory stays available for devices that can only reach it
 * (the floppy controller's ISA DMA needs < 16 MiB, old USB controllers need
 * < 4 GiB).
 */
#include "mm.h"
#include "ke.h"
#include "rtl.h"
#include "loader.h"
#include "console.h"

static KSPIN_LOCK g_lock;
static uint8_t *g_bitmap;           /* direct-mapped */
static uint64_t g_bitmap_pages;     /* number of pages the bitmap describes */
static uint64_t g_free_pages;
static uint64_t g_total_pages;
static uint64_t g_reclaimed_pages;
static uint64_t g_kernel_pages;
static bool     g_ready;

uint64_t MmDirectMapBase;

static inline bool page_used(uint64_t pfn)  { return g_bitmap[pfn >> 3] & (1u << (pfn & 7)); }
static inline void set_used(uint64_t pfn)   { g_bitmap[pfn >> 3] |= (uint8_t)(1u << (pfn & 7)); }
static inline void set_free(uint64_t pfn)   { g_bitmap[pfn >> 3] &= (uint8_t)~(1u << (pfn & 7)); }

static void free_range(uint64_t base, uint64_t length)
{
    uint64_t first = ALIGN_UP(base, PAGE_SIZE) >> PAGE_SHIFT;
    uint64_t last = ALIGN_DOWN(base + length, PAGE_SIZE) >> PAGE_SHIFT;
    if (first == 0)
        first = 1;                  /* never hand out physical page 0 */
    for (uint64_t pfn = first; pfn < last && pfn < g_bitmap_pages; pfn++) {
        if (page_used(pfn)) {
            set_free(pfn);
            g_free_pages++;
            g_total_pages++;
        }
    }
}

static void reserve_range(uint64_t base, uint64_t length)
{
    uint64_t first = ALIGN_DOWN(base, PAGE_SIZE) >> PAGE_SHIFT;
    uint64_t last = ALIGN_UP(base + length, PAGE_SIZE) >> PAGE_SHIFT;
    for (uint64_t pfn = first; pfn < last && pfn < g_bitmap_pages; pfn++) {
        if (!page_used(pfn)) {
            set_used(pfn);
            g_free_pages--;
        }
    }
}

/* Called by MmInitSystem before the kernel has its own page tables. Limine's
   HHDM is in place, so physical memory is reachable at MmDirectMapBase. */
void MiInitPhysicalAllocator(void)
{
    LOADER_BLOCK *lb = &KeLoaderBlock;
    MmDirectMapBase = lb->HhdmOffset;

    uint64_t top = 0;
    for (uint32_t i = 0; i < lb->MemoryMapCount; i++) {
        LOADER_MEMORY_DESCRIPTOR *d = &lb->MemoryMap[i];
        if (d->Type == MEM_USABLE || d->Type == MEM_BOOTLOADER_RECLAIMABLE)
            top = MAX(top, d->Base + d->Length);
        if (d->Type == MEM_KERNEL_AND_MODULES)
            g_kernel_pages += BYTES_TO_PAGES(d->Length);
    }
    g_bitmap_pages = top >> PAGE_SHIFT;
    uint64_t bitmap_bytes = ALIGN_UP((g_bitmap_pages + 7) / 8, PAGE_SIZE);

    /* Put the bitmap in the first usable region above 1 MiB that fits. */
    uint64_t bitmap_pa = 0;
    for (uint32_t i = 0; i < lb->MemoryMapCount; i++) {
        LOADER_MEMORY_DESCRIPTOR *d = &lb->MemoryMap[i];
        uint64_t base = MAX(d->Base, 0x100000ull);
        if (d->Type == MEM_USABLE && d->Base + d->Length > base && d->Base + d->Length - base >= bitmap_bytes) {
            bitmap_pa = ALIGN_UP(base, PAGE_SIZE);
            if (bitmap_pa + bitmap_bytes <= d->Base + d->Length)
                break;
            bitmap_pa = 0;
        }
    }
    if (!bitmap_pa)
        KeBugCheckEx(NO_PAGES_AVAILABLE, bitmap_bytes, top, 0, 0);

    g_bitmap = MmPhysToVirt(bitmap_pa);
    memset(g_bitmap, 0xFF, bitmap_bytes);          /* everything used ... */
    for (uint32_t i = 0; i < lb->MemoryMapCount; i++)   /* ... except usable RAM */
        if (lb->MemoryMap[i].Type == MEM_USABLE)
            free_range(lb->MemoryMap[i].Base, lb->MemoryMap[i].Length);
    reserve_range(bitmap_pa, bitmap_bytes);
    g_ready = true;
}

void MmReclaimBootloaderMemory(void)
{
    LOADER_BLOCK *lb = &KeLoaderBlock;
    KIRQL_STATE s = KeAcquireSpinLock(&g_lock);
    uint64_t before = g_free_pages;
    for (uint32_t i = 0; i < lb->MemoryMapCount; i++)
        if (lb->MemoryMap[i].Type == MEM_BOOTLOADER_RECLAIMABLE)
            free_range(lb->MemoryMap[i].Base, lb->MemoryMap[i].Length);
    g_reclaimed_pages = g_free_pages - before;
    KeReleaseSpinLock(&g_lock, s);
}

/* First fit of 'count' free pages in [lo_pfn, hi_pfn), aligned to align_pages. */
static uint64_t find_run(uint64_t lo_pfn, uint64_t hi_pfn, size_t count, uint64_t align_pages)
{
    hi_pfn = MIN(hi_pfn, g_bitmap_pages);
    uint64_t pfn = ALIGN_UP(MAX(lo_pfn, 1), align_pages);
    while (pfn + count <= hi_pfn) {
        /* Skip whole used bytes quickly. */
        if ((pfn & 7) == 0 && g_bitmap[pfn >> 3] == 0xFF) {
            pfn = ALIGN_UP(pfn + 8, align_pages);
            continue;
        }
        size_t n = 0;
        while (n < count && !page_used(pfn + n))
            n++;
        if (n == count)
            return pfn;
        pfn = ALIGN_UP(pfn + n + 1, align_pages);
    }
    return 0;
}

uint64_t MmAllocatePhysicalPages(size_t count, uint64_t limit, uint64_t align)
{
    if (!g_ready || count == 0)
        return 0;
    uint64_t align_pages = align > PAGE_SIZE ? align >> PAGE_SHIFT : 1;
    uint64_t limit_pfn = limit == MM_ANY_ADDRESS ? g_bitmap_pages : (limit >> PAGE_SHIFT);
    const uint64_t pfn_16m = MM_BELOW_16M >> PAGE_SHIFT;
    const uint64_t pfn_1m = 0x100000 >> PAGE_SHIFT;

    KIRQL_STATE s = KeAcquireSpinLock(&g_lock);
    uint64_t pfn = 0;
    if (limit_pfn > pfn_16m)
        pfn = find_run(pfn_16m, limit_pfn, count, align_pages);
    if (!pfn)
        pfn = find_run(pfn_1m, MIN(limit_pfn, pfn_16m), count, align_pages);
    if (!pfn)
        pfn = find_run(1, MIN(limit_pfn, pfn_1m), count, align_pages);
    if (pfn) {
        for (size_t i = 0; i < count; i++)
            set_used(pfn + i);
        g_free_pages -= count;
    }
    KeReleaseSpinLock(&g_lock, s);

    if (!pfn)
        return 0;
    uint64_t pa = pfn << PAGE_SHIFT;
    memset(MmPhysToVirt(pa), 0, count * PAGE_SIZE);
    return pa;
}

void MmFreePhysicalPages(uint64_t pa, size_t count)
{
    uint64_t pfn = pa >> PAGE_SHIFT;
    if ((pa & (PAGE_SIZE - 1)) || pfn == 0 || pfn + count > g_bitmap_pages)
        KeBugCheckEx(PFN_LIST_CORRUPT, 1, pa, count, 0);
    KIRQL_STATE s = KeAcquireSpinLock(&g_lock);
    for (size_t i = 0; i < count; i++) {
        if (!page_used(pfn + i)) {
            KeReleaseSpinLock(&g_lock, s);
            KeBugCheckEx(PFN_LIST_CORRUPT, 2, (pfn + i) << PAGE_SHIFT, count, 0);   /* double free */
        }
        set_free(pfn + i);
    }
    g_free_pages += count;
    KeReleaseSpinLock(&g_lock, s);
}

uint64_t MmAllocateIsaDmaBuffer(size_t bytes)
{
    if (bytes == 0 || bytes > 65536)
        return 0;
    size_t pages = BYTES_TO_PAGES(bytes);
    /* Aligning to the next power of two >= the size guarantees that the
       buffer never crosses a 64 KiB boundary. */
    uint64_t align = PAGE_SIZE;
    while (align < pages * PAGE_SIZE)
        align <<= 1;
    return MmAllocatePhysicalPages(pages, MM_BELOW_16M, align);
}

void MmQueryStats(MM_STATS *st)
{
    st->TotalPages = g_total_pages;
    st->FreePages = g_free_pages;
    st->KernelImagePages = g_kernel_pages;
    st->ReclaimedPages = g_reclaimed_pages;
    st->HighestAddress = g_bitmap_pages << PAGE_SHIFT;
}
