/* SPDX-License-Identifier: MIT */
/*
 * Kernel console: framebuffer text console + serial mirror.
 */
#pragma once
#include "xenon.h"

struct limine_framebuffer;

/* Colours are 0xRRGGBB. */
#define CON_BLACK       0x000000
#define CON_BLUE        0x0000AA
#define CON_GREEN       0x00AA00
#define CON_CYAN        0x00AAAA
#define CON_RED         0xAA0000
#define CON_MAGENTA     0xAA00AA
#define CON_BROWN       0xAA5500
#define CON_LIGHTGRAY   0xAAAAAA
#define CON_DARKGRAY    0x555555
#define CON_LIGHTBLUE   0x5555FF
#define CON_LIGHTGREEN  0x55FF55
#define CON_LIGHTCYAN   0x55FFFF
#define CON_LIGHTRED    0xFF5555
#define CON_PINK        0xFF55FF
#define CON_YELLOW      0xFFFF55
#define CON_WHITE       0xFFFFFF

/* Framebuffer console */
bool FbConInit(struct limine_framebuffer *fb);
bool FbConPresent(void);
void FbConWrite(const char *s, size_t len);
void FbConSetColors(uint32_t fg, uint32_t bg);
void FbConClear(void);
void FbConGetSize(unsigned *cols, unsigned *rows);
void FbConGetResolution(unsigned *width, unsigned *height, unsigned *font_w, unsigned *font_h);

/* Writes to every console (framebuffer and serial). Accepts ANSI escapes. */
void ConWrite(const char *s, size_t len);
void ConPrint(const char *fmt, ...) PRINTF_LIKE(1, 2);
void ConVPrint(const char *fmt, va_list ap);

/* Kernel log: "[    0.000] message" to all consoles. */
void DbgPrint(const char *fmt, ...) PRINTF_LIKE(1, 2);
