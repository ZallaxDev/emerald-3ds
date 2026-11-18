/* Part of 3ds_bottom_ui.c: the game's own menus and lists as buttons (included after 3ds_bottom_pad.inc, whose arrows it uses). */

/*
 * A menu the game is asking an answer of in the field - a script's choice, a
 * yes/no, the PC's list - drawn as buttons over the section's area, with the
 * game's cursor on the one it is on. What each says and which is chosen are
 * the game's (CtrFieldMenuView, menu.c): a tap is the entry chosen as A would
 * choose it, and the button for B is offered too unless the menu has no B.
 * While the game does not take an answer (the view is locked) the buttons stay
 * but greyed, the cursor's one lit, and nothing can be tapped.
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
    bool8 cancel = !v->yesNo && !v->noCancel, locked = v->locked;

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
        bool8 pressed = !locked && s->pressed == HIT_FM_ENTRY + i, lit = v->cursor == i;
        Rgb colour = locked && !lit ? sOffGrey : v->yesNo && i != 0 ? sCancelGrey : sSlate;

        oy = Plate(x, y, bw, bh, pressed ? sEmerald : colour,
                   pressed ? PLATE_STATE_PRESSED : lit ? PLATE_STATE_FOCUS : PLATE_STATE_NORMAL);
        TextIn(v->text[i], x, oy, bw, bh, &sNormal, 'c', STYLE_WHITE, 4);
        if (!locked)
            AddHit(x, y, bw, bh, HIT_FM_ENTRY + i);
    }
    if (cancel)
    {
        int y = AREA_Y + AREA_H - FM_CANCEL_H, oy;
        bool8 pressed = !locked && s->pressed == HIT_FM_CANCEL;

        oy = Plate(AREA_X, y, AREA_W, FM_CANCEL_H, pressed ? sEmerald : locked ? sOffGrey : sCancelGrey,
                   pressed ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL);
        TextIn(Ascii(CTR_TEXT("CANCEL", "CANCELAR", "ANNULER", "ANNULLA", "CANCELAR", "ABBRECHEN", "やめる")), AREA_X, oy,
               AREA_W, FM_CANCEL_H, &sNormal, 'c', STYLE_WHITE, 4);
        if (!locked)
            AddHit(AREA_X, y, AREA_W, FM_CANCEL_H, HIT_FM_CANCEL);
    }
}

static void FieldMenuTap(u8 id)
{
    /* The view tapped is the one on show: its serial says which menu that is. */
    if (id >= HIT_FM_ENTRY && id < HIT_FM_ENTRY + CTR_FM_MAX)
        CtrFieldMenu_Pick(id - HIT_FM_ENTRY, sShown.fm.serial);
    else if (id == HIT_FM_CANCEL)
        CtrFieldMenu_Pick(-1, sShown.fm.serial);
}

/*
 * A scrolling list the game is asking an answer of (a script's long choice,
 * the move relearner's moves, the Pokéblock case): the rows it shows as
 * buttons, the one the game's cursor is on lit, and in a column on the right
 * an arrow up, an arrow down (a page each) and a cross for B. A row is chosen
 * as A would choose it.
 *
 * The rows share the area's height, up to FL_ROW_PITCH apart: 1 to 6 rows
 * take 26 px each (24 px of plate), 7 rows 23 px, 8 rows 20 px (18 px of
 * plate); the last row ends at AREA_Y + rows * pitch - 2, never past the
 * area's bottom. The column's three buttons are 52 px tall, 4 px apart, and
 * span the area; the rows are beside it, so no zone covers another.
 */
enum
{
    HIT_FL_ROW = HIT_ACTION,        /* + row shown */
    HIT_FL_UP = HIT_ACTION + CTR_FL_ROWS,
    HIT_FL_DOWN,
};

#define FL_ROW_PITCH 26
#define FL_ARROW_W 40
#define FL_COLUMN_H ((AREA_H - 2 * FM_GAP) / 3)

/* A cross: two diagonals of 2 px squares, the list's button for B. */
static void FieldListCross(int cx, int cy, u16 c)
{
    for (int i = 0; i < 12; ++i)
    {
        FillRect(cx - 6 + i, cy - 6 + i, 2, 2, c);
        FillRect(cx + 5 - i, cy - 6 + i, 2, 2, c);
    }
}

static void DrawFieldList(const ViewState *s)
{
    const struct CtrFieldListView *v = &s->fl;
    int listW = AREA_W - FL_ARROW_W - FM_GAP, colX = AREA_X + listW + FM_GAP, oy;
    int pitch = v->count > 0 ? (AREA_H + 2) / v->count : FL_ROW_PITCH, rowH;
    bool8 canUp = v->scroll > 0, canDown = v->scroll + v->count < v->total;

    if (pitch > FL_ROW_PITCH)
        pitch = FL_ROW_PITCH;
    rowH = pitch - 2;
    for (int i = 0; i < v->count; ++i)
    {
        int y = AREA_Y + i * pitch;
        bool8 header = (v->header >> i) & 1, pressed = s->pressed == HIT_FL_ROW + i;

        oy = Plate(AREA_X, y, listW, rowH, pressed ? sEmerald : header ? sGroupOff : sCreamPlate,
                   pressed ? PLATE_STATE_PRESSED : v->selected == i && !header ? PLATE_STATE_FOCUS : PLATE_STATE_NORMAL);
        TextIn(v->text[i], AREA_X, oy, listW, rowH, &sNormal, 'l', pressed ? STYLE_WHITE : header ? STYLE_GRAY : STYLE_INK, 8);
        if (!header)
            AddHit(AREA_X, y, listW, rowH, HIT_FL_ROW + i);
    }
    for (int k = 0; k < 2; ++k)
    {
        int y = AREA_Y + k * (FL_COLUMN_H + FM_GAP);
        bool8 on = k ? canDown : canUp, pressed = s->pressed == (k ? HIT_FL_DOWN : HIT_FL_UP);

        oy = Plate(colX, y, FL_ARROW_W, FL_COLUMN_H, !on ? sOffGrey : pressed ? sEmerald : sSlate, pressed ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL);
        PadArrow(colX + FL_ARROW_W / 2, oy + FL_COLUMN_H / 2, k ? 1 : 0, PackRgb(255, 255, 255));
        if (on)
            AddHit(colX, y, FL_ARROW_W, FL_COLUMN_H, k ? HIT_FL_DOWN : HIT_FL_UP);
    }
    {
        int y = AREA_Y + 2 * (FL_COLUMN_H + FM_GAP);
        bool8 pressed = s->pressed == HIT_FM_CANCEL;

        oy = Plate(colX, y, FL_ARROW_W, FL_COLUMN_H, pressed ? sEmerald : sCancelGrey, pressed ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL);
        FieldListCross(colX + FL_ARROW_W / 2, oy + FL_COLUMN_H / 2, PackRgb(255, 255, 255));
        AddHit(colX, y, FL_ARROW_W, FL_COLUMN_H, HIT_FM_CANCEL);
    }
}

static void FieldListTap(const ViewState *s, u8 id)
{
    /* A row is given as the game's window counts them, which is the first shown plus the row. */
    if (id >= HIT_FL_ROW && id < HIT_FL_ROW + CTR_FL_ROWS)
        CtrFieldList_Pick(CTR_FL_ROW, s->fl.first + (id - HIT_FL_ROW), s->fl.serial);
    else if (id == HIT_FL_UP)
        CtrFieldList_Pick(CTR_FL_SCROLL, -(s16)s->fl.count, s->fl.serial);
    else if (id == HIT_FL_DOWN)
        CtrFieldList_Pick(CTR_FL_SCROLL, s->fl.count, s->fl.serial);
    else if (id == HIT_FM_CANCEL)
        CtrFieldList_Pick(CTR_FL_CANCEL, 0, s->fl.serial);
}
