/*
 * Mm - page tables and kernel virtual memory.
 *
 * At boot the kernel builds its own 4-level page tables instead of keeping
 * Limine's:
 *   - every RAM region from the memory map is mapped at the direct-map
 *     offset, using 2 MiB pages where possible (no-execute);
 *   - the framebuffer is mapped write-combining, which makes the console
 *     many times faster on real hardware;
 *   - the kernel image gets per-section permissions (code read-only and
 *     executable, read-only data, data no-execute).
 *
 * Memory types are chosen through the PAT, which is programmed as:
 *   index 0 WB, 1 WC, 2 UC-, 3 UC, 4 WB, 5 WC, 6 UC-, 7 UC
 */
#include "mm.h"
#include "ke.h"
#include "rtl.h"
#include "loader.h"

#define PTE_PRESENT   (1ull << 0)
#define PTE_WRITE     (1ull << 1)
#define PTE_USER      (1ull << 2)
#define PTE_PWT       (1ull << 3)
#define PTE_PCD       (1ull << 4)
#define PTE_LARGE     (1ull << 7)      /* PS in PD/PDPT entries */
#define PTE_GLOBAL    (1ull << 8)
#define PTE_NX        (1ull << 63)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ull

#define MSR_EFER      0xC0000080
#define MSR_PAT       0x277
#define EFER_NXE      (1ull << 11)
#define CR0_WP        (1ull << 16)
#define CR4_PGE       (1ull << 7)

extern char __kernel_start[], __text_start[], __text_end[], __rodata_start[], __rodata_end[],
            __data_start[], __kernel_end[];

void MiInitPhysicalAllocator(void);

static uint64_t g_pml4_pa;
static uint64_t g_nx;                  /* PTE_NX if the CPU supports it, else 0 */
static KSPIN_LOCK g_lock;
static uint64_t g_io_next = MM_IO_BASE;
static uint64_t g_stack_next = MM_STACK_BASE;

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t v)
{
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

static inline void invlpg(uint64_t va)
{
    __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory");
}

static uint64_t cache_bits(MEMORY_CACHING_TYPE c)
{
    switch (c) {
    case MmWriteCombined: return PTE_PWT;               /* PAT index 1 */
    case MmNonCached:     return PTE_PWT | PTE_PCD;     /* PAT index 3 */
    default:              return 0;                     /* PAT index 0 = WB */
    }
}

static uint64_t pte_flags(unsigned flags, MEMORY_CACHING_TYPE cache, uint64_t va)
{
    uint64_t f = PTE_PRESENT | cache_bits(cache);
    if (flags & MM_MAP_WRITE) f |= PTE_WRITE;
    if (!(flags & MM_MAP_EXEC)) f |= g_nx;
    if (flags & MM_MAP_USER) f |= PTE_USER;
    else if (va >= 0xFFFF800000000000ull) f |= PTE_GLOBAL;
    return f;
}

static uint64_t *table_virt(uint64_t entry)
{
    return MmPhysToVirt(entry & PTE_ADDR_MASK);
}

/* Return the next-level table for entry *e, creating it if 'create'.
   A 2 MiB / 1 GiB page in the way is split into smaller pages. */
static uint64_t *next_level(uint64_t *e, bool create, bool user, int level)
{
    if (*e & PTE_PRESENT) {
        if (!(*e & PTE_LARGE))
            return table_virt(*e);
        if (!create)
            return NULL;
        /* Split a large page into 512 pages of the next size down. */
        uint64_t pa = MmAllocatePhysicalPages(1, MM_ANY_ADDRESS, 0);
        if (!pa)
            KeBugCheckEx(NO_PAGES_AVAILABLE, 1, 0, 0, 0);
        uint64_t *t = MmPhysToVirt(pa);
        /* level 3: a 1 GiB PDPT entry becomes 2 MiB pages (still "large");
           level 2: a 2 MiB PD entry becomes 4 KiB pages. */
        uint64_t base = *e & PTE_ADDR_MASK & (level == 3 ? ~0x3FFFFFFFull : ~0x1FFFFFull);
        uint64_t keep = *e & ~PTE_ADDR_MASK;
        uint64_t step = level == 3 ? LARGE_PAGE_SIZE : PAGE_SIZE;
        if (level != 3)
            keep &= ~PTE_LARGE;
        for (int i = 0; i < 512; i++)
            t[i] = (base + i * step) | keep;
        *e = pa | PTE_PRESENT | PTE_WRITE | (user ? PTE_USER : 0);
        return t;
    }
    if (!create)
        return NULL;
    uint64_t pa = MmAllocatePhysicalPages(1, MM_ANY_ADDRESS, 0);
    if (!pa)
        KeBugCheckEx(NO_PAGES_AVAILABLE, 2, 0, 0, 0);
    *e = pa | PTE_PRESENT | PTE_WRITE | (user ? PTE_USER : 0);
    return MmPhysToVirt(pa);
}

static uint64_t *walk(uint64_t va, bool create, bool user)
{
    uint64_t *pml4 = MmPhysToVirt(g_pml4_pa);
    uint64_t *pdpt = next_level(&pml4[(va >> 39) & 511], create, user, 4);
    if (!pdpt) return NULL;
    uint64_t *pd = next_level(&pdpt[(va >> 30) & 511], create, user, 3);
    if (!pd) return NULL;
    uint64_t *pt = next_level(&pd[(va >> 21) & 511], create, user, 2);
    if (!pt) return NULL;
    return &pt[(va >> 12) & 511];
}

static bool map_page_locked(uint64_t va, uint64_t pa, unsigned flags, MEMORY_CACHING_TYPE cache)
{
    uint64_t *pte = walk(va, true, flags & MM_MAP_USER);
    if (!pte)
        return false;
    *pte = (pa & PTE_ADDR_MASK) | pte_flags(flags, cache, va);
    invlpg(va);
    return true;
}

bool MmMapPage(uint64_t va, uint64_t pa, unsigned flags, MEMORY_CACHING_TYPE cache)
{
    KIRQL_STATE s = KeAcquireSpinLock(&g_lock);
    bool ok = map_page_locked(va, pa, flags, cache);
    KeReleaseSpinLock(&g_lock, s);
    return ok;
}

void MmUnmapPage(uint64_t va)
{
    KIRQL_STATE s = KeAcquireSpinLock(&g_lock);
    uint64_t *pte = walk(va, false, false);
    if (pte) {
        *pte = 0;
        invlpg(va);
    }
    KeReleaseSpinLock(&g_lock, s);
}

uint64_t MmGetPhysicalAddress(const void *p)
{
    uint64_t va = (uint64_t)p;
    uint64_t *pml4 = MmPhysToVirt(g_pml4_pa);
    uint64_t e = pml4[(va >> 39) & 511];
    if (!(e & PTE_PRESENT)) return 0;
    e = table_virt(e)[(va >> 30) & 511];
    if (!(e & PTE_PRESENT)) return 0;
    if (e & PTE_LARGE) return (e & PTE_ADDR_MASK & ~0x3FFFFFFFull) | (va & 0x3FFFFFFF);
    e = table_virt(e)[(va >> 21) & 511];
    if (!(e & PTE_PRESENT)) return 0;
    if (e & PTE_LARGE) return (e & PTE_ADDR_MASK & ~0x1FFFFFull) | (va & 0x1FFFFF);
    e = table_virt(e)[(va >> 12) & 511];
    if (!(e & PTE_PRESENT)) return 0;
    return (e & PTE_ADDR_MASK) | (va & 0xFFF);
}

/* Map [pa, pa+size) at va, using 2 MiB pages where both addresses allow. */
static void map_range(uint64_t va, uint64_t pa, uint64_t size, unsigned flags, MEMORY_CACHING_TYPE cache)
{
    uint64_t end = pa + size;
    while (pa < end) {
        if ((va & (LARGE_PAGE_SIZE - 1)) == 0 && (pa & (LARGE_PAGE_SIZE - 1)) == 0 && end - pa >= LARGE_PAGE_SIZE) {
            uint64_t *pml4 = MmPhysToVirt(g_pml4_pa);
            uint64_t *pdpt = next_level(&pml4[(va >> 39) & 511], true, false, 4);
            uint64_t *pd = next_level(&pdpt[(va >> 30) & 511], true, false, 3);
            pd[(va >> 21) & 511] = pa | pte_flags(flags, cache, va) | PTE_LARGE;
            va += LARGE_PAGE_SIZE;
            pa += LARGE_PAGE_SIZE;
        } else {
            map_page_locked(va, pa, flags, cache);
            va += PAGE_SIZE;
            pa += PAGE_SIZE;
        }
    }
}

static void map_kernel_section(char *start, char *end, unsigned flags)
{
    LOADER_BLOCK *lb = &KeLoaderBlock;
    uint64_t vs = ALIGN_DOWN((uint64_t)start, PAGE_SIZE);
    uint64_t ve = ALIGN_UP((uint64_t)end, PAGE_SIZE);
    for (uint64_t va = vs; va < ve; va += PAGE_SIZE)
        map_page_locked(va, va - lb->KernelVirtualBase + lb->KernelPhysicalBase, flags, MmCached);
}

void MmInitSystem(void)
{
    LOADER_BLOCK *lb = &KeLoaderBlock;
    MiInitPhysicalAllocator();

    /* CPU features: NX, PAT, global pages, write protection in ring 0. */
    uint32_t a, b, c, d;
    cpuid(0x80000001, 0, &a, &b, &c, &d);
    if (d & (1u << 20)) {
        wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE);
        g_nx = PTE_NX;
    }
    /* PAT: WB, WC, UC-, UC, WB, WC, UC-, UC */
    wrmsr(MSR_PAT, 0x0007010600070106ull);

    g_pml4_pa = MmAllocatePhysicalPages(1, MM_ANY_ADDRESS, 0);
    if (!g_pml4_pa)
        KeBugCheckEx(NO_PAGES_AVAILABLE, 3, 0, 0, 0);

    /* Direct map of RAM, firmware data and the framebuffer. */
    for (uint32_t i = 0; i < lb->MemoryMapCount; i++) {
        LOADER_MEMORY_DESCRIPTOR *m = &lb->MemoryMap[i];
        MEMORY_CACHING_TYPE cache = MmCached;
        switch (m->Type) {
        case MEM_USABLE:
        case MEM_BOOTLOADER_RECLAIMABLE:
        case MEM_ACPI_RECLAIMABLE:
        case MEM_ACPI_NVS:
        case MEM_KERNEL_AND_MODULES:
            break;
        case MEM_FRAMEBUFFER:
            cache = MmWriteCombined;
            break;
        default:
            continue;
        }
        uint64_t base = ALIGN_DOWN(m->Base, PAGE_SIZE);
        uint64_t end = ALIGN_UP(m->Base + m->Length, PAGE_SIZE);
        map_range(base + MmDirectMapBase, base, end - base, MM_MAP_WRITE, cache);
    }
    /* Some firmware reports the framebuffer as reserved: map it explicitly. */
    if (lb->HaveFramebuffer) {
        uint64_t base = ALIGN_DOWN(lb->Framebuffer.PhysicalAddress, PAGE_SIZE);
        uint64_t end = ALIGN_UP(lb->Framebuffer.PhysicalAddress + lb->Framebuffer.Pitch * lb->Framebuffer.Height,
                                PAGE_SIZE);
        for (uint64_t pa = base; pa < end; pa += PAGE_SIZE)
            map_page_locked(pa + MmDirectMapBase, pa, MM_MAP_WRITE, MmWriteCombined);
    }

    /* Kernel image with per-section permissions. */
    map_kernel_section(__kernel_start, __text_start, MM_MAP_WRITE);        /* Limine requests */
    map_kernel_section(__text_start, __text_end, MM_MAP_EXEC);
    map_kernel_section(__rodata_start, __rodata_end, 0);
    map_kernel_section(__data_start, __kernel_end, MM_MAP_WRITE);

    __asm__ volatile("mov %0, %%cr0" :: "r"(read_cr0() | CR0_WP));
    __asm__ volatile("mov %0, %%cr3" :: "r"(g_pml4_pa) : "memory");
    __asm__ volatile("mov %0, %%cr4" :: "r"(read_cr4() | CR4_PGE));
}

void *MmMapIoSpace(uint64_t pa, size_t size, MEMORY_CACHING_TYPE cache)
{
    if (size == 0)
        return NULL;
    uint64_t first = ALIGN_DOWN(pa, PAGE_SIZE);
    uint64_t pages = BYTES_TO_PAGES(pa + size - first);
    KIRQL_STATE s = KeAcquireSpinLock(&g_lock);
    uint64_t va = g_io_next;
    if (va + (pages + 1) * PAGE_SIZE > MM_IO_BASE + MM_IO_SIZE) {
        KeReleaseSpinLock(&g_lock, s);
        return NULL;
    }
    g_io_next += (pages + 1) * PAGE_SIZE;           /* +1: unmapped guard page */
    for (uint64_t i = 0; i < pages; i++)
        map_page_locked(va + i * PAGE_SIZE, first + i * PAGE_SIZE, MM_MAP_WRITE, cache);
    KeReleaseSpinLock(&g_lock, s);
    return (void *)(va + (pa - first));
}

void MmUnmapIoSpace(void *p, size_t size)
{
    uint64_t va = ALIGN_DOWN((uint64_t)p, PAGE_SIZE);
    uint64_t pages = BYTES_TO_PAGES((uint64_t)p + size - va);
    for (uint64_t i = 0; i < pages; i++)
        MmUnmapPage(va + i * PAGE_SIZE);
}

void *MmCreateKernelStack(size_t size)
{
    size_t pages = BYTES_TO_PAGES(size);
    KIRQL_STATE s = KeAcquireSpinLock(&g_lock);
    uint64_t base = g_stack_next + PAGE_SIZE;       /* guard page stays unmapped */
    g_stack_next += (pages + 1) * PAGE_SIZE;
    for (size_t i = 0; i < pages; i++) {
        uint64_t pa = MmAllocatePhysicalPages(1, MM_ANY_ADDRESS, 0);
        if (!pa) {
            KeReleaseSpinLock(&g_lock, s);
            return NULL;
        }
        map_page_locked(base + i * PAGE_SIZE, pa, MM_MAP_WRITE, MmCached);
    }
    KeReleaseSpinLock(&g_lock, s);
    return (void *)(base + pages * PAGE_SIZE);
}
