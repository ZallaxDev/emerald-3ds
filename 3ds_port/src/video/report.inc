/*
 * Part of 3ds_video.c. The frame report (CtrVideoFrameReport): each
 * submission's times and the queue's drains.
 */

/* The frame report (CtrVideoFrameReport): this game frame's, and the ids of
 * the submissions. sPerfBoundary: a held top screen or a resume since the
 * last drawn frame, whose interval to the next one is then no frame's. */
static CtrVideoFrameReport sReport;
static uint32_t sSubmitId;
static float sSubmitGpuStartMs;
static uint64_t sSubmitStart;
static bool sPerfBoundary;
/*
 * The same for the pictures' time on screen (ReportGpu): a held top screen or
 * a resume since the last submission (sShownCrossed, set wherever
 * sPerfBoundary is), latched by each submission (sSubmitCrossed). A
 * submission that crossed one is not linked to the one before it: the
 * refreshes between their swaps span the hold, not a frame - up to 254 of them.
 */
static bool sShownCrossed, sSubmitCrossed;

/*
 * Chained in front of citro3d's own callback of the frame queue (set with the
 * queue in C3D_Init): the tick and the VBlank count of each drain in which
 * citro3d swapped the top screen - a submission's last drain, shown at the
 * next VBlank. C3D_GetDrawingTime runs from FrameEnd only, so with the GPU
 * started early (GpuStartEarly) it is not when the GPU finished. Runs on the
 * GSP event thread.
 *
 * The swap is citro3d's own (onQueueFinish calls gfxScreenSwapBuffers for the
 * top screen once the drain after a FrameEnd that drew it ends), seen as the
 * frame buffer the next picture goes to changing across its callback: the
 * top screen is double-buffered (gfxInit's default; the port turns it off
 * for the bottom screen alone, 3ds_log.c), and the swap flips which buffer
 * gfxGetFramebuffer names. That is a plain read of libctru's state, no lock
 * nor service call, as safe on this thread as on any; and nothing else swaps
 * the top screen while the game runs (the main thread cannot run between the
 * two reads: this thread is above it, on its core). Not gspIsPresentPending:
 * it stays true until the VBlank after a swap, and with the GPU started early
 * the queue drains several times a frame - a drain before that VBlank was
 * counted as another swap, and after a late frame the next pictures' time on
 * screen came out unknown.
 */
static void (*sCitroQueueDone)(gxCmdQueue_s *queue);
static volatile uint64_t sSwapTick;
static volatile uint32_t sSwapVBlank, sSwaps;

/*
 * Every drain of the queue, swap or not, with how far the frame had queued
 * (sGpuStage), for perf.bin v6: where the GPU caught up with the CPU, and so
 * what of a late frame's queue was still to run. This thread only appends and
 * ReportGpu only takes, once a submission. With the ring full, whatever a frame
 * without a report left, the newest takes the last place and the drain lost is
 * counted: the last drain is the one worth having. Only with the profiler on.
 */
#define DRAIN_RING 64u
static struct { uint32_t tick; uint8_t stage; } sDrainRing[DRAIN_RING];
static volatile uint32_t sDrainHead, sDrainTail, sDrainsLost;

static void DrainNote(uint32_t tick, unsigned stage)
{
    uint32_t head = sDrainHead;
    bool full = head - sDrainTail >= DRAIN_RING;
    unsigned at = (full ? head - 1 : head) % DRAIN_RING;

    sDrainRing[at].tick = tick;
    sDrainRing[at].stage = (uint8_t)stage;
    __sync_synchronize();
    if (full) ++sDrainsLost;
    else sDrainHead = head + 1;
}

static void QueueDone(gxCmdQueue_s *queue)
{
    const u8 *before = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    bool swapped;
    uint32_t tick;

    sCitroQueueDone(queue);
    swapped = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL) != before;
    if (swapped)
    {
        sSwapTick = svcGetSystemTick();
        sSwapVBlank = C3D_FrameCounter(0);
        __sync_synchronize();
        sSwaps++;
    }
    tick = Port_ProfTick();
    if (tick != 0) DrainNote(tick, sGpuStage | (swapped ? CTR_STAGE_SWAP : 0));
}

static void ChainQueueDone(void)
{
    if (sFrameQueue != NULL && sFrameQueue->callback != NULL && sFrameQueue->callback != QueueDone)
    {
        sCitroQueueDone = sFrameQueue->callback;
        sFrameQueue->callback = QueueDone;
    }
}

void CtrVideo_TakeFrameReport(CtrVideoFrameReport *out)
{
    *out = sReport;
    memset(&sReport, 0, sizeof(sReport));
}

void CtrVideo_NoteResume(void)
{
    sPerfBoundary = true;
    sShownCrossed = true;
}

/* The drains of the submission FrameBegin has just waited for: all there are
 * since the last report, its queue being empty now. Past the list's room the
 * last one goes on replacing the last place. */
static void ReportDrains(void)
{
    static uint32_t sLostSeen;
    uint32_t head = sDrainHead, tail = sDrainTail, count = head - tail, lost = sDrainsLost;

    __sync_synchronize();
    for (uint32_t i = 0; i < count; ++i)
    {
        unsigned at = i < CTR_DRAIN_LIST ? i : CTR_DRAIN_LIST - 1;

        sReport.drainTicks[at] = sDrainRing[(tail + i) % DRAIN_RING].tick - (uint32_t)sSubmitStart;
        sReport.drainStage[at] = sDrainRing[(tail + i) % DRAIN_RING].stage;
    }
    __sync_synchronize();
    sDrainTail = head;
    sReport.drains = count > 255 ? 255 : (uint8_t)count;
    sReport.drainsLost = lost - sLostSeen > 255 ? 255 : (uint8_t)(lost - sLostSeen);
    sLostSeen = lost;
}

/* Right after a FrameBegin that waited for the GPU: whose drawing time it is,
 * and when that submission's swap was asked for and shown (QueueDone). */
static void ReportGpu(void)
{
    static uint32_t sSwapsSeen, sShownOf, sShownAt;
    uint32_t swaps = sSwaps;

    sReport.gpuOf = sSubmitId;
    sReport.gpuMs = sSubmitId ? C3D_GetDrawingTime() : 0.0f;
    sReport.gpuStartMs = sSubmitGpuStartMs;
    __sync_synchronize();
    if (sSubmitId != 0 && swaps != sSwapsSeen)
    {
        /* The screen takes a swap at the VBlank after it is asked for. */
        uint32_t shownAt = sSwapVBlank + 1;

        sReport.gpuDoneMs = (sSwapTick - sSubmitStart) * 1000.0f / SYSCLOCK_ARM11;
        /* Two submissions in a row, each with its one swap seen, and no
         * held top screen or resume between them (sSubmitCrossed). */
        if (sShownOf != 0 && sShownOf + 1 == sSubmitId && swaps == sSwapsSeen + 1 && !sSubmitCrossed)
        {
            uint32_t lost = shownAt - sShownAt - 1;

            sReport.shownLost = lost > 253 ? 253 : (uint8_t)lost;
            sReport.shownKnown = shownAt > sShownAt;
            /* Its swap and the one before in one VBlank: not a time on screen
             * that is unknown, but a picture that never had any. */
            sReport.shownTwice = shownAt == sShownAt;
        }
        sShownOf = sSubmitId;
        sShownAt = shownAt;
    }
    else
        sShownOf = 0;
    sSwapsSeen = swaps;
    if (sSubmitId != 0) ReportDrains();
}

/* Right after its FrameEnd: this frame's submission, and when it set the GPU
 * going (from the frame's start at `start`). */
static uint32_t ReportSubmit(float gpuStartMs, uint64_t start)
{
    sSubmitGpuStartMs = gpuStartMs;
    sSubmitStart = start;
    sSubmitCrossed = sShownCrossed;
    sShownCrossed = false;
    return ++sSubmitId;
}
