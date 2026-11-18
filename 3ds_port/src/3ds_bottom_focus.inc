/* Part of 3ds_bottom_ui.c: the dock and the header walked with the buttons (X). */

/* ------------------------------------------------------------------------ */
/* The dock and the header by the buttons (X)                              */
/* ------------------------------------------------------------------------ */

/*
 * X gives the dock a focus: LEFT and RIGHT walk its buttons, UP goes to the
 * header (Y, the title while it is the wild list's toggle, RUN) and DOWN
 * back; A does what a tap would, B or X again gives the buttons back to the
 * game. While the dock has them the game sees none, so the player stands
 * still. Opened with A, the options take the focus into their panel (up and
 * down through the rows, left and right or A to change one, L and R through
 * the groups, B back to the dock) and the save screen takes A for YES and B
 * for NO; the game's own screens (party, bag, Pokédex, PokéNav) take the
 * buttons themselves, and X inside them brings the focus back to the dock
 * to go somewhere else.
 */
uint16_t CtrBottom_FilterKeys(uint16_t held)
{
    /* The soft reset's chord reaches the game whatever has the buttons
     * (main.c reads it from them), and only that; not while the game
     * forbids the reset (a link save). */
    if ((held & CTR_SOFT_RESET_KEYS) == CTR_SOFT_RESET_KEYS && !gSoftResetDisabled)
        return sFocus != FOCUS_NONE || sInside != INSIDE_NONE || sSwallow ? CTR_SOFT_RESET_KEYS : held;
    return sFocus != FOCUS_NONE || sInside != INSIDE_NONE || sSwallow || sMapFly ? 0 : held;
}

/* Whether the dock is on screen: not in battle, not under the boxes or the
 * bag or party menu that a battle, a shop or the PC opens. */
static bool8 ColumnShown(u8 mode)
{
    if (gMain.inBattle)
        return FALSE;
    switch (mode)
    {
    case MODE_FIELD:
    case MODE_POKENAV:
    case MODE_POKEDEX:
    case MODE_CARD:
        return TRUE;
    case MODE_SUMMARY:
        return !ChoosesForScript(mode);
    case MODE_PARTY_MENU:
        return gPartyMenu.menuType == PARTY_MENU_TYPE_FIELD;
    case MODE_BAG_MENU:
        return CtrBag_InWindow() && !BagReturnsToMenu(mode);
    }
    return FALSE;
}

static bool8 ColumnItemAvailable(u8 item)
{
    if (item < SCR_COUNT)
        return (EnabledScreens() >> item) & 1;
    if (item == COL_Y)
        return gSaveBlock1Ptr->registeredItem != ITEM_NONE;
    if (item == COL_TITLE)
        return TitleToggles(&sShown);
    return FlagGet(FLAG_SYS_B_DASH);
}

/* The next available button left (dir < 0) or right on the focus's row - the
 * dock's, or the header's - or `from`. */
static u8 ColumnStep(u8 from, int dir)
{
    static const u8 header[] = {COL_Y, COL_TITLE, COL_RUN};

    if (from >= SCR_COUNT)
    {
        int i = 0;

        while (i < (int)ARRAY_COUNT(header) && header[i] != from)
            ++i;
        for (i += dir; i >= 0 && i < (int)ARRAY_COUNT(header); i += dir)
            if (ColumnItemAvailable(header[i]))
                return header[i];
        return from;
    }
    for (int i = from + dir; i >= 0 && i < SCR_COUNT; i += dir)
        if (ColumnItemAvailable(i))
            return i;
    return from;
}

/* UP from the dock: the header's middle (the title), or what of it there is. */
static u8 HeaderFocus(void)
{
    if (ColumnItemAvailable(COL_TITLE))
        return COL_TITLE;
    if (ColumnItemAvailable(COL_Y))
        return COL_Y;
    if (ColumnItemAvailable(COL_RUN))
        return COL_RUN;
    return FOCUS_NONE;
}

static void MoveFocus(u8 *focus, u8 to)
{
    if (to != *focus)
    {
        *focus = to;
        PlaySE(SE_SELECT);
    }
}

/* Gives the buttons back to the game once they are let go. */
static void LeaveFocus(void)
{
    sFocus = FOCUS_NONE;
    sInside = INSIDE_NONE;
    sSwallow = TRUE;
}

/* A on a dock or header button: what a tap on it does, and where the focus
 * goes. */
static void ChooseColumnItem(u8 mode, u8 item)
{
    bool8 gameScreen = IsGameScreenItem(item);

    if (item == COL_RUN)
    {
        Activate(HIT_COLUMN + COL_RUN, mode);
        return;
    }
    if (item == COL_TITLE)
    {
        Activate(HIT_MAP_INFO, mode);
        return;
    }
    if (item == COL_Y)
    {
        LeaveFocus();
        Activate(HIT_COLUMN + COL_Y, mode);
        return;
    }
    /* The game's screen on show already: back to it. */
    if (gameScreen && item == sShown.screen && mode != MODE_FIELD)
    {
        LeaveFocus();
        return;
    }
    Activate(HIT_COLUMN + item, mode);
    if (gameScreen)
        LeaveFocus();
    else if (item == SCR_OPTION || item == SCR_SAVE)
    {
        sFocus = FOCUS_NONE;
        sInside = item == SCR_OPTION ? INSIDE_OPTIONS : INSIDE_SAVE;
        if (item == SCR_OPTION)
            sOptFocus = OptionStep(0xFF, 1) == 0xFF ? 0 : OptionStep(0xFF, 1);
    }
}

static void SaveKeys(u8 mode, u16 down)
{
    if (down & A_BUTTON)
        Activate(sSaveStep == SAVE_DONE ? HIT_OK : HIT_YES, mode);
    else if (down & B_BUTTON)
        Activate(sSaveStep == SAVE_DONE ? HIT_OK : HIT_NO, mode);
    /* Done or declined, the screen goes back to the map: so does the focus,
     * to the dock. */
    if (sScreen != SCR_SAVE)
    {
        sInside = INSIDE_NONE;
        sFocus = sScreen;
    }
}

static void ProcessKeys(u8 mode)
{
    const CtrInput *in = CtrInput_Get();
    u16 down = in->down;
    bool8 x = (in->physicalDown & CTR_KEY_X) != 0;

    if (sSwallow && !(in->held & CTR_KEY_GAME) && !(in->physicalHeld & (CTR_KEY_X | CTR_KEY_Y)))
        sSwallow = FALSE;
    if (!ColumnShown(mode))
    {
        if (sFocus != FOCUS_NONE || sInside != INSIDE_NONE)
            LeaveFocus();
        return;
    }
    if (sMapFly)
    {
        if (sShown.mapFly && (down & A_BUTTON))
            FlyFromMap();
        else if (sShown.mapFly && (down & B_BUTTON))
            CancelFlyMap();
        return;
    }
    /* A tap took the screen elsewhere. */
    if ((sInside == INSIDE_OPTIONS && sScreen != SCR_OPTION) || (sInside == INSIDE_SAVE && sScreen != SCR_SAVE))
    {
        sInside = INSIDE_NONE;
        sFocus = sScreen;
    }
    if (sInside != INSIDE_NONE)
    {
        if (x)
        {
            sInside = INSIDE_NONE;
            sFocus = sScreen;
            PlaySE(SE_SELECT);
        }
        else if (sInside == INSIDE_OPTIONS)
            OptionKeys(down);
        else
            SaveKeys(mode, down);
        return;
    }
    if (sFocus == FOCUS_NONE)
    {
        /* Not in the middle of a script or a field effect. */
        if (!x || (mode == MODE_FIELD && (gMain.callback2 != CB2_Overworld || ArePlayerFieldControlsLocked()
                                          || ScriptContext_IsEnabled())))
            return;
        sFocus = sShown.screen < SCR_COUNT && ColumnItemAvailable(sShown.screen) ? sShown.screen : SCR_MAP;
        PlaySE(SE_SELECT);
        return;
    }
    if (!ColumnItemAvailable(sFocus))
        sFocus = sFocus >= SCR_COUNT && HeaderFocus() != FOCUS_NONE ? HeaderFocus() : SCR_MAP;
    if (x || (down & B_BUTTON))
    {
        LeaveFocus();
        PlaySE(SE_SELECT);
    }
    else if (down & DPAD_LEFT)
        MoveFocus(&sFocus, ColumnStep(sFocus, -1));
    else if (down & DPAD_RIGHT)
        MoveFocus(&sFocus, ColumnStep(sFocus, 1));
    else if ((down & DPAD_UP) && sFocus < SCR_COUNT && HeaderFocus() != FOCUS_NONE)
        MoveFocus(&sFocus, HeaderFocus());
    else if ((down & DPAD_DOWN) && sFocus >= SCR_COUNT)
        MoveFocus(&sFocus, sShown.screen < SCR_COUNT && ColumnItemAvailable(sShown.screen) ? sShown.screen : SCR_MAP);
    else if (down & A_BUTTON)
        ChooseColumnItem(mode, sFocus);
}
