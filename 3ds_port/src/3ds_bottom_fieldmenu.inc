/* Part of 3ds_bottom_ui.c: the game's own menus and lists as buttons (included after 3ds_bottom_pad.inc, whose arrows it uses). */

/*
 * A menu the game is asking an answer of in the field - a script's choice, a
 * yes/no, the PC's list - drawn as buttons over the section's area, with the
 * game's cursor on the one it is on. What each says and which is chosen are
 * the game's (CtrFieldMenuView, menu.c): a tap is the entry chosen as A would
 * choose it, and the button for B is offered too.
 */
enum
{
    HIT_FM_ENTRY = HIT_ACTION,  /* + entry */
    HIT_FM_CANCEL = HIT_CANCEL,
};

#define FM_GAP 4
#define FM_CANCEL_H 26

static void DrawFieldMenu(const ViewState *s)
{
    const struct CtrFieldMenuView *v = &s->fm;
    int n = v->count, cols = v->columns > 1 ? v->columns : n <= 5 ? 1 : n <= 10 ? 2 : 3, rows, room, bh, bw;
    bool8 cancel = !v->yesNo;

    if (v->yesNo || n == 2)
        cols = 2;
    rows = (n + cols - 1) / cols;
    room = AREA_H - (cancel ? FM_CANCEL_H + FM_GAP : 0);
    bh = (room - (rows - 1) * FM_GAP) / rows;
    if (bh > 40)
        bh = 40;
    bw = (AREA_W - (cols - 1) * FM_GAP) / cols;
    for (int i = 0; i < n; ++i)
    {
        int c = i % cols, r = i / cols, x = AREA_X + c * (bw + FM_GAP), y = AREA_Y + r * (bh + FM_GAP), oy;
        bool8 pressed = s->pressed == HIT_FM_ENTRY + i;

        oy = Plate(x, y, bw, bh, pressed ? sEmerald : v->yesNo && i == 0 ? sSlate : v->yesNo ? sCancelGrey : sSlate,
                   pressed ? PLATE_STATE_PRESSED : v->cursor == i ? PLATE_STATE_FOCUS : PLATE_STATE_NORMAL);
        TextIn(v->text[i], x, oy, bw, bh, &sNormal, 'c', STYLE_WHITE, 4);
        AddHit(x, y, bw, bh, HIT_FM_ENTRY + i);
    }
    if (cancel)
    {
        int y = AREA_Y + AREA_H - FM_CANCEL_H, oy;
        bool8 pressed = s->pressed == HIT_FM_CANCEL;

        oy = Plate(AREA_X, y, AREA_W, FM_CANCEL_H, pressed ? sEmerald : sCancelGrey,
                   pressed ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL);
        TextIn(Ascii(CTR_TEXT("CANCEL", "CANCELAR", "ANNULER", "ANNULLA", "CANCELAR", "ABBRECHEN", "やめる")), AREA_X, oy,
               AREA_W, FM_CANCEL_H, &sNormal, 'c', STYLE_WHITE, 4);
        AddHit(AREA_X, y, AREA_W, FM_CANCEL_H, HIT_FM_CANCEL);
    }
}

static void FieldMenuTap(u8 id)
{
    if (id >= HIT_FM_ENTRY && id < HIT_FM_ENTRY + CTR_FM_MAX)
        CtrFieldMenu_Pick(id - HIT_FM_ENTRY);
    else if (id == HIT_FM_CANCEL)
        CtrFieldMenu_Pick(-1);
}

/*
 * A scrolling list the game is asking an answer of (a script's long choice,
 * the move relearner's moves, the Pokéblock case): the rows it shows as
 * buttons, the one the game's cursor is on lit, arrows to scroll it by a page
 * and a button for B. A row is chosen as A would choose it.
 */
enum
{
    HIT_FL_ROW = HIT_ACTION,        /* + row shown */
    HIT_FL_UP = HIT_ACTION + CTR_FL_ROWS,
    HIT_FL_DOWN,
};

#define FL_ROW_H 24
#define FL_ARROW_W 40

static void DrawFieldList(const ViewState *s)
{
    const struct CtrFieldListView *v = &s->fl;
    int listW = AREA_W - FL_ARROW_W - FM_GAP, oy;
    bool8 canUp = v->scroll > 0, canDown = v->scroll + v->count < v->total;

    for (int i = 0; i < v->count; ++i)
    {
        int y = AREA_Y + i * (FL_ROW_H + 2);
        bool8 header = (v->header >> i) & 1, pressed = s->pressed == HIT_FL_ROW + i;

        oy = Plate(AREA_X, y, listW, FL_ROW_H, pressed ? sEmerald : header ? sGroupOff : sCreamPlate,
                   pressed ? PLATE_STATE_PRESSED : v->selected == i && !header ? PLATE_STATE_FOCUS : PLATE_STATE_NORMAL);
        TextIn(v->text[i], AREA_X, oy, listW, FL_ROW_H, &sNormal, 'l', pressed ? STYLE_WHITE : header ? STYLE_GRAY : STYLE_INK, 8);
        if (!header)
            AddHit(AREA_X, y, listW, FL_ROW_H, HIT_FL_ROW + i);
    }
    for (int k = 0; k < 2; ++k)
    {
        int y = k ? AREA_Y + 60 : AREA_Y, x = AREA_X + listW + FM_GAP;
        bool8 on = k ? canDown : canUp, pressed = s->pressed == (k ? HIT_FL_DOWN : HIT_FL_UP);

        oy = Plate(x, y, FL_ARROW_W, 54, !on ? sOffGrey : pressed ? sEmerald : sSlate, pressed ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL);
        PadArrow(x + FL_ARROW_W / 2, oy + 27, k ? 1 : 0, PackRgb(255, 255, 255));
        if (on)
            AddHit(x, y, FL_ARROW_W, 54, k ? HIT_FL_DOWN : HIT_FL_UP);
    }
    {
        int y = AREA_Y + AREA_H - FM_CANCEL_H;
        bool8 pressed = s->pressed == HIT_FM_CANCEL;

        oy = Plate(AREA_X, y, AREA_W, FM_CANCEL_H, pressed ? sEmerald : sCancelGrey, pressed ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL);
        TextIn(Ascii(CTR_TEXT("CANCEL", "CANCELAR", "ANNULER", "ANNULLA", "CANCELAR", "ABBRECHEN", "やめる")), AREA_X, oy,
               AREA_W, FM_CANCEL_H, &sNormal, 'c', STYLE_WHITE, 4);
        AddHit(AREA_X, y, AREA_W, FM_CANCEL_H, HIT_FM_CANCEL);
    }
}

static void FieldListTap(const ViewState *s, u8 id)
{
    if (id >= HIT_FL_ROW && id < HIT_FL_ROW + CTR_FL_ROWS)
        CtrFieldList_Pick(CTR_FL_ROW, id - HIT_FL_ROW);
    else if (id == HIT_FL_UP)
        CtrFieldList_Pick(CTR_FL_SCROLL, -(s16)s->fl.count);
    else if (id == HIT_FL_DOWN)
        CtrFieldList_Pick(CTR_FL_SCROLL, s->fl.count);
    else if (id == HIT_FM_CANCEL)
        CtrFieldList_Pick(CTR_FL_CANCEL, 0);
}
