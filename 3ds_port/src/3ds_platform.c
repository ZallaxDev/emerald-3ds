#include <3ds.h>
#include <stdlib.h>
#include <malloc.h>
#include <unistd.h>
#include <string.h>
#include "3ds_platform.h"
#include "3ds_daynight.h"
#include "3ds_data.h"
#include "3ds_locale.h"
#include "3ds_audio.h"
#include "3ds_video.h"
#include "3ds_assets.h"
#include "3ds_local.h"
#include "3ds_bottom.h"
#include "voxel/ctr_voxel.h"

static CtrPlatformHooks sHooks;
static aptHookCookie sAptHook;
static bool sInitialized, sExit, sWaiting;
static uint64_t sFrames, sStart, sLastFrame, sFrameMs;
static uint64_t sWorkStart, sLastTick, sExitStart;
static CtrTiming sTiming;

static void Lifecycle(APT_HookType type, void *parameter)
{
    (void)parameter;
    if (type == APTHOOK_ONSLEEP)
    {
        CtrLog_Write(CTR_LOG_BOOT, "suspend/sleep");
        /* The lid closed in a link: it ends for everyone. The game finds the
         * link lost when it wakes, as the others already have. */
        LocalLink_LeaveBeforeSleep();
    }
    else if (type == APTHOOK_ONSUSPEND)
        CtrLog_Write(CTR_LOG_BOOT, "suspend/sleep");
    else if (type == APTHOOK_ONRESTORE || type == APTHOOK_ONWAKEUP)
    {
        if (type == APTHOOK_ONWAKEUP)
            LocalLink_AfterWake();
        CtrDayNight_Resume();
        CtrInput_Clear();
        CtrBottom_OnResume();
        sLastFrame = CtrPlatform_Milliseconds();
        sLastTick = sExitStart = 0;
        CtrLog_Write(CTR_LOG_BOOT, "resume");
    }
    else if (type == APTHOOK_ONEXIT)
        sExit = true;
}

uint64_t CtrPlatform_Milliseconds(void)
{
    return svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000);
}

int64_t CtrPlatform_WallSeconds(void)
{
    return (int64_t)time(NULL);
}

uint64_t CtrPlatform_Ticks(void)
{
    return svcGetSystemTick();
}

float CtrPlatform_TickMs(uint64_t ticks)
{
    return (float)(ticks * 1000.0 / SYSCLOCK_ARM11);
}

uint64_t CtrPlatform_FrameCount(void)
{
    return sFrames;
}

const CtrTiming *CtrPlatform_GetTiming(void) { return &sTiming; }
void CtrPlatform_NoteBottom(float ms) { sTiming.bottomMs = ms; }

bool CtrPlatform_Init(void)
{
    /* The bottom screen at RGB565 from the start: its frame buffers live in
     * the linear heap, and gfxInitDefault made them BGR8 (450 KiB) before
     * CtrLog_Init turned the screen to 565, which never shrinks them. The
     * top screen stays BGR8: at 565 the top screen came out garbled on a
     * New 3DS from the first frame (Azahar showed it fine). */
    gfxInit(GSP_BGR8_OES, GSP_RGB565_OES, false);
    /* A New 3DS runs the application core at 804 MHz with its L2 cache when
     * asked; an Old 3DS ignores this. */
    osSetSpeedupEnable(true);
    CtrLog_Init();
    sInitialized = true;
    atexit(CtrPlatform_Shutdown);
    aptHook(&sAptHook, Lifecycle, NULL);
    CtrInput_Clear();
    sFrames = 0;
    sStart = sLastFrame = CtrPlatform_Milliseconds();
    CtrLog_Write(CTR_LOG_VIDEO, "diagnostic console; game renderer will use GPU");
    CtrLog_Write(CTR_LOG_INPUT, "HID: held/down/up, touch, raw circle pad");
    if (!CtrFs_Init())
    {
        CtrLog_Write(CTR_LOG_ERROR, "platform init: filesystem failed");
        return false;
    }
    CtrSettings_Load();
    CtrScenario_Init();
    CtrProf_LogIdentity();
    if (!CtrData_Init())
        CtrPlatform_ShowDataError(CtrData_ErrorTitle(), CtrData_ErrorDetail());
    if (!CtrVideo_Init())
    {
        CtrLog_Write(CTR_LOG_ERROR, "platform init: GPU video failed");
        return false;
    }
    /* A console without dumped DSP firmware still plays; it just plays silent.
     * CtrAudio_Init says so in the log and the game is left running. */
    CtrAudio_Init();
    /* The radio of the emulated Wireless Adapter; idle until a link counter
     * starts a session. */
    LocalLink_Init();
    return true;
}

void CtrPlatform_SetHooks(const CtrPlatformHooks *hooks)
{
    if (hooks)
        sHooks = *hooks;
    else
        memset(&sHooks, 0, sizeof(sHooks));
}

/* Free memory in the two pools a 3DSX actually competes for: the application
 * heap newlib allocates from, and the linear heap the GPU can address.
 * mallinfo's free is only what the arena holds now; the heap still grows by
 * sbrk up to fake_heap_end (__system_allocateHeaps), so the room it really
 * has left is both. */
extern char *fake_heap_end;

void CtrPlatform_ReportMemory(const char *stage)
{
    struct mallinfo info = mallinfo();
    char *top = sbrk(0);
    size_t grow = top != (char *)-1 && top < fake_heap_end ? (size_t)(fake_heap_end - top) : 0;

    CtrLog_Write(CTR_LOG_BOOT, "memory %s: heap used=%u free=%u, linear free=%lu, VRAM free=%lu, "
                 "heap room=%lu KiB (%lu KiB past the arena)",
                 stage, (unsigned)info.uordblks, (unsigned)info.fordblks,
                 (unsigned long)linearSpaceFree(), (unsigned long)vramSpaceFree(),
                 (unsigned long)((info.fordblks + grow) >> 10), (unsigned long)(grow >> 10));
}

_Static_assert(sizeof(CtrLock) == sizeof(LightLock), "CtrLock is a LightLock");

void CtrLock_Init(CtrLock *lock) { LightLock_Init((LightLock *)lock); }
void CtrLock_Lock(CtrLock *lock) { LightLock_Lock((LightLock *)lock); }
void CtrLock_Unlock(CtrLock *lock) { LightLock_Unlock((LightLock *)lock); }

bool CtrPlatform_StartThread(void (*entry)(void *), void *arg, unsigned stack, int core)
{
    s32 priority = 0x30;

    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    priority = priority < 0x3F ? priority + 1 : 0x3F;
    /* The system core needs a share of it for the application first; one
     * already asked for (the sound engine's, the voxel streams') is kept. */
    if (core == 1)
    {
        u32 limit = 0;

        if ((R_SUCCEEDED(APT_GetAppCpuTimeLimit(&limit)) && limit >= 30)
            || R_SUCCEEDED(APT_SetAppCpuTimeLimit(30)))
        {
            if (threadCreate(entry, arg, stack, priority, 1, true) != NULL)
                return true;
        }
        core = -2;
    }
    return threadCreate(entry, arg, stack, priority, core, true) != NULL;
}

void CtrPlatform_SleepUs(unsigned microseconds)
{
    svcSleepThread((s64)microseconds * 1000);
}

void CtrPlatform_RequestExit(void) { sExit = true; }

bool CtrPlatform_BeginFrame(void)
{
    if (sExit || !aptMainLoop())
        return false;
    sWorkStart = svcGetSystemTick();
    CtrInput_Scan();
    const CtrInput *input = CtrInput_Get();
    /* Leave START and SELECT individually available to ReadKeys. Only the
     * deliberate one-second chord exits the game; with A and B as well it is
     * the game's soft reset, which may be held longer. */
    if ((input->physicalHeld & (CTR_KEY_START | CTR_KEY_SELECT | CTR_KEY_A | CTR_KEY_B))
        == (CTR_KEY_START | CTR_KEY_SELECT))
    {
        if (!sExitStart) sExitStart = CtrPlatform_Milliseconds();
        if (CtrPlatform_Milliseconds() - sExitStart >= 1000)
        {
            CtrLog_Write(CTR_LOG_INPUT, "START+SELECT: exit");
            return false;
        }
    }
    else sExitStart = 0;
    return true;
}

/*
 * Fast-forward (CtrSettings_SpeedHalves). Each game frame earns two halves;
 * a frame is presented and waits for the display once `speed` halves have
 * been earned, the others run back to back: at 2x one frame in two is shown,
 * at 1.5x two in three. Only
 * a presented frame ticks the sound engine (CtrPlatform_SoundTick), so music
 * and effects keep their speed while the game runs faster; a frame hidden by
 * real-time pacing (below) ticks it too, as it stands for real time. A frame is also
 * presented as soon as one more hidden frame and the present would no longer
 * fit in the display's frame: the game then runs as fast as the console
 * allows (an Old 3DS less than a New 3DS at 804 MHz) and the picture and the
 * sound stay at 60 Hz.
 */
#define FRAME_MS (1000.0f / 60)
static bool sSoundTick = true;
static unsigned sEarned;
/* Real-time pacing's hidden frame (PaceBehind): never two in a row. */
static unsigned sHidden;
static uint64_t sShownTick;
static float sPresentMs = 4.0f;

/*
 * Real-time pacing (OPTIONS' REAL-TIME PACING, key audio_clock), with the
 * sound engine on the DSP's clock (3ds_audio.c). A frame the console takes
 * longer than the display's over - an Old 3DS drawing a town, a New 3DS in
 * 3D with a town streaming in - used to slow the game by that much. Paced,
 * the game keeps the display's time instead: when it has fallen a whole
 * frame behind the clock, the next frame runs hidden, as fast-forward's do,
 * and the picture skips a step while the game and the music do not. Never
 * two hidden frames in a row, so the picture keeps at least half the rate;
 * and a long gap - the HOME menu, sleep, a map load - is let go rather than
 * caught up, which would be a burst of hidden frames.
 */
#define PACE_KEY "audio_clock"
#define PACE_HZ 59.8261f           /* the LCD's refresh */
#define PACE_LET_GO_FRAMES 4.0f
static bool sPace;
/* A battle's party menu, summary or bag up (the top screen held over the
 * battle, which waits under it): hidden frames there too (PaceUpdate). */
static bool sPaceMenus;
static uint64_t sPaceStart;
static float sPaceFrames;
/* This frame went through pacing, not fast-forward (FrameShown): a frame it
 * hides still ticks the sound engine, which otherwise (no worker on the DSP's
 * clock) would slow by exactly the frames pacing hides. */
static bool sPacedFrame;

/* Whether the game is a frame behind the clock, counting this frame as run. */
static bool PaceBehind(uint64_t now)
{
    float elapsed;

    if (sPaceStart == 0)
    {
        sPaceStart = now;
        sPaceFrames = 0.0f;
    }
    sPaceFrames += 1.0f;
    elapsed = (float)((now - sPaceStart) * (double)PACE_HZ / SYSCLOCK_ARM11);
    if (elapsed - sPaceFrames > PACE_LET_GO_FRAMES || elapsed < sPaceFrames - 1.0f)
    {
        /* Too far behind to catch up, or ahead (a timer change): start over. */
        sPaceStart = now;
        sPaceFrames = 0.0f;
        return false;
    }
    return elapsed - sPaceFrames >= 1.0f;
}

/* The setting, read once a frame; the sound engine follows it. The field
 * only (and a battle's menus, below): the voxel world is what costs the
 * frame there, and a battle's own timing (its intro's slides, its cries) is
 * best left as the GBA's. */
static void PaceUpdate(void)
{
    bool setting = CtrSettings_GetInt(PACE_KEY, 1) != 0;
    bool on = setting && !CtrVideo_InBattle();
    /* Nothing of the battle's own runs under its menus, only theirs: there
     * the game keeps the display's time too, a slow redraw of the bottom
     * screen's view costing the picture a step and not the game. The sound
     * engine stays on the game's frames, as in the rest of the battle: a
     * hidden frame ticks it (sPacedFrame), so the music keeps its tempo. */
    bool menus = setting && CtrVideo_InBattle() && CtrVideo_TopHeld();

    if (on != sPace)
    {
        sPace = on;
        sPaceStart = 0;
        CtrAudio_SetClock(on);
    }
    if (menus != sPaceMenus)
    {
        sPaceMenus = menus;
        sPaceStart = 0;
    }
}

/*
 * The battle scene's speed (CtrSettings_BattleSpeedHalves): while the battle
 * itself is on screen, its own speed paces the frames; the bag, the party and
 * the summary a battle opens, and the field, keep the overworld's SPEED. The
 * game reports the scene once a frame (CtrGame_WaitFrame). A link session
 * plays at 1x for everyone and a measured scenario is recorded at 1x, so
 * neither takes the battle's keys or speed.
 */
#define BADGE_MS 2000
static bool sBattleScene;
static uint64_t sBadgeUntil;

void CtrPlatform_SetBattleScene(bool on)
{
    sBattleScene = on;
}

bool CtrPlatform_BattleSpeedKeys(void)
{
    return sBattleScene && !LocalLink_InSession() && !CtrScenario_Active();
}

void CtrPlatform_StepBattleSpeed(int direction, bool wrap)
{
    CtrSettings_StepBattleSpeed(direction, wrap);
    /* Shown even at an end of the range, so the press is answered. */
    sBadgeUntil = CtrPlatform_Milliseconds() + BADGE_MS;
}

int CtrPlatform_BattleSpeedBadge(void)
{
    int halves;

    if (!CtrPlatform_BattleSpeedKeys())
        return 0;
    halves = CtrSettings_BattleSpeedHalves();
    return CtrPlatform_Milliseconds() < sBadgeUntil ? halves : 0;
}

static bool FrameShown(uint64_t now)
{
    /* In a local wireless session everyone plays at the same speed: the link
     * code times out on a partner that runs slower. */
    unsigned speed = LocalLink_InSession() ? 2
                   : CtrPlatform_BattleSpeedKeys() ? CtrSettings_BattleSpeedHalves()
                   : CtrSettings_SpeedHalves();
    float since = sShownTick ? (now - sShownTick) * 1000.0f / SYSCLOCK_ARM11 : FRAME_MS;

    sPacedFrame = (sPace || sPaceMenus) && speed <= 2;
    if (sPacedFrame)
    {
        bool behind = PaceBehind(now);

        if (behind && sHidden == 0)
        {
            sHidden = 1;
            return false;
        }
        sHidden = 0;
        return true;
    }
    sEarned += 2;
    if (speed <= 2 || since + sTiming.gameMs + sPresentMs > FRAME_MS)
    {
        sEarned = 0;
        return true;
    }
    if (sEarned >= speed)
    {
        sEarned -= speed;
        return true;
    }
    return false;
}

bool CtrPlatform_SoundTick(void)
{
    return sSoundTick;
}

/*
 * No HOME Menu in a local wireless session, as the GBA link games' own 3DS
 * releases have it: the link would go on without this system. A press is
 * refused, and a sign shows that it was (CtrVideo_ShowNoHome).
 */
static void UpdateHomeAllowed(void)
{
    bool allowed = !LocalLink_InSession();

    if (allowed != aptIsHomeAllowed())
    {
        aptSetHomeAllowed(allowed);
        CtrLog_Write(CTR_LOG_BOOT, "HOME button %s", allowed ? "allowed" : "blocked (local wireless)");
    }
    if (!allowed && aptCheckHomePressRejected())
        CtrVideo_ShowNoHome();
}

void CtrPlatform_EndFrame(void)
{
    if (sWaiting)
        return;
    sWaiting = true;
    UpdateHomeAllowed();
    uint64_t endStart = svcGetSystemTick();
    /* This frame's game work, for FrameShown (set again below). */
    sTiming.gameMs = (endStart - sWorkStart) * 1000.0f / SYSCLOCK_ARM11;
    PaceUpdate();
    bool shown = FrameShown(endStart);
    sSoundTick = shown || sPacedFrame;
    if (sHooks.audioFrame)
        sHooks.audioFrame();
    /* Latch the logical frame BEFORE GPU reads its registers/OAM/palette.
     * C3D owns display pacing and swapping; no second VBlank wait. */
    if (sHooks.vblank)
        sHooks.vblank();
    uint64_t presentStart = svcGetSystemTick();
    /* Before the present, which reads them: its DROP line is about the frame
     * that just ran, and the voxel builds after FrameEnd estimate the next
     * (gameMs is already this frame's, from above). */
    sTiming.vblankMs = (presentStart - endStart) * 1000.0f / SYSCLOCK_ARM11;
    /* FrameEnd(0) also flushes the console's linear LCD buffer. Calling
     * gfxFlushBuffers here would flush BOTH screens again, not just bottom. */
    if (sHooks.videoPresent && shown)
    {
        sHooks.videoPresent();
        /* The present's own work, without its wait for the display. */
        sPresentMs = (svcGetSystemTick() - presentStart) * 1000.0f / SYSCLOCK_ARM11
                     - CtrVideo_GetStats()->waitMs;
        if (sPresentMs < 0) sPresentMs = 0;
        sShownTick = svcGetSystemTick();
    }
    ++sFrames;
    uint64_t now = CtrPlatform_Milliseconds();
    sFrameMs = now - sLastFrame;
    sLastFrame = now;
    uint64_t tick = svcGetSystemTick();
    sTiming.frameMs = sLastTick ? (tick - sLastTick) * 1000.0 / SYSCLOCK_ARM11 : 0;
    sLastTick = tick;
    sTiming.workMs = (tick - sWorkStart) * 1000.0 / SYSCLOCK_ARM11;
    sTiming.workMs -= CtrVideo_GetStats()->waitMs;
    if (sTiming.workMs < 0) sTiming.workMs = 0;
    if (sTiming.workMs > sTiming.peakWorkMs) sTiming.peakWorkMs = sTiming.workMs;
    if (sTiming.workMs > 1000.0f / 60) ++sTiming.slowFrames;
    CtrProf_EndFrame(sTiming.workMs, CtrVideo_GetStats()->waitMs, sFrames);
    sWaiting = false;
}

void CtrPlatform_Shutdown(void)
{
    if (!sInitialized)
        return;
    sInitialized = false;
    /* The last segment and what the GPU's memory holds, before it is freed. */
    CtrProf_Close();
    CtrAssets_StopWarmup();
    CtrSettings_Shutdown();
    if (sHooks.shutdown)
        sHooks.shutdown();
    LocalLink_Shutdown();
    CtrVideo_Shutdown();
    CtrAudio_Shutdown();
    CtrLog_Write(CTR_LOG_BOOT, "clean shutdown, platform frames=%llu", (unsigned long long)sFrames);
    CtrScenario_Close();
    aptUnhook(&sAptHook);
    CtrFs_Shutdown();
    CtrLog_Close();
    gfxExit();
}

/* Before the GPU compositor exists: a plain text screen on the top LCD that
 * tells the player what to do, until START. */
void CtrPlatform_ShowDataError(const char *title, const char *detail)
{
    PrintConsole console;

    consoleInit(GFX_TOP, &console);
    consoleSelect(&console);
    consoleClear();
    printf("\x1b[2;2HEmerald 3DS\n\n\x1b[31m %s\x1b[0m\n\n", title);
    for (const char *line = detail; line && *line;)
    {
        const char *end = strchr(line, '\n');
        int length = end ? (int)(end - line) : (int)strlen(line);
        printf(" %.*s\n", length, line);
        line = end ? end + 1 : NULL;
    }
    printf("\n\n %s", CTR_TEXT("Press START to exit.",
                                "Press START to exit.",
                                "Appuie sur START pour quitter.",
                                "Premi START per uscire.",
                                "Press START to exit.",
                                "Press START to exit.",
                                "Press START to exit."));
    while (aptMainLoop())
    {
        hidScanInput();
        if (hidKeysDown() & KEY_START)
            break;
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    exit(EXIT_FAILURE);
}

void CtrPlatform_Fatal(const char *reason)
{
    CtrLog_Write(CTR_LOG_ERROR, "%s", reason);
    CtrLog_ShowFatal(reason);
    /* Keep the error visible and lifecycle responsive, without claiming PASS. */
    while (aptMainLoop())
    {
        hidScanInput();
        if (hidKeysDown() & KEY_START)
            break;
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    exit(EXIT_FAILURE);
}

/* Marks along a stretch of game code (see port_platform.h): what each step of
 * a map load costs on the console, which an emulator cannot say. */
void Port_ProfileMark(const char *label)
{
    static uint64_t sLast;
    static unsigned sLogged;
    uint64_t now = svcGetSystemTick();
    float ms = sLast ? (float)((now - sLast) * 1000.0 / SYSCLOCK_ARM11) : 0.0f;

    sLast = now;
    if (ms >= 0.8f && ms < 200.0f && sLogged < 400)
    {
        ++sLogged;
        CtrLog_Write(CTR_LOG_VIDEO, "PROF %s %.1f ms", label, ms);
    }
}

/* Buckets along a path that runs every frame (the game's VBlank handler):
 * Port_ProfBegin starts the clock, Port_ProfAcc(name) charges the time since
 * the previous call to `name`; averaged and logged every 600 frames. */
#define PROF_BUCKETS 12
static struct { const char *name; uint64_t ticks; } sProf[PROF_BUCKETS];
static uint64_t sProfLast;
static unsigned sProfFrames;

void Port_ProfBegin(void)
{
    if (sProfFrames >= 600)
    {
        char text[256] = "";   /* no bucket named: logged empty, not garbage */
        size_t used = 0;

        for (unsigned i = 0; i < PROF_BUCKETS && sProf[i].name != NULL && used < sizeof(text) - 32; ++i)
        {
            used += (size_t)snprintf(text + used, sizeof(text) - used, " %s=%.2f", sProf[i].name,
                                     sProf[i].ticks * 1000.0 / SYSCLOCK_ARM11 / sProfFrames);
            sProf[i].ticks = 0;
        }
        CtrLog_Write(CTR_LOG_VIDEO, "PROF vblank handler (ms/frame):%s", text);
        sProfFrames = 0;
    }
    ++sProfFrames;
    sProfLast = svcGetSystemTick();
}

void Port_ProfAcc(const char *name)
{
    uint64_t now = svcGetSystemTick();

    for (unsigned i = 0; i < PROF_BUCKETS; ++i)
    {
        if (sProf[i].name == name || sProf[i].name == NULL)
        {
            sProf[i].name = name;
            sProf[i].ticks += now - sProfLast;
            break;
        }
    }
    sProfLast = now;
}
