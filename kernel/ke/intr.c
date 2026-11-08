/* SPDX-License-Identifier: MIT */
/*
 * Ke - interrupt objects and dispatch for vectors 0x20 and up.
 *
 * A hardware interrupt runs its handler at IRQL = vector >> 4 with
 * interrupts enabled, so higher-priority interrupts can still come in.
 * On the way out, if the interrupted code was below DISPATCH_LEVEL, queued
 * DPCs run and a pending thread switch happens (preemption).
 */
#include "ke.h"
#include "hal.h"
#include "console.h"

void KiLowerIrqlFromInterrupt(KIRQL irql);

typedef struct {
    KINTERRUPT_ROUTINE Routine;
    void *Context;
    uint64_t Count;
    bool Reserved;
    bool Reported;
} KINTERRUPT;

static KINTERRUPT g_vectors[256];
static KSPIN_LOCK g_lock;
volatile uint64_t KeTickCount;

uint8_t KeAllocateVectorAtIrql(KIRQL irql)
{
    if (irql < 3) irql = 3;
    if (irql > 12) irql = 12;
    KIRQL old = KeAcquireSpinLock(&g_lock);
    /* Try the requested level first, then the levels below and above it. */
    for (int delta = 0; delta < 10; delta++) {
        for (int sign = -1; sign <= 1; sign += 2) {
            int level = irql + sign * delta;
            if (level < 3 || level > 12 || (delta == 0 && sign == 1))
                continue;
            for (unsigned v = (unsigned)level << 4; v < ((unsigned)level << 4) + 16; v++) {
                if (!g_vectors[v].Routine && !g_vectors[v].Reserved) {
                    g_vectors[v].Reserved = true;
                    KeReleaseSpinLock(&g_lock, old);
                    return (uint8_t)v;
                }
            }
        }
    }
    KeReleaseSpinLock(&g_lock, old);
    return 0;
}

uint8_t KeAllocateVector(void)
{
    return KeAllocateVectorAtIrql(5);
}

void KeConnectInterrupt(uint8_t vector, KINTERRUPT_ROUTINE routine, void *context)
{
    KIRQL old = KeAcquireSpinLockRaiseToHigh(&g_lock);
    g_vectors[vector].Context = context;
    g_vectors[vector].Routine = routine;
    g_vectors[vector].Reserved = true;
    KeReleaseSpinLock(&g_lock, old);
}

void KeDisconnectInterrupt(uint8_t vector)
{
    KIRQL old = KeAcquireSpinLockRaiseToHigh(&g_lock);
    g_vectors[vector].Routine = NULL;
    g_vectors[vector].Context = NULL;
    g_vectors[vector].Reserved = false;
    KeReleaseSpinLock(&g_lock, old);
}

uint64_t KeQueryInterruptCount(uint8_t vector)
{
    return g_vectors[vector].Count;
}

/* Called from KiDispatchInterrupt for every vector >= 0x20 (interrupts off). */
void KiDispatchDeviceInterrupt(KTRAP_FRAME *f)
{
    uint8_t v = (uint8_t)f->Vector;
    KINTERRUPT *k = &g_vectors[v];
    k->Count++;

    if (v == VECTOR_SPURIOUS)
        return;                                 /* no EOI for spurious interrupts */
    if (v >= 0x20 && v < 0x30)
        return;                                 /* stray interrupt from the masked 8259 */

    KIRQL old = KeGetCurrentIrql();
    KIRQL level = v >> 4;
    if (level < old)
        level = old;
    __asm__ volatile("mov %0, %%cr8" :: "r"((uint64_t)level) : "memory");
    cpu_sti();

    if (k->Routine) {
        k->Routine(f, k->Context);
    } else if (v == VECTOR_APIC_ERROR) {
        DbgPrint("HAL: local APIC error interrupt");
    } else if (!k->Reported) {
        k->Reported = true;
        DbgPrint("Ke: unexpected interrupt on vector 0x%02x", v);
    }

    cpu_cli();
    HalEndOfInterrupt();
    KiLowerIrqlFromInterrupt(old);
}
