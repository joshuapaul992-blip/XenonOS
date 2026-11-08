/* SPDX-License-Identifier: MIT */
/*
 * IDT setup and first-level interrupt dispatch.
 */
#include "ke.h"
#include "rtl.h"
#include "arch.h"
#include "console.h"

void KiDispatchDeviceInterrupt(KTRAP_FRAME *f);

typedef struct PACKED {
    uint16_t OffsetLow;
    uint16_t Selector;
    uint8_t  Ist;
    uint8_t  TypeAttr;
    uint16_t OffsetMid;
    uint32_t OffsetHigh;
    uint32_t Reserved;
} KIDTENTRY;

static KIDTENTRY g_idt[256] __attribute__((aligned(16)));

static void set_gate(int vec, uint64_t handler, uint8_t ist, uint8_t dpl)
{
    g_idt[vec] = (KIDTENTRY){
        .OffsetLow = handler & 0xFFFF,
        .Selector = SEL_KERNEL_CODE,
        .Ist = ist,
        .TypeAttr = (uint8_t)(0x8E | (dpl << 5)),   /* present, 64-bit interrupt gate */
        .OffsetMid = (handler >> 16) & 0xFFFF,
        .OffsetHigh = (uint32_t)(handler >> 32),
    };
}

void KiInitIdt(void)
{
    for (int v = 0; v < 256; v++)
        set_gate(v, KiIsrTable[v], 0, 0);
    set_gate(2, KiIsrTable[2], IST_NMI, 0);
    set_gate(8, KiIsrTable[8], IST_DOUBLE_FAULT, 0);
    set_gate(18, KiIsrTable[18], IST_MACHINE_CHECK, 0);
    set_gate(3, KiIsrTable[3], 0, 3);   /* int3 allowed from user mode later */

    struct PACKED { uint16_t Limit; uint64_t Base; } idtr = { sizeof(g_idt) - 1, (uint64_t)g_idt };
    __asm__ volatile("lidt %0" :: "m"(idtr));
}

/* NT exception codes, used as parameter 1 of KMODE_EXCEPTION_NOT_HANDLED. */
static uint32_t nt_exception_code(uint64_t vec)
{
    switch (vec) {
    case 0:  return 0xC0000094;   /* STATUS_INTEGER_DIVIDE_BY_ZERO */
    case 1:  return 0x80000004;   /* STATUS_SINGLE_STEP */
    case 3:  return 0x80000003;   /* STATUS_BREAKPOINT */
    case 4:  return 0xC0000095;   /* STATUS_INTEGER_OVERFLOW */
    case 5:  return 0xC000008C;   /* STATUS_ARRAY_BOUNDS_EXCEEDED */
    case 6:  return 0xC000001D;   /* STATUS_ILLEGAL_INSTRUCTION */
    case 7:  return 0xC000008F;   /* STATUS_FLOAT_INEXACT_RESULT (no FPU context yet) */
    case 13: return 0xC0000005;   /* STATUS_ACCESS_VIOLATION */
    case 16: return 0xC0000090;   /* STATUS_FLOAT_INVALID_OPERATION */
    case 17: return 0x80000002;   /* STATUS_DATATYPE_MISALIGNMENT */
    case 19: return 0xC00002B5;   /* STATUS_FLOAT_MULTIPLE_TRAPS */
    default: return 0xC0000420;   /* STATUS_ASSERTION_FAILURE */
    }
}

void KiDispatchInterrupt(KTRAP_FRAME *f)
{
    uint64_t v = f->Vector;

    if (v < 32) {
        switch (v) {
        case 14:   /* #PF: address, error code (write/user/...), faulting RIP */
            KeBugCheckWithFrame(PAGE_FAULT_IN_NONPAGED_AREA, read_cr2(), f->ErrorCode, f->Rip, 0, f);
        case 2:    /* NMI */
        case 8:    /* #DF */
        case 10:   /* #TS */
        case 11:   /* #NP */
        case 12:   /* #SS */
        case 18:   /* #MC */
            KeBugCheckWithFrame(UNEXPECTED_KERNEL_MODE_TRAP, v, f->ErrorCode, f->Rip, 0, f);
        default:
            KeBugCheckWithFrame(KMODE_EXCEPTION_NOT_HANDLED, nt_exception_code(v), f->Rip, f->ErrorCode, v, f);
        }
    }

    KiDispatchDeviceInterrupt(f);
}
