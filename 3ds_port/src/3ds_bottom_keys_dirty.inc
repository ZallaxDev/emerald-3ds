/* Part of 3ds_bottom_ui.c: the parts of the keyboards a redraw can be limited to (included before DirtyRect, which uses Rect). */

/*
 * What a part redraw can be limited to (DirtyRect): a key pressed or let go, the
 * game's cursor on another key, the text written to - the plates of those, or
 * the entry and its counter. The rest is the whole screen.
 */
static void NameAddPlate(Rect *r, int x, int y, int w, int h)
{
    int x0, y0, x1, y1;

    PlateBounds(x, y, w, h, &x0, &y0, &x1, &y1);
    RectAdd(r, x0 - 4, y0 - 4, x1 + 4, y1 + 6);
}

/* The zone the game's cursor is on: a key, or one of the buttons' column (the page button, first, is on the tab it turns to). */
static u8 NameCursorHit(const struct CtrNamingView *v)
{
    int c = v->cursorX, rw = v->cursorY;

    if (c < 0 || rw < 0)
        return HIT_NONE;
    if (c < v->cols)
        return rw < CTR_NAMING_ROWS ? HIT_NAME_KEY + rw * NAME_KEY_COLS + c : HIT_NONE;
    return rw == 0 ? HIT_NAME_TAB + NameTabOfPage(NameNextPage(v->page))
         : rw == 1 ? HIT_NAME_BACK : rw == 2 ? HIT_NAME_OK : HIT_NONE;
}

/* Keys taking taps, or busy but drawn the same (DrawGameNaming): one turns into the other with the cursor's plate. */
static bool8 NamingKeysShown(const struct CtrNamingView *v)
{
    return v->state == CTR_NAMING_VIEW_KEYS || v->state == CTR_NAMING_VIEW_BUSY;
}

static bool8 NamingDirty(const ViewState *now, const ViewState *shown, Rect *r)
{
    static ViewState probe;

    if (!NamingKeysShown(&now->nm) || !NamingKeysShown(&shown->nm))
        return FALSE;
    probe = *now;
    probe.pressed = shown->pressed;
    probe.nm.state = shown->nm.state;
    memcpy(probe.nm.text, shown->nm.text, sizeof(probe.nm.text));
    probe.nm.caret = shown->nm.caret;
    probe.nm.cursorX = shown->nm.cursorX;
    probe.nm.cursorY = shown->nm.cursorY;
    if (memcmp(&probe, shown, sizeof(probe)) != 0)
        return FALSE;
    RectInit(r);
    if (now->pressed != shown->pressed
     && !((now->pressed == HIT_NONE || RectAddPlate(r, now->pressed))
          && (shown->pressed == HIT_NONE || RectAddPlate(r, shown->pressed))))
        return FALSE;
    /* The ring is drawn in KEYS only: busy to keys or back is the cursor's plate, where it was and where it is. */
    if ((now->nm.cursorX != shown->nm.cursorX || now->nm.cursorY != shown->nm.cursorY
         || now->nm.state != shown->nm.state)
     && !(RectAddPlate(r, NameCursorHit(&now->nm)) && RectAddPlate(r, NameCursorHit(&shown->nm))))
        return FALSE;
    if (memcmp(now->nm.text, shown->nm.text, sizeof(now->nm.text)) != 0 || now->nm.caret != shown->nm.caret)
    {
        NameAddPlate(r, NAME_TITLE_X, NAME_ENTRY_Y, NAME_TITLE_W, NAME_ENTRY_H);
        NameAddPlate(r, NAME_TITLE_X, NAME_PORTRAIT_Y, NAME_TITLE_W, NAME_TITLE_H);   /* its counter */
    }
    return r->x0 < r->x1 && r->y0 < r->y1;
}

/* As NamingDirty: a key pressed, the cursor moved, the message written to, a line said. */
static u8 ChatCursorHit(const struct CtrChatView *v)
{
    if (v->row < 0 || v->row >= CTR_CHAT_ROWS || v->col < 0 || v->col >= CTR_CHAT_COLS)
        return HIT_NONE;
    return HIT_CHAT_KEY + v->row * CTR_CHAT_COLS + (v->page == CTR_CHAT_PAGE_REGISTER ? 0 : v->col);
}

/* As NamingKeysShown. */
static bool8 ChatKeysShown(const struct CtrChatView *v)
{
    return v->state == CTR_CHAT_VIEW_KEYS || v->state == CTR_CHAT_VIEW_BUSY;
}

static bool8 ChatDirty(const ViewState *now, const ViewState *shown, Rect *r)
{
    static ViewState probe;

    if (!ChatKeysShown(&now->ch) || !ChatKeysShown(&shown->ch))
        return FALSE;
    probe = *now;
    probe.pressed = shown->pressed;
    probe.ch.state = shown->ch.state;
    memcpy(probe.ch.text, shown->ch.text, sizeof(probe.ch.text));
    probe.ch.length = shown->ch.length;
    probe.ch.row = shown->ch.row;
    probe.ch.col = shown->ch.col;
    probe.ch.numLines = shown->ch.numLines;
    memcpy(probe.ch.lines, shown->ch.lines, sizeof(probe.ch.lines));
    memcpy(probe.ch.lineColor, shown->ch.lineColor, sizeof(probe.ch.lineColor));
    if (memcmp(&probe, shown, sizeof(probe)) != 0)
        return FALSE;
    RectInit(r);
    if (now->pressed != shown->pressed
     && !((now->pressed == HIT_NONE || RectAddPlate(r, now->pressed))
          && (shown->pressed == HIT_NONE || RectAddPlate(r, shown->pressed))))
        return FALSE;
    if ((now->ch.row != shown->ch.row || now->ch.col != shown->ch.col || now->ch.state != shown->ch.state)
     && !(RectAddPlate(r, ChatCursorHit(&now->ch)) && RectAddPlate(r, ChatCursorHit(&shown->ch))))
        return FALSE;
    if (memcmp(now->ch.text, shown->ch.text, sizeof(now->ch.text)) != 0 || now->ch.length != shown->ch.length)
        NameAddPlate(r, 8, CHAT_ENTRY_Y, 162, CHAT_ENTRY_H);
    if (now->ch.numLines != shown->ch.numLines || memcmp(now->ch.lines, shown->ch.lines, sizeof(now->ch.lines)) != 0)
        NameAddPlate(r, 8, CHAT_LOG_Y, 304, CHAT_LOG_H);
    return r->x0 < r->x1 && r->y0 < r->y1;
}
