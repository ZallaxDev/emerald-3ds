/*
 * The Berry Blender's view for the 3DS bottom screen (3ds_game_views.h).
 * Included at the end of src/berry_blender.c (under PLATFORM_3DS): it reads
 * that file's own sBerryBlender. The bowl keeps turning on the top screen;
 * the bottom one has the big button the game reads as A while it is taken,
 * and the figures.
 * The drawing is 3ds_bottom_blender.inc.
 */

#include "3ds_game_views.h"

/* The callbacks that make up the screen, from DoBerryBlending to the way out. */
bool8 CtrBlender_Active(void)
{
    MainCallback cb = gMain.callback2;

    return cb == DoBerryBlending || cb == CB2_LoadBerryBlender || cb == StartBlender
        || cb == CB2_StartBlenderLink || cb == CB2_StartBlenderLocal || cb == CB2_PlayBlender
        || cb == CB2_EndBlenderGame || cb == CB2_CheckPlayAgainLocal || cb == CB2_CheckPlayAgainLink;
}

/*
 * The bowl turns and the game reads A (CB2_PlayBlender), CTR_BLENDER_PLAY; the
 * berries going in and the results show the figures only, CTR_BLENDER_WATCH;
 * the rest (the intro text, the berry choice that is the bag's own) nothing.
 */
bool8 CtrBlender_GetView(struct CtrBlenderView *view)
{
    u8 i, j, n;
    bool8 play = gMain.callback2 == CB2_PlayBlender;

    memset(view, 0, sizeof(*view));
    if (sBerryBlender == NULL)
        return FALSE;
    if (play)
    {
        if (sBerryBlender->gameEndState != 0)
            return FALSE;
        view->state = CTR_BLENDER_PLAY;
    }
    else if (gMain.callback2 == CB2_EndBlenderGame || gMain.callback2 == CB2_CheckPlayAgainLocal
          || gMain.callback2 == CB2_CheckPlayAgainLink
          || ((gMain.callback2 == CB2_StartBlenderLink || gMain.callback2 == CB2_StartBlenderLocal)
              && sBerryBlender->mainState >= 11))
        view->state = CTR_BLENDER_WATCH;
    else
        return FALSE;
    n = sBerryBlender->numPlayers;
    if (n < 2 || n > CTR_BLENDER_PLAYERS)
    {
        view->state = 0;
        return FALSE;
    }
    view->numPlayers = n;
    view->own = gReceivedRemoteLinkPlayers ? GetMultiplayerId() : 0;
    /*
     * Whole revolutions, and the bar a step at a time: the bottom screen
     * redraws whole on any change, and these move every frame otherwise. The
     * live revolutions are not shown once the bowl slows down for the end.
     */
    if (play)
        view->rpm = ArrowSpeedToRPM(sBerryBlender->speed) / 100;
    view->maxRpm = sBerryBlender->maxRPM / 100;
    view->progress = sBerryBlender->maxProgressBarValue / CTR_BLENDER_STEP;
    view->progressMax = MAX_PROGRESS_BAR / CTR_BLENDER_STEP;
    for (i = 0; i < n; i++)
    {
        view->items[i] = sBerryBlender->chosenItemId[i];
        for (j = 0; j < NUM_SCORE_TYPES && j < 3; j++)
            view->scores[i][j] = sBerryBlender->scores[i][j];
    }
    return TRUE;
}
