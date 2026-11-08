/*
 * Mm - memory manager.
 *
 * Physical memory:  a bitmap page allocator with address limits for DMA
 *                   (below 16 MiB for ISA/floppy, below 4 GiB for 32-bit
 *                   USB controllers).
 * Virtual memory:   4-level paging. All RAM is mapped at the direct-map
 *                   offset (MM_DIRECT_MAP_BASE, the same as Limine's HHDM).
 * Pool:             ExAllocatePoolWithTag / ExFreePoolWithTag, as in NT.
 *
 * Kernel virtual address layout:
 *   ffff800000000000  direct map of all physical RAM (and the framebuffer)
 *   ffffb00000000000  kernel stacks (each with an unmapped guard page)
 *   ffffc00000000000  device memory (MmMapIoSpace)
 *   ffffffff80000000  kernel image
 */
#pragma once
#include "xenon.h"

#define PAGE_SIZE           4096ull
#define PAGE_SHIFT          12
#define LARGE_PAGE_SIZE     (2ull * 1024 * 1024)
#define BYTES_TO_PAGES(b)   (((b) + PAGE_SIZE - 1) >> PAGE_SHIFT)

#define MM_STACK_BASE       0xFFFFB00000000000ull
#define MM_IO_BASE          0xFFFFC00000000000ull
#define MM_IO_SIZE          0x0000010000000000ull   /* 1 TiB */

extern uint64_t MmDirectMapBase;

static inline void *MmPhysToVirt(uint64_t pa) { return (void *)(pa + MmDirectMapBase); }
static inline uint64_t MmVirtToPhysDirect(const void *va) { return (uint64_t)va - MmDirectMapBase; }

/* ---- Physical pages ----------------------------------------------------- */

#define MM_ANY_ADDRESS      (~0ull)
#define MM_BELOW_16M        (16ull << 20)
#define MM_BELOW_4G         (4ull << 30)

/* Allocate 'count' physically contiguous pages that end at or below 'limit'
   and start on an 'align'-byte boundary (power of two, >= PAGE_SIZE or 0).
   The pages are zeroed. Returns the physical address, or 0 if out of memory. */
uint64_t MmAllocatePhysicalPages(size_t count, uint64_t limit, uint64_t align);
void     MmFreePhysicalPages(uint64_t pa, size_t count);
/* ISA DMA buffers must not cross a 64 KiB boundary. */
uint64_t MmAllocateIsaDmaBuffer(size_t bytes);

typedef struct {
    uint64_t TotalPages;        /* RAM managed by the allocator */
    uint64_t FreePages;
    uint64_t KernelImagePages;
    uint64_t ReclaimedPages;    /* given back by the bootloader */
    uint64_t HighestAddress;
} MM_STATS;
void MmQueryStats(MM_STATS *stats);

/* ---- Virtual memory ----------------------------------------------------- */

typedef enum { MmCached, MmNonCached, MmWriteCombined } MEMORY_CACHING_TYPE;

#define MM_MAP_WRITE    0x1
#define MM_MAP_EXEC     0x2
#define MM_MAP_USER     0x4

bool     MmMapPage(uint64_t va, uint64_t pa, unsigned flags, MEMORY_CACHING_TYPE cache);
void     MmUnmapPage(uint64_t va);
uint64_t MmGetPhysicalAddress(const void *va);   /* 0 if not mapped */

/* Map device registers (MMIO) or firmware tables into kernel space. */
void *MmMapIoSpace(uint64_t pa, size_t size, MEMORY_CACHING_TYPE cache);
void  MmUnmapIoSpace(void *va, size_t size);

/* Kernel stacks with an unmapped guard page below them. Returns the top. */
void *MmCreateKernelStack(size_t size);

/* Boot: build the kernel's own page tables and switch to them. */
void MmInitSystem(void);
/* Boot: give the bootloader's memory back (after nothing uses it any more). */
void MmReclaimBootloaderMemory(void);

/* ---- Pool --------------------------------------------------------------- */

typedef enum { NonPagedPool, NonPagedPoolZeroed } POOL_TYPE;

/* Tags are four characters written as a character constant, e.g. 'Fat '.
   Tools show them in the order they were written. */
void *ExAllocatePoolWithTag(POOL_TYPE type, size_t size, uint32_t tag);
void  ExFreePoolWithTag(void *p, uint32_t tag);
#define ExAllocatePool(type, size) ExAllocatePoolWithTag((type), (size), 'None')
#define ExFreePool(p)              ExFreePoolWithTag((p), 0)

typedef struct {
    uint32_t Tag;
    uint32_t Allocations;       /* live allocations */
    uint64_t Bytes;             /* live bytes requested */
} POOL_TAG_INFO;

typedef struct {
    uint64_t PagesInPool;       /* pages owned by the pool */
    uint64_t LiveAllocations;
    uint64_t LiveBytes;
    uint64_t TotalAllocations;
    uint64_t TotalFrees;
} POOL_STATS;

void ExQueryPoolStats(POOL_STATS *stats);
/* Copies up to 'max' tags; returns the number of tags in use. */
unsigned ExQueryPoolTags(POOL_TAG_INFO *out, unsigned max);
void ExInitializePool(void);
