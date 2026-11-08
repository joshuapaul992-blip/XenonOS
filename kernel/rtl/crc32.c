/* CRC-32 (IEEE 802.3, the one zlib, GPT and gzip use). */
#include "xenon.h"

static uint32_t g_table[256];
static bool g_ready;

uint32_t RtlCrc32(uint32_t crc, const void *data, size_t len)
{
    if (!g_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            g_table[i] = c;
        }
        g_ready = true;
    }
    const uint8_t *p = data;
    crc = ~crc;
    while (len--)
        crc = g_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
