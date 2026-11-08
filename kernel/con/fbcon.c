/* SPDX-License-Identifier: MIT */
/*
 * Framebuffer text console.
 *
 * Keeps a grid of character cells and draws them with a built-in bitmap
 * font. Understands UTF-8 (mapped to the CP437 glyph set) and the ANSI/VT100
 * escape sequences that the shell and vi need:
 *
 *   ESC[n A/B/C/D   cursor up/down/right/left      ESC[r;c H / f  move cursor
 *   ESC[n J         erase display (0, 1, 2)        ESC[n K        erase line
 *   ESC[...m        colours/attributes (16 colours, 256-colour and 24-bit)
 *   ESC[s / ESC[u   save / restore cursor          ESC[?25 l / h  hide / show cursor
 *
 * Scrolling redraws from the cell grid instead of reading video memory back,
 * because reads from write-combined video memory are very slow on real PCs.
 */
#include "console.h"
#include "rtl.h"
#include "limine.h"
#include "../font/font.h"

#define MAX_COLS 320
#define MAX_ROWS 160

typedef struct {
    uint32_t Fg;
    uint32_t Bg;
    uint8_t  Ch;    /* CP437 glyph index */
} CELL;

static struct {
    bool Present;
    uint8_t *Base;
    uint32_t Width, Height, Pitch, Bpp;
    uint8_t RShift, GShift, BShift, RSize, GSize, BSize;
    const struct KFONT *Font;
    unsigned Cols, Rows;
    unsigned OffX, OffY;            /* centre the text area */
    unsigned Cx, Cy;                /* cursor */
    unsigned SavedCx, SavedCy;
    uint32_t Fg, Bg, DefFg, DefBg;
    bool Bold, Reverse, CursorVisible, CursorDrawn;
    /* escape-sequence parser */
    int EscState;                   /* 0 = normal, 1 = got ESC, 2 = in CSI */
    int Params[16];
    int NParams;
    bool Private;
    /* UTF-8 decoder */
    uint32_t Utf8Cp;
    int Utf8Left;
} g;

static CELL g_cells[MAX_ROWS][MAX_COLS];

static const uint32_t g_palette[16] = {
    CON_BLACK, CON_RED, CON_GREEN, CON_BROWN, CON_BLUE, CON_MAGENTA, CON_CYAN, CON_LIGHTGRAY,
    CON_DARKGRAY, CON_LIGHTRED, CON_LIGHTGREEN, CON_YELLOW, CON_LIGHTBLUE, CON_PINK, CON_LIGHTCYAN, CON_WHITE,
};

/* ---- Pixel drawing ------------------------------------------------------ */

static inline uint32_t pack(uint32_t rgb)
{
    uint32_t r = (rgb >> 16) & 0xFF, gr = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    r >>= 8 - g.RSize;
    gr >>= 8 - g.GSize;
    b >>= 8 - g.BSize;
    return (r << g.RShift) | (gr << g.GShift) | (b << g.BShift);
}

static void draw_cell(unsigned col, unsigned row, bool cursor)
{
    const CELL *c = &g_cells[row][col];
    const struct KFONT *f = g.Font;
    uint32_t fg = pack(c->Fg), bg = pack(c->Bg);
    if (cursor) {
        uint32_t t = fg; fg = bg; bg = t;
    }
    const uint8_t *glyph = f->Glyphs + (size_t)c->Ch * f->BytesPerRow * f->Height;
    unsigned px = g.OffX + col * f->Width, py = g.OffY + row * f->Height;
    unsigned bytespp = g.Bpp / 8;

    for (unsigned y = 0; y < f->Height; y++) {
        uint8_t *line = g.Base + (size_t)(py + y) * g.Pitch + (size_t)px * bytespp;
        const uint8_t *bits = glyph + y * f->BytesPerRow;
        for (unsigned x = 0; x < f->Width; x++) {
            bool on = bits[x >> 3] & (0x80 >> (x & 7));
            uint32_t v = on ? fg : bg;
            switch (bytespp) {
            case 4: ((volatile uint32_t *)line)[x] = v; break;
            case 3:
                line[x * 3] = v & 0xFF;
                line[x * 3 + 1] = (v >> 8) & 0xFF;
                line[x * 3 + 2] = (v >> 16) & 0xFF;
                break;
            case 2: ((volatile uint16_t *)line)[x] = (uint16_t)v; break;
            }
        }
    }
}

static void fill_rect(unsigned x0, unsigned y0, unsigned w, unsigned h, uint32_t rgb)
{
    uint32_t v = pack(rgb);
    unsigned bytespp = g.Bpp / 8;
    for (unsigned y = y0; y < y0 + h && y < g.Height; y++) {
        uint8_t *line = g.Base + (size_t)y * g.Pitch;
        for (unsigned x = x0; x < x0 + w && x < g.Width; x++) {
            if (bytespp == 4) ((volatile uint32_t *)line)[x] = v;
            else if (bytespp == 2) ((volatile uint16_t *)line)[x] = (uint16_t)v;
            else { line[x * 3] = v; line[x * 3 + 1] = v >> 8; line[x * 3 + 2] = v >> 16; }
        }
    }
}

static void hide_cursor(void)
{
    if (g.CursorDrawn) {
        draw_cell(g.Cx < g.Cols ? g.Cx : g.Cols - 1, g.Cy, false);
        g.CursorDrawn = false;
    }
}

static void show_cursor(void)
{
    if (g.CursorVisible && !g.CursorDrawn) {
        draw_cell(g.Cx < g.Cols ? g.Cx : g.Cols - 1, g.Cy, true);
        g.CursorDrawn = true;
    }
}

/* ---- Cell grid ---------------------------------------------------------- */

static uint32_t cur_fg(void) { return g.Reverse ? g.Bg : g.Fg; }
static uint32_t cur_bg(void) { return g.Reverse ? g.Fg : g.Bg; }

static void clear_cells(unsigned row, unsigned col0, unsigned col1)
{
    for (unsigned c = col0; c < col1 && c < g.Cols; c++) {
        g_cells[row][c] = (CELL){ .Fg = cur_fg(), .Bg = cur_bg(), .Ch = ' ' };
        draw_cell(c, row, false);
    }
}

static void redraw_all(void)
{
    for (unsigned r = 0; r < g.Rows; r++)
        for (unsigned c = 0; c < g.Cols; c++)
            draw_cell(c, r, false);
}

static void scroll_up(void)
{
    memmove(&g_cells[0][0], &g_cells[1][0], sizeof(g_cells[0]) * (g.Rows - 1));
    for (unsigned c = 0; c < g.Cols; c++)
        g_cells[g.Rows - 1][c] = (CELL){ .Fg = cur_fg(), .Bg = cur_bg(), .Ch = ' ' };
    redraw_all();
}

static void newline(void)
{
    g.Cx = 0;
    if (++g.Cy >= g.Rows) {
        g.Cy = g.Rows - 1;
        scroll_up();
    }
}

static void put_glyph(uint8_t ch)
{
    if (g.Cx >= g.Cols)
        newline();
    g_cells[g.Cy][g.Cx] = (CELL){ .Fg = cur_fg(), .Bg = cur_bg(), .Ch = ch };
    draw_cell(g.Cx, g.Cy, false);
    g.Cx++;
}

static uint8_t unicode_to_glyph(uint32_t cp)
{
    if (cp >= 0x20 && cp < 0x7F)
        return (uint8_t)cp;
    for (int i = 1; i < 256; i++)
        if (KFontCp437Unicode[i] == cp)
            return (uint8_t)i;
    return '?';
}

/* ---- Escape sequences --------------------------------------------------- */

static uint32_t xterm256(int n)
{
    if (n < 16)
        return g_palette[n];
    if (n < 232) {
        n -= 16;
        static const uint8_t lv[6] = { 0, 95, 135, 175, 215, 255 };
        return ((uint32_t)lv[n / 36] << 16) | ((uint32_t)lv[(n / 6) % 6] << 8) | lv[n % 6];
    }
    uint32_t v = 8 + (uint32_t)(n - 232) * 10;
    return (v << 16) | (v << 8) | v;
}

static void sgr(void)
{
    if (g.NParams == 0)
        g.Params[g.NParams++] = 0;
    for (int i = 0; i < g.NParams; i++) {
        int p = g.Params[i];
        if (p == 0) { g.Fg = g.DefFg; g.Bg = g.DefBg; g.Bold = g.Reverse = false; }
        else if (p == 1) g.Bold = true;
        else if (p == 22) g.Bold = false;
        else if (p == 7) g.Reverse = true;
        else if (p == 27) g.Reverse = false;
        else if (p >= 30 && p <= 37) g.Fg = g_palette[p - 30 + (g.Bold ? 8 : 0)];
        else if (p >= 90 && p <= 97) g.Fg = g_palette[p - 90 + 8];
        else if (p >= 40 && p <= 47) g.Bg = g_palette[p - 40];
        else if (p >= 100 && p <= 107) g.Bg = g_palette[p - 100 + 8];
        else if (p == 39) g.Fg = g.DefFg;
        else if (p == 49) g.Bg = g.DefBg;
        else if ((p == 38 || p == 48) && i + 1 < g.NParams) {
            uint32_t col;
            if (g.Params[i + 1] == 5 && i + 2 < g.NParams) {
                col = xterm256(g.Params[i + 2] & 0xFF);
                i += 2;
            } else if (g.Params[i + 1] == 2 && i + 4 < g.NParams) {
                col = ((uint32_t)(g.Params[i + 2] & 0xFF) << 16) | ((uint32_t)(g.Params[i + 3] & 0xFF) << 8) |
                      (uint32_t)(g.Params[i + 4] & 0xFF);
                i += 4;
            } else {
                continue;
            }
            if (p == 38) g.Fg = col; else g.Bg = col;
        }
    }
}

static int param(int i, int def)
{
    return (i < g.NParams && g.Params[i] > 0) ? g.Params[i] : def;
}

static void csi(char final)
{
    switch (final) {
    case 'A': g.Cy = g.Cy >= (unsigned)param(0, 1) ? g.Cy - param(0, 1) : 0; break;
    case 'B': g.Cy = MIN(g.Rows - 1, g.Cy + param(0, 1)); break;
    case 'C': g.Cx = MIN(g.Cols - 1, g.Cx + param(0, 1)); break;
    case 'D': g.Cx = g.Cx >= (unsigned)param(0, 1) ? g.Cx - param(0, 1) : 0; break;
    case 'G': g.Cx = MIN(g.Cols - 1, (unsigned)param(0, 1) - 1); break;
    case 'd': g.Cy = MIN(g.Rows - 1, (unsigned)param(0, 1) - 1); break;
    case 'H':
    case 'f':
        g.Cy = MIN(g.Rows - 1, (unsigned)param(0, 1) - 1);
        g.Cx = MIN(g.Cols - 1, (unsigned)param(1, 1) - 1);
        break;
    case 'J': {
        int mode = g.NParams ? g.Params[0] : 0;
        if (mode == 2 || mode == 3) {
            for (unsigned r = 0; r < g.Rows; r++)
                clear_cells(r, 0, g.Cols);
        } else if (mode == 0) {
            clear_cells(g.Cy, g.Cx, g.Cols);
            for (unsigned r = g.Cy + 1; r < g.Rows; r++)
                clear_cells(r, 0, g.Cols);
        } else if (mode == 1) {
            for (unsigned r = 0; r < g.Cy; r++)
                clear_cells(r, 0, g.Cols);
            clear_cells(g.Cy, 0, g.Cx + 1);
        }
        break;
    }
    case 'K': {
        int mode = g.NParams ? g.Params[0] : 0;
        if (mode == 0) clear_cells(g.Cy, g.Cx, g.Cols);
        else if (mode == 1) clear_cells(g.Cy, 0, g.Cx + 1);
        else clear_cells(g.Cy, 0, g.Cols);
        break;
    }
    case 'm': sgr(); break;
    case 's': g.SavedCx = g.Cx; g.SavedCy = g.Cy; break;
    case 'u': g.Cx = g.SavedCx; g.Cy = g.SavedCy; break;
    case 'h':
    case 'l':
        if (g.Private && param(0, 0) == 25)
            g.CursorVisible = final == 'h';
        break;
    default:
        break;
    }
}

static void put_char(uint8_t c)
{
    if (g.EscState == 1) {
        if (c == '[') {
            g.EscState = 2;
            g.NParams = 0;
            g.Private = false;
            memset(g.Params, 0, sizeof(g.Params));
            return;
        }
        g.EscState = 0;         /* unsupported two-byte escape: ignore it */
        if (c == 'c') {          /* RIS: reset */
            g.Fg = g.DefFg; g.Bg = g.DefBg; g.Bold = g.Reverse = false;
            FbConClear();
        }
        return;
    }
    if (g.EscState == 2) {
        if (c == '?') { g.Private = true; return; }
        if (c >= '0' && c <= '9') {
            if (g.NParams == 0) g.NParams = 1;
            int *p = &g.Params[g.NParams - 1];
            if (*p < 10000) *p = *p * 10 + (c - '0');
            return;
        }
        if (c == ';') {
            if (g.NParams == 0) g.NParams = 1;
            if (g.NParams < (int)ARRAY_SIZE(g.Params)) g.NParams++;
            return;
        }
        g.EscState = 0;
        csi((char)c);
        return;
    }

    /* UTF-8 decoding */
    if (g.Utf8Left) {
        if ((c & 0xC0) == 0x80) {
            g.Utf8Cp = (g.Utf8Cp << 6) | (c & 0x3F);
            if (--g.Utf8Left == 0)
                put_glyph(unicode_to_glyph(g.Utf8Cp));
            return;
        }
        g.Utf8Left = 0;
        put_glyph('?');
    }
    if (c >= 0x80) {
        if ((c & 0xE0) == 0xC0) { g.Utf8Cp = c & 0x1F; g.Utf8Left = 1; }
        else if ((c & 0xF0) == 0xE0) { g.Utf8Cp = c & 0x0F; g.Utf8Left = 2; }
        else if ((c & 0xF8) == 0xF0) { g.Utf8Cp = c & 0x07; g.Utf8Left = 3; }
        else put_glyph('?');
        return;
    }

    switch (c) {
    case '\x1b': g.EscState = 1; break;
    case '\n': newline(); break;
    case '\r': g.Cx = 0; break;
    case '\b': if (g.Cx) g.Cx--; break;
    case '\t': do put_glyph(' '); while (g.Cx % 8 && g.Cx < g.Cols); break;
    case '\a': break;
    default:
        if (c >= 0x20)
            put_glyph(c);
        break;
    }
}

/* ---- Public interface --------------------------------------------------- */

bool FbConInit(struct limine_framebuffer *fb)
{
    if (!fb || (fb->bpp != 32 && fb->bpp != 24 && fb->bpp != 16) || fb->memory_model != LIMINE_FRAMEBUFFER_RGB)
        return false;
    g.Base = fb->address;
    g.Width = (uint32_t)fb->width;
    g.Height = (uint32_t)fb->height;
    g.Pitch = (uint32_t)fb->pitch;
    g.Bpp = fb->bpp;
    g.RShift = fb->red_mask_shift;   g.RSize = fb->red_mask_size;
    g.GShift = fb->green_mask_shift; g.GSize = fb->green_mask_size;
    g.BShift = fb->blue_mask_shift;  g.BSize = fb->blue_mask_size;
    g.Font = (g.Width >= 2560 && g.Height >= 1440) ? &KFontTerminus32 : &KFontTerminus16;
    g.Cols = MIN(MAX_COLS, g.Width / g.Font->Width);
    g.Rows = MIN(MAX_ROWS, g.Height / g.Font->Height);
    g.OffX = (g.Width - g.Cols * g.Font->Width) / 2;
    g.OffY = (g.Height - g.Rows * g.Font->Height) / 2;
    g.DefFg = g.Fg = CON_LIGHTGRAY;
    g.DefBg = g.Bg = CON_BLACK;
    g.CursorVisible = true;
    g.Present = true;
    FbConClear();
    return true;
}

bool FbConPresent(void)
{
    return g.Present;
}

void FbConClear(void)
{
    if (!g.Present)
        return;
    fill_rect(0, 0, g.Width, g.Height, g.Bg);
    for (unsigned r = 0; r < g.Rows; r++)
        for (unsigned c = 0; c < g.Cols; c++)
            g_cells[r][c] = (CELL){ .Fg = g.Fg, .Bg = g.Bg, .Ch = ' ' };
    g.Cx = g.Cy = 0;
    g.CursorDrawn = false;
    show_cursor();
}

void FbConSetColors(uint32_t fg, uint32_t bg)
{
    g.DefFg = g.Fg = fg;
    g.DefBg = g.Bg = bg;
}

void FbConWrite(const char *s, size_t len)
{
    if (!g.Present)
        return;
    hide_cursor();
    for (size_t i = 0; i < len; i++)
        put_char((uint8_t)s[i]);
    show_cursor();
}

void FbConGetSize(unsigned *cols, unsigned *rows)
{
    *cols = g.Present ? g.Cols : 80;
    *rows = g.Present ? g.Rows : 25;
}

void FbConGetResolution(unsigned *width, unsigned *height, unsigned *font_w, unsigned *font_h)
{
    *width = g.Width;
    *height = g.Height;
    *font_w = g.Font ? g.Font->Width : 0;
    *font_h = g.Font ? g.Font->Height : 0;
}
