/* Part of 3ds_bottom_ui.c: the battle menus drawn: actions, moves, targets. */

static void FightBody(const ViewState *s, int wide)
{
    int x = wide ? 6 : 18, w = wide ? 224 : 284;
    u8 state = PlateState(s, HIT_ACTION + 0, s->cursor == 0);
    int oy = DrawBattlePlate(wide ? BTA_FIGHT_WIDE : BTA_FIGHT_FULL, x, ACT_Y, w, ACT_H, sHueFight, state, s->blink,
                             HIT_ACTION + 0);
    Rgb dark = PLATE_DARK(sHueFight), shadow = PLATE_SHADOW(sHueFight);
    const u8 *label = ActionLabel(s->safari, 0);

    /* Rayquaza is in the plate's tint (BuildTint). */
    if (PlateOutsideClip(x, ACT_Y, w, ACT_H))
        return;
    if (s->safari)
    {
        DrawItemIcon(ITEM_SAFARI_BALL, x + w / 2 - 34, oy + ACT_H / 2 - 15);
        DrawSmoothStr(&sNormal, label, x + w / 2 - 4, oy + ACT_H / 2 - 16, 8, sCream, dark, &shadow);
        DrawSmoothStr(&sSmall, Times(s->safariBalls), x + w / 2 + 52, oy + ACT_H / 2 + 4, 4, sWhite, dark, NULL);
        return;
    }
    DrawSmoothStr(&sNormal, label, x + (60 + w - 64) / 2 - SmoothInkWidth(&sNormal, label, 10) / 2,
                  oy + ACT_H / 2 - 21, 10, sCream, dark, &shadow);
}

static void DrawFightPlate(const ViewState *s, bool8 wide)
{
    int x = wide ? 6 : 18, w = wide ? 224 : 284;
    u8 state = PlateState(s, HIT_ACTION + 0, s->cursor == 0);
    u32 stamp = Mix(Mix(Mix(1, wide), s->safari), s->safariBalls);

    DrawCached(CP_FIGHT, state, stamp, x, ACT_Y, wide ? BTA_FIGHT_WIDE : BTA_FIGHT_FULL, w, ACT_H, HIT_ACTION + 0,
               FightBody, s, wide);
}

/* The quick ball plate's name and count, its contents starting at oy. */
static void QuickBallLabels(u16 ball, u16 count, int oy)
{
    int x = 236, w = 78;
    Rgb dark = PLATE_DARK(sHueBall), shadow = PLATE_SHADOW(sHueBall);
    const u8 *name = GetItemName(ball), *times = Times(count);

    DrawSmoothStr(&sSmall, name, x + w / 2 - SmoothInkWidth(&sSmall, name, 4) / 2, oy + 6, 4, sWhite, dark, NULL);
    DrawSmoothStr(&sNormal, times, x + w / 2 - SmoothInkWidth(&sNormal, times, 4) / 2, oy + ACT_H - 16, 4,
                  sCream, dark, &shadow);
}

static void QuickBallBody(const ViewState *s, int unused)
{
    int x = 236, w = 78;

    (void)unused;
    u8 state = PlateState(s, HIT_QUICK_BALL, s->cursor == 4);
    int oy = DrawBattlePlate(BTA_BALL, x, ACT_Y, w, ACT_H, sHueBall, state, s->blink, HIT_QUICK_BALL);

    if (PlateOutsideClip(x, ACT_Y, w, ACT_H))
        return;
    DrawItemIconShadow(s->quickBall, x + 27, oy + ACT_H / 2 - 11);
    DrawItemIcon(s->quickBall, x + 26, oy + ACT_H / 2 - 13);
    QuickBallLabels(s->quickBall, s->quickBallCount, oy);
    /* The cycle arrows: translucent, the plate shows through. */
    for (int row = 0; row < 13; ++row)
    {
        int halfWidth = (row < 6 ? row : 12 - row) + 1;

        ShadeRect(x + 7 + 4 - halfWidth, oy + ACT_H / 2 - 6 + row, halfWidth + 1, 1);
        ShadeRect(x + w - 8 - 4, oy + ACT_H / 2 - 6 + row, halfWidth + 1, 1);
    }
}

static void DrawQuickBall(const ViewState *s)
{
    u8 state = PlateState(s, HIT_QUICK_BALL, s->cursor == 4);
    u32 stamp = Mix(Mix(2, s->quickBall), s->quickBallCount);

    DrawCached(CP_BALL, state, stamp, 236, ACT_Y, BTA_BALL, 78, ACT_H, HIT_QUICK_BALL, QuickBallBody, s, 0);
    AddHit(236, ACT_Y, 16, ACT_H, HIT_QUICK_BALL_PREV);
    AddHit(298, ACT_Y, 16, ACT_H, HIT_QUICK_BALL_NEXT);
}

static void ActionRowBody(const ViewState *s, int k)
{
    static const s16 rowX[3] = {6, 110, 214};
    const Rgb hues[3] = {sHueBag, sHueMon, sHueRun};
    int x = rowX[k], idx = k + 1;
    u8 state = PlateState(s, HIT_ACTION + idx, s->cursor == idx);
    int oy = DrawBattlePlate(BTA_BOTTOM, x, ROW_Y, ROW_W, ROW_H, hues[k], state, s->blink, HIT_ACTION + idx);
    const u8 *label = ActionLabel(s->safari, idx);

    if (PlateOutsideClip(x, ROW_Y, ROW_W, ROW_H))
        return;
    if (k == 1 && !s->safari)
        for (int b = 0; b < PARTY_SIZE; ++b)
            DrawBta(BTA_PARTY_OK + s->partyBalls[b], x + ROW_W - 82 + b * 14, oy + 18, hues[k], hues[k]);
    else if (s->safari)
        DrawBta(k == 0 ? BTA_ICON_BLOCK : k == 1 ? BTA_ICON_NEAR : BTA_ICON_RUN, x, oy, hues[k], hues[k]);
    else
        DrawBta(k == 0 ? BTA_ICON_BAG : BTA_ICON_RUN, x, oy, hues[k], hues[k]);
    DrawSmoothStr(&sNormal, label, x + 9, oy + ROW_H - 28, 6, PLATE_DARK(hues[k]), sCream, NULL);
}

static void DrawBattleActions(const ViewState *s)
{
    static const s16 rowX[3] = {6, 110, 214};
    bool8 ball = !s->safari && s->quickBall != ITEM_NONE;

    DrawFightPlate(s, ball);
    if (ball)
        DrawQuickBall(s);
    for (int k = 0; k < 3; ++k)
    {
        int idx = k + 1;
        u32 stamp = Mix(Mix(3 + k, s->safari), 0);

        if (k == 1)
            for (int b = 0; b < PARTY_SIZE; ++b)
                stamp = Mix(stamp, s->partyBalls[b]);
        DrawCached(CP_BAG + k, PlateState(s, HIT_ACTION + idx, s->cursor == idx), stamp, rowX[k], ROW_Y, BTA_BOTTOM,
                   ROW_W, ROW_H, HIT_ACTION + idx, ActionRowBody, s, k);
    }
}

/* The PP's colour as the game warns: at 0 red, then orange to a quarter,
 * yellow to half. */
static void PpColours(u8 pp, u8 maxPp, u16 *fg, u16 *shadow)
{
    if (pp == 0)
        *fg = TXT_RED, *shadow = TXT_LRED;
    else if (pp <= maxPp / 4)
        *fg = PackRgb(224, 112, 32), *shadow = PackRgb(248, 200, 152);
    else if (pp <= maxPp / 2)
        *fg = PackRgb(200, 160, 16), *shadow = PackRgb(248, 232, 152);
    else
        *fg = TXT_DARK, *shadow = TXT_LIGHT;
}

#define MOVE_W 150
#define MOVE_H 80

static void MoveBody(const ViewState *s, int i)
{
    int x = i & 1 ? 164 : 6, y = i & 2 ? 106 : 20, oy;
    u16 move = s->moves4.moves[i], fg, sh;
    const struct BattleMove *data = &gBattleMoves[move];
    Rgb colour, dark, shadow;
    u8 text[20];

    if (move == MOVE_NONE)
    {
        DrawBattlePlate(BTA_MOVE, x, y, MOVE_W, MOVE_H, sHueEmpty, BTA_NORMAL, 0, HIT_NONE);
        return;
    }
    colour = data->type < NUMBER_OF_MON_TYPES ? sTypeColour[data->type] : sHueEmpty;
    dark = PLATE_DARK(colour);
    shadow = PLATE_SHADOW(colour);
    oy = DrawBattlePlate(BTA_MOVE, x, y, MOVE_W, MOVE_H, colour, PlateState(s, HIT_MOVE + i, s->cursor == i),
                         s->blink, HIT_MOVE + i);
    if (PlateOutsideClip(x, y, MOVE_W, MOVE_H))
        return;
    DrawSmoothStr(&sNormal, gMoveNames[move], x + 10, oy + 5, 4, sWhite, dark, &shadow);
    DrawTypeIcon(data->type, x + 11, oy + 33);
    DrawCategoryIcon(move, x + 46, oy + 33);
    StringCopy(text, gText_MoveInterfacePP);
    StringAppend(text, Number(s->moves4.currentPp[i], 2, STR_CONV_MODE_RIGHT_ALIGN));
    StringAppend(text, gText_Slash);
    StringAppend(text, Number(s->moves4.maxPp[i], 2, STR_CONV_MODE_RIGHT_ALIGN));
    PpColours(s->moves4.currentPp[i], s->moves4.maxPp[i], &fg, &sh);
    DrawStrRight(&sNormal, text, x + MOVE_W - 12, oy + 32, fg, sh);
    {
        int tx = DrawStr(&sSmall, Ascii(CTR_TEXT("POW ", "POT. ", "PUISS. ", "POT. ", "POD. ", "STÄ. ", "いりょく ")), x + 12, oy + 56, TXT_DARK, TXT_LIGHT);

        DrawStr(&sSmall, data->power > 1 ? Number(data->power, 3, STR_CONV_MODE_LEFT_ALIGN) : Ascii("---"), tx,
                oy + 56, TXT_DARK, TXT_LIGHT);
        StringCopy(text, Ascii(CTR_TEXT("ACC ", "PREC. ", "PRÉC. ", "PREC. ", "PREC. ", "GEN. ", "めいちゅう ")));
        StringAppend(text, data->accuracy ? Number(data->accuracy, 3, STR_CONV_MODE_LEFT_ALIGN) : Ascii("---"));
        DrawStrRight(&sSmall, text, x + MOVE_W - 12, oy + 56, TXT_DARK, TXT_LIGHT);
    }
}

static void DrawMovePlate(const ViewState *s, int i)
{
    int x = i & 1 ? 164 : 6, y = i & 2 ? 106 : 20;
    int pid = (s->isDouble && GetBattlerPosition(s->battler) == B_POSITION_PLAYER_RIGHT ? CP_MOVE_RIGHT0 : CP_MOVE0) + i;
    u16 move = s->moves4.moves[i];
    /* The category icon follows ENHANCEMENTS' PHYS/SPEC SPLIT. */
    u32 stamp = Mix(Mix(Mix(Mix(Mix(4, move), s->moves4.currentPp[i]), s->moves4.maxPp[i]), i), MoveSplitEnabled());

    if (move == MOVE_NONE)
        MoveBody(s, i);
    else
        DrawCached(pid, PlateState(s, HIT_MOVE + i, s->cursor == i), stamp, x, y, BTA_MOVE, MOVE_W, MOVE_H,
                   HIT_MOVE + i, MoveBody, s, i);
}

static void CancelBody(const ViewState *s, int unused)
{
    int oy = DrawBattlePlate(BTA_CANCEL, 84, 192, 152, 26, sHueCancel,
                             PlateState(s, HIT_CANCEL, s->cursor == MAX_MON_MOVES), s->blink, HIT_CANCEL);

    (void)unused;
    DrawSmoothStr(&sNormal, gText_Cancel2, 160 - SmoothInkWidth(&sNormal, gText_Cancel2, 4) / 2, oy + 5, 4, sWhite,
                  PLATE_DARK(sHueCancel), NULL);
}

static void DrawBattleMoves(const ViewState *s)
{
    for (int i = 0; i < MAX_MON_MOVES; ++i)
        DrawMovePlate(s, i);
    DrawCached(CP_CANCEL, PlateState(s, HIT_CANCEL, s->cursor == MAX_MON_MOVES), 5, 84, 192, BTA_CANCEL, 152, 26,
               HIT_CANCEL, CancelBody, s, 0);
}

/*
 * The target choice: the battlers where the moves were, opponents above in
 * red, the player's side below in blue, each with its party icon, name,
 * level and HP (the figures for the player's side only, as its health boxes
 * have them). One the move cannot reach is grey through and through and
 * takes no tap; an empty place has no plate at all.
 */
static void TargetBody(const ViewState *s, int i)
{
    const TargetView *t = &s->targets[i];
    int x = i & 1 ? 164 : 6, y = i & 2 ? 106 : 20, oy, tx;
    Rgb colour = !t->live ? sHueEmpty : i < 2 ? sHueFight : sHueRun;
    Rgb dark = PLATE_DARK(colour), shadow = PLATE_SHADOW(colour);
    u16 dim = PackRgb(dark.r, dark.g, dark.b), faint = PackRgb(200, 200, 204);
    u8 text[16];

    oy = DrawBattlePlate(BTA_MOVE, x, y, MOVE_W, MOVE_H, colour,
                         t->live ? PlateState(s, HIT_TARGET + i, s->cursor == i) : BTA_NORMAL, s->blink,
                         t->live ? HIT_TARGET + i : HIT_NONE);
    if (!t->species || PlateOutsideClip(x, y, MOVE_W, MOVE_H))
        return;
    {
        const u8 *tiles = MonIcon(t->iconSpecies, t->deoxys);
        u8 pal = gMonIconPaletteIndices[t->iconSpecies];

        u16 grey[16];

        if (tiles && pal < 3)
        {
            if (!t->live)
                GreyPal(sRes.monIconPal[pal].c, grey);
            DrawSpriteCentred(tiles, 4, 4, 2 * (x + 26), 2 * (oy + 46), t->live ? sRes.monIconPal[pal].c : grey);
        }
    }
    DrawSmoothStr(&sNormal, t->nick, x + MOVE_W / 2 - SmoothInkWidth(&sNormal, t->nick, 4) / 2, oy + 5, 4, sWhite,
                  dark, &shadow);
    text[0] = CHAR_LV;
    StringCopy(text + 1, Number(t->level, 3, STR_CONV_MODE_LEFT_ALIGN));
    tx = DrawStr(&sSmall, text, x + 50, oy + 32, t->live ? TXT_DARK : dim, t->live ? TXT_LIGHT : faint);
    if (t->gender != MON_GENDERLESS && t->species != SPECIES_NIDORAN_F && t->species != SPECIES_NIDORAN_M)
    {
        text[0] = t->gender == MON_FEMALE ? CHAR_FEMALE : CHAR_MALE;
        text[1] = EOS;
        if (!t->live)
            DrawStr(&sSmall, text, tx + 4, oy + 32, dim, faint);
        else
            DrawStr(&sSmall, text, tx + 4, oy + 32, t->gender == MON_FEMALE ? TXT_RED : TXT_BLUE,
                    t->gender == MON_FEMALE ? TXT_LRED : TXT_LBLUE);
    }
    /* The health box's bar: green over half, yellow over a fifth, then red. */
    {
        int bx = x + 50, by = oy + 48, bw = 90, fill = t->maxHp ? (t->hp * (bw - 2) + t->maxHp - 1) / t->maxHp : 0;
        u16 bar = !t->live ? PackRgb(176, 176, 180) : t->hp * 2 > t->maxHp ? PackRgb(88, 208, 128)
                : t->hp * 5 > t->maxHp ? PackRgb(248, 224, 0) : PackRgb(248, 88, 40);

        FillRect(bx, by, bw, 6, dim);
        FillRect(bx + 1, by + 1, bw - 2, 4, t->live ? PackRgb(80, 104, 88) : PackRgb(120, 120, 126));
        FillRect(bx + 1, by + 1, fill, 4, bar);
    }
    if (GetBattlerSide(GetBattlerAtPosition(sTargetPlatePos[i])) == B_SIDE_PLAYER)
    {
        StringCopy(text, Number(t->hp, 3, STR_CONV_MODE_LEFT_ALIGN));
        StringAppend(text, gText_Slash);
        StringAppend(text, Number(t->maxHp, 3, STR_CONV_MODE_LEFT_ALIGN));
        DrawStrRight(&sSmall, text, x + MOVE_W - 10, oy + 56, t->live ? TXT_DARK : dim, t->live ? TXT_LIGHT : faint);
    }
}

static void DrawBattleTarget(const ViewState *s)
{
    for (int i = 0; i < MAX_BATTLERS_COUNT; ++i)
    {
        const TargetView *t = &s->targets[i];
        u32 stamp = Mix(Mix(Mix(Mix(Mix(7, i), t->iconSpecies | (t->deoxys << 16)), t->species),
                            t->hp | (t->maxHp << 16)), t->level | (t->gender << 8) | (t->live << 16));

        if (!t->species)
            continue;
        for (int k = 0; k < POKEMON_NAME_LENGTH + 1; ++k)
            stamp = Mix(stamp, t->nick[k]);
        DrawCached(CP_TARGET0 + i, t->live ? PlateState(s, HIT_TARGET + i, s->cursor == i) : BTA_NORMAL, stamp,
                   i & 1 ? 164 : 6, i & 2 ? 106 : 20, BTA_MOVE, MOVE_W, MOVE_H, t->live ? HIT_TARGET + i : HIT_NONE,
                   TargetBody, s, i);
    }
    DrawCached(CP_CANCEL, PlateState(s, HIT_CANCEL, FALSE), 5, 84, 192, BTA_CANCEL, 152, 26, HIT_CANCEL, CancelBody,
               s, 0);
}
