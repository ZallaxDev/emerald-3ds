/* Part of 3ds_bottom_ui.c: what a tap does in the sections drawn here: SAVE, the pass, the map. */

/* ------------------------------------------------------------------------ */
/* What a tap does                                                          */
/* ------------------------------------------------------------------------ */

static struct
{
    bool8 active, dragged;
    s16 startX, startY, lastX, lastY;
    u8 pressed;
    bool8 bag;   /* on the game's bag or Pokédex, which take it themselves */
} sTouch;

/* The save, done here as start_menu.c's SaveDoSaveCallback does it. */
static void DoSave(void)
{
    u8 status;

    SaveMapView();
    IncrementGameStat(GAME_STAT_SAVED_GAME);
    if (gDifferentSaveFile == TRUE)
    {
        status = TrySavingData(SAVE_OVERWRITE_DIFFERENT_FILE);
        gDifferentSaveFile = FALSE;
    }
    else
    {
        status = TrySavingData(SAVE_NORMAL);
    }
    StringExpandPlaceholders(sSaveMessage, status == SAVE_STATUS_OK ? gText_PlayerSavedGame : gText_SaveError);
    if (status == SAVE_STATUS_OK)
        PlaySE(SE_SAVE);
    sSaveStep = SAVE_DONE;
}

static void OpenSave(void)
{
    sSaveStep = SAVE_ASK;
    sSaveSel = 0;
    StringExpandPlaceholders(sSaveMessage, gText_ConfirmSave);
}

/* A column screen chosen while one of the game's is on show, for when the
 * area is the bottom screen's again. sScreen may still be SAVE from before
 * that screen opened: SAVE opens on its question all the same. */
/* A screen chosen from the dock while one of the game's was up, for that one
 * to go to when it closes (CtrBottom_LeaveTo), and the frames it has been
 * waiting. */
static u8 sLeaveTo = SCR_COUNT;
static u16 sLeaveToWait;
/* The same choice, kept until the field is reached (OpenAsked): the game's
 * screen drops sLeaveTo as it closes, before that. */
static bool8 sLeaveWanted;

static void LeaveForScreen(u8 screen)
{
    if (screen == SCR_SAVE)
        OpenSave();
    sLeaveWanted = TRUE;
    sScreen = screen;
    sLeaveTo = screen;
    sLeaveToWait = 0;
}

/*
 * Where one of the game's screens goes as it closes, given where it would
 * go (field): with a screen of the game's own chosen from the dock, that
 * screen, opened straight from this one as the start menu would open it
 * from the field - one screen closing and the other opening, as the
 * sections of the bottom screen change, not the field in between. The
 * bottom screen's own sections open with the field back, as before.
 */
/* A screen chosen from the dock is waiting for the one on show to close. */
bool8 CtrBottom_LeavePending(void)
{
    return sLeaveTo < SCR_COUNT;
}

MainCallback CtrBottom_LeaveTo(MainCallback field)
{
    u8 screen = sLeaveTo;
    u8 action = START_NONE;
    MainCallback cb;

    sLeaveTo = SCR_COUNT;
    if (field != CB2_ReturnToFieldWithOpenMenu || screen >= SCR_COUNT || !(EnabledScreens() & (1 << screen)))
        return field;
    switch (screen)
    {
    case SCR_POKEMON: action = START_POKEMON; break;
    case SCR_BAG:     action = START_BAG; break;
    case SCR_POKEDEX: action = START_POKEDEX; break;
    case SCR_POKENAV: action = START_POKENAV; break;
    case SCR_CARD:    action = sClassicMenus ? START_PLAYER : START_NONE; break;
    }
    if (action == START_NONE || (cb = CtrStartMenu_DirectCallback(action)) == NULL)
        return field;
    CtrLog_Write(CTR_LOG_VIDEO, "bottom screen: straight to screen %u", screen);
    return cb;
}


/* The Frontier Pass, from PASS on the card: whether a battle is recorded is
 * read from the save now, once, as the game's pass does as it opens. */
static void OpenPass(void)
{
    sCardView = CARD_VIEW_PASS;
    sPassPick = PASS_PICK_SYMBOLS;
    sPassRecord = CanCopyRecordedBattleSaveData() != FALSE;
    PlaySE(SE_SELECT);
}

/* BACK, B or the card's own button: the map to the pass, the pass to the card. */
static void PassBack(void)
{
    sCardView = sCardView == CARD_VIEW_MAP ? CARD_VIEW_PASS : CARD_VIEW_CARD;
    PlaySE(SE_PC_OFF);
}

/* A touch on the pass or its map, as the game's cursor and A would have it. */
static void PassTap(u8 id)
{
    if (id == HIT_PASS_BACK)
        PassBack();
    else if (sShown.cardView == CARD_VIEW_MAP)
    {
        if (id >= HIT_PASS_FACILITY && id < HIT_PASS_FACILITY + NUM_FRONTIER_FACILITIES
         && id - HIT_PASS_FACILITY != sPassMapPos)
        {
            sPassMapPos = id - HIT_PASS_FACILITY;
            PlaySE(SE_DEX_SCROLL);
        }
    }
    else if (id == HIT_PASS_MAP)
    {
        /* The map opens on its first facility, as the game's does. */
        sCardView = CARD_VIEW_MAP;
        sPassMapPos = 0;
        PlaySE(SE_SELECT);
    }
    else if (id == HIT_PASS_RECORD)
    {
        if (!sShown.passRecord)
        {
            sPassPick = PASS_PICK_NO_RECORD;
            PlaySE(SE_SELECT);
        }
        else if (FieldIdle())
        {
            PlaySE(SE_SELECT);
            CtrFrontierPass_PlayRecord();
        }
    }
    else if (id == HIT_PASS_POINTS || id == HIT_PASS_SYMBOLS
          || (id >= HIT_PASS_SYMBOL && id < HIT_PASS_SYMBOL + NUM_FRONTIER_FACILITIES))
    {
        sPassPick = id == HIT_PASS_POINTS ? PASS_PICK_POINTS : id == HIT_PASS_SYMBOLS ? PASS_PICK_SYMBOLS
                  : PASS_PICK_SYMBOL + (id - HIT_PASS_SYMBOL);
        PlaySE(SE_SELECT);
    }
}

/* Touches use the displayed snapshot and only act on enabled controls. */
static void MapExtrasTap(u8 id)
{
    if (id == HIT_MAP_INFO && sShown.encOn && sShown.screen == SCR_MAP)
    {
        sEncView = !sShown.encView;
        sEncPage = 0;
        sPickMapsec = MAPSEC_NONE;
        PlaySE(SE_SELECT);
    }
    else if (id == HIT_MAP_FILTER && sShown.encView && sShown.encTotal)
    {
        sEncUncaught = !sShown.encUncaught;
        sEncPage = sShown.encTogglePage;
        PlaySE(SE_SELECT);
    }
    else if (id == HIT_MAP_PAGE && sShown.encView && sShown.encPages > 1)
    {
        sEncPage = (sShown.encPage + 1) % sShown.encPages;
        PlaySE(SE_SELECT);
    }
    else if (id == HIT_CLOCK_NOTICE && sShown.clockNotice)
        sClockNoticeSerial = CtrClockStore_LoadSerial();
}

static bool8 OpenGameScreen(u8 screen);

/*
 * The Fly map, from public PR #108: FLY from the party or the bag's HM opens
 * the MAP here, its icons on it, instead of the game's Fly map. Till FLY or
 * CANCEL the game sees no buttons (CtrBottom_FilterKeys) and the dock is
 * locked; A is FLY, B is CANCEL, the +Control Pad moves the pick
 * (FlyMapDpad) as the game's Fly map moves its cursor.
 */
void CtrBottom_OpenFlyMap(u8 slot, bool8 fromBag)
{
    u8 x, y;

    sMapFly = TRUE;
    sMapZoom = FALSE;
    sFlySlot = slot;
    sFlyFrom = fromBag ? SCR_BAG : SCR_POKEMON;
    sScreen = SCR_MAP;
    sEncView = FALSE;
    sFocus = FOCUS_NONE;
    sInside = INSIDE_NONE;
    /* The cursor where the player is, as CB2_OpenFlyMap has it. */
    sPickMapsec = PlayerRegionPosition(&x, &y);
    sPickX = x;
    sPickY = y;
    sPickPos = sPickMapsec != MAPSEC_NONE ? CtrRegionMap_PosWithinMapSec(x, y, sPickMapsec) : 0;
    CtrLog_Write(CTR_LOG_INPUT, "bottom screen: fly map by %u", slot);
}

static void EndFlyMap(void)
{
    sMapFly = FALSE;
    sMapZoom = FALSE;
    sPickMapsec = MAPSEC_NONE;
    /* The A or B that ended it is not the game's. */
    sSwallow = TRUE;
}

/* CANCEL: back to the party or the bag it came from, as the game's Fly map
 * goes back; on the map if the field is not free for it. */
static void CancelFlyMap(void)
{
    if (!sMapFly)
        return;
    EndFlyMap();
    PlaySE(SE_SELECT);
    OpenGameScreen(sFlyFrom);
}

/* FLY: CB_ExitFlyMap's destination, then the flight from the field as it is
 * (CtrParty_Fly). Only where the game's Fly map would let it go, with the
 * player standing in the field and nothing under way. */
static void FlyFromMap(void)
{
    if (!sShown.mapFly || !sShown.flyCanFly || sPickMapsec == MAPSEC_NONE)
        return;
    if (!FieldIdle() || !Overworld_MapTypeAllowsTeleportAndFly(gMapHeader.mapType))
    {
        PlaySE(SE_FAILURE);
        return;
    }
    PlaySE(SE_SELECT);
    CtrLog_Write(CTR_LOG_INPUT, "bottom screen: fly to %u (%u) by %u", sPickMapsec, sPickPos, sFlySlot);
    RememberFlyer(sFlySlot);
    CtrParty_Fly(sFlySlot, sPickMapsec, sPickPos);
    EndFlyMap();
}

/* The +Control Pad (or the Circle Pad) on the Fly map: a press moves the
 * pick (StepPickCell), held it goes on after a moment, as the game's Fly
 * map's cursor does, so the place can be chosen without the touch screen. */
#define FLY_DPAD_DELAY 16
#define FLY_DPAD_REPEAT 4
static u8 sFlyDpadFrames;

static void FlyMapDpad(u16 held, u16 down)
{
    if (down)
    {
        sFlyDpadFrames = 0;
        StepPickCell(down);
    }
    else if (!held)
        sFlyDpadFrames = 0;
    else if (++sFlyDpadFrames >= FLY_DPAD_DELAY)
    {
        sFlyDpadFrames = FLY_DPAD_DELAY - FLY_DPAD_REPEAT;
        StepPickCell(held);
    }
}

/* The map's own FLY: the destination as the Fly map's A would take it, and
 * the flight from the field with the Pokemon shown (CtrParty_Fly). Tapping
 * the town and then FLY is the choice; there is nothing more to confirm. */
static void MapFlyNow(void)
{
    u8 slot;

    if (sShown.flyNow != MAP_FLY_READY || sShown.mapFly || sPickMapsec == MAPSEC_NONE)
        return;
    slot = MapFlyerSlot(MapFlyers());
    /* The tap itself only with the player standing still, as the Fly map's
     * FLY (FlyFromMap): not mid-hop on a ledge, on ice or a spinner. */
    if (slot >= PARTY_SIZE || !FlagGet(FLAG_BADGE06_GET) || !MapFlyField()
     || gPlayerAvatar.tileTransitionState != T_NOT_MOVING
     || (CtrRegionMap_MapSecType(sPickMapsec) != MAPSECTYPE_CITY_CANFLY
      && CtrRegionMap_MapSecType(sPickMapsec) != MAPSECTYPE_BATTLE_FRONTIER))
    {
        PlaySE(SE_FAILURE);
        return;
    }
    PlaySE(SE_SELECT);
    CtrLog_Write(CTR_LOG_INPUT, "bottom screen: map FLY to %u (%u) by %u", sPickMapsec, sPickPos, slot);
    RememberFlyer(slot);
    CtrParty_Fly(slot, sPickMapsec, sPickPos);
    sPickMapsec = MAPSEC_NONE;
    sMapZoom = FALSE;
}

static void MapTap(u8 id)
{
    switch (id)
    {
    case HIT_MAP:
        PickMapCell(sTouch.lastX, sTouch.lastY);
        break;
    case HIT_MAP_ZOOM:
        if (sShown.encView)
            break;
        sMapZoom = !sMapZoom;
        if (sMapZoom)
        {
            u8 x, y;

            if (sPickMapsec != MAPSEC_NONE)
                MapZoomTo(sPickX, sPickY, PickPanelWidth(sPickMapsec, sPickPos));
            else if (PlayerRegionPosition(&x, &y) != MAPSEC_NONE)
                MapZoomTo(x, y, 0);
            else
                MapZoomTo(MAP_PIC_W / 16, MAP_PIC_H / 16, 0);
        }
        PlaySE(SE_SELECT);
        break;
    case HIT_MAP_FLY:
        FlyFromMap();
        break;
    case HIT_MAP_FLY_CANCEL:
        CancelFlyMap();
        break;
    case HIT_MAP_FLY_NOW:
        MapFlyNow();
        break;
    case HIT_MAP_PANEL:
        break;
    default:
        if (id >= HIT_MAP_FLYER && id < HIT_MAP_FLYER + PARTY_SIZE)
        {
            if (sShown.flyNow == MAP_FLY_READY && ((sShown.flyers >> (id - HIT_MAP_FLYER)) & 1))
            {
                RememberFlyer(id - HIT_MAP_FLYER);
                PlaySE(SE_SELECT);
            }
            break;
        }
        MapExtrasTap(id);
        break;
    }
}
