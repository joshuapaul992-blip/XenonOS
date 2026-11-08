/* SPDX-License-Identifier: MIT */
/*
 * HAL - reset and (test-only) power off.
 */
#include "hal.h"

NORETURN void HalReboot(void)
{
    cpu_cli();
    /* Pulse the reset line through the 8042 keyboard controller. */
    for (int i = 0; i < 100000 && (inb(0x64) & 0x02); i++)
        cpu_pause();
    outb(0x64, 0xFE);
    for (volatile int i = 0; i < 1000000; i++)
        ;
    /* Fall back to a triple fault: load an empty IDT and raise an exception. */
    struct PACKED { uint16_t limit; uint64_t base; } idtr = { 0, 0 };
    __asm__ volatile("lidt %0; int3" :: "m"(idtr));
    cpu_halt_forever();
}

void HalDebugExit(uint8_t code)
{
    /* QEMU "isa-debug-exit" device; the VM exits with status (code << 1) | 1. */
    outb(0xF4, code);
}
