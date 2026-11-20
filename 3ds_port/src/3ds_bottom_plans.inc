/* Part of 3ds_bottom_ui.c: buttons pressed for the player: one press, or a START entry. */

/* ------------------------------------------------------------------------ */
/* Pressing buttons for the player: one press, or a START entry             */
/* ------------------------------------------------------------------------ */

enum
{
    PLAN_NONE,
    PLAN_PRESS,      /* one press of `keys` */
    PLAN_START,      /* open start menu entry `target` */
};

typedef struct
{
    u8 kind, steps, wait, tries;
    bool8 release;
    s16 target;
    u16 keys;
} Plan;

static Plan sPlan;
static u16 sPadHeld;
static u16 sInjected;

static Plan MakePlan(u8 kind, s16 target)
{
    Plan p;

    memset(&p, 0, sizeof(p));
    p.kind = kind;
    p.target = target;
    return p;
}

static void StartPlan(u8 kind, s16 target)
{
    sPlan = MakePlan(kind, target);
}

/* One press of keys, as the player's own: the header's Y is SELECT. */
static void PressOnce(u16 keys)
{
    StartPlan(PLAN_PRESS, 0);
    sPlan.keys = keys;
}

static void FinishPlan(void)
{
    sPlan.kind = PLAN_NONE;
}

static void CancelPlan(void)
{
    if (sPlan.kind == PLAN_START)
        CtrStartMenu_Request(START_NONE);
    sPlan.kind = PLAN_NONE;
}

/* START with a request pending opens that entry (see start_menu.c). */
static void RunStartPlan(void)
{
    if (sPlan.tries && !CtrStartMenu_Pending())
    {
        FinishPlan();                             /* served */
        return;
    }
    if (++sPlan.wait < 8 && sPlan.tries)
        return;
    /* START only registers with the player free to move; try a few times. */
    if (sPlan.tries >= 6 || gMain.callback2 != CB2_Overworld || CtrStartMenu_Busy())
    {
        if (sPlan.tries >= 6 || ++sPlan.steps > 90)
            CancelPlan();
        return;
    }
    CtrStartMenu_Request(sPlan.target);
    sInjected = START_BUTTON;
    sPlan.release = TRUE;
    sPlan.wait = 0;
    ++sPlan.tries;
}

static void RunPlan(void)
{
    if (sPlan.kind == PLAN_NONE)
        return;
    /* The player's own buttons always win; not those the bottom screen holds back
     * (CtrBottom_FilterKeys), as the A that chose BAG while it opens. */
    if (CtrBottom_FilterKeys(CtrInput_Get()->held) & CTR_KEY_GAME)
    {
        CancelPlan();
        return;
    }
    /* A press only registers as new after a frame with the key up. */
    if (sPlan.release)
    {
        sPlan.release = FALSE;
        return;
    }
    switch (sPlan.kind)
    {
    case PLAN_PRESS:
        sInjected = sPlan.keys;
        FinishPlan();
        sPlan.release = TRUE;
        return;
    case PLAN_START:
        RunStartPlan();
        return;
    }
}

/* The keys the bottom screen presses for the game (a tap turned into a key), taken each frame. */

uint16_t CtrBottom_InjectedKeys(void)
{
    return sInjected | sPadHeld;
}
