/*
 * The Berry Blender's view for the 3DS bottom screen (3ds_game_views.h).
 * Included at the end of src/berry_blender.c (under PLATFORM_3DS): it reads
 * that file's own sBerryBlender. The bowl keeps turning on the top screen;
 * the bottom one has the big button the game reads as A, and the figures.
 * The drawing is 3ds_bottom_blender.inc.
 */

#include "3ds_game_views.h"

bool8 CtrBlender_GetView(struct CtrBlenderView *view)
{
    u8 i, j, n;

    memset(view, 0, sizeof(*view));
    if (sBerryBlender == NULL || gMain.callback2 != CB2_PlayBlender || sBerryBlender->gameEndState != 0)
        return FALSE;
    n = sBerryBlender->numPlayers;
    if (n == 0 || n > CTR_BLENDER_PLAYERS)
        return FALSE;
    view->state = 1;
    view->numPlayers = n;
    view->own = gReceivedRemoteLinkPlayers ? GetMultiplayerId() : 0;
    // Whole revolutions: it moves a little every frame and the screen draws on a change.
    view->rpm = ArrowSpeedToRPM(sBerryBlender->speed) / 100;
    view->maxRpm = sBerryBlender->maxRPM / 100;
    view->progress = sBerryBlender->progressBarValue;
    view->progressMax = sBerryBlender->maxProgressBarValue;
    for (i = 0; i < n; i++)
    {
        view->items[i] = sBerryBlender->chosenItemId[i];
        for (j = 0; j < NUM_SCORE_TYPES && j < 3; j++)
            view->scores[i][j] = sBerryBlender->scores[i][j];
    }
    return TRUE;
}
