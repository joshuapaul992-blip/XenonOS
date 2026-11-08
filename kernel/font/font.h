/* Bitmap console fonts built into the kernel. */
#pragma once

struct KFONT {
    unsigned Width;          /* pixels */
    unsigned Height;         /* pixels */
    unsigned BytesPerRow;
    const unsigned char *Glyphs;   /* 256 glyphs in CP437 order */
};

extern const struct KFONT KFontTerminus16;    /* 8x16  */
extern const struct KFONT KFontTerminus32;    /* 16x32 (high-resolution screens) */
extern const unsigned short KFontCp437Unicode[256];
