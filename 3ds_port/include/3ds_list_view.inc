/*
 * The 3DS bottom screen's view of the game's scrolling lists
 * (3ds_game_views.h, CtrFieldList_*). Included at the end of src/list_menu.c
 * (under PLATFORM_3DS): the list is the one the game is asking for an answer
 * (ListMenu_ProcessInput, which tells it so), read from its own task data.
 * A tap on a row selects and answers as A would; the arrows scroll as the
 * D-pad does (CtrListMenu_Select, CtrListMenu_Scroll above).
 */

#include "3ds_game_views.h"

static u8 sCtrListTask;
static u32 sCtrListPolled = 0xFFFFFFF0;
static struct {
    bool8 pending;
    u8 kind;
    s16 value;
    u32 frame;
} sCtrListPick;

void CtrFieldList_Pick(u8 kind, s16 value)
{
    sCtrListPick.pending = TRUE;
    sCtrListPick.kind = kind;
    sCtrListPick.value = value;
    sCtrListPick.frame = gMain.vblankCounter1;
}

// ListMenu_ProcessInput, as it is asked: the list is waiting; and a tap
// from the bottom screen is acted on - TRUE with the answer to give.
bool8 CtrListField_Poll(u8 listTaskId, s32 *answer)
{
    struct ListMenu *list = (void *) gTasks[listTaskId].data;

    sCtrListTask = listTaskId;
    sCtrListPolled = gMain.vblankCounter1;
    if (!sCtrListPick.pending)
        return FALSE;
    sCtrListPick.pending = FALSE;
    if (gMain.vblankCounter1 - sCtrListPick.frame > 30)
        return FALSE;
    switch (sCtrListPick.kind)
    {
    case CTR_FL_CANCEL:
        *answer = LIST_CANCEL;
        return TRUE;
    case CTR_FL_SCROLL:
        CtrListMenu_Scroll(listTaskId, sCtrListPick.value);
        *answer = LIST_NOTHING_CHOSEN;
        return TRUE;
    case CTR_FL_ROW:
    {
        s32 item;

        if (sCtrListPick.value < 0 || sCtrListPick.value >= list->template.maxShowed
         || list->scrollOffset + sCtrListPick.value >= list->template.totalItems)
            return FALSE;
        item = list->template.items[list->scrollOffset + sCtrListPick.value].id;
        if (item == LIST_HEADER)
            return FALSE;
        CtrListMenu_Select(listTaskId, sCtrListPick.value);
        PlaySE(SE_SELECT);
        *answer = list->template.items[list->scrollOffset + list->selectedRow].id;
        return TRUE;
    }
    }
    return FALSE;
}

bool8 CtrFieldList_GetView(struct CtrFieldListView *view)
{
    struct ListMenu *list;
    u8 i, shown, buffer[2 * CTR_FM_TEXT + 16];

    memset(view, 0, sizeof(*view));
    if (gMain.vblankCounter1 - sCtrListPolled > 2 || !gTasks[sCtrListTask].isActive)
        return FALSE;
    list = (void *) gTasks[sCtrListTask].data;
    if (list->template.items == NULL || list->template.totalItems == 0)
        return FALSE;
    shown = list->template.maxShowed;
    if (shown > list->template.totalItems - list->scrollOffset)
        shown = list->template.totalItems - list->scrollOffset;
    if (shown > CTR_FL_ROWS)
        shown = CTR_FL_ROWS;
    view->count = shown;
    view->selected = list->selectedRow;
    view->total = list->template.totalItems;
    view->scroll = list->scrollOffset;
    for (i = 0; i < shown; i++)
    {
        const struct ListMenuItem *item = &list->template.items[list->scrollOffset + i];

        if (item->id == LIST_HEADER)
            view->header |= 1 << i;
        if (item->name == NULL)
            continue;
        StringExpandPlaceholders(buffer, item->name);
        StringCopyN(view->text[i], buffer, CTR_FM_TEXT - 1);
        view->text[i][CTR_FM_TEXT - 1] = EOS;
    }
    return TRUE;
}
