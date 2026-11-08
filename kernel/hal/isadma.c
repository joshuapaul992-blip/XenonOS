/*
 * HAL - 8237 ISA DMA controller (channels 0-3, 8-bit). The floppy controller
 * uses channel 2.
 */
#include "hal.h"
#include "ke.h"

static const uint8_t g_addr_port[4]  = { 0x00, 0x02, 0x04, 0x06 };
static const uint8_t g_count_port[4] = { 0x01, 0x03, 0x05, 0x07 };
static const uint8_t g_page_port[4]  = { 0x87, 0x83, 0x81, 0x82 };
static KSPIN_LOCK g_lock;

bool HalIsaDmaStart(unsigned ch, uint64_t phys, uint32_t length, bool to_memory)
{
    if (ch > 3 || length == 0 || length > 65536 || phys + length > (16u << 20) ||
        (phys >> 16) != ((phys + length - 1) >> 16))
        return false;
    uint32_t count = length - 1;
    /* Mode: single transfer, address increment, no auto-init;
       "write" (01) = device to memory, "read" (10) = memory to device. */
    uint8_t mode = (uint8_t)(0x40 | (to_memory ? 0x04 : 0x08) | ch);
    KIRQL old = KeAcquireSpinLockRaiseToHigh(&g_lock);
    outb(0x0A, (uint8_t)(0x04 | ch));          /* mask channel */
    outb(0x0C, 0xFF);                          /* reset flip-flop */
    outb(0x0B, mode);
    outb(g_addr_port[ch], phys & 0xFF);
    outb(g_addr_port[ch], (phys >> 8) & 0xFF);
    outb(g_page_port[ch], (phys >> 16) & 0xFF);
    outb(0x0C, 0xFF);
    outb(g_count_port[ch], count & 0xFF);
    outb(g_count_port[ch], (count >> 8) & 0xFF);
    outb(0x0A, (uint8_t)ch);                   /* unmask */
    KeReleaseSpinLock(&g_lock, old);
    return true;
}
