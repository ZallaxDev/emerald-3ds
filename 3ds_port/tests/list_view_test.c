/*
 * The game's scrolling lists as the bottom screen sees them
 * (include/3ds_list_view.inc over mocks of list_menu.c's state): the rows
 * shown with their names, the list being waited on, a row tapped chosen as A
 * would (its item's id), a scroll by a page, B, a heading that cannot be
 * chosen, a tap that is too old.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

typedef uint8_t u8; typedef int8_t s8; typedef uint16_t u16; typedef int16_t s16; typedef uint32_t u32; typedef int32_t s32;
typedef u8 bool8;
#define TRUE 1
#define FALSE 0
#define NULL ((void *)0)
#define EOS 0xFF
#define SE_SELECT 5
#define LIST_NOTHING_CHOSEN -1
#define LIST_CANCEL -2
#define LIST_HEADER -3

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

#define GUARD_3DS_GAME_VIEWS_H
#define CTR_FM_TEXT 24
#define CTR_FL_ROWS 8
struct CtrFieldListView { u8 count, selected; u16 total, scroll; u8 header; u8 first; u8 serial; u8 text[CTR_FL_ROWS][CTR_FM_TEXT]; };
enum { CTR_FL_ROW, CTR_FL_SCROLL, CTR_FL_CANCEL };

struct ListMenuItem { const u8 *name; s32 id; };
struct ListMenu {
    struct { const struct ListMenuItem *items; u16 totalItems, maxShowed; } template;
    u16 scrollOffset, selectedRow;
};
static void ListMenuDummyTask(u8 taskId) { (void)taskId; }
static void OtherTask(u8 taskId) { (void)taskId; }
static struct { void (*func)(u8); u8 isActive; s16 data[40]; } gTasks[8];
static struct { u32 vblankCounter1; } gMain;
static int sPlayed;
static int sSelects;

void PlaySE(int se) { (void)se; ++sPlayed; }
/* As the game's: a placeholder is replaced (1: RRR, 2: 255 L), a control code is copied with what it takes. */
u8 *StringExpandPlaceholders(u8 *dst, const u8 *src)
{
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

/* list_menu.c's, as it moves the window and puts the selection back on a row that can be chosen. */
static s32 FindRow(const struct ListMenu *list, s32 dir)
{
    s32 row, last = list->template.maxShowed;

    if (last > list->template.totalItems - list->scrollOffset)
        last = list->template.totalItems - list->scrollOffset;
    for (row = list->selectedRow + dir; row >= 0 && row < last; row += dir)
        if (list->template.items[list->scrollOffset + row].id != LIST_HEADER)
            return row;
    return -1;
}
s16 CtrListMenu_Scroll(u8 task, s16 rows)
{
    struct ListMenu *l = (void *)gTasks[task].data;
    s32 max = l->template.totalItems - l->template.maxShowed, scroll = l->scrollOffset + rows;

    if (scroll > max) scroll = max;
    if (scroll < 0) scroll = 0;
    rows = (s16)(scroll - l->scrollOffset);
    if (rows == 0)
        return 0;
    l->scrollOffset = (u16)scroll;
    if (l->template.items[l->scrollOffset + l->selectedRow].id == LIST_HEADER)
    {
        s32 row = FindRow(l, rows > 0 ? 1 : -1);

        if (row < 0)
            row = FindRow(l, rows > 0 ? -1 : 1);
        if (row >= 0)
            l->selectedRow = row;
    }
    ++sSelects;
    return rows;
}
void CtrListMenu_Select(u8 task, u16 row)
{
    struct ListMenu *l = (void *)gTasks[task].data;

    if (row == l->selectedRow || row >= l->template.maxShowed || l->scrollOffset + row >= l->template.totalItems
     || l->template.items[l->scrollOffset + row].id == LIST_HEADER)
        return;
    l->selectedRow = row;
    ++sSelects;
}

static const u8 *G(const char *s)
{
    static u8 buf[40][160];
    static int k;
    u8 *out = buf[k++ % 40];
    int i;

    for (i = 0; s[i]; i++) out[i] = (u8)s[i];
    out[i] = EOS;
    return out;
}
static int Same(const u8 *text, const char *want)
{
    int i = 0;

    while (want[i] && text[i] != EOS && (u8)want[i] == text[i]) i++;
    return !want[i] && text[i] == EOS;
}
/* Every byte after the string's end is 0: the view is compared as bytes to know whether it changed. */
static int Clean(const u8 *text)
{
    int i = 0;

    while (text[i] != EOS && i < CTR_FM_TEXT) i++;
    for (i++; i < CTR_FM_TEXT; i++)
        if (text[i] != 0) return 0;
    return 1;
}

#include "../include/3ds_list_view.inc"

/* A tap on what is on show: its first row and serial given as the bottom screen gives them. */
static void TapRow(u8 shownRow)
{
    struct CtrFieldListView v;

    CtrFieldList_GetView(&v);
    CtrFieldList_Pick(CTR_FL_ROW, v.first + shownRow, v.serial);
}

int main(void)
{
    static struct ListMenuItem items[20];
    static const char *names[10] = {"ONE", "TWO", "THREE", "FOUR", "FIVE", "SIX", "SEVEN", "EIGHT", "NINE", "TEN"};
    struct CtrFieldListView v;
    struct ListMenu *list = (void *)gTasks[2].data;
    s32 answer = 0;
    int i;

    for (i = 0; i < 10; i++) { items[i].name = G(names[i]); items[i].id = 100 + i; }
    items[3].id = LIST_HEADER;
    gTasks[2].isActive = TRUE; gTasks[2].func = ListMenuDummyTask;
    CtrList_NewList();
    list->template.items = items; list->template.totalItems = 10; list->template.maxShowed = 4;
    list->scrollOffset = 0; list->selectedRow = 1;
    gMain.vblankCounter1 = 500;

    /* Not asked for an answer yet: nothing. */
    assert(!CtrFieldList_GetView(&v) && v.count == 0);
    assert(!CtrListField_Poll(2, &answer));
    assert(CtrFieldList_GetView(&v) && v.count == 4 && v.selected == 1 && v.total == 10 && v.scroll == 0 && v.first == 0);
    assert(Same(v.text[0], "ONE") && Same(v.text[2], "THREE") && Same(v.text[3], "FOUR") && (v.header & 8) && !(v.header & 7));
    assert(Clean(v.text[0]) && Clean(v.text[2]));
    /* A while after, it is not waiting any more. */
    gMain.vblankCounter1 = 510;
    assert(!CtrFieldList_GetView(&v));
    gMain.vblankCounter1 = 600;

    /* A row tapped: selected, and its item's id given as A gives it. The sounds are the keys': none made here. */
    CtrListField_Poll(2, &answer);
    TapRow(2);
    assert(CtrListField_Poll(2, &answer) && answer == 102 && list->selectedRow == 2 && sPlayed == 0);
    assert(!CtrListField_Poll(2, &answer));
    /* The heading is not one to choose. */
    TapRow(3);
    assert(!CtrListField_Poll(2, &answer));
    /* A page down: the window moves, nothing is chosen. */
    CtrFieldList_GetView(&v);
    CtrFieldList_Pick(CTR_FL_SCROLL, 4, v.serial);
    assert(CtrListField_Poll(2, &answer) && answer == LIST_NOTHING_CHOSEN && list->scrollOffset == 4);
    assert(CtrFieldList_GetView(&v) && v.scroll == 4 && Same(v.text[0], "FIVE"));
    /* Not past the end: 10 items, 4 shown, the last window starts at 6; a window with fewer rows left shows fewer. */
    CtrFieldList_Pick(CTR_FL_SCROLL, 4, v.serial);
    assert(CtrListField_Poll(2, &answer) && list->scrollOffset == 6);
    list->scrollOffset = 8;
    assert(CtrFieldList_GetView(&v) && v.count == 2);
    list->scrollOffset = 6;
    /* B. */
    CtrFieldList_Pick(CTR_FL_CANCEL, 0, v.serial);
    assert(CtrListField_Poll(2, &answer) && answer == LIST_CANCEL);
    /* A tap nobody took for long gives nothing. */
    CtrFieldList_Pick(CTR_FL_ROW, 0, v.serial);
    gMain.vblankCounter1 += 40;
    assert(!CtrListField_Poll(2, &answer));
    gMain.vblankCounter1 += 1;
    CtrListField_Poll(2, &answer);

    /* A tap is tied to the list it was made on: the next list (or another created since) does not take it. */
    CtrFieldList_GetView(&v);
    CtrFieldList_Pick(CTR_FL_CANCEL, 0, v.serial);
    CtrList_NewList();
    assert(!CtrListField_Poll(2, &answer));
    CtrFieldList_GetView(&v);
    CtrFieldList_Pick(CTR_FL_CANCEL, 0, v.serial);
    assert(CtrListField_Poll(2, &answer) && answer == LIST_CANCEL);

    /* A list that is gone is not a list; nor is a slot another task has taken since. */
    gTasks[2].isActive = FALSE;
    assert(!CtrFieldList_GetView(&v));
    gTasks[2].isActive = TRUE; gTasks[2].func = OtherTask;
    assert(!CtrFieldList_GetView(&v));
    gTasks[2].func = ListMenuDummyTask;
    assert(CtrFieldList_GetView(&v));

    /* A scroll past the items (a list that shrank) is no list, not a count that wrapped. */
    list->scrollOffset = 10;
    assert(!CtrFieldList_GetView(&v) && v.count == 0);
    list->scrollOffset = 12;
    assert(!CtrFieldList_GetView(&v) && v.count == 0);
    list->scrollOffset = 0;
    list->template.totalItems = 0;
    assert(!CtrFieldList_GetView(&v));
    list->template.totalItems = 10;
    list->template.maxShowed = 0;
    assert(!CtrFieldList_GetView(&v));
    list->template.maxShowed = 4;

    /* A page onto a heading puts the selection on the nearest row that can be chosen, never before the first. */
    for (i = 0; i < 10; i++) items[i].id = 100 + i;
    items[4].id = LIST_HEADER; items[0].id = LIST_HEADER; items[1].id = LIST_HEADER;
    list->scrollOffset = 4; list->selectedRow = 0;        /* items 4..7, on the heading */
    list->scrollOffset = 8; list->selectedRow = 1;
    CtrListMenu_Scroll(2, -4);                            /* up to 4: row 1 is item 5, fine */
    assert(list->scrollOffset == 4 && list->selectedRow == 1);
    list->selectedRow = 0;
    CtrListMenu_Scroll(2, -4);                            /* up to 0: row 0 is a heading, row 1 too: down to row 2 */
    assert(list->scrollOffset == 0 && list->selectedRow == 2);
    list->scrollOffset = 2; list->selectedRow = 2;
    CtrListMenu_Scroll(2, 2);                             /* down to 4: row 2 is item 6 */
    assert(list->scrollOffset == 4 && list->selectedRow == 2);
    items[6].id = items[7].id = LIST_HEADER;
    list->scrollOffset = 2; list->selectedRow = 2;
    CtrListMenu_Scroll(2, 2);                             /* down to 4: rows 2 and 3 are headings, row 0 (heading) and 1: none below, up to row 1 */
    assert(list->scrollOffset == 4 && list->selectedRow == 1);
    list->scrollOffset = 0; list->selectedRow = 0;
    items[2].id = items[3].id = LIST_HEADER;
    CtrListMenu_Scroll(2, 0);                             /* nowhere to go: no move */
    assert(list->selectedRow == 0 && list->scrollOffset == 0);
    for (i = 0; i < 10; i++) items[i].id = 100 + i;

    /* A list of more rows than the view has: the cursor's row is always among those shown. */
    for (i = 0; i < 20; i++) { items[i].name = G("ROW"); items[i].id = 200 + i; }
    list->template.totalItems = 20; list->template.maxShowed = 10;
    list->scrollOffset = 3; list->selectedRow = 2;
    CtrListField_Poll(2, &answer);
    assert(CtrFieldList_GetView(&v) && v.count == 8 && v.first == 0 && v.selected == 2 && v.scroll == 3);
    list->selectedRow = 9;
    assert(CtrFieldList_GetView(&v) && v.count == 8 && v.first == 2 && v.selected == 7 && v.scroll == 5);
    list->selectedRow = 7;
    assert(CtrFieldList_GetView(&v) && v.first == 0 && v.selected == 7);
    /* A row tapped is the game's row: the first shown plus the row. */
    list->selectedRow = 9;
    CtrFieldList_GetView(&v);
    CtrFieldList_Pick(CTR_FL_ROW, v.first + 3, v.serial);
    assert(CtrListField_Poll(2, &answer) && answer == 200 + 3 + 3 + 2 && list->selectedRow == 5);
    /* A row the game's window has not (a row past the items, or past its window) is nothing. */
    CtrFieldList_Pick(CTR_FL_ROW, 10, v.serial);
    assert(!CtrListField_Poll(2, &answer));
    list->scrollOffset = 18; list->template.maxShowed = 10; list->template.totalItems = 20;
    assert(CtrFieldList_GetView(&v) && v.count == 2);
    CtrFieldList_Pick(CTR_FL_ROW, 2, v.serial);
    assert(!CtrListField_Poll(2, &answer));
    list->scrollOffset = 0;

    /* Names: kept whole units at a time, ended, zeros after, a placeholder expanded. */
    list->template.totalItems = 4; list->template.maxShowed = 4; list->selectedRow = 0;
    {
        u8 shortName[] = {'a', 'b', EOS};
        u8 holder[] = {'<', PLACEHOLDER_BEGIN, 1, '>', EOS};
        u8 cut[100], big[] = {PLACEHOLDER_BEGIN, 2, PLACEHOLDER_BEGIN, 2, PLACEHOLDER_BEGIN, 2, EOS};

        memset(cut, 'x', sizeof(cut));
        cut[20] = EXT_CTRL_CODE_BEGIN; cut[21] = EXT_CTRL_CODE_COLOR; cut[22] = 5; cut[23] = 'y'; cut[24] = 'y'; cut[99] = EOS;
        /* the long name's colour code is whole at the cut, or not in at all */
        items[0].name = shortName; items[1].name = holder; items[2].name = cut; items[3].name = big;
        CtrListField_Poll(2, &answer);
        assert(CtrFieldList_GetView(&v) && v.count == 4);
        assert(Same(v.text[0], "ab") && Clean(v.text[0]));
        assert(Same(v.text[1], "<RRR>") && Clean(v.text[1]));
        assert(Clean(v.text[2]));
        assert(v.text[2][20] == EXT_CTRL_CODE_BEGIN && v.text[2][21] == EXT_CTRL_CODE_COLOR && v.text[2][22] == 5);
        for (i = 0; v.text[2][i] != EOS; i++) {}
        assert(i == 23);
        assert(v.text[3][CTR_FM_TEXT - 2] == 'L' && v.text[3][CTR_FM_TEXT - 1] == EOS);
        /* a name that ends inside a control code is left out of it */
        {
            u8 half[] = {'h', EXT_CTRL_CODE_BEGIN, EOS};

            items[0].name = half;
            assert(CtrFieldList_GetView(&v) && Same(v.text[0], "h") && Clean(v.text[0]));
        }
    }

    printf("PASS list view: rows with their names and headings, the list being waited on, a row chosen as A (its item's id, no sound of its own), a scroll by a page, B, a heading (and the selection put back beside it), more rows than the view has, a slot another task took, a scroll past the items, names kept whole and ended, a tap tied to its list, an old tap\n");
    return 0;
}
