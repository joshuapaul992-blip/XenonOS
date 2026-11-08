/* SPDX-License-Identifier: MIT */
/*
 * Loader block: everything the kernel needs from the bootloader, copied into
 * kernel memory at the very start of boot (like NT's LOADER_PARAMETER_BLOCK).
 *
 * After the copy, the memory Limine used ("bootloader reclaimable") is given
 * back to the page allocator, so nothing may point into Limine's structures.
 */
#pragma once
#include "xenon.h"

#define LOADER_MAX_MEMMAP 256

/* Memory map entry types (same values as Limine). */
#define MEM_USABLE                 0
#define MEM_RESERVED               1
#define MEM_ACPI_RECLAIMABLE       2
#define MEM_ACPI_NVS               3
#define MEM_BAD                    4
#define MEM_BOOTLOADER_RECLAIMABLE 5
#define MEM_KERNEL_AND_MODULES     6
#define MEM_FRAMEBUFFER            7

typedef struct {
    uint64_t Base;
    uint64_t Length;
    uint32_t Type;
} LOADER_MEMORY_DESCRIPTOR;

typedef struct {
    uint64_t PhysicalAddress;
    uint64_t Width, Height, Pitch;
    uint16_t Bpp;
    uint8_t  MemoryModel;
    uint8_t  RedSize, RedShift, GreenSize, GreenShift, BlueSize, BlueShift;
} LOADER_FRAMEBUFFER;

typedef struct {
    LOADER_MEMORY_DESCRIPTOR MemoryMap[LOADER_MAX_MEMMAP];
    uint32_t MemoryMapCount;
    uint64_t HhdmOffset;            /* virtual = physical + HhdmOffset */
    uint64_t KernelPhysicalBase;
    uint64_t KernelVirtualBase;
    uint64_t RsdpPhysical;          /* 0 if none */
    bool     HaveFramebuffer;
    LOADER_FRAMEBUFFER Framebuffer;
    char     CommandLine[256];
    char     BootloaderName[32];
    char     BootloaderVersion[32];
    const char *FirmwareName;
    bool     Uefi;
} LOADER_BLOCK;

extern LOADER_BLOCK KeLoaderBlock;
