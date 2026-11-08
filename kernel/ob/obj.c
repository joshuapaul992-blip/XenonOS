/* SPDX-License-Identifier: MIT */
/*
 * Ob - objects, reference counts and the object namespace.
 *
 * Memory layout of every object:
 *
 *     OBJECT_HEADER   type, reference and handle counts, name, directory link
 *     body            what ObCreateObject returns (16-byte aligned)
 *
 * Directories keep their entries in a list sorted by name. A permanent named
 * object is referenced by its directory; a temporary one loses its name when
 * its last handle is closed. Symbolic links are followed during lookup by
 * splicing the link target in front of the rest of the path.
 */
#include "ob.h"
#include "ke.h"
#include "mm.h"
#include "rtl.h"
#include "console.h"

#define OB_MAX_PATH       512
#define OB_MAX_REPARSE    32
#define OBH_NAMED         0x1
#define OBH_PERMANENT     0x2

typedef struct OBJECT_HEADER {
    OBJECT_TYPE *Type;
    volatile int64_t PointerCount;
    volatile int64_t HandleCount;
    struct OBJECT_DIRECTORY *Directory;
    char *Name;
    LIST_ENTRY DirectoryEntry;
    uint32_t Flags;
    uint32_t Pad;
} __attribute__((aligned(16))) OBJECT_HEADER;

typedef struct OBJECT_DIRECTORY {
    LIST_ENTRY Entries;            /* OBJECT_HEADER.DirectoryEntry, sorted by name */
    uint32_t Count;
} OBJECT_DIRECTORY;

typedef struct OBJECT_SYMBOLIC_LINK {
    char *Target;
} OBJECT_SYMBOLIC_LINK;

static inline OBJECT_HEADER *header(void *body) { return (OBJECT_HEADER *)body - 1; }
static inline void *body(OBJECT_HEADER *h) { return h + 1; }

OBJECT_TYPE *ObTypeObjectType, *ObDirectoryObjectType, *ObSymbolicLinkObjectType;
static OBJECT_DIRECTORY *g_root;
static OBJECT_DIRECTORY *g_types_dir;
static KSPIN_LOCK g_ns_lock;        /* namespace */

/* ---- Helpers ------------------------------------------------------------ */

static char *dup_string(const char *s, size_t n)
{
    char *p = ExAllocatePoolWithTag(NonPagedPool, n + 1, 'ObNm');
    if (p) {
        memcpy(p, s, n);
        p[n] = 0;
    }
    return p;
}

static bool is_sep(char c) { return c == '\\' || c == '/'; }

/* Directory entry with this name (case-insensitive), or NULL. Caller holds the lock. */
static OBJECT_HEADER *dir_find(OBJECT_DIRECTORY *d, const char *name, size_t len)
{
    for (LIST_ENTRY *e = d->Entries.Flink; e != &d->Entries; e = e->Flink) {
        OBJECT_HEADER *h = CONTAINING_RECORD(e, OBJECT_HEADER, DirectoryEntry);
        if (strlen(h->Name) == len && !strncasecmp(h->Name, name, len))
            return h;
    }
    return NULL;
}

static void dir_insert(OBJECT_DIRECTORY *d, OBJECT_HEADER *h)
{
    LIST_ENTRY *e = d->Entries.Flink;
    while (e != &d->Entries && strcasecmp(CONTAINING_RECORD(e, OBJECT_HEADER, DirectoryEntry)->Name, h->Name) < 0)
        e = e->Flink;
    InsertBeforeList(e, &h->DirectoryEntry);
    d->Count++;
}

/* ---- Types and objects -------------------------------------------------- */

NTSTATUS ObCreateObjectType(const char *name, void (*del)(void *), uint32_t tag, OBJECT_TYPE **out)
{
    OBJECT_TYPE *t;
    NTSTATUS st = ObCreateObject(ObTypeObjectType, sizeof(OBJECT_TYPE), (void **)&t);
    if (!NT_SUCCESS(st))
        return st;
    strlcpy(t->Name, name, sizeof(t->Name));
    t->DeleteProcedure = del;
    t->PoolTag = tag;
    if (g_types_dir) {
        char path[64];
        snprintf(path, sizeof(path), "\\ObjectTypes\\%s", name);
        ObInsertObjectName(t, path, OBJ_PERMANENT);
        ObDereferenceObject(t);          /* the directory keeps it alive */
    }
    *out = t;
    return STATUS_SUCCESS;
}

NTSTATUS ObCreateObject(OBJECT_TYPE *type, size_t body_size, void **object)
{
    OBJECT_HEADER *h = ExAllocatePoolWithTag(NonPagedPoolZeroed, sizeof(OBJECT_HEADER) + body_size,
                                             type && type->PoolTag ? type->PoolTag : 'ObjX');
    if (!h)
        return STATUS_INSUFFICIENT_RESOURCES;
    h->Type = type;
    h->PointerCount = 1;
    InitializeListHead(&h->DirectoryEntry);
    if (type) {
        uint32_t n = __atomic_add_fetch(&type->TotalObjects, 1, __ATOMIC_RELAXED);
        if (n > type->HighWaterObjects)
            type->HighWaterObjects = n;
    }
    *object = body(h);
    return STATUS_SUCCESS;
}

static void delete_object(OBJECT_HEADER *h)
{
    void *b = body(h);
    if (h->Type) {
        if (h->Type->DeleteProcedure)
            h->Type->DeleteProcedure(b);
        __atomic_sub_fetch(&h->Type->TotalObjects, 1, __ATOMIC_RELAXED);
    }
    if (h->Name)
        ExFreePoolWithTag(h->Name, 'ObNm');
    ExFreePoolWithTag(h, 0);
}

void ObReferenceObject(void *object)
{
    OBJECT_HEADER *h = header(object);
    if (__atomic_add_fetch(&h->PointerCount, 1, __ATOMIC_ACQ_REL) <= 1)
        KeBugCheckEx(REFERENCE_BY_POINTER, (uint64_t)h->Type, (uint64_t)object, (uint64_t)h->PointerCount, 1);
}

void ObDereferenceObject(void *object)
{
    OBJECT_HEADER *h = header(object);
    int64_t n = __atomic_sub_fetch(&h->PointerCount, 1, __ATOMIC_ACQ_REL);
    if (n < 0)
        KeBugCheckEx(REFERENCE_BY_POINTER, (uint64_t)h->Type, (uint64_t)object, (uint64_t)n, 2);
    if (n == 0) {
        if (h->HandleCount)
            KeBugCheckEx(REFERENCE_BY_POINTER, (uint64_t)h->Type, (uint64_t)object, (uint64_t)h->HandleCount, 3);
        delete_object(h);
    }
}

OBJECT_TYPE *ObGetObjectType(void *object) { return header(object)->Type; }
const char *ObGetObjectName(void *object) { return header(object)->Name ? header(object)->Name : ""; }
int64_t ObGetPointerCount(void *object) { return header(object)->PointerCount; }
int64_t ObGetHandleCount(void *object) { return header(object)->HandleCount; }

/* ---- Names -------------------------------------------------------------- */

/* Split "\A\B\Name" into the directory "\A\B" (looked up) and "Name". */
static NTSTATUS lookup(const char *path, uint32_t flags, OBJECT_HEADER **out, char *leaf,
                       size_t *leaf_len, bool want_parent);

NTSTATUS ObInsertObjectName(void *object, const char *path, uint32_t flags)
{
    OBJECT_HEADER *h = header(object);
    if (h->Flags & OBH_NAMED)
        return STATUS_OBJECT_NAME_COLLISION;
    OBJECT_HEADER *dirh;
    char leaf[256];
    size_t leaf_len;
    NTSTATUS st = lookup(path, 0, &dirh, leaf, &leaf_len, true);
    if (!NT_SUCCESS(st))
        return st;
    if (dirh->Type != ObDirectoryObjectType) {
        ObDereferenceObject(body(dirh));
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }
    if (leaf_len == 0 || leaf_len > 255) {
        ObDereferenceObject(body(dirh));
        return STATUS_OBJECT_NAME_INVALID;
    }
    char *name = dup_string(leaf, leaf_len);
    if (!name) {
        ObDereferenceObject(body(dirh));
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    OBJECT_DIRECTORY *dir = body(dirh);
    KIRQL old = KeAcquireSpinLock(&g_ns_lock);
    if (dir_find(dir, leaf, leaf_len)) {
        KeReleaseSpinLock(&g_ns_lock, old);
        ExFreePoolWithTag(name, 'ObNm');
        ObDereferenceObject(body(dirh));
        return STATUS_OBJECT_NAME_COLLISION;
    }
    h->Name = name;
    h->Directory = dir;
    h->Flags |= OBH_NAMED;
    dir_insert(dir, h);
    KeReleaseSpinLock(&g_ns_lock, old);
    /* The object keeps a reference on its directory. */
    if (flags & OBJ_PERMANENT) {
        h->Flags |= OBH_PERMANENT;
        ObReferenceObject(object);       /* ...and the directory on a permanent object */
    }
    return STATUS_SUCCESS;
}

static void remove_name(OBJECT_HEADER *h)
{
    OBJECT_DIRECTORY *dir = NULL;
    KIRQL old = KeAcquireSpinLock(&g_ns_lock);
    if (h->Flags & OBH_NAMED) {
        RemoveEntryList(&h->DirectoryEntry);
        h->Directory->Count--;
        dir = h->Directory;
        h->Directory = NULL;
        h->Flags &= ~OBH_NAMED;
    }
    KeReleaseSpinLock(&g_ns_lock, old);
    if (dir)
        ObDereferenceObject(dir);
}

void ObMakeTemporaryObject(void *object)
{
    OBJECT_HEADER *h = header(object);
    bool was_permanent = h->Flags & OBH_PERMANENT;
    h->Flags &= ~OBH_PERMANENT;
    if (h->HandleCount == 0)
        remove_name(h);
    if (was_permanent)
        ObDereferenceObject(object);
}

/* Called by the handle code when the last handle to an object goes away. */
void ObpLastHandleClosed(void *object)
{
    OBJECT_HEADER *h = header(object);
    if ((h->Flags & OBH_NAMED) && !(h->Flags & OBH_PERMANENT))
        remove_name(h);
}

void ObpAdjustHandleCount(void *object, int delta)
{
    OBJECT_HEADER *h = header(object);
    int64_t n = __atomic_add_fetch(&h->HandleCount, delta, __ATOMIC_ACQ_REL);
    if (h->Type) {
        if (delta > 0) __atomic_add_fetch(&h->Type->TotalHandles, 1, __ATOMIC_RELAXED);
        else __atomic_sub_fetch(&h->Type->TotalHandles, 1, __ATOMIC_RELAXED);
    }
    if (n == 0 && delta < 0)
        ObpLastHandleClosed(object);
}

/*
 * Walk a path. With want_parent, stops at the directory that should contain
 * the last component and returns that component in *leaf. Returns a
 * referenced object header.
 */
static NTSTATUS lookup(const char *path, uint32_t flags, OBJECT_HEADER **out, char *leaf,
                       size_t *leaf_len, bool want_parent)
{
    if (want_parent) {
        leaf[0] = 0;
        *leaf_len = 0;
    }
    static char buf[2][OB_MAX_PATH];      /* protected by g_ns_lock while in use */
    char *cur = NULL;
    int which = 0;
    NTSTATUS st = STATUS_SUCCESS;

    if (!path || !is_sep(path[0]))
        return STATUS_OBJECT_PATH_NOT_FOUND;
    if (strlen(path) >= OB_MAX_PATH)
        return STATUS_OBJECT_NAME_INVALID;

    KIRQL old = KeAcquireSpinLock(&g_ns_lock);
    strlcpy(buf[0], path, OB_MAX_PATH);
    cur = buf[0];
    OBJECT_HEADER *h = header(g_root);
    const char *p = cur;
    int reparses = 0;

    for (;;) {
        while (is_sep(*p))
            p++;
        if (!*p)
            break;                                     /* path ended on a directory */
        const char *start = p;
        while (*p && !is_sep(*p))
            p++;
        size_t len = (size_t)(p - start);
        const char *rest = p;
        while (is_sep(*rest))
            rest++;
        bool last = *rest == 0;

        if (last && want_parent) {
            /* Return the directory and a copy of the final name (256 bytes). */
            if (len > 255) {
                st = STATUS_OBJECT_NAME_INVALID;
                goto out;
            }
            memcpy(leaf, start, len);
            leaf[len] = 0;
            *leaf_len = len;
            break;
        }
        if (h->Type != ObDirectoryObjectType) {
            st = STATUS_OBJECT_PATH_NOT_FOUND;
            goto out;
        }
        OBJECT_HEADER *child = dir_find(body(h), start, len);
        if (!child) {
            st = last ? STATUS_OBJECT_NAME_NOT_FOUND : STATUS_OBJECT_PATH_NOT_FOUND;
            goto out;
        }
        if (child->Type == ObSymbolicLinkObjectType && !(last && (flags & OBJ_OPENLINK))) {
            if (++reparses > OB_MAX_REPARSE) {
                st = STATUS_REPARSE_LOOP;
                goto out;
            }
            const char *target = ((OBJECT_SYMBOLIC_LINK *)body(child))->Target;
            char *next = buf[which ^ 1];
            int n = snprintf(next, OB_MAX_PATH, "%s%s%s", target, *rest ? "\\" : "", rest);
            if (n >= OB_MAX_PATH) {
                st = STATUS_OBJECT_NAME_INVALID;
                goto out;
            }
            which ^= 1;
            cur = next;
            p = cur;
            h = header(g_root);
            continue;
        }
        h = child;
    }
    ObReferenceObject(body(h));
    *out = h;
out:
    KeReleaseSpinLock(&g_ns_lock, old);
    return st;
}

NTSTATUS ObReferenceObjectByName(const char *path, uint32_t flags, OBJECT_TYPE *type, void **object)
{
    OBJECT_HEADER *h;
    NTSTATUS st = lookup(path, flags, &h, NULL, NULL, false);
    if (!NT_SUCCESS(st))
        return st;
    if (type && h->Type != type) {
        ObDereferenceObject(body(h));
        return STATUS_OBJECT_TYPE_MISMATCH;
    }
    *object = body(h);
    return STATUS_SUCCESS;
}

size_t ObQueryNameString(void *object, char *buf, size_t size)
{
    /* Build the path from the leaf up to the root. */
    const char *parts[32];
    int n = 0;
    KIRQL old = KeAcquireSpinLock(&g_ns_lock);
    OBJECT_HEADER *h = header(object);
    while (h && (h->Flags & OBH_NAMED) && n < 32) {
        parts[n++] = h->Name;
        h = h->Directory ? header(h->Directory) : NULL;
    }
    size_t pos = 0;
    if (n == 0)
        pos = snprintf(buf, size, "%s", object == g_root ? "\\" : "");
    for (int i = n - 1; i >= 0 && pos < size; i--)
        pos += snprintf(buf + pos, size - pos, "\\%s", parts[i]);
    KeReleaseSpinLock(&g_ns_lock, old);
    return pos;
}

/* ---- Directories and symbolic links ------------------------------------- */

static void delete_directory(void *object)
{
    OBJECT_DIRECTORY *d = object;
    if (d->Count)
        KeBugCheckEx(REFERENCE_BY_POINTER, (uint64_t)object, d->Count, 0, 4);
}

static void delete_symlink(void *object)
{
    OBJECT_SYMBOLIC_LINK *l = object;
    if (l->Target)
        ExFreePoolWithTag(l->Target, 'ObNm');
}

NTSTATUS ObCreateDirectoryObject(const char *path, void **out)
{
    OBJECT_DIRECTORY *d;
    NTSTATUS st = ObCreateObject(ObDirectoryObjectType, sizeof(OBJECT_DIRECTORY), (void **)&d);
    if (!NT_SUCCESS(st))
        return st;
    InitializeListHead(&d->Entries);
    if (path) {
        st = ObInsertObjectName(d, path, OBJ_PERMANENT);
        if (!NT_SUCCESS(st)) {
            ObDereferenceObject(d);
            return st;
        }
    }
    if (out)
        *out = d;
    else
        ObDereferenceObject(d);
    return STATUS_SUCCESS;
}

NTSTATUS ObCreateSymbolicLink(const char *path, const char *target)
{
    OBJECT_SYMBOLIC_LINK *l;
    NTSTATUS st = ObCreateObject(ObSymbolicLinkObjectType, sizeof(OBJECT_SYMBOLIC_LINK), (void **)&l);
    if (!NT_SUCCESS(st))
        return st;
    l->Target = dup_string(target, strlen(target));
    if (!l->Target) {
        ObDereferenceObject(l);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    st = ObInsertObjectName(l, path, OBJ_PERMANENT);
    ObDereferenceObject(l);
    return st;
}

const char *ObGetSymbolicLinkTarget(void *link)
{
    if (header(link)->Type != ObSymbolicLinkObjectType)
        return NULL;
    return ((OBJECT_SYMBOLIC_LINK *)link)->Target;
}

NTSTATUS ObEnumerateDirectory(void *dir, bool (*cb)(void *object, void *ctx), void *ctx)
{
    if (header(dir)->Type != ObDirectoryObjectType)
        return STATUS_OBJECT_TYPE_MISMATCH;
    OBJECT_DIRECTORY *d = dir;
    /* Take a referenced snapshot so the callback can do anything. */
    KIRQL old = KeAcquireSpinLock(&g_ns_lock);
    uint32_t n = d->Count;
    void **list = n ? ExAllocatePoolWithTag(NonPagedPool, n * sizeof(void *), 'ObEn') : NULL;
    uint32_t i = 0;
    if (list) {
        for (LIST_ENTRY *e = d->Entries.Flink; e != &d->Entries && i < n; e = e->Flink) {
            OBJECT_HEADER *h = CONTAINING_RECORD(e, OBJECT_HEADER, DirectoryEntry);
            __atomic_add_fetch(&h->PointerCount, 1, __ATOMIC_ACQ_REL);
            list[i++] = body(h);
        }
    }
    KeReleaseSpinLock(&g_ns_lock, old);
    if (n && !list)
        return STATUS_INSUFFICIENT_RESOURCES;
    bool go = true;
    for (uint32_t j = 0; j < i; j++) {
        if (go)
            go = cb(list[j], ctx);
        ObDereferenceObject(list[j]);
    }
    if (list)
        ExFreePoolWithTag(list, 'ObEn');
    return STATUS_SUCCESS;
}

/* ---- Initialisation ----------------------------------------------------- */

OBJECT_TYPE *ExEventObjectType, *ExMutantObjectType, *ExSemaphoreObjectType, *ExTimerObjectType;

static void delete_timer_object(void *object)
{
    KeCancelTimer(object);
}

void ObInitSystem(void)
{
    /* Bootstrap: the "Type" type describes itself. */
    void *tt;
    if (!NT_SUCCESS(ObCreateObject(NULL, sizeof(OBJECT_TYPE), &tt)))
        KeBugCheckEx(OBJECT_INITIALIZATION_FAILED, 1, 0, 0, 0);
    ObTypeObjectType = tt;
    header(tt)->Type = ObTypeObjectType;
    strlcpy(ObTypeObjectType->Name, "Type", sizeof(ObTypeObjectType->Name));
    ObTypeObjectType->PoolTag = 'ObjT';
    ObTypeObjectType->TotalObjects = 1;

    ObCreateObjectType("Directory", delete_directory, 'ObDi', &ObDirectoryObjectType);
    ObCreateObjectType("SymbolicLink", delete_symlink, 'ObSy', &ObSymbolicLinkObjectType);

    /* Root directory, unnamed. */
    if (!NT_SUCCESS(ObCreateDirectoryObject(NULL, (void **)&g_root)))
        KeBugCheckEx(OBJECT_INITIALIZATION_FAILED, 2, 0, 0, 0);

    ObCreateDirectoryObject("\\ObjectTypes", (void **)&g_types_dir);
    /* Give the first three types their names now that the directory exists. */
    ObInsertObjectName(ObTypeObjectType, "\\ObjectTypes\\Type", OBJ_PERMANENT);
    ObInsertObjectName(ObDirectoryObjectType, "\\ObjectTypes\\Directory", OBJ_PERMANENT);
    ObInsertObjectName(ObSymbolicLinkObjectType, "\\ObjectTypes\\SymbolicLink", OBJ_PERMANENT);
    ObDereferenceObject(ObDirectoryObjectType);
    ObDereferenceObject(ObSymbolicLinkObjectType);

    ObCreateObjectType("Event", NULL, 'Even', &ExEventObjectType);
    ObCreateObjectType("Mutant", NULL, 'Muta', &ExMutantObjectType);
    ObCreateObjectType("Semaphore", NULL, 'Sema', &ExSemaphoreObjectType);
    ObCreateObjectType("Timer", delete_timer_object, 'Time', &ExTimerObjectType);

    ObCreateDirectoryObject("\\Device", NULL);
    ObCreateDirectoryObject("\\Driver", NULL);
    ObCreateDirectoryObject("\\GLOBAL??", NULL);
    ObCreateSymbolicLink("\\DosDevices", "\\GLOBAL??");
    ObCreateSymbolicLink("\\??", "\\GLOBAL??");
    ObCreateDirectoryObject("\\KernelObjects", NULL);
    ObCreateDirectoryObject("\\BaseNamedObjects", NULL);
}
