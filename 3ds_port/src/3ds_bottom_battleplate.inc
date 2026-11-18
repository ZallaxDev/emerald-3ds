/* Part of 3ds_bottom_ui.c: the battle plates: tints, the pictures kept of each plate. */

/* ------------------------------------------------------------------------ */
/* Drawing: battle                                                          */
/* ------------------------------------------------------------------------ */

/*
 * The battle menus, over the whole screen on the battle backdrop (see
 * "Battle art"): FIGHT and the quick ball on top, BAG, POKéMON and RUN under
 * them; the four moves two by two with CANCEL; the target choice. While the
 * turn plays out, the backdrop alone: the top screen shows the battle and its
 * message. Labels are the game's strings and glyphs (DrawSmoothStr), item
 * and type icons the game's; FIGHT's watermark is Rayquaza's own picture.
 */

static const Rgb sHueFight = {230, 52, 56}, sHueBag = {240, 178, 36}, sHueMon = {36, 172, 96},
                 sHueRun = {40, 112, 226}, sHueBall = {140, 76, 200}, sHueCancel = {88, 104, 130},
                 sHueEmpty = {150, 150, 158};
static const Rgb sCream = {252, 245, 234}, sWhite = {255, 255, 255};
static const Rgb sFocusRing[2] = {{255, 120, 40}, {255, 206, 72}};

/* A colour at keep/100 of its brightness. */
static Rgb Shade(Rgb c, int keep)
{
    return (Rgb){(u8)(c.r * keep / 100), (u8)(c.g * keep / 100), (u8)(c.b * keep / 100)};
}

/* A plate's labels: outlined in its dark tone, shadowed darker still. */
#define PLATE_DARK(c) Shade(c, 38)
#define PLATE_SHADOW(c) Shade(c, 27)

/* A move's category (physical, special, status), 32x16, as the summary shows
 * it: only with ENHANCEMENTS' PHYS/SPEC SPLIT on. */
static void DrawCategoryIcon(u16 move, int x, int y)
{
    if (sRes.categoryTiles && move < MOVES_COUNT && MoveSplitEnabled())
        DrawSprite(sRes.categoryTiles + gMoveCategories[move] * 8 * 32, 4, 2, x, y, sRes.categoryPal.c);
}

static void DrawTypeIcon(u8 type, int x, int y)
{
    if (sRes.typeTiles && type < NUMBER_OF_MON_TYPES)
        DrawSprite(sRes.typeTiles + type * 8 * 32, 4, 2, x, y, sRes.typePal[sTypeIconPal[type]].c);
}

static u8 PlateState(const ViewState *s, u8 hit, bool8 focused)
{
    if ((s->pressed == hit || (hit == HIT_QUICK_BALL
         && (s->pressed == HIT_QUICK_BALL_PREV || s->pressed == HIT_QUICK_BALL_NEXT))) && hit != HIT_NONE)
        return BTA_PRESSED;
    return focused ? BTA_FOCUS : BTA_NORMAL;
}

/*
 * A plate in a colour, worked out once: the same plate in the same colour
 * (and ring) is then copied. Stored a column at a time in the canvas' own
 * order (bottom to top), each pixel as its coverage and the colour it adds;
 * the run of each column the plate covers whole is copied with memcpy, only
 * its edges and shadow are blended. FIGHT's has Rayquaza in it already.
 */
/* Every plate of a double battle's menus in all its looks: the actions, both
 * mons' moves (each move type its own colour), CANCEL and the target plates
 * (red and blue, each normal, focused in both blink tones and pressed; grey
 * is an empty move's), 47 + 16 + 8 at most; a few to spare. Made only as
 * used: a slot costs its pixels (w * h * 3 bytes, 36 KiB for a move plate)
 * once it holds a plate. */
#define TINT_SLOTS 80

typedef struct
{
    u32 age;
    u8 id;
    Rgb colour, ring;
    s16 x, y;
    u16 w, h;
    u16 *p;          /* premultiplied RGB565, w columns of h */
    u8 *a;           /* coverage, the same way */
    u8 *run;         /* per column: the opaque run's first and end index */
    u16 done;        /* columns built so far: a build can span frames (sSliceMs) */
} Tint;

static Tint sTints[TINT_SLOTS];
static u32 sTintClock;

static bool8 SameRgb(Rgb a, Rgb b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

/* 0: failed, 1: built, 2: more columns to go (the slice was over). */
static int BuildTint(Tint *t, u8 id, Rgb colour, Rgb ring, bool8 resume)
{
    const BtaElem *e = &sBta.e[id];
    bool8 wide = id >= BTA_FIGHT_WIDE && id < BTA_FIGHT_WIDE + 3, full = id >= BTA_FIGHT_FULL && id < BTA_FIGHT_FULL + 3;
    u8 clip = wide ? BTA_CLIP_FIGHT_WIDE : BTA_CLIP_FIGHT_FULL;
    int sink = (wide && id - BTA_FIGHT_WIDE == BTA_PRESSED) || (full && id - BTA_FIGHT_FULL == BTA_PRESSED) ? BTA_SINK : 0;
    int plateW = wide ? 224 : 284;
    const Rgb body = Shade(sHueFight, 82), lines = Shade(sHueFight, 66);

    if (!resume)
    {
        free(t->p);
        t->p = malloc((size_t)e->w * e->h * 3 + e->w * 2);
        if (!t->p)
            return 0;
        t->a = (u8 *)(t->p + e->w * e->h);
        t->run = t->a + e->w * e->h;
        t->id = id;
        t->colour = colour;
        t->ring = ring;
        t->x = e->x;
        t->y = e->y;
        t->w = e->w;
        t->h = e->h;
        t->done = 0;
    }
    for (int i = t->done; i < e->w; ++i)
    {
        int best0 = 0, best1 = 0, start = -1;

        for (int k = 0; k <= e->h; ++k)
        {
            int al = 0;

            if (k < e->h)
            {
                int j = e->h - 1 - k, o = i * e->h + k;
                u32 q = (u32)j * e->w + i;
                int A = e->a[q], R = e->r ? e->r[q] : 0;
                Rgb add = RgbOf(e->c[q * 2] | (e->c[q * 2 + 1] << 8));
                int r = Div255(colour.r * A + ring.r * R) + add.r, g = Div255(colour.g * A + ring.g * R) + add.g,
                    b = Div255(colour.b * A + ring.b * R) + add.b;

                al = e->alpha[q];
                /* FIGHT's watermark, over the colour, inside it. */
                if (wide || full)
                {
                    int px = e->x + i, py = e->y + j, rx = px - (plateW - 84), ry = py - (sink - 1);

                    if (rx >= 0 && ry >= 0 && rx < RAY_SIZE && ry < RAY_SIZE)
                    {
                        int c = BtaAlpha(clip, px, py - sink);
                        int cb = Div255(Div255(sRayBody[ry * RAY_SIZE + rx] * c) * 217);
                        int cl = Div255(Div255(sRayLines[ry * RAY_SIZE + rx] * c) * 153);

                        r = Div255(r * (255 - cb) + body.r * cb);
                        g = Div255(g * (255 - cb) + body.g * cb);
                        b = Div255(b * (255 - cb) + body.b * cb);
                        r = Div255(r * (255 - cl) + lines.r * cl);
                        g = Div255(g * (255 - cl) + lines.g * cl);
                        b = Div255(b * (255 - cl) + lines.b * cl);
                    }
                }
                t->p[o] = PackRgb(r, g, b);
                t->a[o] = al;
            }
            if (k < e->h && al == 255)
            {
                if (start < 0)
                    start = k;
            }
            else if (start >= 0)
            {
                if (k - start > best1 - best0)
                    best0 = start, best1 = k;
                start = -1;
            }
        }
        /* Columns are at most a plate's height: the run fits a byte each. */
        t->run[i * 2] = best0;
        t->run[i * 2 + 1] = best1;
        t->done = i + 1;
        if (sSliceMs > 0.0f && i + 1 < e->w && CtrPlatform_TickMs(CtrPlatform_Ticks() - sSliceStart) >= sSliceMs)
            return 2;
    }
    return 1;
}

static const Tint *GetTint(u8 id, Rgb colour, Rgb ring)
{
    Tint *victim = &sTints[0];
    int state;

    if (id >= BTA_COUNT || !sBta.e[id].alpha || !sBta.e[id].a || sBta.e[id].h > 255)
        return NULL;
    for (int i = 0; i < TINT_SLOTS; ++i)
    {
        Tint *t = &sTints[i];

        /* The ring only shows on a focused plate. */
        if (t->p && t->id == id && SameRgb(t->colour, colour) && (!sBta.e[id].r || SameRgb(t->ring, ring)))
        {
            /* One a slice began and did not finish goes on. */
            if (t->done < t->w)
            {
                int state;

                if (!TakeBuildBudget())
                    return NULL;
                state = BuildTint(t, id, colour, ring, TRUE);
                if (state != 1)
                {
                    t->age = state == 2 ? 0xFFFFFFFF : 0;
                    return NULL;
                }
            }
            t->age = ++sTintClock;
            return t;
        }
        if (t->age < victim->age)
            victim = t;
    }
    if (!TakeBuildBudget())
        return NULL;
    state = BuildTint(victim, id, colour, ring, FALSE);
    if (state != 1)
    {
        /* Unfinished: kept out of the way of the next victim, and of any drawing. */
        victim->age = state == 2 ? 0xFFFFFFFF : 0;
        return NULL;
    }
    victim->age = ++sTintClock;
    return victim;
}

static void DrawTint(const Tint *t, int ax, int ay)
{
    int x0 = ax + t->x + sOX, y0 = ay + t->y;
    int i0 = sClipX0 - x0 > 0 ? sClipX0 - x0 : 0, i1 = sClipX1 - x0 < t->w ? sClipX1 - x0 : t->w;
    /* Rows y0 .. y0 + h - 1 are column indices h - 1 .. 0 (bottom up). */
    int k0 = y0 + t->h - sClipY1, k1 = y0 + t->h - sClipY0;

    if (k0 < 0) k0 = 0;
    if (k1 > t->h) k1 = t->h;
    for (int i = i0; i < i1; ++i)
    {
        u16 *col = sDst + (x0 + i) * H + (H - y0 - t->h);
        const u16 *p = t->p + i * t->h;
        const u8 *a = t->a + i * t->h;
        int r0 = t->run[i * 2], r1 = t->run[i * 2 + 1];

        if (r0 < k0) r0 = k0;
        if (r1 > k1) r1 = k1;
        if (r0 < r1)
            memcpy(col + r0, p + r0, (r1 - r0) * sizeof(u16));
        else
            r0 = r1 = k1;
        for (int k = k0; k < k1; ++k)
        {
            Rgb under, add;
            int al;

            if (k == r0)
            {
                k = r1 - 1;
                continue;
            }
            al = a[k];
            if (!al && !p[k])
                continue;
            if (al == 255)
            {
                col[k] = p[k];
                continue;
            }
            under = RgbOf(col[k]);
            add = RgbOf(p[k]);
            col[k] = PackRgb(Div255(under.r * (255 - al)) + add.r, Div255(under.g * (255 - al)) + add.g,
                             Div255(under.b * (255 - al)) + add.b);
        }
    }
}

/* Whether a plate and what is on it lie outside a partial redraw's clip: its
 * labels then need no drawing (the plate itself and its hit still run). The
 * warm-up (a build budget, an empty clip) walks everything. */
static bool8 PlateOutsideClip(int x, int y, int w, int h)
{
    return sBuildBudget == 0xFF && !ClipIsFull() && !RectsMeet(x - 4, y - 4, x + w + 4, y + h + 8, sClipX0, sClipY0, sClipX1, sClipY1);
}

/* A plate, its hit added; returns the y its contents start at (lower when
 * pressed). Without the art, a flat box in its colour. */
static int DrawBattlePlate(u8 id, int x, int y, int w, int h, Rgb colour, u8 state, u8 blink, u8 hit)
{
    const Tint *t = GetTint(id + state, colour, sFocusRing[blink & 1]);

    if (t)
        DrawTint(t, x, y);
    else if (!sBta.e[id + state].alpha)
        FillRect(x, y, w, h, PackRgb(colour.r, colour.g, colour.b));
    if (hit != HIT_NONE)
        AddHit(x, y, w, h, hit);
    return y + (state == BTA_PRESSED ? BTA_SINK : 0);
}

/* The index-th of the four words of the game's action menu text, split where
 * the game moves on to the next ({CLEAR_TO}, a new line, or the two spaces
 * the Japanese game puts between them; one space stays, as in GO NEAR). */
static const u8 *ActionLabel(bool8 safari, int index)
{
    static u8 out[4][24];
    const u8 *src = safari ? gText_SafariZoneMenu : gText_BattleMenu;
    int part = 0, n = 0;

    while (*src != EOS && part <= index)
    {
        if (*src == EXT_CTRL_CODE_BEGIN || *src == CHAR_NEWLINE || (*src == CHAR_SPACE && src[1] == CHAR_SPACE))
        {
            if (*src == CHAR_SPACE)
                while (*src == CHAR_SPACE)
                    ++src;
            else if (*src++ == EXT_CTRL_CODE_BEGIN && *src != EOS)
                src += GetExtCtrlCodeLength(*src);
            if (n || part < index)
                ++part;
            continue;
        }
        if (part == index && n < (int)sizeof(out[0]) - 1)
            out[index][n++] = *src;
        ++src;
    }
    out[index][n] = EOS;
    return out[index];
}

/* "×N", the game's way of counting items. */
static const u8 *Times(u32 n)
{
    static u8 out[2][8];
    static u8 next;
    u8 *text = out[next++ & 1];

    text[0] = CHAR_MULT_SIGN;
    StringCopy(text + 1, Number(n, 3, STR_CONV_MODE_LEFT_ALIGN));
    return text;
}

#define ACT_Y 32
#define ACT_H 74
#define ROW_Y 138
#define ROW_W 100
#define ROW_H 72

/*
 * Plate composites. A plate with its label and icons is the same picture
 * every time it is in the same look (normal, focused in either blink tone,
 * pressed) and shows the same things (the stamp: moves and their PP, the
 * ball and its count, the party's balls). While the turn plays out each
 * look is drawn once into a picture of its own - the backdrop under it
 * included (WarmBattleMenus) - and moving the cursor, a blink or a press is
 * then a copy of two such pictures: no blending, no label, no icon.
 */
/* A double battle's second mon (the player's right) has move pictures of its
 * own: sharing the first one's, warming one mon's moves threw out the other's. */
enum { CP_FIGHT, CP_BALL, CP_BAG, CP_MON, CP_RUN, CP_MOVE0, CP_CANCEL = CP_MOVE0 + MAX_MON_MOVES, CP_TARGET0,
       CP_MOVE_RIGHT0 = CP_TARGET0 + MAX_BATTLERS_COUNT, CP_COUNT = CP_MOVE_RIGHT0 + MAX_MON_MOVES };
#define CP_LOOKS 4

typedef struct
{
    u16 *px;               /* the rectangle's columns, in the canvas' own order */
    s16 x, y;
    u16 w, h;
    u32 stamp;
    bool8 valid;
} Comp;

typedef void (*PlateBody)(const ViewState *s, int arg);

static Comp sComps[CP_COUNT][CP_LOOKS];
static bool8 sCompBuild, sCompCapture;
static u8 sCompBudget;
static unsigned sCompFailed;
static bool8 sCompAny;

static u32 Mix(u32 h, u32 v)
{
    return (h ^ v) * 16777619u;
}

static void FreeComps(void)
{
    for (int i = 0; i < CP_COUNT; ++i)
        for (int k = 0; k < CP_LOOKS; ++k)
        {
            free(sComps[i][k].px);
            memset(&sComps[i][k], 0, sizeof(sComps[i][k]));
        }
}

/* The picture over the canvas, inside the clip. */
static void CompCopy(const Comp *c)
{
    int x0 = c->x > sClipX0 ? c->x : sClipX0, x1 = c->x + c->w < sClipX1 ? c->x + c->w : sClipX1;
    int y0 = c->y > sClipY0 ? c->y : sClipY0, y1 = c->y + c->h < sClipY1 ? c->y + c->h : sClipY1;

    if (x0 >= x1 || y0 >= y1)
        return;
    for (int x = x0; x < x1; ++x)
        memcpy(sCanvas + x * H + (H - y1), c->px + (x - c->x) * c->h + (c->h - (y1 - c->y)),
               (size_t)(y1 - y0) * sizeof(u16));
}

/* Every icon the snapshot needs is in (Prefetch loaded none this frame): a
 * plate drawn now is whole, and its picture may be kept. */
static bool8 sIconsReady;
/* Every icon the screen needs is loaded (this frame's sIconsReady stays FALSE
 * on the frame they come in, for the captures' sake). */
static bool8 sIconsIn;
/* Icons come in for a native view: its area drawn again, not the screen. */
static bool8 sAreaRedraw;
/* Plates kept on screen this frame: one. All of them in the redraw that opens
 * the action menu was 17-18 ms on an Old 3DS; the rest follow on the blinks. */
static u8 sShowCaptures;

/* Draws a plate: from its picture if it has one, else by its body. In the
 * warm-up (sCompBuild) a missing picture is made, one a frame. On screen a
 * look the warm-up has not reached yet is kept as it is drawn, when the
 * redraw holds the whole plate: the ring's next blink in that tone, or the
 * next press, is then a copy. In a battle over the voxel world the warm-up
 * gets little of the frame, and every blink of the action menu drew its
 * plates again, 5-10 ms on an Old 3DS (DROP bottom=9). */
static void DrawCached(int pid, u8 state, u32 stamp, int ax, int ay, u8 tintBase, int hw, int hh, u8 hit,
                       PlateBody body, const ViewState *s, int arg)
{
    u8 look = state == BTA_PRESSED ? 3 : state == BTA_FOCUS ? 1 + (s->blink & 1) : 0;
    Comp *c = &sComps[pid][look];
    const BtaElem *e = &sBta.e[tintBase + state];

    if (!e->alpha || !e->w || !e->h)
    {
        body(s, arg);
        return;
    }
    if (c->valid && c->stamp == stamp)
    {
        CompCopy(c);
        if (hit != HIT_NONE)
            AddHit(ax, ay, hw, hh, hit);
        return;
    }
    bool8 warm = sCompBuild && sCompBudget && !sCompCapture;
    int rx = ax + e->x, ry = ay + e->y;
    bool8 show = !warm && !sCompBuild && !sCompCapture && sBuildBudget == 0xFF && sDst == sCanvas && sIconsReady
              && sShowCaptures && rx >= sClipX0 && ry >= sClipY0 && rx + e->w <= sClipX1 && ry + e->h <= sClipY1;

    if (warm || show)
    {
        int clip[4] = {sClipX0, sClipY0, sClipX1, sClipY1};
        u8 budget = sBuildBudget;
        bool8 icons = sIconBudget, refine = sRefineLabels;
        int bad = 0;

        if (rx < 0 || ry < 0 || rx + e->w > W || ry + e->h > H)
        {
            if (show)
                body(s, arg);
            return;
        }
        if (warm)
            --sCompBudget;
        else
            --sShowCaptures;
        sClipX0 = rx;
        sClipY0 = ry;
        sClipX1 = rx + e->w;
        sClipY1 = ry + e->h;
        if (warm)
        {
            sBuildBudget = 0;           /* nothing may be built meanwhile: a gap would be kept */
            sIconBudget = TRUE;
        }
        sCompBad = FALSE;
        sRefineLabels = FALSE;      /* a label drawn flat for now is not kept either */
        sCompCapture = TRUE;
        CopyCache(sCache[CACHE_BATTLE] ? CACHE_BATTLE : CACHE_WIDE);
        body(s, arg);
        sCompCapture = FALSE;
        bad = sCompBad || sRefineLabels;
        sRefineLabels |= refine;
        if (!bad && (c->px == NULL || c->w != e->w || c->h != e->h))
        {
            free(c->px);
            c->px = malloc((size_t)e->w * e->h * sizeof(u16));
        }
        if (!bad && c->px)
        {
            for (int x = 0; x < e->w; ++x)
                memcpy(c->px + x * e->h, sCanvas + (rx + x) * H + (H - ry - e->h), (size_t)e->h * sizeof(u16));
            c->x = rx;
            c->y = ry;
            c->w = e->w;
            c->h = e->h;
            c->stamp = stamp;
            c->valid = TRUE;
            sCompAny = TRUE;
        }
        else if (warm)
            ++sCompFailed;
        sClipX0 = clip[0];
        sClipY0 = clip[1];
        sClipX1 = clip[2];
        sClipY1 = clip[3];
        sBuildBudget = budget;
        sIconBudget = icons;
        return;
    }
    body(s, arg);
}
