/*
 * The 3DS bottom screen's view of the game's menus (3ds_game_views.h,
 * CtrFieldMenu_*). Included at the end of src/menu.c (under PLATFORM_3DS):
 * it reads that file's own sMenu. What the entries say is taken from what the
 * game printed into the menu's window (text.c, CtrMenu_NotePrint), so every
 * menu - a script's multichoice, the PC's, a yes/no box - needs nothing of
 * its own; an entry is chosen the way a tap on a list is (CtrMenu_Choose).
 *
 * A tap is tied to the menu it was made on: every menu set up, and every
 * window that goes away, changes sCtrMenuSerial, and a tap carries the serial
 * of the view it was made on, so it is not taken by the next menu to ask.
 */

#include "3ds_game_views.h"

// A game string is a pointer the port may map to the text of the language played.
#ifdef PORT_BRIDGE
const u8 *Port_ResolveTextPointer(const u8 *ptr);
#define CTR_RESOLVE_TEXT(str) Port_ResolveTextPointer(str)
#else
#define CTR_RESOLVE_TEXT(str) (str)
#endif

s8 CtrMenu_Choose(s8 entry, bool8 sound);
bool8 CtrMenu_YesNoOpen(void);

#define CTR_PRINT_RING 48
#define CTR_PRINT_RAW 64            /* bytes of a printed string kept */
#define CTR_PRINT_PLACEHOLDERS 2    /* placeholders of it kept: each can expand to a whole gStringVar */
#define CTR_EXPANDED_SIZE (CTR_PRINT_RAW + CTR_PRINT_PLACEHOLDERS * 0x100 + 1)

static struct {
    bool8 used, multi;              /* multi: more than one line, or placeholders that may be */
    u8 window, x, y;
    u32 seq;                        /* the order of the prints: the newest in a place is what it says */
    u8 text[CTR_PRINT_RAW + 1];
} sCtrPrints[CTR_PRINT_RING];
static u32 sCtrPrintSeq;
static u32 sCtrMenuPolled;
static u8 sCtrMenuSerial;
static bool8 sCtrMenuLocked;
static bool8 sCtrNoCancel;
static u8 sCtrNoCancelWindow;
static u8 sCtrExpanded[CTR_EXPANDED_SIZE];
static struct {
    bool8 pending;
    s8 entry;
    u8 serial;
    u32 frame;
} sCtrPick;

// The bytes a string takes as StringExpandPlaceholders reads it from s: 1 for a
// character, a control code or a placeholder with what it takes after it. 0
// when the string ends inside one.
static u8 CtrMenu_UnitLength(const u8 *s)
{
    u8 len, k;

    switch (s[0])
    {
    case PLACEHOLDER_BEGIN:
        len = 2;
        break;
    case EXT_CTRL_CODE_BEGIN:
        switch (s[1])
        {
        case EOS:
            return 0;
        case EXT_CTRL_CODE_RESET_FONT:
        case EXT_CTRL_CODE_PAUSE_UNTIL_PRESS:
        case EXT_CTRL_CODE_FILL_WINDOW:
        case EXT_CTRL_CODE_JPN:
        case EXT_CTRL_CODE_ENG:
        case EXT_CTRL_CODE_PAUSE_MUSIC:
        case EXT_CTRL_CODE_RESUME_MUSIC:
            len = 2;
            break;
        case EXT_CTRL_CODE_COLOR_HIGHLIGHT_SHADOW:
            len = 5;
            break;
        case EXT_CTRL_CODE_PLAY_BGM:
            len = 4;
            break;
        default:
            len = 3;
            break;
        }
        break;
    default:
        return 1;
    }
    for (k = 1; k < len; k++)
        if (s[k] == EOS)
            return 0;
    return len;
}

// src copied into dst (room for cap + 1 bytes) a character, control code or
// placeholder at a time, so none is cut in two: up to cap bytes, to the end of
// the string (or of its line), and at most maxPlaceholders placeholders. Ends
// with EOS and every byte after it is 0, so two copies of the same text are
// the same bytes.
static void CtrMenu_CopyUnits(u8 *dst, u8 cap, const u8 *src, u8 maxPlaceholders, bool8 oneLine)
{
    u8 n = 0, len, placeholders = 0;

    while (src[n] != EOS && !(oneLine && src[n] == CHAR_NEWLINE))
    {
        len = CtrMenu_UnitLength(src + n);
        if (len == 0 || n + len > cap)
            break;
        if (src[n] == PLACEHOLDER_BEGIN && ++placeholders > maxPlaceholders)
            break;
        memcpy(dst + n, src + n, len);
        n += len;
    }
    memset(dst + n, 0, cap - n + 1);
    dst[n] = EOS;
}

// text.c: a string printed into a window, at (x, y) in it.
void CtrMenu_NotePrint(u8 windowId, u8 x, u8 y, const u8 *str)
{
    u8 i, slot = CTR_PRINT_RING, freeSlot = CTR_PRINT_RING, oldest = CTR_PRINT_RING;

    if (str == NULL)
        return;
    // The menu's cursor, drawn where an entry's text is (and drawn again where it moved).
    if (str[0] == CHAR_BLACK_TRIANGLE && str[1] == EOS)
        return;
    for (i = 0; i < CTR_PRINT_RING; i++)
    {
        if (!sCtrPrints[i].used)
        {
            if (freeSlot == CTR_PRINT_RING)
                freeSlot = i;
        }
        else if (sCtrPrints[i].window == windowId && sCtrPrints[i].x == x && sCtrPrints[i].y == y)
        {
            slot = i;
            break;
        }
        else if (oldest == CTR_PRINT_RING || sCtrPrints[i].seq < sCtrPrints[oldest].seq)
        {
            oldest = i;
        }
    }
    if (slot == CTR_PRINT_RING)
        slot = freeSlot != CTR_PRINT_RING ? freeSlot : oldest;
    sCtrPrints[slot].used = TRUE;
    sCtrPrints[slot].window = windowId;
    sCtrPrints[slot].x = x;
    sCtrPrints[slot].y = y;
    sCtrPrints[slot].seq = ++sCtrPrintSeq;
    CtrMenu_CopyUnits(sCtrPrints[slot].text, CTR_PRINT_RAW, str, CTR_PRINT_PLACEHOLDERS, FALSE);
    sCtrPrints[slot].multi = FALSE;
    for (i = 0; sCtrPrints[slot].text[i] != EOS; i++)
        if (sCtrPrints[slot].text[i] == CHAR_NEWLINE || sCtrPrints[slot].text[i] == PLACEHOLDER_BEGIN)
            sCtrPrints[slot].multi = TRUE;
}

// A menu is set up (menu.c, every InitMenu*): what is asked of the one before
// is not asked of this.
static void CtrMenu_NewMenu(void)
{
    sCtrMenuSerial++;
    sCtrMenuLocked = FALSE;
    sCtrNoCancel = FALSE;
}

// A window is gone or whole again, and its number will serve another one. What
// was printed in it must not be read as the new window's entries, nor a tap
// made on it taken by what comes after.
void CtrMenu_ForgetWindow(u8 windowId)
{
    u8 i;

    for (i = 0; i < CTR_PRINT_RING; i++)
        if (sCtrPrints[i].window == windowId)
            sCtrPrints[i].used = FALSE;
    if (windowId == sYesNoWindowId)
        sCtrYesNoOpen = FALSE;
    if (windowId == sMenu.windowId)
    {
        sCtrMenuSerial++;
        sCtrNoCancel = FALSE;
    }
}

// window.c: every window is gone.
void CtrMenu_ForgetAll(void)
{
    u8 i;

    for (i = 0; i < CTR_PRINT_RING; i++)
        sCtrPrints[i].used = FALSE;
    sCtrYesNoOpen = FALSE;
    sCtrMenuSerial++;
    sCtrNoCancel = FALSE;
}

// A menu the game keeps itself, not through menu.c's cursor (the title menu):
// its code says each frame what the entries are and where its cursor is, and
// takes the entry a tap chose. The bottom screen sees it as any other menu.
static struct {
    u32 polled;
    u8 count, cursor;
    bool8 noCancel, locked;
    const u8 *text[CTR_FM_MAX];
} sCtrCustom;

// The menu just set up ignores B.
void CtrMenu_SetNoCancel(void)
{
    sCtrNoCancel = TRUE;
    sCtrNoCancelWindow = sMenu.windowId;
}

static bool8 CtrMenu_NoCancel(void)
{
    return sCtrNoCancel && sCtrNoCancelWindow == sMenu.windowId;
}

// The game's code, on a frame its menu is up but is not asked for an answer.
// The buttons stay, locked, and a tap made before is not acted on later.
void CtrMenu_Hold(void)
{
    if (sCtrCustom.count != 0 && gMain.vblankCounter1 - sCtrCustom.polled <= 2)
    {
        sCtrCustom.polled = gMain.vblankCounter1;
        sCtrCustom.locked = TRUE;
    }
    else
    {
        sCtrMenuPolled = gMain.vblankCounter1;
        sCtrMenuLocked = TRUE;
    }
    sCtrPick.pending = FALSE;
}

// Called by every one of menu.c's input functions as it is asked for an answer:
// the menu is waiting for one; and an entry tapped on the bottom screen is
// chosen as A would. Returns TRUE with the selection to give.
bool8 CtrMenuField_Poll(s8 *selection)
{
    sCtrMenuPolled = gMain.vblankCounter1;
    sCtrMenuLocked = FALSE;
    if (!sCtrPick.pending)
        return FALSE;
    sCtrPick.pending = FALSE;
    if (gMain.vblankCounter1 - sCtrPick.frame > 30 || sCtrPick.serial != sCtrMenuSerial)
        return FALSE;
    if (sCtrPick.entry < 0)
    {
        if (CtrMenu_NoCancel())
            return FALSE;
        *selection = MENU_B_PRESSED;
        return TRUE;
    }
    if (sCtrPick.entry < sMenu.minCursorPos || sCtrPick.entry > sMenu.maxCursorPos)
        return FALSE;
    CtrMenu_Choose(sCtrPick.entry, TRUE);
    *selection = sCtrPick.entry;
    return TRUE;
}

void CtrFieldMenu_Pick(s8 entry, u8 serial)
{
    // Locked, the menu takes no answer: a tap that was on its way is dropped.
    if ((sCtrMenuLocked && gMain.vblankCounter1 - sCtrMenuPolled <= 2)
     || (sCtrCustom.locked && gMain.vblankCounter1 - sCtrCustom.polled <= 2))
        return;
    sCtrPick.pending = TRUE;
    sCtrPick.entry = entry;
    sCtrPick.serial = serial;
    sCtrPick.frame = gMain.vblankCounter1;
}

void CtrMenu_Custom(u8 count, u8 cursor, const u8 *const *texts)
{
    u8 i;
    bool8 same = gMain.vblankCounter1 - sCtrCustom.polled <= 2;

    if (count > CTR_FM_MAX)
        count = CTR_FM_MAX;
    // A different menu (or the same one again after a while) is another to tap on.
    if (count != sCtrCustom.count)
        same = FALSE;
    for (i = 0; same && i < count; i++)
        if (sCtrCustom.text[i] != texts[i])
            same = FALSE;
    if (!same)
        sCtrMenuSerial++;
    sCtrCustom.polled = gMain.vblankCounter1;
    sCtrCustom.count = count;
    sCtrCustom.cursor = cursor;
    sCtrCustom.noCancel = FALSE;
    sCtrCustom.locked = FALSE;
    sCtrMenuLocked = FALSE;
    for (i = 0; i < count; i++)
        sCtrCustom.text[i] = texts[i];
}

// The menu of this frame's CtrMenu_Custom has no B: no CANCEL to offer.
void CtrMenu_CustomNoCancel(void)
{
    sCtrCustom.noCancel = TRUE;
}

// The entry tapped on a custom menu, TRUE with -1 for CANCEL (B), TRUE with
// the entry's number otherwise.
bool8 CtrMenu_CustomTake(s8 *entry)
{
    if (!sCtrPick.pending)
        return FALSE;
    sCtrPick.pending = FALSE;
    if (gMain.vblankCounter1 - sCtrPick.frame > 30 || sCtrPick.serial != sCtrMenuSerial
     || gMain.vblankCounter1 - sCtrCustom.polled > 2)
        return FALSE;
    if (sCtrPick.entry < 0)
    {
        if (sCtrCustom.noCancel)
            return FALSE;
        *entry = -1;
        return TRUE;
    }
    if (sCtrPick.entry >= sCtrCustom.count)
        return FALSE;
    *entry = sCtrPick.entry;
    return TRUE;
}

// The text printed at an entry's place in the menu's window, expanded. A
// string of several lines (the yes/no box's "YES\nNO") gives one entry a line.
// Where several prints reach the same place, the last one printed is it.
static void CtrFieldMenu_EntryText(u8 windowId, s16 x0, s16 x1, s16 y0, s16 y1, s16 height, u8 *out)
{
    u8 i, k;
    u32 newest = 0;

    memset(out, 0, CTR_FM_TEXT);
    out[0] = EOS;
    for (i = 0; i < CTR_PRINT_RING; i++)
    {
        const u8 *line;
        s16 y = sCtrPrints[i].y;

        if (!sCtrPrints[i].used || sCtrPrints[i].window != windowId)
            continue;
        if (sCtrPrints[i].x < x0 || sCtrPrints[i].x >= x1 || sCtrPrints[i].seq < newest)
            continue;
        // Its lines go down from y: the ones that could be at this entry's place, and no more.
        if (y >= y1 || y + (sCtrPrints[i].multi ? 7 * height : 0) < y0)
            continue;
        if (!sCtrPrints[i].multi)
        {
            line = sCtrPrints[i].text;
        }
        else
        {
            StringExpandPlaceholders(sCtrExpanded, sCtrPrints[i].text);
            line = sCtrExpanded;
            for (k = 0; k < 8 && !(y + k * height >= y0 && y + k * height < y1); k++)
            {
                while (*line != EOS && *line != CHAR_NEWLINE)
                    line++;
                if (*line == EOS)
                    break;
                line++;
            }
            if (k == 8 || (*line == EOS && !(y + k * height >= y0 && y + k * height < y1)))
                continue;
        }
        CtrMenu_CopyUnits(out, CTR_FM_TEXT - 1, line, 0, TRUE);
        newest = sCtrPrints[i].seq;
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
        u8 raw[CTR_PRINT_RAW + 1];

        view->count = sCtrCustom.count;
        view->columns = 1;
        view->cursor = sCtrCustom.cursor;
        view->noCancel = sCtrCustom.noCancel;
        view->locked = sCtrCustom.locked;
        view->serial = sCtrMenuSerial;
        for (i = 0; i < sCtrCustom.count; i++)
        {
            CtrMenu_CopyUnits(raw, CTR_PRINT_RAW, CTR_RESOLVE_TEXT(sCtrCustom.text[i]), CTR_PRINT_PLACEHOLDERS, FALSE);
            StringExpandPlaceholders(sCtrExpanded, raw);
            CtrMenu_CopyUnits(view->text[i], CTR_FM_TEXT - 1, sCtrExpanded, 0, TRUE);
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
    view->locked = sCtrMenuLocked;
    view->noCancel = CtrMenu_NoCancel();
    view->serial = sCtrMenuSerial;
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
