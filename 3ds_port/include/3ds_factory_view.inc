/*
 * The Rental Pokemon screens' view for the 3DS bottom screen (3ds_game_views.h).
 * Included at the end of src/battle_factory_screen.c (under PLATFORM_3DS): it
 * reads that file's own state. A tap is never written into the game's: it is
 * stored (CtrFactory_Act) and, each frame, turned into the one key that
 * takes the game's cursor a step nearer (or A, B), so the animations and
 * the popups are the game's own. The drawing is 3ds_bottom_factory.inc.
 */

#include "3ds_game_views.h"

#define CTR_FAC_TTL 240
/* The handlers' states that take input (STATE_*_HANDLE_INPUT, which are local to their functions). */
#define CTR_FAC_CHOOSE_STATE 1
#define CTR_FAC_MENU_STATE 3
#define CTR_FAC_YESNO_STATE 5

static u8 sCtrFacAct = CTR_FAC_ACT_NONE, sCtrFacA, sCtrFacTtl;

static u8 CtrFac_Screen(void)
{
    if (sFactorySelectScreen != NULL && gMain.callback2 == CB2_SelectScreen)
        return CTR_FAC_SELECT;
    if (sFactorySwapScreen != NULL && gMain.callback2 == Swap_CB2)
        return CTR_FAC_SWAP;
    return CTR_FAC_NONE;
}

static bool8 CtrFac_TaskIn(TaskFunc func, u8 state)
{
    u8 id = FindTaskIdByFunc(func);

    return id != TASK_NONE && gTasks[id].data[0] == state;
}

static u8 CtrFac_Phase(u8 screen)
{
    if (screen == CTR_FAC_SELECT)
    {
        if (sFactorySelectScreen->monPicAnimating)
            return CTR_FAC_BUSY;
        if (CtrFac_TaskIn(Select_Task_HandleChooseMons, CTR_FAC_CHOOSE_STATE))
            return CTR_FAC_CHOOSE;
        if (CtrFac_TaskIn(Select_Task_HandleMenu, CTR_FAC_MENU_STATE))
            return CTR_FAC_MENU;
        if (CtrFac_TaskIn(Select_Task_HandleYesNo, CTR_FAC_YESNO_STATE))
            return CTR_FAC_YESNO;
    }
    else if (screen == CTR_FAC_SWAP)
    {
        if (CtrFac_TaskIn(Swap_Task_HandleChooseMons, CTR_FAC_CHOOSE_STATE))
            return CTR_FAC_CHOOSE;
        if (sFactorySwapScreen->monPicAnimating)
            return CTR_FAC_BUSY;
        if (CtrFac_TaskIn(Swap_Task_HandleMenu, CTR_FAC_MENU_STATE))
            return CTR_FAC_MENU;
        if (CtrFac_TaskIn(Swap_Task_HandleYesNo, CTR_FAC_YESNO_STATE))
            return CTR_FAC_YESNO;
    }
    return CTR_FAC_BUSY;
}

/* The Pokemon the cursor is on, or was last on: the top screen's panel shows it. */
static void CtrFac_Focus(struct CtrFactoryView *view)
{
    static u8 sLastFocus;
    struct Pokemon *mon;
    u8 i;

    if (view->cursor < view->count)
        sLastFocus = view->cursor;
    if (sLastFocus >= view->count)
        sLastFocus = 0;
    view->focus = sLastFocus;
    if (view->screen == CTR_FAC_SELECT)
        mon = &sFactorySelectScreen->mons[sLastFocus].monData;
    else
        mon = &(sFactorySwapScreen->inEnemyScreen ? gEnemyParty : gPlayerParty)[sLastFocus];
    view->level = GetMonData(mon, MON_DATA_LEVEL, NULL);
    view->ability = GetMonAbility(mon);
    view->nature = GetNature(mon);
    view->item = GetMonData(mon, MON_DATA_HELD_ITEM, NULL);
    for (i = 0; i < MAX_MON_MOVES; i++)
        view->moves[i] = GetMonData(mon, MON_DATA_MOVE1 + i, NULL);
}

bool8 CtrFactory_GetView(struct CtrFactoryView *view)
{
    u8 i;

    memset(view, 0, sizeof(*view));
    view->screen = CtrFac_Screen();
    if (view->screen == CTR_FAC_NONE)
        return FALSE;
    view->phase = CtrFac_Phase(view->screen);
    if (view->screen == CTR_FAC_SELECT)
    {
        struct FactorySelectScreen *s = sFactorySelectScreen;

        view->count = SELECTABLE_MONS_COUNT;
        view->cursor = s->cursorPos;
        view->menuCursor = s->menuCursorPos;
        view->yesNoCursor = s->yesNoCursorPos;
        for (i = 0; i < SELECTABLE_MONS_COUNT; i++)
        {
            view->species[i] = GetMonData(&s->mons[i].monData, MON_DATA_SPECIES, NULL);
            view->personality[i] = GetMonData(&s->mons[i].monData, MON_DATA_PERSONALITY, NULL);
            view->order[i] = s->mons[i].selectedId;
            if (s->mons[i].selectedId)
                view->chosen++;
        }
        view->cursorChosen = s->cursorPos < SELECTABLE_MONS_COUNT && s->mons[s->cursorPos].selectedId != 0;
        view->question = gText_TheseThreePkmnOkay;
    }
    else
    {
        struct FactorySwapScreen *s = sFactorySwapScreen;
        struct Pokemon *party = s->inEnemyScreen ? gEnemyParty : gPlayerParty;

        view->count = FRONTIER_PARTY_SIZE;
        view->cursor = s->cursorPos;
        view->buttons = s->actionsCount > FRONTIER_PARTY_SIZE ? s->actionsCount - FRONTIER_PARTY_SIZE : 0;
        view->enemy = s->inEnemyScreen;
        view->menuCursor = s->menuCursorPos;
        view->yesNoCursor = s->yesNoCursorPos;
        for (i = 0; i < FRONTIER_PARTY_SIZE; i++)
        {
            view->species[i] = GetMonData(&party[i], MON_DATA_SPECIES, NULL);
            view->personality[i] = GetMonData(&party[i], MON_DATA_PERSONALITY, NULL);
        }
        view->question = s->monSwapped ? gText_AcceptThisPkmn : gText_QuitSwapping;
    }
    CtrFac_Focus(view);
    return TRUE;
}

void CtrFactory_Act(u8 kind, u8 a)
{
    sCtrFacAct = kind;
    sCtrFacA = a;
    sCtrFacTtl = CTR_FAC_TTL;
}

static u16 CtrFac_Toward(int now, int target, u16 less, u16 more)
{
    return now == target ? 0 : now < target ? more : less;
}

/* The key a tap pending asks for this frame, before the tasks read their input. */
static u16 CtrFac_NextKey(void)
{
    u8 screen = CtrFac_Screen(), phase;
    int cursor, target = sCtrFacA;
    u16 key;

    if (sCtrFacAct == CTR_FAC_ACT_NONE)
        return 0;
    if (screen == CTR_FAC_NONE || sCtrFacTtl == 0 || --sCtrFacTtl == 0)
    {
        sCtrFacAct = CTR_FAC_ACT_NONE;
        return 0;
    }
    phase = CtrFac_Phase(screen);
    cursor = screen == CTR_FAC_SELECT ? sFactorySelectScreen->cursorPos : sFactorySwapScreen->cursorPos;
    switch (sCtrFacAct)
    {
    case CTR_FAC_ACT_BACK:
        if (phase == CTR_FAC_BUSY)
            return 0;
        sCtrFacAct = CTR_FAC_ACT_NONE;
        return B_BUTTON;
    case CTR_FAC_ACT_CURSOR:
    case CTR_FAC_ACT_SLOT:
        if (phase != CTR_FAC_CHOOSE)
            return phase == CTR_FAC_BUSY ? 0 : (sCtrFacAct = CTR_FAC_ACT_NONE, 0);
        if (screen == CTR_FAC_SELECT || target < FRONTIER_PARTY_SIZE)
        {
            /* Among the Pokemon: left and right (a button is left with up first). */
            key = cursor >= FRONTIER_PARTY_SIZE && screen == CTR_FAC_SWAP ? DPAD_UP
                : CtrFac_Toward(cursor, target, DPAD_LEFT, DPAD_RIGHT);
        }
        else
        {
            key = cursor < FRONTIER_PARTY_SIZE ? DPAD_DOWN : CtrFac_Toward(cursor, target, DPAD_UP, DPAD_DOWN);
        }
        if (!key && sCtrFacAct == CTR_FAC_ACT_SLOT)
            key = A_BUTTON;
        if (!key || key == A_BUTTON)
            sCtrFacAct = CTR_FAC_ACT_NONE;
        return key;
    case CTR_FAC_ACT_OPTION:
        if (phase == CTR_FAC_CHOOSE)
            return A_BUTTON;
        if (phase == CTR_FAC_YESNO)
        {
            sCtrFacAct = CTR_FAC_ACT_NONE;
            return 0;
        }
        if (phase != CTR_FAC_MENU)
            return 0;
        cursor = screen == CTR_FAC_SELECT ? sFactorySelectScreen->menuCursorPos : sFactorySwapScreen->menuCursorPos;
        key = CtrFac_Toward(cursor, target, DPAD_UP, DPAD_DOWN);
        if (!key)
        {
            sCtrFacAct = CTR_FAC_ACT_NONE;
            return A_BUTTON;
        }
        return key;
    case CTR_FAC_ACT_YES:
    case CTR_FAC_ACT_NO:
        if (phase != CTR_FAC_YESNO)
            return 0;
        if (sCtrFacAct == CTR_FAC_ACT_NO)
        {
            sCtrFacAct = CTR_FAC_ACT_NONE;
            return B_BUTTON;
        }
        cursor = screen == CTR_FAC_SELECT ? sFactorySelectScreen->yesNoCursorPos : sFactorySwapScreen->yesNoCursorPos;
        if (cursor != 0)
            return DPAD_UP;
        sCtrFacAct = CTR_FAC_ACT_NONE;
        return A_BUTTON;
    }
    sCtrFacAct = CTR_FAC_ACT_NONE;
    return 0;
}

/* The top of each screen's main callback, before its tasks run. */
static void CtrFac_Inject(void)
{
    u16 key = CtrFac_NextKey();

    gMain.newKeys |= key;
    gMain.newAndRepeatedKeys |= key;
}
