/*
 * Ob - object manager.
 *
 * Every kernel object (directory, symbolic link, event, mutex, semaphore,
 * timer, process, thread, and later devices, drivers and files) is created
 * by the object manager. It is reference counted, may have a name in the
 * object namespace, and can be opened through handles.
 *
 * Namespace layout (case-insensitive, '\' or '/' as separator):
 *   \ObjectTypes        one entry per object type
 *   \Device             device objects (disks, volumes, ...)
 *   \Driver             driver objects
 *   \GLOBAL??           drive letters and other DOS device names
 *   \DosDevices         symbolic link to \GLOBAL??
 *   \KernelObjects      named kernel events
 *   \BaseNamedObjects   named objects created by programs
 */
#pragma once
#include "xenon.h"
#include "list.h"
#include "status.h"

typedef uint32_t HANDLE;           /* multiples of 4; 0 is never a valid handle */
#define INVALID_HANDLE 0

typedef struct OBJECT_TYPE {
    char Name[24];
    void (*DeleteProcedure)(void *object);
    uint32_t PoolTag;
    volatile uint32_t TotalObjects;
    volatile uint32_t TotalHandles;
    uint32_t HighWaterObjects;
} OBJECT_TYPE;

#define OBJ_PERMANENT        0x10    /* keep the name and object even with no handles */
#define OBJ_OPENIF           0x80    /* creating an existing name opens it instead */
#define OBJ_OPENLINK         0x100   /* do not follow a symbolic link at the end of the path */

extern OBJECT_TYPE *ObTypeObjectType, *ObDirectoryObjectType, *ObSymbolicLinkObjectType;
extern OBJECT_TYPE *ExEventObjectType, *ExMutantObjectType, *ExSemaphoreObjectType, *ExTimerObjectType;

void ObInitSystem(void);

NTSTATUS ObCreateObjectType(const char *name, void (*del)(void *), uint32_t tag, OBJECT_TYPE **type);
/* Creates an unnamed object with one reference. The body is zeroed. */
NTSTATUS ObCreateObject(OBJECT_TYPE *type, size_t body_size, void **object);
/* Give an object a name ("\Dir\Name"). Permanent objects keep it until
   ObMakeTemporaryObject; others lose it when their last handle closes. */
NTSTATUS ObInsertObjectName(void *object, const char *path, uint32_t flags);
void ObMakeTemporaryObject(void *object);

void ObReferenceObject(void *object);
void ObDereferenceObject(void *object);
NTSTATUS ObReferenceObjectByName(const char *path, uint32_t flags, OBJECT_TYPE *type, void **object);

/* Handles in the current process's handle table. */
NTSTATUS ObCreateHandle(void *object, HANDLE *handle);
NTSTATUS ObReferenceObjectByHandle(HANDLE handle, OBJECT_TYPE *type, void **object);
NTSTATUS ObCloseHandle(HANDLE handle);

NTSTATUS ObCreateDirectoryObject(const char *path, void **dir);
NTSTATUS ObCreateSymbolicLink(const char *path, const char *target);

OBJECT_TYPE *ObGetObjectType(void *object);
const char *ObGetObjectName(void *object);          /* "" if unnamed */
int64_t ObGetPointerCount(void *object);
int64_t ObGetHandleCount(void *object);
size_t ObQueryNameString(void *object, char *buf, size_t size);   /* full path */
const char *ObGetSymbolicLinkTarget(void *link);

/* Calls cb for each entry of a directory, in name order. cb returns false to stop. */
NTSTATUS ObEnumerateDirectory(void *dir, bool (*cb)(void *object, void *ctx), void *ctx);

/* Handle tables (one per process). */
typedef struct HANDLE_TABLE_ENTRY { void *Object; uint32_t Access; } HANDLE_TABLE_ENTRY;
typedef struct HANDLE_TABLE {
    HANDLE_TABLE_ENTRY *Entries;
    uint32_t Capacity;
    uint32_t Count;
    uint32_t NextFree;
} HANDLE_TABLE;
void ObInitializeHandleTable(HANDLE_TABLE *t);
void ObKillHandleTable(HANDLE_TABLE *t);
