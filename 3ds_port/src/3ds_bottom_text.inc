/* Part of 3ds_bottom_ui.c: the game's fonts, strings, and the smooth labels made a slice at a time. */

/* ------------------------------------------------------------------------ */
/* Text                                                                     */
/* ------------------------------------------------------------------------ */

typedef struct
{
    const u16 *glyphs;
    const u8 *widths;
    u8 height, lineHeight;
    bool8 japanese;
} Font;

/* The game's two fonts, and their narrow cuts for long translations. */
static Font sSmall, sNormal, sSmallNarrow, sNarrow;

/* The 2bpp bits of one 8-pixel half of a glyph row. A Latin glyph is four
 * tiles (top-left, top-right, bottom-left, bottom-right); a Japanese one is a
 * single 8-pixel column of a sheet of 16 glyphs a row, its lower tile 0x80
 * further on (text.c's DecompressGlyph_*). */
static inline u16 GlyphBits(const Font *font, u16 glyph, int row, int half)
{
    if (font->japanese)
        return half ? 0 : font->glyphs[0x100 * (glyph >> 4) + 8 * (glyph & 0xF) + (row >= 8 ? 0x80 : 0) + (row & 7)];
    return font->glyphs[glyph * 0x20 + (row >= 8 ? 0x10 : 0) + half * 8 + (row & 7)];
}

/* Japanese glyphs are all 8 pixels wide, as the game draws them. */
static inline int GlyphWidth(const Font *font, u16 glyph)
{
    return font->japanese ? 8 : font->widths[glyph];
}

static const u16 *ResolveFontBase(const u16 *glyphs)
{
    const void *resolved = Port_ResolveFontPointer(glyphs);

    if (resolved == glyphs)
        resolved = Port_ResolveAssetPointer(glyphs);
    return resolved;
}

/* The glyph payloads live in the asset cache; resolve them per redraw. */
static void ResolveFonts(void)
{
#if GAME_LANGUAGE == LANGUAGE_JAPANESE
    /* The Japanese game's strings are in the Japanese glyph set. */
    sSmall.glyphs = ResolveFontBase(gFontSmallJapaneseGlyphs);
    sSmall.height = 12;
    sSmall.lineHeight = 13;
    sSmall.japanese = TRUE;
    sNormal.glyphs = ResolveFontBase(gFontNormalJapaneseGlyphs);
    sNormal.height = 16;
    sNormal.lineHeight = 16;
    sNormal.japanese = TRUE;
    /* No narrow fonts there: its glyphs are all 8 pixels wide. */
    sSmallNarrow = sSmall;
    sNarrow = sNormal;
#else
    sSmall.glyphs = ResolveFontBase(gFontSmallLatinGlyphs);
    sSmall.widths = gFontSmallLatinGlyphWidths;
    sSmall.height = 13;
    sSmall.lineHeight = 13;
    sNormal.glyphs = ResolveFontBase(gFontNormalLatinGlyphs);
    sNormal.widths = gFontNormalLatinGlyphWidths;
    sNormal.height = 16;
    sNormal.lineHeight = 16;
    sSmallNarrow.glyphs = ResolveFontBase(gFontSmallNarrowLatinGlyphs);
    sSmallNarrow.widths = gFontSmallNarrowLatinGlyphWidths;
    sSmallNarrow.height = 13;
    sSmallNarrow.lineHeight = 13;
    sNarrow.glyphs = ResolveFontBase(gFontNarrowLatinGlyphs);
    sNarrow.widths = gFontNarrowLatinGlyphWidths;
    sNarrow.height = 16;
    sNarrow.lineHeight = 16;
#endif
}

/* Walks a game string: returns the next glyph, 0xFFFE for a line break or
 * 0xFFFF at the end. */
static u16 NextGlyph(const u8 **str)
{
    for (;;)
    {
        u8 c = *(*str)++;

        switch (c)
        {
        case EOS:
            --*str;
            return 0xFFFF;
        case CHAR_NEWLINE:
        case CHAR_PROMPT_SCROLL:
        case CHAR_PROMPT_CLEAR:
            return 0xFFFE;
        case EXT_CTRL_CODE_BEGIN:
            if (**str == EOS)
                return 0xFFFF;
            *str += GetExtCtrlCodeLength(**str);
            break;
        case PLACEHOLDER_BEGIN:
        case CHAR_DYNAMIC:
        case CHAR_KEYPAD_ICON:
            if (**str != EOS)
                ++*str;
            break;
        case CHAR_EXTRA_SYMBOL:
            if (**str == EOS)
                return 0xFFFF;
            return 0x100 | *(*str)++;
        default:
            return c;
        }
    }
}

static int StrWidth(const Font *font, const u8 *str)
{
    int width = 0, best = 0;

    if (!str)
        return 0;
    for (u16 g; (g = NextGlyph(&str)) != 0xFFFF;)
    {
        if (g == 0xFFFE)
        {
            if (width > best) best = width;
            width = 0;
            continue;
        }
        width += GlyphWidth(font, g);
    }
    return width > best ? width : best;
}

/*
 * Where a line's ink starts and ends, from its left edge as drawn: the
 * glyphs' advances leave room after each (a "1" is drawn at the left of its
 * cell), so a line centred by its advance sits left of the middle. FALSE for
 * a line with no ink.
 */
static bool8 StrInk(const Font *font, const u8 *str, int *left, int *right)
{
    int x = 0;

    *left = 0x7FFF;
    *right = -1;
    if (!font->glyphs || !str)
        return FALSE;
    for (u16 g; (g = NextGlyph(&str)) != 0xFFFF;)
    {
        int width = GlyphWidth(font, g);

        if (g == 0xFFFE)
            break;
        for (int row = 0; row < font->height; ++row)
            for (int half = 0; half < 2 && half * 8 < width && half * 8 < 16; ++half)
            {
                u16 bits = GlyphBits(font, g, row, half);

                for (int k = 0; bits && k < 8 && half * 8 + k < width; ++k)
                {
                    u8 byte = k < 4 ? bits >> 8 : bits & 0xFF;
                    u8 v = (byte >> (6 - 2 * (k & 3))) & 3;

                    if (v != 1 && v != 2)
                        continue;
                    if (x + half * 8 + k < *left) *left = x + half * 8 + k;
                    if (x + half * 8 + k > *right) *right = x + half * 8 + k;
                }
            }
        x += width;
    }
    return *right >= *left;
}

static int DrawGlyph(const Font *font, u16 glyph, int x, int y, u16 fg, u16 shadow)
{
    int width = GlyphWidth(font, glyph);
    int sx = x + sOX;
    bool8 inside = sx >= sClipX0 && y >= sClipY0 && sx + width <= sClipX1 && y + font->height <= sClipY1;
    u16 *origin = sDst + (inside ? sx * H + (H - 1 - y) : 0);

    /* Wholly outside the clip: nothing to write. */
    if (sx >= sClipX1 || y >= sClipY1 || sx + width <= sClipX0 || y + font->height <= sClipY0)
        return GlyphWidth(font, glyph);
    if (width > 16)
        width = 16;
    for (int row = 0; row < font->height; ++row)
    {
        for (int half = 0; half < 2 && half * 8 < width; ++half)
        {
            u16 bits = GlyphBits(font, glyph, row, half);

            if (!bits)
                continue;
            for (int k = 0; k < 8 && half * 8 + k < width; ++k)
            {
                u8 byte = k < 4 ? bits >> 8 : bits & 0xFF;
                u8 v = (byte >> (6 - 2 * (k & 3))) & 3;
                int px = half * 8 + k;

                if (v != 1 && v != 2)
                    continue;
                if (inside)
                    origin[px * H - row] = v == 1 ? fg : shadow;
                else
                    Put(x + px, y + row, v == 1 ? fg : shadow);
            }
        }
    }
    return GlyphWidth(font, glyph);
}

/* Draws a game string; returns the x where it ended. */
static int DrawStr(const Font *font, const u8 *str, int x, int y, u16 fg, u16 shadow)
{
    int left = x;

    if (!font->glyphs || !str)
        return x;
    for (u16 g; (g = NextGlyph(&str)) != 0xFFFF;)
    {
        if (g == 0xFFFE)
        {
            x = left;
            y += font->lineHeight;
            continue;
        }
        x += DrawGlyph(font, g, x, y, fg, shadow);
    }
    return x;
}

static void DrawStrRight(const Font *font, const u8 *str, int right, int y, u16 fg, u16 shadow)
{
    DrawStr(font, str, right - StrWidth(font, str), y, fg, shadow);
}

/* Whether a glyph paints (glyph or shadow) the pixel at column px of row. */
static bool8 GlyphInk(const Font *font, u16 glyph, int px, int row)
{
    u16 bits = GlyphBits(font, glyph, row, px / 8);
    u8 byte = (px & 7) < 4 ? bits >> 8 : bits & 0xFF;
    u8 v = (byte >> (6 - 2 * (px & 3))) & 3;

    return v == 1 || v == 2;
}

/* The columns a one-line string really paints, [*x0, *x1) from its origin;
 * *x1 <= *x0 when it paints none. */
static void InkColumns(const Font *font, const u8 *str, int *x0, int *x1)
{
    int x = 0;

    *x0 = 0x7FFF;
    *x1 = 0;
    for (u16 g; (g = NextGlyph(&str)) != 0xFFFF && g != 0xFFFE;)
    {
        int width = GlyphWidth(font, g) > 16 ? 16 : GlyphWidth(font, g);

        for (int px = 0; px < width; ++px)
            for (int row = 0; row < font->height; ++row)
                if (GlyphInk(font, g, px, row))
                {
                    if (x + px < *x0) *x0 = x + px;
                    if (x + px + 1 > *x1) *x1 = x + px + 1;
                    break;
                }
        x += GlyphWidth(font, g);
    }
}

/* The rows a capital paints, [*top, *bottom): every label is centred on the
 * same band, so one with an accent (POKéMON) keeps the others' baseline. */
static void CapRows(const Font *font, int *top, int *bottom)
{
    *top = font->height;
    *bottom = 0;
    for (int row = 0; row < font->height; ++row)
        for (int px = 0; px < GlyphWidth(font, CHAR_H); ++px)
            if (GlyphInk(font, CHAR_H, px, row))
            {
                if (row < *top) *top = row;
                if (row + 1 > *bottom) *bottom = row + 1;
                break;
            }
}

/* Labels for the few words the game has no standalone string for: Latin, and
 * kana for the Japanese game (3ds_locale_text.h). */
static const u8 *Ascii(const char *text)
{
    static u8 ring[8][40];
    static u8 next;
    u8 *out = ring[next++ & 7];
    CtrLocale_ToGame(out, sizeof(ring[0]), text);
    return out;
}

static const u8 *Number(u32 value, int digits, enum StringConvertMode mode)
{
    static u8 ring[8][12];
    static u8 next;
    u8 *out = ring[next++ & 7];

    ConvertIntToDecimalStringN(out, value, mode, digits);
    return out;
}

/*
 * The battle plates' labels: the game's glyphs, enlarged by 1, 1.5, 2 or
 * 2.5 (k = 4, 6, 8, 10 quarter pixels a glyph pixel), with a smooth round
 * outline and, for some, a soft drop shadow, as gen_battle_art.py's mockups
 * draw them. Built at 4x - glyphs, a blurred-and-hardened outline merged with
 * a dilation, the outline moved down and blurred for the shadow - then
 * averaged down to screen pixels. A label is built once and kept.
 */
#define SMOOTH_PAD 4    /* glyph pixels around the text, for outline and shadow */
#define SMOOTH_SLOTS 28

typedef struct
{
    u32 age;
    u8 key[40], font, k;
    Rgb fg, outline, shadow;
    bool8 hasShadow;
    u16 w, h;
    u8 *px;            /* per pixel: coverage, then premultiplied r, g, b */
} SmoothText;

static SmoothText sSmooth[SMOOTH_SLOTS];
static u32 sSmoothClock;
/* How many labels or plates a redraw may still build (0xFF: any). The warm
 * up while the turn plays out builds one a frame (WarmBattleMenus). */
static u8 sBuildBudget = 0xFF;
static bool8 sCompBad;   /* a plate picture being made hit something not built yet */

static bool8 TakeBuildBudget(void)
{
    if (sBuildBudget == 0)
    {
        sCompBad = TRUE;
        return FALSE;
    }
    if (sBuildBudget != 0xFF)
        --sBuildBudget;
    return TRUE;
}

/*
 * Three box blurs in a row approximate a Gaussian. Each sweep is its own
 * step so a label can be built over several frames (SmoothJob). Neither
 * sweep divides: this CPU has no divide instruction, and a division per
 * pixel per sweep was most of what a label cost. The mean is a multiply by
 * the span's reciprocal (rounded up, so a full window stays 255).
 */
static void BlurRows(const u8 *src, u8 *dst, int w, int h, int radius, int y0, int y1)
{
    int span = 2 * radius + 1;

    (void)h;
    u32 inv = (65536u + span - 1) / span;

    for (int y = y0; y < y1; ++y)
    {
        const u8 *row = src + y * w;
        u8 *out = dst + y * w;
        int sum = 0, x = 0;

        for (int i = -radius; i <= radius; ++i)
            sum += row[i < 0 ? 0 : (i >= w ? w - 1 : i)];
        /* The left edge and the right edge clamp; the middle needs no test. */
        for (; x < w && x < radius; ++x)
        {
            int add = x + radius + 1;

            out[x] = (u8)(((u32)sum * inv) >> 16);
            sum += row[add >= w ? w - 1 : add] - row[0];
        }
        for (; x < w && x + radius + 1 < w; ++x)
        {
            out[x] = (u8)(((u32)sum * inv) >> 16);
            sum += row[x + radius + 1] - row[x - radius];
        }
        for (; x < w; ++x)
        {
            int sub = x - radius;

            out[x] = (u8)(((u32)sum * inv) >> 16);
            sum += row[w - 1] - row[sub < 0 ? 0 : sub];
        }
    }
}

/* Down the columns, a row at a time: the sums of every column are kept in
 * `sums`, so memory is read in order instead of a column per stride. */
static void BlurColumns(const u8 *src, u8 *dst, int w, int h, int radius, int *sums, int x0, int x1)
{
    int span = 2 * radius + 1;
    u32 inv = (65536u + span - 1) / span;

    memset(sums + x0, 0, (size_t)(x1 - x0) * sizeof(int));
    for (int i = -radius; i <= radius; ++i)
    {
        const u8 *row = src + (i < 0 ? 0 : (i >= h ? h - 1 : i)) * w;

        for (int x = x0; x < x1; ++x)
            sums[x] += row[x];
    }
    for (int y = 0; y < h; ++y)
    {
        u8 *out = dst + y * w;
        int add = y + radius + 1, sub = y - radius;
        const u8 *addRow = src + (add >= h ? h - 1 : add) * w, *subRow = src + (sub < 0 ? 0 : sub) * w;

        for (int x = x0; x < x1; ++x)
        {
            out[x] = (u8)(((u32)sums[x] * inv) >> 16);
            sums[x] += addRow[x] - subRow[x];
        }
    }
}

static int GaussRadius(float sigma)
{
    return (int)((sqrtf(4.0f * sigma * sigma + 1.0f) - 1.0f) / 2.0f + 0.5f);
}

/* A square dilation of a 0/255 mask, separable: a running count of the set
 * pixels in the window, so its cost does not grow with the radius. Rows of
 * the first sweep and columns of the second are independent: a band at a time. */
static void DilateRows(const u8 *src, u8 *tmp, int w, int radius, int y0, int y1)
{
    for (int y = y0; y < y1; ++y)
    {
        const u8 *row = src + y * w;
        int count = 0;

        for (int x = 0; x < radius && x < w; ++x)
            count += row[x] != 0;
        for (int x = 0; x < w; ++x)
        {
            if (x + radius < w)
                count += row[x + radius] != 0;
            if (x - radius - 1 >= 0)
                count -= row[x - radius - 1] != 0;
            tmp[y * w + x] = count ? 255 : 0;
        }
    }
}

static void DilateColumns(const u8 *tmp, u8 *dst, int w, int h, int radius, int x0, int x1)
{
    for (int x = x0; x < x1; ++x)
    {
        int count = 0;

        for (int y = 0; y < radius && y < h; ++y)
            count += tmp[y * w + x] != 0;
        for (int y = 0; y < h; ++y)
        {
            if (y + radius < h)
                count += tmp[(y + radius) * w + x] != 0;
            if (y - radius - 1 >= 0)
                count -= tmp[(y - radius - 1) * w + x] != 0;
            dst[y * w + x] = count ? 255 : 0;
        }
    }
}

/*
 * A label's build, in steps. In the frames the battle's turn plays out the
 * next menus' labels are made ready (WarmBattleMenus) and a big one takes
 * tens of milliseconds at once: so it runs a step or several at a time up
 * to sSliceMs milliseconds after sSliceStart (0: no limit) and the rest
 * comes next frame. The job keeps its buffers between frames.
 */
enum
{
    SJ_GLYPHS,
    SJ_RING_BLUR,       /* six sweeps: three box blurs, rows then columns each */
    SJ_RING_HARDEN = SJ_RING_BLUR + 6,
    SJ_DILATE_ROWS,     /* the sweeps of a blur or a dilation are SJ_PARTS steps each */
    SJ_DILATE_COLUMNS,
    SJ_RING_MERGE,
    SJ_SHADOW_SHIFT,
    SJ_SHADOW_BLUR,     /* six sweeps */
    SJ_SHADOW_FADE = SJ_SHADOW_BLUR + 6,
    SJ_DOWN,            /* screen pixels, a band of rows a step */
    SJ_DONE,
};

/* Steps of a few tenths of a millisecond on an Old 3DS at the largest k
 * (a label of ~100k quarter pixels): a band of 24 rows down, or a whole
 * image's pass, was 1-3 ms in one step, past any slice. */
#define SJ_BAND 4
#define SJ_PARTS 8
#define SJ_GLYPHS_A_STEP 3

typedef struct
{
    SmoothText *t;
    const Font *font;
    u8 stage, k, part;
    u16 row;
    u16 strAt;          /* SJ_GLYPHS: the next glyph's byte in the key (0: the first step) */
    int glyphX;         /* ... and where it goes */
    int w4, h4, ringRadius, shadowRadius;
    u8 *glyph, *ring, *shadow, *tmp;
    int *sums;
} SmoothJob;

static SmoothJob sJob;
static uint64_t sSliceStart;
static float sSliceMs;   /* 0: no limit */

static void AbortSmoothJob(void)
{
    if (sJob.t)
    {
        sJob.t->age = 0;
        free(sJob.t->px);
        sJob.t->px = NULL;
    }
    free(sJob.glyph);
    free(sJob.sums);
    memset(&sJob, 0, sizeof(sJob));
}

static bool8 StartSmoothJob(SmoothText *t, const Font *font)
{
    int textW = StrWidth(font, t->key), k = t->k;
    int r = k > 4 ? k : 4;

    AbortSmoothJob();
    free(t->px);
    t->px = NULL;
    sJob.t = t;
    sJob.font = font;
    sJob.k = k;
    sJob.w4 = (textW + 2 * SMOOTH_PAD) * k;
    sJob.h4 = (font->height + 2 * SMOOTH_PAD) * k;
    sJob.ringRadius = GaussRadius(r * 0.55f);
    sJob.shadowRadius = GaussRadius(3.2f);
    sJob.glyph = calloc((size_t)sJob.w4 * sJob.h4, 4);
    sJob.sums = malloc((size_t)sJob.w4 * sizeof(int));
    if (!sJob.glyph || !sJob.sums)
    {
        free(sJob.glyph);
        free(sJob.sums);
        memset(&sJob, 0, sizeof(sJob));
        t->age = 0;
        return FALSE;
    }
    sJob.ring = sJob.glyph + sJob.w4 * sJob.h4;
    sJob.shadow = sJob.ring + sJob.w4 * sJob.h4;
    sJob.tmp = sJob.shadow + sJob.w4 * sJob.h4;
    t->age = 0xFFFFFFFF;   /* never the slot the next build takes over */
    return TRUE;
}

static void StepSmoothJob(void)
{
    SmoothText *t = sJob.t;
    int w4 = sJob.w4, h4 = sJob.h4, k = sJob.k;
    u8 *glyph = sJob.glyph, *ring = sJob.ring, *shadow = sJob.shadow, *tmp = sJob.tmp;

    if (sJob.stage == SJ_GLYPHS)
    {
        const Font *font = sJob.font;
        const u8 *str = t->key + sJob.strAt;
        int x = sJob.strAt ? sJob.glyphX : SMOOTH_PAD;

        for (int n = 0;; ++n)
        {
            const u8 *at = str;
            u16 g = NextGlyph(&str);

            if (g == 0xFFFF || g == 0xFFFE)
                break;
            if (n == SJ_GLYPHS_A_STEP)
            {
                /* The rest next step, from this glyph on. */
                sJob.strAt = (u16)(at - t->key);
                sJob.glyphX = x;
                return;
            }
            int width = GlyphWidth(font, g) > 16 ? 16 : GlyphWidth(font, g);

            for (int row = 0; row < font->height; ++row)
                for (int px = 0; px < width; ++px)
                {
                    u16 bits = GlyphBits(font, g, row, px / 8);
                    u8 byte = (px & 7) < 4 ? bits >> 8 : bits & 0xFF;

                    if (((byte >> (6 - 2 * (px & 3))) & 3) != 1)
                        continue;
                    for (int dy = 0; dy < k; ++dy)
                        memset(glyph + ((SMOOTH_PAD + row) * k + dy) * w4 + (x + px) * k, 255, k);
                }
            x += GlyphWidth(font, g);
        }
        /* The outline: blurred and hardened, and never thinner than a dilation. */
        memcpy(ring, glyph, (size_t)w4 * h4);
    }
    else if (sJob.stage >= SJ_RING_BLUR && sJob.stage < SJ_RING_HARDEN)
    {
        int p = sJob.part;

        if ((sJob.stage - SJ_RING_BLUR) & 1)
            BlurColumns(tmp, ring, w4, h4, sJob.ringRadius, sJob.sums, w4 * p / SJ_PARTS, w4 * (p + 1) / SJ_PARTS);
        else
            BlurRows(ring, tmp, w4, h4, sJob.ringRadius, h4 * p / SJ_PARTS, h4 * (p + 1) / SJ_PARTS);
        if (++sJob.part < SJ_PARTS)
            return;
    }
    else if (sJob.stage == SJ_RING_HARDEN)
    {
        int p = sJob.part, end = w4 * h4 * (p + 1) / SJ_PARTS;

        for (int i = w4 * h4 * p / SJ_PARTS; i < end; ++i)
            ring[i] = ring[i] > 18 ? 255 : ring[i] * 14;
        if (++sJob.part < SJ_PARTS)
            return;
    }
    else if (sJob.stage == SJ_DILATE_ROWS)
    {
        int p = sJob.part;

        DilateRows(glyph, tmp, w4, (int)((k > 4 ? k : 4) * 0.7f + 0.5f), h4 * p / SJ_PARTS, h4 * (p + 1) / SJ_PARTS);
        if (++sJob.part < SJ_PARTS)
            return;
    }
    else if (sJob.stage == SJ_DILATE_COLUMNS)
    {
        int p = sJob.part;

        DilateColumns(tmp, shadow, w4, h4, (int)((k > 4 ? k : 4) * 0.7f + 0.5f), w4 * p / SJ_PARTS, w4 * (p + 1) / SJ_PARTS);
        if (++sJob.part < SJ_PARTS)
            return;
    }
    else if (sJob.stage == SJ_RING_MERGE)
    {
        int p = sJob.part, begin = w4 * h4 * p / SJ_PARTS, end = w4 * h4 * (p + 1) / SJ_PARTS;

        for (int i = begin; i < end; ++i)
            if (shadow[i] > ring[i])
                ring[i] = shadow[i];
        memset(shadow + begin, 0, (size_t)(end - begin));
        if (++sJob.part < SJ_PARTS)
            return;
    }
    else if (sJob.stage == SJ_SHADOW_SHIFT)
    {
        if (t->hasShadow)
        {
            int drop = (int)(4.8f * (k > 5 ? k / 5.0f : 1.0f) + 0.5f);

            for (int y = drop; y < h4; ++y)
                memcpy(shadow + y * w4, ring + (y - drop) * w4, w4);
        }
        else
            sJob.stage = SJ_SHADOW_FADE;   /* nothing to blur or fade: the ++ below leaves it */
    }
    else if (sJob.stage >= SJ_SHADOW_BLUR && sJob.stage < SJ_SHADOW_FADE)
    {
        int p = sJob.part;

        if ((sJob.stage - SJ_SHADOW_BLUR) & 1)
            BlurColumns(tmp, shadow, w4, h4, sJob.shadowRadius, sJob.sums, w4 * p / SJ_PARTS, w4 * (p + 1) / SJ_PARTS);
        else
            BlurRows(shadow, tmp, w4, h4, sJob.shadowRadius, h4 * p / SJ_PARTS, h4 * (p + 1) / SJ_PARTS);
        if (++sJob.part < SJ_PARTS)
            return;
    }
    else if (sJob.stage == SJ_SHADOW_FADE)
    {
        int p = sJob.part, end = w4 * h4 * (p + 1) / SJ_PARTS;

        /* x * 6 / 10 for x < 256, without a divide (the ARM11 has none). */
        for (int i = w4 * h4 * p / SJ_PARTS; i < end; ++i)
            shadow[i] = (u8)((shadow[i] * 39322u) >> 16);
        if (++sJob.part < SJ_PARTS)
            return;
    }
    else if (sJob.stage == SJ_DOWN)
    {
        /* Down to screen pixels: coverage and premultiplied colour. */
        if (!sJob.row)
        {
            t->w = (w4 + 3) / 4;
            t->h = (h4 + 3) / 4;
            t->px = calloc((size_t)t->w * t->h, 4);
            if (!t->px)
            {
                AbortSmoothJob();
                return;
            }
        }
        for (int oy = sJob.row; oy < t->h && oy < sJob.row + SJ_BAND; ++oy)
            for (int ox = 0; ox < t->w; ++ox)
            {
                int a = 0, cr = 0, cg = 0, cb = 0;

                for (int sy = oy * 4; sy < oy * 4 + 4 && sy < h4; ++sy)
                    for (int sx = ox * 4; sx < ox * 4 + 4 && sx < w4; ++sx)
                    {
                        int i = sy * w4 + sx, ri = ring[i], al = ri > shadow[i] ? ri : shadow[i];
                        Rgb c;

                        if (!al)
                            continue;
                        if (glyph[i])
                            c = t->fg;
                        else
                            c = (Rgb){(u8)Div255(t->shadow.r * (255 - ri) + t->outline.r * ri),
                                      (u8)Div255(t->shadow.g * (255 - ri) + t->outline.g * ri),
                                      (u8)Div255(t->shadow.b * (255 - ri) + t->outline.b * ri)};
                        a += al;
                        cr += c.r * al;
                        cg += c.g * al;
                        cb += c.b * al;
                    }
                t->px[(oy * t->w + ox) * 4 + 0] = a / 16;
                t->px[(oy * t->w + ox) * 4 + 1] = cr / (255 * 16);
                t->px[(oy * t->w + ox) * 4 + 2] = cg / (255 * 16);
                t->px[(oy * t->w + ox) * 4 + 3] = cb / (255 * 16);
            }
        sJob.row += SJ_BAND;
        if (sJob.row < t->h)
            return;   /* the next band, same stage */
    }
    sJob.part = 0;
    ++sJob.stage;
}

/* Runs the job until it is done or the slice is over; TRUE once t->px is
 * ready. A job that cannot go on is dropped and its slot left empty. */
static bool8 RunSmoothJob(void)
{
    while (sJob.t && sJob.stage < SJ_DONE)
    {
        StepSmoothJob();
        if (sJob.t && sJob.stage < SJ_DONE && sSliceMs > 0.0f && CtrPlatform_TickMs(CtrPlatform_Ticks() - sSliceStart) >= sSliceMs)
            return FALSE;
    }
    if (!sJob.t)
        return FALSE;
    free(sJob.glyph);
    free(sJob.sums);
    memset(&sJob, 0, sizeof(sJob));
    return TRUE;
}

/*
 * A whole redraw (a new mode: the action menu, the field's column) builds
 * the labels it lacks for up to sRedrawLabelMs from sRedrawStart; past it a
 * missing label is drawn flat (DrawFlatStr) for now and sRefineLabels asks
 * for the redraw again next frame, which carries the half-built label on.
 * All of them at once was 6-13 ms in one frame on an Old 3DS. 0: no limit.
 */
static float sRedrawLabelMs;
static uint64_t sRedrawStart;
static bool8 sRefineLabels;
/* This redraw has built some of a label. Each redraw builds at least one
 * slice, however long its plates took: on an Old 3DS the move menu's took
 * the whole 2 ms before its first label, every label stayed flat, and the
 * menu was drawn again every frame for as long as it was open (12 ms a
 * frame, 30 fps). */
static bool8 sRedrawLabelWorked;
/* Labels found built, and built or left flat, since the last report. */
static u32 sLabelHits, sLabelMisses;
#define REDRAW_LABEL_MS 2.0f

/* A label's stand-in: its glyphs in their colour over a one-pixel outline,
 * no blur, no shadow, at the size the built one has. */
static void DrawFlatStr(const Font *font, const u8 *str, int x, int y, int k, Rgb fg, Rgb outline)
{
    u16 colours[2] = {PackRgb(outline.r, outline.g, outline.b), PackRgb(fg.r, fg.g, fg.b)};

    for (int pass = 0; pass < 2; ++pass)
    {
        const u8 *s = str;
        int gx = 0;

        for (u16 g; (g = NextGlyph(&s)) != 0xFFFF && g != 0xFFFE;)
        {
            int width = GlyphWidth(font, g) > 16 ? 16 : GlyphWidth(font, g);

            for (int row = 0; row < font->height; ++row)
                for (int px = 0; px < width; ++px)
                {
                    u16 bits = GlyphBits(font, g, row, px / 8);
                    u8 byte = (px & 7) < 4 ? bits >> 8 : bits & 0xFF;
                    int x0 = x + ((gx + px) * k >> 2), y0 = y + (row * k >> 2);
                    int x1 = x + ((gx + px + 1) * k >> 2), y1 = y + ((row + 1) * k >> 2);

                    if (((byte >> (6 - 2 * (px & 3))) & 3) != 1)
                        continue;
                    if (pass == 0)
                        FillRect(x0 - 1, y0 - 1, x1 - x0 + 2, y1 - y0 + 2, colours[0]);
                    else
                        FillRect(x0, y0, x1 - x0, y1 - y0, colours[1]);
                }
            gx += GlyphWidth(font, g);
        }
    }
}

/* Draws a label with its glyph origin at (x, y); k quarter pixels a glyph pixel. */
static void DrawSmoothStr(const Font *font, const u8 *str, int x, int y, int k, Rgb fg, Rgb outline,
                          const Rgb *shadow)
{
    SmoothText *t = NULL, *victim = &sSmooth[0];
    int n = 0;

    if (!font->glyphs || !str)
        return;
    while (str[n] != EOS && n < (int)sizeof(t->key) - 1)
        ++n;
    for (int i = 0; i < SMOOTH_SLOTS && !t; ++i)
    {
        SmoothText *s = &sSmooth[i];

        if (s->px && s != sJob.t && s->font == (font == &sSmall) && s->k == k && memcmp(s->key, str, n) == 0 && s->key[n] == EOS
         && !memcmp(&s->fg, &fg, sizeof(fg)) && !memcmp(&s->outline, &outline, sizeof(outline))
         && s->hasShadow == (shadow != NULL) && (!shadow || !memcmp(&s->shadow, shadow, sizeof(*shadow))))
            t = s;
        else if (s->age < victim->age)
            victim = s;
    }
    if (!t)
    {
        SmoothText *job = sJob.t;
        /* A whole redraw's own build, within its time (sRedrawLabelMs); never
         * into a cache kept beyond the frame, which would keep it flat. */
        bool8 timed = sRedrawLabelMs > 0.0f && sBuildBudget == 0xFF && sDst == sCanvas;

        bool8 late = timed && CtrPlatform_TickMs(CtrPlatform_Ticks() - sRedrawStart) >= sRedrawLabelMs;

        ++sLabelMisses;
        if (late && sRedrawLabelWorked)
        {
            DrawFlatStr(font, str, x, y, k, fg, outline);
            sRefineLabels = TRUE;
            return;
        }
        if (!TakeBuildBudget())
            return;
        /* The label an earlier slice left half built goes on; any other
         * label drops it and starts over. */
        if (job && job->font == (font == &sSmall) && job->k == k && memcmp(job->key, str, n) == 0 && job->key[n] == EOS
         && !memcmp(&job->fg, &fg, sizeof(fg)) && !memcmp(&job->outline, &outline, sizeof(outline))
         && job->hasShadow == (shadow != NULL) && (!shadow || !memcmp(&job->shadow, shadow, sizeof(*shadow))))
            t = job;
        else
        {
            t = victim;
            memcpy(t->key, str, n);
            t->key[n] = EOS;
            t->font = font == &sSmall;
            t->k = k;
            t->fg = fg;
            t->outline = outline;
            t->hasShadow = shadow != NULL;
            t->shadow = shadow ? *shadow : outline;
            if (!StartSmoothJob(t, font))
                return;
        }
        if (timed)
        {
            float slice = sSliceMs;
            uint64_t sliceStart = sSliceStart;
            bool8 done;

            /* Past the redraw's time, still this one slice of its own. */
            sSliceMs = sRedrawLabelMs;
            sSliceStart = late ? CtrPlatform_Ticks() : sRedrawStart;
            sRedrawLabelWorked = TRUE;
            done = RunSmoothJob();
            sSliceMs = slice;
            sSliceStart = sliceStart;
            if (!done)
            {
                if (sJob.t)
                {
                    DrawFlatStr(font, str, x, y, k, fg, outline);
                    sRefineLabels = TRUE;
                }
                return;
            }
        }
        else if (!RunSmoothJob())
            return;
    }
    else
        ++sLabelHits;
    t->age = ++sSmoothClock;
    /* The bitmap starts SMOOTH_PAD glyph pixels up and left: k screen pixels. */
    x -= k;
    y -= k;
    for (int i = 0; i < t->w; ++i)
    {
        int sx = x + i + sOX;

        if (sx < sClipX0 || sx >= sClipX1)
            continue;
        for (int j = 0; j < t->h; ++j)
        {
            const u8 *p = t->px + (j * t->w + i) * 4;
            int sy = y + j;
            u16 *dst;
            Rgb under;

            if (!p[0] || sy < sClipY0 || sy >= sClipY1)
                continue;
            dst = sDst + sx * H + (H - 1 - sy);
            under = RgbOf(*dst);
            *dst = PackRgb(Div255(under.r * (255 - p[0])) + p[1], Div255(under.g * (255 - p[0])) + p[2],
                           Div255(under.b * (255 - p[0])) + p[3]);
        }
    }
}

/* The width a label's pixels span, enlarged k quarter pixels a glyph pixel. */
static int SmoothInkWidth(const Font *font, const u8 *str, int k)
{
    int x0, x1;

    InkColumns(font, str, &x0, &x1);
    return x1 > x0 ? (x1 - x0) * k / 4 : 0;
}

/* Text colours from the message box palette. */
#define TXT(i) (sRes.text.c[(i)])
#define TXT_DARK TXT(TEXT_COLOR_DARK_GRAY)
#define TXT_LIGHT TXT(TEXT_COLOR_LIGHT_GRAY)
#define TXT_RED TXT(TEXT_COLOR_RED)
#define TXT_LRED TXT(TEXT_COLOR_LIGHT_RED)
#define TXT_BLUE TXT(TEXT_COLOR_BLUE)
#define TXT_LBLUE TXT(TEXT_COLOR_LIGHT_BLUE)
