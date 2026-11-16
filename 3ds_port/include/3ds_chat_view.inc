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

static u16 sCtrChatKeys;        // keys a tap stands for, for a frame or two
static u8 sCtrChatAge;
static s8 sCtrChatItem = -1;    // a menu entry a tap chose
static u8 sCtrChatItemAge;
static int sCtrChatMsg = -1;    // the standard message on show, or -1
static u8 sCtrChatLines[CTR_CHAT_LINES][CTR_CHAT_LINE_LEN];
static u8 sCtrChatLineColor[CTR_CHAT_LINES];
static u8 sCtrChatLineCount;

// A tap's key, taken once by the code that reads it.
bool8 CtrChat_Virtual(u16 keys)
{
    if (sCtrChatKeys & keys)
    {
        sCtrChatKeys &= ~keys;
        return TRUE;
    }
    return FALSE;
}

// Every frame the chat runs: a tap nobody read does not wait for a later one to find it.
void CtrChat_Tick(void)
{
    if (sCtrChatKeys && ++sCtrChatAge > 2)
    {
        sCtrChatKeys = 0;
        sCtrChatAge = 0;
    }
    // An entry waits for its menu to open, which takes some frames; not for ever.
    if (sCtrChatItem >= 0 && ++sCtrChatItemAge > 120)
        sCtrChatItem = -1;
}

// A menu entry a tap chose, or -2 when none: the page menu's and the questions'.
s8 CtrChat_TakeMenuItem(void)
{
    s8 item = sCtrChatItem;

    sCtrChatItem = -1;
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

void CtrChat_NativeTap(u8 kind, u8 a, u8 b)
{
    if (!CtrChat_IsOpen())
        return;
    switch (kind)
    {
    case CTR_CHAT_TAP_KEY:
        if (a >= CTR_CHAT_COLS || b >= CTR_CHAT_ROWS)
            return;
        sChat->currentCol = a;
        sChat->currentRow = b;
        sCtrChatKeys |= A_BUTTON;
        break;
    case CTR_CHAT_TAP_PAGE:
        if (a >= UNION_ROOM_KB_PAGE_COUNT || a == sChat->currentPage)
            return;
        // SELECT opens the page menu; its next frame is given the page as the entry chosen.
        sCtrChatKeys |= SELECT_BUTTON;
        sCtrChatItem = a;
        sCtrChatItemAge = 0;
        break;
    case CTR_CHAT_TAP_BACK:
        sCtrChatKeys |= B_BUTTON;
        break;
    case CTR_CHAT_TAP_SEND:
        sCtrChatKeys |= START_BUTTON;
        break;
    case CTR_CHAT_TAP_CASE:
        sCtrChatKeys |= R_BUTTON;
        break;
    case CTR_CHAT_TAP_ITEM:
        sCtrChatItem = a;
        sCtrChatItemAge = 0;
        break;
    case CTR_CHAT_TAP_MESSAGE:
        sCtrChatKeys |= A_BUTTON;
        break;
    }
    sCtrChatAge = 0;
}

// What the view makes of where the chat is in its flow.
static u8 CtrChat_State(void)
{
    bool8 message = sCtrChatMsg >= 0;

    switch (sChat->funcId)
    {
    case CHAT_FUNC_HANDLE_INPUT:
        return sChat->funcState == 0 ? CTR_CHAT_VIEW_KEYS : CTR_CHAT_VIEW_BUSY;
    case CHAT_FUNC_SWITCH:
        return sChat->funcState == 2 ? CTR_CHAT_VIEW_MENU : CTR_CHAT_VIEW_BUSY;
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

bool8 CtrChat_GetView(struct CtrChatView *view)
{
    u8 row, col, i;
    const struct MessageWindowInfo *info;

    memset(view, 0, sizeof(*view));
    if (!CtrChat_IsOpen())
        return FALSE;
    view->state = CtrChat_State();
    view->page = sChat->currentPage;
    view->col = sChat->currentCol;
    view->row = sChat->currentRow;
    view->length = sChat->bufferCursorPos;
    StringCopyN(view->text, sChat->messageEntryBuffer, sizeof(view->text) - 1);
    if (sChat->currentPage == UNION_ROOM_KB_PAGE_REGISTER)
    {
        for (row = 0; row < CTR_CHAT_ROWS; row++)
            StringCopyN(view->registered[row], sChat->registeredTexts[row], sizeof(view->registered[row]) - 1);
    }
    else
    {
        for (row = 0; row < CTR_CHAT_ROWS; row++)
            for (col = 0; col < CTR_CHAT_COLS; col++)
                CtrChat_Key(sUnionRoomKeyboardText[sChat->currentPage][row], col, view->keys[row][col]);
    }
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
