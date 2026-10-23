/*
 * Audio transport: the mixer's PCM frames to NDSP.
 *
 * The mixer runs on the processor that talks to the DSP, so this file is only
 * a queue: convert one mixer frame
 * to interleaved 16-bit stereo, hand it to NDSP, and keep the queue from
 * running dry or running away.
 *
 * Drift is the one thing that needs care. The mixer produces exactly one frame
 * per VBlank, so its output rate is 701 * (whatever the console's refresh
 * really is), while NDSP consumes at the rate we ask for. Those are never the
 * same number, and a dropped frame makes the gap worse. Rather than let the
 * queue drain and click every few seconds, the playback rate is nudged by
 * fractions of a percent towards the depth we want to keep. That is inaudible
 * and it never needs a resync.
 */

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include "3ds_audio.h"
#include "3ds_platform.h"

/* Eight frames is 133 ms of headroom; CTR_AUDIO_TARGET_DEPTH is where we sit. */
#define CTR_AUDIO_BUFFERS 8
/* 701 today. The rest is headroom in case the game changes its sound mode. */
#define CTR_AUDIO_MAX_FRAMES 1024
/* Per queued buffer away from the target, as a fraction of the sample rate. */
#define CTR_AUDIO_TRIM      0.0025f
#define CTR_AUDIO_TRIM_MAX  0.0100f

static ndspWaveBuf sWave[CTR_AUDIO_BUFFERS];
static int16_t *sSamples;
static unsigned sNext;
static unsigned sPrimed;
static float sRate;
static bool sReady;
static CtrAudioStats sStats;
/* The sound engine's worker, at the end of this file. */
static Thread sWorker;
static LightEvent sKick, sIdle;
static volatile bool sWorkerQuit;
/* The worker on the DSP's clock (CtrAudio_SetClock, below). */
static volatile bool sClock;

CtrAudioStats *CtrAudio_Stats(void) { return &sStats; }

bool CtrAudio_Init(void)
{
    Result rc;

    if (sReady)
        return true;

    /* NDSP needs the DSP firmware dumped to the SD card. Without it there is
     * no way to make a sound, and reporting success would hide that. */
    rc = ndspInit();
    if (R_FAILED(rc))
    {
        CtrLog_Write(CTR_LOG_ERROR,
                     "ndspInit failed (%08lx): no DSP firmware, audio is off",
                     (unsigned long)rc);
        return false;
    }

    sSamples = linearAlloc(CTR_AUDIO_BUFFERS * CTR_AUDIO_MAX_FRAMES * 2 * sizeof(int16_t));
    if (sSamples == NULL)
    {
        CtrLog_Write(CTR_LOG_ERROR, "no linear memory for %u audio frames",
                     CTR_AUDIO_BUFFERS * CTR_AUDIO_MAX_FRAMES);
        ndspExit();
        return false;
    }
    memset(sSamples, 0, CTR_AUDIO_BUFFERS * CTR_AUDIO_MAX_FRAMES * 2 * sizeof(int16_t));

    sRate = (float)CTR_AUDIO_SAMPLE_RATE;
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, sRate);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);

    float mix[12];
    memset(mix, 0, sizeof(mix));
    mix[0] = 1.0f; /* front left  */
    mix[1] = 1.0f; /* front right */
    ndspChnSetMix(0, mix);

    memset(sWave, 0, sizeof(sWave));
    for (unsigned i = 0; i < CTR_AUDIO_BUFFERS; ++i)
        sWave[i].status = NDSP_WBUF_DONE;

    memset(&sStats, 0, sizeof(sStats));
    sStats.rateHz = sRate;
    sNext = 0;
    sPrimed = 0;
    sReady = true;
    CtrLog_Write(CTR_LOG_AUDIO, "NDSP ready: stereo %d Hz, %d frame buffers",
                 CTR_AUDIO_SAMPLE_RATE, CTR_AUDIO_BUFFERS);
    return true;
}

void CtrAudio_Shutdown(void)
{
    if (sWorker != NULL)
    {
        ndspSetCallback(NULL, NULL);
        if (!(sClock && sReady))
            LightEvent_Wait(&sIdle);
        sWorkerQuit = true;
        LightEvent_Signal(&sKick);
        threadJoin(sWorker, U64_MAX);
        threadFree(sWorker);
        sWorker = NULL;
    }
    if (!sReady)
        return;
    sReady = false;
    ndspChnWaveBufClear(0);
    ndspExit();
    if (sSamples != NULL)
    {
        linearFree(sSamples);
        sSamples = NULL;
    }
}

static unsigned QueuedBuffers(void)
{
    unsigned queued = 0;

    for (unsigned i = 0; i < CTR_AUDIO_BUFFERS; ++i)
    {
        if (sWave[i].status == NDSP_WBUF_QUEUED || sWave[i].status == NDSP_WBUF_PLAYING)
            ++queued;
    }
    return queued;
}

/*
 * Pull the playback rate towards the queue depth we want. One buffer of error
 * is a quarter of a percent, which is about four cents of pitch: below what
 * anyone can hear, and enough to absorb both the refresh-rate mismatch and the
 * occasional frame the game spends too long drawing.
 */
static void CorrectDrift(unsigned queued)
{
    float trim = ((float)queued - (float)CTR_AUDIO_TARGET_DEPTH) * CTR_AUDIO_TRIM;
    float rate;

    if (trim > CTR_AUDIO_TRIM_MAX)
        trim = CTR_AUDIO_TRIM_MAX;
    else if (trim < -CTR_AUDIO_TRIM_MAX)
        trim = -CTR_AUDIO_TRIM_MAX;

    rate = (float)CTR_AUDIO_SAMPLE_RATE * (1.0f + trim);
    if (rate != sRate)
    {
        sRate = rate;
        ndspChnSetRate(0, rate);
    }
    sStats.rateHz = rate;
}

void CtrAudio_Queue(const int32_t *interleaved, int frames)
{
    uint64_t start;
    unsigned queued;
    ndspWaveBuf *buf;
    int16_t *out;
    int32_t peak = 0;

    if (!sReady || interleaved == NULL || frames <= 0)
        return;
    if (frames > CTR_AUDIO_MAX_FRAMES)
    {
        static bool reported;
        if (!reported)
        {
            reported = true;
            CtrLog_Write(CTR_LOG_ERROR, "mixer frame of %d samples exceeds the %d the DSP buffers hold",
                         frames, CTR_AUDIO_MAX_FRAMES);
        }
        frames = CTR_AUDIO_MAX_FRAMES;
    }

    start = svcGetSystemTick();
    queued = QueuedBuffers();

    /* An empty queue once the pipeline is primed means the DSP reached the end
     * of the last buffer before this frame arrived: the speakers went silent. */
    if (queued == 0 && sPrimed >= CTR_AUDIO_TARGET_DEPTH)
        ++sStats.underruns;

    buf = &sWave[sNext];
    if (buf->status != NDSP_WBUF_DONE && buf->status != NDSP_WBUF_FREE)
    {
        /* Every buffer is still pending: the game is ahead of the DSP. Drop
         * this frame rather than overwrite audio that is about to play. */
        ++sStats.drops;
        sStats.queued = queued;
        CorrectDrift(queued);
        return;
    }

    out = sSamples + (size_t)sNext * CTR_AUDIO_MAX_FRAMES * 2;
    for (int i = 0; i < frames * 2; ++i)
    {
        /* The mixer's fixed point has 1 << 23 to full scale: 8 bits below
         * the 16 the DSP plays, rounded off. */
        int32_t sample = (interleaved[i] + 128) >> 8;

        if (sample > 32767)
            sample = 32767;
        else if (sample < -32768)
            sample = -32768;
        out[i] = (int16_t)sample;
        if (sample < 0)
            sample = -sample;
        if (sample > peak)
            peak = sample;
    }

#ifdef CTR_AUDIO_DUMP
    {
        static FILE *dump;
        static unsigned dumped;

        if (dumped == 0)
            dump = fopen("sdmc:/3ds/emerald3ds/audio.raw", "wb");
        if (dump && dumped >= 600 && dumped < 2400)
            fwrite(out, sizeof(int16_t), (size_t)frames * 2, dump);
        if (dump && ++dumped == 2400)
            fclose(dump), dump = NULL;
    }
#endif
    DSP_FlushDataCache(out, (size_t)frames * 2 * sizeof(int16_t));

    memset(buf, 0, sizeof(*buf));
    buf->data_vaddr = out;
    buf->nsamples = (u32)frames;
    ndspChnWaveBufAdd(0, buf);

    sNext = (sNext + 1) % CTR_AUDIO_BUFFERS;
    if (sPrimed < CTR_AUDIO_TARGET_DEPTH)
        ++sPrimed;
    ++sStats.frames;
    sStats.peak = (uint32_t)peak;
    sStats.queued = queued + 1;
    CorrectDrift(queued + 1);
    sStats.submitMs = (float)((svcGetSystemTick() - start) * 1000.0 / SYSCLOCK_ARM11);
}

/* ── The sound engine's worker ─────────────────────────────────────────── */

/*
 * m4a mixes every voice in software, in floats, once a frame: on an Old 3DS
 * that is milliseconds of the game thread's frame. On a core of its own it
 * runs beside the next frame's game logic instead. The New 3DS has two cores
 * the system leaves idle; the Old 3DS lends its system core for the share of
 * time asked for here. Each frame waits for the previous mix before handing
 * over the next, so the engine still advances exactly one frame per frame.
 */
#define CTR_AUDIO_SYSCORE_PERCENT 50
/* On the system core the mix only gets that share of the time. If the game
 * thread ends up waiting for it more than this on average, it mixes on its own
 * thread again: never slower than without the worker. */
#define CTR_AUDIO_WAIT_LIMIT_MS 1.0f
#define CTR_AUDIO_WAIT_WINDOW 120
static int sWorkerCore;
static unsigned sKicks;
static uint64_t sWaited;

static RecursiveLock sSoundLock;
static void (*sMix)(void);

/*
 * The sound engine on the DSP's clock (OPTIONS' REAL-TIME PACING,
 * CtrAudio_SetClock): the worker mixes a frame whenever the DSP has fewer
 * than CTR_AUDIO_TARGET_DEPTH queued, woken by NDSP's own frame callback,
 * instead of once per game frame. A frame the game takes too long over then
 * costs it a little speed, not the sound a click: music keeps its tempo,
 * played by the DSP's time and not the display's. Off, the engine advances
 * one frame per shown frame, as on the GBA, and the game thread waits for
 * the mix it asked for (CtrAudio_Kick).
 */
/* With nothing to queue (the engine's DMA off, as before m4aSoundInit) the
 * worker still mixes, but no faster than the game would have. */
#define CTR_AUDIO_CLOCK_IDLE_MS 16.0f

/* NDSP's thread, once per DSP frame (about 5 ms): the queue may have moved. */
/* Set once the engine is back on the game thread (CtrAudio_Kick): the
 * worker must never mix again, or two cores would run the engine at once
 * when the clock is turned back on. */
static volatile bool sGivenUp;

static void DspFrame(void *arg)
{
    (void)arg;
    if (sClock && !sGivenUp)
        LightEvent_Signal(&sKick);
}

static void Worker(void *arg)
{
    uint64_t idleUntil = 0;

    (void)arg;
    for (;;)
    {
        LightEvent_Wait(&sKick);
        if (sWorkerQuit)
            break;
        /* Without the DSP (CtrAudio_Init failed: an emulator without its
         * firmware) there is no clock to follow and nothing to queue; the
         * engine then runs once per shown frame as below, so that what the
         * game asks of it - a cry's end, a fanfare's - still comes. */
        if (sClock && sReady)
        {
            /* The game thread never waits in this mode, but leaves the idle
             * event set so that leaving it finds the worker idle. */
            LightEvent_Signal(&sIdle);
            /* Up to the target depth, one frame per DSP frame at the most
             * each: the next callback brings the rest, with the queue's
             * statuses updated in between. */
            if (QueuedBuffers() < CTR_AUDIO_TARGET_DEPTH && svcGetSystemTick() >= idleUntil)
            {
                uint32_t before = sStats.frames;

                RecursiveLock_Lock(&sSoundLock);
                sMix();
                RecursiveLock_Unlock(&sSoundLock);
                if (sStats.frames == before)
                    idleUntil = svcGetSystemTick() + (uint64_t)(CTR_AUDIO_CLOCK_IDLE_MS * SYSCLOCK_ARM11 / 1000.0f);
            }
            continue;
        }
        RecursiveLock_Lock(&sSoundLock);
        sMix();
        RecursiveLock_Unlock(&sSoundLock);
        LightEvent_Signal(&sIdle);
    }
    LightEvent_Signal(&sIdle);
}

void CtrAudio_SetClock(bool on)
{
    if (on == sClock)
        return;
    sClock = on;
    if (sWorker != NULL)
        CtrLog_Write(CTR_LOG_AUDIO, "sound engine %s", on ? "on the DSP's clock" : "once per shown frame");
}

bool CtrAudio_StartWorker(void (*mix)(void))
{
    s32 priority = 0x30;
    int core = 2;
    bool newModel = false;

    if (sWorker != NULL)
        return true;
    sMix = mix;
    RecursiveLock_Init(&sSoundLock);
    LightEvent_Init(&sKick, RESET_ONESHOT);
    LightEvent_Init(&sIdle, RESET_STICKY);
    LightEvent_Signal(&sIdle);
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    /* Above the game thread: it only ever waits on the mix it asked for. */
    priority = priority > 0x18 ? priority - 1 : priority;
    /* Core 2 is a New 3DS's alone. An Old 3DS refuses it, but Azahar
     * emulating one takes it and crashes, so only a New 3DS asks. */
    if (R_SUCCEEDED(APT_CheckNew3DS(&newModel)) && newModel)
        sWorker = threadCreate(Worker, NULL, 32 * 1024, priority, core, false);
    if (sWorker == NULL && R_SUCCEEDED(APT_SetAppCpuTimeLimit(CTR_AUDIO_SYSCORE_PERCENT)))
    {
        core = 1;
        sWorker = threadCreate(Worker, NULL, 32 * 1024, priority, core, false);
    }
    if (sWorker == NULL)
    {
        CtrLog_Write(CTR_LOG_AUDIO, "sound engine mixes on the game thread (no core to spare)");
        return false;
    }
    sWorkerCore = core;
    ndspSetCallback(DspFrame, NULL);
    CtrLog_Write(CTR_LOG_AUDIO, "sound engine mixes on core %d%s", core, sClock ? ", on the DSP's clock" : "");
    return true;
}

bool CtrAudio_Kick(void)
{
    if (sGivenUp)
        return false;
    if (sClock && sReady)
    {
        /* The DSP's callback wakes the worker; the game thread never waits. */
        LightEvent_Signal(&sKick);
        return true;
    }
    uint64_t start = svcGetSystemTick();

    LightEvent_Wait(&sIdle);
    sWaited += svcGetSystemTick() - start;
    if (++sKicks == CTR_AUDIO_WAIT_WINDOW)
    {
        float waitMs = (float)(sWaited * 1000.0 / SYSCLOCK_ARM11 / sKicks);

        sKicks = 0;
        sWaited = 0;
        if (sWorkerCore == 1 && waitMs > CTR_AUDIO_WAIT_LIMIT_MS)
        {
            /* The worker is idle and stays so: it is never kicked again. */
            sGivenUp = true;
            CtrLog_Write(CTR_LOG_AUDIO, "sound engine back on the game thread: it waited %.2f ms "
                         "a frame for the system core", waitMs);
            return false;
        }
    }
    LightEvent_Clear(&sIdle);
    LightEvent_Signal(&sKick);
    return true;
}

void CtrAudio_LockSound(void)
{
    if (sWorker != NULL)
        RecursiveLock_Lock(&sSoundLock);
}

void CtrAudio_UnlockSound(void)
{
    if (sWorker != NULL)
        RecursiveLock_Unlock(&sSoundLock);
}
