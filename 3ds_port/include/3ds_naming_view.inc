/*
 * The naming screen's view and taps for the 3DS bottom screen's keyboard.
 * Included at the end of src/naming_screen.c (under PLATFORM_3DS): it reads
 * and drives that file's own statics, so it is part of its translation unit.
 * The bottom screen's drawing is 3ds_bottom_naming.inc.
 */

//--------------------------------------------------
// The 3DS bottom screen's keyboard (3ds_game_views.h)
//--------------------------------------------------
// The game's code runs as always. This tells the bottom screen where the
// naming screen is, so it draws it itself, and takes its taps as the events
// the buttons would give (CtrNaming_TakeTap, from Input_Enabled).

static void CtrNaming_CaptureTitle(const u8 *title)
{
    u8 i;

    // As printed: still with its placeholders, which the view expands.
    for (i = 0; i < CTR_VIEW_TEXT - 1 && title[i] != EOS; i++)
        sCtrTitle[i] = title[i];
    sCtrTitle[i] = EOS;
}

// Removes the character before the caret and brings the rest of the text back one.
static void CtrNaming_DeleteBeforeCaret(void)
{
    u8 i;
    u8 max = sNamingScreen->template->maxChars;
    u8 caret = sCtrCaret;

    if (caret == 0)
        return;
    for (i = caret - 1; i + 1 < max; i++)
        sNamingScreen->textBuffer[i] = sNamingScreen->textBuffer[i + 1];
    sNamingScreen->textBuffer[max - 1] = EOS;
    sCtrCaret = caret - 1;
    DrawTextEntry();
    CopyBgTilemapBufferToVram(3);
    PlaySE(SE_BALL);
}

bool8 CtrNaming_IsOpen(void)
{
    return sNamingScreen != NULL && gMain.callback2 == CB2_NamingScreen;
}

// A tap, waiting for Input_Enabled. Only one at a time: a later one replaces it,
// and one nobody took within CTR_TAP_FRAMES is gone.
#define CTR_TAP_FRAMES 30
static struct {
    bool8 pending;
    u8 kind, a, b;
    u8 page;    // the page a key was tapped on
    u32 frame;
} sCtrTap;

static void CtrNaming_Reset(void)
{
    sCtrCaret = CTR_NO_CARET;
    sCtrPageTarget = CTR_NO_PAGE;
    sCtrTitle[0] = EOS;
    sCtrTap.pending = FALSE;
}

// For a key, b is its row and, in its upper bits, the page the tap was made on.
void CtrNaming_NativeTap(u8 kind, u8 a, u8 b)
{
    if (kind == CTR_NAMING_TAP_MESSAGE)
    {
        CtrMenu_PostAck();
        return;
    }
    if (!CtrNaming_IsOpen())
        return;
    sCtrTap.pending = TRUE;
    sCtrTap.kind = kind;
    sCtrTap.a = a;
    sCtrTap.b = b & 0x0F;
    sCtrTap.page = b >> 4;
    sCtrTap.frame = gMain.vblankCounter1;
}

// The keys of a page, for any page and not only the one the game is on
// (GetCurrentPageColumnCount, GetCharAtKeyboardPos).
static u8 CtrNaming_PageColumns(u8 page)
{
#ifdef JAPANESE_KEYBOARD
    return GetJapanesePageColumnCount(sPageToKeyboardId[page]);
#else
    return sPageColumnCounts[sPageToKeyboardId[page]];
#endif
}

static u8 CtrNaming_PageChar(u8 page, s16 x, s16 y)
{
#ifdef JAPANESE_KEYBOARD
    return GetJapaneseKeyboardChar(sPageToKeyboardId[page], x, y);
#else
    return sKeyboardChars[sPageToKeyboardId[page]][y][x];
#endif
}

// A page's keys and how they are laid out in groups.
static void CtrNaming_FillPage(struct CtrNamingView *view, u8 page)
{
    u8 row, col;

    view->page = page;
    view->cols = CtrNaming_PageColumns(page);
    if (view->cols > CTR_NAMING_COLS)
        view->cols = CTR_NAMING_COLS;
    for (row = 0; row < CTR_NAMING_ROWS; row++)
        for (col = 0; col < view->cols; col++)
            view->keys[row][col] = CtrNaming_PageChar(page, col, row);

#ifdef JAPANESE_KEYBOARD
    // The kana come in groups with a space between: a column that is a space in
    // every row holds no key, and a gap lies after it.
    for (col = 0; col < view->cols; col++)
    {
        bool8 all = TRUE;

        for (row = 0; row < CTR_NAMING_ROWS; row++)
            if (view->keys[row][col] != CHAR_SPACE)
                all = FALSE;
        if (all)
            view->spacer |= 1 << col;
    }
    for (col = 0; col + 1 < view->cols; col++)
        if ((view->spacer >> col) & 1 || (view->spacer >> (col + 1)) & 1)
            view->gapAfter |= 1 << col;
#else
    // The columns are laid out at fixed x positions (sPageColumnXPos): a wide
    // distance starts a new group (3ds_naming_layout.h). A column the text
    // leaves empty in every row (the ninth of a nine-column page) has no key.
    for (col = 0; col < view->cols; col++)
    {
        bool8 all = TRUE;

        for (row = 0; row < CTR_NAMING_ROWS; row++)
            if (view->keys[row][col] != EOS)
                all = FALSE;
        if (all)
            view->spacer |= 1 << col;
    }
    view->gapAfter = CtrNaming_GapsFromPositions(sPageColumnXPos[sPageToKeyboardId[page]], view->cols);
#endif
}

// The page the keyboard turns to: the next of the game's cycle, which is one way.
static u8 CtrNaming_NextPage(u8 page)
{
    return (page + 1) % KBPAGE_COUNT;
}

// The page the view shows: the one a tap is turning to, or the one the game is
// turning to, so the keys on show are those the next input will find.
static u8 CtrNaming_ShownPage(void)
{
    u8 page = sNamingScreen->currentPage;

    if (sCtrPageTarget < KBPAGE_COUNT && sCtrPageTarget != page)
        return sCtrPageTarget;
    if (sNamingScreen->state == STATE_START_PAGE_SWAP || sNamingScreen->state == STATE_WAIT_PAGE_SWAP)
        return CtrNaming_NextPage(page);
    return page;
}

// The cursor's column after the turns from the game's page to `shown`, as
// MainState_WaitPageSwap leaves it at the end of each.
static s16 CtrNaming_CursorColumnOn(u8 shown, s16 x)
{
    u8 page = sNamingScreen->currentPage;
    u8 turns;

    for (turns = 0; page != shown && turns < KBPAGE_COUNT; turns++)
    {
        bool8 onButtons = x == CtrNaming_PageColumns(page);

        page = CtrNaming_NextPage(page);
        if (onButtons)
            x = CtrNaming_PageColumns(page);
        else if (x >= CtrNaming_PageColumns(page))
            x = CtrNaming_PageColumns(page) - 1;
    }
    return x;
}

// The pages go round one way (symbols, upper, lower). A tab two turns away
// turns there in one: the page between is put behind the one shown, as the
// end of a turn would put it (MainState_WaitPageSwap), so the next turn shows
// the page tapped and never the one between.
static void CtrNaming_SkipPage(void)
{
    s16 x, y;

    GetCursorPos(&x, &y);
    x = CtrNaming_CursorColumnOn(CtrNaming_NextPage(sNamingScreen->currentPage), x);
    sNamingScreen->currentPage = CtrNaming_NextPage(sNamingScreen->currentPage);
    SetCursorPos(x, y);
    DrawKeyboardPageOnDeck();
}

#define tKeyboardEvent data[1] // as the input handler's, undefined after it
// The event a tap gives, from Input_Enabled: the keyboard is waiting for input.
static bool8 CtrNaming_TakeTap(struct Task *task)
{
    u8 columns = GetCurrentPageColumnCount();
    u8 length = 0;

    // A page to turn to is turned to one page at a time: SELECT, as pressed.
    if (sCtrPageTarget != CTR_NO_PAGE)
    {
        if (sNamingScreen->currentPage == sCtrPageTarget)
        {
            sCtrPageTarget = CTR_NO_PAGE;
        }
        else
        {
            task->tKeyboardEvent = INPUT_SELECT;
            return TRUE;
        }
    }
    if (!sCtrTap.pending)
        return FALSE;
    sCtrTap.pending = FALSE;
    if (gMain.vblankCounter1 - sCtrTap.frame > CTR_TAP_FRAMES)
        return FALSE;

    switch (sCtrTap.kind)
    {
    case CTR_NAMING_TAP_KEY:
        // A key of the page it was tapped on, not of the one the game turned to since.
        if (sCtrTap.page != sNamingScreen->currentPage || sCtrTap.a >= columns || sCtrTap.b >= KBROW_COUNT)
            return FALSE;
        SetCursorPos(sCtrTap.a, sCtrTap.b);
        task->tKeyboardEvent = INPUT_A_BUTTON;
        return TRUE;
    case CTR_NAMING_TAP_PAGE:
        if (sCtrTap.a >= KBPAGE_COUNT || sCtrTap.a == sNamingScreen->currentPage)
            return FALSE;
        if (CtrNaming_NextPage(sNamingScreen->currentPage) != sCtrTap.a)
            CtrNaming_SkipPage();
        sCtrPageTarget = sCtrTap.a;
        task->tKeyboardEvent = INPUT_SELECT;
        return TRUE;
    case CTR_NAMING_TAP_BACK:
        task->tKeyboardEvent = INPUT_B_BUTTON;
        return TRUE;
    case CTR_NAMING_TAP_OK:
        // The OK button is the last of the buttons' column; A on it accepts.
        SetCursorPos(columns, 2);
        task->tKeyboardEvent = INPUT_A_BUTTON;
        return TRUE;
    case CTR_NAMING_TAP_CELL:
        // The text is whole from its start: a cell past its end is its end.
        while (length < sNamingScreen->template->maxChars && sNamingScreen->textBuffer[length] != EOS)
            length++;
        sCtrCaret = sCtrTap.a < length ? sCtrTap.a : CTR_NO_CARET;
        return FALSE;
    }
    return FALSE;
}

#undef tKeyboardEvent

// Fills the view from the game's own state. FALSE: no naming screen to draw.
bool8 CtrNaming_GetView(struct CtrNamingView *view)
{
    u8 i;
    u8 shown;
    s16 col, row;

    memset(view, 0, sizeof(*view));
    if (!CtrNaming_IsOpen())
        return FALSE;

    view->use = sNamingScreen->templateNum;
    view->gender = sNamingScreen->monGender;
    view->species = sNamingScreen->monSpecies;
    view->personality = sNamingScreen->monPersonality;
    switch (sNamingScreen->state)
    {
    case STATE_FADE_IN:
        // Before the screen is made: the question is not printed yet. Only who
        // is named is known, for the picture to be loaded by the time it shows.
        view->state = CTR_NAMING_VIEW_NONE;
        return TRUE;
    case STATE_HANDLE_INPUT:
        view->state = CTR_NAMING_VIEW_KEYS;
        break;
    case STATE_WAIT_SENT_TO_PC_MESSAGE:
        view->state = CTR_NAMING_VIEW_MESSAGE;
        break;
    default:
        // Coming in, a page turning, the cursor on its way to OK, going out.
        view->state = CTR_NAMING_VIEW_BUSY;
        break;
    }

    shown = CtrNaming_ShownPage();
    // The keys are live only on the page the game is on.
    if (view->state == CTR_NAMING_VIEW_KEYS && shown != sNamingScreen->currentPage)
        view->state = CTR_NAMING_VIEW_BUSY;

    view->maxChars = sNamingScreen->template->maxChars;
    for (i = 0; i < CTR_NAMING_TEXT && i < view->maxChars; i++)
        view->text[i] = sNamingScreen->textBuffer[i];
    for (; i < CTR_NAMING_TEXT; i++)
        view->text[i] = EOS;
    view->caret = GetTextEntryPosition();
    CtrNaming_FillPage(view, shown);

    GetCursorPos(&col, &row);
    view->cursorX = (s8)CtrNaming_CursorColumnOn(shown, col);
    view->cursorY = (s8)row;
    StringExpandPlaceholders(view->title, sCtrTitle);
    if (view->state == CTR_NAMING_VIEW_MESSAGE)
        StringCopyN(view->msg, gStringVar4, CTR_VIEW_TEXT - 1);
    return TRUE;
}

// A keyboard page as the bottom screen would show it with no naming screen up
// (to make its plates ahead): the player's name on that page, in this game's
// language. The cursor and the rest are the caller's.
bool8 CtrNaming_WarmView(struct CtrNamingView *view, u8 page)
{
    memset(view, 0, sizeof(*view));
    if (page >= KBPAGE_COUNT)
        return FALSE;
    view->state = CTR_NAMING_VIEW_KEYS;
    view->use = NAMING_SCREEN_PLAYER;
    view->maxChars = sNamingScreenTemplates[NAMING_SCREEN_PLAYER]->maxChars;
    CtrNaming_FillPage(view, page);
    return TRUE;
}
