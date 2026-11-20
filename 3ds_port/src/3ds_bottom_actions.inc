/* Part of 3ds_bottom_ui.c: a game's screen opened from the dock, a tap carried out, the touch itself. */

/*
 * The game's bag, party menu or Pokédex for BAG, POKéMON or POKéDEX, opened from the field as the
 * start menu opens it. Chosen while another screen was up - the Pokédex, the
 * bag, the party menu, the PokéNav - it opens once that one has closed and
 * the field is idle again (OpenAsked); until then the area is black.
 */
static bool8 OpenGameScreen(u8 screen)
{
    if (!FieldIdle() || !CtrStartMenu_Available())
        return FALSE;
    StartPlan(PLAN_START, screen == SCR_BAG ? START_BAG : screen == SCR_POKEMON ? START_POKEMON
                        : screen == SCR_POKENAV ? START_POKENAV : screen == SCR_CARD ? START_PLAYER : START_POKEDEX);
    BeginSession(FALSE);
    return TRUE;
}

/* The screens the game draws in the window, opened by OpenAsked. */
static bool8 AskedScreen(u8 screen)
{
    return screen == SCR_BAG || screen == SCR_POKEDEX || screen == SCR_POKEMON || screen == SCR_POKENAV
        || (screen == SCR_CARD && sClassicMenus);
}

static void OpenAsked(u8 mode)
{
    static u8 lastMode = MODE_OFF;
    static u16 waited;

    /* Closed by its own button or B: back to the map, not opened again. */
    if (mode != lastMode)
    {
        if ((lastMode == MODE_BAG_MENU && sScreen == SCR_BAG) || (lastMode == MODE_POKEDEX && sScreen == SCR_POKEDEX)
         || (lastMode == MODE_PARTY_MENU && sScreen == SCR_POKEMON) || (lastMode == MODE_POKENAV && sScreen == SCR_POKENAV)
         || (lastMode == MODE_CARD && sScreen == SCR_CARD && sClassicMenus))
            sScreen = SCR_MAP;
        lastMode = mode;
        waited = 0;
    }
    /* A live browser is not a START menu. Only its explicit ACTIONS request
     * starts the original menu, after walking ends and while the field is safe. */
    if (sLivePartyPending != PARTY_SIZE)
    {
        if (mode != MODE_FIELD || sScreen != SCR_POKEMON || sClassicMenus
         || CtrExtras_Get("live_party") != 1 || gMain.callback2 != CB2_Overworld
         || ScriptContext_IsEnabled() || ArePlayerFieldControlsLocked() || gPaletteFade.active)
            sLivePartyPending = PARTY_SIZE;
        else if (FieldIdle() && CtrStartMenu_Available() && OpenGameScreen(SCR_POKEMON))
        {
            sLivePartyHandoff = sLivePartyPending;
            sLivePartyHandoffFrames = 0;
            sLivePartyPending = PARTY_SIZE;
        }
    }
    if (mode == MODE_FIELD && sScreen == SCR_POKEMON && !sClassicMenus
     && CtrExtras_Get("live_party") == 1)
        return;
    if (mode != MODE_FIELD || !AskedScreen(sScreen) || sSession.active || sPlan.kind != PLAN_NONE)
        return;
    if (!(EnabledScreens() & (1 << sScreen)))
        sScreen = SCR_MAP;
    else if (OpenGameScreen(sScreen))
        waited = 0;
    /* Two seconds without the field coming back idle: given up. */
    else if (++waited > 120)
        sScreen = SCR_MAP;
}

/* The game menu is now ready: send exactly one selection to its own handler,
 * preserving its animations, checks and later actions. Never force a menu
 * input during a battle, script or a different screen. */
static void CompleteLivePartyHandoff(u8 mode)
{
    if (sLivePartyHandoff == PARTY_SIZE)
        return;
    if (mode == MODE_PARTY_MENU)
    {
        struct CtrPartyView v;

        if (CtrParty_InWindow() && CtrParty_GetView(&v) && v.state == CTR_PARTY_VIEW_LIST
         && sLivePartyHandoff < gPlayerPartyCount)
        {
            CtrParty_TapSlot(sLivePartyHandoff);
            sLivePartyHandoff = PARTY_SIZE;
            return;
        }
    }
    if (mode == MODE_OFF || mode >= MODE_BATTLE_INFO || ++sLivePartyHandoffFrames > 180)
        sLivePartyHandoff = PARTY_SIZE;
}

/* The passive view only changes a selection. ACTIONS enters the real menu;
 * no bag, party or event data are mutated by touching the live snapshot. */
static void LivePartyTap(u8 id)
{
    if (id >= HIT_NAT_SLOT && id < HIT_NAT_SLOT + PARTY_SIZE)
    {
        u8 slot = id - HIT_NAT_SLOT;

        if (slot < gPlayerPartyCount && GetMonData(&gPlayerParty[slot], MON_DATA_SPECIES) != SPECIES_NONE)
        {
            sLivePartyCursor = slot;
            PlaySE(SE_SELECT);
        }
    }
    else if (id == HIT_CANCEL && gPlayerPartyCount && sLivePartyCursor < gPlayerPartyCount)
    {
        sLivePartyPending = sLivePartyCursor;
        PlaySE(SE_SELECT);
    }
}

/* The dock's buttons for a screen of the game's own, which takes the buttons
 * itself once open. */
static bool8 IsGameScreenItem(u8 item)
{
    return item == SCR_POKEMON || item == SCR_BAG || item == SCR_POKEDEX || item == SCR_POKENAV
        || (item == SCR_CARD && sClassicMenus);
}

static void LeaveFocus(void);

static void Activate(u8 id, u8 mode)
{
    if (id == HIT_NONE)
        return;
    CtrLog_Write(CTR_LOG_INPUT, "bottom screen: tap %02x (mode %u screen %u)", id, mode, sShown.screen);
    /* The field's map and dock still up while another screen of the game is on its way: nothing of
     * theirs is to be had from there. */
    if (mode == MODE_FIELD && gMain.callback2 != CB2_Overworld)
        return;
    /* The contest's move choice: its plates are taps. */
    if (mode == MODE_PAD && sShown.co.state && !sShown.fm.count && !sShown.fl.count)
    {
        ContestTap(id);
        return;
    }
    /* Easy Chat: its buttons are taps, carried out as the keys the game reads. */
    if (mode == MODE_PAD && sShown.ec.state && !sShown.fm.count && !sShown.fl.count)
    {
        EasyChatTap(id);
        return;
    }
    /* The Rental Pokemon screens: carried out as the keys the game reads. */
    if (mode == MODE_PAD && sShown.fac.screen && !sShown.fm.count && !sShown.fl.count)
    {
        FactoryTap(id);
        return;
    }
    /* The buttons act as they are held (PadKeysHeld), not as a tap. */
    if (mode == MODE_PAD && !sShown.fm.count && !sShown.fl.count)
        return;
    /* A menu or a list of the game's waiting: its buttons are the only things to tap. */
    if ((mode == MODE_FIELD || mode == MODE_PAD || mode == MODE_OFF) && (sShown.fm.count || sShown.fl.count))
    {
        if (sShown.fl.count)
            FieldListTap(&sShown, id);
        else
            FieldMenuTap(id);
        return;
    }
    if (mode == MODE_NAMING)
    {
        if (sShown.chat)
            ChatTap(id);
        else
            NamingTap(id);
        return;
    }
    if (mode >= MODE_BATTLE_INFO)
    {
        /* The controller waiting for it takes it on its next frame. */
        if (mode != MODE_BATTLE_INFO)
            sBattleTap = id;
        return;
    }

    /* Y: SELECT in the field, the registered item. RUN: running by default. */
    if (id == HIT_COLUMN + COL_Y)
    {
        if (mode == MODE_FIELD && FieldIdle() && gSaveBlock1Ptr->registeredItem != ITEM_NONE)
            PressOnce(SELECT_BUTTON);
        return;
    }
    if (id == HIT_COLUMN + COL_RUN)
    {
        if (FlagGet(FLAG_SYS_B_DASH))
        {
            CtrSettings_SetRunAlways(!CtrSettings_RunAlways());
            PlaySE(SE_SELECT);
        }
        return;
    }
    if (id >= HIT_COLUMN && id < HIT_COLUMN + SCR_COUNT)
    {
        u8 screen = id - HIT_COLUMN;

        /* The Fly map: the dock is locked; MAP, its own, cancels it. */
        if (sShown.mapFly)
        {
            if (screen == SCR_MAP)
                CancelFlyMap();
            return;
        }
        /* A battle's party menu, summary or bag, or the bag at the PC or a
         * shop: only its own button, their B. */
        if ((gMain.inBattle || sShown.dockLocked) && screen != sShown.screen)
            return;

        if (mode == MODE_POKENAV)
        {
            /* Its own button is its B; any other leaves it for that screen. */
            if (screen == SCR_POKENAV)
                NavPress(B_BUTTON);
            else
            {
                /* Off from the menu on show, straight to that screen
                 * (Task_Pokenav, CtrBottom_LeaveTo). */
                NavStop();
                CtrPokenav_Leave();
                LeaveForScreen(screen);
            }
            return;
        }
        /* The game's card: its own button closes it (B on its front, A on
         * its back); any other closes it for that screen. The pass the
         * same with B, which on its map goes back to the pass first: the
         * map closing for another screen goes straight to it
         * (CtrFrontierPass_Leave). */
        if (mode == MODE_CARD)
        {
            u8 side = CtrTrainerCard_Side();

            if (side != 2)
            {
                PressOnce(side == 0 ? B_BUTTON : A_BUTTON);
                if (screen != SCR_CARD)
                    LeaveForScreen(screen);
            }
            else if (!CtrTrainerCard_FieldOpen() && CtrFrontierPass_Waiting())
            {
                PressOnce(B_BUTTON);
                if (screen != SCR_CARD)
                    LeaveForScreen(screen);
            }
            return;
        }
        /* The bag on show: its own button closes it, as B does; any other
         * closes it for that screen. */
        if (mode == MODE_BAG_MENU)
        {
            if (CtrBag_Close() && screen != SCR_BAG)
                LeaveForScreen(screen);
            return;
        }
        /* The Pokédex's is its B; any other leaves it for that screen, B
         * after B, as the PokéNav's do. */
        if (mode == MODE_POKEDEX)
        {
            if (screen == SCR_POKEDEX)
                CtrPokedex_Close(FALSE);
            else if (CtrPokedex_Close(TRUE))
                LeaveForScreen(screen);
            return;
        }
        /* The game's party menu on show: its own button is B, as the bag's
         * and the Pokédex's are; any other closes it for that screen. */
        /* The summary opened from the party menu: POKéMON is its B, back to
         * the menu; any other goes back and leaves the menu for its screen.
         * With a move chosen or being moved, B only ends that: no leave. */
        if (mode == MODE_SUMMARY)
        {
            static struct CtrSummaryView view;
            bool8 closes = CtrSummary_GetView(&view) && view.movesMode == CTR_SUM_MOVES_VIEW;

            if (screen != SCR_POKEMON && closes)
            {
                LeaveForScreen(screen);
                CtrSummary_NativeTap(CTR_SUM_TAP_LEAVE, 0);
            }
            else
                CtrSummary_NativeTap(CTR_SUM_TAP_BACK, 0);
            return;
        }
        if (mode == MODE_PARTY_MENU)
        {
            if (screen == SCR_POKEMON)
                CtrParty_Close(FALSE);
            else if (CtrParty_Close(TRUE))
                LeaveForScreen(screen);
            return;
        }
        if (mode != MODE_FIELD)
            return;
        /* The optional read-only Pokémon view never starts a hidden menu.
         * Pressing its own dock icon again returns to MAP. */
        if (screen == SCR_POKEMON && !sClassicMenus && CtrExtras_Get("live_party") == 1)
        {
            sScreen = sScreen == SCR_POKEMON ? SCR_MAP : SCR_POKEMON;
            sLivePartyPending = PARTY_SIZE;
            PlaySE(SE_SELECT);
            return;
        }
        if (sScreen == SCR_POKEMON && !sClassicMenus && CtrExtras_Get("live_party") == 1)
        {
            sScreen = SCR_MAP;
            sLivePartyPending = PARTY_SIZE;
        }
        /* The PokéNav takes the area when it opens; the top keeps the world
         * from the moment it is asked for, fade included. */
        if (screen == SCR_POKENAV)
        {
            if (FieldIdle())
            {
                StartPlan(PLAN_START, START_POKENAV);
                BeginSession(FALSE);
            }
            return;
        }
        /* The game's card with the classic menus, as the PokéNav. */
        if (screen == SCR_CARD && sClassicMenus)
        {
            if (FieldIdle())
            {
                StartPlan(PLAN_START, START_PLAYER);
                BeginSession(FALSE);
            }
            return;
        }
        /* The game's bag and Pokédex, as the PokéNav: they take the area
         * when they open. */
        if (screen == SCR_BAG || screen == SCR_POKEDEX || screen == SCR_POKEMON)
        {
            OpenGameScreen(screen);
            return;
        }
        if (screen == SCR_SAVE && sScreen != SCR_SAVE)
            OpenSave();
        if (screen == SCR_CARD && sScreen != SCR_CARD)
        {
            sCardBack = FALSE;
            sCardView = CARD_VIEW_CARD;
        }
        /* Its own button on the pass or its map: back a step, as B. */
        else if (screen == SCR_CARD && sCardView != CARD_VIEW_CARD)
        {
            PassBack();
            return;
        }
        if (screen != sScreen)
            PlaySE(SE_SELECT);
        sScreen = screen;
        sPickMapsec = MAPSEC_NONE;
        return;
    }
    /* The title: the wild list's toggle, on the map. */
    if (id == HIT_MAP_INFO)
    {
        MapExtrasTap(id);
        return;
    }

    /* The game's screen drawn here: the tap its own code takes. Not on a
     * view kept from before while the game's has none to read: whatever
     * menu is up now would take it (a YES on a new question). */
    if (sShown.native)
    {
        if (!sNativeStale)
        {
            if (sShown.liveParty)
                LivePartyTap(id);
            else
                GameViewTap(&sShown, id);
        }
        return;
    }
    switch (sShown.screen)
    {
    case SCR_MAP:
        MapTap(id);
        break;
    case SCR_CARD:
        if (id == HIT_OK)
        {
            sCardBack = !sCardBack;
            PlaySE(SE_SELECT);
        }
        /* PASS: the Frontier Pass, drawn here. */
        else if (id == HIT_YES && sShown.hasPass && sShown.cardView == CARD_VIEW_CARD)
            OpenPass();
        else if (sShown.cardView != CARD_VIEW_CARD)
            PassTap(id);
        break;
    case SCR_NUZLOCKE:
        NuzlockeTap(id);
        break;
    case SCR_SAVE:
        if (id == HIT_YES && FieldIdle())
        {
            if (sSaveStep == SAVE_ASK && gSaveFileStatus != SAVE_STATUS_EMPTY && gSaveFileStatus != SAVE_STATUS_CORRUPT)
            {
                sSaveStep = SAVE_OVERWRITE;
                StringExpandPlaceholders(sSaveMessage, gDifferentSaveFile ? gText_DifferentSaveFile
                                                                         : gText_AlreadySavedFile);
            }
            else
                DoSave();
        }
        else if (id == HIT_NO || id == HIT_OK)
        {
            OpenSave();
            sScreen = SCR_MAP;
        }
        break;
    case SCR_OPTION:
        ActivateOption(id);
        break;
    }
}

#ifdef CTR_TEST_SWOOP
/* Azahar aid (3ds_game_full.c): the OPTIONS screen opened on the bottom, on
 * WORLD > 3D, then VOXEL 3D tapped there, as the player does it (bottom
 * redraws between). */
void CtrBottom_TestSwoop(unsigned step)
{
    if (step == 0)
    {
        sScreen = SCR_OPTION;
        sOptPage = CAT_WORLD;
        sOptSub = 0;
    }
    else
        Activate(HIT_OPTION, MODE_FIELD); /* the group's first row */
}
#endif

/* Whether a point is on the game's picture in its window (or, with the whole
 * screen, in the middle of it: the same place). */
static bool8 InGameWindow(int x, int y)
{
    return x >= CTR_BOTTOM_WIN_X && x < CTR_BOTTOM_WIN_X + 240 && y >= CTR_BOTTOM_WIN_Y
        && y < CTR_BOTTOM_WIN_Y + 160;
}

/* Returns the id to show as pressed. */
static u8 ProcessTouch(u8 mode)
{
    const CtrInput *in = CtrInput_Get();

    if (mode != sShown.mode)
    {
        /* A touch that began on another screen does not act on this one. */
        if (sTouch.bag)
        {
            CtrBag_Touch(BAG_TOUCH_CANCEL, 0, 0);
            CtrPokedex_Touch(BAG_TOUCH_CANCEL, 0, 0);
            CtrShop_Touch(BAG_TOUCH_CANCEL, 0, 0);
        }
        sTouch.active = FALSE;
        sTouch.bag = FALSE;
        return HIT_NONE;
    }
    /* The shop's BUY screen: its picture in the middle of the whole screen,
     * the touch in pixels of it, as it goes (drags scroll its list). */
    if ((in->touchDown && ShopShown(mode)) || (sTouch.bag && sTouch.active && ShopShown(mode)))
    {
        int ox = CTR_BOTTOM_WIN_X, oy = CTR_BOTTOM_WIN_Y;

        if (in->touchDown)
        {
            sTouch.active = sTouch.bag = TRUE;
            CtrShop_Touch(BAG_TOUCH_DOWN, in->touchX - ox, in->touchY - oy);
        }
        else if (in->touchActive)
            CtrShop_Touch(BAG_TOUCH_MOVE, in->touchX - ox, in->touchY - oy);
        else
        {
            sTouch.active = sTouch.bag = FALSE;
            CtrShop_Touch(BAG_TOUCH_UP, 0, 0);
        }
        return HIT_NONE;
    }
    /* The bag drawn here: a touch on its list's rows goes to the bag's own
     * list touch, at the same row of its list, as it goes. */
    if ((in->touchDown && BagShown(mode) && sShown.native == NATIVE_BAG
         && BagListTouch(&sShown, BAG_TOUCH_DOWN, in->touchX, in->touchY, TRUE))
     || (sTouch.bag && sTouch.active && BagShown(mode) && sShown.native == NATIVE_BAG))
    {
        if (in->touchDown)
            sTouch.active = sTouch.bag = TRUE;
        else if (in->touchActive)
            BagListTouch(&sShown, BAG_TOUCH_MOVE, in->touchX, in->touchY, FALSE);
        else
        {
            sTouch.active = sTouch.bag = FALSE;
            CtrBag_Touch(BAG_TOUCH_UP, 0, 0);
        }
        return HIT_NONE;
    }
    /* The game's bag: its picture in its window, the touch in pixels of it,
     * as it goes. With the whole screen, anywhere. */
    if ((in->touchDown && BagShown(mode) && !sShown.native
         && (sShown.bagView == BAG_VIEW_WHOLE || InGameWindow(in->touchX, in->touchY)))
     || (sTouch.bag && sTouch.active && BagShown(mode)))
    {
        int ox = CTR_BOTTOM_WIN_X, oy = CTR_BOTTOM_WIN_Y;

        if (in->touchDown)
        {
            sTouch.active = sTouch.bag = TRUE;
            CtrBag_Touch(BAG_TOUCH_DOWN, in->touchX - ox, in->touchY - oy);
        }
        else if (in->touchActive)
            CtrBag_Touch(BAG_TOUCH_MOVE, in->touchX - ox, in->touchY - oy);
        else
        {
            sTouch.active = sTouch.bag = FALSE;
            CtrBag_Touch(BAG_TOUCH_UP, 0, 0);
        }
        return HIT_NONE;
    }
    /* The Pokédex drawn here: a touch on its list or over an entry goes to
     * its own touch, at the same place of its picture, as it goes. */
    if ((in->touchDown && DexShown(mode) && sShown.native == NATIVE_DEX
         && DexTouch(&sShown, BAG_TOUCH_DOWN, in->touchX, in->touchY, TRUE))
     || (sTouch.bag && sTouch.active && DexShown(mode) && sShown.native == NATIVE_DEX))
    {
        if (in->touchDown)
            sTouch.active = sTouch.bag = TRUE;
        else if (in->touchActive)
            DexTouch(&sShown, BAG_TOUCH_MOVE, in->touchX, in->touchY, FALSE);
        else
        {
            sTouch.active = sTouch.bag = FALSE;
            DexTouch(&sShown, BAG_TOUCH_UP, 0, 0, FALSE);
        }
        return HIT_NONE;
    }
    /* The game's Pokédex, the same way. */
    if ((in->touchDown && DexShown(mode) && !sShown.native && InGameWindow(in->touchX, in->touchY))
     || (sTouch.bag && sTouch.active && DexShown(mode)))
    {
        int ox = CTR_BOTTOM_WIN_X, oy = CTR_BOTTOM_WIN_Y;

        if (in->touchDown)
        {
            sTouch.active = sTouch.bag = TRUE;
            CtrPokedex_Touch(BAG_TOUCH_DOWN, in->touchX - ox, in->touchY - oy);
        }
        else if (in->touchActive)
            CtrPokedex_Touch(BAG_TOUCH_MOVE, in->touchX - ox, in->touchY - oy);
        else
        {
            sTouch.active = sTouch.bag = FALSE;
            CtrPokedex_Touch(BAG_TOUCH_UP, 0, 0);
        }
        return HIT_NONE;
    }
    if (in->touchDown)
    {
        sTouch.active = TRUE;
        sTouch.dragged = FALSE;
        sTouch.startX = sTouch.lastX = in->touchX;
        sTouch.startY = sTouch.lastY = in->touchY;
        sTouch.pressed = HitTest(in->touchX, in->touchY);
    }
    else if (in->touchActive && sTouch.active)
    {
        int dy = in->touchY - sTouch.startY, dx = in->touchX - sTouch.startX;

        sTouch.lastX = in->touchX;
        sTouch.lastY = in->touchY;
        if (!sTouch.dragged && (dy > 8 || dy < -8 || dx > 8 || dx < -8))
            sTouch.dragged = TRUE;
    }
    else if (in->touchUp && sTouch.active)
    {
        sTouch.active = FALSE;
        /* The game's party menu, its picture in its window: the tap goes to
         * whatever waits for input in it (a mon, the buttons, its menu, a
         * question, a message), in pixels of that picture. The header's and
         * the dock's buttons are ours. */
        if (mode == MODE_PARTY_MENU && !sShown.native
         && (sShown.bagView == BAG_VIEW_WHOLE || InGameWindow(sTouch.startX, sTouch.startY)))
        {
            int ox = CTR_BOTTOM_WIN_X, oy = CTR_BOTTOM_WIN_Y;

            if (!sTouch.dragged)
                CtrMenu_PostTap(sTouch.lastX - ox, sTouch.lastY - oy);
            return HIT_NONE;
        }
        /* The boxes have the whole screen, their picture in the middle of it,
         * and take the tap themselves, in pixels of that picture. */
        if (mode == MODE_STORAGE)
        {
            int x = sTouch.lastX - CTR_BOTTOM_WIN_X, y = sTouch.lastY - CTR_BOTTOM_WIN_Y;

            /* Or a summary opened from them, the same way. So does the
             * trade menu. */
            if (!sTouch.dragged && CtrStorage_IsOpen())
                CtrStorage_Tap(x, y);
            else if (!sTouch.dragged && CtrTrade_IsOpen())
                CtrMenu_PostTap(x, y);
            else if (!sTouch.dragged)
                CtrSummary_Tap(x, y);
            return HIT_NONE;
        }
        if (mode == MODE_CARD && InGameWindow(sTouch.startX, sTouch.startY))
        {
            u8 side = CtrTrainerCard_Side();

            if (CtrTrainerCard_FieldOpen())
            {
                if (!sTouch.dragged && side != 2)
                    PressOnce(side == 0 ? A_BUTTON : B_BUTTON);
            }
            /* The pass and its map take the tap where it is. */
            else if (!sTouch.dragged)
                CtrFrontierPass_Tap(sTouch.lastX - CTR_BOTTOM_WIN_X, sTouch.lastY - CTR_BOTTOM_WIN_Y);
            return HIT_NONE;
        }
        if (mode == MODE_POKENAV && InGameWindow(sTouch.startX, sTouch.startY))
        {
            if (!sTouch.dragged)
                NavTap(sTouch.lastX - CTR_BOTTOM_WIN_X, CtrVideo_BottomPictureY(sTouch.lastY));
            else if (sTouch.lastY - sTouch.startY > 24 || sTouch.lastY - sTouch.startY < -24)
                NavSwipe(sTouch.lastY - sTouch.startY);
            return HIT_NONE;
        }
        /* While the turn plays out the screen is the backdrop: a tap on it is
         * A, on with the text. */
        if (mode == MODE_BATTLE_INFO)
        {
            if (!sTouch.dragged)
                PressOnce(A_BUTTON);
            return HIT_NONE;
        }
        /* A message in the field (a sign, a person, the text after a
         * battle): a tap off the buttons goes on with it, as in a battle.
         * Only what waits on a message takes it (CtrMenu_PostAck), never a
         * menu or a yes/no box, and the map picks no place meanwhile. */
        if (mode == MODE_FIELD && !sTouch.dragged && FieldMessageUp()
         && (sTouch.pressed == HIT_NONE || sTouch.pressed == HIT_MAP || sTouch.pressed == HIT_MAP_PANEL))
        {
            CtrMenu_PostAck();
            return HIT_NONE;
        }
        /* Off every button of a screen drawn over the backdrop, or on the idle backdrop itself: a tap
         * goes on with whatever message the game's screen waits on (the professor's speech, the
         * contest's, the Berry Blender's results), as on the field. Never with a menu or a list up:
         * what is not one of its buttons is nothing. */
        if (!sTouch.dragged && sTouch.pressed == HIT_NONE && !sShown.fm.count && !sShown.fl.count
         && (mode == MODE_PAD || (mode == MODE_OFF && sShown.dim)))
        {
            CtrMenu_PostAck();
            return HIT_NONE;
        }
        if ((!sTouch.dragged || sTouch.pressed == HIT_MAP) && HitTest(sTouch.lastX, sTouch.lastY) == sTouch.pressed)
        {
            u8 id = sTouch.pressed;

            Activate(id, mode);
            /* A game's screen opened by a tap while the dock had the focus
             * (X): the buttons go to it, as when A chose it. */
            if (id >= HIT_COLUMN && id < HIT_COLUMN + SCR_COUNT && IsGameScreenItem(id - HIT_COLUMN)
             && (sFocus != FOCUS_NONE || sInside != INSIDE_NONE))
                LeaveFocus();
        }
        return HIT_NONE;
    }
    if (!sTouch.active || sTouch.dragged)
        return HIT_NONE;
    return sTouch.pressed;
}
