/* SPDX-License-Identifier: MIT */
/* x86-64 architecture definitions. */
#pragma once
#include "xenon.h"

#define SEL_KERNEL_CODE   0x08
#define SEL_KERNEL_DATA   0x10
#define SEL_USER_DATA     0x1B
#define SEL_USER_CODE     0x23
#define SEL_TSS           0x28

#define IST_DOUBLE_FAULT  1
#define IST_NMI           2
#define IST_MACHINE_CHECK 3

#define KERNEL_BASE       0xFFFFFFFF80000000ull

void KiLoadGdt(void *gdtr, uint16_t code_sel, uint16_t data_sel);   /* isr.asm */
extern uint64_t KiIsrTable[256];                                    /* isr.asm */

/* Legacy 8259 PIC: remapped to vectors 0x20-0x2F and fully masked. */
void HalPicInitMasked(void);

NORETURN void KiSwitchStack(void *stack_top, void (*continuation)(void));   /* stack.asm */
