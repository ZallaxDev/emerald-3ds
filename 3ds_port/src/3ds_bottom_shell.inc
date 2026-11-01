/*
 * The bottom screen's frame (included by 3ds_bottom_ui.c): the header row,
 * the section dock and the windows the map and the game's screens sit in.
 *
 *   y 0..10    the backdrop's dark top strip (romfs/bottom/menu.bin)
 *   y 12..36   the header: Y, the title, the Repel, the clock, RUN
 *   y 38..202  the content area, CX 4 CW 312
 *   y 204..240 the dock: one button a section, NUZLOCKE last when it is on
 *
 * Every piece is a plate (3ds_bottom_plate.inc); a chosen or pressed one is
 * emerald. The game's own screens (the party menu, the bag, the Pokédex,
 * the PokéNav, the boxes) are its 240x160 picture 1:1 in a window at
 * (CTR_BOTTOM_WIN_X, CTR_BOTTOM_WIN_Y), drawn by the compositor; this canvas
 * owns everything round it.
 */

/* The header row. */
#define HEAD_Y 12
#define HEAD_H 24
#define HEAD_Y_X 2
#define HEAD_Y_W 30
#define TITLE_X 34
#define TITLE_W 118
#define TITLE_GRASS_W 26
#define REPEL_X 154
#define REPEL_W 46
#define CLOCK_X 202
#define CLOCK_W 84
#define RUN_X 288
#define RUN_W 30
/* Without the Repel's plate (REPEL STEPS off) the title takes its room. */
#define TITLE_WIDE_W (CLOCK_X - 2 - TITLE_X)

/* The dock. */
#define DOCK_Y 204
#define DOCK_TOP 207
#define DOCK_H 30
#define DOCK_GAP 2

/* The content area. */
#define AREA_X 4
#define AREA_Y 38
#define AREA_W 312
#define AREA_H 164

/* The map's window, the map 1:1 inside its 2px frame. */
#define MAP_WIN_X 34
#define MAP_WIN_Y 38
#define MAP_WIN_W 252
#define MAP_WIN_H 164
#define MAP_ORIGIN_X (MAP_WIN_X + 2)
#define MAP_ORIGIN_Y (MAP_WIN_Y + 2)
/* The game's window, its picture 1:1 inside the same frame. */
#define GAME_WIN_X (CTR_BOTTOM_WIN_X - 2)
#define GAME_WIN_Y (CTR_BOTTOM_WIN_Y - 2)
#define GAME_WIN_W (240 + 4)
#define GAME_WIN_H (160 + 4)

static const Rgb sEmerald = {88, 192, 128}, sLight = {236, 246, 238}, sOffGrey = {150, 156, 166},
                 sYGreen = {120, 200, 96}, sWildOff = {96, 160, 128}, sCreamPlate = {252, 245, 234},
                 sSlate = {96, 128, 176}, sCancelGrey = {120, 128, 140}, sCardGreen = {72, 160, 112},
                 sGroupOff = {214, 234, 220}, sBlue = {56, 144, 216}, sRedPlate = {216, 88, 80},
                 sDexRed = {208, 64, 56}, sFieldBlue = {64, 120, 200};
/* Lines on cream (rows, rules) and the odd rows of a list. */
static const Rgb sRule = {228, 218, 202}, sOddRow = {243, 234, 219};
static const Rgb sLed = {64, 232, 96};

/* A palette's grey, as the mockups grey an icon: its luma. */
static void GreyPal(const u16 *src, u16 *dst)
{
    for (int i = 0; i < 16; ++i)
    {
        Rgb c = RgbOf(src[i]);
        int l = (c.r * 299 + c.g * 587 + c.b * 114 + 500) / 1000;

        dst[i] = PackRgb(l, l, l);
    }
}

/* An icon's visible pixels centred on (cx2, cy2), doubled; grey with `grey`. */
static void IconCentred(const u8 *tiles, int size, int cx2, int cy2, const u16 *pal, bool8 grey)
{
    u16 g[16];

    if (!tiles)
        return;
    if (grey)
    {
        GreyPal(pal, g);
        pal = g;
    }
    DrawSpriteCentred(tiles, size, size, cx2, cy2, pal);
}

/* Draws only inside [x0, x1) x [y0, y1) as well as the clip it had. */
static void ClipPush(int saved[4], int x0, int y0, int x1, int y1)
{
    saved[0] = sClipX0;
    saved[1] = sClipY0;
    saved[2] = sClipX1;
    saved[3] = sClipY1;
    x0 += sOX;
    x1 += sOX;
    if (sClipX0 < x0) sClipX0 = x0;
    if (sClipY0 < y0) sClipY0 = y0;
    if (sClipX1 > x1) sClipX1 = x1;
    if (sClipY1 > y1) sClipY1 = y1;
    if (sClipX1 < sClipX0) sClipX1 = sClipX0;
    if (sClipY1 < sClipY0) sClipY1 = sClipY0;
}

static void ClipPop(const int saved[4])
{
    sClipX0 = saved[0];
    sClipY0 = saved[1];
    sClipX1 = saved[2];
    sClipY1 = saved[3];
}

/* A filled rectangle in a colour. */
static void FillRgb(int x, int y, int w, int h, Rgb c)
{
    FillRect(x, y, w, h, PackRgb(c.r, c.g, c.b));
}

/*
 * A window's frame (the mockups' chamfer_frame): a soft shadow, an ink
 * outline with its corners cut c, a cream rim fading down, and inside it
 * the hole the picture covers, `border` in. Drawn once into a cache.
 */
static void DrawChamferFrame(int x, int y, int w, int h, int c, int border)
{
    static const Rgbf ink = {22, 28, 38}, shadow = {8, 36, 24}, rimTop = {252, 245, 234}, rimLow = {236, 224, 208};
    int x0 = x - 6 < 0 ? 0 : x - 6, y0 = y - 6 < 0 ? 0 : y - 6;
    int x1 = x + w + 6 > W ? W : x + w + 6, y1 = y + h + 8 > H ? H : y + h + 8;
    float hc = c - border / 2 > 1 ? c - border / 2 : 1;

    for (int py = y0; py < y1; ++py)
        for (int px = x0; px < x1; ++px)
        {
            float u = px + 0.5f, v = py + 0.5f;
            float hole = Cover(ChamferDist(u, v, x + border, y + border, w - 2 * border, h - 2 * border, hc));
            Rgb under;
            Px p;

            if (hole >= 1.0f || px + sOX < sClipX0 || px + sOX >= sClipX1 || py < sClipY0 || py >= sClipY1)
                continue;
            under = RgbOf(sDst[(px + sOX) * H + (H - 1 - py)]);
            p = (Px){1.0f, 0, 0, 0};
            Paint(&p, shadow, 0.55f * Blurred(ChamferDist(u, v, x, y + 3.5f, w, h, c), 1.2f));
            Paint(&p, ink, Cover(ChamferDist(u, v, x, y, w, h, c)));
            Paint(&p, Mixf(rimTop, rimLow, Clamp01((v - y) / h)),
                  Cover(ChamferDist(u, v, x + 1.2f, y + 1.2f, w - 2.4f, h - 2.4f, c)));
            Paint(&p, ink, hole);
            Put(px, py, PackRgb((int)(under.r * p.t + p.r + 0.5f), (int)(under.g * p.t + p.g + 0.5f),
                                (int)(under.b * p.t + p.b + 0.5f)));
        }
}

/* ------------------------------------------------------------------------ */
/* The dock                                                                 */
/* ------------------------------------------------------------------------ */

static int DockCount(const ViewState *s)
{
    return (s->enabled >> SCR_NUZLOCKE) & 1 ? SCR_COUNT : SCR_NUZLOCKE;
}

/* Button i's left edge and width: n buttons gap 2 apart, centred in 316px. */
static void DockButton(const ViewState *s, int i, int *x, int *w)
{
    int n = DockCount(s), bw = (316 - (n - 1) * DOCK_GAP) / n;

    *w = bw;
    *x = (W - (n * bw + (n - 1) * DOCK_GAP)) / 2 + i * (bw + DOCK_GAP);
}

static void DrawDock(const ViewState *s)
{
    for (int i = 0; i < DockCount(s); ++i)
    {
        bool8 enabled = (s->enabled >> i) & 1 && (!s->dockLocked || i == s->screen);
        bool8 pressed = enabled && s->pressed == HIT_COLUMN + i, chosen = enabled && s->screen == i;
        const Icon *icon = &sRes.column[i];
        int x, w, oy;
        u8 state = pressed ? PLATE_STATE_PRESSED : s->focus == i ? PLATE_STATE_FOCUS : PLATE_STATE_NORMAL;

        DockButton(s, i, &x, &w);
        oy = Plate(x, DOCK_TOP, w, DOCK_H, !enabled ? sOffGrey : pressed || chosen ? sEmerald : sLight, state);
        IconCentred(icon->tiles, icon->size, 2 * x + w, 2 * oy + DOCK_H - 1, icon->pal.c, !enabled);
        if (enabled)
            AddHit(x - DOCK_GAP / 2, DOCK_Y, w + DOCK_GAP, H - DOCK_Y, HIT_COLUMN + i);
    }
}

/* ------------------------------------------------------------------------ */
/* The header                                                               */
/* ------------------------------------------------------------------------ */

/* A section's name, as its title and as the title while its button is held. */
static const u8 *SectionName(int screen)
{
    switch (screen)
    {
    case SCR_MAP: return Ascii(CTR_TEXT("MAP", "MAPA", "CARTE", "MAPPA", "MAPA", "KARTE", "マップ"));
    case SCR_POKEDEX: return gText_MenuPokedex;
    case SCR_POKEMON: return gText_MenuPokemon;
    case SCR_BAG: return gText_MenuBag;
    case SCR_POKENAV: return gText_MenuPokenav;
    case SCR_CARD: return Ascii(CTR_TEXT("TRAINER CARD", "FICHA ENTREN.", "CARTE DRESSEUR", "SCHEDA ALLEN.", "CARTÃO TREIN.", "TRAINERPASS", "トレーナーカード"));
    case SCR_SAVE: return gText_MenuSave;
    case SCR_OPTION: return gText_MenuOption;
    case SCR_NUZLOCKE: return Ascii("NUZLOCKE");
    }
    return Ascii("");
}

/* The title plate's width: the Repel's room too while it has no plate. */
static int TitleWidth(const ViewState *s)
{
    return s->repelShown || s->flyNow ? TITLE_W : TITLE_WIDE_W;
}

/* The wild list's toggle is on the title, on the map with AREA WILD LIST on. */
static bool8 TitleToggles(const ViewState *s)
{
    return s->screen == SCR_MAP && s->encOn;
}

static void DrawTitle(const ViewState *s)
{
    u8 name[32];
    int tw = TitleWidth(s), oy;
    const u8 *title;
    u8 held = s->pressed >= HIT_COLUMN && s->pressed < HIT_COLUMN + SCR_COUNT ? s->pressed - HIT_COLUMN : SCR_COUNT;

    if (held < SCR_COUNT)
    {
        /* A dock button held: the section it opens, on emerald. */
        oy = Plate(TITLE_X, HEAD_Y, tw, HEAD_H, sEmerald, PLATE_STATE_NORMAL);
        TextIn(SectionName(held), TITLE_X, oy, tw, HEAD_H, &sNormal, 'c', STYLE_WHITE, 6);
        return;
    }
    if (s->screen == SCR_MAP)
    {
        u8 mapsec = s->pickMapsec != MAPSEC_NONE ? s->pickMapsec : s->mapsec;

        if (mapsec != MAPSEC_NONE)
        {
            GetMapName(name, mapsec, 0);
            title = name;
        }
        else
            title = SectionName(SCR_MAP);
    }
    else
        title = SectionName(s->screen < SCR_COUNT ? s->screen : SCR_MAP);
    if (TitleToggles(s))
    {
        u8 state = s->pressed == HIT_MAP_INFO ? PLATE_STATE_PRESSED
                 : s->focus == COL_TITLE ? PLATE_STATE_FOCUS : PLATE_STATE_NORMAL;

        oy = PlateEx(TITLE_X, HEAD_Y, tw, HEAD_H, s->encView ? sEmerald : sWildOff, state, tw - TITLE_GRASS_W - 4, 35,
                     PlateCut(HEAD_H), 0);
        TextIn(title, TITLE_X, oy, tw - TITLE_GRASS_W - 6, HEAD_H, &sNormal, 'l', STYLE_INK, 7);
        IconCentred(sArtGrass, 2, 2 * (TITLE_X + tw) - TITLE_GRASS_W + 2, 2 * oy + HEAD_H - 1, sArtPal2.c, FALSE);
        AddHit(TITLE_X, HEAD_Y - 2, tw, HEAD_H + 4, HIT_MAP_INFO);
        return;
    }
    oy = PlateEx(TITLE_X, HEAD_Y, tw, HEAD_H, sLight, PLATE_STATE_NORMAL, tw - 10, 30, PlateCut(HEAD_H), 0);
    TextIn(title, TITLE_X, oy, tw - 8, HEAD_H, &sNormal, 'l', STYLE_INK, 7);
}

/* The header's Y last drawn without its icon, not in yet: the whole screen
 * when it comes (CtrBottom_Frame). */
static bool8 sHeaderYMissing;

/* Y, the registered item (the 3DS's Y is SELECT): its icon on green, grey
 * with nothing registered; the Y badge on its corner. */
static void DrawHeaderY(const ViewState *s)
{
    bool8 available = s->registered != ITEM_NONE, pressed = available && s->pressed == HIT_COLUMN + COL_Y;
    u8 state = pressed ? PLATE_STATE_PRESSED : s->focus == COL_Y ? PLATE_STATE_FOCUS : PLATE_STATE_NORMAL;
    int oy = Plate(HEAD_Y_X, HEAD_Y, HEAD_Y_W, HEAD_H, !available ? sOffGrey : pressed ? sEmerald : sYGreen, state);
    int saved[4];

    if (!available && !sPlateWarm)
        sHeaderYMissing = FALSE;
    if (available)
    {
        int slot = ItemIcon(s->registered);

        if (!sPlateWarm)
            sHeaderYMissing = slot < 0;
        if (slot >= 0)
            IconCentred(sItemIcons[slot].tiles, 3, 2 * (HEAD_Y_X + 16), 2 * oy + HEAD_H, sItemIcons[slot].pal.c, FALSE);
        AddHit(0, HEAD_Y - 2, HEAD_Y_X + HEAD_Y_W + 1, HEAD_H + 4, HIT_COLUMN + COL_Y);
    }
    ClipPush(saved, 0, HEAD_Y - 3, 13, HEAD_Y + 8);
    if (available)
        DrawSprite(sArtBadgeY, 2, 2, 0, HEAD_Y - 3, sArtPal.c);
    else
    {
        u16 g[16];

        GreyPal(sArtPal.c, g);
        DrawSprite(sArtBadgeY, 2, 2, 0, HEAD_Y - 3, g);
    }
    ClipPop(saved);
}

/* The Repel's plate: its icon and the steps it has left, grey with none. */
static void DrawRepel(const ViewState *s)
{
    int oy, saved[4];

    if (!s->repelShown)
        return;
    oy = Plate(REPEL_X, HEAD_Y, REPEL_W, HEAD_H, sLight, PLATE_STATE_NORMAL);
    if (sRes.repel.tiles)
    {
        int x0, y0, x1, y1;
        u16 g[16];
        const u16 *pal = sRes.repel.pal.c;

        if (!s->repelSteps)
        {
            GreyPal(pal, g);
            pal = g;
        }
        /* Its visible pixels' box at (159, oy + 2), cut to the plate's rows. */
        SpriteBox(sRes.repel.tiles, 3, 3, &x0, &y0, &x1, &y1);
        ClipPush(saved, REPEL_X + 5, oy + 2, REPEL_X + REPEL_W, oy + HEAD_H - 1);
        DrawSprite(sRes.repel.tiles, 3, 3, REPEL_X + 5 - x0, oy + 2 - y0, pal);
        ClipPop(saved);
    }
    StyledRight(&sNormal, Number(s->repelSteps, 3, STR_CONV_MODE_LEFT_ALIGN), REPEL_X + REPEL_W - 7,
                TextY(oy, HEAD_H, &sNormal), s->repelSteps ? STYLE_INK : STYLE_GRAY);
}

/* The part of the day the clock is in: 0 morning, 1 day, 2 evening, 3 night. */
static unsigned ClockPart(const ViewState *s)
{
    if (s->clockShown)
        return s->clockPhase == CTR_TIME_NIGHT ? 3u : s->clockPhase == CTR_TIME_MORNING ? 0u
             : s->clockMinute >= 16u * 60u ? 2u : 1u;
    /* Without the day/night clock, by the hour alone. */
    return s->clockMinute < 6u * 60u || s->clockMinute >= 20u * 60u ? 3u : s->clockMinute >= 17u * 60u ? 2u : 1u;
}

/* The clock: the sun, the evening star or the moon, the time, and the clock's
 * speed with the day/night clock or SHOW CLOCK on. */
static void DrawClock(const ViewState *s)
{
    char time[8], speed[8];
    const u8 *icon;
    int oy = Plate(CLOCK_X, HEAD_Y, CLOCK_W, HEAD_H, sLight, PLATE_STATE_NORMAL);
    unsigned part = ClockPart(s);

    icon = part == 3 ? sArtMoon : part == 2 ? sArtStar : sArtSun;
    IconCentred(icon, 2, 2 * (CLOCK_X + 11), 2 * oy + HEAD_H - 1, sArtPal2.c, !s->clockValid);
    if (s->clockValid)
        snprintf(time, sizeof(time), "%02u:%02u", s->clockMinute / 60u, s->clockMinute % 60u);
    else
        strcpy(time, "--:--");
    StyledStr(&sNormal, Ascii(time), CLOCK_X + 21, TextY(oy, HEAD_H, &sNormal), STYLE_INK);
    if (!s->clockShown)
        return;
    if (s->clockValid)
        snprintf(speed, sizeof(speed), "%uX", s->clockRate);
    else
        strcpy(speed, "--");
    StyledRight(&sSmall, Ascii(speed), CLOCK_X + CLOCK_W - 7, TextY(oy, HEAD_H, &sSmall), STYLE_GREEN);
}

/* RUN: running by default. On, emerald with the shoe's speed lines and its
 * lamp lit; grey before the player has the shoes. */
static void DrawRun(const ViewState *s)
{
    bool8 available = (s->run & 2) != 0, on = available && (s->run & 1);
    bool8 pressed = available && s->pressed == HIT_COLUMN + COL_RUN;
    u8 state = pressed ? PLATE_STATE_PRESSED : s->focus == COL_RUN ? PLATE_STATE_FOCUS : PLATE_STATE_NORMAL;
    int oy = Plate(RUN_X, HEAD_Y, RUN_W, HEAD_H, !available ? sOffGrey : on || pressed ? sEmerald : sLight, state);

    IconCentred(on ? sArtShoeOn : sArtShoeOff, 3, 2 * RUN_X + RUN_W, 2 * oy + HEAD_H - 1, sArtPal.c, !available);
    if (on)
        FillRgb(RUN_X + RUN_W - 6, oy + 3, 2, 2, sLed);
    if (available)
        AddHit(RUN_X - 1, HEAD_Y - 2, W - RUN_X + 1, HEAD_H + 4, HIT_COLUMN + COL_RUN);
}

static void DrawFlyHeader(const ViewState *s);
static void DrawMapFlyButton(const ViewState *s);

static void DrawHeader(const ViewState *s)
{
    DrawHeaderY(s);
    if (s->mapFly)
    {
        DrawFlyHeader(s);
        return;
    }
    DrawTitle(s);
    /* The map's own FLY: where the Repel and the clock were. */
    if (s->flyNow)
        DrawMapFlyButton(s);
    else
    {
        DrawRepel(s);
        if (!s->inBattle)
            DrawClock(s);
    }
    DrawRun(s);
}

/* The header's parts on screen, shadows included: for partial redraws. */
static void TitleRect(const ViewState *s, int *x0, int *y0, int *x1, int *y1)
{
    PlateBounds(TITLE_X, HEAD_Y, TitleWidth(s), HEAD_H, x0, y0, x1, y1);
}

static void RepelRect(int *x0, int *y0, int *x1, int *y1)
{
    PlateBounds(REPEL_X, HEAD_Y, REPEL_W, HEAD_H, x0, y0, x1, y1);
}

static void ClockRect(int *x0, int *y0, int *x1, int *y1)
{
    PlateBounds(CLOCK_X, HEAD_Y, CLOCK_W, HEAD_H, x0, y0, x1, y1);
}

/* ------------------------------------------------------------------------ */
/* The backdrop                                                             */
/* ------------------------------------------------------------------------ */

/*
 * The menu's backdrop, romfs/bottom/menu.bin (scripts/gen_menu_art.py): the
 * battle art's format, one element the size of the screen in the canvas'
 * layout. Without it, its gradient alone, so the screen still works.
 */
static bool8 LoadMenuBackdrop(u16 *dst)
{
    FILE *f = fopen("romfs:/bottom/menu.bin", "rb");
    long size = 0;
    u8 *data = NULL;
    bool8 ok = FALSE;

    if (f)
    {
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fseek(f, 0, SEEK_SET);
        data = size > 0 ? malloc(size) : NULL;
        if (data && fread(data, 1, size, f) != (size_t)size)
            size = 0;
        fclose(f);
    }

    if (data && size >= (long)(10 + 10 + sizeof(u16) * W * H) && memcmp(data, "EM3DMNU1", 8) == 0)
    {
        const u8 *e = data + 10;

        if ((e[2] | (e[3] << 8)) == 0 && (e[4] | (e[5] << 8)) == 0 && (e[6] | (e[7] << 8)) == W
         && (e[8] | (e[9] << 8)) == H)
        {
            memcpy(dst, e + 10, sizeof(u16) * W * H);
            ok = TRUE;
        }
    }
    free(data);
    if (!ok)
    {
        CtrLog_Write(CTR_LOG_ERROR, "bottom: romfs/bottom/menu.bin missing or bad, plain backdrop");
        for (int x = 0; x < W; ++x)
            for (int y = 0; y < H; ++y)
            {
                int t = y * 255 / (H - 1);
                u16 c = y < 11 || y >= DOCK_Y ? PackRgb(18, 52, 40)
                      : PackRgb(168 - 50 * t / 255, 226 - 34 * t / 255, 190 - 42 * t / 255);

                dst[x * H + (H - 1 - y)] = c;
            }
    }
    return ok;
}
