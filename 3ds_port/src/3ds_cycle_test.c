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
#include "battle_interface.h"
#include "script_pokemon_util.h"
#include "overworld.h"
#include "constants/species.h"
#include "constants/opponents.h"
#include "trainer_see.h"
#include "pokemon.h"
#include "string_util.h"
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
 *
 * Under a scenario's playback (an input of its own, all zeros, made by
 * scripts/make_idle_scenario.py) the clock and so the battle are the same
 * every run: the double battle of the engine measurements (trainer=271
 * move=57: TATE&LIZA against Surf).
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

#ifdef CTR_BATTLE_TEST_STATUS
/*
 * -DCTR_BATTLE_TEST_STATUS: from battle frame 400 on, every 400 frames the
 * battlers take the next row of statuses (all five pills across the rows,
 * the first two columns for a single battle), held every 20 frames against
 * the battle's own cures, so the info boxes' status labels can be looked at
 * in each language.
 */
static void BattleTestStatuses(void)
{
    static const u32 rows[3][4] = {
        {STATUS1_POISON, STATUS1_PARALYSIS, STATUS1_SLEEP_TURN(5), STATUS1_BURN},
        {STATUS1_FREEZE, STATUS1_TOXIC_POISON, STATUS1_BURN, STATUS1_PARALYSIS},
        {STATUS1_SLEEP_TURN(5), STATUS1_FREEZE, STATUS1_POISON, STATUS1_FREEZE},
    };
    unsigned i, phase = ((sBattleFrames - 400) / 400) % 3;

    if (sBattleFrames < 400 || sBattleFrames % 20 != 0) return;
    for (i = 0; i < gBattlersCount && i < 4; ++i)
    {
        struct Pokemon *mon = GetBattlerSide(i) == B_SIDE_PLAYER ? &gPlayerParty[gBattlerPartyIndexes[i]]
                                                                  : &gEnemyParty[gBattlerPartyIndexes[i]];
        u32 status = rows[phase][i];

        SetMonData(mon, MON_DATA_STATUS, &status);
        gBattleMons[i].status1 = status;
        UpdateHealthboxAttribute(gHealthboxSpriteIds[i], mon, HEALTHBOX_STATUS_ICON);
    }
}
#endif

void CtrCycle_Frame(void)
{
    ++sFrame;
    if (!sConfigRead) BattleTestConfig();
    if (gMain.inBattle)
    {
        ++sBattleFrames;
#ifdef CTR_BATTLE_TEST_STATUS
        BattleTestStatuses();
#endif
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
            /* Two of the player's own on the field for a doubles trainer: a
             * second one if the save has only the one, everyone healed. No
             * random IVs or personality, so a scenario's run stays the same. */
            if (GetMonData(&gPlayerParty[1], MON_DATA_SPECIES) == SPECIES_NONE)
            {
                CreateMon(&gPlayerParty[1], SPECIES_WAILMER, sLevel, 31, TRUE, 0, OT_ID_PLAYER_ID, 0);
                CalculatePlayerPartyCount();
            }
            HealPlayerParty();
            SetMonMoveSlot(&gPlayerParty[0], sMove, 0);
            SetMonMoveSlot(&gPlayerParty[1], sMove, 0);
            CtrLog_Write(CTR_LOG_GAME, "BATTLE test: lead knows move %d", sMove);
        }
#ifdef CTR_BATTLE_TEST_STATUS
        /* A party of its own: a save of another language keeps its nicknames
         * in that language's characters and length, which the battle's name
         * boxes of this build do not take. */
        ZeroPlayerPartyMons();
        CreateMon(&gPlayerParty[0], SPECIES_GENGAR, 48, 31, TRUE, 0, OT_ID_PLAYER_ID, 0);
        CreateMon(&gPlayerParty[1], SPECIES_UMBREON, 100, 31, TRUE, 0, OT_ID_PLAYER_ID, 0);
        CalculatePlayerPartyCount();
#endif
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

#elif defined(CTR_WEATHER_TEST)

#include "field_weather.h"
#include "constants/weather.h"

/*
 * The field's weather on demand, for frame dumps of the weather's sprites the
 * compositor carries over the view - the fog's and the ash's lattice, the
 * sandstorm, rain, snow, bubbles and clouds (-DCTR_WEATHER_TEST=frames): once
 * the field is up, each weather below in turn for that many field frames,
 * asked for again whenever a map's own takes over. Nothing is pressed (a
 * scenario's own input plays) and nothing is saved.
 */
static const u8 sWeathers[] = {
    WEATHER_FOG_HORIZONTAL, WEATHER_VOLCANIC_ASH, WEATHER_SANDSTORM, WEATHER_RAIN,
    WEATHER_SNOW, WEATHER_FOG_DIAGONAL, WEATHER_UNDERWATER_BUBBLES, WEATHER_SUNNY_CLOUDS,
};
static u32 sFieldFrames;

void CtrCycle_Frame(void)
{
    u8 wanted;

    if (gMain.callback2 != CB2_Overworld || gMain.inBattle) return;
    wanted = sWeathers[(sFieldFrames++ / CTR_WEATHER_TEST) % ARRAY_COUNT(sWeathers)];
    if (gWeatherPtr->nextWeather != wanted)
    {
        SetNextWeather(wanted);
        CtrLog_Write(CTR_LOG_GAME, "WEATHER test: weather %u at field frame %lu", (unsigned)wanted,
                     (unsigned long)sFieldFrames);
    }
}

void CtrCycle_Input(CtrInputSample *sample) { (void)sample; }

#elif defined(CTR_ROUTE_TEST)

/*
 * A ride on demand, for measuring the field on the move (-DCTR_ROUTE_TEST=laps):
 * A through the title, then the Mach Bike along the waypoints of
 * sdmc:/3ds/emerald3ds/route.txt ("group num x y" a line, each leg along one
 * axis, and "laps N" over the build's own count; a waypoint on another map than the player's is ridden towards in the
 * direction held last, across the connection), there and back `laps` times,
 * never stopping, and out ("ROUTE test done"). It steers by the player's own
 * square, so two builds whose frames cost differently ride the same way. Every
 * trainer counts as beaten; a wild battle is left by RUN. Under a scenario's
 * playback (an idle input, scripts/make_idle_scenario.py) the clock and the
 * save are the scenario's, and nothing is saved.
 */
#include "bike.h"
#include "event_object_movement.h"
#include "field_player_avatar.h"
#include "fieldmap.h"
#include "script.h"
#include "global.fieldmap.h"

#define ROUTE_POINTS 64
extern u8 gActionSelectionCursor[MAX_BATTLERS_COUNT];
static struct { s8 group, num; s16 x, y; } sPoints[ROUTE_POINTS];
static int sPointCount = -1, sNext, sStep = 1, sLapsLeft = CTR_ROUTE_TEST;
static u32 sFrame, sFieldFrames;
static u16 sHeld, sLast;

static void RouteRead(void)
{
    FILE *file = fopen("sdmc:/3ds/emerald3ds/route.txt", "r");
    char line[64];

    sPointCount = 0;
    if (file == NULL) return;
    while (sPointCount < ROUTE_POINTS && fgets(line, sizeof(line), file))
    {
        int g, n, x, y;

        if (sscanf(line, "laps %d", &g) == 1) sLapsLeft = g;
        else if (sscanf(line, "%d %d %d %d", &g, &n, &x, &y) == 4)
        {
            sPoints[sPointCount].group = g; sPoints[sPointCount].num = n;
            sPoints[sPointCount].x = x; sPoints[sPointCount].y = y;
            ++sPointCount;
        }
    }
    fclose(file);
    for (unsigned i = 1; i < TRAINERS_COUNT; ++i)
        SetTrainerFlag(i);
    CtrLog_Write(CTR_LOG_GAME, "ROUTE test: %d waypoints, %d laps", sPointCount, sLapsLeft);
}

void CtrCycle_Frame(void)
{
    const struct ObjectEvent *player;
    int x, y;

    ++sFrame;
    sHeld = 0;
    if (gMain.inBattle)
    {
        for (unsigned i = 0; i < MAX_BATTLERS_COUNT; ++i)
            gActionSelectionCursor[i] = 3;
        if ((sFrame % 20) < 2) sHeld = 1;
        return;
    }
    if (gMain.callback2 != CB2_Overworld)
    {
        if (sFieldFrames == 0 && (sFrame % 40) < 2) sHeld = 1;
        return;
    }
    if (sPointCount < 0) RouteRead();
    if (++sFieldFrames < 90 || sPointCount == 0) return;
    if (ArePlayerFieldControlsLocked())
    {
        /* A message (a sign, a call): on with B. */
        if ((sFrame % 20) < 2) sHeld = 2;
        return;
    }
    if (!TestPlayerAvatarFlags(PLAYER_AVATAR_FLAG_MACH_BIKE))
    {
        if (sFieldFrames % 30 == 0) GetOnOffBike(PLAYER_AVATAR_FLAG_MACH_BIKE);
        return;
    }
    player = &gObjectEvents[gPlayerAvatar.objectEventId];
    x = player->currentCoords.x - MAP_OFFSET;
    y = player->currentCoords.y - MAP_OFFSET;
    while (gSaveBlock1Ptr->location.mapGroup == sPoints[sNext].group
        && gSaveBlock1Ptr->location.mapNum == sPoints[sNext].num
        && x == sPoints[sNext].x && y == sPoints[sNext].y)
    {
        CtrLog_Write(CTR_LOG_GAME, "ROUTE test: waypoint %d at frame %lu", sNext, (unsigned long)sFrame);
        if ((sStep > 0 && sNext == sPointCount - 1) || (sStep < 0 && sNext == 0))
        {
            if (sStep < 0 && --sLapsLeft <= 0)
            {
                CtrLog_Write(CTR_LOG_GAME, "ROUTE test done at frame %lu", (unsigned long)sFrame);
                CtrPlatform_RequestExit();
                sPointCount = 0;
                return;
            }
            sStep = -sStep;
        }
        sNext += sStep;
    }
    if (gSaveBlock1Ptr->location.mapGroup == sPoints[sNext].group
     && gSaveBlock1Ptr->location.mapNum == sPoints[sNext].num)
    {
        if (x != sPoints[sNext].x && y != sPoints[sNext].y)
        {
            /* Off the leg: back onto it along the axis the last heading was not on. */
            if (sLast & (16 | 32)) sLast = y < sPoints[sNext].y ? 128 : 64;
            else sLast = x < sPoints[sNext].x ? 16 : 32;
        }
        else if (x != sPoints[sNext].x) sLast = x < sPoints[sNext].x ? 16 : 32;
        else sLast = y < sPoints[sNext].y ? 128 : 64;
    }
    sHeld = sLast;
}

void CtrCycle_Input(CtrInputSample *sample)
{
    sample->buttons |= sHeld;
}

#else

void CtrCycle_Frame(void) {}
void CtrCycle_Input(CtrInputSample *sample) { (void)sample; }

#endif
