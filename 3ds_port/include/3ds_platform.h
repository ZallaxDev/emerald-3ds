#ifndef CTR_PLATFORM_H
#define CTR_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "3ds_log.h"
#include "3ds_input.h"

int64_t CtrPlatform_WallSeconds(void);

/* SDK-free boundary: game translation units never include libctru headers. */
typedef struct {
    float frameMs, workMs, peakWorkMs;
    /* Of the last frame: the game up to its VBlank wait, then audio and the
     * VBlank handler. What remains of workMs is the present. */
    float gameMs, vblankMs;
    /* ... of which the bottom screen, before the game ran (CtrBottom_Frame). */
    float bottomMs;
    uint32_t slowFrames;
} CtrTiming;

typedef struct
{
    void (*vblank)(void);
    void (*videoPresent)(void);
    void (*audioFrame)(void);
    void (*shutdown)(void);
} CtrPlatformHooks;

bool CtrPlatform_Init(void);
void CtrPlatform_Shutdown(void);
bool CtrPlatform_BeginFrame(void);
void CtrPlatform_EndFrame(void);
void CtrPlatform_SetHooks(const CtrPlatformHooks *hooks);
void CtrPlatform_RequestExit(void);
void CtrPlatform_Fatal(const char *reason) __attribute__((noreturn));
void CtrPlatform_ShowDataError(const char *title, const char *detail) __attribute__((noreturn));
uint64_t CtrPlatform_Milliseconds(void);
/* Raw ARM11 ticks, for game translation units that cannot include libctru. */
uint64_t CtrPlatform_Ticks(void);
float CtrPlatform_TickMs(uint64_t ticks);
uint64_t CtrPlatform_FrameCount(void);
const CtrTiming *CtrPlatform_GetTiming(void);
void CtrPlatform_NoteBottom(float ms);
void CtrPlatform_ReportMemory(const char *stage);
/*
 * Threads and locks for the port's own workers, usable from game translation
 * units. A CtrLock is a libctru LightLock. A thread is started detached, one
 * priority step below the caller, on core (-2: the application's default;
 * 1: the system core, asking for its share first, or the default if refused).
 */
typedef int32_t CtrLock;
void CtrLock_Init(CtrLock *lock);
void CtrLock_Lock(CtrLock *lock);
void CtrLock_Unlock(CtrLock *lock);
bool CtrPlatform_StartThread(void (*entry)(void *), void *arg, unsigned stack, int core);
void CtrPlatform_SleepUs(unsigned microseconds);
/* The frame profiler's report and reset, at present (3ds_prof.c). */
void CtrProf_EndFrame(float workMs, float waitMs, uint64_t frame);
/* What executable and console the log comes from, at boot; the last
 * segment, at exit. */
void CtrProf_LogIdentity(void);
/* The game's current screen (its callback2), as the profiler last heard. */
uintptr_t CtrProf_Scene(void);
void CtrProf_Close(void);

/*
 * Measured scenarios (3ds_scenario.c): a session recorded from boot - its
 * save, its settings and every frame's input - played back as many times as
 * wanted, so that two builds or two settings are compared on the same frames.
 * sdmc:/3ds/emerald3ds/scenario.txt chooses (its keys: 3ds_scenario.c).
 * While one plays, the save on the card is never written and the clock the
 * game reads is the recorded one, advanced a second every 60 frames.
 */
void CtrScenario_Init(void);
void CtrScenario_Close(void);
bool CtrScenario_Active(void);
bool CtrScenario_Playing(void);
/* The frame's input: recorded, or replaced by the recording. */
void CtrScenario_Input(CtrInputSample *sample);
/* The game clock while a scenario runs, in seconds since 1970. */
bool CtrScenario_Clock(int64_t *seconds);
/* The save image to load instead of the player's, or NULL. */
const char *CtrScenario_SavePath(void);
/* False while a scenario plays: the game's saves stay in memory. */
bool CtrScenario_SaveWrites(void);
/* The 3D slider, or the value the scenario fixes it at. */
float CtrScenario_Slider(float physical);
/* Fixes the slider outside a scenario too (the scene cycling test); below 0,
 * no longer. */
void CtrScenario_ForceSlider(float value);
/* The scene cycling test of phase F2 (3ds_cycle_test.c): nothing unless
 * built with CTR_CYCLE_TEST. Once a frame, and on each input sample. */
void CtrCycle_Frame(void);
void CtrCycle_Input(CtrInputSample *sample);
/* Game state digests (3ds_game_full.c): due every 600 frames and at the end
 * of the input; compared with the recording's while playing. */
bool CtrScenario_CheckpointDue(void);
void CtrScenario_Checkpoint(const char *what, uint32_t digest);
/* Settings applied for this session only (scenario.txt, the recording). */
void CtrSettings_ApplyLine(const char *line);
void CtrSettings_Defaults(void);
void CtrSettings_Freeze(void);
int CtrSettings_Text(char *text, unsigned size);

bool CtrFs_Init(void);
void CtrFs_Shutdown(void);
FILE *CtrFs_OpenAsset(const char *relativePath);
FILE *CtrFs_OpenData(const char *relativePath, const char *mode);

/* The port's own settings (settings.txt on the SD card, see 3ds_settings.c). */
void CtrSettings_Load(void);
void CtrSettings_Shutdown(void);
bool CtrSettings_Voxel(void);
void CtrSettings_SetVoxel(bool on);
/* Voxel camera pitch in degrees and zoom in percent, from a short fixed list. */
int CtrSettings_VoxelPitch(void);
int CtrSettings_VoxelZoom(void);
void CtrSettings_StepVoxelPitch(int direction);
void CtrSettings_StepVoxelZoom(int direction);
/* The voxel picture's tilt-shift blur: its strength in percent, 0 (off) to
 * 100 (the default) in steps of 10. */
bool CtrSettings_VoxelBlur(void);
int CtrSettings_VoxelBlurStrength(void);
void CtrSettings_StepVoxelBlur(int direction);
void CtrSettings_SetVoxelBlur(bool on);
/* Battles in front of the voxel world (3ds_video.c), off by default. */
bool CtrSettings_VoxelBattle(void);
void CtrSettings_SetVoxelBattle(bool on);
/* The FPS counter on the top screen, off by default. */
bool CtrSettings_ShowFps(void);
void CtrSettings_SetShowFps(bool on);
/* Running without holding B, B then walking (the bottom screen's RUN), off by default. */
bool CtrSettings_RunAlways(void);
void CtrSettings_SetRunAlways(bool on);
/* Settings kept by key (the OPTIONS pages of 3ds_extras.c). */
int CtrSettings_GetInt(const char *key, int fallback);
void CtrSettings_SetInt(const char *key, int value);
/* Fast-forward: game frames per shown frame in halves, 2 (1x) to 8 (4x),
 * 3 being 1.5x (OPTIONS, ZR/ZL). */
int CtrSettings_SpeedHalves(void);
void CtrSettings_StepSpeed(int direction, bool wrap);
/* The battle scene's own speed, in the same halves: the overworld's SPEED
 * until L or R is pressed in a battle (settings.txt battle_speed=). */
int CtrSettings_BattleSpeedHalves(void);
void CtrSettings_StepBattleSpeed(int direction, bool wrap);
/* The game says each frame whether the battle scene itself is on (not the
 * bag, the party or the summary a battle opens). While it is, and no link or
 * measured scenario holds the speed, L and R are the battle speed's keys
 * (CtrPlatform_BattleSpeedKeys) and the frames follow it. */
void CtrPlatform_SetBattleScene(bool on);
bool CtrPlatform_BattleSpeedKeys(void);
/* L (-1) or R (+1) pressed in the battle scene; shows the speed badge. With
 * `wrap` (R alone, when L is A) the speed goes round from 4x to 1x. */
void CtrPlatform_StepBattleSpeed(int direction, bool wrap);
/* The speed the badge on the top screen's corner shows, in halves, or 0: in
 * the battle scene, for two seconds after each press of L or R. */
int CtrPlatform_BattleSpeedBadge(void);
/* False on a fast-forwarded frame that is not shown: the sound engine skips
 * it, so music and effects keep their own speed (3ds_sound.c). */
bool CtrPlatform_SoundTick(void);

void CtrGame_Init(void);
/* The game's variables back to how AgbMain first saw them, AgbMain again:
 * returns only when the reset is unavailable. */
void CtrGame_SoftReset(void);
void CtrGame_VBlank(void);
/* The field is on screen (CB2_Overworld), whichever way it is drawn. */
bool CtrGame_IsOverworld(void);
bool CtrGame_IsOutdoor(void);
bool CtrGame_LocalTimeSeconds(unsigned *seconds);
/* A field effect is storing into VRAM directly (not through a copy). */
bool CtrGame_FieldRawVram(void);
bool CtrGame_FieldPoison(void);

/* C identifiers cannot start with '3'. Logs retain the plan's 3DS_STUB tag. */
#define CTR_STUB(id, message) CtrLog_Write(CTR_LOG_GAME, "[3DS_STUB] %s: %s", id, message)
#define CTR_UNIMPLEMENTED(id) CtrPlatform_Fatal("[3DS_UNIMPLEMENTED] " id)

#endif
