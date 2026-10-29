/*
 * Scene cycling for the GPU memory gate of phase F2 (-DCTR_CYCLE_TEST=cycles):
 * "100 scene cycles and a long session without a failure, corruption or loss
 * of 3D". Built only for that test; an ordinary build has none of it.
 *
 * From the player's save it presses A until the field is up, then cycles: the
 * voxel world on or off, the battle over it on or off and the 3D slider at 0
 * or 1 (the eight combinations in turn), a few seconds of field, a wild
 * battle, out of it by RUN (the action cursor set to it, A pressed), and back
 * to the field. Each cycle is logged; the frame profile's segments say how
 * much 3D each scene showed and GPUMEM what failed. The settings it changes
 * are not saved, and the game is never saved.
 */
#include <stdio.h>
#include "global.h"
#include "main.h"
#include "battle.h"
#include "battle_setup.h"
#include "script_pokemon_util.h"
#include "overworld.h"
#include "constants/species.h"
#include "constants/opponents.h"
#include "trainer_see.h"
#include "pokemon.h"
#include "3ds_platform.h"

#ifdef CTR_CYCLE_TEST

extern u8 gActionSelectionCursor[MAX_BATTLERS_COUNT];

#define CYCLE_FIELD_FRAMES 300
#define CYCLE_PRESS_EVERY 20

static u32 sFrame, sFieldSince, sCycle, sBattles;
static bool8 sStarted, sInBattle, sPress;

void CtrCycle_Frame(void)
{
    bool8 field = gMain.callback2 == CB2_Overworld && !gMain.inBattle;

    ++sFrame;
    if (!sStarted)
    {
        sStarted = TRUE;
        CtrSettings_Freeze();
        CtrSettings_ApplyLine("speed=1");
        CtrLog_Write(CTR_LOG_GAME, "CYCLE test: %u cycles", (unsigned)CTR_CYCLE_TEST);
    }
    sPress = FALSE;
    if (gMain.inBattle)
    {
        if (!sInBattle)
            ++sBattles;
        sInBattle = TRUE;
        for (unsigned i = 0; i < MAX_BATTLERS_COUNT; ++i)
            gActionSelectionCursor[i] = 3;
        sPress = (sFrame % CYCLE_PRESS_EVERY) < 2;
        sFieldSince = 0;
        return;
    }
    sInBattle = FALSE;
    if (!field)
    {
        /* The title and the main menu: CONTINUE is the default. */
        sPress = sCycle == 0 && (sFrame % 40) < 2;
        sFieldSince = 0;
        return;
    }
    if (!sFieldSince)
    {
        unsigned k = sCycle;
        char line[32];

        if (sCycle == CTR_CYCLE_TEST)
        {
            CtrLog_Write(CTR_LOG_GAME, "CYCLE test done: %lu cycles, %lu battles", (unsigned long)sCycle,
                         (unsigned long)sBattles);
            CtrPlatform_RequestExit();
            sFieldSince = sFrame;
            return;
        }
        snprintf(line, sizeof(line), "voxel=%u", k & 1);
        CtrSettings_ApplyLine(line);
        snprintf(line, sizeof(line), "voxel_battle=%u", (k >> 1) & 1);
        CtrSettings_ApplyLine(line);
        CtrScenario_ForceSlider((k >> 2) & 1 ? 1.0f : 0.0f);
        CtrLog_Write(CTR_LOG_GAME, "CYCLE %u voxel=%u battle3d=%u slider=%u at frame %lu", k, k & 1, (k >> 1) & 1,
                     (k >> 2) & 1, (unsigned long)sFrame);
        sFieldSince = sFrame;
        return;
    }
    if (sCycle < CTR_CYCLE_TEST && sFrame - sFieldSince == CYCLE_FIELD_FRAMES)
    {
        ++sCycle;
        CreateScriptedWildMon(SPECIES_ZIGZAGOON, 5, 0);
        BattleSetup_StartScriptedWildBattle();
    }
}

void CtrCycle_Input(CtrInputSample *sample)
{
    if (sPress)
        sample->buttons |= 1;
}

#elif defined(CTR_BATTLE_TEST)

/*
 * A battle on demand, for looking at the battle screen in an emulator
 * (-DCTR_BATTLE_TEST=SPECIES_x, optionally -DCTR_BATTLE_TEST_LEVEL=n, or
 * -DCTR_BATTLE_TEST_TRAINER=TRAINER_x for that trainer's battle - a double
 * one for a doubles trainer): a second and a half after the field is up,
 * every time it is; in battle A is pressed every 12 frames, so that its
 * turns play (FIGHT, the first move, the messages). A is pressed through the
 * title too, CONTINUE being the default.
 *
 * sdmc:/3ds/emerald3ds/battletest.txt, optional, one key=value a line, for
 * measuring one battle over and over: species= level= (the wild one), move=
 * (the lead's first move, its PP and both sides' HP kept full so the battle
 * never ends), trainer= (that trainer's battle instead, a double one for a
 * doubles trainer), slider= (the 3D slider fixed), frames= (the app quits after
 * that many battle frames, "BATTLE test done" in the log), vramleft= (KiB of
 * VRAM left free from battle frame vramat= on (1), the rest held, as on a
 * console with the voxel world's atlases in it).
 */
#ifndef CTR_BATTLE_TEST_LEVEL
#define CTR_BATTLE_TEST_LEVEL 50
#endif
static u32 sFrame, sFieldSince, sBattleFrames;
static bool8 sFieldSeen, sConfigRead;
static int sSpecies = CTR_BATTLE_TEST, sLevel = CTR_BATTLE_TEST_LEVEL, sMove, sQuitAfter, sTrainer, sVramLeft, sVramAt = 1;
void *vramAlloc(size_t size);
u32 vramSpaceFree(void);

static void BattleTestConfig(void)
{
    FILE *file = fopen("sdmc:/3ds/emerald3ds/battletest.txt", "r");
    char line[64];

    sConfigRead = TRUE;
    if (file == NULL) return;
    while (fgets(line, sizeof(line), file))
    {
        int value;
        float slider;

        if (sscanf(line, "species=%d", &value) == 1) sSpecies = value;
        else if (sscanf(line, "level=%d", &value) == 1) sLevel = value;
        else if (sscanf(line, "move=%d", &value) == 1) sMove = value;
        else if (sscanf(line, "frames=%d", &value) == 1) sQuitAfter = value;
        else if (sscanf(line, "trainer=%d", &value) == 1) sTrainer = value;
        else if (sscanf(line, "slider=%f", &slider) == 1) CtrScenario_ForceSlider(slider);
        else if (sscanf(line, "vramleft=%d", &value) == 1) sVramLeft = value;
        else if (sscanf(line, "vramat=%d", &value) == 1) sVramAt = value;
    }
    fclose(file);
    CtrLog_Write(CTR_LOG_GAME, "BATTLE test: species %d lv %d move %d, %d frames", sSpecies, sLevel, sMove,
                 sQuitAfter);
}

void CtrCycle_Frame(void)
{
    ++sFrame;
    if (!sConfigRead) BattleTestConfig();
    if (gMain.inBattle)
    {
        ++sBattleFrames;
        if (sVramLeft && sBattleFrames == (u32)sVramAt)
        {
            while (vramSpaceFree() > (u32)sVramLeft * 1024u + 8192u && vramAlloc(8192)) {}
            CtrLog_Write(CTR_LOG_GAME, "BATTLE test: VRAM held, %lu free", (unsigned long)vramSpaceFree());
        }
        if (sMove)
        {
            for (unsigned i = 0; i < gBattlersCount; ++i)
                gBattleMons[i].hp = gBattleMons[i].maxHP;
            gBattleMons[0].pp[0] = 30;
            gBattleMons[2].pp[0] = 30;
        }
        if (sQuitAfter && sBattleFrames == (u32)sQuitAfter)
        {
            CtrLog_Write(CTR_LOG_GAME, "BATTLE test done: %lu battle frames", (unsigned long)sBattleFrames);
            CtrPlatform_RequestExit();
        }
    }
    if (gMain.callback2 != CB2_Overworld || gMain.inBattle)
    {
        sFieldSince = 0;
        return;
    }
    sFieldSeen = TRUE;
    if (!sFieldSince)
        sFieldSince = sFrame;
    if (sFrame - sFieldSince == 90)
    {
        if (sMove)
        {
            SetMonMoveSlot(&gPlayerParty[0], sMove, 0);
            SetMonMoveSlot(&gPlayerParty[1], sMove, 0);
            CtrLog_Write(CTR_LOG_GAME, "BATTLE test: lead knows move %d", sMove);
        }
#ifdef CTR_BATTLE_TEST_TRAINER
        if (!sTrainer) sTrainer = CTR_BATTLE_TEST_TRAINER;
#endif
        if (sTrainer)
        {
            gNoOfApproachingTrainers = 1;
            gTrainerBattleOpponent_A = sTrainer;
            BattleSetup_StartTrainerBattle();
        }
        else
        {
            CreateScriptedWildMon(sSpecies, sLevel, 0);
            BattleSetup_StartScriptedWildBattle();
        }
    }
}

void CtrCycle_Input(CtrInputSample *sample)
{
    /* In battle, A every 12 frames: FIGHT, the first move, the messages. */
    if (gMain.inBattle && sFrame % 12 < 2)
        sample->buttons |= 1;
    /* The title and the main menu, until the field is first up. */
    if (!sFieldSeen && !gMain.inBattle && sFrame % 40 < 2)
        sample->buttons |= 1;
}

#else

void CtrCycle_Frame(void) {}
void CtrCycle_Input(CtrInputSample *sample) { (void)sample; }

#endif
