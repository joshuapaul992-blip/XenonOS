/*
 * Ke - interrupt objects and dispatch for vectors 0x20 and up.
 */
#include "ke.h"
#include "hal.h"
#include "console.h"

typedef struct {
    KINTERRUPT_ROUTINE Routine;
    void *Context;
    uint64_t Count;
    bool Reported;
} KINTERRUPT;

static KINTERRUPT g_vectors[256];
static KSPIN_LOCK g_lock;
volatile uint64_t KeTickCount;

uint8_t KeAllocateVector(void)
{
    KIRQL_STATE s = KeAcquireSpinLock(&g_lock);
    for (unsigned v = VECTOR_DEVICE_FIRST; v <= VECTOR_DEVICE_LAST; v++) {
        if (!g_vectors[v].Routine) {
            /* Reserve it until KeConnectInterrupt fills it in. */
            g_vectors[v].Routine = (KINTERRUPT_ROUTINE)1;
            KeReleaseSpinLock(&g_lock, s);
            return (uint8_t)v;
        }
    }
    KeReleaseSpinLock(&g_lock, s);
    return 0;
}

void KeConnectInterrupt(uint8_t vector, KINTERRUPT_ROUTINE routine, void *context)
{
    KIRQL_STATE s = KeAcquireSpinLock(&g_lock);
    g_vectors[vector].Context = context;
    g_vectors[vector].Routine = routine;
    KeReleaseSpinLock(&g_lock, s);
}

void KeDisconnectInterrupt(uint8_t vector)
{
    KIRQL_STATE s = KeAcquireSpinLock(&g_lock);
    g_vectors[vector].Routine = NULL;
    g_vectors[vector].Context = NULL;
    KeReleaseSpinLock(&g_lock, s);
}

uint64_t KeQueryInterruptCount(uint8_t vector)
{
    return g_vectors[vector].Count;
}

uint64_t KeQueryUptimeNs(void)
{
    return HalGetTimeNs();
}

/* Called from KiDispatchInterrupt for every vector >= 0x20. */
void KiDispatchDeviceInterrupt(KTRAP_FRAME *f)
{
    uint8_t v = (uint8_t)f->Vector;
    KINTERRUPT *k = &g_vectors[v];
    k->Count++;

    if (v == VECTOR_SPURIOUS)
        return;                                 /* no EOI for spurious interrupts */
    if (v >= 0x20 && v < 0x30)
        return;                                 /* stray interrupt from the masked 8259 */

    if (k->Routine && k->Routine != (KINTERRUPT_ROUTINE)1) {
        k->Routine(f, k->Context);
    } else if (v == VECTOR_APIC_ERROR) {
        DbgPrint("HAL: local APIC error interrupt");
    } else if (!k->Reported) {
        k->Reported = true;
        DbgPrint("Ke: unexpected interrupt on vector 0x%02x", v);
    }
    HalEndOfInterrupt();
}
