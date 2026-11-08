/*
 * HAL - 16550 UART driver (COM1), used for the kernel debug log.
 */
#include "hal.h"

#define COM1        0x3F8
#define REG_DATA    0
#define REG_IER     1
#define REG_FCR     2
#define REG_LCR     3
#define REG_MCR     4
#define REG_LSR     5
#define LSR_THRE    0x20

static bool g_present;

bool HalSerialInit(void)
{
    outb(COM1 + REG_IER, 0x00);     /* no interrupts */
    outb(COM1 + REG_LCR, 0x80);     /* DLAB on */
    outb(COM1 + REG_DATA, 0x01);    /* divisor 1 = 115200 baud */
    outb(COM1 + REG_IER, 0x00);
    outb(COM1 + REG_LCR, 0x03);     /* 8N1, DLAB off */
    outb(COM1 + REG_FCR, 0xC7);     /* FIFO on, clear, 14-byte threshold */
    outb(COM1 + REG_MCR, 0x1E);     /* loopback mode for the self-test */
    outb(COM1 + REG_DATA, 0xAE);
    if (inb(COM1 + REG_DATA) != 0xAE) {
        g_present = false;
        return false;
    }
    outb(COM1 + REG_MCR, 0x0F);     /* normal mode: DTR, RTS, OUT1, OUT2 */
    g_present = true;
    return true;
}

bool HalSerialPresent(void)
{
    return g_present;
}

void HalSerialPutChar(char c)
{
    if (!g_present)
        return;
    for (int spin = 0; spin < 100000 && !(inb(COM1 + REG_LSR) & LSR_THRE); spin++)
        cpu_pause();
    outb(COM1 + REG_DATA, (uint8_t)c);
}

void HalSerialWrite(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\n')
            HalSerialPutChar('\r');
        HalSerialPutChar(s[i]);
    }
}
