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
/* Select: the message that a Pokemon cannot be picked twice, waiting for A; Swap: the same message's task. */
#define CTR_FAC_SELECT_INFO_STATE 11
#define CTR_FAC_SWAP_INFO_STATE 1

/* The tap being carried out, and the frames it has left; the Pokemon last in focus. */
static u8 sCtrFacAct = CTR_FAC_ACT_NONE, sCtrFacA, sCtrFacTtl;
static u8 sCtrFacLastFocus;

/* A new Rental session starts with nothing of the last one left. */
static void CtrFac_Reset(void)
{
    sCtrFacAct = CTR_FAC_ACT_NONE;
    sCtrFacA = 0;
    sCtrFacTtl = 0;
    sCtrFacLastFocus = 0;
}

/* The screen whose callback is running, its set-up callbacks included. */
static u8 CtrFac_Screen(void)
{
    MainCallback cb = gMain.callback2;

    if (cb == CB2_InitSelectScreen || cb == CB2_SelectScreen)
        return CTR_FAC_SELECT;
    if (cb == CB2_InitSwapScreen || cb == Swap_CB2)
        return CTR_FAC_SWAP;
    return CTR_FAC_NONE;
}

/* Its main callback: the tasks run, so a tap can be carried out. */
static bool8 CtrFac_Running(void)
{
    return gMain.callback2 == CB2_SelectScreen || gMain.callback2 == Swap_CB2;
}

/* The screen is on its way to the summary or out: it fades to black and its structures go. */
static bool8 CtrFac_Leaving(void)
{
    return FuncIsActiveTask(Select_Task_Exit) || FuncIsActiveTask(Swap_Task_Exit)
        || FuncIsActiveTask(Select_Task_OpenSummaryScreen) || FuncIsActiveTask(Swap_Task_OpenSummaryScreen);
}

/* A tap is only for the screen running now: one left over from a screen that is gone is dropped. */
static void CtrFac_DropStale(void)
{
    if (!CtrFac_Running())
        sCtrFacAct = CTR_FAC_ACT_NONE;
}

/* How far the screen's fade to black has gone (0..16), as it leaves: the top screen's panel follows it.
 * Asked for apart from the view, which would have the bottom screen drawn again at every step of it. */
u8 CtrFactory_Dark(void)
{
    /* The fade is the game's own: all palettes towards black, and it stays at
     * its end until the next one (or the next set-up) begins. mode 0 is the normal fade. */
    if (CtrFac_Running() && gPaletteFade.mode == 0 && gPaletteFade.blendColor == RGB_BLACK
     && gPaletteFade.targetY == 16 && !gPaletteFade.yDec)
        return gPaletteFade.y > 16 ? 16 : gPaletteFade.y;
    return 0;
}

bool8 CtrFactory_Active(void)
{
    CtrFac_DropStale();
    return CtrFac_Screen() != CTR_FAC_NONE;
}

static bool8 CtrFac_TaskIn(TaskFunc func, u8 state)
{
    u8 id = FindTaskIdByFunc(func);

    return id != TASK_NONE && gTasks[id].data[0] == state;
}

/* Only while the screen's structure exists (it is allocated part-way through the set-up). */
static u8 CtrFac_Phase(u8 screen)
{
    if (screen == CTR_FAC_SELECT && sFactorySelectScreen != NULL)
    {
        if (sFactorySelectScreen->monPicAnimating)
            return CTR_FAC_BUSY;
        if (CtrFac_TaskIn(Select_Task_HandleChooseMons, CTR_FAC_CHOOSE_STATE))
            return CTR_FAC_CHOOSE;
        if (CtrFac_TaskIn(Select_Task_HandleChooseMons, CTR_FAC_SELECT_INFO_STATE))
            return CTR_FAC_INFO;
        if (CtrFac_TaskIn(Select_Task_HandleMenu, CTR_FAC_MENU_STATE))
            return CTR_FAC_MENU;
        if (CtrFac_TaskIn(Select_Task_HandleYesNo, CTR_FAC_YESNO_STATE))
            return CTR_FAC_YESNO;
    }
    else if (screen == CTR_FAC_SWAP && sFactorySwapScreen != NULL)
    {
        if (CtrFac_TaskIn(Swap_Task_HandleChooseMons, CTR_FAC_CHOOSE_STATE))
            return CTR_FAC_CHOOSE;
        if (sFactorySwapScreen->monPicAnimating)
            return CTR_FAC_BUSY;
        if (CtrFac_TaskIn(Swap_TaskCantHaveSameMons, CTR_FAC_SWAP_INFO_STATE))
            return CTR_FAC_INFO;
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
    struct Pokemon *mon;
    u8 i;

    if (view->cursor < view->count)
        sCtrFacLastFocus = view->cursor;
    if (sCtrFacLastFocus >= view->count)
        sCtrFacLastFocus = 0;
    view->focus = sCtrFacLastFocus;
    if (view->screen == CTR_FAC_SELECT)
        mon = &sFactorySelectScreen->mons[sCtrFacLastFocus].monData;
    else
        mon = &(sFactorySwapScreen->inEnemyScreen ? gEnemyParty : gPlayerParty)[sCtrFacLastFocus];
    view->level = GetMonData(mon, MON_DATA_LEVEL, NULL);
    view->ability = GetMonAbility(mon);
    view->nature = GetNature(mon);
    view->item = GetMonData(mon, MON_DATA_HELD_ITEM, NULL);
    view->shiny = IsMonShiny(mon);
    for (i = 0; i < MAX_MON_MOVES; i++)
        view->moves[i] = GetMonData(mon, MON_DATA_MOVE1 + i, NULL);
}

bool8 CtrFactory_GetView(struct CtrFactoryView *view)
{
    u8 i;

    memset(view, 0, sizeof(*view));
    view->screen = CtrFac_Screen();
    CtrFac_DropStale();
    if (view->screen == CTR_FAC_NONE)
        return FALSE;
    view->phase = CTR_FAC_BUSY;
    if (view->screen == CTR_FAC_SELECT)
    {
        struct FactorySelectScreen *s = sFactorySelectScreen;

        /* The first time the screen is set up its structure does not exist yet: nothing to show but the screen. */
        if (s == NULL)
            return TRUE;
        view->phase = CtrFac_Phase(view->screen);
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
        view->question = view->phase == CTR_FAC_INFO ? gText_CantSelectSamePkmn : gText_TheseThreePkmnOkay;
    }
    else
    {
        struct FactorySwapScreen *s = sFactorySwapScreen;
        struct Pokemon *party;

        if (s == NULL)
            return TRUE;
        party = s->inEnemyScreen ? gEnemyParty : gPlayerParty;
        view->phase = CtrFac_Phase(view->screen);
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
        view->question = view->phase == CTR_FAC_INFO ? gText_SamePkmnInPartyAlready
                       : s->monSwapped ? gText_AcceptThisPkmn : gText_QuitSwapping;
    }
    CtrFac_Focus(view);
    return TRUE;
}

void CtrFactory_Act(u8 kind, u8 a)
{
    /* Only the running screen takes a tap, and not once it is leaving. */
    if (!CtrFac_Running() || CtrFac_Leaving())
    {
        sCtrFacAct = CTR_FAC_ACT_NONE;
        return;
    }
    sCtrFacAct = kind;
    sCtrFacA = a;
    sCtrFacTtl = CTR_FAC_TTL;
}

static u16 CtrFac_Toward(int now, int target, u16 less, u16 more)
{
    return now == target ? 0 : now < target ? more : less;
}

/*
 * The key a tap pending asks for this frame, before the tasks read their
 * input. Closed loop: every frame it looks at where the game is, so a popup,
 * an animation or a message in the way is waited for or closed, never
 * assumed. The game's keys it uses, per screen:
 *   Select  CHOOSE: left/right (the cursor), A (the popup); MENU: up/down, A, B
 *           (closes it); YESNO: up/down, A, B; INFO: A.
 *   Swap    CHOOSE: left/right among the Pokemon, down/up to a button and back,
 *           A, B (asks to quit); MENU: up/down, A, B; YESNO: up/down, A, B; INFO: A.
 */
static u16 CtrFac_NextKey(void)
{
    u8 screen = CtrFac_Screen(), phase;
    int cursor, target = sCtrFacA, count;
    u16 key;

    if (sCtrFacAct == CTR_FAC_ACT_NONE)
        return 0;
    if (!CtrFac_Running() || CtrFac_Leaving() || sCtrFacTtl == 0 || --sCtrFacTtl == 0)
    {
        sCtrFacAct = CTR_FAC_ACT_NONE;
        return 0;
    }
    phase = CtrFac_Phase(screen);
    /* The popup's options are only for the Select screen's A: on Swap the popup opens by itself. */
    if (sCtrFacAct == CTR_FAC_ACT_OPTION && screen == CTR_FAC_SWAP && phase != CTR_FAC_MENU)
    {
        sCtrFacAct = CTR_FAC_ACT_NONE;
        return 0;
    }
    /* An animation or a transition: wait for the game (the frames left bound it). */
    if (phase == CTR_FAC_BUSY)
        return 0;
    if (phase == CTR_FAC_INFO)
    {
        key = sCtrFacAct == CTR_FAC_ACT_OK || sCtrFacAct == CTR_FAC_ACT_BACK ? A_BUTTON : 0;
        sCtrFacAct = CTR_FAC_ACT_NONE;
        return key;
    }
    count = screen == CTR_FAC_SELECT ? SELECTABLE_MONS_COUNT : sFactorySwapScreen->actionsCount;
    cursor = screen == CTR_FAC_SELECT ? sFactorySelectScreen->cursorPos : sFactorySwapScreen->cursorPos;
    switch (sCtrFacAct)
    {
    case CTR_FAC_ACT_BACK:
        sCtrFacAct = CTR_FAC_ACT_NONE;
        return B_BUTTON;
    case CTR_FAC_ACT_CURSOR:
    case CTR_FAC_ACT_SLOT:
        /* A popup open: B closes it (the game's own animation), then on to the one tapped. */
        if (phase == CTR_FAC_MENU)
            return B_BUTTON;
        if (phase != CTR_FAC_CHOOSE || target >= count)
        {
            sCtrFacAct = CTR_FAC_ACT_NONE;
            return 0;
        }
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
            return A_BUTTON;    /* Select: opens the popup, whose option is then moved to */
        if (phase != CTR_FAC_MENU || target >= 3)
        {
            sCtrFacAct = CTR_FAC_ACT_NONE;
            return 0;
        }
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
        {
            sCtrFacAct = CTR_FAC_ACT_NONE;
            return 0;
        }
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
    u16 key;

    /* A key pressed on the console takes over from a tap being carried out. */
    if (sCtrFacAct != CTR_FAC_ACT_NONE && ((gMain.newKeys | gMain.newAndRepeatedKeys) & (A_BUTTON | B_BUTTON | DPAD_ANY)))
        sCtrFacAct = CTR_FAC_ACT_NONE;
    key = CtrFac_NextKey();
    gMain.newKeys |= key;
    gMain.newAndRepeatedKeys |= key;
}
