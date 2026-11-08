/* SPDX-License-Identifier: MIT */
/*
 * HAL - legacy 8259 PIC.
 *
 * The PIC is remapped away from the CPU exception vectors and then fully
 * masked. Phase 1.1 switches interrupt delivery to the local APIC and IOAPIC.
 */
#include "hal.h"
#include "../arch/x86_64/arch.h"

void HalPicInitMasked(void)
{
    outb(0x20, 0x11); io_wait();     /* ICW1: init, ICW4 needed */
    outb(0xA0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait();     /* ICW2: master base vector 0x20 */
    outb(0xA1, 0x28); io_wait();     /* ICW2: slave base vector 0x28 */
    outb(0x21, 0x04); io_wait();     /* ICW3: slave on IRQ2 */
    outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait();     /* ICW4: 8086 mode */
    outb(0xA1, 0x01); io_wait();
    outb(0x21, 0xFF);                /* mask everything */
    outb(0xA1, 0xFF);
}
