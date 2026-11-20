/* Part of 3ds_bottom_ui.c: what the battle's controllers ask of the bottom screen and take from it. */

/* ------------------------------------------------------------------------ */
/* The battle menus                                                         */
/* ------------------------------------------------------------------------ */

/*
 * The action, move and target menus are the bottom screen's: the controllers
 * do not draw theirs (the top keeps its message box) and read their keys
 * through these, which move the game's cursor in the bottom screen's order
 * and turn a tap into the key that confirms it. What each choice does stays
 * the controller's own code.
 */
bool8 CtrBattleMenu_Active(void)
{
    /* Link battles too (local wireless): the player's choices go through the
     * same controller. Drawn on the top screen, the menu ran into the wide
     * text box. Not for a recorded battle played back: nobody chooses. */
    return sRes.ready && !(gBattleTypeFlags & BATTLE_TYPE_RECORDED);
}

void CtrBattleMenu_Begin(void)
{
    sBattleTap = HIT_NONE;
    sMoveCancel = FALSE;
    sQuickBallTap = FALSE;
    sQuickBallFocus = FALSE;
}

/* The ball button once per tap, or X once per press (R and L are the battle
 * speed's): the action handler throws the ball. X is free in a battle, the
 * column that takes it in the field is not shown. */
bool8 CtrBattleMenu_TakeQuickBall(void)
{
    bool8 tapped = sQuickBallTap || (CtrInput_Get()->physicalDown & CTR_KEY_X) != 0;

    sQuickBallTap = FALSE;
    return tapped;
}

/* "What will X do?", in the message box: the one thing left on top. */
void CtrBattleMenu_ShowPrompt(void)
{
    /* The printer reads it while it prints: a copy of its own. */
    static u8 prompt[64];
    int i;

    for (i = 0; i < (int)sizeof(prompt) - 1 && gDisplayedStringBattle[i] != EOS; ++i)
        prompt[i] = gDisplayedStringBattle[i];
    prompt[i] = EOS;
    BattlePutTextOnWindow(prompt, B_WIN_MSG);
}

/* Wally's controller picks by itself: the menu is shown, a tap is dropped. */
static void ShowBattleAsk(u8 kind)
{
    sBattleTap = HIT_NONE;
    sQuickBallFocus = FALSE;
    sAsk.kind = kind;
    sAsk.battler = gActiveBattler;
}

void CtrBattleMenu_ShowAction(void)
{
    ShowBattleAsk(ASK_ACTION);
}

void CtrBattleMenu_ShowMove(void)
{
    ShowBattleAsk(ASK_MOVE);
}

static u8 TakeBattleTap(u8 kind)
{
    u8 tap = sBattleTap;

    sBattleTap = HIT_NONE;
    sAsk.kind = kind;
    sAsk.battler = gActiveBattler;
    return tap;
}

void CtrBattleMenu_ActionInput(u8 *cursor, bool8 safari)
{
    u8 tap = TakeBattleTap(ASK_ACTION), next = *cursor;
    u16 dpad = gMain.newKeys & DPAD_ANY;
    /* FIGHT on top; BAG, POKéMON and RUN in a row under it. The ball
     * button, right of FIGHT and over RUN, is reached from both. */
    bool8 ball = !safari && CtrBattle_QuickBallItem() != ITEM_NONE;

    gMain.newKeys &= ~DPAD_ANY;
    if (!ball)
        sQuickBallFocus = FALSE;
    if (sQuickBallFocus)
    {
        if (dpad & (DPAD_LEFT | DPAD_DOWN))
        {
            sQuickBallFocus = FALSE;
            next = (dpad & DPAD_LEFT) ? 0 : 3;
            PlaySE(SE_SELECT);
            *cursor = next;
        }
        else if (dpad & DPAD_RIGHT)
        {
            if (CtrBattle_QuickBallCycle(1))
                PlaySE(SE_SELECT);
        }
        else if (gMain.newKeys & A_BUTTON)
        {
            gMain.newKeys &= ~A_BUTTON;
            sQuickBallTap = TRUE;
        }
        dpad = 0;
    }
    else if (ball && (((dpad & DPAD_RIGHT) && next == 0) || ((dpad & DPAD_UP) && next == 3)))
    {
        sQuickBallFocus = TRUE;
        PlaySE(SE_SELECT);
        dpad = 0;
    }
    if (dpad & DPAD_UP)
        next = 0;
    else if ((dpad & DPAD_DOWN) && next == 0)
        next = 2;
    else if ((dpad & DPAD_LEFT) && next > 1)
        --next;
    else if ((dpad & DPAD_RIGHT) && next != 0 && next < 3)
        ++next;
    if (next != *cursor)
    {
        PlaySE(SE_SELECT);
        *cursor = next;
    }
    if (tap == HIT_QUICK_BALL && !safari)
        sQuickBallTap = TRUE;
    else if (tap == HIT_QUICK_BALL_PREV || tap == HIT_QUICK_BALL_NEXT)
    {
        if (CtrBattle_QuickBallCycle(tap == HIT_QUICK_BALL_PREV ? -1 : 1))
            PlaySE(SE_SELECT);
        sQuickBallFocus = TRUE;
    }
    if (tap >= HIT_ACTION && tap < HIT_ACTION + 4)
    {
        sQuickBallFocus = FALSE;
        *cursor = tap - HIT_ACTION;
        gMain.newKeys |= A_BUTTON;
    }
    /* BAG and POKéMON open menus that run hidden. */
    if ((gMain.newKeys & A_BUTTON) && !safari && (*cursor == 1 || *cursor == 2))
        BeginSession(TRUE);
}

void CtrBattleMenu_MoveInput(u8 *cursor, const u16 *moves)
{
    u8 tap = TakeBattleTap(ASK_MOVE), next = *cursor, count = 0;
    u16 dpad = gMain.newKeys & DPAD_ANY;
    bool8 cancel = sMoveCancel;

    for (int i = 0; i < MAX_MON_MOVES; ++i)
        if (moves[i] != MOVE_NONE)
            ++count;
    /* The moves two by two, CANCEL under them. No reordering (SELECT). */
    gMain.newKeys &= ~(DPAD_ANY | SELECT_BUTTON);
    if (cancel)
    {
        if (dpad & DPAD_UP)
            cancel = FALSE;
    }
    else if (dpad & DPAD_UP)
    {
        if (next & 2)
            next ^= 2;
    }
    else if (dpad & DPAD_DOWN)
    {
        if (!(next & 2) && (next ^ 2) < count)
            next ^= 2;
        else
            cancel = TRUE;
    }
    else if (dpad & DPAD_LEFT)
    {
        if (next & 1)
            next ^= 1;
    }
    else if (dpad & DPAD_RIGHT)
    {
        if (!(next & 1) && (next ^ 1) < count)
            next ^= 1;
    }
    if (next != *cursor || cancel != sMoveCancel)
        PlaySE(SE_SELECT);
    *cursor = next;
    sMoveCancel = cancel;
    if (tap >= HIT_MOVE && tap < HIT_MOVE + MAX_MON_MOVES && moves[tap - HIT_MOVE] != MOVE_NONE)
    {
        *cursor = tap - HIT_MOVE;
        sMoveCancel = FALSE;
        gMain.newKeys |= A_BUTTON;
    }
    else if (tap == HIT_CANCEL)
        gMain.newKeys |= B_BUTTON;
    /* A on CANCEL is B. */
    if (sMoveCancel && (gMain.newKeys & A_BUTTON))
        gMain.newKeys = (gMain.newKeys & ~A_BUTTON) | B_BUTTON;
}

/*
 * The target plates two by two as the battle stands on the top screen: the
 * opponents above (their right one is on the left), the player's side below.
 */
static const u8 sTargetPlatePos[MAX_BATTLERS_COUNT] = {
    B_POSITION_OPPONENT_RIGHT, B_POSITION_OPPONENT_LEFT, B_POSITION_PLAYER_LEFT, B_POSITION_PLAYER_RIGHT,
};

static u8 TargetPlate(u8 battler)
{
    for (int i = 0; i < MAX_BATTLERS_COUNT; ++i)
        if (sTargetPlatePos[i] == GetBattlerPosition(battler))
            return i;
    return 0;
}

/* HandleInputChooseTarget's test: someone there, and not the user itself
 * unless its move may be aimed at the user. */
static bool8 TargetLive(u8 battler, u8 user)
{
    u16 move;

    if (battler >= gBattlersCount || (gAbsentBattlerFlags & (1u << battler)))
        return FALSE;
    if (GetBattlerSide(battler) == B_SIDE_OPPONENT || battler != user)
        return TRUE;
    move = GetMonData(&gPlayerParty[gBattlerPartyIndexes[user]], MON_DATA_MOVE1 + gMoveSelectionCursor[user]);
    return (gBattleMoves[move].target & MOVE_TARGET_USER_OR_SELECTED) != 0;
}

/* The plate a press from plate moves to along axis (1 across, 2 up and
 * down); up and down take the other column when the plate above is empty. */
static u8 TargetStep(u8 plate, u8 axis, u8 user)
{
    u8 next = plate ^ axis;

    if (TargetLive(GetBattlerAtPosition(sTargetPlatePos[next]), user))
        return next;
    next ^= 1;
    if (axis == 2 && TargetLive(GetBattlerAtPosition(sTargetPlatePos[next]), user))
        return next;
    return plate;
}

/* What the game's left and right do to the cursor, straight to a battler. */
static void SetTargetCursor(u8 battler)
{
    if (battler == gMultiUsePlayerCursor)
        return;
    PlaySE(SE_SELECT);
    gSprites[gBattlerSpriteIds[gMultiUsePlayerCursor]].callback = SpriteCB_HideAsMoveTarget;
    EndBounceEffect(gMultiUsePlayerCursor, BOUNCE_HEALTHBOX);
    gMultiUsePlayerCursor = battler;
    gSprites[gBattlerSpriteIds[battler]].callback = SpriteCB_ShowAsMoveTarget;
}

void CtrBattleMenu_TargetInput(void)
{
    u8 tap = TakeBattleTap(ASK_TARGET), user = gActiveBattler, plate = TargetPlate(gMultiUsePlayerCursor), next = plate;
    u16 dpad = gMain.newKeys & DPAD_ANY;

    gMain.newKeys &= ~DPAD_ANY;
    if (dpad & (DPAD_UP | DPAD_DOWN))
        next = TargetStep(plate, 2, user);
    else if (dpad & (DPAD_LEFT | DPAD_RIGHT))
        next = TargetStep(plate, 1, user);
    if (next != plate)
        SetTargetCursor(GetBattlerAtPosition(sTargetPlatePos[next]));
    if (tap >= HIT_TARGET && tap < HIT_TARGET + MAX_BATTLERS_COUNT)
    {
        u8 battler = GetBattlerAtPosition(sTargetPlatePos[tap - HIT_TARGET]);

        if (TargetLive(battler, user))
        {
            SetTargetCursor(battler);
            gMain.newKeys |= A_BUTTON;
        }
    }
    else if (tap == HIT_CANCEL)
        gMain.newKeys |= B_BUTTON;
}
