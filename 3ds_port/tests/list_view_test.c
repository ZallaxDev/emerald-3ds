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

#define GUARD_3DS_GAME_VIEWS_H
#define CTR_FM_TEXT 24
#define CTR_FL_ROWS 8
struct CtrFieldListView { u8 count, selected; u16 total, scroll; u8 header; u8 text[CTR_FL_ROWS][CTR_FM_TEXT]; };
enum { CTR_FL_ROW, CTR_FL_SCROLL, CTR_FL_CANCEL };

struct ListMenuItem { const u8 *name; s32 id; };
struct ListMenu {
    struct { const struct ListMenuItem *items; u16 totalItems, maxShowed; } template;
    u16 scrollOffset, selectedRow;
};
static struct { u8 isActive; s16 data[40]; } gTasks[8];
static struct { u32 vblankCounter1; } gMain;
static int sPlayed;

void PlaySE(int se) { (void)se; ++sPlayed; }
void StringExpandPlaceholders(u8 *dst, const u8 *src) { while ((*dst++ = *src++) != EOS) {} }
u8 *StringCopyN(u8 *dst, const u8 *src, u8 n) { u8 i; for (i = 0; i < n && src[i] != EOS; i++) dst[i] = src[i]; dst[i] = EOS; return dst + i; }

/* Moves the window as the game's does; the selection stays on its row. */
s16 CtrListMenu_Scroll(u8 task, s16 rows)
{
    struct ListMenu *l = (void *)gTasks[task].data;
    s32 max = l->template.totalItems - l->template.maxShowed, scroll = l->scrollOffset + rows;

    if (scroll > max) scroll = max;
    if (scroll < 0) scroll = 0;
    rows = (s16)(scroll - l->scrollOffset);
    l->scrollOffset = (u16)scroll;
    return rows;
}
void CtrListMenu_Select(u8 task, u16 row) { ((struct ListMenu *)(void *)gTasks[task].data)->selectedRow = row; }

static const u8 *G(const char *s)
{
    static u8 buf[40][24];
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

#include "../include/3ds_list_view.inc"

int main(void)
{
    static struct ListMenuItem items[10];
    static const char *names[10] = {"ONE", "TWO", "THREE", "FOUR", "FIVE", "SIX", "SEVEN", "EIGHT", "NINE", "TEN"};
    struct CtrFieldListView v;
    struct ListMenu *list = (void *)gTasks[2].data;
    s32 answer = 0;

    for (int i = 0; i < 10; i++) { items[i].name = G(names[i]); items[i].id = 100 + i; }
    items[3].id = LIST_HEADER;
    gTasks[2].isActive = TRUE;
    list->template.items = items; list->template.totalItems = 10; list->template.maxShowed = 4;
    list->scrollOffset = 0; list->selectedRow = 1;
    gMain.vblankCounter1 = 500;

    /* Not asked for an answer yet: nothing. */
    assert(!CtrFieldList_GetView(&v) && v.count == 0);
    assert(!CtrListField_Poll(2, &answer));
    assert(CtrFieldList_GetView(&v) && v.count == 4 && v.selected == 1 && v.total == 10 && v.scroll == 0);
    assert(Same(v.text[0], "ONE") && Same(v.text[2], "THREE") && Same(v.text[3], "FOUR") && (v.header & 8) && !(v.header & 7));
    /* A while after, it is not waiting any more. */
    gMain.vblankCounter1 = 510;
    assert(!CtrFieldList_GetView(&v));
    gMain.vblankCounter1 = 600;

    /* A row tapped: selected, and its item's id given as A gives it. */
    CtrFieldList_Pick(CTR_FL_ROW, 2);
    assert(CtrListField_Poll(2, &answer) && answer == 102 && list->selectedRow == 2 && sPlayed == 1);
    assert(!CtrListField_Poll(2, &answer));
    /* The heading is not one to choose. */
    CtrFieldList_Pick(CTR_FL_ROW, 3);
    assert(!CtrListField_Poll(2, &answer));
    /* A page down: the window moves, nothing is chosen. */
    CtrFieldList_Pick(CTR_FL_SCROLL, 4);
    assert(CtrListField_Poll(2, &answer) && answer == LIST_NOTHING_CHOSEN && list->scrollOffset == 4);
    assert(CtrFieldList_GetView(&v) && v.scroll == 4 && Same(v.text[0], "FIVE"));
    /* Not past the end: 10 items, 4 shown, the last window starts at 6; a window with fewer rows left shows fewer. */
    CtrFieldList_Pick(CTR_FL_SCROLL, 4);
    assert(CtrListField_Poll(2, &answer) && list->scrollOffset == 6);
    list->scrollOffset = 8;
    assert(CtrFieldList_GetView(&v) && v.count == 2);
    list->scrollOffset = 6;
    /* B. */
    CtrFieldList_Pick(CTR_FL_CANCEL, 0);
    assert(CtrListField_Poll(2, &answer) && answer == LIST_CANCEL);
    /* A tap nobody took for long gives nothing. */
    CtrFieldList_Pick(CTR_FL_ROW, 0);
    gMain.vblankCounter1 += 40;
    assert(!CtrListField_Poll(2, &answer));
    /* A list task that is gone is not a list. */
    gTasks[2].isActive = FALSE;
    assert(!CtrFieldList_GetView(&v));

    printf("PASS list view: rows with their names and headings, the list being waited on, a row chosen as A (its item's id), a scroll by a page, B, a heading, an old tap\n");
    return 0;
}
