/* Part of 3ds_bottom_ui.c: the naming screen's keyboard (included after 3ds_bottom_game.inc). */

/* ------------------------------------------------------------------------ */
/* The naming screen                                                        */
/* ------------------------------------------------------------------------ */

/*
 * The keyboard of the player's name, a Pokémon's nickname, a box's name and
 * Walda's phrase (naming_screen.c): over the whole screen, with the
 * player's or the Pokémon's own picture. What the keys write, the question,
 * the limits and the pages are the game's (CtrNamingView); a tap is the
 * event the key would give (CtrNaming_NativeTap).
 */
enum
{
    HIT_NAME_KEY = 0,           /* + row * NAME_KEY_COLS + column */
    HIT_NAME_TAB = HIT_ROW,     /* + 0..2: the pages in the order shown */
    HIT_NAME_BACK = HIT_ROW + 4,
    HIT_NAME_OK,
    HIT_NAME_MSG,
    HIT_NAME_CELL = HIT_ACTION, /* + the text's character cell */
};

#define NAME_KEY_COLS 12
#define NAME_PORTRAIT_X 8
#define NAME_PORTRAIT_Y 16
#define NAME_PORTRAIT_S 76
#define NAME_TITLE_X 90
#define NAME_TITLE_W 222
#define NAME_TITLE_H 26
#define NAME_ENTRY_Y 48
#define NAME_ENTRY_H 44
#define NAME_TABS_Y 98
#define NAME_TABS_H 18
#define NAME_KEYS_Y 121
#define NAME_KEY_H 23
#define NAME_KEY_GAP 3
#define NAME_KEYS_X 8
#define NAME_KEYS_W 230
#define NAME_BUTTONS_X 246
#define NAME_BUTTONS_W 66
#define NAME_DOCK_ROWS 36

/* The pages as the tabs show them: the game's page numbers (0 symbols, 1 upper, 2 lower). */
static const u8 sNameTabPages[3] = {1, 2, 0};

static const u8 *NameOne(u8 ch)
{
    static u8 text[2];

    text[0] = ch;
    text[1] = EOS;
    return text;
}

static const u8 *NameTabLabel(u8 page)
{
    switch (page)
    {
    case 1: return Ascii(CTR_TEXT("ABC", "ABC", "ABC", "ABC", "ABC", "ABC", "ア"));
    case 2: return Ascii(CTR_TEXT("abc", "abc", "abc", "abc", "abc", "abc", "あ"));
    }
    return Ascii(CTR_TEXT("123 !?", "123 !?", "123 !?", "123 !?", "123 !?", "123 !?", "1 ?"));
}

/* The player's or Pokémon's picture on its cream panel; the box and Walda's father have no picture of their own. */
static void DrawNamePortrait(const ViewState *s)
{
    const struct CtrNamingView *v = &s->nm;
    int oy = Plate(NAME_PORTRAIT_X, NAME_PORTRAIT_Y, NAME_PORTRAIT_S, NAME_PORTRAIT_S, sCreamPlate, PLATE_STATE_NORMAL);
    int px = NAME_PORTRAIT_X + (NAME_PORTRAIT_S - 64) / 2, py = oy + (NAME_PORTRAIT_S - 64) / 2 - 2;

    switch (v->use)
    {
    case NAMING_SCREEN_PLAYER:
    {
        int g = v->species == FEMALE ? 1 : 0;

        if (sRes.trainerPic[g])
            DrawSprite(sRes.trainerPic[g], 8, 8, px, py, sRes.trainerPicPal[g].c);
        break;
    }
    case NAMING_SCREEN_CAUGHT_MON:
    case NAMING_SCREEN_NICKNAME:
    {
        int slot = v->species ? FrontPic(v->species, FALSE) : -1;

        if (slot >= 0)
            DrawSprite(sFronts[slot].tiles, 8, 8, px, py, sFronts[slot].pal.c);
        break;
    }
    case NAMING_SCREEN_BOX:
        FillRect(px + 8, py + 14, 48, 36, PackRgb(22, 28, 38));
        FillRect(px + 10, py + 16, 44, 32, PackRgb(122, 176, 240));
        FillRect(px + 10, py + 16, 44, 9, PackRgb(168, 208, 255));
        FillRect(px + 24, py + 32, 16, 5, PackRgb(252, 245, 234));
        break;
    default:
        /* Walda's father, as the game shows him: a man's silhouette. */
        FillRect(px + 22, py + 10, 20, 20, PackRgb(244, 208, 168));
        FillRect(px + 14, py + 32, 36, 28, PackRgb(106, 138, 208));
        break;
    }
}

/* "3/7": the characters in the text and how many it takes. */
static const u8 *NameCount(const struct CtrNamingView *v)
{
    static u8 text[12];
    u8 n = 0, *end;

    for (u8 i = 0; i < v->maxChars && i < CTR_NAMING_TEXT; ++i)
        if (v->text[i] != EOS)
            ++n;
    end = ConvertIntToDecimalStringN(text, n, STR_CONV_MODE_LEFT_ALIGN, 2);
    *end++ = CHAR_SLASH;
    ConvertIntToDecimalStringN(end, v->maxChars, STR_CONV_MODE_LEFT_ALIGN, 2);
    return text;
}

static void DrawNameTitle(const ViewState *s)
{
    const struct CtrNamingView *v = &s->nm;
    int oy = Plate(NAME_TITLE_X, NAME_PORTRAIT_Y, NAME_TITLE_W, NAME_TITLE_H, sEmerald, PLATE_STATE_NORMAL);
    int end = TextIn(v->title, NAME_TITLE_X, oy, NAME_TITLE_W - 48, NAME_TITLE_H, &sNormal, 'l', STYLE_WHITE, 8);

    if ((v->use == NAMING_SCREEN_CAUGHT_MON || v->use == NAMING_SCREEN_NICKNAME) && end < NAME_TITLE_X + NAME_TITLE_W - 52)
        GenderSign(end + 3, TextY(oy, NAME_TITLE_H, &sSmall), v->gender);
    FillRect(NAME_TITLE_X + NAME_TITLE_W - 46, oy + 5, 38, NAME_TITLE_H - 10, PackRgb(16, 56, 44));
    TextIn(NameCount(v), NAME_TITLE_X + NAME_TITLE_W - 46, oy, 38, NAME_TITLE_H, &sSmall, 'c', STYLE_WHITE, 0);
}

/* The text: a cell for each character it can take, the one the next key writes in marked. */
static void DrawNameEntry(const ViewState *s, bool8 live)
{
    const struct CtrNamingView *v = &s->nm;
    int oy = Plate(NAME_TITLE_X, NAME_ENTRY_Y, NAME_TITLE_W, NAME_ENTRY_H, sCreamPlate, PLATE_STATE_NORMAL);
    int max = v->maxChars < CTR_NAMING_TEXT ? v->maxChars : CTR_NAMING_TEXT;
    int cw = (NAME_TITLE_W - 16) / (max ? max : 1), x0;
    u16 line = PackRgb(150, 140, 126), caret = PackRgb(255, 120, 40);

    if (cw > 26)
        cw = 26;
    x0 = NAME_TITLE_X + (NAME_TITLE_W - cw * max) / 2;
    for (int i = 0; i < max; ++i)
    {
        int cx = x0 + i * cw, cy = oy + 6;

        if (v->text[i] != EOS)
            TextIn(NameOne(v->text[i]), cx, cy, cw, 22, &sNormal, 'c', STYLE_INK, 0);
        FillRect(cx + 2, oy + NAME_ENTRY_H - 14, cw - 4, i == v->caret ? 3 : 2, i == v->caret ? caret : line);
        if (i == v->caret)
            FillRect(cx + cw / 2 - 2, oy + 4, 5, 2, caret);
        if (live)
            AddHit(cx, NAME_ENTRY_Y, cw, NAME_ENTRY_H, HIT_NAME_CELL + i);
    }
}

static void DrawNameTabs(const ViewState *s, bool8 live)
{
    const struct CtrNamingView *v = &s->nm;
    int gap = 4, w = (304 - 2 * gap) / 3, x0 = 8 + (304 - (3 * w + 2 * gap)) / 2;

    for (int i = 0; i < 3; ++i)
    {
        int x = x0 + i * (w + gap), oy;
        bool8 on = v->page == sNameTabPages[i], pressed = s->pressed == HIT_NAME_TAB + i;

        oy = Plate(x, NAME_TABS_Y, w, NAME_TABS_H, on || pressed ? sEmerald : sCreamPlate,
                   pressed ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL);
        TextIn(NameTabLabel(sNameTabPages[i]), x, oy, w, NAME_TABS_H, &sSmall, 'c', on || pressed ? STYLE_WHITE : STYLE_INK, 2);
        if (live)
            AddHit(x, NAME_TABS_Y, w, NAME_TABS_H, HIT_NAME_TAB + i);
    }
}

/* The keys of the page, in groups of columns as the game lays them out, centred in the room the buttons leave. */
static void DrawNameKeys(const ViewState *s, bool8 live)
{
    const struct CtrNamingView *v = &s->nm;
    int cols = v->cols > NAME_KEY_COLS ? NAME_KEY_COLS : v->cols;
    struct CtrNamingKeys keys;

    CtrNaming_LayoutKeys(v->spacer, v->gapAfter, cols, NAME_KEYS_X, NAME_KEYS_W, &keys);
    for (int r = 0; r < CTR_NAMING_ROWS; ++r)
    {
        int y = NAME_KEYS_Y + r * (NAME_KEY_H + NAME_KEY_GAP);

        for (int c = 0; c < cols; ++c)
        {
            u8 ch = v->keys[r][c], id = HIT_NAME_KEY + r * NAME_KEY_COLS + c;
            bool8 pressed = s->pressed == id, focus = live && v->cursorX == c && v->cursorY == r, blank = ch == CHAR_SPACE;
            int oy, x = keys.x[c];

            if (x < 0)
                continue;
            oy = Plate(x, y, keys.width, NAME_KEY_H, pressed ? sEmerald : blank ? sGroupOff : sCreamPlate,
                       pressed ? PLATE_STATE_PRESSED : focus ? PLATE_STATE_FOCUS : PLATE_STATE_NORMAL);
            if (blank)
                FillRect(x + (keys.width - 10) / 2, oy + NAME_KEY_H - 8, 10, 2, PackRgb(150, 156, 166));
            else
                TextIn(NameOne(ch), x, oy, keys.width, NAME_KEY_H, &sNormal, 'c',
                       pressed ? STYLE_WHITE : ch == CHAR_MALE ? STYLE_MALE : ch == CHAR_FEMALE ? STYLE_FEMALE : STYLE_INK, 0);
            if (live)
                AddHit(x, y, keys.width, NAME_KEY_H, id);
        }
    }
}

static void DrawNameButtons(const ViewState *s, bool8 live)
{
    const struct CtrNamingView *v = &s->nm;
    int h = 2 * NAME_KEY_H + NAME_KEY_GAP, oy;
    bool8 onButtons = live && v->cursorX == v->cols;

    oy = Plate(NAME_BUTTONS_X, NAME_KEYS_Y, NAME_BUTTONS_W, h, sRedPlate,
               s->pressed == HIT_NAME_BACK ? PLATE_STATE_PRESSED : onButtons && v->cursorY == 1 ? PLATE_STATE_FOCUS : PLATE_STATE_NORMAL);
    TextIn(Ascii(CTR_TEXT("DEL", "BORRAR", "EFFACER", "CANC.", "APAGAR", "LÖSCHEN", "けす")), NAME_BUTTONS_X, oy,
           NAME_BUTTONS_W, h, &sSmall, 'c', STYLE_WHITE, 2);
    oy = Plate(NAME_BUTTONS_X, NAME_KEYS_Y + 2 * (NAME_KEY_H + NAME_KEY_GAP), NAME_BUTTONS_W, h, sEmerald,
               s->pressed == HIT_NAME_OK ? PLATE_STATE_PRESSED : onButtons && v->cursorY == 2 ? PLATE_STATE_FOCUS : PLATE_STATE_NORMAL);
    TextIn(Ascii("OK"), NAME_BUTTONS_X, oy, NAME_BUTTONS_W, h, &sNormal, 'c', STYLE_WHITE, 2);
    if (live)
    {
        AddHit(NAME_BUTTONS_X, NAME_KEYS_Y, NAME_BUTTONS_W, h, HIT_NAME_BACK);
        AddHit(NAME_BUTTONS_X, NAME_KEYS_Y + 2 * (NAME_KEY_H + NAME_KEY_GAP), NAME_BUTTONS_W, h, HIT_NAME_OK);
    }
}

/* No dock on these screens: its band gives way to the backdrop's lattice, and the screen ends in a strip of its own, lower. */
static void NameBackdrop(void)
{
    int y0 = sClipY0 > H - NAME_DOCK_ROWS ? sClipY0 : H - NAME_DOCK_ROWS;

    /* Plates made ahead (WarmScreens) draw nothing: the canvas is the shown screen's. */
    if (sPlateWarm)
        return;
    /* The cache's dock band, in the part being drawn, from the lattice above it (the cache's own rows, so a
     * part redrawn alone does not copy what is drawn on the canvas). */
    if (sCache[CACHE_MENU] && y0 < sClipY1)
        for (int x = sClipX0; x < sClipX1; ++x)
            memcpy(sDst + x * H + (H - sClipY1), sCache[CACHE_MENU] + x * H + (H - sClipY1) + NAME_DOCK_ROWS,
                   (sClipY1 - y0) * sizeof(u16));
    FillRect(0, H - 16, W, 16, PackRgb(18, 52, 40));
    FillRect(0, H - 16, W, 1, PackRgb(160, 224, 184));
}

static void DrawGameNaming(const ViewState *s)
{
    const struct CtrNamingView *v = &s->nm;
    bool8 live = v->state == CTR_NAMING_VIEW_KEYS;

    NameBackdrop();

    if (v->state == CTR_NAMING_VIEW_NONE)
        return;
    DrawNamePortrait(s);
    DrawNameTitle(s);
    DrawNameEntry(s, live);
    DrawNameTabs(s, live);
    if (v->state == CTR_NAMING_VIEW_MESSAGE)
    {
        /* "Sent to the PC": the keyboard gives way to what the game says; a tap goes on. */
        int oy = Plate(8, NAME_KEYS_Y - 4, 304, 100, sCreamPlate, PLATE_STATE_NORMAL);

        TextLines(v->msg, 20, oy + 10, 280, 16, 4, &sNormal, STYLE_INK, TRUE);
        AddHit(8, NAME_KEYS_Y - 4, 304, 100, HIT_NAME_MSG);
        return;
    }
    DrawNameKeys(s, live);
    DrawNameButtons(s, live);
}

/* A tap on a zone, as the event it is for the game. */
static void NamingTap(u8 id)
{
    if (id < HIT_NAME_KEY + CTR_NAMING_ROWS * NAME_KEY_COLS)
        CtrNaming_NativeTap(CTR_NAMING_TAP_KEY, id % NAME_KEY_COLS, id / NAME_KEY_COLS);
    else if (id >= HIT_NAME_TAB && id < HIT_NAME_TAB + 3)
        CtrNaming_NativeTap(CTR_NAMING_TAP_PAGE, sNameTabPages[id - HIT_NAME_TAB], 0);
    else if (id == HIT_NAME_BACK)
        CtrNaming_NativeTap(CTR_NAMING_TAP_BACK, 0, 0);
    else if (id == HIT_NAME_OK)
        CtrNaming_NativeTap(CTR_NAMING_TAP_OK, 0, 0);
    else if (id == HIT_NAME_MSG)
        CtrNaming_NativeTap(CTR_NAMING_TAP_MESSAGE, 0, 0);
    else if (id >= HIT_NAME_CELL && id < HIT_NAME_CELL + CTR_NAMING_TEXT)
        CtrNaming_NativeTap(CTR_NAMING_TAP_CELL, id - HIT_NAME_CELL, 0);
}


/*
 * The keyboards as they are first seen, for making their plates in the idle
 * frames before (WarmScreens): the English layout, a key pressed, another with
 * the cursor on it, the page on its tab. Another language's keys are another
 * width and their plates are made when it is first shown.
 */
static void WarmKeyboardView(ViewState *v, bool8 chat)
{
    static const u8 gaps[2] = {2, 6};

    memset(v, 0, sizeof(*v));
    v->mode = MODE_NAMING;
    v->screen = SCR_COUNT;
    v->chat = chat;
    v->pressed = HIT_NAME_KEY;
    if (chat)
    {
        struct CtrChatView *c = &v->ch;

        c->state = CTR_CHAT_VIEW_KEYS;
        c->page = 0;
        c->row = 1;
        c->col = 1;
        for (int r = 0; r < CTR_CHAT_ROWS; ++r)
            for (int k = 0; k < CTR_CHAT_COLS; ++k)
                c->keys[r][k][0] = CHAR_A + (r * CTR_CHAT_COLS + k) % 26;
        for (int i = 0; i < 4; ++i)
            c->tabs[i] = Ascii("ABC");
        return;
    }
    v->nm.state = CTR_NAMING_VIEW_KEYS;
    v->nm.use = NAMING_SCREEN_PLAYER;
    v->nm.maxChars = PLAYER_NAME_LENGTH;
    v->nm.page = 1;
    v->nm.cols = 8;
    v->nm.gapAfter = (1u << gaps[0]) | (1u << gaps[1]);
    v->nm.cursorX = 3;
    v->nm.cursorY = 1;
    for (int r = 0; r < CTR_NAMING_ROWS; ++r)
        for (int k = 0; k < 8; ++k)
            v->nm.keys[r][k] = CHAR_A + (r * 8 + k) % 26;
    for (int i = 0; i < CTR_NAMING_TEXT; ++i)
        v->nm.text[i] = EOS;
}
