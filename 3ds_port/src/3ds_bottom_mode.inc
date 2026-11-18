/* Part of 3ds_bottom_ui.c: which mode the game is in, and the hidden sessions that hold the top screen. */

/* ------------------------------------------------------------------------ */
/* Game state                                                               */
/* ------------------------------------------------------------------------ */


/* The player stands in the field with nothing else going on. */
static bool8 FieldIdle(void)
{
    return gMain.callback2 == CB2_Overworld && !ArePlayerFieldControlsLocked() && !ScriptContext_IsEnabled()
        && gPlayerAvatar.tileTransitionState == T_NOT_MOVING && !gPaletteFade.active && !CtrStartMenu_Busy();
}

/* A field message box is up (field_message_box.c), as when a person talks. */
static bool8 FieldMessageUp(void)
{
    return gMain.callback2 == CB2_Overworld && !IsFieldMessageBoxHidden() && !gPaletteFade.active;
}

/*
 * FLY on the map itself, as the party menu would let Fly be used: the
 * party's Pokemon that know it (not an egg), the Feather Badge, a map whose
 * type allows it (SetUpFieldMove_Fly), and the field free - no script, no
 * fade, nothing holding the player. A step under way is no bar: the flight
 * from the field ends it first (CtrFly_Start).
 */
static u8 MapFlyers(void)
{
    u8 flyers = 0;

    for (int i = 0; i < PARTY_SIZE && i < gPlayerPartyCount; ++i)
    {
        struct Pokemon *mon = &gPlayerParty[i];

        if (GetMonData(mon, MON_DATA_SPECIES) == SPECIES_NONE || GetMonData(mon, MON_DATA_IS_EGG))
            continue;
        for (int j = 0; j < MAX_MON_MOVES; ++j)
            if (GetMonData(mon, MON_DATA_MOVE1 + j) == MOVE_FLY)
            {
                flyers |= 1 << i;
                break;
            }
    }
    return flyers;
}

static bool8 MapFlyField(void)
{
    return !gMain.inBattle && gMain.callback2 == CB2_Overworld && !ArePlayerFieldControlsLocked()
        && !ScriptContext_IsEnabled() && !gPaletteFade.active && !CtrStartMenu_Busy()
        && Overworld_MapTypeAllowsTeleportAndFly(gMapHeader.mapType);
}

/* Of the ones that know Fly, the one chosen or last flown with, else the first. */
static u8 MapFlyerSlot(u8 flyers)
{
    if (flyers == 0)
        return PARTY_SIZE;
    if (sFlyerKnown)
        for (int i = 0; i < PARTY_SIZE; ++i)
            if (((flyers >> i) & 1) && GetMonData(&gPlayerParty[i], MON_DATA_PERSONALITY) == sFlyerPersonality)
                return i;
    for (int i = 0; i < PARTY_SIZE; ++i)
        if ((flyers >> i) & 1)
            return i;
    return PARTY_SIZE;
}

static void RememberFlyer(u8 slot)
{
    if (slot >= PARTY_SIZE)
        return;
    sFlyerPersonality = GetMonData(&gPlayerParty[slot], MON_DATA_PERSONALITY);
    sFlyerKnown = TRUE;
}

static u16 EnabledScreens(void)
{
    u16 mask = (1 << SCR_MAP) | (1 << SCR_BAG) | (1 << SCR_CARD) | (1 << SCR_OPTION);

    if (FlagGet(FLAG_SYS_POKEMON_GET))
        mask |= 1 << SCR_POKEMON;
    if (FlagGet(FLAG_SYS_POKEDEX_GET))
        mask |= 1 << SCR_POKEDEX;
    if (FlagGet(FLAG_SYS_POKENAV_GET) && CtrStartMenu_Available())
        mask |= 1 << SCR_POKENAV;
    if (CtrStartMenu_Available())
        mask |= 1 << SCR_SAVE;
    if (CtrNuzlockeUi_Enabled())
        mask |= 1 << SCR_NUZLOCKE;
    return mask;
}

/*
 * What the battle controllers wait for. Their input handlers call in here
 * every frame they run (CtrBattleMenu_*Input); last frame's call is what the
 * bottom screen shows.
 */
enum { ASK_NONE, ASK_ACTION, ASK_MOVE, ASK_TARGET };

typedef struct
{
    u8 kind, battler;
} BattleAsk;

static BattleAsk sAsk, sAsked;
static u8 sBattleTap = 0xFF;   /* a tap for the controller to take */
static bool8 sMoveCancel;      /* the move menu's cursor is on CANCEL */
static bool8 sQuickBallTap;    /* the ball button was tapped */
static bool8 sQuickBallFocus;  /* the D-pad is on the ball button (A throws) */

/*
 * The quick ball (battle_controller_player.c, an optional patch in the
 * public tree): the ball X or the ball button would throw now, or
 * ITEM_NONE. Without that patch no battle offers one.
 */
u16 __attribute__((weak)) CtrBattle_QuickBallItem(void)
{
    return ITEM_NONE;
}

bool8 __attribute__((weak)) CtrBattle_QuickBallCycle(s8 direction)
{
    (void)direction;
    return FALSE;
}

/* Frames a screen of the game with none of its own here has been up before the field's map and dock give way
 * to the idle backdrop: a map being loaded goes by quicker than that. */
#define PAD_APPEARS_AFTER 45

/*
 * The game's screens drawn here over the idle backdrop (MODE_PAD), each from the first frame its own
 * callback runs to the last: the screen says so itself, nothing is waited for, so neither the field's map
 * nor the game's own picture of it is on show while it is being set up.
 */
static bool8 PadScreenActive(void)
{
    return CtrBlender_Active() || CtrContest_Active() || CtrEasyChat_Active() || CtrFactory_Active();
}

/* From the main menu to the field (main_menu.c): no black screen, the idle backdrop. */
static bool8 sPreField;

void CtrBottom_PreField(bool8 on)
{
    sPreField = on;
}

/* MODE_PAD was the last of the game's screens: whatever follows it short of the field is one too. */
static bool8 sPadFollows;

static u8 CurrentMode(void)
{
    /* Before the field is reached too: the player's name is asked in the
     * professor's speech. */
    if (CtrNaming_IsOpen() || CtrChat_IsOpen())
        return MODE_NAMING;
    if (gMain.callback2 == CB2_Overworld)
    {
        sInGame = TRUE;
        sPreField = FALSE;
        sPadFollows = FALSE;
    }
    /* The title screen again (B on the main menu): its own bottom screen. */
    if (sPreField && CtrTitleScreen_RayquazaBg() >= 0)
        sPreField = FALSE;
    if (!sInGame || !gSaveBlock1Ptr || !gSaveBlock2Ptr)
        return MODE_OFF;
    /* Before the magnified stage: the contest is one, and its move choice is drawn here. */
    if (PadScreenActive())
    {
        sPadFollows = TRUE;
        return MODE_PAD;
    }
    /* The legendaries' cutscenes have the top screen to themselves, the
     * bottom one dark under them. */
    if (CtrVideo_StageZoomed())
        return MODE_OFF;
    if (CtrPokenav_IsOpen())
        return MODE_POKENAV;
    if (CtrPokedex_IsOpen())
        return MODE_POKEDEX;
    /* The game's card, or its Frontier Pass (PLAYER with the pass): the
     * classic menus' own screens, in the window. */
    if (CtrTrainerCard_FieldOpen() || CtrFrontierPass_FieldOpen())
        return MODE_CARD;
    /* The summary from the field's or a battle's party menu, drawn here; a
     * classic one has the whole screen, as from the boxes (MODE_STORAGE). */
    if (!sClassicMenus && CtrSummary_InWindow())
        return MODE_SUMMARY;
    /* Before the boxes: the bag can be opened from them, whole screen too. */
    if (gMain.callback2 == CB2_BagMenuRun && gBagMenu)
        return MODE_BAG_MENU;
    /* A summary opened from the boxes is on the whole screen too. */
    if (CtrStorage_IsOpen() || CtrVideo_BottomWhole())
        return MODE_STORAGE;
    if (FuncIsActiveTask(Task_HandleChooseMonInput))
        sPartyMenuCallback = gMain.callback2;
    if ((sPartyMenuCallback && gMain.callback2 == sPartyMenuCallback) || CtrParty_Building())
        return MODE_PARTY_MENU;
    if (gMain.inBattle)
    {
        if (gMain.callback2 == BattleMainCB2 && !gPaletteFade.active)
        {
            if (sAsked.kind == ASK_ACTION) return MODE_BATTLE_ACTION;
            if (sAsked.kind == ASK_MOVE) return MODE_BATTLE_MOVE;
            if (sAsked.kind == ASK_TARGET) return MODE_BATTLE_TARGET;
        }
        return MODE_BATTLE_INFO;
    }
    /* A screen of the game the bottom one does not draw: the idle backdrop. At once after one of those
     * drawn here (the summary a Rental Pokemon is looked at in being set up, the way back to the field),
     * or with a menu of the game's to answer; else once it has been up a moment (not on the way between
     * two maps). */
    {
        static MainCallback last;
        static u16 up;

        /* Another screen starts the wait again: a map being loaded goes by quicker than it. */
        if (gMain.callback2 != last || gMain.callback2 == CB2_Overworld)
            up = 0;
        last = gMain.callback2;
        if (gMain.callback2 != CB2_Overworld)
        {
            static struct CtrFieldMenuView menu;
            static struct CtrFieldListView list;

            if (up < 0xFFFF)
                ++up;
            if (sPadFollows || up > PAD_APPEARS_AFTER || CtrFieldMenu_GetView(&menu) || CtrFieldList_GetView(&list))
            {
                sPadFollows = TRUE;
                return MODE_PAD;
            }
        }
    }
    return MODE_FIELD;
}

/* ------------------------------------------------------------------------ */
/* Hidden sessions: the game's menus running under the world              */
/* ------------------------------------------------------------------------ */

static struct
{
    bool8 active, entered, battle;
    /* Opened by CtrBottom_KeepWorld: a game screen is on its way. */
    bool8 awaitMenu;
    u16 frames, away;
} sSession;

static void BeginSession(bool8 battle)
{
    if (!sSession.active)
        CtrLog_Write(CTR_LOG_VIDEO, "bottom screen: hidden menu session (%s)", battle ? "battle" : "field");
    sSession.active = TRUE;
    sSession.entered = FALSE;
    sSession.awaitMenu = FALSE;
    sSession.battle = battle;
    sSession.frames = sSession.away = 0;
}

/*
 * Whether the top screen holds its frame. A session holds from the first
 * press that leads to the menu (the fade out is not shown) through the menu
 * and back until the fade in is over. A screen the session did not expect -
 * an evolution, the move to forget, the fly map - is shown after a moment.
 */
static bool8 UpdateSession(u8 mode, bool8 planRunning)
{
    bool8 inMenu = mode == MODE_PARTY_MENU || mode == MODE_BAG_MENU || mode == MODE_POKENAV
                || mode == MODE_STORAGE || mode == MODE_POKEDEX || mode == MODE_SUMMARY || mode == MODE_CARD
                || mode == MODE_NAMING;
    bool8 home = gMain.callback2 == CB2_Overworld || gMain.callback2 == BattleMainCB2;

    if (inMenu && !sSession.active)
        BeginSession(gMain.inBattle);
    if (!sSession.active)
        return FALSE;
    ++sSession.frames;
    if (inMenu)
    {
        sSession.entered = TRUE;
        sSession.away = 0;
        return TRUE;
    }
    if (home)
    {
        sSession.away = 0;
        /* A screen asked for by the game (the shop's BUY, the bag to sell
         * from) starts a frame after the fade out: not the session's end. */
        if (sSession.awaitMenu && !sSession.entered && sSession.frames < 180)
            return TRUE;
        if ((sSession.entered || (!planRunning && sSession.frames > 30)) && !gPaletteFade.active)
        {
            sSession.active = FALSE;
            CtrLog_Write(CTR_LOG_VIDEO, "bottom screen: hidden menu session over");
            return FALSE;
        }
        return TRUE;
    }
    /* A menu's setup and the map reload pass through here too: brief. */
    return ++sSession.away < 40;
}

/* Hidden menus need not wait on their fades and slow text. */
static void FastForward(void)
{
    for (int i = 0; i < 6; ++i)
    {
        if (gPaletteFade.active)
            UpdatePaletteFade();
        RunTextPrinters();
    }
}
