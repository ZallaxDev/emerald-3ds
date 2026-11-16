/*
 * The contest's move choice for the 3DS bottom screen (3ds_game_views.h).
 * Included at the end of src/contest.c (under PLATFORM_3DS): it reads that
 * file's own state. The stage stays on the top screen; the bottom one offers
 * the moves as buttons. A tap is stored and taken by the choice's own task
 * (CtrContest_Take), which then does what the D-pad, A or B would have done.
 * The drawing is 3ds_bottom_contest.inc.
 */

#include "3ds_game_views.h"

static u8 sCtrContestAct = 0xFF, sCtrContestValue;
static bool8 sCtrContestWaiting;

bool8 CtrContest_GetView(struct CtrContestView *view)
{
    u8 i;
    const struct ContestPokemon *mon;

    memset(view, 0, sizeof(*view));
    if (!sCtrContestWaiting)
        return FALSE;
    mon = &gContestMons[gContestPlayerMonIndex];
    view->state = 1;
    view->round = eContest.appealNumber + 1;
    view->rounds = CONTEST_NUM_APPEALS;
    view->species = mon->species;
    view->personality = 0;
    memcpy(view->nick, mon->nickname, CTR_CONTEST_NICK - 1);
    view->nick[CTR_CONTEST_NICK - 1] = EOS;
    for (i = 0; i < MAX_MON_MOVES; i++)
    {
        u16 move = mon->moves[i];
        const struct ContestEffect *effect = &gContestEffects[gContestMoves[move].effect];
        u8 hearts = effect->appeal == 0xFF ? 0 : effect->appeal / 10;
        u8 jam = effect->jam == 0xFF ? 0 : effect->jam / 10;

        if (move == MOVE_NONE)
            break;
        view->moves[i] = move;
        view->category[i] = gContestMoves[move].contestCategory;
        view->hearts[i] = hearts > MAX_CONTEST_MOVE_HEARTS ? MAX_CONTEST_MOVE_HEARTS : hearts;
        view->jam[i] = jam > MAX_CONTEST_MOVE_HEARTS ? MAX_CONTEST_MOVE_HEARTS : jam;
        if (eContestantStatus[gContestPlayerMonIndex].prevMove != MOVE_NONE
            && IsContestantAllowedToCombo(gContestPlayerMonIndex)
            && AreMovesContestCombo(eContestantStatus[gContestPlayerMonIndex].prevMove, move)
            && eContestantStatus[gContestPlayerMonIndex].hasJudgesAttention)
            view->flags[i] |= CTR_CONTEST_COMBO;
        else if (eContestantStatus[gContestPlayerMonIndex].prevMove == move
                 && gContestMoves[move].effect != CONTEST_EFFECT_REPETITION_NOT_BORING)
            view->flags[i] |= CTR_CONTEST_REPEAT;
    }
    view->count = i;
    view->selected = eContest.playerMoveChoice;
    view->effect = gContestEffectDescriptionPointers[gContestMoves[mon->moves[eContest.playerMoveChoice]].effect];
    return TRUE;
}

void CtrContest_Act(u8 kind, u8 value)
{
    sCtrContestAct = kind;
    sCtrContestValue = value;
}

/* Called by the choice's task each frame: what a tap asked for, as keys. */
static void CtrContest_Take(u16 *keys, bool8 *confirm, u8 numMoves)
{
    u8 act = sCtrContestAct;

    sCtrContestWaiting = TRUE;
    if (act == 0xFF)
        return;
    sCtrContestAct = 0xFF;
    if (act == CTR_CONTEST_CONFIRM)
        *confirm = TRUE;
    else if (act == CTR_CONTEST_CANCEL)
        *keys = B_BUTTON;
    else if (sCtrContestValue < numMoves && sCtrContestValue != eContest.playerMoveChoice)
    {
        EraseMoveSelectArrow(eContest.playerMoveChoice);
        eContest.playerMoveChoice = sCtrContestValue;
        DrawMoveSelectArrow(eContest.playerMoveChoice);
        PrintContestMoveDescription(gContestMons[gContestPlayerMonIndex].moves[eContest.playerMoveChoice]);
        PlaySE(SE_SELECT);
    }
}

static void CtrContest_Done(void)
{
    sCtrContestWaiting = FALSE;
    sCtrContestAct = 0xFF;
}
