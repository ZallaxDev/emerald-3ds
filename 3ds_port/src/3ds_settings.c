/*
 * The port's own settings, kept on the SD card next to the save
 * (sdmc:/3ds/emerald3ds/settings.txt) and written as soon as one changes.
 *
 * They are not part of the game's save: the GBA save block has no room for
 * them, and a setting that only sticks once the game is saved is not what a
 * player expects from a console option. One "key=value" per line; unknown
 * keys are ignored, so older and newer builds can share the file.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <3ds.h>
#include "3ds_platform.h"
#include "3ds_daynight.h"
#include "3ds_data.h"

#define SETTINGS_FILE "settings.txt"
/* Written first, then put in the file's place (WriteText). */
#define SETTINGS_TEMP "settings.txt.tmp"

/* The voxel overworld is opt-in: the classic 2D field is the default. */
static bool sVoxel = false;

/*
 * The voxel camera: a few degrees either way of its 40 degree pitch, and a
 * few zoom steps (percent of the adapted distance, higher is closer). The
 * renderer builds what the camera actually sees at any of them (its frustum,
 * ctr_voxel.c); further out or flatter would only cost more chunks per frame.
 */
static const int sPitches[] = {34, 37, 40, 43, 46};
static const int sZooms[] = {90, 100, 110, 120};
#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
static int sPitch = 2, sZoom = 1;
/* The HD-2D tilt-shift blur over the voxel picture (3ds_video.c): its
 * strength in percent, full unless turned down; 0 turns it off. Settings
 * written before it had a strength say 1 for on. */
static const int sBlurs[] = {0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
static int sBlur = 10;
/* Battles in front of the voxel world rather than the GBA's scenery
 * (3ds_video.c, RenderBattleWorld): off unless turned on. */
static bool sVoxelBattle = false;
/* The FPS counter in the top screen's corner (3ds_video.c): off unless
 * turned on. */
static bool sShowFps = false;
/* Running without holding B (the bottom screen's RUN button; B then walks):
 * off unless turned on. */
static bool sRunAlways = false;
/* Fast-forward: game frames per shown frame (3ds_platform.c, FrameShown),
 * in halves so 1.5x fits; 2 is normal speed. settings.txt writes it as
 * speed=1, speed=1.5, speed=2 ... */
static const int sSpeeds[] = {2, 3, 4, 6, 8};
static int sSpeed = 0;

/*
 * Settings without a variable of their own (the ENHANCEMENTS and CHEATS pages,
 * 3ds_extras.c): "key=number" lines kept as read and written back with the
 * rest. A key no build knows yet survives in the file too.
 */
#define EXTRA_KEYS 96
/* The longest line: a 23-character key, '=', an int's 11 characters, '\n'. */
#define EXTRA_LINE_BYTES 40
static struct { char key[24]; int value; } sExtra[EXTRA_KEYS];
static int sExtraCount;

static int ExtraIndex(const char *key, bool add)
{
    static bool sFullLogged;

    for (int i = 0; i < sExtraCount; ++i)
        if (strcmp(sExtra[i].key, key) == 0)
            return i;
    if (!add || strlen(key) >= sizeof(sExtra[0].key))
        return -1;
    if (sExtraCount == EXTRA_KEYS)
    {
        /* Not stored: the setting keeps its default and is not saved. */
        if (!sFullLogged)
            CtrLog_Write(CTR_LOG_ERROR, "settings: %d keys, the most kept; %s not saved", EXTRA_KEYS, key);
        sFullLogged = true;
        return -1;
    }
    strcpy(sExtra[sExtraCount].key, key);
    sExtra[sExtraCount].value = 0;
    return sExtraCount++;
}

static int Find(const int *values, int count, int value, int fallback)
{
    for (int i = 0; i < count; ++i)
        if (values[i] == value)
            return i;
    return fallback;
}

/*
 * Every setting as a new card has it, the extras included. A measured
 * scenario starts from here: its meta.txt holds what the recording's card
 * had, and a key that card never had is at its default there - not at
 * whatever this card says (an EXP Share on here made the same battle 437
 * frames longer than on the console it was recorded on).
 */
void CtrSettings_Defaults(void)
{
    sVoxel = false;
    sPitch = 2;
    sZoom = 1;
    sBlur = COUNT(sBlurs) - 1;
    sVoxelBattle = false;
    sShowFps = false;
    sRunAlways = false;
    sSpeed = 0;
    sExtraCount = 0;
}

/* One "key=value" line, from settings.txt or a measured scenario. */
void CtrSettings_ApplyLine(const char *line)
{
    int value;

    if (strncmp(line, "voxel=", 6) == 0)
        sVoxel = line[6] == '1';
    else if (sscanf(line, "voxel_pitch=%d", &value) == 1)
        sPitch = Find(sPitches, COUNT(sPitches), value, sPitch);
    else if (sscanf(line, "voxel_zoom=%d", &value) == 1)
        sZoom = Find(sZooms, COUNT(sZooms), value, sZoom);
    else if (sscanf(line, "voxel_blur=%d", &value) == 1)
        sBlur = Find(sBlurs, COUNT(sBlurs), value == 1 ? 100 : value, sBlur);
    else if (strncmp(line, "voxel_battle=", 13) == 0)
        sVoxelBattle = line[13] == '1';
    else if (strncmp(line, "fps=", 4) == 0)
        sShowFps = line[4] == '1';
    else if (strncmp(line, "run=", 4) == 0)
        sRunAlways = line[4] == '1';
    else if (strncmp(line, "speed=", 6) == 0)
        sSpeed = Find(sSpeeds, COUNT(sSpeeds), (int)(strtof(line + 6, NULL) * 2 + 0.5f), sSpeed);
    else
    {
        char key[sizeof(sExtra[0].key)];
        int index;

        if (sscanf(line, "%23[a-z0-9_]=%d", key, &value) == 2 && (index = ExtraIndex(key, true)) >= 0)
            sExtra[index].value = value;
    }
}

void CtrSettings_Load(void)
{
    FILE *file = CtrFs_OpenData(SETTINGS_FILE, "r");
    char line[64];

    /* A power cut between WriteText's remove and rename leaves only the
       temp file, complete. */
    if (file == NULL)
        file = CtrFs_OpenData(SETTINGS_TEMP, "r");
    if (file == NULL)
    {
        CtrLog_Write(CTR_LOG_FS, "settings: none yet, defaults (voxel=0 fps=0)");
        return;
    }
    while (fgets(line, sizeof(line), file))
        CtrSettings_ApplyLine(line);
    fclose(file);
    CtrLog_Write(CTR_LOG_FS, "settings: voxel=%d pitch=%d zoom=%d blur=%d battle=%d fps=%d run=%d speed=%d%s",
                 sVoxel ? 1 : 0, sPitches[sPitch], sZooms[sZoom], sBlurs[sBlur],
                 sVoxelBattle ? 1 : 0, sShowFps ? 1 : 0, sRunAlways ? 1 : 0,
                 sSpeeds[sSpeed] / 2, sSpeeds[sSpeed] & 1 ? ".5" : "");
}

/*
 * Written by a thread of its own. Opening, truncating and closing a file on
 * the SD card took a second on hardware, and the settings are changed from
 * the bottom screen in the middle of play: every tap on an option froze the
 * game for that long. The text is composed here and handed over; the thread
 * writes the latest one, waiting on the card while the game runs.
 */
static Thread sSaver;
static LightEvent sSaveWake;
static LightLock sSaveLock = 1;
static char sSaveText[160 + EXTRA_KEYS * EXTRA_LINE_BYTES];
static bool sSavePending, sSaveQuit;

/* Into a temp file, then in place of the old one: rewritten in place, a power
 * cut mid-write lost every setting. FAT's rename does not replace, so the old
 * file goes first; until the rename, Load reads the temp file. */
static void WriteText(const char *text)
{
    FILE *file = CtrFs_OpenData(SETTINGS_TEMP, "w");

    if (file == NULL)
    {
        CtrLog_Write(CTR_LOG_ERROR, "settings: could not open %s", SETTINGS_TEMP);
        return;
    }
    bool ok = fputs(text, file) >= 0;
    if (fclose(file) != 0)
        ok = false;
    if (!ok)
    {
        CtrLog_Write(CTR_LOG_ERROR, "settings: write failed for %s", SETTINGS_TEMP);
        remove(CTR_DATA_DIR SETTINGS_TEMP);
        return;
    }
    remove(CTR_DATA_DIR SETTINGS_FILE);
    if (rename(CTR_DATA_DIR SETTINGS_TEMP, CTR_DATA_DIR SETTINGS_FILE) != 0)
        CtrLog_Write(CTR_LOG_ERROR, "settings: could not replace %s", SETTINGS_FILE);
}

static void Saver(void *arg)
{
    /* Off the thread's 8 KB stack: the one saver thread uses it. */
    static char text[sizeof(sSaveText)];

    (void)arg;
    for (;;)
    {
        bool pending, quit;

        LightEvent_Wait(&sSaveWake);
        LightLock_Lock(&sSaveLock);
        pending = sSavePending;
        quit = sSaveQuit;
        sSavePending = false;
        memcpy(text, sSaveText, sizeof(text));
        LightLock_Unlock(&sSaveLock);
        if (pending)
            WriteText(text);
        if (quit)
            break;
    }
}

/* A measured scenario's settings are for its session alone. */
static bool sFrozen;

void CtrSettings_Freeze(void)
{
    sFrozen = true;
}

int CtrSettings_Text(char *text, unsigned size)
{
    int used = snprintf(text, size,
             "voxel=%d\nvoxel_pitch=%d\nvoxel_zoom=%d\nvoxel_blur=%d\nvoxel_battle=%d\nfps=%d\nrun=%d\n"
             "speed=%d%s\n",
             sVoxel ? 1 : 0, sPitches[sPitch], sZooms[sZoom], sBlurs[sBlur], sVoxelBattle ? 1 : 0,
             sShowFps ? 1 : 0, sRunAlways ? 1 : 0, sSpeeds[sSpeed] / 2, sSpeeds[sSpeed] & 1 ? ".5" : "");
    for (int i = 0; i < sExtraCount && used >= 0 && used < (int)size; ++i)
        used += snprintf(text + used, size - used, "%s=%d\n", sExtra[i].key, sExtra[i].value);
    return used;
}

static void Save(void)
{
    /* Game thread only (below): static, as the text has grown to 4 KB. */
    static char text[sizeof(sSaveText)];

    /* Settings and shutdown are submitted by the game thread. */
    if (sSaveQuit || sFrozen)
        return;
    CtrSettings_Text(text, sizeof(text));
    if (sSaver == NULL)
    {
        s32 priority = 0x30;

        LightEvent_Init(&sSaveWake, RESET_ONESHOT);
        svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
        sSaver = threadCreate(Saver, NULL, 8 * 1024, priority + 1 <= 0x3F ? priority + 1 : 0x3F,
                              -2, false);
        if (sSaver == NULL)
        {
            WriteText(text); /* no thread: written here, as before */
            return;
        }
    }
    LightLock_Lock(&sSaveLock);
    memcpy(sSaveText, text, sizeof(text));
    sSavePending = true;
    LightLock_Unlock(&sSaveLock);
    LightEvent_Signal(&sSaveWake);
}

void CtrSettings_Shutdown(void)
{
    LightLock_Lock(&sSaveLock);
    sSaveQuit = true;
    LightLock_Unlock(&sSaveLock);
    if (sSaver == NULL)
        return;
    LightEvent_Signal(&sSaveWake);
    threadJoin(sSaver, U64_MAX);
    threadFree(sSaver);
    sSaver = NULL;
}

bool CtrSettings_Voxel(void)
{
    return sVoxel;
}

int CtrSettings_GetInt(const char *key, int fallback)
{
    int index = ExtraIndex(key, false);

    return index >= 0 ? sExtra[index].value : fallback;
}

void CtrSettings_SetInt(const char *key, int value)
{
    int index = ExtraIndex(key, true);

    if (index < 0 || sExtra[index].value == value)
        return;
    if (strcmp(key, "clock_speed") == 0)
        CtrDayNight_Update();
    sExtra[index].value = value;
    Save();
    CtrLog_Write(CTR_LOG_FS, "settings: %s=%d", key, value);
}

void CtrSettings_SetVoxel(bool on)
{
    if (sVoxel == on)
        return;
    sVoxel = on;
    Save();
    CtrLog_Write(CTR_LOG_FS, "settings: voxel=%d", on ? 1 : 0);
}

int CtrSettings_VoxelPitch(void)
{
    return sPitches[sPitch];
}

int CtrSettings_VoxelZoom(void)
{
    return sZooms[sZoom];
}

/* One step up or down, wrapping round like the game's own options. */
static void Step(int *index, int count, int direction)
{
    *index = (*index + (direction < 0 ? count - 1 : 1)) % count;
    Save();
}

void CtrSettings_StepVoxelPitch(int direction)
{
    Step(&sPitch, COUNT(sPitches), direction);
}

void CtrSettings_StepVoxelZoom(int direction)
{
    Step(&sZoom, COUNT(sZooms), direction);
}

int CtrSettings_SpeedHalves(void)
{
    return sSpeeds[sSpeed];
}

/* The OPTIONS cell wraps round like the others; ZR and ZL stop at the ends. */
void CtrSettings_StepSpeed(int direction, bool wrap)
{
    int next = sSpeed + (direction < 0 ? -1 : 1);

    if (!wrap && (next < 0 || next >= COUNT(sSpeeds)))
        return;
    Step(&sSpeed, COUNT(sSpeeds), direction);
    CtrLog_Write(CTR_LOG_FS, "settings: speed=%d%s", sSpeeds[sSpeed] / 2, sSpeeds[sSpeed] & 1 ? ".5" : "");
}

/*
 * The battle scene's own fast-forward (L and R in battle, 3ds_platform.c):
 * settings.txt's battle_speed=, in halves like sSpeeds. 0, as a new card
 * has it, means the battle follows the SPEED the player uses in the
 * overworld; the first L or R press in a battle sets one of its own.
 */
int CtrSettings_BattleSpeedHalves(void)
{
    int halves = CtrSettings_GetInt("battle_speed", 0);

    return Find(sSpeeds, COUNT(sSpeeds), halves, -1) >= 0 ? halves : sSpeeds[sSpeed];
}

/* Stops at 1x and 4x like ZR and ZL, or goes round (4x to 1x) when `wrap`. */
void CtrSettings_StepBattleSpeed(int direction, bool wrap)
{
    int next = Find(sSpeeds, COUNT(sSpeeds), CtrSettings_BattleSpeedHalves(), 0) + (direction < 0 ? -1 : 1);

    if (wrap)
        next = (next + COUNT(sSpeeds)) % COUNT(sSpeeds);
    if (next >= 0 && next < COUNT(sSpeeds))
        CtrSettings_SetInt("battle_speed", sSpeeds[next]);
}

bool CtrSettings_VoxelBlur(void)
{
    return sBlurs[sBlur] > 0;
}

int CtrSettings_VoxelBlurStrength(void)
{
    return sBlurs[sBlur];
}

void CtrSettings_StepVoxelBlur(int direction)
{
    Step(&sBlur, COUNT(sBlurs), direction);
}

/* On is the full blur, unless a strength was chosen already. */
void CtrSettings_SetVoxelBlur(bool on)
{
    if (CtrSettings_VoxelBlur() == on)
        return;
    sBlur = on ? COUNT(sBlurs) - 1 : 0;
    Save();
}

bool CtrSettings_VoxelBattle(void)
{
    return sVoxelBattle;
}

void CtrSettings_SetVoxelBattle(bool on)
{
    if (sVoxelBattle == on)
        return;
    sVoxelBattle = on;
    Save();
}

bool CtrSettings_ShowFps(void)
{
    return sShowFps;
}

void CtrSettings_SetShowFps(bool on)
{
    if (sShowFps == on)
        return;
    sShowFps = on;
    Save();
}

bool CtrSettings_RunAlways(void)
{
    return sRunAlways;
}

void CtrSettings_SetRunAlways(bool on)
{
    if (sRunAlways == on)
        return;
    sRunAlways = on;
    Save();
}
