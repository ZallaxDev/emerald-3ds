/*
 * The union room chat's view and taps for the 3DS bottom screen's keyboard.
 * Included at the end of src/union_room_chat.c (under PLATFORM_3DS): it reads
 * that file's own statics. A tap is a key the game's own code reads
 * (CHAT_NEW, CHAT_REPEAT) or the menu entry its menu code asks for
 * (ProcessMenuInput); nothing of what the keys do is redone here.
 * The drawing is 3ds_bottom_chat.inc.
 */

#include "3ds_game_views.h"

_Static_assert(UNION_ROOM_KB_PAGE_REGISTER == CTR_CHAT_PAGE_REGISTER, "the view's register page");
_Static_assert(UNION_ROOM_KB_PAGE_COUNT == 4, "the view's four pages");

// A tap waits for the game's own code to read it: one at a time, a later one
// replaces it, and one nobody read within CTR_CHAT_TAP_FRAMES is gone.
#define CTR_CHAT_TAP_FRAMES 30
#define CTR_CHAT_ITEM_FRAMES 8      // a menu entry waits for the menu that is open to ask for it

static u16 sCtrChatKeys;        // the key a tap stands for
static u8 sCtrChatAge;
static s8 sCtrChatTapRow = -1;  // a key's tap: where the cursor goes as the tap is read
static s8 sCtrChatTapCol;
static u8 sCtrChatTapPage;      // and the page it was made on
static s8 sCtrChatItem = -1;    // a menu entry a tap chose
static u8 sCtrChatItemAge;
static bool8 sCtrChatPageTap;   // the entry is the page a tab asked for: SELECT opens the menu it is chosen in
static int sCtrChatMsg = -1;    // the standard message on show, or -1
static u8 sCtrChatLines[CTR_CHAT_LINES][CTR_CHAT_LINE_LEN];
static u8 sCtrChatLineColor[CTR_CHAT_LINES];
static u8 sCtrChatLineCount;

// A new chat starts with nothing of the last one.
void CtrChat_Reset(void)
{
    sCtrChatKeys = 0;
    sCtrChatAge = 0;
    sCtrChatTapRow = -1;
    sCtrChatTapCol = 0;
    sCtrChatTapPage = 0;
    sCtrChatItem = -1;
    sCtrChatItemAge = 0;
    sCtrChatPageTap = FALSE;
    sCtrChatMsg = -1;
    memset(sCtrChatLines, 0, sizeof(sCtrChatLines));
    memset(sCtrChatLineColor, 0, sizeof(sCtrChatLineColor));
    sCtrChatLineCount = 0;
}

static void CtrChat_DropItem(void)
{
    sCtrChatItem = -1;
    sCtrChatPageTap = FALSE;
}

// Where a key's tap is read: typing in the keys, or the row to register in.
static bool8 CtrChat_KeyTapIsRead(void)
{
    return (sChat->funcId == CHAT_FUNC_HANDLE_INPUT && sChat->funcState == 0)
        || (sChat->funcId == CHAT_FUNC_REGISTER && sChat->funcState == 1);
}

// A tap's key, taken once by the code that reads it.
bool8 CtrChat_Virtual(u16 keys)
{
    if (!(sCtrChatKeys & keys))
        return FALSE;
    if (sCtrChatTapRow >= 0)
    {
        // The cursor goes to the key as its tap is read, not before: a tap read
        // late, or by nobody, leaves it where the game has it. Not read at all
        // on a page other than the one tapped.
        if (!CtrChat_KeyTapIsRead())
            return FALSE;
        if (sChat->currentPage != sCtrChatTapPage)
        {
            sCtrChatKeys = 0;
            sCtrChatTapRow = -1;
            return FALSE;
        }
        sChat->currentCol = sCtrChatTapCol;
        sChat->currentRow = sCtrChatTapRow;
    }
    sCtrChatKeys &= ~keys;
    sCtrChatTapRow = -1;
    return TRUE;
}

// Every frame the chat runs: a tap nobody read does not wait for a later one to find it.
void CtrChat_Tick(void)
{
    if (sCtrChatKeys && ++sCtrChatAge > CTR_CHAT_TAP_FRAMES)
    {
        sCtrChatKeys = 0;
        sCtrChatTapRow = -1;
        if (sCtrChatPageTap)
            CtrChat_DropItem();
    }
    // A key's tap, or a page asked for, is not kept for a chat that has gone on to something else.
    if (sCtrChatTapRow >= 0 && sChat->funcId != CHAT_FUNC_HANDLE_INPUT && sChat->funcId != CHAT_FUNC_REGISTER)
    {
        sCtrChatKeys = 0;
        sCtrChatTapRow = -1;
    }
    if (sCtrChatPageTap && sChat->funcId != CHAT_FUNC_HANDLE_INPUT && sChat->funcId != CHAT_FUNC_SWITCH)
    {
        sCtrChatKeys = 0;
        CtrChat_DropItem();
    }
    // An entry waits for its menu to open: the page menu a tab asks for takes
    // some frames after its SELECT is read. It does not wait for ever.
    if (sCtrChatItem >= 0)
    {
        if (sCtrChatPageTap && (sCtrChatKeys || sChat->funcId == CHAT_FUNC_SWITCH))
            sCtrChatItemAge = 0;
        else if (++sCtrChatItemAge > CTR_CHAT_ITEM_FRAMES)
            CtrChat_DropItem();
    }
}

// A menu entry a tap chose, or -2 when none: the page menu's and the questions'.
s8 CtrChat_TakeMenuItem(void)
{
    s8 item = sCtrChatItem;

    CtrChat_DropItem();
    return item >= 0 ? item : -2;
}

void CtrChat_NoteMessage(int msgId)
{
    sCtrChatMsg = msgId;
}

void CtrChat_NoteLine(const u8 *str, u8 color)
{
    u8 i;

    if (sCtrChatLineCount == CTR_CHAT_LINES)
    {
        for (i = 1; i < CTR_CHAT_LINES; i++)
        {
            memcpy(sCtrChatLines[i - 1], sCtrChatLines[i], CTR_CHAT_LINE_LEN);
            sCtrChatLineColor[i - 1] = sCtrChatLineColor[i];
        }
        sCtrChatLineCount--;
    }
    StringCopyN(sCtrChatLines[sCtrChatLineCount], str, CTR_CHAT_LINE_LEN - 1);
    sCtrChatLines[sCtrChatLineCount][CTR_CHAT_LINE_LEN - 1] = EOS;
    sCtrChatLineColor[sCtrChatLineCount] = color;
    sCtrChatLineCount++;
}

bool8 CtrChat_IsOpen(void)
{
    return sChat != NULL && gMain.callback2 == CB2_UnionRoomChatMain;
}

// A key's tap or a button's: the key the game reads, replacing a tap not read yet.
static void CtrChat_PostKey(u16 keys)
{
    sCtrChatKeys = keys;
    sCtrChatTapRow = -1;
    sCtrChatAge = 0;
    CtrChat_DropItem();
}

void CtrChat_NativeTap(u8 kind, u8 a, u8 b)
{
    if (!CtrChat_IsOpen())
        return;
    switch (kind)
    {
    case CTR_CHAT_TAP_KEY:
        if (a >= CTR_CHAT_COLS || b >= CTR_CHAT_ROWS)
            return;
        CtrChat_PostKey(A_BUTTON);
        sCtrChatTapCol = a;
        sCtrChatTapRow = b;
        sCtrChatTapPage = sChat->currentPage;
        break;
    case CTR_CHAT_TAP_PAGE:
        if (a >= UNION_ROOM_KB_PAGE_COUNT || a == sChat->currentPage)
            return;
        // SELECT opens the page menu; its next frame is given the page as the entry chosen.
        CtrChat_PostKey(SELECT_BUTTON);
        sCtrChatItem = a;
        sCtrChatItemAge = 0;
        sCtrChatPageTap = TRUE;
        break;
    case CTR_CHAT_TAP_BACK:
        CtrChat_PostKey(B_BUTTON);
        break;
    case CTR_CHAT_TAP_SEND:
        CtrChat_PostKey(START_BUTTON);
        break;
    case CTR_CHAT_TAP_CASE:
        CtrChat_PostKey(R_BUTTON);
        break;
    case CTR_CHAT_TAP_ITEM:
        CtrChat_DropItem();
        sCtrChatItem = a;
        sCtrChatItemAge = 0;
        break;
    case CTR_CHAT_TAP_MESSAGE:
        CtrChat_PostKey(A_BUTTON);
        break;
    }
}

// The page a tab asked for, while the game has not got there: -1 for none. The
// view shows it at once, as the game's own page menu would not.
static s8 CtrChat_TargetPage(void)
{
    if (!sCtrChatPageTap || sCtrChatItem < 0 || sCtrChatItem >= UNION_ROOM_KB_PAGE_COUNT)
        return -1;
    if (sChat->funcId != CHAT_FUNC_HANDLE_INPUT && sChat->funcId != CHAT_FUNC_SWITCH)
        return -1;
    return sCtrChatItem != sChat->currentPage ? sCtrChatItem : -1;
}

// What the view makes of where the chat is in its flow.
static u8 CtrChat_State(void)
{
    bool8 message = sCtrChatMsg >= 0;

    switch (sChat->funcId)
    {
    case CHAT_FUNC_HANDLE_INPUT:
        // Also while it waits for its display (funcState 1): a tap is read when it is done. Not
        // while a tab's page is on its way: that is shown, busy.
        return CtrChat_TargetPage() < 0 ? CTR_CHAT_VIEW_KEYS : CTR_CHAT_VIEW_BUSY;
    case CHAT_FUNC_SWITCH:
        // The page menu, unless a tab chose its entry already: it would be one frame of the menu.
        return sChat->funcState == 2 && CtrChat_TargetPage() < 0 ? CTR_CHAT_VIEW_MENU : CTR_CHAT_VIEW_BUSY;
    case CHAT_FUNC_ASK_QUIT:
        if (sChat->funcState == 2 || sChat->funcState == 8)
            return CTR_CHAT_VIEW_YESNO;
        break;
    case CHAT_FUNC_SAVE_AND_EXIT:
        if (sChat->funcState == 2 || sChat->funcState == 5)
            return CTR_CHAT_VIEW_YESNO;
        break;
    case CHAT_FUNC_REGISTER:
        if (sChat->funcState == 1)
            return CTR_CHAT_VIEW_PICK;
        if (sChat->funcState == 6)
            return CTR_CHAT_VIEW_MESSAGE;
        break;
    }
    return message ? CTR_CHAT_VIEW_MESSAGE : CTR_CHAT_VIEW_BUSY;
}

// One key's character out of a keyboard row: a symbol may take two bytes.
static const u8 *CtrChat_Key(const u8 *row, u8 col, u8 *out)
{
    u8 i;

    for (i = 0; i < col && *row != EOS; i++)
        row += (*row == CHAR_EXTRA_SYMBOL) ? 2 : 1;
    out[0] = out[1] = out[2] = EOS;
    if (*row == EOS)
        return row;
    out[0] = row[0];
    if (*row == CHAR_EXTRA_SYMBOL)
        out[1] = row[1];
    return row;
}

// A page's keys, or its registered texts.
static void CtrChat_FillPage(struct CtrChatView *view, u8 page)
{
    u8 row, col;

    view->page = page;
    if (page == UNION_ROOM_KB_PAGE_REGISTER)
    {
        for (row = 0; row < CTR_CHAT_ROWS; row++)
            StringCopyN(view->registered[row], sChat->registeredTexts[row], sizeof(view->registered[row]) - 1);
    }
    else
    {
        for (row = 0; row < CTR_CHAT_ROWS; row++)
            for (col = 0; col < CTR_CHAT_COLS; col++)
                CtrChat_Key(sUnionRoomKeyboardText[page][row], col, view->keys[row][col]);
    }
}

bool8 CtrChat_GetView(struct CtrChatView *view)
{
    u8 i;
    s8 target;
    const struct MessageWindowInfo *info;

    memset(view, 0, sizeof(*view));
    if (!CtrChat_IsOpen())
        return FALSE;
    view->state = CtrChat_State();
    target = CtrChat_TargetPage();
    if (target >= 0)
    {
        // The page a tab asked for, as the game will have it: the cursor at its first key.
        CtrChat_FillPage(view, target);
        view->col = 0;
        view->row = 0;
    }
    else
    {
        CtrChat_FillPage(view, sChat->currentPage);
        view->col = sChat->currentCol;
        view->row = sChat->currentRow;
    }
    view->length = sChat->bufferCursorPos;
    StringCopyN(view->text, sChat->messageEntryBuffer, sizeof(view->text) - 1);
    for (i = 0; i < 4; i++)
        view->tabs[i] = sKeyboardPageTitleTexts[i].text;
    view->numLines = sCtrChatLineCount;
    for (i = 0; i < sCtrChatLineCount; i++)
    {
        memcpy(view->lines[i], sCtrChatLines[i], CTR_CHAT_LINE_LEN);
        view->lineColor[i] = sCtrChatLineColor[i];
    }
    if (view->state == CTR_CHAT_VIEW_MENU)
    {
        // The page menu has an entry for each page, and a last one that leaves it.
        view->numItems = ARRAY_COUNT(sKeyboardPageTitleTexts) < CTR_CHAT_ITEMS ? ARRAY_COUNT(sKeyboardPageTitleTexts) : CTR_CHAT_ITEMS;
        for (i = 0; i < view->numItems; i++)
            view->items[i] = sKeyboardPageTitleTexts[i].text;
        view->itemCursor = sChat->currentPage;
    }
    if (sCtrChatMsg >= 0 && sCtrChatMsg < (int)ARRAY_COUNT(sDisplayStdMessages))
    {
        info = &sDisplayStdMessages[sCtrChatMsg];
        if (info->hasPlaceholders)
            StringExpandPlaceholders(view->prompt, info->text);
        else
            StringCopyN(view->prompt, info->text, sizeof(view->prompt) - 1);
    }
    return TRUE;
}
