/* SPDX-License-Identifier: MIT */
/* Io - internal definitions shared by the disk class, cache and partition manager. */
#pragma once
#include "io.h"

#define CC_LINE_SIZE 4096u

typedef struct DISK_EXTENSION {
    DISK_PARAMS Params;
    char Model[41];
    char Serial[21];
    char Bus[40];
    char Name[48];                  /* \Device\Harddisk0\DR0 or \Device\Floppy0 */
    char Style[8];                  /* partition table type, set by the partition manager */
    uint32_t Number;                /* N in HarddiskN / FloppyN */
    uint32_t BytesPerSector;
    uint64_t SectorCount;
    bool MediaPresent;
    bool WriteProtected;
    struct CACHE *Cache;
    KMUTEX PortLock;                /* one request at a time into the port driver */
} DISK_EXTENSION;

/* Uncached access to the port driver (used by the cache). Splits requests
   into pieces the port accepts. Offsets and lengths are in bytes and must be
   sector aligned. */
NTSTATUS DiskRawRead(DEVICE_OBJECT *disk, uint64_t offset, uint32_t length, void *buffer);
NTSTATUS DiskRawWrite(DEVICE_OBJECT *disk, uint64_t offset, uint32_t length, const void *buffer);
void CcInitialize(void);
