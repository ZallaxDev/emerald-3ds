/* Part of 3ds_bottom_ui.c: the battle menus' art, the title's Rayquaza, the types' colours. */

/* ------------------------------------------------------------------------ */
/* Battle art                                                               */
/* ------------------------------------------------------------------------ */

/*
 * The battle menus' backdrop and plates are the port's own pictures,
 * drawn smooth by scripts/gen_battle_art.py into bottom/battle.bin. A plate
 * takes its colour here - FIGHT's red, a move's type - so each pixel is
 * stored as what it adds over whatever lies under it:
 *
 *     out = under * (255 - alpha) / 255 + colour * A / 255 + ring * R / 255 + C
 *
 * (R only where a focused plate has its ring). Ids as the script's.
 */
enum
{
    BTA_BACKDROP = 0,
    BTA_FIGHT_WIDE = 16, BTA_FIGHT_FULL = 19, BTA_BALL = 22, BTA_BOTTOM = 25, BTA_MOVE = 28, BTA_CANCEL = 31,
    BTA_TARGET = 34,
    BTA_CLIP_FIGHT_WIDE = 40, BTA_CLIP_FIGHT_FULL = 41,
    BTA_ICON_BAG = 48, BTA_ICON_RUN, BTA_ICON_NEAR, BTA_ICON_BLOCK,
    BTA_PARTY_OK = 56, BTA_PARTY_STATUS, BTA_PARTY_FAINT, BTA_PARTY_EMPTY,
    BTA_COUNT = 64,
};
/* A plate's id plus its state. */
enum { BTA_NORMAL, BTA_FOCUS, BTA_PRESSED };
/* How far a pressed plate sinks. */
#define BTA_SINK 2

typedef struct
{
    s16 x, y;          /* the box, from the element's anchor */
    u16 w, h;
    u8 flags;
    const u8 *alpha, *a, *r, *c;   /* c: RGB565, little endian */
} BtaElem;

static struct
{
    u8 *file;
    BtaElem e[BTA_COUNT];
} sBta;

static u8 sExpand5[32], sExpand6[64];

static void LoadBattleArt(void)
{
    FILE *f = fopen("romfs:/bottom/battle.bin", "rb");
    long size;
    const u8 *p, *end;
    u16 count;

    for (int i = 0; i < 32; ++i)
        sExpand5[i] = i * 255 / 31;
    for (int i = 0; i < 64; ++i)
        sExpand6[i] = i * 255 / 63;
    if (!f)
    {
        CtrLog_Write(CTR_LOG_ERROR, "bottom: bottom/battle.bin missing, battle menus stay plain");
        return;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    sBta.file = size > 10 ? malloc(size) : NULL;
    if (!sBta.file || fread(sBta.file, 1, size, f) != (size_t)size || memcmp(sBta.file, "EM3DBTA1", 8) != 0)
    {
        CtrLog_Write(CTR_LOG_ERROR, "bottom: bottom/battle.bin unreadable");
        free(sBta.file);
        sBta.file = NULL;
        fclose(f);
        return;
    }
    fclose(f);
    p = sBta.file + 8;
    end = sBta.file + size;
    count = p[0] | (p[1] << 8);
    p += 2;
    for (u16 n = 0; n < count && p + 10 <= end; ++n)
    {
        u8 id = p[0];
        BtaElem e = {
            .flags = p[1],
            .x = (s16)(p[2] | (p[3] << 8)), .y = (s16)(p[4] | (p[5] << 8)),
            .w = p[6] | (p[7] << 8), .h = p[8] | (p[9] << 8),
        };
        u32 area = (u32)e.w * e.h;

        p += 10;
        if (e.flags & 8)
        {
            e.c = p;
            p += area * 2;
        }
        else
        {
            e.alpha = p;
            p += area;
            if (!(e.flags & 4))
            {
                e.a = p;
                e.c = p + area;
                p += area * 3;
                if (e.flags & 2)
                {
                    e.r = p;
                    p += area;
                }
            }
        }
        if (p > end)
            break;
        if (id < BTA_COUNT)
            sBta.e[id] = e;
    }
    /* The backdrop is in the canvas' layout already: it is a cache. */
    sCache[CACHE_BATTLE] = (u16 *)sBta.e[BTA_BACKDROP].c;
}

typedef struct { u8 r, g, b; } Rgb;

static inline Rgb RgbOf(u16 c)
{
    return (Rgb){sExpand5[c >> 11], sExpand6[(c >> 5) & 63], sExpand5[c & 31]};
}

static inline u16 PackRgb(int r, int g, int b)
{
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

/* x / 255 for x up to 255 * 255 * 3, rounded. */
static inline int Div255(int x)
{
    return (x + 128 + ((x + 128) >> 8)) >> 8;
}

/* An element at its anchor, in `colour`, its ring (if any) in `ring`. */
static void DrawBta(u8 id, int ax, int ay, Rgb colour, Rgb ring)
{
    const BtaElem *e = id < BTA_COUNT ? &sBta.e[id] : NULL;
    int x0, y0, i0, i1, j0, j1;

    if (!e || !e->alpha || !e->a)
        return;
    x0 = ax + e->x + sOX;
    y0 = ay + e->y;
    i0 = sClipX0 - x0 > 0 ? sClipX0 - x0 : 0;
    j0 = sClipY0 - y0 > 0 ? sClipY0 - y0 : 0;
    i1 = sClipX1 - x0 < e->w ? sClipX1 - x0 : e->w;
    j1 = sClipY1 - y0 < e->h ? sClipY1 - y0 : e->h;
    for (int i = i0; i < i1; ++i)
    {
        u16 *dst = sDst + (x0 + i) * H + (H - 1 - (y0 + j0));

        for (int j = j0; j < j1; ++j, --dst)
        {
            u32 q = (u32)j * e->w + i;
            int al = e->alpha[q], A = e->a[q], R = e->r ? e->r[q] : 0;
            u16 c = e->c[q * 2] | (e->c[q * 2 + 1] << 8);
            Rgb under, add;

            if (!al && !A && !R && !c)
                continue;
            under = RgbOf(*dst);
            add = RgbOf(c);
            *dst = PackRgb(Div255(under.r * (255 - al) + colour.r * A + ring.r * R) + add.r,
                           Div255(under.g * (255 - al) + colour.g * A + ring.g * R) + add.g,
                           Div255(under.b * (255 - al) + colour.b * A + ring.b * R) + add.b);
        }
    }
}

/* An alpha mask's value at a point from its anchor (0 outside it). */
static u8 BtaAlpha(u8 id, int dx, int dy)
{
    const BtaElem *e = &sBta.e[id];

    dx -= e->x;
    dy -= e->y;
    if (!e->alpha || dx < 0 || dy < 0 || dx >= e->w || dy >= e->h)
        return 0;
    return e->alpha[dy * e->w + dx];
}

/* A coverage map (0-255 per pixel, w x h) in one colour, times a clip. */
static void DrawCoverage(const u8 *cover, int w, int h, int x, int y, Rgb colour, int opacity, u8 clip, int clipX,
                         int clipY)
{
    for (int i = 0; i < w; ++i)
    {
        int sx = x + i + sOX;

        if (sx < sClipX0 || sx >= sClipX1)
            continue;
        for (int j = 0; j < h; ++j)
        {
            int sy = y + j, al = cover[j * w + i];
            u16 *dst;
            Rgb under;

            if (!al || sy < sClipY0 || sy >= sClipY1)
                continue;
            if (clip != BTA_COUNT)
                al = Div255(al * BtaAlpha(clip, x + i - clipX, y + j - clipY));
            al = Div255(al * opacity);
            if (!al)
                continue;
            dst = sDst + sx * H + (H - 1 - sy);
            under = RgbOf(*dst);
            *dst = PackRgb(Div255(under.r * (255 - al) + colour.r * al), Div255(under.g * (255 - al) + colour.g * al),
                           Div255(under.b * (255 - al) + colour.b * al));
        }
    }
}

/* An item icon's shadow: its shape in black at a quarter. */
static void DrawItemIconShadow(u16 item, int x, int y)
{
    int slot = ItemIcon(item);
    static u8 cover[24 * 24];

    if (slot < 0)
        return;
    for (int t = 0; t < 9; ++t)
        for (int py = 0; py < 8; ++py)
            for (int px = 0; px < 8; ++px)
            {
                u8 v = (sItemIcons[slot].tiles[t * 32 + py * 4 + px / 2] >> ((px & 1) * 4)) & 15;

                cover[((t / 3) * 8 + py) * 24 + (t % 3) * 8 + px] = v ? 255 : 0;
            }
    DrawCoverage(cover, 24, 24, x, y, (Rgb){0, 0, 0}, 64, BTA_COUNT, 0, 0);
}

/*
 * FIGHT's watermark: Rayquaza, from its own front picture, as a smooth
 * silhouette 1.1 times its size, with its darkest pixels (its outline and
 * inner lines) as a second mask. Each screen pixel takes 4x4 samples of the
 * picture, softened by a [1 2 1] blur and read bilinearly, against a
 * threshold: an edge that follows the shape instead of its pixel steps.
 */
#define RAY_SIZE 70
static u8 sRayBody[RAY_SIZE * RAY_SIZE], sRayLines[RAY_SIZE * RAY_SIZE];

static float RaySample(const float *m, float u, float v)
{
    int x0 = (int)floorf(u), y0 = (int)floorf(v);
    float fx = u - x0, fy = v - y0, s = 0;

    for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx)
        {
            int x = x0 + dx, y = y0 + dy;
            float wgt = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);

            if (x >= 0 && y >= 0 && x < 64 && y < 64)
                s += wgt * m[y * 64 + x];
        }
    return s;
}

static void BuildRayquaza(void)
{
    static float body[64 * 64], lines[64 * 64], soft[64 * 64];
    u8 *tiles = Unlz(gMonFrontPicTable[SPECIES_RAYQUAZA].data, NULL);
    u16 *pal = Unlz(gMonPaletteTable[SPECIES_RAYQUAZA].data, NULL);

    if (tiles && pal)
    {
        for (int t = 0; t < 64; ++t)
            for (int py = 0; py < 8; ++py)
                for (int px = 0; px < 8; ++px)
                {
                    u8 v = (tiles[t * 32 + py * 4 + px / 2] >> ((px & 1) * 4)) & 15;
                    int x = (t % 8) * 8 + px, y = (t / 8) * 8 + py;
                    u16 c = pal[v];
                    int sum = ((c & 31) + ((c >> 5) & 31) + ((c >> 10) & 31)) * 255 / 31;

                    body[y * 64 + x] = v ? 1.0f : 0.0f;
                    lines[y * 64 + x] = v && sum < 200 ? 1.0f : 0.0f;
                }
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x)
            {
                float s = 0, n = 0;

                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        float wgt = (dx ? 1.0f : 2.0f) * (dy ? 1.0f : 2.0f);

                        if (x + dx >= 0 && y + dy >= 0 && x + dx < 64 && y + dy < 64)
                            s += wgt * body[(y + dy) * 64 + x + dx];
                        n += wgt;
                    }
                soft[y * 64 + x] = s / n;
            }
        for (int Y = 0; Y < RAY_SIZE; ++Y)
            for (int X = 0; X < RAY_SIZE; ++X)
            {
                int inBody = 0, inLines = 0;

                for (int sy = 0; sy < 4; ++sy)
                    for (int sx = 0; sx < 4; ++sx)
                    {
                        float u = (X + (sx + 0.5f) / 4) * (64.0f / 70.4f) - 0.5f;
                        float v = (Y + (sy + 0.5f) / 4) * (64.0f / 70.4f) - 0.5f;

                        inBody += RaySample(soft, u, v) >= 0.47f;
                        inLines += RaySample(lines, u, v) >= 0.43f;
                    }
                sRayBody[Y * RAY_SIZE + X] = inBody * 255 / 16;
                sRayLines[Y * RAY_SIZE + X] = inLines * 255 / 16;
            }
    }
    free(tiles);
    free(pal);
}

/* Which of the three type label palettes each type uses
 * (pokemon_summary_screen.c's sMoveTypeToOamPaletteNum, minus 13). */
static const u8 sTypeIconPal[NUMBER_OF_MON_TYPES] = {
    [TYPE_NORMAL] = 0, [TYPE_FIGHTING] = 0, [TYPE_FLYING] = 1, [TYPE_POISON] = 1,
    [TYPE_GROUND] = 0, [TYPE_ROCK] = 0, [TYPE_BUG] = 2, [TYPE_GHOST] = 1,
    [TYPE_STEEL] = 0, [TYPE_MYSTERY] = 2, [TYPE_FIRE] = 0, [TYPE_WATER] = 1,
    [TYPE_GRASS] = 2, [TYPE_ELECTRIC] = 0, [TYPE_PSYCHIC] = 1, [TYPE_ICE] = 1,
    [TYPE_DRAGON] = 2, [TYPE_DARK] = 0,
};

/* A move plate's colour: the main colour of the type's own label, its most
 * used one that is neither its white nor its dark outline. */
static Rgb sTypeColour[NUMBER_OF_MON_TYPES];

static void BuildTypeColours(void)
{
    for (int type = 0; type < NUMBER_OF_MON_TYPES; ++type)
    {
        const u16 *pal = sRes.typePal[sTypeIconPal[type]].c;
        int count[16] = {0}, best = -1;

        sTypeColour[type] = (Rgb){150, 150, 150};
        if (!sRes.typeTiles)
            continue;
        for (int i = 0; i < 8 * 32; ++i)
        {
            u8 byte = sRes.typeTiles[type * 8 * 32 + i];

            ++count[byte & 15];
            ++count[byte >> 4];
        }
        for (int v = 1; v < 16; ++v)
        {
            Rgb c = RgbOf(pal[v]);

            if ((c.r > 230 && c.g > 230 && c.b > 230) || c.r + c.g + c.b <= 120)
                continue;
            if (best < 0 || count[v] > count[best])
                best = v;
        }
        if (best >= 0)
            sTypeColour[type] = RgbOf(pal[best]);
    }
}
