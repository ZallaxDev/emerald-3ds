/* Part of 3ds_bottom_ui.c: the rectangle a change needs drawn again. */

/* ------------------------------------------------------------------------ */
/* Partial redraws                                                          */
/* ------------------------------------------------------------------------ */

typedef struct { int x0, y0, x1, y1; } Rect;

static void RectInit(Rect *r)
{
    r->x0 = r->y0 = W;
    r->x1 = r->y1 = 0;
}

static void RectAdd(Rect *r, int x0, int y0, int x1, int y1)
{
    if (x0 < r->x0) r->x0 = x0;
    if (y0 < r->y0) r->y0 = y0;
    if (x1 > r->x1) r->x1 = x1;
    if (y1 > r->y1) r->y1 = y1;
}

/* A button's rectangle, as the last redraw laid it out, with the room its
 * plate's ring and shadow take; every hit of the id counts. FALSE when there
 * is none. */
static bool8 RectAddHitOf(Rect *r, u8 id)
{
    bool8 found = FALSE;

    for (int i = 0; i < sHitCount; ++i)
        if (sHits[i].id == id)
        {
            RectAdd(r, sHits[i].x - PLATE_ML, sHits[i].y - PLATE_MT, sHits[i].x + sHits[i].w + PLATE_MR,
                    sHits[i].y + sHits[i].h + PLATE_MB);
            found = TRUE;
        }
    return found;
}

static bool8 RectAddHit(Rect *r, u8 id)
{
    if (id == HIT_NONE)
        return TRUE;
    /* An option row is lit whole, whichever of its two halves is touched. */
    if (id >= HIT_OPTION && id < HIT_OPTION + 2 * HIT_OPTION_BACK)
    {
        u8 row = (id - HIT_OPTION) % HIT_OPTION_BACK;

        /* An action row has no back half: its one hit is enough. */
        bool8 forward = RectAddHitOf(r, HIT_OPTION + row);
        bool8 backward = RectAddHitOf(r, HIT_OPTION + HIT_OPTION_BACK + row);

        return forward || backward;
    }
    /* A dock button held names its section on the title. */
    if (id >= HIT_COLUMN && id < HIT_COLUMN + SCR_COUNT && sShown.dock)
    {
        int x0, y0, x1, y1;

        TitleRect(&sShown, &x0, &y0, &x1, &y1);
        RectAdd(r, x0, y0, x1, y1);
    }
    return RectAddHitOf(r, id);
}

/* A battle plate's hit, with the room its ring and shadow take. */
static bool8 RectAddPlate(Rect *r, u8 id)
{
    Rect plate;

    RectInit(&plate);
    if (id == HIT_QUICK_BALL_PREV || id == HIT_QUICK_BALL_NEXT)
        id = HIT_QUICK_BALL;
    if (!RectAddHitOf(&plate, id))
        return FALSE;
    RectAdd(r, plate.x0 - 4, plate.y0 - 4, plate.x1 + 4, plate.y1 + 6);
    return TRUE;
}

/* The battle cursor's plate: the action (or the quick ball, 4), the move
 * (or CANCEL), or the target. */
static u8 BattleCursorHit(const ViewState *s, u8 cursor)
{
    if (s->mode == MODE_BATTLE_ACTION)
        return cursor == 4 ? HIT_QUICK_BALL : HIT_ACTION + cursor;
    if (s->mode == MODE_BATTLE_MOVE)
        return cursor == MAX_MON_MOVES ? HIT_CANCEL : HIT_MOVE + cursor;
    return HIT_TARGET + cursor;
}

/* The content area, its plates' shadows and rings included. */
static void AreaRect(Rect *r)
{
    RectAdd(r, AREA_X - PLATE_ML, AREA_Y - PLATE_MT, AREA_X + AREA_W + PLATE_MR, AREA_Y + AREA_H + PLATE_MB);
}

/* What a focus on the dock or the header lights. FALSE when it cannot be told. */
static bool8 RectAddFocus(Rect *r, const ViewState *s, u8 focus)
{
    int x0, y0, x1, y1;

    if (focus == FOCUS_NONE)
        return TRUE;
    if (focus == COL_TITLE)
    {
        TitleRect(s, &x0, &y0, &x1, &y1);
        RectAdd(r, x0, y0, x1, y1);
        return TRUE;
    }
    if (focus < SCR_COUNT)
    {
        DockButton(s, focus, &x0, &x1);
        PlateBounds(x0, DOCK_TOP, x1, DOCK_H, &x0, &y0, &x1, &y1);
        RectAdd(r, x0, y0, x1, y1);
        return TRUE;
    }
    return RectAddHit(r, HIT_COLUMN + focus);
}
