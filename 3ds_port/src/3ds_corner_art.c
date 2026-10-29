#include "3ds_corner_art.h"

#include <math.h>
#include <stdbool.h>

#include "3ds_video.h"

/* A GBA colour (five bits a channel) as the screen shows it, in RGB565. */
typedef struct { float r, g, b; } Colour;

static Colour Gba(unsigned r, unsigned g, unsigned b)
{
    return (Colour){(float)(r * 8 + r / 4), (float)(g * 8 + g / 4), (float)(b * 8 + b / 4)};
}

static Colour Shade(Colour c, float k)
{
    return (Colour){c.r * k, c.g * k, c.b * k};
}

static uint16_t Pack(Colour c)
{
    unsigned r = c.r > 255.0f ? 255u : (unsigned)(c.r + 0.5f);
    unsigned g = c.g > 255.0f ? 255u : (unsigned)(c.g + 0.5f);
    unsigned b = c.b > 255.0f ? 255u : (unsigned)(c.b + 0.5f);

    return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}

static void Put(uint16_t *texture, unsigned dim, unsigned x, unsigned y, Colour c)
{
    texture[CtrVideo_Texel(x % dim, y % dim, dim)] = Pack(c);
}

/* Signed distance from a to b round a repeating period, in [-period/2, period/2). */
static int Around(int a, int b, int period)
{
    return ((a - b + period + period / 2) % period) - period / 2;
}

void CtrCornerArt_Felt(uint16_t *pattern, uint16_t *trim, uint16_t table)
{
    /* Poké Balls one shade darker than the felt, in staggered rows: 64 apart
     * across, rows 32 apart, every other row moved by half. */
    static const int balls[2][2] = {{32, 16}, {0, 48}};
    const unsigned dim = CTR_CORNER_PATTERN_DIM, trimDim = CTR_CORNER_TRIM_DIM;
    Colour base = Gba(table & 31, (table >> 5) & 31, (table >> 10) & 31);
    Colour dark = Shade(base, 0.82f), lid = Shade(base, 0.92f);
    Colour gold = Gba(30, 24, 8), goldDark = Gba(22, 15, 4);

    for (unsigned y = 0; y < dim; ++y)
        for (unsigned x = 0; x < dim; ++x)
        {
            Colour c = base;

            for (unsigned i = 0; i < 2; ++i)
            {
                float ox = Around((int)x, balls[i][0], (int)dim) + 0.5f;
                float oy = Around((int)y, balls[i][1], (int)dim) + 0.5f;
                float d = sqrtf(ox * ox + oy * oy);

                if (d >= 11.5f) continue;
                /* Rim, the band across and the button's ring; the lid a
                 * touch darker than the felt. */
                if (d > 10.0f || (fabsf(oy) < 1.2f && d > 3.5f) || (d > 2.0f && d < 3.5f))
                    c = dark;
                else if (oy < 0.0f)
                    c = lid;
            }
            Put(pattern, dim, x, y, c);
        }

    for (unsigned y = 0; y < trimDim; ++y)
        for (unsigned x = 0; x < trimDim; ++x)
            Put(trim, trimDim, x, y, Shade(base, 0.66f));
    /* The cord along row 0 and down column 0. */
    for (unsigned i = 0; i < trimDim; ++i)
    {
        Put(trim, trimDim, i, 0, i % 4 < 2 ? gold : goldDark);
        Put(trim, trimDim, 0, i, i % 4 < 2 ? gold : goldDark);
    }
    Put(trim, trimDim, CTR_CORNER_FELT_LINE, CTR_CORNER_FELT_LINE, Shade(base, 0.74f));
}

/* A 9x9 gold coin centred on (cx, cy), wrapping round the pattern. */
static void Coin(uint16_t *pattern, int cx, int cy)
{
    const unsigned dim = CTR_CORNER_PATTERN_DIM;
    Colour gold = Gba(31, 26, 8), goldDark = Gba(24, 16, 4), shine = Gba(31, 31, 20);

    for (int dy = -5; dy <= 5; ++dy)
        for (int dx = -5; dx <= 5; ++dx)
        {
            float d = sqrtf((float)(dx * dx + dy * dy));
            Colour c = gold;

            if (d > 4.6f) continue;
            if (d > 3.6f || dy == 0) c = goldDark;
            else if (dx <= -1 && dy <= -1 && d < 3.0f) c = shine;
            Put(pattern, dim, (unsigned)(cx + dx + (int)dim), (unsigned)(cy + dy + (int)dim), c);
        }
}

/* A 4x4 bulb, its corners off, its top-left texel at (x, y). */
static void Bulb(uint16_t *trim, unsigned x, unsigned y, bool lit)
{
    Colour gold = Gba(31, 26, 8), goldDark = Gba(24, 16, 4), shine = Gba(31, 31, 20);

    for (unsigned j = 0; j < 4; ++j)
        for (unsigned i = 0; i < 4; ++i)
        {
            bool inner = i >= 1 && i <= 2 && j >= 1 && j <= 2;

            if ((i == 0 || i == 3) && (j == 0 || j == 3)) continue;
            Put(trim, CTR_CORNER_TRIM_DIM, x + i, y + j, !lit ? goldDark : inner ? shine : gold);
        }
}

void CtrCornerArt_Wall(uint16_t *pattern, uint16_t *trim)
{
    const unsigned dim = CTR_CORNER_PATTERN_DIM, trimDim = CTR_CORNER_TRIM_DIM;
    Colour wall = Gba(3, 9, 9), wallLight = Gba(4, 11, 11), seam = Gba(7, 16, 15);
    Colour red = Gba(18, 4, 6);

    /* Diamonds 32 across, two shades in a check, their seams lighter, a coin
     * in the middle of each. */
    for (unsigned y = 0; y < dim; ++y)
        for (unsigned x = 0; x < dim; ++x)
        {
            unsigned u = x + y, v = x + dim - y;
            Colour c = ((u / 32 + v / 32) & 1) ? wallLight : wall;

            if (u % 32 == 0 || v % 32 == 0) c = seam;
            Put(pattern, dim, x, y, c);
        }
    for (int cy = 0; cy < (int)dim; cy += 16)
        for (int cx = (cy % 32) ? 0 : 16; cx < (int)dim; cx += 32)
            Coin(pattern, cx, cy);

    for (unsigned y = 0; y < trimDim; ++y)
        for (unsigned x = 0; x < trimDim; ++x)
            Put(trim, trimDim, x, y, red);
    /* Along the band and down it, lit and dark in turn. */
    Bulb(trim, 1, 1, true);
    Bulb(trim, 17, 1, false);
    Bulb(trim, 1, 17, false);
    Put(trim, trimDim, CTR_CORNER_FRAME_DARK, CTR_CORNER_FRAME_DARK, Gba(24, 16, 4));
    Put(trim, trimDim, CTR_CORNER_FRAME_GOLD, CTR_CORNER_FRAME_GOLD, Gba(31, 26, 8));
    Put(trim, trimDim, CTR_CORNER_FRAME_SHADOW, CTR_CORNER_FRAME_SHADOW, Gba(2, 4, 4));
}
