/*
 * Ob - handle tables.
 *
 * A handle is an index into the current process's table, times 4 plus 4
 * (so the first handle is 4, like on NT). Each handle holds one reference
 * on its object.
 */
#include "ob.h"
#include "ps.h"
#include "mm.h"
#include "rtl.h"

void ObpAdjustHandleCount(void *object, int delta);
static KSPIN_LOCK g_lock;

#define HANDLE_TO_INDEX(h) (((h) >> 2) - 1)
#define INDEX_TO_HANDLE(i) ((HANDLE)(((i) + 1) << 2))

void ObInitializeHandleTable(HANDLE_TABLE *t)
{
    memset(t, 0, sizeof(*t));
}

void ObKillHandleTable(HANDLE_TABLE *t)
{
    for (uint32_t i = 0; i < t->Capacity; i++) {
        if (t->Entries[i].Object) {
            void *o = t->Entries[i].Object;
            t->Entries[i].Object = NULL;
            ObpAdjustHandleCount(o, -1);
            ObDereferenceObject(o);
        }
    }
    if (t->Entries)
        ExFreePoolWithTag(t->Entries, 'ObHt');
    memset(t, 0, sizeof(*t));
}

static HANDLE_TABLE *current_table(void)
{
    EPROCESS *p = PsGetCurrentProcess();
    return p ? &p->HandleTable : NULL;
}

NTSTATUS ObCreateHandle(void *object, HANDLE *handle)
{
    HANDLE_TABLE *t = current_table();
    if (!t)
        return STATUS_INVALID_PARAMETER;
    KIRQL old = KeAcquireSpinLock(&g_lock);
    if (t->Count == t->Capacity) {
        uint32_t cap = t->Capacity ? t->Capacity * 2 : 32;
        if (cap > (1u << 24)) {
            KeReleaseSpinLock(&g_lock, old);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        HANDLE_TABLE_ENTRY *e = ExAllocatePoolWithTag(NonPagedPoolZeroed, cap * sizeof(*e), 'ObHt');
        if (!e) {
            KeReleaseSpinLock(&g_lock, old);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        if (t->Entries) {
            memcpy(e, t->Entries, t->Capacity * sizeof(*e));
            ExFreePoolWithTag(t->Entries, 'ObHt');
        }
        t->Entries = e;
        t->NextFree = t->Capacity;
        t->Capacity = cap;
    }
    uint32_t i = t->NextFree;
    while (t->Entries[i % t->Capacity].Object)
        i++;
    i %= t->Capacity;
    t->Entries[i].Object = object;
    t->Entries[i].Access = 0;
    t->Count++;
    t->NextFree = (i + 1) % t->Capacity;
    KeReleaseSpinLock(&g_lock, old);

    ObReferenceObject(object);
    ObpAdjustHandleCount(object, 1);
    *handle = INDEX_TO_HANDLE(i);
    return STATUS_SUCCESS;
}

NTSTATUS ObReferenceObjectByHandle(HANDLE handle, OBJECT_TYPE *type, void **object)
{
    HANDLE_TABLE *t = current_table();
    if (!t || handle == 0 || (handle & 3))
        return STATUS_INVALID_HANDLE;
    uint32_t i = HANDLE_TO_INDEX(handle);
    KIRQL old = KeAcquireSpinLock(&g_lock);
    void *o = i < t->Capacity ? t->Entries[i].Object : NULL;
    if (!o) {
        KeReleaseSpinLock(&g_lock, old);
        return STATUS_INVALID_HANDLE;
    }
    if (type && ObGetObjectType(o) != type) {
        KeReleaseSpinLock(&g_lock, old);
        return STATUS_OBJECT_TYPE_MISMATCH;
    }
    ObReferenceObject(o);
    KeReleaseSpinLock(&g_lock, old);
    *object = o;
    return STATUS_SUCCESS;
}

NTSTATUS ObCloseHandle(HANDLE handle)
{
    HANDLE_TABLE *t = current_table();
    if (!t || handle == 0 || (handle & 3))
        return STATUS_INVALID_HANDLE;
    uint32_t i = HANDLE_TO_INDEX(handle);
    KIRQL old = KeAcquireSpinLock(&g_lock);
    void *o = i < t->Capacity ? t->Entries[i].Object : NULL;
    if (!o) {
        KeReleaseSpinLock(&g_lock, old);
        return STATUS_INVALID_HANDLE;
    }
    t->Entries[i].Object = NULL;
    t->Count--;
    KeReleaseSpinLock(&g_lock, old);
    ObpAdjustHandleCount(o, -1);
    ObDereferenceObject(o);
    return STATUS_SUCCESS;
}
