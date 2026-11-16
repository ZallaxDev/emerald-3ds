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

// A tap, waiting for Input_Enabled. Only one at a time: a later one replaces it.
static struct {
    bool8 pending;
    u8 kind, a, b;
} sCtrTap;

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
    sCtrTap.b = b;
}

// The pages go round one way (symbols, upper, lower). A tab two turns away
// turns there in one: the page between is put behind the one shown, as the
// end of a turn would put it (MainState_WaitPageSwap), so the next turn shows
// the page tapped and never the one between.
static void CtrNaming_SkipPage(void)
{
    s16 x, y;
    bool32 onLastColumn;

    GetCursorPos(&x, &y);
    onLastColumn = (x == GetCurrentPageColumnCount());
    sNamingScreen->currentPage++;
    sNamingScreen->currentPage %= KBPAGE_COUNT;
    if (onLastColumn)
        x = GetCurrentPageColumnCount();
    else if (x >= GetCurrentPageColumnCount())
        x = GetCurrentPageColumnCount() - 1;
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

    switch (sCtrTap.kind)
    {
    case CTR_NAMING_TAP_KEY:
        if (sCtrTap.a >= columns || sCtrTap.b >= KBROW_COUNT)
            return FALSE;
        SetCursorPos(sCtrTap.a, sCtrTap.b);
        task->tKeyboardEvent = INPUT_A_BUTTON;
        return TRUE;
    case CTR_NAMING_TAP_PAGE:
        if (sCtrTap.a >= KBPAGE_COUNT || sCtrTap.a == sNamingScreen->currentPage)
            return FALSE;
        if ((sNamingScreen->currentPage + 1) % KBPAGE_COUNT != sCtrTap.a)
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
    u8 i, row, col;
    u8 keyboard;

    memset(view, 0, sizeof(*view));
    if (!CtrNaming_IsOpen())
        return FALSE;

    switch (sNamingScreen->state)
    {
    case STATE_HANDLE_INPUT:
        view->state = CTR_NAMING_VIEW_KEYS;
        break;
    case STATE_WAIT_SENT_TO_PC_MESSAGE:
        view->state = CTR_NAMING_VIEW_MESSAGE;
        break;
    case STATE_FADE_OUT:
    case STATE_EXIT:
        view->state = CTR_NAMING_VIEW_NONE;
        break;
    default:
        // Coming in, or a page turning, or the cursor on its way to OK.
        view->state = CTR_NAMING_VIEW_BUSY;
        break;
    }
    if (view->state == CTR_NAMING_VIEW_NONE)
        return TRUE;

    view->use = sNamingScreen->templateNum;
    view->maxChars = sNamingScreen->template->maxChars;
    for (i = 0; i < CTR_NAMING_TEXT && i < view->maxChars; i++)
        view->text[i] = sNamingScreen->textBuffer[i];
    for (; i < CTR_NAMING_TEXT; i++)
        view->text[i] = EOS;
    view->caret = GetTextEntryPosition();
    view->page = sNamingScreen->currentPage;
    view->cols = GetCurrentPageColumnCount();
    if (view->cols > CTR_NAMING_COLS)
        view->cols = CTR_NAMING_COLS;
    keyboard = CurrentPageToKeyboardId();
    for (row = 0; row < CTR_NAMING_ROWS; row++)
        for (col = 0; col < view->cols; col++)
            view->keys[row][col] = GetCharAtKeyboardPos(col, row);

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
    // distance starts a new group (3ds_naming_layout.h).
    view->gapAfter = CtrNaming_GapsFromPositions(sPageColumnXPos[keyboard], view->cols);
#endif

    GetCursorPos(&col, &row);
    view->cursorX = (s8)col;
    view->cursorY = (s8)row;
    view->gender = sNamingScreen->monGender;
    view->species = sNamingScreen->monSpecies;
    view->personality = sNamingScreen->monPersonality;
    StringExpandPlaceholders(view->title, sCtrTitle);
    if (view->state == CTR_NAMING_VIEW_MESSAGE)
        StringCopyN(view->msg, gStringVar4, CTR_VIEW_TEXT - 1);
    return TRUE;
}
