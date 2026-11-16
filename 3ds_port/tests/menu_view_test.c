/*
 * The game's menus as the bottom screen sees them (include/3ds_menu_view.inc,
 * included here over mocks of menu.c's state): what each entry says from what
 * the game printed into the window - one line to an entry for the yes/no
 * box's two-line string, a grid's columns - the menu being waited on, and an
 * entry tapped chosen as A would.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

typedef uint8_t u8; typedef int8_t s8; typedef uint16_t u16; typedef int16_t s16; typedef uint32_t u32;
typedef u8 bool8;
#define TRUE 1
#define FALSE 0
#define NULL ((void *)0)
#define EOS 0xFF
#define CHAR_NEWLINE 0xFE
#define WINDOW_NONE 0xFF
#define MENU_B_PRESSED -1
#define CTR_VIEW_TEXT 160

/* The part of 3ds_game_views.h this reads (the header itself needs the game's own). */
#define GUARD_3DS_GAME_VIEWS_H
#define CTR_FM_MAX 12
#define CTR_FM_TEXT 24
struct CtrFieldMenuView { u8 count, columns, cursor, yesNo; u8 text[CTR_FM_MAX][CTR_FM_TEXT]; };

static struct { u8 windowId, minCursorPos, maxCursorPos, cursorPos, columns, optionWidth; s16 left, top, optionHeight; } sMenu;
static struct { void *tileData; } gWindows[32];
static struct { u32 vblankCounter1; } gMain;
static bool8 sYesNo;
static s8 sChosen = -1;

void StringExpandPlaceholders(u8 *dst, const u8 *src) { while ((*dst++ = *src++) != EOS) {} }
u8 *StringCopyN(u8 *dst, const u8 *src, u8 n) { u8 i; for (i = 0; i < n && src[i] != EOS; i++) dst[i] = src[i]; dst[i] = EOS; return dst + i; }
s8 CtrMenu_Choose(s8 entry, bool8 sound) { (void)sound; sChosen = entry; sMenu.cursorPos = entry; return entry; }
bool8 CtrMenu_YesNoOpen(void) { return sYesNo; }

/* A game string from ASCII, with \n as the game's newline. */
static const u8 *G(const char *s)
{
    static u8 buf[8][64];
    static int k;
    u8 *out = buf[k++ & 7];
    int i;

    for (i = 0; s[i]; i++)
        out[i] = s[i] == '\n' ? CHAR_NEWLINE : (u8)s[i];
    out[i] = EOS;
    return out;
}

#include "../include/3ds_menu_view.inc"

static int Same(const u8 *text, const char *want)
{
    const u8 *w = G(want);
    int i = 0;

    while (w[i] != EOS && text[i] != EOS && w[i] == text[i])
        i++;
    return w[i] == EOS && text[i] == EOS;
}

int main(void)
{
    struct CtrFieldMenuView v;
    s8 selection = 0;

    gMain.vblankCounter1 = 100;
    /* A list of three, printed at y = 1, 17, 33 in window 3 (PrintMenuTable). */
    gWindows[3].tileData = &gWindows;
    sMenu.windowId = 3; sMenu.minCursorPos = 0; sMenu.maxCursorPos = 2; sMenu.optionHeight = 16; sMenu.top = 1;
    CtrMenu_NotePrint(3, 8, 1, G("BUY"));
    CtrMenu_NotePrint(3, 8, 17, G("SELL"));
    CtrMenu_NotePrint(3, 8, 33, G("QUIT"));
    /* Not asked for an answer yet: no menu. */
    assert(!CtrFieldMenu_GetView(&v) && v.count == 0);
    CtrMenuField_Poll(&selection);
    assert(CtrFieldMenu_GetView(&v) && v.count == 3 && v.columns == 1 && !v.yesNo);
    assert(Same(v.text[0], "BUY") && Same(v.text[1], "SELL") && Same(v.text[2], "QUIT"));
    /* Asked again in the next frame or two: still open; not for a while: gone. */
    gMain.vblankCounter1 = 102;
    assert(CtrFieldMenu_GetView(&v));
    gMain.vblankCounter1 = 104;
    assert(!CtrFieldMenu_GetView(&v));

    /* The yes/no box: one string of two lines. */
    gMain.vblankCounter1 = 200;
    sMenu.windowId = 4; sMenu.maxCursorPos = 1; sYesNo = TRUE; gWindows[4].tileData = &gWindows;
    CtrMenu_NotePrint(4, 8, 1, G("YES\nNO"));
    CtrMenuField_Poll(&selection);
    assert(CtrFieldMenu_GetView(&v) && v.count == 2 && v.yesNo && Same(v.text[0], "YES") && Same(v.text[1], "NO"));
    sYesNo = FALSE;

    /* A grid of two columns, 80 wide, printed at x = 8 and 88. */
    sMenu.windowId = 5; sMenu.maxCursorPos = 3; sMenu.columns = 2; sMenu.optionWidth = 80; sMenu.left = 0; gWindows[5].tileData = &gWindows;
    CtrMenu_NotePrint(5, 8, 1, G("A")); CtrMenu_NotePrint(5, 88, 1, G("B"));
    CtrMenu_NotePrint(5, 8, 17, G("C")); CtrMenu_NotePrint(5, 88, 17, G("D"));
    CtrMenuField_Poll(&selection);
    assert(CtrFieldMenu_GetView(&v) && v.count == 4 && v.columns == 2);
    assert(Same(v.text[0], "A") && Same(v.text[1], "B") && Same(v.text[2], "C") && Same(v.text[3], "D"));
    sMenu.columns = 0;

    /* The same place printed again (another menu in the window) is what it now says. */
    sMenu.windowId = 3; sMenu.maxCursorPos = 1;
    CtrMenu_NotePrint(3, 8, 1, G("HEAL"));
    CtrMenuField_Poll(&selection);
    assert(CtrFieldMenu_GetView(&v) && v.count == 2 && Same(v.text[0], "HEAL") && Same(v.text[1], "SELL"));

    /* A tap: chosen as A would, given to the next ask, once. */
    CtrFieldMenu_Pick(1);
    assert(CtrMenuField_Poll(&selection) && selection == 1 && sChosen == 1);
    assert(!CtrMenuField_Poll(&selection));
    /* B is B. */
    CtrFieldMenu_Pick(-1);
    assert(CtrMenuField_Poll(&selection) && selection == MENU_B_PRESSED);
    /* An entry that is not in the menu, or a tap nobody took for long, gives nothing. */
    CtrFieldMenu_Pick(9);
    assert(!CtrMenuField_Poll(&selection));
    CtrFieldMenu_Pick(0);
    gMain.vblankCounter1 += 40;
    assert(!CtrMenuField_Poll(&selection));

    printf("PASS menu view: entries from what was printed (lines of one string, grid columns, a window reused), waiting and gone, a tap chosen once, B, an entry out of range, an old tap\n");
    return 0;
}
