/* Part of 3ds_bottom_ui.c: what changed between two snapshots, and the part redraw. */

/*
 * The clip a redraw can be limited to, when the new view differs from the
 * shown one only in things that touch one part of the screen: the button lit
 * by a press or the battle cursor, the clock, the Repel's steps, the
 * options' panel (a value, the focus), the options' tabs. FALSE when anything
 * else differs, or the part cannot be told.
 */
static bool8 DirtyRect(const ViewState *now, const ViewState *shown, Rect *r)
{
    static ViewState probe;
    bool8 battle = now->mode >= MODE_BATTLE_INFO;
    bool8 iconsOnly = FALSE;

    if (now->mode != shown->mode || now->screen != shown->screen || now->dock != shown->dock)
        return FALSE;
    /* The keyboards: the key, the cursor or the text that changed (3ds_bottom_naming.inc, _chat.inc). */
    if (now->mode == MODE_NAMING)
        return now->chat == shown->chat && (now->chat ? ChatDirty(now, shown, r) : NamingDirty(now, shown, r));
    if (!battle && !now->dock)
        return FALSE;
    probe = *now;
    probe.pressed = shown->pressed;
    if (battle)
    {
        probe.cursor = shown->cursor;
        probe.blink = shown->blink;
        /* Another ball chosen: its plate alone (both draw one, of one size). */
        if (now->mode == MODE_BATTLE_ACTION && !now->safari && now->quickBall != ITEM_NONE
         && shown->quickBall != ITEM_NONE)
        {
            probe.quickBall = shown->quickBall;
            probe.quickBallCount = shown->quickBallCount;
        }
        if (memcmp(&probe, shown, sizeof(probe)) != 0)
            return FALSE;
        RectInit(r);
        /* A plate pressed or let go, the cursor moved, its ring blinked. */
        if (now->pressed != shown->pressed
         && !((now->pressed == HIT_NONE || RectAddPlate(r, now->pressed))
              && (shown->pressed == HIT_NONE || RectAddPlate(r, shown->pressed))))
            return FALSE;
        if ((now->cursor != shown->cursor || now->blink != shown->blink)
         && !(RectAddPlate(r, BattleCursorHit(now, now->cursor)) && RectAddPlate(r, BattleCursorHit(now, shown->cursor))))
            return FALSE;
        if ((now->quickBall != shown->quickBall || now->quickBallCount != shown->quickBallCount)
         && !RectAddPlate(r, HIT_QUICK_BALL))
            return FALSE;
        return r->x0 < r->x1 && r->y0 < r->y1;
    }

    /* The header's clock and Repel. */
    probe.clockShown = shown->clockShown;
    probe.clockValid = shown->clockValid;
    probe.clockRate = shown->clockRate;
    probe.clockPhase = shown->clockPhase;
    probe.clockMinute = shown->clockMinute;
    probe.repelSteps = shown->repelSteps;
    probe.focus = shown->focus;
    /* The area's name on the title, crossing into another: off the MAP
     * (whose mark moves with it, MoveMapCursor) and the SAVE file (which
     * shows it in its panel), it is on the title alone. */
    if (now->screen != SCR_MAP && now->screen != SCR_SAVE)
        probe.mapsec = shown->mapsec;
    /* The options' panel, and their tabs. */
    if (now->screen == SCR_OPTION)
    {
        memcpy(probe.options, shown->options, sizeof(probe.options));
        memcpy(probe.rows, shown->rows, sizeof(probe.rows));
        probe.optFocus = shown->optFocus;
        probe.optPage = shown->optPage;
        probe.optSub = shown->optSub;
    }
    /* A native view of the game's screen: its area. */
    if (now->native && now->native == shown->native)
    {
        probe.pv = shown->pv;
        probe.sv = shown->sv;
        probe.bv = shown->bv;
        probe.dx = shown->dx;
        memcpy(probe.party, shown->party, sizeof(probe.party));
    }
    if (memcmp(&probe, shown, sizeof(probe)) != 0)
        return FALSE;
    RectInit(r);
    /* The party menu's icons moving, and nothing else of it: theirs alone.
     * A slot whose icon is not drawn (all but the chosen one over the menu)
     * goes on animating unseen: nothing to draw for it, not a whole redraw. */
    if (now->native == NATIVE_PARTY && memcmp(&now->pv, &shown->pv, offsetof(struct CtrPartyView, iconFrame)) == 0
     && memcmp(now->party, shown->party, sizeof(now->party)) == 0)
    {
        iconsOnly = TRUE;
        for (int i = 0; i < PARTY_SIZE; ++i)
        {
            int x0, y0, x1, y1;

            if (now->pv.iconFrame[i] == shown->pv.iconFrame[i] && now->pv.iconLift[i] == shown->pv.iconLift[i])
                continue;
            if (PartyIconRect(now, i, &x0, &y0, &x1, &y1))
                RectAdd(r, x0, y0, x1, y1);
        }
    }
    /* The summary's mon moving with its animation, and nothing else of it:
     * the picture's part of its plate alone. */
    else if (now->native == NATIVE_SUMMARY
          && memcmp(&now->sv, &shown->sv, offsetof(struct CtrSummaryView, picTiles)) == 0
          && memcmp(&now->pv, &shown->pv, sizeof(now->pv)) == 0 && memcmp(&now->bv, &shown->bv, sizeof(now->bv)) == 0
          && memcmp(&now->dx, &shown->dx, sizeof(now->dx)) == 0
          && memcmp(now->party, shown->party, sizeof(now->party)) == 0)
    {
        if (memcmp(&now->sv, &shown->sv, sizeof(now->sv)) != 0)
            RectAdd(r, AREA_X, SUM_Y, AREA_X + SUM_LEFT_W, SUM_PIC_BOTTOM);
    }
    else if (now->native && (memcmp(&now->pv, &shown->pv, sizeof(now->pv)) != 0
                        || memcmp(&now->sv, &shown->sv, sizeof(now->sv)) != 0
                        || memcmp(&now->bv, &shown->bv, sizeof(now->bv)) != 0
                        || memcmp(&now->dx, &shown->dx, sizeof(now->dx)) != 0
                        || memcmp(now->party, shown->party, sizeof(now->party)) != 0))
        AreaRect(r);
    if (now->clockShown != shown->clockShown || now->clockValid != shown->clockValid
     || now->clockRate != shown->clockRate || now->clockPhase != shown->clockPhase
     || now->clockMinute != shown->clockMinute)
    {
        int x0, y0, x1, y1;

        ClockRect(&x0, &y0, &x1, &y1);
        RectAdd(r, x0, y0, x1, y1);
    }
    if (now->repelSteps != shown->repelSteps)
    {
        int x0, y0, x1, y1;

        RepelRect(&x0, &y0, &x1, &y1);
        RectAdd(r, x0, y0, x1, y1);
    }
    if (now->mapsec != shown->mapsec)
    {
        int x0, y0, x1, y1;

        TitleRect(now, &x0, &y0, &x1, &y1);
        RectAdd(r, x0, y0, x1, y1);
    }
    if (now->pressed != shown->pressed && !(RectAddHit(r, now->pressed) && RectAddHit(r, shown->pressed)))
        return FALSE;
    if (now->focus != shown->focus && !(RectAddFocus(r, now, now->focus) && RectAddFocus(r, shown, shown->focus)))
        return FALSE;
    if (now->screen == SCR_OPTION)
    {
        if (now->optPage != shown->optPage || now->optSub != shown->optSub)
            AreaRect(r);
        else if (memcmp(now->options, shown->options, sizeof(now->options)) != 0
              || memcmp(now->rows, shown->rows, sizeof(now->rows)) != 0 || now->optFocus != shown->optFocus)
        {
            int x0, y0, x1, y1;

            OptionsPanelRect(&x0, &y0, &x1, &y1);
            RectAdd(r, x0, y0, x1, y1);
        }
    }
    /* Only hidden icons moved: TRUE with an empty rect, which draws nothing. */
    return (r->x0 < r->x1 && r->y0 < r->y1) || iconsOnly;
}


/* Draws one part of the screen. The canvas outside it keeps what is shown. */
static void RenderPart(const ViewState *s, int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    if (x0 >= x1 || y0 >= y1)
        return;
    sClipX0 = x0;
    sClipY0 = y0;
    sClipX1 = x1;
    sClipY1 = y1;
    Render(s);
    sClipX0 = sClipY0 = 0;
    sClipX1 = W;
    sClipY1 = H;
}
