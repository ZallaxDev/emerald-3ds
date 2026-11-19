/*
 * The 3DS bottom screen's view of the game's scrolling lists
 * (3ds_game_views.h, CtrFieldList_*). Included at the end of src/list_menu.c
 * (under PLATFORM_3DS): the list is the one the game is asking for an answer
 * (ListMenu_ProcessInput, which tells it so), read from its own task data.
 * A tap on a row selects and answers as A would; the arrows scroll as the
 * D-pad does (CtrListMenu_Select, CtrListMenu_Scroll above).
 */

#include "3ds_game_views.h"

u8 *StringExpandPlaceholders(u8 *dest, const u8 *src);

// A game string is a pointer the port may map to the text of the language played.
#ifdef PORT_BRIDGE
const u8 *Port_ResolveTextPointer(const u8 *ptr);
#define CTR_RESOLVE_TEXT(str) Port_ResolveTextPointer(str)
#else
#define CTR_RESOLVE_TEXT(str) (str)
#endif

#define CTR_LIST_RAW 64             /* bytes of an item's name kept */
#define CTR_LIST_PLACEHOLDERS 2     /* placeholders of it kept: each can expand to a whole gStringVar */
#define CTR_LIST_EXPANDED_SIZE (CTR_LIST_RAW + CTR_LIST_PLACEHOLDERS * 0x100 + 1)

static u8 sCtrListTask;
static u32 sCtrListPolled = 0xFFFFFFF0;
static u8 sCtrListSerial;
static u8 sCtrListExpanded[CTR_LIST_EXPANDED_SIZE];
static struct {
    bool8 pending;
    u8 kind, serial;
    s16 value;
    u32 frame;
} sCtrListPick;

// A list is created (list_menu.c): a tap made on the one before is not for it.
static void CtrList_NewList(void)
{
    sCtrListSerial++;
}

// The bytes a name takes as StringExpandPlaceholders reads it from s: 1 for a
// character, a control code or a placeholder with what it takes after it. 0
// when the string ends inside one.
static u8 CtrList_UnitLength(const u8 *s)
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
// placeholder at a time, so none is cut in two: up to cap bytes, and at most
// maxPlaceholders placeholders. Ends with EOS and every byte after it is 0,
// so two copies of the same name are the same bytes.
static void CtrList_CopyUnits(u8 *dst, u8 cap, const u8 *src, u8 maxPlaceholders)
{
    u8 n = 0, len, placeholders = 0;

    while (src[n] != EOS)
    {
        len = CtrList_UnitLength(src + n);
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

// The task is a list's own: its slot may have been taken by another task since.
static bool8 CtrList_IsList(u8 taskId)
{
    return gTasks[taskId].isActive && gTasks[taskId].func == ListMenuDummyTask;
}

void CtrFieldList_Pick(u8 kind, s16 value, u8 serial)
{
    sCtrListPick.pending = TRUE;
    sCtrListPick.kind = kind;
    sCtrListPick.value = value;
    sCtrListPick.serial = serial;
    sCtrListPick.frame = gMain.vblankCounter1;
}

// ListMenu_ProcessInput, as it is asked: the list is waiting; and a tap
// from the bottom screen is acted on - TRUE with the answer to give. The
// sounds are the keys': the selection moving plays its own (the list's cursor
// callback), and what the answer does is the caller's.
bool8 CtrListField_Poll(u8 listTaskId, s32 *answer)
{
    struct ListMenu *list = (void *) gTasks[listTaskId].data;

    sCtrListTask = listTaskId;
    sCtrListPolled = gMain.vblankCounter1;
    if (!sCtrListPick.pending)
        return FALSE;
    sCtrListPick.pending = FALSE;
    if (gMain.vblankCounter1 - sCtrListPick.frame > 30 || sCtrListPick.serial != sCtrListSerial)
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
        if (list->scrollOffset + list->selectedRow >= list->template.totalItems)
            return FALSE;
        *answer = list->template.items[list->scrollOffset + list->selectedRow].id;
        return TRUE;
    }
    }
    return FALSE;
}

bool8 CtrFieldList_GetView(struct CtrFieldListView *view)
{
    struct ListMenu *list;
    u32 shown, game;
    u8 i, first = 0, raw[CTR_LIST_RAW + 1];

    memset(view, 0, sizeof(*view));
    if (gMain.vblankCounter1 - sCtrListPolled > 2 || !CtrList_IsList(sCtrListTask))
        return FALSE;
    list = (void *) gTasks[sCtrListTask].data;
    if (list->template.items == NULL || list->template.totalItems == 0 || list->scrollOffset >= list->template.totalItems)
        return FALSE;
    // The rows the game shows: no more than the list has left past its scroll.
    game = list->template.maxShowed;
    if (game > (u32)(list->template.totalItems - list->scrollOffset))
        game = list->template.totalItems - list->scrollOffset;
    if (game == 0)
        return FALSE;
    shown = game;
    if (shown > CTR_FL_ROWS)
    {
        // More rows than fit: the ones that do are those the cursor is on, or the first.
        shown = CTR_FL_ROWS;
        if (list->selectedRow >= CTR_FL_ROWS && list->selectedRow < game)
            first = list->selectedRow - (CTR_FL_ROWS - 1);
    }
    view->count = shown;
    view->first = first;
    view->selected = list->selectedRow >= first && list->selectedRow < first + shown ? list->selectedRow - first : 0xFF;
    view->total = list->template.totalItems;
    view->scroll = list->scrollOffset + first;
    view->serial = sCtrListSerial;
    for (i = 0; i < shown; i++)
    {
        const struct ListMenuItem *item = &list->template.items[list->scrollOffset + first + i];

        if (item->id == LIST_HEADER)
            view->header |= 1 << i;
        if (item->name == NULL)
            continue;
        CtrList_CopyUnits(raw, CTR_LIST_RAW, CTR_RESOLVE_TEXT(item->name), CTR_LIST_PLACEHOLDERS);
        StringExpandPlaceholders(sCtrListExpanded, raw);
        CtrList_CopyUnits(view->text[i], CTR_FM_TEXT - 1, sCtrListExpanded, 0);
    }
    return TRUE;
}
