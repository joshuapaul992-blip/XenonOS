/*
 * GDT and TSS.
 *
 * Selector layout (fixed, the SYSCALL/SYSRET MSRs in Phase 1.5 depend on it):
 *   0x00 null
 *   0x08 kernel code (64-bit)
 *   0x10 kernel data
 *   0x18 user data   (RPL 3 -> 0x1B)
 *   0x20 user code   (RPL 3 -> 0x23)
 *   0x28 TSS (16-byte descriptor)
 */
#include "ke.h"
#include "rtl.h"
#include "arch.h"

typedef struct PACKED {
    uint32_t Reserved0;
    uint64_t Rsp[3];
    uint64_t Reserved1;
    uint64_t Ist[7];
    uint64_t Reserved2;
    uint16_t Reserved3;
    uint16_t IoMapBase;
} KTSS;

static uint64_t g_gdt[7];
static KTSS g_tss;

/* Separate stacks for faults that can happen with a broken kernel stack. */
static uint8_t g_ist_double_fault[16384] __attribute__((aligned(16)));
static uint8_t g_ist_nmi[8192] __attribute__((aligned(16)));
static uint8_t g_ist_machine_check[8192] __attribute__((aligned(16)));

static uint64_t make_segment(bool code, unsigned dpl)
{
    uint64_t d = 0;
    d |= 0xFFFFull;                        /* limit 15:0 (ignored in long mode) */
    d |= (uint64_t)(code ? 0x9A : 0x92) << 40;   /* present, S, type */
    d |= (uint64_t)dpl << 45;
    d |= 0xFull << 48;                     /* limit 19:16 */
    d |= (code ? (1ull << 53) : (1ull << 54)); /* L for code, D/B for data */
    d |= 1ull << 55;                       /* 4K granularity */
    return d;
}

void KiInitGdt(void)
{
    g_gdt[0] = 0;
    g_gdt[1] = make_segment(true, 0);
    g_gdt[2] = make_segment(false, 0);
    g_gdt[3] = make_segment(false, 3);
    g_gdt[4] = make_segment(true, 3);

    memset(&g_tss, 0, sizeof(g_tss));
    g_tss.Ist[IST_DOUBLE_FAULT - 1] = (uint64_t)(g_ist_double_fault + sizeof(g_ist_double_fault));
    g_tss.Ist[IST_NMI - 1] = (uint64_t)(g_ist_nmi + sizeof(g_ist_nmi));
    g_tss.Ist[IST_MACHINE_CHECK - 1] = (uint64_t)(g_ist_machine_check + sizeof(g_ist_machine_check));
    g_tss.IoMapBase = sizeof(g_tss);

    uint64_t base = (uint64_t)&g_tss, limit = sizeof(g_tss) - 1;
    g_gdt[5] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ull << 40) |
               (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    g_gdt[6] = base >> 32;

    struct PACKED { uint16_t Limit; uint64_t Base; } gdtr = { sizeof(g_gdt) - 1, (uint64_t)g_gdt };
    KiLoadGdt(&gdtr, SEL_KERNEL_CODE, SEL_KERNEL_DATA);
    __asm__ volatile("ltr %w0" :: "r"((uint16_t)SEL_TSS));
}

/* Stack used when an interrupt arrives in user mode (set on every thread switch). */
void KiSetKernelStack(uint64_t rsp0)
{
    g_tss.Rsp[0] = rsp0;
}
