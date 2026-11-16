/*
 * The 3DS bottom screen's view of the game's menus (3ds_game_views.h,
 * CtrFieldMenu_*). Included at the end of src/menu.c (under PLATFORM_3DS):
 * it reads that file's own sMenu. What the entries say is taken from what the
 * game printed into the menu's window (text.c, CtrMenu_NotePrint), so every
 * menu - a script's multichoice, the PC's, a yes/no box - needs nothing of
 * its own; an entry is chosen the way a tap on a list is (CtrMenu_Choose).
 */

#include "3ds_game_views.h"

s8 CtrMenu_Choose(s8 entry, bool8 sound);
bool8 CtrMenu_YesNoOpen(void);

#define CTR_PRINT_RING 48

static struct {
    bool8 used;
    u8 window, x, y;
    u8 text[CTR_FM_TEXT];
} sCtrPrints[CTR_PRINT_RING];
static u8 sCtrPrintNext;
static u32 sCtrMenuPolled;
static struct {
    bool8 pending;
    s8 entry;
    u32 frame;
} sCtrPick;

// text.c: a string printed into a window, at (x, y) in it.
void CtrMenu_NotePrint(u8 windowId, u8 x, u8 y, const u8 *str)
{
    u8 i, slot = CTR_PRINT_RING;

    if (str == NULL)
        return;
    for (i = 0; i < CTR_PRINT_RING; i++)
        if (sCtrPrints[i].used && sCtrPrints[i].window == windowId && sCtrPrints[i].x == x && sCtrPrints[i].y == y)
            slot = i;
    if (slot == CTR_PRINT_RING)
    {
        slot = sCtrPrintNext;
        sCtrPrintNext = (sCtrPrintNext + 1) % CTR_PRINT_RING;
    }
    sCtrPrints[slot].used = TRUE;
    sCtrPrints[slot].window = windowId;
    sCtrPrints[slot].x = x;
    sCtrPrints[slot].y = y;
    for (i = 0; i < CTR_FM_TEXT - 1 && str[i] != EOS; i++)
        sCtrPrints[slot].text[i] = str[i];
    sCtrPrints[slot].text[i] = EOS;
}

// window.c: a window is gone, and its number will serve another one. What was
// printed in it must not be read as the new window's entries.
void CtrMenu_ForgetWindow(u8 windowId)
{
    u8 i;

    for (i = 0; i < CTR_PRINT_RING; i++)
        if (sCtrPrints[i].window == windowId)
            sCtrPrints[i].used = FALSE;
}

// Called by every one of menu.c's input functions as it is asked for an answer:
// the menu is waiting for one; and an entry tapped on the bottom screen is
// chosen as A would. Returns TRUE with the selection to give.
bool8 CtrMenuField_Poll(s8 *selection)
{
    sCtrMenuPolled = gMain.vblankCounter1;
    if (!sCtrPick.pending)
        return FALSE;
    sCtrPick.pending = FALSE;
    if (gMain.vblankCounter1 - sCtrPick.frame > 30)
        return FALSE;
    if (sCtrPick.entry < 0)
    {
        *selection = MENU_B_PRESSED;
        return TRUE;
    }
    if (sCtrPick.entry < sMenu.minCursorPos || sCtrPick.entry > sMenu.maxCursorPos)
        return FALSE;
    CtrMenu_Choose(sCtrPick.entry, TRUE);
    *selection = sCtrPick.entry;
    return TRUE;
}

void CtrFieldMenu_Pick(s8 entry)
{
    sCtrPick.pending = TRUE;
    sCtrPick.entry = entry;
    sCtrPick.frame = gMain.vblankCounter1;
}

// A menu the game keeps itself, not through menu.c's cursor (the title menu):
// its code says each frame what the entries are and where its cursor is, and
// takes the entry a tap chose. The bottom screen sees it as any other menu.
static struct {
    u32 polled;
    u8 count, cursor;
    const u8 *text[CTR_FM_MAX];
} sCtrCustom;

void CtrMenu_Custom(u8 count, u8 cursor, const u8 *const *texts)
{
    u8 i;

    sCtrCustom.polled = gMain.vblankCounter1;
    sCtrCustom.count = count > CTR_FM_MAX ? CTR_FM_MAX : count;
    sCtrCustom.cursor = cursor;
    for (i = 0; i < sCtrCustom.count; i++)
        sCtrCustom.text[i] = texts[i];
}

bool8 CtrMenu_CustomTake(s8 *entry)
{
    if (!sCtrPick.pending)
        return FALSE;
    sCtrPick.pending = FALSE;
    if (gMain.vblankCounter1 - sCtrPick.frame > 30 || sCtrPick.entry < 0 || sCtrPick.entry >= sCtrCustom.count)
        return FALSE;
    *entry = sCtrPick.entry;
    return TRUE;
}

// The text printed at an entry's place in the menu's window, expanded. A
// string of several lines (the yes/no box's "YES\nNO") gives one entry a line.
static void CtrFieldMenu_EntryText(u8 windowId, s16 x0, s16 x1, s16 y0, s16 y1, s16 height, u8 *out)
{
    u8 i, k, buffer[2 * CTR_FM_TEXT + 16];

    out[0] = EOS;
    for (i = 0; i < CTR_PRINT_RING; i++)
    {
        const u8 *line;

        if (!sCtrPrints[i].used || sCtrPrints[i].window != windowId)
            continue;
        if (sCtrPrints[i].x < x0 || sCtrPrints[i].x >= x1)
            continue;
        StringExpandPlaceholders(buffer, sCtrPrints[i].text);
        line = buffer;
        for (k = 0; k < 8; k++)
        {
            s16 lineY = sCtrPrints[i].y + k * height;
            u8 n = 0;

            if (lineY >= y0 && lineY < y1)
            {
                while (n < CTR_FM_TEXT - 1 && line[n] != EOS && line[n] != CHAR_NEWLINE)
                {
                    out[n] = line[n];
                    n++;
                }
                out[n] = EOS;
                return;
            }
            while (*line != EOS && *line != CHAR_NEWLINE)
                line++;
            if (*line == EOS)
                break;
            line++;
        }
    }
}

bool8 CtrFieldMenu_GetView(struct CtrFieldMenuView *view)
{
    u8 i, count;
    s16 height = sMenu.optionHeight, width = sMenu.optionWidth;
    u8 columns = sMenu.columns > 1 ? sMenu.columns : 1;

    memset(view, 0, sizeof(*view));
    if (gMain.vblankCounter1 - sCtrCustom.polled <= 2 && sCtrCustom.count != 0)
    {
        u8 buffer[2 * CTR_FM_TEXT + 16];

        view->count = sCtrCustom.count;
        view->columns = 1;
        view->cursor = sCtrCustom.cursor;
        for (i = 0; i < sCtrCustom.count; i++)
        {
            StringExpandPlaceholders(buffer, sCtrCustom.text[i]);
            StringCopyN(view->text[i], buffer, CTR_FM_TEXT - 1);
        }
        return TRUE;
    }
    // Waiting for an answer: asked for one in this frame or the last.
    if (gMain.vblankCounter1 - sCtrMenuPolled > 2 || sMenu.windowId == WINDOW_NONE || height == 0
     || gWindows[sMenu.windowId].tileData == NULL)
        return FALSE;
    count = sMenu.maxCursorPos - sMenu.minCursorPos + 1;
    if (count == 0 || count > CTR_FM_MAX)
        return FALSE;
    view->count = count;
    view->columns = columns;
    view->cursor = sMenu.cursorPos;
    view->yesNo = CtrMenu_YesNoOpen();
    for (i = 0; i < count; i++)
    {
        u8 row = i / columns, column = i % columns;
        s16 y0 = sMenu.top + row * height - 2;
        s16 x0 = columns > 1 ? sMenu.left + column * width : 0;
        s16 x1 = columns > 1 ? x0 + width : 256;

        CtrFieldMenu_EntryText(sMenu.windowId, x0, x1, y0, y0 + height, height, view->text[i]);
    }
    return TRUE;
}
