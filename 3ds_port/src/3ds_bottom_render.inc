/* Part of 3ds_bottom_ui.c: the backgrounds kept and the whole redraw. */

/* ------------------------------------------------------------------------ */
/* Redraw and present                                                       */
/* ------------------------------------------------------------------------ */

static void BuildBackgroundCaches(void)
{
    sOX = 0;
    /* The menu's backdrop; the map's window on it; the game's window on it,
     * black inside until the game's picture is there. A battle without its
     * art keeps the party menu's background. */
    LoadMenuBackdrop(sCache[CACHE_MENU]);
    sDst = sCache[CACHE_WIDE];
    DrawPartyBackground(W / 8, H / 8);
    BuildMapCache(FALSE, 0, 0);
    if (sCache[CACHE_GAME])
    {
        memcpy(sCache[CACHE_GAME], sCache[CACHE_MENU], sizeof(sCanvas));
        sDst = sCache[CACHE_GAME];
        DrawChamferFrame(GAME_WIN_X, GAME_WIN_Y, GAME_WIN_W, GAME_WIN_H, 4, 2);
        FillRect(CTR_BOTTOM_WIN_X, CTR_BOTTOM_WIN_Y, 240, 160, 0);
    }
    sDst = sCanvas;
}

/* The section's backdrop: the map's window, the game's, or the plain one. */
static int SectionCache(const ViewState *s)
{
    if (s->native)
        return CACHE_MENU;
    switch (s->screen)
    {
    case SCR_MAP:
        return s->encView ? CACHE_MENU : CACHE_MAP;
    case SCR_POKEDEX:
    case SCR_POKEMON:
    case SCR_BAG:
        /* Drawn here without the classic menus: the plain backdrop while
         * they open, never the game's black window. */
        if (!sClassicMenus)
            return CACHE_MENU;
        /* fall through */
    case SCR_POKENAV:
        /* Still in the field, on its way: the backdrop and its icon. */
        if (s->mode == MODE_FIELD)
            return CACHE_MENU;
        return sCache[CACHE_GAME] ? CACHE_GAME : CACHE_MENU;
    case SCR_CARD:
        /* The game's card in the window, with the classic menus. */
        if (s->mode == MODE_CARD)
            return sCache[CACHE_GAME] ? CACHE_GAME : CACHE_MENU;
        break;
    }
    return CACHE_MENU;
}

/* A part of the canvas to the screen; with the game's picture in its window,
 * everything round the window and nothing of it. */
static void BlitAroundWindow(int x0, int y0, int x1, int y1)
{
    const int wx0 = CTR_BOTTOM_WIN_X, wy0 = CTR_BOTTOM_WIN_Y, wx1 = wx0 + 240, wy1 = wy0 + 160;
    int my0, my1;

    CtrVideo_BottomStrips(sCanvas);
    if (!CtrVideo_BottomInUse() || sState.native || x1 <= wx0 || x0 >= wx1 || y1 <= wy0 || y0 >= wy1)
    {
        if (x0 == 0 && y0 == 0 && x1 == W && y1 == H)
            CtrBottom_Blit(sCanvas, 0, W);
        else
            CtrBottom_BlitRect(sCanvas, x0, y0, x1, y1);
        return;
    }
    my0 = y0 > wy0 ? y0 : wy0;
    my1 = y1 < wy1 ? y1 : wy1;
    if (y0 < wy0)
        CtrBottom_BlitRect(sCanvas, x0, y0, x1, wy0);
    if (y1 > wy1)
        CtrBottom_BlitRect(sCanvas, x0, wy1, x1, y1);
    if (x0 < wx0)
        CtrBottom_BlitRect(sCanvas, x0, my0, wx0, my1);
    if (x1 > wx1)
        CtrBottom_BlitRect(sCanvas, wx1, my0, x1, my1);
}

/* The battle's entrance (BattleIntro, below). */
static struct
{
    u8 phase, step;
    bool8 defer;
} sIntro;

/*
 * Under the title screen: the developer's logo in the bottom-left corner, the
 * build's id above it (engine/build_id.bin: two systems with the same one can
 * play together over local wireless) and the build's version (CTR_APP_VERSION,
 * the Makefile's APP_VERSION) in the bottom-right corner, all in the logo's
 * own grey over black.
 */
#ifndef CTR_APP_VERSION
#define CTR_APP_VERSION "dev"
#endif
#define TITLE_MARGIN_X 16
#define TITLE_MARGIN_Y 6
#define TITLE_GREY 100   /* the logo's coverage at its strongest */

static void DrawTitleCredit(void)
{
    int x0 = TITLE_MARGIN_X, y0 = H - TITLE_MARGIN_Y - ART_LOGO_H, ink = ART_LOGO_W;
    u16 grey = PackRgb(TITLE_GREY, TITLE_GREY, TITLE_GREY);

    for (int y = 0; y < ART_LOGO_H; ++y)
        for (int x = 0; x < ART_LOGO_W; ++x)
        {
            u8 a = sArtLogo[y * ART_LOGO_W + x];

            if (a)
            {
                Put(x0 + x, y0 + y, PackRgb(a, a, a));
                if (x < ink)
                    ink = x;
            }
        }
    /* Its capitals level with the logo's lettering (the logo's rows 8 to 20). */
    {
        const u8 *str = Ascii("v" CTR_APP_VERSION);
        int ix0, ix1, top, bottom;

        if (!sNormal.glyphs)
            return;
        InkColumns(&sNormal, str, &ix0, &ix1);
        CapRows(&sNormal, &top, &bottom);
        DrawStr(&sNormal, str, W - TITLE_MARGIN_X - ix1, y0 + 8 + (12 - (bottom - top)) / 2 - top, grey, 0);
    }
    /* The id's capitals 4 pixels over the logo, its first ink on the logo's. */
    if (LocalLink_BuildName() && sSmall.glyphs)
    {
        char text[40];
        const u8 *str;
        int ix0, ix1, top, bottom;

        snprintf(text, sizeof(text), "%s %s",
                 CTR_TEXT("Build", "Compilación", "Build", "Build", "Build", "Build", "ビルド"),
                 LocalLink_BuildName());
        str = Ascii(text);
        InkColumns(&sSmall, str, &ix0, &ix1);
        CapRows(&sSmall, &top, &bottom);
        DrawStr(&sSmall, str, x0 + ink - ix0, y0 - 4 - bottom, grey, 0);
    }
}

/*
 * The sections drawn here (map, card, save, options, Nuzlocke) drawn whole, kept with
 * the state they were drawn from. Back from a battle - or from anything that
 * leaves the screen as it was - it is put back instead of drawn again: 10-15 ms in one frame on an Old 3DS, a dropped frame
 * at the end of every battle (F0). Only a screen with nothing animated, every
 * label finished and every icon in, so the copy is what Render would draw;
 * CTR_BOTTOM_CACHE_AUDIT draws it anyway and logs a difference.
 *
 * The copy is taken as a battle opens (MapCacheTake), not after each drawing:
 * on an Old 3DS copying the whole screen at every redraw made the first visit
 * to a screen 3 ms slower than it saved coming back.
 */
static struct
{
    bool8 valid;
    ViewState state;
    u16 canvas[W * H];
    Hit hits[ARRAY_COUNT(sHits)];
    u8 hitCount;
} sMapCache;
/* The canvas is a whole drawing of a cacheable screen, nothing animated, every
 * label and icon in: set by the last whole Render, kept by the partial ones
 * (they keep the canvas what a whole drawing of the new state would be). */
static bool8 sMapShotOk;

static bool8 MapCacheable(const ViewState *s)
{
    return s->mode == MODE_FIELD && s->dock && !s->fm.count && !s->fl.count
        && (s->screen == SCR_MAP || s->screen == SCR_CARD || s->screen == SCR_SAVE || s->screen == SCR_OPTION
            || s->screen == SCR_NUZLOCKE)
        && !CtrVideo_BottomInUse() && !CtrVideo_BottomWhole() && ClipIsFull();
}

/* The screen on show, kept for when the battle that opens now is over. */
static void MapCacheTake(void)
{
    if (!sMapShotOk || !MapCacheable(&sShown) || sRefineLabels || sForceRedraw)
        return;
    sMapCache.state = sShown;
    memcpy(sMapCache.canvas, sCanvas, sizeof(sCanvas));
    memcpy(sMapCache.hits, sHits, sizeof(sHits));
    sMapCache.hitCount = sHitCount;
    sMapCache.valid = TRUE;
}

/*
 * A whole redraw over CTR_RENDER_SLOW_MS, phase by phase, for the console's
 * log: Azahar copies memory far faster than an Old 3DS, so where a 10 ms
 * redraw goes can only be read there. CTR_RENDER_SLOW_LINES of them for the
 * field, again for each battle and the field after it.
 */
#define CTR_RENDER_SLOW_MS 4.0f
#define CTR_RENDER_SLOW_LINES 60

static void Render(const ViewState *s)
{
    bool8 cacheable = MapCacheable(s), blitted = FALSE;
    uint64_t phase[6] = {CtrPlatform_Ticks()};
    u32 platesAt = sPlatesMade, evictedAt = sPlatesEvicted;
    uint64_t plateTicksAt = sPlateTicks, drawTicksAt = sPlateDrawTicks, textTicksAt = sTextTicks;

    ResolveFonts();
    if (cacheable && sMapCache.valid && memcmp(&sMapCache.state, s, sizeof(*s)) == 0)
    {
#ifdef CTR_BOTTOM_CACHE_AUDIT
        static u16 kept[W * H];
        static Hit keptHits[ARRAY_COUNT(sHits)];
        u8 keptCount = sMapCache.hitCount;
        static unsigned hits;

        memcpy(kept, sMapCache.canvas, sizeof(kept));
        memcpy(keptHits, sMapCache.hits, sizeof(keptHits));
        sMapCache.valid = FALSE;
        Render(s);
        if (memcmp(kept, sCanvas, sizeof(kept)) != 0 || sHitCount != keptCount
            || memcmp(sHits, keptHits, sHitCount * sizeof(Hit)) != 0)
            CtrLog_Write(CTR_LOG_ERROR, "CONTRACT bottom map cache differs from a redraw");
        else
            CtrLog_Write(CTR_LOG_VIDEO, "bottom map cache: same as a redraw (%u)", ++hits);
        return;
#else
        memcpy(sCanvas, sMapCache.canvas, sizeof(sCanvas));
        memcpy(sHits, sMapCache.hits, sizeof(sHits));
        sHitCount = sMapCache.hitCount;
        sDst = sCanvas;
        sOX = 0;
        if (!sIntro.defer)
            CtrBottom_Blit(sCanvas, 0, W);
        sMapShotOk = TRUE;
        return;
#endif
    }
    sHitCount = 0;
    sDst = sCanvas;
    sOX = 0;

    if (s->mode == MODE_OFF && (s->fm.count || s->fl.count))
    {
        CopyCache(CACHE_MENU);
        phase[1] = CtrPlatform_Ticks();
        NameBackdrop();
        if (s->fl.count)
            DrawFieldList(s);
        else
            DrawFieldMenu(s);
        phase[2] = CtrPlatform_Ticks();
    }
    else if (s->mode == MODE_OFF && s->dim)
    {
        CopyCache(CACHE_MENU);
        phase[1] = CtrPlatform_Ticks();
        IdleBackdrop();
        phase[2] = CtrPlatform_Ticks();
    }
    else if (s->mode == MODE_OFF)
    {
        memset(sCanvas, 0, sizeof(sCanvas));
        phase[1] = CtrPlatform_Ticks();
        if (s->title)
            DrawTitleCredit();
        phase[2] = CtrPlatform_Ticks();
    }
    else if (s->mode >= MODE_BATTLE_INFO)
    {
        CopyCache(sCache[CACHE_BATTLE] ? CACHE_BATTLE : CACHE_WIDE);
        phase[1] = CtrPlatform_Ticks();
        if (s->mode == MODE_BATTLE_ACTION)
            DrawBattleActions(s);
        else if (s->mode == MODE_BATTLE_MOVE)
            DrawBattleMoves(s);
        else if (s->mode == MODE_BATTLE_TARGET)
            DrawBattleTarget(s);
        phase[2] = CtrPlatform_Ticks();
    }
    else if (s->mode == MODE_PAD)
    {
        CopyCache(CACHE_MENU);
        phase[1] = CtrPlatform_Ticks();
        if (s->fl.count || s->fm.count)
        {
            NameBackdrop();
            if (s->fl.count)
                DrawFieldList(s);
            else
                DrawFieldMenu(s);
        }
        else if (s->bl.state)
            DrawBlender(s);
        else if (s->co.state)
            DrawContest(s);
        else if (s->ec.state)
            DrawEasyChat(s);
        else if (s->fac.screen)
            DrawFactory(s);
        else
            IdleBackdrop();
        phase[2] = CtrPlatform_Ticks();
    }
    else if (s->mode == MODE_NAMING)
    {
        CopyCache(CACHE_MENU);
        phase[1] = CtrPlatform_Ticks();
        if (s->chat)
            DrawGameChat(s);
        else
            DrawGameNaming(s);
        phase[2] = CtrPlatform_Ticks();
    }
    else if (!s->dock)
    {
        /* The game's screen opened from a battle, a shop or the PC has the
         * whole screen: black until it is drawn there. */
        FillRect(0, 0, W, H, 0);
        phase[1] = phase[2] = CtrPlatform_Ticks();
    }
    else
    {
        if (!s->native && s->screen == SCR_MAP && !s->encView && !s->fm.count && !s->fl.count)
            EnsureMapCache(s);
        CopyCache(s->fm.count || s->fl.count ? CACHE_MENU : SectionCache(s));
        phase[1] = CtrPlatform_Ticks();
        if (s->fl.count)
            DrawFieldList(s);
        else if (s->fm.count)
            DrawFieldMenu(s);
        else if (s->native)
            DrawGameView(s);
        else switch (s->screen)
        {
        case SCR_MAP: DrawRegionMap(s); break;
        case SCR_CARD:
            if (s->mode != MODE_CARD)
                DrawTrainerCard(s);
            break;
        case SCR_POKEDEX:
        case SCR_POKEMON:
        case SCR_BAG:
        case SCR_POKENAV:
            if ((!sClassicMenus && s->screen != SCR_POKENAV) || s->mode == MODE_FIELD)
                DrawSectionWait(s);
            break;
        case SCR_SAVE: DrawSave(s); break;
        case SCR_OPTION: DrawOptions(s); break;
        case SCR_NUZLOCKE: DrawNuzlocke(s); break;
        }
        /* The game's party menu, bag, Pokédex and PokéNav are drawn in their
         * window by the compositor; black until they are, as they fade in
         * from black, and while the field is on its way to opening them
         * (OpenAsked): the cache's. */
        phase[2] = CtrPlatform_Ticks();
        DrawHeader(s);
        DrawDock(s);
    }
    phase[3] = CtrPlatform_Ticks();
    phase[4] = CtrPlatform_Ticks();
    /* The game's window is the compositor's while it draws there, and the
     * whole screen the boxes'. */
    if (!ClipIsFull())
    {
        BlitAroundWindow(sClipX0, sClipY0, sClipX1, sClipY1);
        blitted = TRUE;
    }
    else if (!CtrVideo_BottomWhole() && !sIntro.defer)
    {
        BlitAroundWindow(0, 0, W, H);
        blitted = TRUE;
    }
    phase[5] = CtrPlatform_Ticks();
    if (ClipIsFull())
        sMapShotOk = cacheable && !sRefineLabels && sIconsIn;
    /* Whole redraws and partial ones alike (a page of the options is one):
     * the backdrop's copy (with the card's cache finished in it), and the
     * present split into its copy to the screen and its cache flush. */
    {
        static unsigned lines;
        static bool8 battle;
        float total = CtrPlatform_TickMs(phase[5] - phase[0]);
        uint64_t copy = 0, flush = 0;

        /* Each battle, and the field after it, has lines of its own. */
        if (s->inBattle != battle)
        {
            battle = s->inBattle;
            lines = 0;
        }

        if (total > CTR_RENDER_SLOW_MS && lines < CTR_RENDER_SLOW_LINES)
        {
            if (blitted)
                CtrBottom_LastBlitTicks(&copy, &flush);
            ++lines;
            CtrLog_Write(CTR_LOG_VIDEO, "bottom render f=%lu mode=%u screen=%u page=%u part=%d,%d-%d,%d total=%.2f: "
                         "backdrop=%.2f screen=%.2f frame=%.2f icons=%.2f blit=%.2f "
                         "(copy=%.2f flush=%.2f) ms plates made=%lu in %.2f ms evicted=%lu; drawn plates=%.2f "
                         "text=%.2f ms",
                         (unsigned long)sFrames, s->mode, s->screen, s->optPage, sClipX0, sClipY0, sClipX1, sClipY1,
                         total, CtrPlatform_TickMs(phase[1] - phase[0]),
                         CtrPlatform_TickMs(phase[2] - phase[1]), CtrPlatform_TickMs(phase[3] - phase[2]),
                         CtrPlatform_TickMs(phase[4] - phase[3]), CtrPlatform_TickMs(phase[5] - phase[4]),
                         CtrPlatform_TickMs(copy), CtrPlatform_TickMs(flush), (unsigned long)(sPlatesMade - platesAt),
                         CtrPlatform_TickMs(sPlateTicks - plateTicksAt), (unsigned long)(sPlatesEvicted - evictedAt),
                         CtrPlatform_TickMs(sPlateDrawTicks - drawTicksAt), CtrPlatform_TickMs(sTextTicks - textTicksAt));
        }
    }
}
