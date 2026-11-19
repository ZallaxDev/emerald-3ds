/*
 * The game's menus as the bottom screen sees them (include/3ds_menu_view.inc,
 * included here over mocks of menu.c's state): what each entry says from what
 * the game printed into the window - one line to an entry for the yes/no
 * box's two-line string, a grid's columns, the newest print of a place, never
 * the cursor's arrow - what a window forgotten leaves behind, the menu being
 * waited on (locked, with no B), an entry tapped chosen as A would, a tap tied
 * to the menu it was made on, and the title menu's custom menu.
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
#define CHAR_BLACK_TRIANGLE 0xEF
#define PLACEHOLDER_BEGIN 0xFD
#define EXT_CTRL_CODE_BEGIN 0xFC
#define EXT_CTRL_CODE_COLOR 0x01
#define EXT_CTRL_CODE_COLOR_HIGHLIGHT_SHADOW 0x04
#define EXT_CTRL_CODE_RESET_FONT 0x07
#define EXT_CTRL_CODE_PAUSE_UNTIL_PRESS 0x09
#define EXT_CTRL_CODE_PLAY_BGM 0x0B
#define EXT_CTRL_CODE_FILL_WINDOW 0x0F
#define EXT_CTRL_CODE_JPN 0x15
#define EXT_CTRL_CODE_ENG 0x16
#define EXT_CTRL_CODE_PAUSE_MUSIC 0x17
#define EXT_CTRL_CODE_RESUME_MUSIC 0x18
#define WINDOW_NONE 0xFF
#define MENU_B_PRESSED -1

/* The part of 3ds_game_views.h this reads (the header itself needs the game's own). */
#define GUARD_3DS_GAME_VIEWS_H
#define CTR_FM_MAX 12
#define CTR_FM_TEXT 24
struct CtrFieldMenuView { u8 count, columns, cursor, yesNo; u8 locked; u8 noCancel; u8 serial; u8 text[CTR_FM_MAX][CTR_FM_TEXT]; };

static struct { u8 windowId, minCursorPos, maxCursorPos, cursorPos, columns, optionWidth; s16 left, top, optionHeight; } sMenu;
static struct { void *tileData; } gWindows[32];
static struct { u32 vblankCounter1; } gMain;
static bool8 sCtrYesNoOpen;
static u8 sYesNoWindowId;
static s8 sChosen = -1;
static int sExpandCalls;

/* As the game's: a placeholder is replaced (1: RRR, 2: 255 L), a control code is copied with what it takes. */
u8 *StringExpandPlaceholders(u8 *dst, const u8 *src)
{
    sExpandCalls++;
    for (;;)
    {
        u8 c = *src++;

        switch (c)
        {
        case PLACEHOLDER_BEGIN:
        {
            u8 id = *src++;
            int n = id == 1 ? 3 : id == 2 ? 255 : 0;

            memset(dst, id == 1 ? 'R' : 'L', n);
            dst += n;
            break;
        }
        case EXT_CTRL_CODE_BEGIN:
            *dst++ = c;
            c = *src++;
            *dst++ = c;
            switch (c)
            {
            case EXT_CTRL_CODE_RESET_FONT: case EXT_CTRL_CODE_PAUSE_UNTIL_PRESS: case EXT_CTRL_CODE_FILL_WINDOW:
            case EXT_CTRL_CODE_JPN: case EXT_CTRL_CODE_ENG: case EXT_CTRL_CODE_PAUSE_MUSIC: case EXT_CTRL_CODE_RESUME_MUSIC:
                break;
            case EXT_CTRL_CODE_COLOR_HIGHLIGHT_SHADOW:
                *dst++ = *src++;
                /* fall through */
            case EXT_CTRL_CODE_PLAY_BGM:
                *dst++ = *src++;
                /* fall through */
            default:
                *dst++ = *src++;
            }
            break;
        case EOS:
            *dst = EOS;
            return dst;
        default:
            *dst++ = c;
        }
    }
}
/* As the game's: n bytes are copied, the string's end or not, and nothing is terminated. Nothing here may use it. */
u8 *StringCopyN(u8 *dst, const u8 *src, u8 n) { u8 i; for (i = 0; i < n; i++) dst[i] = src[i]; return &dst[n]; }
s8 CtrMenu_Choose(s8 entry, bool8 sound) { (void)sound; sChosen = entry; sMenu.cursorPos = entry; return entry; }
bool8 CtrMenu_YesNoOpen(void) { return sCtrYesNoOpen; }

/* A game string from ASCII, with \n as the game's newline. */
static const u8 *G(const char *s)
{
    static u8 buf[16][160];
    static int k;
    u8 *out = buf[k++ & 15];
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

/* Every byte after the string's end is 0: the view is compared as bytes to know whether it changed. */
static int Clean(const u8 *text)
{
    int i = 0;

    while (text[i] != EOS && i < CTR_FM_TEXT)
        i++;
    for (i++; i < CTR_FM_TEXT; i++)
        if (text[i] != 0)
            return 0;
    return 1;
}

static s8 selection;
static struct CtrFieldMenuView v;

/* A menu of n entries in window w being asked for an answer. */
static void Menu(u8 w, u8 n)
{
    gWindows[w].tileData = &gWindows;
    sMenu.windowId = w; sMenu.minCursorPos = 0; sMenu.maxCursorPos = n - 1; sMenu.optionHeight = 16; sMenu.top = 1;
    sMenu.columns = 0; sMenu.cursorPos = 0;
    CtrMenu_NewMenu();
    CtrMenuField_Poll(&selection);
}

/* A tap on an entry of what is on show. */
static void Tap(s8 entry)
{
    CtrFieldMenu_GetView(&v);
    CtrFieldMenu_Pick(entry, v.serial);
}

int main(void)
{
    const u8 *custom[2];
    u8 raw[100];
    int i;

    gMain.vblankCounter1 = 100;
    /* A list of three, printed at y = 1, 17, 33 in window 3 (PrintMenuTable). */
    sMenu.windowId = 3; sMenu.minCursorPos = 0; sMenu.maxCursorPos = 2; sMenu.optionHeight = 16; sMenu.top = 1;
    gWindows[3].tileData = &gWindows;
    CtrMenu_NotePrint(3, 8, 1, G("BUY"));
    CtrMenu_NotePrint(3, 8, 17, G("SELL"));
    CtrMenu_NotePrint(3, 8, 33, G("QUIT"));
    /* Not asked for an answer yet: no menu. */
    assert(!CtrFieldMenu_GetView(&v) && v.count == 0);
    CtrMenuField_Poll(&selection);
    assert(CtrFieldMenu_GetView(&v) && v.count == 3 && v.columns == 1 && !v.yesNo && !v.locked && !v.noCancel);
    assert(Same(v.text[0], "BUY") && Same(v.text[1], "SELL") && Same(v.text[2], "QUIT"));
    assert(Clean(v.text[0]) && Clean(v.text[1]) && Clean(v.text[2]));
    /* Asked again in the next frame or two: still open; not for a while: gone. */
    gMain.vblankCounter1 = 102;
    assert(CtrFieldMenu_GetView(&v));
    gMain.vblankCounter1 = 104;
    assert(!CtrFieldMenu_GetView(&v));

    /* The yes/no box: one string of two lines. */
    gMain.vblankCounter1 = 200;
    gWindows[4].tileData = &gWindows;
    sMenu.windowId = 4; sMenu.maxCursorPos = 1; sCtrYesNoOpen = TRUE; sYesNoWindowId = 4;
    CtrMenu_NotePrint(4, 8, 1, G("YES\nNO"));
    CtrMenuField_Poll(&selection);
    assert(CtrFieldMenu_GetView(&v) && v.count == 2 && v.yesNo && Same(v.text[0], "YES") && Same(v.text[1], "NO"));
    /* The box's window removed by anything but the box's own erasing: it is not open. */
    CtrMenu_ForgetWindow(4);
    assert(!sCtrYesNoOpen);
    sCtrYesNoOpen = FALSE;

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
    Tap(1);
    assert(CtrMenuField_Poll(&selection) && selection == 1 && sChosen == 1);
    assert(!CtrMenuField_Poll(&selection));
    /* B is B. */
    Tap(-1);
    assert(CtrMenuField_Poll(&selection) && selection == MENU_B_PRESSED);
    /* An entry that is not in the menu, or a tap nobody took for long, gives nothing. */
    Tap(9);
    assert(!CtrMenuField_Poll(&selection));
    Tap(0);
    gMain.vblankCounter1 += 40;
    assert(!CtrMenuField_Poll(&selection));

    /* The title menu's windows are gone (InitWindows) and the next menu in window 1 is the gender's:
       what the title menu printed there is not an entry of it. */
    CtrMenu_ForgetAll();
    CtrMenu_NotePrint(1, 8, 1, G("NEW GAME")); CtrMenu_NotePrint(1, 8, 17, G("CONTINUE")); CtrMenu_NotePrint(1, 8, 33, G("OPTION"));
    CtrMenu_ForgetAll();
    CtrMenu_NotePrint(1, 8, 1, G("BOY")); CtrMenu_NotePrint(1, 8, 17, G("GIRL"));
    Menu(1, 3);
    assert(CtrFieldMenu_GetView(&v) && v.count == 3 && Same(v.text[0], "BOY") && Same(v.text[1], "GIRL") && Same(v.text[2], ""));
    /* A window added again under a number it had (AddWindow) starts with nothing printed. */
    CtrMenu_ForgetWindow(1);
    CtrMenu_NotePrint(1, 8, 1, G("RED"));
    Menu(1, 2);
    assert(CtrFieldMenu_GetView(&v) && Same(v.text[0], "RED") && Same(v.text[1], ""));

    /* The cursor's arrow is printed where an entry's text is (and again where it moves): never an entry. */
    CtrMenu_ForgetAll();
    {
        u8 arrow[2] = {CHAR_BLACK_TRIANGLE, EOS};

        CtrMenu_NotePrint(2, 0, 1, arrow);
        CtrMenu_NotePrint(2, 8, 1, G("FIRST"));
        CtrMenu_NotePrint(2, 8, 17, G("SECOND"));
        CtrMenu_NotePrint(2, 0, 17, arrow);
        CtrMenu_NotePrint(2, 0, 1, arrow);
        Menu(2, 2);
        assert(CtrFieldMenu_GetView(&v) && Same(v.text[0], "FIRST") && Same(v.text[1], "SECOND"));
    }

    /* Where several prints reach an entry, the newest is it, whatever slot it is in. */
    CtrMenu_ForgetAll();
    CtrMenu_NotePrint(6, 8, 1, G("OLD"));
    CtrMenu_NotePrint(6, 4, 1, G("NEWER"));
    Menu(6, 1);
    assert(CtrFieldMenu_GetView(&v) && Same(v.text[0], "NEWER"));
    CtrMenu_NotePrint(6, 8, 1, G("NEWEST"));     /* the first slot again */
    assert(CtrFieldMenu_GetView(&v) && Same(v.text[0], "NEWEST"));
    /* A free slot is used before another print is evicted: 48 places, the first one printed is still there. */
    CtrMenu_ForgetAll();
    CtrMenu_NotePrint(7, 8, 1, G("KEPT"));
    CtrMenu_NotePrint(8, 0, 0, G("X"));
    CtrMenu_ForgetWindow(8);
    for (i = 0; i < CTR_PRINT_RING - 1; i++)
        CtrMenu_NotePrint(8, 8, (u8)(i + 100), G("Y"));
    Menu(7, 1);
    assert(CtrFieldMenu_GetView(&v) && Same(v.text[0], "KEPT"));
    /* A full ring gives up the oldest print, not the one that is next in turn. */
    CtrMenu_NotePrint(8, 8, 200, G("Z"));
    assert(CtrFieldMenu_GetView(&v) && Same(v.text[0], ""));
    CtrMenu_ForgetAll();

    /* A long string is kept whole units at a time: a control code is not cut, and the end is terminated. */
    memset(raw, 'a', sizeof(raw));
    raw[CTR_PRINT_RAW - 3] = EXT_CTRL_CODE_BEGIN; raw[CTR_PRINT_RAW - 2] = EXT_CTRL_CODE_COLOR; raw[CTR_PRINT_RAW - 1] = 5;
    raw[CTR_PRINT_RAW] = 'b'; raw[CTR_PRINT_RAW + 1] = EOS;
    CtrMenu_NotePrint(9, 8, 1, raw);
    {
        int slot = -1, n;

        for (i = 0; i < CTR_PRINT_RING; i++)
            if (sCtrPrints[i].used && sCtrPrints[i].window == 9)
                slot = i;
        assert(slot >= 0);
        for (n = 0; sCtrPrints[slot].text[n] != EOS; n++) {}
        assert(n == CTR_PRINT_RAW);          /* the code (3 bytes) fits whole */
        raw[CTR_PRINT_RAW - 3] = 'a'; raw[CTR_PRINT_RAW - 2] = EXT_CTRL_CODE_BEGIN; raw[CTR_PRINT_RAW - 1] = EXT_CTRL_CODE_COLOR;
        CtrMenu_NotePrint(9, 8, 1, raw);     /* the code would end one byte past the room: left out whole */
        for (n = 0; sCtrPrints[slot].text[n] != EOS; n++) {}
        assert(n == CTR_PRINT_RAW - 2 && sCtrPrints[slot].text[n - 1] == 'a');
        for (; n <= CTR_PRINT_RAW; n++)
            assert(n == CTR_PRINT_RAW - 2 || sCtrPrints[slot].text[n] == 0);
    }
    /* A string that ends inside a code is kept up to it. */
    {
        u8 cut[] = {'h', 'i', EXT_CTRL_CODE_BEGIN, EOS};

        CtrMenu_NotePrint(9, 8, 17, cut);
        Menu(9, 2);
        assert(CtrFieldMenu_GetView(&v) && Same(v.text[1], "hi") && Clean(v.text[1]));
    }
    CtrMenu_ForgetAll();
    /* A placeholder expands in the entry (once the entry is asked), and the ones past the second are not kept. */
    {
        u8 s[] = {'<', PLACEHOLDER_BEGIN, 1, '>', PLACEHOLDER_BEGIN, 1, PLACEHOLDER_BEGIN, 1, 'x', EOS};

        CtrMenu_NotePrint(10, 8, 1, s);
        Menu(10, 1);
        assert(CtrFieldMenu_GetView(&v) && Same(v.text[0], "<RRR>RRR") && Clean(v.text[0]));
        /* Long expansions are bounded: two placeholders of 255 letters then the text cut at the view's width. */
        {
            u8 big[] = {PLACEHOLDER_BEGIN, 2, PLACEHOLDER_BEGIN, 2, PLACEHOLDER_BEGIN, 2, EOS};

            CtrMenu_NotePrint(10, 8, 1, big);
            assert(CtrFieldMenu_GetView(&v) && v.text[0][CTR_FM_TEXT - 1] == EOS && v.text[0][CTR_FM_TEXT - 2] == 'L');
        }
    }
    CtrMenu_ForgetAll();

    /* A print outside an entry's lines is not expanded to be looked at: only multi-line strings and placeholders are. */
    CtrMenu_NotePrint(11, 8, 1, G("ONE"));
    CtrMenu_NotePrint(11, 8, 17, G("TWO"));
    CtrMenu_NotePrint(11, 8, 100, G("FAR"));
    Menu(11, 2);
    sExpandCalls = 0;
    assert(CtrFieldMenu_GetView(&v) && Same(v.text[0], "ONE") && Same(v.text[1], "TWO"));
    assert(sExpandCalls == 0);
    CtrMenu_ForgetAll();

    /* Locked: the game is not taking an answer (an animation of its own). The buttons stay; a tap on its way is dropped. */
    CtrMenu_NotePrint(12, 8, 1, G("BOY"));
    CtrMenu_NotePrint(12, 8, 17, G("GIRL"));
    Menu(12, 2);
    Tap(1);
    gMain.vblankCounter1 += 1;
    CtrMenu_Hold();
    assert(CtrFieldMenu_GetView(&v) && v.locked && v.count == 2 && Same(v.text[1], "GIRL"));
    Tap(0); gMain.vblankCounter1 += 1;
    CtrMenu_Hold();
    assert(CtrFieldMenu_GetView(&v) && v.locked);
    /* it holds the menu on for as long as it is called, with no ask */
    for (i = 0; i < 10; i++) { gMain.vblankCounter1++; CtrMenu_Hold(); assert(CtrFieldMenu_GetView(&v) && v.locked); }
    /* a tap made while it is locked is not acted on after */
    CtrFieldMenu_Pick(0, v.serial);
    assert(!CtrMenuField_Poll(&selection));
    assert(CtrFieldMenu_GetView(&v) && !v.locked);
    /* a tap on its way when Hold is called is dropped */
    CtrFieldMenu_Pick(1, v.serial);
    CtrMenu_Hold();
    assert(!CtrMenuField_Poll(&selection));
    /* asked for an answer again: unlocked, taps are taken */
    Tap(1);
    assert(CtrMenuField_Poll(&selection) && selection == 1);

    /* No B: the flag is the menu's, until it is another menu or its window is forgotten. */
    Menu(12, 2);
    assert(CtrFieldMenu_GetView(&v) && !v.noCancel);
    CtrMenu_SetNoCancel();
    assert(CtrFieldMenu_GetView(&v) && v.noCancel);
    Tap(-1);
    assert(!CtrMenuField_Poll(&selection));
    Tap(1);
    assert(CtrMenuField_Poll(&selection) && selection == 1);
    Menu(12, 2);
    assert(CtrFieldMenu_GetView(&v) && !v.noCancel);
    CtrMenu_SetNoCancel();
    CtrMenu_ForgetWindow(12);
    assert(CtrFieldMenu_GetView(&v) && !v.noCancel);
    CtrMenu_SetNoCancel();
    CtrMenu_ForgetAll();
    assert(CtrFieldMenu_GetView(&v) && !v.noCancel);
    CtrMenu_SetNoCancel();
    sMenu.windowId = 13;   /* the flag is of the window it was set on */
    assert(!CtrFieldMenu_GetView(&v) || !v.noCancel);
    sMenu.windowId = 12;

    /* A tap is tied to the menu it was made on: the next menu (the YES/NO that follows) does not take it. */
    CtrMenu_ForgetAll();
    CtrMenu_NotePrint(14, 8, 1, G("YES\nNO"));
    Menu(14, 2);
    Tap(0);
    Menu(14, 2);                                   /* another menu set up in the same window */
    assert(!CtrMenuField_Poll(&selection));
    Tap(0);
    CtrMenu_ForgetWindow(14);                      /* the window is gone */
    CtrMenu_NotePrint(14, 8, 1, G("YES\nNO"));
    assert(!CtrMenuField_Poll(&selection));
    Tap(1);
    assert(CtrMenuField_Poll(&selection) && selection == 1);

    /* The title menu: its code says the entries each frame, and takes the tap. */
    gMain.vblankCounter1 += 100;
    custom[0] = G("CONTINUE"); custom[1] = G("NEW GAME");
    CtrMenu_Custom(2, 1, custom);
    assert(CtrFieldMenu_GetView(&v) && v.count == 2 && v.cursor == 1 && !v.locked && !v.noCancel
        && Same(v.text[0], "CONTINUE") && Clean(v.text[0]) && Clean(v.text[1]));
    {
        s8 entry = 7;

        /* CANCEL (B) is -1, TRUE. */
        Tap(-1);
        CtrMenu_Custom(2, 1, custom);
        assert(CtrMenu_CustomTake(&entry) && entry == -1);
        assert(!CtrMenu_CustomTake(&entry));
        Tap(0);
        CtrMenu_Custom(2, 1, custom);
        assert(CtrMenu_CustomTake(&entry) && entry == 0);
        Tap(5);
        CtrMenu_Custom(2, 1, custom);
        assert(!CtrMenu_CustomTake(&entry));       /* not an entry */
        Tap(1);
        gMain.vblankCounter1 += 40;
        CtrMenu_Custom(2, 1, custom);
        assert(!CtrMenu_CustomTake(&entry));       /* too old */

        /* A one-button box with no B, and the title menu after it has B again. */
        custom[0] = G("OK");
        CtrMenu_Custom(1, 0, custom);
        CtrMenu_CustomNoCancel();
        assert(CtrFieldMenu_GetView(&v) && v.count == 1 && v.noCancel && Same(v.text[0], "OK"));
        Tap(-1);
        CtrMenu_Custom(1, 0, custom);
        CtrMenu_CustomNoCancel();
        assert(!CtrMenu_CustomTake(&entry));
        Tap(0);
        CtrMenu_Custom(1, 0, custom);
        CtrMenu_CustomNoCancel();
        assert(CtrMenu_CustomTake(&entry) && entry == 0);
        gMain.vblankCounter1++;
        custom[0] = G("CONTINUE");
        CtrMenu_Custom(2, 0, custom);
        assert(CtrFieldMenu_GetView(&v) && !v.noCancel);
        Tap(-1);
        CtrMenu_Custom(2, 0, custom);
        assert(CtrMenu_CustomTake(&entry) && entry == -1);

        /* Held (the title menu's fade-out after a choice, Custom not called): it stays, locked, taps dropped. */
        CtrMenu_Custom(2, 0, custom);
        Tap(0);
        for (i = 0; i < 10; i++)
        {
            gMain.vblankCounter1++;
            CtrMenu_Hold();
            assert(CtrFieldMenu_GetView(&v) && v.locked && v.count == 2 && Same(v.text[0], "CONTINUE"));
        }
        assert(!CtrMenu_CustomTake(&entry));       /* the tap made before was dropped */
        CtrFieldMenu_Pick(0, v.serial);            /* one made while locked too */
        gMain.vblankCounter1++;
        CtrMenu_Custom(2, 0, custom);
        assert(CtrFieldMenu_GetView(&v) && !v.locked);
        assert(!CtrMenu_CustomTake(&entry));
        /* Not held and not asked for: gone. */
        gMain.vblankCounter1 += 5;
        assert(!CtrFieldMenu_GetView(&v));
        /* A tap on the menu that was is not for the one that comes (the serial changes with the entries). */
        CtrMenu_Custom(2, 0, custom);
        Tap(0);
        custom[1] = G("OTHER");
        CtrMenu_Custom(2, 0, custom);
        assert(!CtrMenu_CustomTake(&entry));
    }

    printf("PASS menu view: entries from what was printed (lines of one string, grid columns, a window reused or forgotten, the newest print, never the arrow, units not cut, bounded), waiting and gone, locked, no B, a tap chosen once and tied to its menu, B, an entry out of range, an old tap, the custom menu (B is -1, no B, held)\n");
    return 0;
}
