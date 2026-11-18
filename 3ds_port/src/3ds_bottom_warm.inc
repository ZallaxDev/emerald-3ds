/* Part of 3ds_bottom_ui.c: the sections' and screens' plates made ahead in idle frames. */

/*
 * Plates made ahead. The first time a section or one of the game's screens
 * is drawn, each of its plates is worked out pixel by pixel: 50-100 ms in one
 * frame in Azahar for the party menu, the bag, the summary or the card, more
 * on an Old 3DS. While the field is idle and a frame has time left, the
 * sections the dock can open are walked with nothing to draw into, so only
 * their missing plate pictures are made (sPlateWarm, WarmPlate): one at a
 * time within what the frame leaves, only into room free now - a plate made
 * already is never given up for it. The game's own screens are walked as
 * they were last shown (sWarmSeen); the party menu also before it ever has
 * been, from the party itself. Labels are left to the redraw.
 * A battle's party menu, summary and bag are drawn with their dock locked
 * and the header without Y, RUN or the Repel: plates of their own, made ahead
 * in a battle's idle frames once its menus are (WarmBattleMenus), from the
 * battle's screens as last shown, the party menu from the party.
 */
#define PLATE_WARM_MS 4.0f
static ViewState sWarmSeen[NATIVE_BLANK], sWarmSeenBattle[NATIVE_BLANK];
static bool8 sWarmSeenOk[NATIVE_BLANK], sWarmSeenBattleOk[NATIVE_BLANK];
enum { WARM_POKEMON, WARM_BAG, WARM_SUMMARY, WARM_DEX, WARM_CARD, WARM_SAVE, WARM_OPTION, WARM_NUZLOCKE, WARM_NAMING, WARM_CHAT,
       WARM_COUNT };
static bool8 sWarmScreenDone[WARM_COUNT], sWarmBattleDone[WARM_COUNT];
static u8 sWarmNext;
static u32 sWarmEvicted = 0xFFFFFFFF;

/* What the screen `which` would show, opened now (in a battle, from its
 * menu); FALSE for none to walk. */
static bool8 WarmScreenView(u8 which, ViewState *v, bool8 battle)
{
    static const u8 screens[WARM_COUNT] = {SCR_POKEMON, SCR_BAG, SCR_POKEMON, SCR_POKEDEX, SCR_CARD, SCR_SAVE,
                                           SCR_OPTION, SCR_NUZLOCKE};
    static const u8 natives[WARM_COUNT] = {NATIVE_PARTY, NATIVE_BAG, NATIVE_SUMMARY, NATIVE_DEX};
    u8 screen, keep = sScreen;

    /* The naming screen's keyboard and the chat's, from nothing seen yet. */
    if (which >= WARM_NAMING)
    {
        if (battle)
            return FALSE;
        WarmKeyboardView(v, which == WARM_CHAT);
        return TRUE;
    }
    screen = screens[which];
    if (battle)
    {
        u8 native;

        if (which > WARM_SUMMARY || sClassicMenus || !(EnabledScreens() & (1 << screen)))
            return FALSE;
        native = natives[which];
        if (sWarmSeenBattleOk[native])
        {
            *v = sWarmSeenBattle[native];
            v->pressed = HIT_NONE;
            v->focus = FOCUS_NONE;
            return TRUE;
        }
        if (native != NATIVE_PARTY)
            return FALSE;
        /* As Snapshot has a battle's party menu: in its section, only its
         * own button on the dock. */
        sScreen = screen;
        Snapshot(v, MODE_FIELD, HIT_NONE);
        sScreen = keep;
        v->focus = FOCUS_NONE;
        v->screen = screen;
        v->dock = v->dockLocked = TRUE;
        v->registered = ITEM_NONE;
        v->run = 0;
        v->mode = MODE_PARTY_MENU;
        v->native = NATIVE_PARTY;
        v->bagView = BAG_VIEW_SECTION;
        SnapshotPartyFull(v);
        memset(&v->pv, 0, sizeof(v->pv));
        v->pv.state = CTR_PARTY_VIEW_LIST;
        v->pv.moving = v->pv.chosen = -1;
        memset(v->pv.desc, CTR_PARTY_DESC_NONE, sizeof(v->pv.desc));
        return TRUE;
    }

    if (!(EnabledScreens() & (1 << screen)) || screen == sScreen)
        return FALSE;
    if (which <= WARM_DEX)
    {
        u8 native = natives[which];

        if (sClassicMenus)
            return FALSE;
        if (sWarmSeenOk[native])
        {
            *v = sWarmSeen[native];
            v->pressed = HIT_NONE;
            v->focus = FOCUS_NONE;
            return TRUE;
        }
        if (native != NATIVE_PARTY)
            return FALSE;
    }
    if (which == WARM_CARD && sClassicMenus)
        return FALSE;
    sScreen = screen;
    Snapshot(v, MODE_FIELD, HIT_NONE);
    sScreen = keep;
    v->focus = FOCUS_NONE;
    if (which == WARM_POKEMON)
    {
        /* The party menu as it opens: its list, the cursor on the first. */
        v->mode = MODE_PARTY_MENU;
        v->native = NATIVE_PARTY;
        v->bagView = BAG_VIEW_SECTION;
        SnapshotPartyFull(v);
        memset(&v->pv, 0, sizeof(v->pv));
        v->pv.state = CTR_PARTY_VIEW_LIST;
        v->pv.moving = v->pv.chosen = -1;
        memset(v->pv.desc, CTR_PARTY_DESC_NONE, sizeof(v->pv.desc));
    }
    return v->dock;
}

/* One idle frame's walk, the field's or a battle's: the next screen not done
 * yet, then the next. */
static void WarmScreens(bool8 battle, u8 first, u8 last)
{
    bool8 *done = battle ? sWarmBattleDone : sWarmScreenDone;
    static ViewState v;
    int ox = sOX, hits = sHitCount, clip[4] = {sClipX0, sClipY0, sClipX1, sClipY1};
    bool8 refine = sRefineLabels, worked = sRedrawLabelWorked, bad = sCompBad, icons = sIconBudget;
    u32 misses = sLabelMisses;
    float slice;

    if (sPlatesEvicted != sWarmEvicted)
    {
        /* Plates given up since: the walks are worth making again. */
        sWarmEvicted = sPlatesEvicted;
        memset(sWarmScreenDone, 0, sizeof(sWarmScreenDone));
        memset(sWarmBattleDone, 0, sizeof(sWarmBattleDone));
        WarmKeyboardRestart();
    }
    /* What the last frame's work left, as for the battle menus, up to
     * PLATE_WARM_MS: a plate that costs more waits for a frame with room. */
    {
        const CtrTiming *timing = CtrPlatform_GetTiming();

        slice = WARM_FRAME_MS - (timing->workMs - timing->bottomMs) - WARM_MARGIN_MS;
        if (slice > PLATE_WARM_MS)
            slice = PLATE_WARM_MS;
        if (slice < WARM_MIN_MS)
            return;
    }
    for (int tries = 0; tries < WARM_COUNT; ++tries)
    {
        u8 which = sWarmNext;

        sWarmNext = (sWarmNext + 1) % WARM_COUNT;
        if (done[which] || which < first || which > last)
            continue;
        if (!WarmScreenView(which, &v, battle))
        {
            done[which] = TRUE;
            continue;
        }
        ResolveFonts();
        sOX = 0;
        sClipX0 = sClipY0 = sClipX1 = sClipY1 = 0;
        sBuildBudget = 0;
        sIconBudget = FALSE;
        sPlateWarm = TRUE;
        sPlateWarmStart = CtrPlatform_Ticks();
        sPlateWarmMs = slice;
        sPlateWarmShort = sPlateWarmFull = FALSE;
        sPlateWarmBuilt = 0;
        sPlateJobSeen = FALSE;
        if (v.mode == MODE_NAMING)
        {
            if (v.chat)
                DrawGameChat(&v);
            else
                DrawGameNaming(&v);
        }
        else if (v.native)
            DrawGameView(&v);
        else if (v.screen == SCR_CARD)
            DrawTrainerCard(&v);
        else if (v.screen == SCR_SAVE)
            DrawSave(&v);
        else if (v.screen == SCR_OPTION)
            DrawOptions(&v);
        else if (v.screen == SCR_NUZLOCKE)
            DrawNuzlocke(&v);
        if (v.mode != MODE_NAMING)
        {
            DrawHeader(&v);
            DrawDock(&v);
        }
        sPlateWarm = FALSE;
        /* A plate begun for a screen that no longer has it goes; one still
         * being made keeps the walk on its screen. */
        if (sPlateJob.active && !sPlateJobSeen)
            PlateJobDrop();
        if (sPlateJob.active)
            sWarmNext = which;
        /* Nothing missing that fits the cache: this screen is ready. */
        if (!sPlateWarmShort)
            done[which] = TRUE;
        /* The keyboards are walked a view at a time: ready with the last (WarmKeyboardView). */
        if (!sPlateWarmShort && v.mode == MODE_NAMING && WarmKeyboardAdvance(v.chat))
            done[which] = FALSE;
        if (sPlateWarmBuilt)
            CtrLog_Write(CTR_LOG_VIDEO, "bottom screen: %u plates made ahead for screen %u%s", sPlateWarmBuilt,
                         v.screen, sPlateWarmFull ? " (cache full)" : "");
        break;
    }
    sBuildBudget = 0xFF;
    sIconBudget = icons;
    sRefineLabels = refine;
    sRedrawLabelWorked = worked;
    sCompBad = bad;
    sLabelMisses = misses;
    sOX = ox;
    sHitCount = hits;
    sClipX0 = clip[0];
    sClipY0 = clip[1];
    sClipX1 = clip[2];
    sClipY1 = clip[3];
}
