/* Kernel initialisation. */
#pragma once
#include "xenon.h"

/* Exit codes for the QEMU debug-exit device (test mode only).
   QEMU's own exit status is (code << 1) | 1. */
#define XENON_EXIT_HALTED    0x10    /* QEMU exit status 33 */
#define XENON_EXIT_BUGCHECK  0x20    /* QEMU exit status 65 */

/* True when booted with "xenon.debugexit" (automated tests). */
bool KiTestMode(void);

/* The kernel command line given by the bootloader ("" if none). */
const char *KiGetCommandLine(void);
