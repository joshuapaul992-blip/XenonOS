/*
 * HAL - hardware abstraction layer.
 */
#pragma once
#include "xenon.h"

/* Serial port (COM1 by default). Used for the debug log and automated tests. */
bool HalSerialInit(void);
bool HalSerialPresent(void);
void HalSerialWrite(const char *s, size_t len);
void HalSerialPutChar(char c);

/* Reboot the machine (keyboard controller, then triple fault). */
NORETURN void HalReboot(void);

/* QEMU/Bochs debug exit: lets automated tests stop the VM. No-op on real hardware. */
void HalDebugExit(uint8_t code);

/* Early time source (TSC calibrated with the PIT). */
uint64_t HalCalibrateTsc(void);
uint64_t HalGetTscHz(void);
uint64_t HalGetTimeNs(void);      /* nanoseconds since calibration */
void     HalStallMs(unsigned ms);
