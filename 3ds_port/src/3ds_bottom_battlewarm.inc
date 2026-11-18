/* Part of 3ds_bottom_ui.c: the battle menus made ahead while the turn plays out. */

/*
 * While the turn plays out (the backdrop alone) the next menus are made
 * ready, one plate or label a frame: the action menu as it will come, the
 * moves of the mon on the left and, in a double battle, the target menu,
 * with the focus ring's two blink tones. The
 * menus are walked with an empty clip, so nothing is drawn: only the one
 * piece missing is built. When they come they are copied together instead
 * of worked out in the frame they appear in. A pressed plate is built when
 * it is first pressed.
 */
static u32 sWarmKey = 0xFFFFFFFF;
/* The menus the walk below is on, and how far it is: with the pictures freed
 * it starts again, even for the same menus (the next battle, same moves). */
static u32 sWarmIdxKey = 0xFFFFFFFF;
static bool8 sWarmDone;
#define WARM_SLICE_MS 1.5f
/*
 * The slice is what the frame leaves, not a fixed 1.5 ms: a 3D battle's frame
 * is 15-17 ms of work on an Old 3DS before any of this, and a fixed slice on
 * top of it dropped the frame (DROP bottom=6-7). Read off the last frame's
 * work less its bottom screen, with WARM_MARGIN_MS to spare; under
 * WARM_MIN_MS nothing is warmed, but after WARM_STARVED_FRAMES such frames
 * WARM_MIN_MS is taken anyway, so the menus still get made.
 */
#define WARM_FRAME_MS 16.7f
#define WARM_MARGIN_MS 1.0f
#define WARM_MIN_MS 0.4f
#define WARM_STARVED_FRAMES 20u
/* What a plate's picture (DrawCached in the warm-up) cost when last made. */
static float sWarmCompMs = 1.0f;
/*
 * A picture is made whole in its pass, and costs more than WARM_SLICE_MS: it
 * may take what the frame leaves, up to WARM_COMP_ROOM_MS. Held to the slice
 * it waited WARM_STARVED_FRAMES for each, and the action menu came up with
 * most of its looks missing.
 */
#define WARM_COMP_ROOM_MS 5.0f

/* The moves of `battler` as its move menu will show them. */
static void WarmMoves(struct ChooseMoveStruct *moves, u8 battler)
{
    for (int i = 0; i < MAX_MON_MOVES; ++i)
    {
        moves->moves[i] = gBattleMons[battler].moves[i];
        moves->currentPp[i] = gBattleMons[battler].pp[i];
        moves->maxPp[i] = CalculatePPWithBonus(gBattleMons[battler].moves[i], gBattleMons[battler].ppBonuses, i);
    }
}

/* The player's second mon in a double battle, whose moves the player picks
 * too, or MAX_BATTLERS_COUNT: not a double battle, a partner's mon (multi
 * battles, Steven's), or none there. */
static u8 WarmRightBattler(void)
{
    u8 right;

    if (!(gBattleTypeFlags & BATTLE_TYPE_DOUBLE) || (gBattleTypeFlags & (BATTLE_TYPE_MULTI | BATTLE_TYPE_INGAME_PARTNER)))
        return MAX_BATTLERS_COUNT;
    right = GetBattlerAtPosition(B_POSITION_PLAYER_RIGHT);
    if (right >= MAX_BATTLERS_COUNT || (gAbsentBattlerFlags & (1u << right)) || gBattleMons[right].hp == 0)
        return MAX_BATTLERS_COUNT;
    return right;
}

/* What the next menus will show, as the game will hand it to Snapshot. */
static void WarmView(ViewState *v, u8 battler)
{
    memset(v, 0, sizeof(*v));
    v->battler = battler;
    v->isDouble = (gBattleTypeFlags & BATTLE_TYPE_DOUBLE) != 0;
    v->safari = (gBattleTypeFlags & BATTLE_TYPE_SAFARI) != 0;
    v->quickBall = v->safari ? ITEM_NONE : CtrBattle_QuickBallItem();
    v->pressed = HIT_NONE;
    if (v->safari)
        v->safariBalls = gNumSafariBalls;
    if (v->quickBall != ITEM_NONE)
        v->quickBallCount = CountTotalItemQuantityInBag(v->quickBall);
    WarmMoves(&v->moves4, battler);
    SnapshotPartyBalls(v);
    /* The target plates as the mon on the left will be shown them: the one on
     * the right sees its own plate grey instead, and those two are made when
     * it chooses. */
    if (v->isDouble)
        SnapshotTargets(v, battler);
}

/* The battle's own menus all made (WarmBattleMenus): its frames may make the
 * party menu's and the bag's plates ahead (WarmScreens). */
static bool8 sBattleMenusReady;

static void WarmBattleMenus(void)
{
    static ViewState v, vr;
    uint64_t start;
    float slice;
    u8 battler = GetBattlerAtPosition(B_POSITION_PLAYER_LEFT);
    int ox = sOX, hits = sHitCount, clip[4] = {sClipX0, sClipY0, sClipX1, sClipY1};
    u32 key = 1;
    float room;

    if (!sBta.file || battler >= MAX_BATTLERS_COUNT || sBuildBudget != 0xFF)
        return;
    WarmView(&v, battler);
    /* The second mon's move menu, drawn from a copy holding its moves. */
    vr = v;
    vr.battler = WarmRightBattler();
    if (vr.battler < MAX_BATTLERS_COUNT)
        WarmMoves(&vr.moves4, vr.battler);
    key = Mix(key, vr.battler);
    for (int i = 0; i < MAX_MON_MOVES && vr.battler < MAX_BATTLERS_COUNT; ++i)
    {
        key = Mix(key, vr.moves4.moves[i]);
        key = Mix(key, vr.moves4.currentPp[i]);
        key = Mix(key, vr.moves4.maxPp[i]);
    }
    key = Mix(key, v.isDouble);
    key = Mix(key, v.safari);
    key = Mix(key, v.quickBall);
    key = Mix(key, v.quickBallCount);
    key = Mix(key, v.safariBalls);
    for (int i = 0; i < MAX_MON_MOVES; ++i)
    {
        key = Mix(key, v.moves4.moves[i]);
        key = Mix(key, v.moves4.currentPp[i]);
        key = Mix(key, v.moves4.maxPp[i]);
    }
    for (int i = 0; i < PARTY_SIZE; ++i)
        key = Mix(key, v.partyBalls[i]);
    /* A hit, a faint or a switch in the turn changes the target plates. */
    for (int i = 0; i < MAX_BATTLERS_COUNT && v.isDouble; ++i)
    {
        const TargetView *t = &v.targets[i];

        key = Mix(key, t->species | (t->iconSpecies << 16));
        key = Mix(key, t->hp | (t->maxHp << 16));
        key = Mix(key, t->level | (t->gender << 8) | (t->live << 16) | (t->deoxys << 24));
        for (int k = 0; k < POKEMON_NAME_LENGTH + 1; ++k)
            key = Mix(key, t->nick[k]);
    }
    sBattleMenusReady = key == sWarmKey;
    if (key == sWarmKey)
        return;
    {
        static unsigned starved;
        const CtrTiming *timing = CtrPlatform_GetTiming();

        slice = WARM_FRAME_MS - (timing->workMs - timing->bottomMs) - WARM_MARGIN_MS;
        room = slice < WARM_COMP_ROOM_MS ? slice : WARM_COMP_ROOM_MS;
        if (slice > WARM_SLICE_MS)
            slice = WARM_SLICE_MS;
        if (slice < WARM_MIN_MS)
        {
            if (++starved < WARM_STARVED_FRAMES)
                return;
            slice = WARM_MIN_MS;
        }
        starved = 0;
        if (room < slice)
            room = slice;
    }
    ResolveFonts();
    sOX = 0;
    sClipX0 = sClipY0 = sClipX1 = sClipY1 = 0;
    sBuildBudget = 1;
    /* A label is built a few milliseconds a frame, not all at once. */
    start = CtrPlatform_Ticks();
    sSliceStart = start;
    sSliceMs = slice;
    /* Every look a plate can have, in the order it is likely to be wanted: the
     * focus on each plate in both blink tones, then each plate pressed. First
     * the tints and labels (phase 0), then each look's picture (phase 1,
     * DrawCached): moving the cursor onto a plate that was never lit built its
     * tint in the frame it was lit, and drawing one is still blending. A
     * double battle's target menu and its second mon's moves come last:
     * built when they first came up, they took an Old 3DS's frame (a sharp
     * drop the first time a target, or the second mon's move, was chosen). */
    {
        static const u8 targetHits[] = {HIT_TARGET, HIT_TARGET + 1, HIT_TARGET + 2, HIT_TARGET + 3, HIT_CANCEL};
        static int idx;
        int nA = v.quickBall != ITEM_NONE ? 5 : 4, nM = MAX_MON_MOVES + 1;
        int nT = v.isDouble ? (int)ARRAY_COUNT(targetHits) : 0;
        int nR = vr.battler < MAX_BATTLERS_COUNT ? MAX_MON_MOVES : 0, per = nA + nM + nT + nR, total = per * 3;

        if (sWarmIdxKey != key)
        {
            sWarmIdxKey = key;
            idx = 0;
            sCompFailed = 0;
        }
        static unsigned compWait;
        bool8 first = TRUE;

        while (idx < total * 2 && sBuildBudget == 1
            && CtrPlatform_TickMs(CtrPlatform_Ticks() - start) < (idx >= total ? room : slice))
        {
            int c = idx % total, r = c % per;
            bool8 action = r < nA, right = r >= nA + nM + nT, target = !right && r >= nA + nM;
            int n = action ? r : right ? r - nA - nM - nT : target ? r - nA - nM : r - nA;
            ViewState *w = right ? &vr : &v;
            uint64_t passStart = CtrPlatform_Ticks();

            /* A plate's picture is made whole in its pass: only when what it
             * cost last fits, or, waited for long enough, as the frame's first. */
            if (idx >= total && CtrPlatform_TickMs(passStart - start) + sWarmCompMs > room)
            {
                if (!first || ++compWait < WARM_STARVED_FRAMES)
                    break;
            }
            compWait = 0;
            first = FALSE;

            sCompBuild = idx >= total;
            sCompBudget = 1;
            w->mode = action ? MODE_BATTLE_ACTION : target ? MODE_BATTLE_TARGET : MODE_BATTLE_MOVE;
            w->blink = c < per * 2 ? c / per : 0;
            w->cursor = 0;
            w->pressed = HIT_NONE;
            if (c >= per * 2)
                w->pressed = action ? (n == 4 ? HIT_QUICK_BALL : HIT_ACTION + n)
                           : target ? targetHits[n]
                           : (n == MAX_MON_MOVES ? HIT_CANCEL : HIT_MOVE + n);
            else
                w->cursor = n;
            if (action)
                DrawBattleActions(w);
            else if (target)
                DrawBattleTarget(w);
            else
                DrawBattleMoves(w);
            sCompBuild = FALSE;
            if (idx >= total && sCompBudget == 0)
            {
                float ms = CtrPlatform_TickMs(CtrPlatform_Ticks() - passStart);

                sWarmCompMs = ms > sWarmCompMs ? ms : sWarmCompMs * 0.8f + ms * 0.2f;
            }
            /* A walk that built something is made again next frame, to be sure. */
            if (sBuildBudget == 1 && sCompBudget == 1)
                ++idx;
            else if (sCompFailed > 8)
                idx = total * 2;   /* the pictures will not go in: the menus draw as before */
        }
        sWarmDone = idx >= total * 2;
    }
    sSliceMs = 0.0f;
    /* A pass that built nothing: all of it is ready. */
    if (sBuildBudget == 1 && !sJob.t && sWarmDone)
        sWarmKey = key;
    sBuildBudget = 0xFF;
    sOX = ox;
    sHitCount = hits;
    sClipX0 = clip[0];
    sClipY0 = clip[1];
    sClipX1 = clip[2];
    sClipY1 = clip[3];
}

/*
 * What an idle field frame may spend making a cache ahead: the slice
 * WarmBattleMenus takes, what the last frame's work leaves, at most
 * WARM_SLICE_MS. FALSE while that leaves less than WARM_MIN_MS, until the
 * warm-up has been starved WARM_STARVED_FRAMES frames: then WARM_MIN_MS.
 */
static bool8 IdleSlice(unsigned *starved, float *slice)
{
    const CtrTiming *timing = CtrPlatform_GetTiming();

    *slice = WARM_FRAME_MS - (timing->workMs - timing->bottomMs) - WARM_MARGIN_MS;
    if (*slice > WARM_SLICE_MS)
        *slice = WARM_SLICE_MS;
    if (*slice < WARM_MIN_MS)
    {
        if (++*starved < WARM_STARVED_FRAMES)
            return FALSE;
        *slice = WARM_MIN_MS;
    }
    *starved = 0;
    return TRUE;
}

/* The Poke Balls pocket slot of the first stack of `ball`, or the capacity. */
static u32 FirstBallStack(u16 ball)
{
    u32 i = 0;

    while (i < gBagPockets[BALLS_POCKET].capacity && gBagPockets[BALLS_POCKET].itemSlots[i].itemId != ball)
        ++i;
    return i;
}

/* The ball a press of the plate's arrow would choose: the next one in the
 * Poke Balls pocket that the bag holds, either way round, from the first
 * stack of the current one, as CtrBattle_QuickBallCycle walks it (every ball
 * counts once, at its first stack). ITEM_NONE if none is. */
static u16 NeighbourBall(u16 current, s8 direction)
{
    u32 capacity = gBagPockets[BALLS_POCKET].capacity, at = FirstBallStack(current);

    if (at == capacity)
        return ITEM_NONE;
    for (u32 step = 1; step < capacity; ++step)
    {
        u32 slot = (at + (direction > 0 ? step : capacity - step)) % capacity;
        u16 ball = gBagPockets[BALLS_POCKET].itemSlots[slot].itemId;

        if (ball >= FIRST_BALL && ball <= LAST_BALL && FirstBallStack(ball) == slot && CheckBagHasItem(ball, 1))
            return ball;
    }
    return ITEM_NONE;
}

/* The ball WarmQuickBalls made ready the neighbours of (ITEM_NONE: none). */
static u16 sQuickWarmBall = ITEM_NONE;

/*
 * While the action menu waits, the plates of the balls either arrow would
 * bring are made ready: their icons (one a frame) and their name and count,
 * with the slice an idle frame leaves. A new ball is then its plate drawn
 * from what is ready, not built in the frame it is chosen. Once all of it
 * is, nothing more is asked until another ball is chosen.
 */
/* TRUE: nothing of the quick balls' left to make (or none to make). */
static bool8 WarmQuickBalls(const ViewState *s)
{
    static unsigned starved;
    static u16 count, balls[2], counts[2];
    static bool8 ready;
    int ox = sOX, hits = sHitCount, clip[4] = {sClipX0, sClipY0, sClipX1, sClipY1};
    bool8 icons = TRUE;
    float slice;

    if (s->mode != MODE_BATTLE_ACTION || s->safari || s->quickBall == ITEM_NONE || sBuildBudget != 0xFF)
        return sBuildBudget == 0xFF;
    if (sQuickWarmBall != s->quickBall || count != s->quickBallCount)
    {
        sQuickWarmBall = s->quickBall;
        count = s->quickBallCount;
        for (int i = 0; i < 2; ++i)
        {
            balls[i] = NeighbourBall(sQuickWarmBall, i == 0 ? 1 : -1);
            counts[i] = balls[i] != ITEM_NONE ? CountTotalItemQuantityInBag(balls[i]) : 0;
        }
        ready = FALSE;
    }
    if (ready)
        return TRUE;
    sIconBudget = TRUE;
    for (int i = 0; i < 2; ++i)
        if (balls[i] != ITEM_NONE && ItemIcon(balls[i]) < 0 && !sIconBudget)
            icons = FALSE;
    sIconBudget = FALSE;
    if (!IdleSlice(&starved, &slice))
        return FALSE;
    ResolveFonts();
    sOX = 0;
    sClipX0 = sClipY0 = sClipX1 = sClipY1 = 0;
    sBuildBudget = 1;
    sSliceStart = CtrPlatform_Ticks();
    sSliceMs = slice;
    for (int i = 0; i < 2 && sBuildBudget == 1; ++i)
        if (balls[i] != ITEM_NONE)
            QuickBallLabels(balls[i], counts[i], ACT_Y);
    /* Nothing was left to build, and the icons were in before this frame. */
    ready = sBuildBudget == 1 && icons;
    sSliceMs = 0.0f;
    sBuildBudget = 0xFF;
    sOX = ox;
    sHitCount = hits;
    sClipX0 = clip[0];
    sClipY0 = clip[1];
    sClipX1 = clip[2];
    sClipY1 = clip[3];
    return FALSE;
}
