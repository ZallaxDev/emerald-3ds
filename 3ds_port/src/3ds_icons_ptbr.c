/*
 * The Portuguese build's graphics with English words, redrawn when they are
 * read from the data pack (ReadPayload, 3ds_assets.c): the player's own
 * sheet keeps its frames and colours, its English letters are cleared and
 * the Portuguese name is written in a small capital font drawn for this
 * project. No pixel of the game is kept here.
 *
 * tools/type_icons_ptbr.py draws the same thing into a PNG, as a preview.
 */
#include <stdlib.h>
#include <string.h>

/* Declared in compat/port_platform.h, which 3ds_assets.c includes. */
void Port_TransformAsset(const char *path, unsigned char *data, unsigned size);
const unsigned char *Port_LatinGlyph(unsigned font, unsigned char code, int *width);

#if CTR_LANG_PT_BR

/* 4bpp tiles, 16 to a row: graphics/interface/menu_info.4bpp is 128x128. */
#define SHEET_TILES_WIDE 16
#define INK 0xF
#define SHADOW 0xE

/* The sheet being drawn: menu_info is 16 tiles wide, a summary type icon
 * (a 32x16 sprite) 4. */
static int sTilesWide = SHEET_TILES_WIDE;

static unsigned SheetPixel(const unsigned char *tiles, int x, int y)
{
    const unsigned char *tile = tiles + ((y / 8) * sTilesWide + x / 8) * 32;
    unsigned char byte = tile[(y % 8) * 4 + (x % 8) / 2];
    return (x & 1) ? byte >> 4 : byte & 0xF;
}

static void SetSheetPixel(unsigned char *tiles, int x, int y, unsigned value)
{
    unsigned char *tile = tiles + ((y / 8) * sTilesWide + x / 8) * 32;
    unsigned char *byte = &tile[(y % 8) * 4 + (x % 8) / 2];
    *byte = (x & 1) ? (unsigned char)((*byte & 0x0F) | (value << 4)) : (unsigned char)((*byte & 0xF0) | value);
}

/*
 * A capital font 3 pixels wide (M 5, N 4) and 7 high, '#' for ink; the
 * accents sit on the row above. Drawn for this project.
 */
struct Glyph
{
    char ch;
    const char *rows[7];
};

static const struct Glyph sGlyphs[] = {
    {'A', {".#.", "#.#", "#.#", "###", "#.#", "#.#", "#.#"}},
    {'B', {"##.", "#.#", "#.#", "##.", "#.#", "#.#", "##."}},
    {'C', {".##", "#..", "#..", "#..", "#..", "#..", ".##"}},
    {'D', {"##.", "#.#", "#.#", "#.#", "#.#", "#.#", "##."}},
    {'E', {"###", "#..", "#..", "##.", "#..", "#..", "###"}},
    {'F', {"###", "#..", "#..", "##.", "#..", "#..", "#.."}},
    {'G', {".##", "#..", "#..", "#.#", "#.#", "#.#", ".##"}},
    {'H', {"#.#", "#.#", "#.#", "###", "#.#", "#.#", "#.#"}},
    {'I', {"###", ".#.", ".#.", ".#.", ".#.", ".#.", "###"}},
    {'L', {"#..", "#..", "#..", "#..", "#..", "#..", "###"}},
    {'M', {"#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"}},
    {'N', {"#..#", "##.#", "##.#", "#.##", "#.##", "#..#", "#..#"}},
    {'O', {".#.", "#.#", "#.#", "#.#", "#.#", "#.#", ".#."}},
    {'P', {"##.", "#.#", "#.#", "##.", "#..", "#..", "#.."}},
    {'Q', {".#.", "#.#", "#.#", "#.#", "#.#", "##.", ".##"}},
    {'R', {"##.", "#.#", "#.#", "##.", "#.#", "#.#", "#.#"}},
    {'S', {".##", "#..", "#..", ".#.", "..#", "..#", "##."}},
    {'T', {"###", ".#.", ".#.", ".#.", ".#.", ".#.", ".#."}},
    {'U', {"#.#", "#.#", "#.#", "#.#", "#.#", "#.#", "###"}},
    {'V', {"#.#", "#.#", "#.#", "#.#", "#.#", ".#.", ".#."}},
    {'X', {"#.#", "#.#", "#.#", ".#.", "#.#", "#.#", "#.#"}},
    {'Z', {"###", "..#", "..#", ".#.", "#..", "#..", "###"}},
    {' ', {"..", "..", "..", "..", "..", "..", ".."}},
    {'.', {".", ".", ".", ".", ".", ".", "#"}},
};

/* The names as byte strings: 'a' stands for Á, 'e' for É, 'i' for Í, 'c' for
 * Ç and '~' for Ã, so the table stays ASCII. */
static const struct
{
    unsigned short tile;
    const char *name;
} sTypeNames[] = {
    {0x20, "NORMAL"}, {0x64, "LUTADOR"}, {0x60, "VOADOR"}, {0x80, "VENENO"}, {0x48, "TERRA"},
    {0x44, "PEDRA"}, {0x6C, "INSETO"}, {0x68, "FANTAS"}, {0x88, "AcO"}, {0x24, "FOGO"},
    {0x28, "aGUA"}, {0x2C, "PLANTA"}, {0x40, "ELeTR."}, {0x84, "PSiQ."}, {0x4C, "GELO"},
    {0xA0, "DRAG~O"}, {0x8C, "SOMBR."},
};

/* Ink of letter ch on row r (-1 the accent row, 0-6 the letter, 7 the
 * cedilla's tail) at column c; its width in *width. */
static int GlyphInk(char ch, int r, int c, int *width)
{
    char base = ch;
    unsigned i;

    switch (ch)
    {
    case 'a': case '~': base = 'A'; break;
    case 'e': base = 'E'; break;
    case 'i': base = 'I'; break;
    case 'c': base = 'C'; break;
    }
    for (i = 0; i < sizeof(sGlyphs) / sizeof(sGlyphs[0]); i++)
    {
        if (sGlyphs[i].ch != base)
            continue;
        *width = (int)strlen(sGlyphs[i].rows[0]);
        if (r == -1)
        {
            if (ch == 'a' || ch == 'e' || ch == 'i')
                return c == 1;
            if (ch == '~')
                return 1;
            return 0;
        }
        if (r == 7)
            return ch == 'c' && c == 1;
        return sGlyphs[i].rows[r][c] == '#';
    }
    *width = 0;
    return 0;
}

/* The icon's letters sit on rows top to top + 6 (the accents on top - 1);
 * rows first to last - 1 are cleared. */
static void DrawTypeNameAt(unsigned char *tiles, int x0, int y0, int first, int last, int top, const char *name)
{
    unsigned rowBg[16];
    int total = -1, x, y;
    const char *p;

    /* Some icons fade to a second colour at the bottom: each row is
     * cleared with its own background, read at the icon's right edge. */
    for (y = 0; y < 16; y++)
        rowBg[y] = 0;
    for (y = first; y < last; y++)
    {
        for (x = 31; x > 0; x--)
        {
            unsigned v = SheetPixel(tiles, x0 + x, y0 + y);
            if (v != INK && v != SHADOW && v != 0)
            {
                rowBg[y] = v;
                break;
            }
        }
        for (x = 0; x < 32; x++)
        {
            unsigned v = SheetPixel(tiles, x0 + x, y0 + y);
            if (v == INK || v == SHADOW)
                SetSheetPixel(tiles, x0 + x, y0 + y, rowBg[y]);
        }
    }
    for (p = name; *p; p++)
    {
        int w;
        GlyphInk(*p, 0, 0, &w);
        total += w + 1;
    }
    x = x0 + (31 - total) / 2;
    for (p = name; *p; p++)
    {
        int w, r, c;
        GlyphInk(*p, 0, 0, &w);
        for (r = -1; r <= 7; r++)
        {
            for (c = 0; c < w; c++)
            {
                int dx, dy, ink;
                int dummy;
                ink = GlyphInk(*p, r, c, &dummy);
                if (!ink)
                    continue;
                y = y0 + top + r;
                SetSheetPixel(tiles, x + c, y, INK);
                for (dy = 0; dy <= 1; dy++)
                    for (dx = 0; dx <= 1; dx++)
                    {
                        int sy = y + dy - y0;
                        if ((dx || dy) && sy < last && SheetPixel(tiles, x + c + dx, y + dy) == rowBg[sy])
                            SetSheetPixel(tiles, x + c + dx, y + dy, SHADOW);
                    }
            }
        }
        x += w + 1;
    }
}

static void DrawTypeName(unsigned char *tiles, unsigned tile, const char *name)
{
    /* Rows 0-11 cleared, the letter on rows 2-8. */
    DrawTypeNameAt(tiles, (int)(tile % SHEET_TILES_WIDE) * 8, (int)(tile / SHEET_TILES_WIDE) * 8, 0, 12, 2, name);
}

/* The summary's type icons (graphics/types/move_types.4bpp), one 32x16
 * sprite of 8 tiles (256 bytes) per type, in the order of TYPE_*, then the
 * contest categories (COOL, BEAUTY, CUTE, SMART, TOUGH); ??? stays. */
static const char *const sSummaryTypeNames[] = {
    "NORMAL", "LUTADOR", "VOADOR", "VENENO", "TERRA", "PEDRA", "INSETO", "FANTAS", "AcO", NULL,
    "FOGO", "aGUA", "PLANTA", "ELeTR.", "PSiQ.", "GELO", "DRAG~O", "SOMBR.",
    "ESTILO", "BELEZA", "FOFURA", "ESPERT.", "FORcA",
};

/* The move window's labels, 42x12: white letters outlined in the colour of
 * the bar they sit on (rows 4-8). '~' stands for Ã. */
static const struct
{
    unsigned short tile;
    const char *name;
} sLabels[] = {
    {0xA8, "TIPO"}, {0xC0, "PODER"}, {0xC8, "PRECIS~O"}, {0xE0, "PP"}, {0xE8, "EFEITO"},
};

#define LABEL_W 42
#define LABEL_H 12

static void DrawLabel(unsigned char *tiles, unsigned tile, const char *name)
{
    int x0 = (int)(tile % SHEET_TILES_WIDE) * 8, y0 = (int)(tile / SHEET_TILES_WIDE) * 8;
    unsigned char inked[LABEL_H][LABEL_W];
    int total = -1, x, y;
    const char *p;

    /* The bar back without its letters, and nothing above or below it. */
    for (y = 0; y < LABEL_H; y++)
        for (x = 0; x < LABEL_W; x++)
        {
            if (y >= 4 && y <= 8)
            {
                if (SheetPixel(tiles, x0 + x, y0 + y) != 0)
                    SetSheetPixel(tiles, x0 + x, y0 + y, SHADOW);
            }
            else
            {
                SetSheetPixel(tiles, x0 + x, y0 + y, 0);
            }
        }
    memset(inked, 0, sizeof(inked));
    for (p = name; *p; p++)
    {
        int w;
        GlyphInk(*p, 0, 0, &w);
        total += w + 1;
    }
    x = (LABEL_W - 1 - total) / 2;
    for (p = name; *p; p++)
    {
        int w, r, c, dummy;
        GlyphInk(*p, 0, 0, &w);
        for (r = -1; r <= 7; r++)
            for (c = 0; c < w; c++)
            {
                int ly = 3 + r; /* row -1 on the label's row 2, the letter on rows 3-9 */
                if (GlyphInk(*p, r, c, &dummy) && ly >= 0 && ly < LABEL_H && x + c < LABEL_W)
                    inked[ly][x + c] = 1;
            }
        x += w + 1;
    }
    for (y = 0; y < LABEL_H; y++)
        for (x = 0; x < LABEL_W; x++)
        {
            int dx, dy;
            if (!inked[y][x])
                continue;
            for (dy = -1; dy <= 1; dy++)
                for (dx = -1; dx <= 1; dx++)
                {
                    int nx = x + dx, ny = y + dy;
                    if (nx >= 0 && ny >= 0 && nx < LABEL_W && ny < LABEL_H && !inked[ny][nx])
                        SetSheetPixel(tiles, x0 + nx, y0 + ny, SHADOW);
                }
        }
    for (y = 0; y < LABEL_H; y++)
        for (x = 0; x < LABEL_W; x++)
            if (inked[y][x])
                SetSheetPixel(tiles, x0 + x, y0 + y, INK);
}

/*
 * The summary's page titles (graphics/summary_screen/tiles.4bpp, 16 tiles to
 * a row): one 8-pixel row of tiles each, the letters in colour 2 with the
 * shadow 6 below and to the right, on the strip's colour. The font loses its
 * third row there, so the accents fit on the tile's row 0 and the cedilla on
 * row 7.
 */
#define TITLE_INK 0x2
#define TITLE_SHADOW 0x6

static const struct
{
    unsigned short tile;
    unsigned char tiles;
    const char *name;
} sSummaryTitles[] = {
    {128, 8, "PERFIL"}, {144, 9, "HABILIDADE"}, {176, 9, "ESTADO"}, {224, 9, "MEMO TREINADOR"},
    {192, 8, "GOLPES"}, {202, 6, "EFEITO"}, {208, 10, "DESCRIc~O"},
    {137, 7, "ITEM"}, {153, 7, "FITAS"}, {169, 7, "ATRIBUTOS"}, {185, 7, "EXP."},
};

static void DrawSummaryTitle(unsigned char *tiles, unsigned tile, int width, const char *name)
{
    int x0 = (int)(tile % SHEET_TILES_WIDE) * 8, y0 = (int)(tile / SHEET_TILES_WIDE) * 8;
    unsigned char inked[8][80];
    int x, y;
    const char *p;

    for (y = 0; y < 8; y++)
    {
        /* The strip's colour: the commonest one besides the letters' (the
         * right end may have a border). */
        unsigned count[16] = {0}, bg = 0;
        for (x = 0; x < width; x++)
            count[SheetPixel(tiles, x0 + x, y0 + y)]++;
        count[TITLE_INK] = count[TITLE_SHADOW] = 0;
        for (x = 1; x < 16; x++)
            if (count[x] > count[bg])
                bg = (unsigned)x;
        for (x = 0; x < width; x++)
        {
            unsigned v = SheetPixel(tiles, x0 + x, y0 + y);
            if (v == TITLE_INK || v == TITLE_SHADOW)
                SetSheetPixel(tiles, x0 + x, y0 + y, bg);
        }
    }
    memset(inked, 0, sizeof(inked));
    x = 2;
    for (p = name; *p; p++)
    {
        int w, t, c, dummy;
        GlyphInk(*p, 0, 0, &w);
        for (t = 0; t < 8; t++)
        {
            /* Tile row 0 the accent, 1-6 the letter without its row 2, 7 the cedilla. */
            int r = t == 0 ? -1 : t == 7 ? 7 : t < 3 ? t - 1 : t;
            for (c = 0; c < w; c++)
                if (x + c < width - 1 && GlyphInk(*p, r, c, &dummy))
                    inked[t][x + c] = 1;
        }
        x += w + 1;
    }
    for (y = 0; y < 8; y++)
        for (x = 0; x < width; x++)
        {
            int dx, dy;
            if (!inked[y][x])
                continue;
            for (dy = 0; dy <= 1; dy++)
                for (dx = 0; dx <= 1; dx++)
                    if ((dx || dy) && y + dy < 8 && x + dx < width && !inked[y + dy][x + dx])
                        SetSheetPixel(tiles, x0 + x + dx, y0 + y + dy, TITLE_SHADOW);
            SetSheetPixel(tiles, x0 + x, y0 + y, TITLE_INK);
        }
}

/*
 * PRESS START on the title screen (graphics/title_screen/press_start.4bpp,
 * 16 tiles to a row): five 32x8 sprites from tile 1, 160 pixels in a line.
 * APERTE START is put together from the sheet's own letters, each cut from
 * its left outline to its right one (the next letter shares that column).
 */
static const struct
{
    char ch;
    unsigned char from, to;
} sStartCells[] = {
    {'P', 35, 43}, {'R', 43, 51}, {'E', 51, 59}, {'T', 91, 100}, {'A', 100, 107}, {'*', 83, 124}, /* '*' the word START */
};

static int StartLinear(int l, int y, int *sx, int *sy)
{
    int tile = 1 + l / 8;
    *sx = (tile % SHEET_TILES_WIDE) * 8 + l % 8;
    *sy = (tile / SHEET_TILES_WIDE) * 8 + y;
    return tile <= 20;
}

static void DrawPressStart(unsigned char *tiles)
{
    static const char sText[] = "APERTE *";
    unsigned char source[8][128], line[8][160];
    int l = 23, x, y, sx, sy;
    const char *p;
    unsigned i;

    for (y = 0; y < 8; y++)
        for (x = 0; x < 128; x++)
            source[y][x] = (unsigned char)SheetPixel(tiles, x, y);
    memset(line, 0, sizeof(line));
    for (p = sText; *p; p++)
    {
        if (*p == ' ')
        {
            l += 7;
            continue;
        }
        for (i = 0; i < sizeof(sStartCells) / sizeof(sStartCells[0]); i++)
        {
            if (sStartCells[i].ch != *p)
                continue;
            for (x = sStartCells[i].from; x <= sStartCells[i].to; x++)
                for (y = 0; y < 8; y++)
                {
                    unsigned v = source[y][x];
                    int at = l + x - sStartCells[i].from;
                    /* The A's column 100 above its apex is the T's bar. */
                    if (*p == 'A' && x == 100 && y <= 3)
                        v = 0;
                    /* An outline (1) gives way to a letter's colours. */
                    if (v != 0 && at < 160 && (line[y][at] == 0 || line[y][at] == 1))
                        line[y][at] = (unsigned char)v;
                }
            l += sStartCells[i].to - sStartCells[i].from;
            break;
        }
    }
    for (l = 0; l < 160; l++)
        for (y = 0; y < 8; y++)
            if (StartLinear(l, y, &sx, &sy))
                SetSheetPixel(tiles, sx, sy, line[y][l]);
}

/*
 * The status icons (graphics/interface/status_icons.4bpp, 4 tiles to a row):
 * one 32x8 icon per row in the order of the summary (PSN, PAR, SLP, FRZ,
 * BRN, PKRS, FNT), the letters in colour 2 on rows 1-6, no shadow. The font
 * loses its third row, as in the summary's titles; PKRS stays.
 */
static const char *const sStatusNames[] = {"ENV", "PAR", "DOR", "CON", "QUE", NULL, "DES"};

static void DrawStatusIcon(unsigned char *tiles, int y0, const char *name)
{
    int total = -1, x, y;
    const char *p;

    for (y = 1; y <= 6; y++)
    {
        unsigned bg = SheetPixel(tiles, 8, y0 + y); /* left of the letters */
        for (x = 0; x < 32; x++)
            if (SheetPixel(tiles, x, y0 + y) == TITLE_INK)
                SetSheetPixel(tiles, x, y0 + y, bg);
    }
    for (p = name; *p; p++)
    {
        int w;
        GlyphInk(*p, 0, 0, &w);
        total += w + 1;
    }
    x = (32 - total) / 2;
    for (p = name; *p; p++)
    {
        int w, t, c, dummy;
        GlyphInk(*p, 0, 0, &w);
        for (t = 1; t <= 6; t++)
        {
            int r = t < 3 ? t - 1 : t;
            for (c = 0; c < w; c++)
                if (GlyphInk(*p, r, c, &dummy))
                    SetSheetPixel(tiles, x + c, y0 + t, TITLE_INK);
        }
        x += w + 1;
    }
}

/*
 * Words written with the game's own fonts (Port_LatinGlyph, 3ds_compat.c),
 * for the graphics whose letters are as big as the text's: each label is a
 * box of the sheet, the English ink and shadow in it cleared to the row's
 * background, the Portuguese in the same colours. A text wider than the box
 * in the normal font is written in the narrow one.
 */
enum { FONT_SMALL = 1, FONT_NARROW = 2, FONT_SHORT = 3, FONT_NORMAL = 4 };
#define NO_SHADOW 0x10

/* The game's character code of the UTF-8 letter at *s (the few the labels
 * use), advancing *s; -1 for one it does not know. */
static int NextGameChar(const char **s)
{
    static const struct { const char *utf8; unsigned char code; } sAccents[] = {
        {"À", 0x01}, {"Á", 0x02}, {"Â", 0x03}, {"Ç", 0x04}, {"É", 0x06}, {"Ê", 0x07}, {"Í", 0x5A},
        {"Ó", 0x0E}, {"Ô", 0x0F}, {"Ú", 0x12}, {"Ã", 0x2F}, {"Õ", 0x30}, {"é", 0x1B},
    };
    unsigned char c = (unsigned char)**s;
    unsigned i;

    if (c >= 'A' && c <= 'Z') { (*s)++; return 0xBB + (c - 'A'); }
    if (c >= 'a' && c <= 'z') { (*s)++; return 0xD5 + (c - 'a'); }
    if (c >= '0' && c <= '9') { (*s)++; return 0xA1 + (c - '0'); }
    if (c == ' ') { (*s)++; return 0x00; }
    if (c == '.') { (*s)++; return 0xAD; }
    for (i = 0; i < sizeof(sAccents) / sizeof(sAccents[0]); i++)
    {
        size_t n = strlen(sAccents[i].utf8);
        if (strncmp(*s, sAccents[i].utf8, n) == 0)
        {
            *s += n;
            return sAccents[i].code;
        }
    }
    (*s)++;
    return -1;
}

static unsigned GlyphValue(const unsigned char *glyph, int x, int y)
{
    unsigned char byte = glyph[((y / 8) * 2 + x / 8) * 16 + (y % 8) * 2 + ((x % 8) < 4)];
    return (byte >> (6 - 2 * (x % 4))) & 3;
}

static int GameTextWidth(unsigned font, const char *text)
{
    int total = 0, w, code;

    while (*text)
        if ((code = NextGameChar(&text)) >= 0 && Port_LatinGlyph(font, (unsigned char)code, &w) != NULL)
            total += w;
    return total;
}

/* The first row of B that the font inks: the top of its capitals. */
static int CapitalTop(unsigned font)
{
    int w, x, y;
    const unsigned char *glyph = Port_LatinGlyph(font, 0xBB + 1, &w);

    for (y = 0; glyph != NULL && y < 16; y++)
        for (x = 0; x < 16; x++)
            if (GlyphValue(glyph, x, y) == 1 || GlyphValue(glyph, x, y) == 2)
                return y;
    return 0;
}

struct GameLabel
{
    unsigned char x, y, width, height; /* the box in the sheet */
    unsigned char capitals;            /* the row the capitals start on */
    unsigned char ink, shadow; /* shadow NO_SHADOW for text without one */
    unsigned char centred;
    const char *text;
    unsigned char font; /* 0 for the normal font */
};

static void DrawGameLabel(unsigned char *tiles, const struct GameLabel *label)
{
    unsigned font = label->font ? label->font : FONT_NORMAL;
    int x, y, top, width, code, w, tight = 0, pass;
    const char *p;

    for (y = label->y; y < label->y + label->height; y++)
    {
        /* Each pixel of the English text takes the colour of the nearest
         * pixel of the same row that is not text, left first: the
         * button's own background, even next to its border. */
        unsigned char row[256];
        for (x = 0; x < label->width; x++)
            row[x] = (unsigned char)SheetPixel(tiles, label->x + x, y);
        for (x = 0; x < label->width; x++)
        {
            int d;
            if (row[x] != label->ink && row[x] != label->shadow)
                continue;
            for (d = 1; d < label->width; d++)
            {
                if (x - d >= 0 && row[x - d] != label->ink && row[x - d] != label->shadow)
                {
                    SetSheetPixel(tiles, label->x + x, y, row[x - d]);
                    break;
                }
                if (x + d < label->width && row[x + d] != label->ink && row[x + d] != label->shadow)
                {
                    SetSheetPixel(tiles, label->x + x, y, row[x + d]);
                    break;
                }
            }
        }
    }
    /* Narrow when it would not leave a few pixels to the box's edges. */
    if (font != FONT_NARROW && font != FONT_SMALL
     && GameTextWidth(font, label->text) > label->width - (label->centred && label->width >= 40 ? 6 : 0))
        font = FONT_NARROW;
    width = GameTextWidth(font, label->text);
    /* Still too wide: the letters a pixel closer together. */
    if (width > label->width)
    {
        int letters = 0;
        for (p = label->text; *p;)
            if (NextGameChar(&p) >= 0)
                letters++;
        if (width - (letters - 1) <= label->width)
        {
            tight = 1;
            width -= letters - 1;
        }
    }
    top = label->capitals - CapitalTop(font);
    /* The shadows first and the ink over them, so that letters a pixel
     * closer together keep their ink. */
    for (pass = 2; pass >= 1; pass--)
    {
        x = label->x + (label->centred ? (label->width - width) / 2 : 0);
        for (p = label->text; *p;)
        {
            const unsigned char *glyph;
            int gx, gy;

            if ((code = NextGameChar(&p)) < 0 || (glyph = Port_LatinGlyph(font, (unsigned char)code, &w)) == NULL)
                continue;
            for (gy = 0; gy < 16; gy++)
                for (gx = 0; gx < w; gx++)
                {
                    int px = x + gx, py = top + gy;
                    /* An accent above a box too short for it: on its top row. */
                    if (py < label->y && py >= label->y - 3)
                        py = label->y;
                    if (GlyphValue(glyph, gx, gy) == (unsigned)pass && (pass == 1 || label->shadow != NO_SHADOW)
                     && px >= label->x && px < label->x + label->width
                     && py >= label->y && py < label->y + label->height)
                        SetSheetPixel(tiles, px, py, pass == 1 ? label->ink : label->shadow);
                }
            x += w - tight;
        }
    }
}

/* graphics/pokedex/menu.4bpp, 16 tiles to a row: the tabs of a species'
 * page, the B button's and the submenu's CANCEL, and the START menu. */
static const struct GameLabel sPokedexMenuLabels[] = {
    {16, 17, 24, 14, 20, 0xF, 0xD, 1, "ÁREA", 0},
    {48, 17, 40, 14, 20, 0xF, 0xD, 1, "GRITO", 0},
    {88, 17, 40, 14, 20, 0xF, 0xD, 1, "TAMANHO", 0},
    {18, 34, 38, 13, 36, 0x9, 0x5, 0, "VOLTAR", 0},
    {64, 34, 40, 13, 36, 0xF, 0x2, 1, "VOLTAR", 0},
    {0, 48, 72, 16, 51, 0x9, 0x5, 0, "VOLTAR À LISTA", 0},
    {0, 64, 72, 16, 67, 0x9, 0x5, 0, "TOPO DA LISTA", 0},
    {0, 80, 72, 16, 83, 0x9, 0x5, 0, "FIM DA LISTA", 0},
    {0, 96, 88, 16, 99, 0x9, 0x5, 0, "VOLTAR À POKéDEX", 0},
    {0, 112, 88, 16, 115, 0x9, 0x5, 0, "FECHAR POKéDEX", 0},
};

/* graphics/pokedex/search_menu.4bpp: the search's buttons, 40x16 each, in
 * the short font. */
static const struct GameLabel sPokedexSearchLabels[] = {
    {0, 16, 40, 16, 20, 0x4, 0xB, 1, "MODO", FONT_SHORT},
    {40, 16, 40, 16, 20, 0x4, 0xB, 1, "ORDEM", FONT_SHORT},
    {80, 16, 40, 16, 20, 0x4, 0xB, 1, "NOME", FONT_SHORT},
    {0, 32, 40, 16, 36, 0x4, 0xB, 1, "COR", FONT_SHORT},
    {40, 32, 40, 16, 36, 0x4, 0xB, 1, "TIPO", FONT_SHORT},
    {80, 32, 40, 16, 36, 0x4, 0x9, 1, "VOLTAR", FONT_SHORT},
    {0, 48, 40, 16, 52, 0x4, 0x9, 1, "OK", FONT_SHORT},
    {40, 48, 40, 16, 52, 0x4, 0x9, 1, "TROCAR", FONT_SHORT},
    {80, 48, 40, 16, 52, 0x4, 0x9, 1, "BUSCAR", FONT_SHORT},
};

/* graphics/pokedex/interface.4bpp, the list's sprites: 32x16 ones (4 tiles
 * wide) and the 64x32 SEEN and OWN (8 tiles wide), on a clear background:
 * SEARCH in the narrow font, the rest in the small one. NATIONAL becomes
 * NACIONAL by its T (columns 9-12) redrawn as a C: its letters are kerned
 * by hand, and the font's are too wide to fit at that spacing. */
static void NationalToNacional(unsigned char *sprite)
{
    static const char *const sC[] = {".##", "#..", "#..", "#..", "#..", "#..", ".##"};
    int x, y;

    for (y = 4; y <= 11; y++)
        for (x = 9; x <= 12; x++)
            SetSheetPixel(sprite, x, y, 0);
    for (y = 0; y < 7; y++)
        for (x = 0; x < 3; x++)
            if (sC[y][x] == '#')
            {
                int dx, dy;
                for (dy = 0; dy <= 1; dy++)
                    for (dx = 0; dx <= 1; dx++)
                        if ((dx || dy) && SheetPixel(sprite, 9 + x + dx, 4 + y + dy) == 0)
                            SetSheetPixel(sprite, 9 + x + dx, 4 + y + dy, 0xF);
            }
    for (y = 0; y < 7; y++)
        for (x = 0; x < 3; x++)
            if (sC[y][x] == '#')
                SetSheetPixel(sprite, 9 + x, 4 + y, 0x1);
}

/* MENU, HOENN, SELECT and START stay. */
static const struct
{
    unsigned short tile;
    unsigned char tilesWide;
    struct GameLabel label;
} sPokedexListSprites[] = {
    {40, 4, {0, 0, 32, 16, 3, 0x1, 0xF, 0, "BUSCAR", FONT_NARROW}},
    {64, 8, {6, 6, 52, 8, 6, 0x1, 0xF, 1, "VISTOS", FONT_SMALL}},
    {96, 8, {5, 6, 53, 8, 6, 0x1, 0xF, 1, "CAPTURADOS", FONT_SMALL}},
};

/* graphics/pokenav/options/options.4bpp: each option a 128x16 strip of four
 * 32x16 sprites (32 tiles), in the normal font after the strip's square.
 * PARTY's PKMN glyph goes with it. */
static const struct GameLabel sPokenavOptionLabels[] = {
    {16, 2, 112, 14, 4, 0x1, 0x2, 0, "MAPA DE HOENN", 0},
    {16, 2, 112, 14, 4, 0x1, 0x2, 0, "CONDIÇÃO", 0},
    {16, 2, 112, 14, 4, 0x1, 0x2, 0, "CHAMADAS", 0},
    {16, 2, 112, 14, 4, 0x1, 0x2, 0, "FITAS", 0},
    {16, 2, 112, 14, 4, 0x1, 0x2, 0, "DESLIGAR", 0},
    {16, 0, 112, 16, 4, 0x1, 0x2, 0, "EQUIPE", 0},
    {16, 2, 112, 14, 4, 0x1, 0x2, 0, "BUSCAR", 0},
    {8, 2, 120, 14, 4, 0x1, 0x2, 0, "ESTILO", 0},
    {8, 2, 120, 14, 4, 0x1, 0x2, 0, "BELEZA", 0},
    {8, 2, 120, 14, 4, 0x1, 0x2, 0, "FOFURA", 0},
    {8, 2, 120, 14, 4, 0x1, 0x2, 0, "ESPERTEZA", 0},
    {8, 2, 120, 14, 4, 0x1, 0x2, 0, "FORÇA", 0},
    {16, 2, 112, 14, 4, 0x1, 0x2, 0, "VOLTAR", 0},
};

/* A row of sprites (each spriteW x spriteH tiles, one after another in the
 * sheet) to a grid of tiles side by side, and back. */
static void SpritesToGrid(unsigned char *grid, const unsigned char *sprites, int spriteW, int spriteH, int count,
                          int toGrid)
{
    int r, c;

    for (r = 0; r < spriteH; r++)
        for (c = 0; c < spriteW * count; c++)
        {
            unsigned char *g = grid + (r * spriteW * count + c) * 32;
            unsigned char *t = (unsigned char *)sprites
                             + ((c / spriteW) * spriteW * spriteH + r * spriteW + c % spriteW) * 32;
            if (toGrid)
                memcpy(g, t, 32);
            else
                memcpy(t, g, 32);
        }
}

/* graphics/pokenav/left_headers: the main menus' headers, two 64x32 sprites
 * (a 128x32 grid), and the submenus', four 32x16 ones (a 128x16 grid), both
 * centred in the normal font; HOENN MAP and its FULL VIEW and ZOOM VIEW are
 * three separate 64x32 sprites, 56 pixels wide (MAPA: MAPA DE HOENN does not
 * fit, and the menu's option already says it). */
static const struct
{
    const char *file;
    struct GameLabel label;
} sPokenavHeaders[] = {
    {"left_headers/main_menu.4bpp", {0, 5, 86, 13, 7, 0x1, 0x2, 1, "MENU PRINCIPAL", 0}},
    {"left_headers/condition.4bpp", {0, 5, 86, 13, 7, 0x1, 0x2, 1, "CONDIÇÃO", 0}},
    {"left_headers/match_call.4bpp", {0, 5, 86, 13, 7, 0x1, 0x2, 1, "CHAMADAS", 0}},
    {"left_headers/ribbons.4bpp", {0, 5, 86, 13, 7, 0x1, 0x2, 1, "FITAS", 0}},
    {"left_headers/beauty.4bpp", {0, 2, 62, 12, 4, 0x1, 0x2, 1, "BELEZA", 0}},
    {"left_headers/cool.4bpp", {0, 2, 62, 12, 4, 0x1, 0x2, 1, "ESTILO", 0}},
    {"left_headers/cute.4bpp", {0, 2, 62, 12, 4, 0x1, 0x2, 1, "FOFURA", 0}},
    {"left_headers/smart.4bpp", {0, 2, 62, 12, 4, 0x1, 0x2, 1, "ESPERTEZA", 0}},
    {"left_headers/tough.4bpp", {0, 2, 62, 12, 4, 0x1, 0x2, 1, "FORÇA", 0}},
    {"left_headers/party.4bpp", {0, 0, 62, 16, 4, 0x1, 0x2, 1, "EQUIPE", 0}},
    {"left_headers/search.4bpp", {0, 2, 62, 12, 4, 0x1, 0x2, 1, "BUSCAR", 0}},
};

/* graphics/pokenav/region_map/city_zoom_text.4bpp: the zoomed map's key,
 * five strips of 96x8 (12 tiles each) that three 32x8 sprites slide
 * through, each a coloured square and the name in the small font, one
 * colour and no shadow. */
static const struct GameLabel sCityZoomKey[] = {
    {14, 0, 82, 8, 1, 0x3, NO_SHADOW, 0, "CENTRO POKéMON", FONT_SMALL},
    {14, 0, 82, 8, 1, 0x5, NO_SHADOW, 0, "POKé MART", FONT_SMALL},
    {14, 0, 82, 8, 1, 0xF, NO_SHADOW, 0, "GINÁSIO POKéMON", FONT_SMALL},
    {14, 0, 82, 8, 1, 0x2, NO_SHADOW, 0, "TENDA DE BATALHA", FONT_SMALL},
    {14, 0, 82, 8, 1, 0x4, NO_SHADOW, 0, "CONCURSO POKéMON", FONT_SMALL},
};

/* graphics/pokenav/condition/cancel.4bpp (32x16, 4 tiles wide): CANCEL in
 * letters 5 pixels high that no font of the game matches, in colour F on 2
 * (x 4-22, rows 5-9). VOLTAR does not fit in 18 pixels: SAIR, drawn here. */
static void DrawConditionCancel(unsigned char *tiles)
{
    static const char *const sSair[] = {
        "##.###.#.##.", "#..#.#.#.#.#", "##.###.#.##.", ".#.#.#.#.#.#", "##.#.#.#.#.#",
    };
    int x, y;

    for (y = 5; y <= 9; y++)
        for (x = 4; x <= 22; x++)
            SetSheetPixel(tiles, x, y, 0x2);
    for (y = 0; y < 5; y++)
        for (x = 0; x < 12; x++)
            if (sSair[y][x] == '#')
                SetSheetPixel(tiles, 7 + x, 5 + y, 0xF);
}

static const struct GameLabel sHoennMapHeaders[] = {
    {1, 5, 54, 13, 7, 0x1, 0x2, 1, "MAPA", 0},
    {1, 5, 54, 13, 7, 0x1, 0x2, 1, "VISÃO GERAL", 0},
    {1, 5, 54, 13, 7, 0x1, 0x2, 1, "AMPLIADO", 0},
};

/*
 * The title screen's logo (graphics/title_screen/emerald_version.8bpp, two
 * 64x32 sprites of 8bpp tiles, one byte a pixel), whole: one hex digit a
 * pixel, an index into the English logo's palette (0 is clear). ESMERALDA is
 * the Spanish game's EDICIÓN ESMERALDA logo's, as it draws it; VERSÃO stands
 * where EDICIÓN does, in its style (E and O are EDICIÓN's, R and A
 * ESMERALDA's at its size, V and S the English logo's), with a tilde in its
 * accent's place. Unlike the rest of this file, these are a game's pixels.
 * tools/logo_ptbr.py writes this table into a PNG, as a preview.
 */
#define LOGO_W 128
#define LOGO_H 32

static const char sLogo[LOGO_H][LOGO_W + 1] = {
    "00000000000000000000000000000000000000000000000000000000000000444444400000000000000000000000000000000000000000000000000000000000",
    "000000000000000000000000000000000000000000000000000000000000044ff44f400000000000000000000000000000000000000000000000000000000000",
    "00000000000000000000000000000000000000000000000000000000000004f44ff4400000000000000000000000000000000000000000000000000000000000",
    "00000000000000000000000000000074444444444444444444444444444444444444444444444000000000000000000000000000000000000000000000000000",
    "0000000000000000000000000000007d74443d444dd44433333d4444daaa4444a1d44443ff344400000000000000000000000000000000044400000000000000",
    "000000000000000000000000000000743a4df343fffa44ffffffd441f3334444fff4443ff3ff4400000000000000000000000000000000000044040000000000",
    "00000000000000000000000000000004af4dfa4ff31444ff3d3fa44ffa44444afff144f344af1400000000000000000000000000000000000000400000000000",
    "000000000000000000000000000000041fa1f443ff3a44fff3f3d443ff33444ffdf344f3441f1400000000000000000000000000000000000000000000000000",
    "0000000000000000000000000000000473ff344aff31443ffff14444d1ff34af31f3443f3aff4400000000000000000000000000000000000000000000000000",
    "0000000000000000000000000000000074ff3441ff1a443f31ffd44a11ffa4ffffffd44ffff14400000000000000000000000000000000000000000000000000",
    "0000000000000000000000000000000074afa441ffffd433a4a334d3fffa44ffd43fa444dd444400000000000000000000000000000000000000000000000000",
    "0000000000000000000000000000000004441d4444444444444444444444444444444444444dd400000000000000000000000000000000000000000000000000",
    "000000000000000000000000000000004444444e4eedd1aae4a33333a1440004d1dde40044444440000000000000000000000000000000000000000000000000",
    "000000000000000000000000044444444ed13deeefffffffd4ffffffff3e404efffffe404eadde40004444000000000000000000000000000000000000000000",
    "00000000000000000000000047edd1e443fff344efffff3fd4ffff3ffff1404afffff1404dfff3400777e7444000000000000000000000000000000000000000",
    "0000000000000000044444474effffa44fffffe4effffdee443fff4dfffa44effffff34441fff1400eaffa1d7440000000000000000000000000000000000000",
    "000000000000000444d1a3144efffffeefffff144ffff3ffd4ffffa3fff144afff1fffee4afffd400effffff3d44000000000000000000000000000000000000",
    "0000000000444444eaffff344efffff1dffffff443ffffffd43fffffff144efffaefff1443fffd4047ffffffffa4400044444000000000000000000000000000",
    "000000044447deee3fff33144effffffaff3fffd43fffadde43ffffff3e441fffadfffa44ffffe4041fff3affff1400471de4440000000000000000000000000",
    "00000444ed3ff14dfff1eee44efffafffffdfffa4afffa11143fff1fff34effffffffffeefff34744afff14dfff344441fffad44000000000000000000000000",
    "00004edafffffa41fffffffa4efff1affff4afffeafffffffe3fffeafffaaffffffffffdeffff1dee3fffe44ffff47e1fffffa44000000000000000000000000",
    "0000443ffffade4dffffffffaefff1efff341fffd1ffffff3ea3334e33a333ffd441fffa1fffffffdffff44dfff344dffffff344000000000000000000000000",
    "0000441fff3d1fe413f33fff3efff14aff14ef3adedde777774447777744444e4444d1a1affffff31ffff33ffffa4effffaff344000000000000000000000000",
    "000004efffffff144ee4dfffaefffa4e3ae44444444444444444444444444444400444444eed1a3aaffffffffffe43fff11ff374000000000000000000000000",
    "00000443fffffae4ef33ffffee3ade44444444444400000000000000000000000000000444444744ed1afffff3e43ffffe1fff74000000000000000000000000",
    "0000004dffffeea1efffff3e4444444444000000000000000000000000000000000000000000044444444edde44affffffffff74000000000000000000000000",
    "0000004effffffffd33a1d444440000000000000000000000000000000000000000000000000000000044444e4eafff33fffffe4000000000000000000000000",
    "00000044affffff3e4444444000000000000000000000000000000000000000000000000000000000000000044444d144d3fffe4000000000000000000000000",
    "00000004dfff31e444440000000000000000000000000000000000000000000000000000000000000000000000044444741fffd4000000000000000000000000",
    "00000004e31e444400000000000000000000000000000000000000000000000000000000000000000000000000000000444e13d4000000000000000000000000",
    "00000004444440000000000000000000000000000000000000000000000000000000000000000000000000000000000000444444000000000000000000000000",
    "00000000774000000000000000000000000000000000000000000000000000000000000000000000000000000000000000004470000000000000000000000000",
};

static void DrawLogo(unsigned char *data)
{
    int x, y;

    for (y = 0; y < LOGO_H; y++)
        for (x = 0; x < LOGO_W; x++)
        {
            char c = sLogo[y][x];
            data[((x / 64) * 32 + (y / 8) * 8 + (x % 64) / 8) * 64 + (y % 8) * 8 + x % 8]
                = (unsigned char)(c <= '9' ? c - '0' : c - 'a' + 10);
        }
}

/*
 * The contest results' title (graphics/contest/results_screen/tiles.4bpp,
 * 16 tiles to a row): the rank and the category, each 40x16, in a tall
 * narrow font of 2-pixel strokes that no font of the game matches, drawn
 * here: ink 1, its shadow 9 to the right and below, on 8. The four ranks
 * share the tiles of RANK, after the rank's own: those become NÍVEL and the
 * rank tilemaps put them first (TransformContestRankTilemap). 'i' stands
 * for Í and 'c' for Ç.
 */
static const struct
{
    char ch;
    const char *rows[11];
} sTitleGlyphs[] = {
    {'A', {".###.", "##.##", "##.##", "##.##", "##.##", "#####", "#####", "##.##", "##.##", "##.##", "##.##"}},
    {'B', {"####.", "##.##", "##.##", "##.##", "####.", "####.", "##.##", "##.##", "##.##", "##.##", "####."}},
    {'C', {".####", "##...", "##...", "##...", "##...", "##...", "##...", "##...", "##...", "##...", ".####"}},
    {'E', {"#####", "##...", "##...", "##...", "####.", "####.", "##...", "##...", "##...", "##...", "#####"}},
    {'F', {"#####", "##...", "##...", "##...", "####.", "####.", "##...", "##...", "##...", "##...", "##..."}},
    {'H', {"##.##", "##.##", "##.##", "##.##", "##.##", "#####", "#####", "##.##", "##.##", "##.##", "##.##"}},
    {'I', {"##", "##", "##", "##", "##", "##", "##", "##", "##", "##", "##"}},
    {'L', {"##..", "##..", "##..", "##..", "##..", "##..", "##..", "##..", "##..", "##..", "####"}},
    {'M', {"##...##", "###.###", "#######", "##.#.##", "##.#.##", "##.#.##", "##...##", "##...##", "##...##", "##...##", "##...##"}},
    {'N', {"##..##", "###.##", "###.##", "######", "######", "##.###", "##.###", "##..##", "##..##", "##..##", "##..##"}},
    {'O', {".###.", "##.##", "##.##", "##.##", "##.##", "##.##", "##.##", "##.##", "##.##", "##.##", ".###."}},
    {'P', {"####.", "##.##", "##.##", "##.##", "##.##", "####.", "##...", "##...", "##...", "##...", "##..."}},
    {'R', {"####.", "##.##", "##.##", "##.##", "####.", "####.", "##.##", "##.##", "##.##", "##.##", "##.##"}},
    {'S', {".####", "##...", "##...", "##...", ".###.", "...##", "...##", "...##", "...##", "...##", "####."}},
    {'T', {"######", "..##..", "..##..", "..##..", "..##..", "..##..", "..##..", "..##..", "..##..", "..##..", "..##.."}},
    {'U', {"##.##", "##.##", "##.##", "##.##", "##.##", "##.##", "##.##", "##.##", "##.##", "##.##", ".###."}},
    {'V', {"##.##", "##.##", "##.##", "##.##", "##.##", "##.##", "##.##", "##.##", ".###.", ".###.", "..#.."}},
    {'Z', {"#####", "...##", "...##", "..##.", "..##.", ".##..", ".##..", "##...", "##...", "##...", "#####"}},
    {'.', {"..", "..", "..", "..", "..", "..", "..", "..", "..", "##", "##"}},
};

static const char *TitleGlyph(char ch, int row, int *width)
{
    char base = ch == 'i' ? 'I' : ch == 'c' ? 'C' : ch;
    unsigned i;

    for (i = 0; i < sizeof(sTitleGlyphs) / sizeof(sTitleGlyphs[0]); i++)
        if (sTitleGlyphs[i].ch == base)
        {
            *width = (int)strlen(sTitleGlyphs[i].rows[0]);
            return sTitleGlyphs[i].rows[row];
        }
    *width = 0;
    return NULL;
}

static void DrawContestTitle(unsigned char *tiles, int x0, int y0, const char *text)
{
    unsigned char ink[16][40];
    int width = -1, x, y, w;
    const char *p;

    for (p = text; *p; p++)
    {
        TitleGlyph(*p, 0, &w);
        width += w + 1;
    }
    memset(ink, 0, sizeof(ink));
    x = (40 - width) / 2;
    for (p = text; *p; p++)
    {
        int r, c;
        for (r = 0; r < 11; r++)
        {
            const char *row = TitleGlyph(*p, r, &w);
            for (c = 0; row != NULL && c < w; c++)
                if (row[c] == '#' && x + c >= 0 && x + c < 40)
                    ink[r + 2][x + c] = 1;
        }
        if (*p == 'i') /* the acute, two rows above */
        {
            if (x + 2 < 40) ink[0][x + 2] = 1;
            if (x + 1 < 40) ink[1][x + 1] = 1;
        }
        if (*p == 'c' && x + 2 < 40) /* the cedilla, below */
            ink[13][x + 2] = 1;
        x += w + 1;
    }
    for (y = 0; y < 16; y++)
        for (x = 0; x < 40; x++)
            SetSheetPixel(tiles, x0 + x, y0 + y, ink[y][x] ? 0x1 : 0x8);
    for (y = 0; y < 16; y++)
        for (x = 0; x < 40; x++)
        {
            int dx, dy;
            if (!ink[y][x])
                continue;
            for (dy = 0; dy <= 1; dy++)
                for (dx = 0; dx <= 1; dx++)
                    if ((dx || dy) && x + dx < 40 && y + dy < 16 && SheetPixel(tiles, x0 + x + dx, y0 + y + dy) == 0x8)
                        SetSheetPixel(tiles, x0 + x + dx, y0 + y + dy, 0x9);
        }
}

static const struct
{
    unsigned char x, y;
    const char *text;
} sContestTitles[] = {
    {88, 48, "NiVEL"},  /* RANK, tiles 107-111 */
    {0, 16, "NORMAL"},  /* tiles 32-36 */
    {40, 16, "SUPER"},  /* 37-41 */
    {80, 16, "HIPER"},  /* 42-46 */
    {80, 0, "MESTRE"},  /* 10-14 */
    {0, 96, "ESTILO"},  /* COOL, 192-196 */
    {40, 96, "BELEZA"}, /* 197-201 */
    {80, 96, "FOFURA"}, /* 202-206 */
    {0, 112, "ESPERT."}, /* SMART, 224-228 */
    {40, 112, "FORcA"}, /* TOUGH, 229-233 */
};

/* A rank's title tilemap, 10x2: its own five tiles, then RANK's. */
static void TransformContestRankTilemap(unsigned char *map)
{
    int r, c;

    for (r = 0; r < 2; r++)
        for (c = 0; c < 5; c++)
        {
            unsigned char *a = map + (r * 10 + c) * 2, *b = map + (r * 10 + c + 5) * 2, t0 = a[0], t1 = a[1];
            a[0] = b[0];
            a[1] = b[1];
            b[0] = t0;
            b[1] = t1;
        }
}

static int EndsWith(const char *path, const char *suffix)
{
    size_t n = strlen(path), m = strlen(suffix);
    return n >= m && strcmp(path + n - m, suffix) == 0;
}

void Port_TransformAsset(const char *path, unsigned char *data, unsigned size)
{
    unsigned i;

    if (EndsWith(path, "interface/menu_info.4bpp") && size >= 128 * 128 / 2)
    {
        for (i = 0; i < sizeof(sTypeNames) / sizeof(sTypeNames[0]); i++)
            DrawTypeName(data, sTypeNames[i].tile, sTypeNames[i].name);
        for (i = 0; i < sizeof(sLabels) / sizeof(sLabels[0]); i++)
            DrawLabel(data, sLabels[i].tile, sLabels[i].name);
    }
    else if (EndsWith(path, "summary_screen/tiles.4bpp") && size >= 128 * 120 / 2)
    {
        for (i = 0; i < sizeof(sSummaryTitles) / sizeof(sSummaryTitles[0]); i++)
            DrawSummaryTitle(data, sSummaryTitles[i].tile, sSummaryTitles[i].tiles * 8, sSummaryTitles[i].name);
    }
    else if (EndsWith(path, "title_screen/press_start.4bpp") && size >= 21 * 32)
    {
        DrawPressStart(data);
    }
    else if (EndsWith(path, "title_screen/emerald_version.8bpp") && size >= LOGO_W * LOGO_H)
    {
        DrawLogo(data);
    }
    else if (EndsWith(path, "contest/results_screen/tiles.4bpp") && size >= 256 * 32)
    {
        for (i = 0; i < sizeof(sContestTitles) / sizeof(sContestTitles[0]); i++)
            DrawContestTitle(data, sContestTitles[i].x, sContestTitles[i].y, sContestTitles[i].text);
    }
    else if ((EndsWith(path, "results_screen/title_normal.bin") || EndsWith(path, "results_screen/title_super.bin")
           || EndsWith(path, "results_screen/title_hyper.bin") || EndsWith(path, "results_screen/title_master.bin"))
          && size >= 10 * 2 * 2)
    {
        TransformContestRankTilemap(data);
    }
    else if (EndsWith(path, "pokedex/menu.4bpp") && size >= 128 * 128 / 2)
    {
        for (i = 0; i < sizeof(sPokedexMenuLabels) / sizeof(sPokedexMenuLabels[0]); i++)
            DrawGameLabel(data, &sPokedexMenuLabels[i]);
    }
    else if (EndsWith(path, "pokedex/search_menu.4bpp") && size >= 128 * 64 / 2)
    {
        for (i = 0; i < sizeof(sPokedexSearchLabels) / sizeof(sPokedexSearchLabels[0]); i++)
            DrawGameLabel(data, &sPokedexSearchLabels[i]);
    }
    else if (EndsWith(path, "pokedex/interface.4bpp") && size >= 200 * 32)
    {
        for (i = 0; i < sizeof(sPokedexListSprites) / sizeof(sPokedexListSprites[0]); i++)
        {
            sTilesWide = sPokedexListSprites[i].tilesWide;
            DrawGameLabel(data + sPokedexListSprites[i].tile * 32, &sPokedexListSprites[i].label);
        }
        sTilesWide = 4;
        NationalToNacional(data + 168 * 32);
        sTilesWide = SHEET_TILES_WIDE;
    }
    else if (EndsWith(path, "pokenav/options/options.4bpp")
          && size >= sizeof(sPokenavOptionLabels) / sizeof(sPokenavOptionLabels[0]) * 32 * 32)
    {
        unsigned char grid[32 * 32];
        for (i = 0; i < sizeof(sPokenavOptionLabels) / sizeof(sPokenavOptionLabels[0]); i++)
        {
            SpritesToGrid(grid, data + i * 32 * 32, 4, 2, 4, 1);
            DrawGameLabel(grid, &sPokenavOptionLabels[i]);
            SpritesToGrid(grid, data + i * 32 * 32, 4, 2, 4, 0);
        }
    }
    else if (EndsWith(path, "pokenav/condition/cancel.4bpp") && size >= 8 * 32)
    {
        sTilesWide = 4;
        DrawConditionCancel(data);
        sTilesWide = SHEET_TILES_WIDE;
    }
    else if (EndsWith(path, "region_map/city_zoom_text.4bpp") && size >= 5 * 12 * 32)
    {
        sTilesWide = 12;
        for (i = 0; i < 5; i++)
            DrawGameLabel(data + i * 12 * 32, &sCityZoomKey[i]);
        sTilesWide = SHEET_TILES_WIDE;
    }
    else if (EndsWith(path, "left_headers/hoenn_map.4bpp") && size >= 3 * 32 * 32)
    {
        sTilesWide = 8;
        for (i = 0; i < 3; i++)
            DrawGameLabel(data + i * 32 * 32, &sHoennMapHeaders[i]);
        sTilesWide = SHEET_TILES_WIDE;
    }
    else if (strstr(path, "pokenav/left_headers/") != NULL)
    {
        unsigned char grid[64 * 32];
        for (i = 0; i < sizeof(sPokenavHeaders) / sizeof(sPokenavHeaders[0]); i++)
        {
            int main = sPokenavHeaders[i].label.capitals == 7;
            if (!EndsWith(path, sPokenavHeaders[i].file) || size < (main ? 64u : 32u) * 32)
                continue;
            /* Two 64x32 sprites, or four 32x16 ones: either way a grid 16 tiles wide. */
            SpritesToGrid(grid, data, main ? 8 : 4, main ? 4 : 2, main ? 2 : 4, 1);
            DrawGameLabel(grid, &sPokenavHeaders[i].label);
            SpritesToGrid(grid, data, main ? 8 : 4, main ? 4 : 2, main ? 2 : 4, 0);
        }
    }
    else if (EndsWith(path, "interface/status_icons.4bpp") && size >= 7 * 4 * 32)
    {
        sTilesWide = 4;
        for (i = 0; i < sizeof(sStatusNames) / sizeof(sStatusNames[0]); i++)
            if (sStatusNames[i] != NULL)
                DrawStatusIcon(data, (int)i * 8, sStatusNames[i]);
        sTilesWide = SHEET_TILES_WIDE;
    }
    else if (EndsWith(path, "types/move_types.4bpp") && size >= sizeof(sSummaryTypeNames) / sizeof(sSummaryTypeNames[0]) * 256)
    {
        sTilesWide = 4;
        for (i = 0; i < sizeof(sSummaryTypeNames) / sizeof(sSummaryTypeNames[0]); i++)
            if (sSummaryTypeNames[i] != NULL)
                /* Rows 3-12 cleared, the letter on rows 4-10. */
                DrawTypeNameAt(data + i * 256, 0, 0, 3, 13, 4, sSummaryTypeNames[i]);
        sTilesWide = SHEET_TILES_WIDE;
    }
}

#else

void Port_TransformAsset(const char *path, unsigned char *data, unsigned size)
{
    (void)path;
    (void)data;
    (void)size;
}

#endif
