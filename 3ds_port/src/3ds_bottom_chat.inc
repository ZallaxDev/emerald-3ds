/* Part of 3ds_bottom_ui.c: the union room chat's keyboard (included after 3ds_bottom_naming.inc). */

/*
 * The chat of the Union Room (union_room_chat.c): what was said, the message
 * being written, the four pages of keys (the game lays them out in rows of
 * five: here two rows side by side, so ten keys across) and the questions it
 * asks. What the keys write and what the game does with them are its own
 * (CtrChatView); a tap is a key or a menu entry it reads (CtrChat_NativeTap).
 */
enum
{
    HIT_CHAT_KEY = 0,           /* + game row * CTR_CHAT_COLS + column */
    HIT_CHAT_TAB = HIT_ROW,     /* + page */
    HIT_CHAT_BACK = HIT_ROW + 4,
    HIT_CHAT_CASE,
    HIT_CHAT_SEND,
    HIT_CHAT_YES,
    HIT_CHAT_NO,
    HIT_CHAT_MSG,
    HIT_CHAT_ITEM = HIT_ACTION, /* + the page menu's item */
};

#define CHAT_LOG_Y 16
#define CHAT_LOG_H 52
#define CHAT_ENTRY_Y 72
#define CHAT_ENTRY_H 22
#define CHAT_TABS_Y 98
#define CHAT_KEYS_Y 120
#define CHAT_KEY_H 18
#define CHAT_KEY_GAP 3
#define CHAT_KEY_W 28
#define CHAT_GROUP_GAP 6

static const u8 *ChatSendLabel(void)
{
    return Ascii(CTR_TEXT("SEND", "ENVIAR", "ENVOYER", "INVIA", "ENVIAR", "SENDEN", "そうしん"));
}

/* Who said it, as the game colours them: gray, red, green, blue. */
static u8 ChatLineStyle(u8 colour)
{
    static const u8 styles[4] = {STYLE_GRAY, STYLE_RED, STYLE_GREEN, STYLE_MALE};

    return styles[colour & 3];
}

static void DrawChatLog(const ViewState *s)
{
    const struct CtrChatView *v = &s->ch;
    int oy = Plate(8, CHAT_LOG_Y, 304, CHAT_LOG_H, sCreamPlate, PLATE_STATE_NORMAL);

    if (v->state == CTR_CHAT_VIEW_PICK)
    {
        /* "Register text where?" in place of what was said. */
        TextLines(v->prompt, 18, oy + 8, 284, 14, 3, &sNormal, STYLE_INK, TRUE);
        return;
    }
    for (int i = 0; i < v->numLines; ++i)
        TextIn(v->lines[i], 8, oy + 3 + i * 12, 304, 12, &sSmall, 'l', ChatLineStyle(v->lineColor[i]), 8);
}

static void DrawChatEntry(const ViewState *s, bool8 live)
{
    const struct CtrChatView *v = &s->ch;
    int oy = Plate(8, CHAT_ENTRY_Y, 162, CHAT_ENTRY_H, sCreamPlate, PLATE_STATE_NORMAL), end;
    u16 caret = PackRgb(255, 120, 40);

    end = TextIn(v->text, 8, oy, 162, CHAT_ENTRY_H, &sNormal, 'l', STYLE_INK, 6);
    if (v->length < CTR_CHAT_MAX)
        FillRect(end + 1, oy + CHAT_ENTRY_H - 7, 8, 2, caret);

    oy = Plate(174, CHAT_ENTRY_Y, 56, CHAT_ENTRY_H, sRedPlate, s->pressed == HIT_CHAT_BACK ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL);
    TextIn(Ascii(CTR_TEXT("DEL", "BORRAR", "EFFACER", "CANC.", "APAGAR", "LÖSCHEN", "けす")), 174, oy, 56, CHAT_ENTRY_H,
           &sSmall, 'c', STYLE_WHITE, 2);
    oy = Plate(234, CHAT_ENTRY_Y, 32, CHAT_ENTRY_H, sSlate, s->pressed == HIT_CHAT_CASE ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL);
    TextIn(Ascii("Aa"), 234, oy, 32, CHAT_ENTRY_H, &sSmall, 'c', STYLE_WHITE, 2);
    oy = Plate(270, CHAT_ENTRY_Y, 42, CHAT_ENTRY_H, sEmerald, s->pressed == HIT_CHAT_SEND ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL);
    TextIn(ChatSendLabel(), 270, oy, 42, CHAT_ENTRY_H, &sSmall, 'c', STYLE_WHITE, 2);
    if (live)
    {
        AddHit(174, CHAT_ENTRY_Y, 56, CHAT_ENTRY_H, HIT_CHAT_BACK);
        AddHit(234, CHAT_ENTRY_Y, 32, CHAT_ENTRY_H, HIT_CHAT_CASE);
        AddHit(270, CHAT_ENTRY_Y, 42, CHAT_ENTRY_H, HIT_CHAT_SEND);
    }
}

static void DrawChatTabs(const ViewState *s, bool8 live)
{
    const struct CtrChatView *v = &s->ch;
    int gap = 4, w = (304 - 3 * gap) / 4;

    for (int i = 0; i < 4; ++i)
    {
        int x = 8 + i * (w + gap), oy;
        bool8 on = v->page == i, pressed = s->pressed == HIT_CHAT_TAB + i;

        oy = Plate(x, CHAT_TABS_Y, w, 18, on || pressed ? sEmerald : sCreamPlate,
                   pressed ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL);
        TextIn(v->tabs[i], x, oy, w, 18, &sSmall, 'c', on || pressed ? STYLE_WHITE : STYLE_INK, 2);
        if (live)
            AddHit(x, CHAT_TABS_Y, w, 18, HIT_CHAT_TAB + i);
    }
}

/* Two game rows side by side make a row of ten keys; the registered page is ten texts, two by five. */
static void DrawChatKeys(const ViewState *s, bool8 live)
{
    const struct CtrChatView *v = &s->ch;
    bool8 registered = v->page == CTR_CHAT_PAGE_REGISTER;
    int x0 = 8 + (304 - (10 * CHAT_KEY_W + 8 * 2 + CHAT_GROUP_GAP)) / 2;

    for (int sr = 0; sr < CTR_CHAT_ROWS / 2; ++sr)
    {
        int y = CHAT_KEYS_Y + sr * (CHAT_KEY_H + CHAT_KEY_GAP);

        for (int half = 0; half < 2; ++half)
        {
            int row = 2 * sr + half;

            if (registered)
            {
                int x = 8 + half * 154, oy;
                u8 id = HIT_CHAT_KEY + row * CTR_CHAT_COLS;
                bool8 pressed = s->pressed == id, focus = live && v->row == row;

                oy = Plate(x, y, 150, CHAT_KEY_H, pressed ? sEmerald : sCreamPlate,
                           pressed ? PLATE_STATE_PRESSED : focus ? PLATE_STATE_FOCUS : PLATE_STATE_NORMAL);
                TextIn(v->registered[row], x, oy, 150, CHAT_KEY_H, &sSmall, 'l', pressed ? STYLE_WHITE : STYLE_INK, 6);
                if (live)
                    AddHit(x, y, 150, CHAT_KEY_H, id);
                continue;
            }
            for (int col = 0; col < CTR_CHAT_COLS; ++col)
            {
                int x = x0 + (half * CTR_CHAT_COLS + col) * (CHAT_KEY_W + 2) + (half ? CHAT_GROUP_GAP - 2 : 0), oy;
                u8 id = HIT_CHAT_KEY + row * CTR_CHAT_COLS + col;
                bool8 pressed = s->pressed == id, focus = live && v->row == row && v->col == col;
                const u8 *key = v->keys[row][col];

                if (key[0] == EOS)
                    continue;
                oy = Plate(x, y, CHAT_KEY_W, CHAT_KEY_H, pressed ? sEmerald : sCreamPlate,
                           pressed ? PLATE_STATE_PRESSED : focus ? PLATE_STATE_FOCUS : PLATE_STATE_NORMAL);
                TextIn(key, x, oy, CHAT_KEY_W, CHAT_KEY_H, &sNormal, 'c', pressed ? STYLE_WHITE : STYLE_INK, 0);
                if (live)
                    AddHit(x, y, CHAT_KEY_W, CHAT_KEY_H, id);
            }
        }
    }
}

/* A question, the page menu or a message over the keys. */
static void DrawChatDialog(const ViewState *s)
{
    const struct CtrChatView *v = &s->ch;
    int oy = Plate(8, CHAT_KEYS_Y - 2, 304, 102, sCreamPlate, PLATE_STATE_NORMAL);

    if (v->state == CTR_CHAT_VIEW_MENU)
    {
        for (int i = 0; i < v->numItems; ++i)
        {
            int y = oy + 4 + i * 19;
            bool8 pressed = s->pressed == HIT_CHAT_ITEM + i;
            int by = Plate(60, y, 200, 16, pressed ? sEmerald : sLight,
                           pressed ? PLATE_STATE_PRESSED : v->itemCursor == i ? PLATE_STATE_FOCUS : PLATE_STATE_NORMAL);

            TextIn(v->items[i], 60, by, 200, 16, &sSmall, 'c', pressed ? STYLE_WHITE : STYLE_INK, 4);
            AddHit(60, y, 200, 16, HIT_CHAT_ITEM + i);
        }
        return;
    }
    TextLines(v->prompt, 20, oy + 10, 280, 16, 3, &sNormal, STYLE_INK, TRUE);
    if (v->state == CTR_CHAT_VIEW_YESNO)
    {
        for (int k = 0; k < 2; ++k)
        {
            int x = 70 + k * 100, by;
            u8 id = k ? HIT_CHAT_NO : HIT_CHAT_YES;
            bool8 pressed = s->pressed == id;

            by = Plate(x, oy + 66, 80, 26, pressed ? sEmerald : k ? sCancelGrey : sSlate,
                       pressed ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL);
            TextIn(k ? gText_No : gText_Yes, x, by, 80, 26, &sNormal, 'c', STYLE_WHITE, 3);
            AddHit(x, oy + 66, 80, 26, id);
        }
    }
    else
        AddHit(8, CHAT_KEYS_Y - 2, 304, 102, HIT_CHAT_MSG);
}

static void DrawGameChat(const ViewState *s)
{
    const struct CtrChatView *v = &s->ch;
    bool8 live = v->state == CTR_CHAT_VIEW_KEYS || v->state == CTR_CHAT_VIEW_PICK;

    NameBackdrop();
    if (v->state == CTR_CHAT_VIEW_NONE)
        return;
    DrawChatLog(s);
    DrawChatEntry(s, v->state == CTR_CHAT_VIEW_KEYS);
    DrawChatTabs(s, v->state == CTR_CHAT_VIEW_KEYS);
    if (v->state == CTR_CHAT_VIEW_MENU || v->state == CTR_CHAT_VIEW_YESNO || v->state == CTR_CHAT_VIEW_MESSAGE)
        DrawChatDialog(s);
    else
        DrawChatKeys(s, live);
}

static void ChatTap(u8 id)
{
    if (id < HIT_CHAT_KEY + CTR_CHAT_ROWS * CTR_CHAT_COLS)
        CtrChat_NativeTap(CTR_CHAT_TAP_KEY, id % CTR_CHAT_COLS, id / CTR_CHAT_COLS);
    else if (id >= HIT_CHAT_TAB && id < HIT_CHAT_TAB + 4)
        CtrChat_NativeTap(CTR_CHAT_TAP_PAGE, id - HIT_CHAT_TAB, 0);
    else if (id == HIT_CHAT_BACK)
        CtrChat_NativeTap(CTR_CHAT_TAP_BACK, 0, 0);
    else if (id == HIT_CHAT_CASE)
        CtrChat_NativeTap(CTR_CHAT_TAP_CASE, 0, 0);
    else if (id == HIT_CHAT_SEND)
        CtrChat_NativeTap(CTR_CHAT_TAP_SEND, 0, 0);
    else if (id == HIT_CHAT_YES)
        CtrChat_NativeTap(CTR_CHAT_TAP_ITEM, 0, 0);
    else if (id == HIT_CHAT_NO)
        CtrChat_NativeTap(CTR_CHAT_TAP_ITEM, 1, 0);
    else if (id == HIT_CHAT_MSG)
        CtrChat_NativeTap(CTR_CHAT_TAP_MESSAGE, 0, 0);
    else if (id >= HIT_CHAT_ITEM && id < HIT_CHAT_ITEM + CTR_CHAT_ITEMS)
        CtrChat_NativeTap(CTR_CHAT_TAP_ITEM, id - HIT_CHAT_ITEM, 0);
}
