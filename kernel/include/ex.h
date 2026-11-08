/* SPDX-License-Identifier: MIT */
/*
 * Ex - executive support: the kernel log buffer.
 */
#pragma once
#include "xenon.h"

#define EX_LOG_SIZE (128 * 1024)

/* Append text to the kernel log ring buffer (safe at any IRQL). */
void ExLogWrite(const char *s, size_t len);
/* Copy the log (oldest first) into buf; returns the number of bytes copied. */
size_t ExLogRead(char *buf, size_t size);
/* Total bytes ever written (so readers can tell what is new). */
uint64_t ExLogTotal(void);
