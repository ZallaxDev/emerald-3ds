/*
 * Part of 3ds_video.c. The GX queue and the wraps of GX, GSP and Citro3D
 * calls: flushes, draw and copy counters, the GPU started early within a
 * frame, clears and texture cache drops. Offers the voxel uploads' room
 * (CtrVideo_TryVoxelUpload), ClearWhenMade and TexCacheDrop.
 */

/* A battle is on, transition and all (CtrVideo_InBattle). */
static bool sInBattle;

/* Public queue passed by libctru, never a cast of Citro3D's private context.
 * Uploads reserve worst-case split+copy entries and leave the compositor's
 * clears, postprocessing, battle layers and final transfers room to finish. */
static gxCmdQueue_s *sFrameQueue;
/* Each top screen swap's tick and VBlank, for the frame report. */
static void ChainQueueDone(void);
static unsigned sUploadCommands, sRenderReserve;
/* The room a frame's own drawing is kept (sRenderReserve); a battle and the
 * voxel world per eye take the most. */
#define RENDER_RESERVE 24u
#define RENDER_RESERVE_MOST 28u
/*
 * Citro3D makes its queue 32 entries long (C3D_Init) and binds it once, empty
 * (C3Di_RenderQueueInit), here: it is lengthened then, and C3D_Fini frees the
 * longer one. Everything the frame queued before keeps the 32 it had
 * (GX_QUEUE_SHARED); the rest is the animated tiles' alone
 * (CtrVideo_TryVoxelAnimUpload), whose copies of a pass go out in one frame.
 */
#define GX_QUEUE_SHARED 32u
#define GX_QUEUE_ENTRIES 128u
static unsigned sAnimCommands;
void __real_GX_BindQueue(gxCmdQueue_s *queue);
void __wrap_GX_BindQueue(gxCmdQueue_s *queue)
{
    if (queue != NULL && queue->maxEntries < GX_QUEUE_ENTRIES && queue->numEntries == 0)
    {
        gxCmdEntry_s *entries = realloc(queue->entries, GX_QUEUE_ENTRIES * sizeof(*entries));

        if (entries != NULL)
        {
            queue->entries = entries;
            queue->maxEntries = GX_QUEUE_ENTRIES;
        }
    }
    sFrameQueue = queue;
    __real_GX_BindQueue(queue);
}

/* The queue as the shared uploads and the frame's drawing count it. */
static unsigned SharedCapacity(void)
{
    return sFrameQueue->maxEntries < GX_QUEUE_SHARED ? sFrameQueue->maxEntries : GX_QUEUE_SHARED;
}

static unsigned SharedUsed(void)
{
    unsigned used = sFrameQueue->numEntries;

    return used > sAnimCommands ? used - sAnimCommands : 0;
}

/*
 * The GPU set to work before C3D_FrameEnd (GpuStartEarly). Citro3D runs its
 * GX queue only at FrameEnd, so the GPU waits for the whole composition and
 * the frame takes CPU + GPU: 8 + 10-15 ms in a battle with the 3D slider up
 * on an Old 3DS, a dropped frame each time. Run from a split in the frame,
 * the queue takes each later command as it is added, while the CPU records
 * the rest.
 *
 * Two things then change. What the GPU reads must be in memory when it is
 * queued, not at FrameEnd, where Citro3D flushes the linear heap: every
 * command list, copy and transfer queued meanwhile, whoever queues it -
 * Citro2D's clears, Citro3D's own splits - is preceded by a flush (the GX
 * wrappers below; the link wraps them, Makefile). And FrameEnd must not find
 * the queue running: it queues the display transfers before it sets the
 * flags its completion callback swaps the screens by, so a queue that caught
 * up in between would lose a swap. So before FrameEnd the queue is waited on
 * and stopped while idle (stopping a running one loses the count of the
 * commands still in flight, and the next wait never returns), and FrameEnd
 * starts it again as it always does. An earlier attempt (removed in
 * 63b91299e) kicked the queue without either guard.
 */
extern u32 __ctru_linear_heap, __ctru_linear_heap_size;
static bool sGpuEarly;
static float sCmdBufPeak;
static uint64_t sGpuEarlyStart;

static uint64_t sFlushTicks;
static unsigned sFlushes;

/*
 * Every data cache flush - ours, Citro3D's at FrameEnd, the voxel uploads' -
 * goes through GSPGPU_FlushDataCache, a request to the GSP service, which
 * runs on the system core and makes the same kernel call for this process.
 * The request is a round trip to the other core per flush (0.6 ms each for
 * the whole heap on an Old 3DS, three a frame with the GPU started early).
 * The kernel call made here does the same work, clean and invalidate, with
 * no service in between. Should the kernel refuse it, the service is used
 * from then on. For the whole heap the two are timed against each other over
 * the first FLUSH_TRIAL flushes of each and the faster kept, logged once.
 */
Result __real_GSPGPU_FlushDataCache(const void *adr, u32 size);
static int sFlushDirect = 1;        /* 0: the kernel refused the call */
static int sFlushChoice = -1;       /* whole heap: -1 still timing, 0 service, 1 direct */
static uint64_t sFlushTrial[2];
static unsigned sFlushTrials[2];
#define FLUSH_TRIAL 120u
#define FLUSH_LARGE (1u << 20)

static Result FlushDirect(const void *adr, u32 size)
{
    Result rc = svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)adr, size);

    if (R_FAILED(rc))
    {
        sFlushDirect = 0;
        CtrLog_Write(CTR_LOG_VIDEO, "cache flush: kernel call refused (%08lx), GSP service used",
                     (unsigned long)rc);
        return __real_GSPGPU_FlushDataCache(adr, size);
    }
    return rc;
}

Result __wrap_GSPGPU_FlushDataCache(const void *adr, u32 size)
{
    uint64_t start;
    Result rc;
    int way;

    if (sFlushDirect == 0)
        return __real_GSPGPU_FlushDataCache(adr, size);
    if (size < FLUSH_LARGE)
        return FlushDirect(adr, size);
    way = sFlushChoice >= 0 ? sFlushChoice : (int)((sFlushTrials[0] + sFlushTrials[1]) & 1);
    start = svcGetSystemTick();
    rc = way ? FlushDirect(adr, size) : __real_GSPGPU_FlushDataCache(adr, size);
    if (sFlushChoice < 0 && sFlushDirect != 0)
    {
        sFlushTrial[way] += svcGetSystemTick() - start;
        if (++sFlushTrials[way] >= FLUSH_TRIAL && sFlushTrials[!way] >= FLUSH_TRIAL)
        {
            float service = sFlushTrial[0] * 1000.0f / SYSCLOCK_ARM11 / sFlushTrials[0];
            float direct = sFlushTrial[1] * 1000.0f / SYSCLOCK_ARM11 / sFlushTrials[1];

            sFlushChoice = direct <= service;
            CtrLog_Write(CTR_LOG_VIDEO, "cache flush of the heap: GSP service %.3f ms, kernel call %.3f ms; "
                         "%s kept", service, direct, sFlushChoice ? "kernel call" : "GSP service");
        }
    }
    return rc;
}

static void FlushLinear(void)
{
    uint64_t start = svcGetSystemTick();

    GSPGPU_FlushDataCache((void *)__ctru_linear_heap, __ctru_linear_heap_size);
    sFlushTicks += svcGetSystemTick() - start;
    ++sFlushes;
}

/*
 * A render target made in VRAM holds whatever the VRAM held before - on
 * hardware the atlas or surface that was there last; Azahar hands out
 * zeroes. The battle's sprite slots once came out as an old atlas where the
 * info boxes should be, and a surface is only ever drawn into where it is
 * used, so each is cleared whole once, in the next frame (the targets are
 * made between frames, where no command list is open).
 *
 * Each fill is an entry of the GX queue, which holds 32 and stops the
 * console (svcBreak in gxCmdQueueAdd) at the 33rd, and one fill has two
 * units: the targets are cleared two to an entry, colour only (none of them
 * has a depth buffer), in the room the frame's drawing leaves: four
 * entries in the fullest frame, eight targets. The most made at once are
 * seven (the bottom screen, the three planes and the battle's scene, slots
 * and window); the layers need no clear (LayerCreate).
 * A clear past that waits for the next frame, logged, and wipes what this
 * frame draws there: the caller forgets the scene cache then.
 */
#define MADE_CLEARS 16
static C3D_RenderTarget *sMadeClears[MADE_CLEARS];
static bool sMadeLate[MADE_CLEARS];
static unsigned sMadeClearCount;

static void ClearWhenMade(C3D_RenderTarget *target)
{
    if (target == NULL) return;
    for (unsigned i = 0; i < sMadeClearCount; ++i)
        if (sMadeClears[i] == target) return;
    if (sMadeClearCount < MADE_CLEARS)
    {
        sMadeLate[sMadeClearCount] = false;
        sMadeClears[sMadeClearCount++] = target;
    }
    else CtrLog_Write(CTR_LOG_VIDEO, "made-target clears full: one left as made");
}

/* A target released before its clear: forgotten. */
static void ClearWhenMadeForget(C3D_RenderTarget *target)
{
    for (unsigned i = 0; i < sMadeClearCount; ++i)
        if (sMadeClears[i] == target)
        {
            --sMadeClearCount;
            sMadeClears[i] = sMadeClears[sMadeClearCount];
            sMadeLate[i] = sMadeLate[sMadeClearCount];
        }
}

/* One fill unit: a target's colour buffer, whole, to zero. */
static void ClearFillUnit(const C3D_FrameBuf *frame, u32 **start, u32 **end, u16 *control)
{
    /* Bytes past 2: RGBA8 4, RGB8 3, the rest 2 (Citro3D's colorFmtSizes). */
    unsigned wide = frame->colorFmt == GPU_RB_RGBA8 ? 2 : frame->colorFmt == GPU_RB_RGB8 ? 1 : 0;

    *start = (u32 *)frame->colorBuf;
    *end = (u32 *)((u8 *)frame->colorBuf + (u32)frame->width * frame->height * (2 + wide));
    *control = (u16)(wide << 8 | 1);
}

/* Right after C3D_FrameBegin: the fills go into the queue before anything
 * the frame draws. True when one of them was made frames ago, drawn into
 * since and now wiped. */
static bool ClearsRun(void)
{
    unsigned room = 0, done = 0;
    bool late = false;

    if (sFrameQueue)
    {
        unsigned capacity = SharedCapacity(), used = SharedUsed();

        room = capacity > used + RENDER_RESERVE_MOST ? capacity - used - RENDER_RESERVE_MOST : 0;
    }
    for (; done < sMadeClearCount && room; --room)
    {
        u32 *start0, *end0, *start1 = NULL, *end1 = NULL;
        u16 control0, control1 = 0;

        late |= sMadeLate[done];
        ClearFillUnit(&sMadeClears[done++]->frameBuf, &start0, &end0, &control0);
        if (done < sMadeClearCount)
        {
            late |= sMadeLate[done];
            ClearFillUnit(&sMadeClears[done++]->frameBuf, &start1, &end1, &control1);
        }
        GX_MemoryFill(start0, 0, end0, control0, start1, 0, end1, control1);
    }
    if (done < sMadeClearCount)
    {
        CtrLog_Write(CTR_LOG_VIDEO, "made-target clears: %u of %u wait for the next frame",
                     sMadeClearCount - done, sMadeClearCount);
        memmove(sMadeClears, sMadeClears + done, (sMadeClearCount - done) * sizeof(*sMadeClears));
        for (unsigned i = 0; i < sMadeClearCount - done; ++i) sMadeLate[i] = true;
    }
    sMadeClearCount -= done;
    return late;
}

Result __real_GX_ProcessCommandList(u32 *buf0a, u32 buf0s, u8 flags);
Result __wrap_GX_ProcessCommandList(u32 *buf0a, u32 buf0s, u8 flags)
{
    if (sGpuEarly) FlushLinear();
    return __real_GX_ProcessCommandList(buf0a, buf0s, flags);
}

/*
 * GPU copies queued back to back behind one split of the frame's command list
 * (CtrVideo_CopiesBegin/End). C3D_SyncTextureCopy splits it for every copy,
 * and with the GPU started early each split and each copy flushed the whole
 * linear heap: a pass of the animated sea, some 40 copies, flushed it 80
 * times in one frame - the frame drops while walking on an Old 3DS (port.log
 * 2026-10-28). The split flushes once; the copies' sources are the caller's
 * to flush.
 */
static bool sCopyBatch;
/* The frame's GPU work by count, for the frame profiler (CtrGpuCounts_Take). */
static CtrGpuCounts sGpuCounts;

void CtrGpuCounts_Take(CtrGpuCounts *out)
{
    *out = sGpuCounts;
    memset(&sGpuCounts, 0, sizeof(sGpuCounts));
}

/*
 * Texels the CPU writes once the GPU is started on the frame (GpuStartEarly):
 * a tile decoded into the atlas (GetTileSlot), a texture filled from the CPU
 * (TexFilled). The data cache is flushed before each list the queue is given
 * (the wrappers above), so they reach memory; but the PICA's texture cache,
 * dropped at FrameBegin before anything this frame samples (TexCacheClear),
 * may by then hold lines of that memory the GPU fetched since - a tile's
 * neighbour in the atlas sampled earlier in the frame, should a line of that
 * cache span more than one 128-byte slot (not known; Azahar has no such
 * cache, so no picture of Azahar's can show it either way). Before the GPU
 * started, every write came ahead of the drop at FrameBegin. After it, the
 * next draw recorded drops the cache first (the draw wrappers below), once
 * for every run of writes - one register write, and only on frames that
 * write after the start; any drop emitted meanwhile (a target switched to,
 * a split) serves as well. Counted for the log (sTexelsLateDrops).
 */
static bool sTexelsLate;
static uint32_t sTexelsLateDrops;

static void TexelsWritten(void)
{
    sTexelsLate |= sGpuEarly;
}

/* The PICA's texture cache dropped, in command-list order. */
static void TexCacheDrop(void)
{
    GPUCMD_AddMaskedWrite(GPUREG_TEXUNIT_CONFIG, 0x4, 1u << 16);
    sTexelsLate = false;
}

/* A texture the CPU filled: flushed to memory, and its texels new to the GPU. */
static void TexFilled(C3D_Tex *tex)
{
    C3D_TexFlush(tex);
    TexelsWritten();
}

/* Before a draw: the drop owed to texels written since the last one. */
static void TexelsLateDrop(void)
{
    if (!sTexelsLate) return;
    TexCacheDrop();
    ++sTexelsLateDrops;
}

void __real_C3D_DrawArrays(GPU_Primitive_t primitive, int first, int size);
void __wrap_C3D_DrawArrays(GPU_Primitive_t primitive, int first, int size)
{
    ++sGpuCounts.draws;
    sGpuCounts.vertices += (uint32_t)size;
    TexelsLateDrop();
    __real_C3D_DrawArrays(primitive, first, size);
}

void __real_C3D_DrawElements(GPU_Primitive_t primitive, int count, int type, const void *indices);
void __wrap_C3D_DrawElements(GPU_Primitive_t primitive, int count, int type, const void *indices)
{
    ++sGpuCounts.draws;
    sGpuCounts.vertices += (uint32_t)count;
    TexelsLateDrop();
    __real_C3D_DrawElements(primitive, count, type, indices);
}

bool __real_C3D_FrameDrawOn(C3D_RenderTarget *target);
bool __wrap_C3D_FrameDrawOn(C3D_RenderTarget *target)
{
    ++sGpuCounts.targets;
    return __real_C3D_FrameDrawOn(target);
}

Result __real_GX_TextureCopy(u32 *inadr, u32 indim, u32 *outadr, u32 outdim, u32 size, u32 flags);
Result __wrap_GX_TextureCopy(u32 *inadr, u32 indim, u32 *outadr, u32 outdim, u32 size, u32 flags)
{
    ++sGpuCounts.copies;
    if (sGpuEarly && !sCopyBatch) FlushLinear();
    return __real_GX_TextureCopy(inadr, indim, outadr, outdim, size, flags);
}

void CtrVideo_CopiesBegin(void)
{
    C3D_FrameSplit(0);
    sCopyBatch = true;
}

void CtrVideo_CopiesEnd(void)
{
    sCopyBatch = false;
}

Result __real_GX_DisplayTransfer(u32 *inadr, u32 indim, u32 *outadr, u32 outdim, u32 flags);
Result __wrap_GX_DisplayTransfer(u32 *inadr, u32 indim, u32 *outadr, u32 outdim, u32 flags)
{
    /* Screens and render targets: VRAM the GPU wrote, nothing to flush. */
    ++sGpuCounts.copies;
    return __real_GX_DisplayTransfer(inadr, indim, outadr, outdim, flags);
}

Result __real_GX_RequestDma(u32 *src, u32 *dst, u32 length);
Result __wrap_GX_RequestDma(u32 *src, u32 *dst, u32 length)
{
    if (sGpuEarly) FlushLinear();
    return __real_GX_RequestDma(src, dst, length);
}

/* In the frame: queue what is recorded so far and start the GPU on it. */
#ifndef CTR_GPU_EARLY
#define CTR_GPU_EARLY 1
#endif
/* Measured on an Old 3DS by turning it off every other 600 frames: the
 * voxel battle 59.8 fps with it, 48-52 without; the 2D battle no worse. */
static bool GpuEarlyThisFrame(void)
{
    return CTR_GPU_EARLY;
}

/*
 * Every route of the frame calls this at the first point where the GPU has
 * work queued (CTR_EARLY_*, which the frame profiler keeps per picture), and
 * it starts the queue at the first of them; the later ones find it running.
 * What the CPU writes after that point must not be anything a command
 * already queued reads (the protocol above): the tile atlas's slots are
 * written only for a tile not yet checked this frame (GetTileSlot), so never
 * a slot a queued command uses; the textures filled from the CPU are filled
 * before the frame draws (HudFrame), keyed so that a second fill in the frame
 * writes nothing (BoxWindowSync, LeavesTexture, CornerArtReady), or made the
 * first time they are drawn; the geometry goes into buffers only appended to
 * in a frame (Citro2D's, FastCells'); and nothing is freed in the frame.
 * Texels written after the start reach memory by the flush before each list,
 * and the GPU's texture cache is dropped before the next draw (TexelsWritten):
 * what it fetched of their neighbours earlier in the frame may cover them.
 */
static uint8_t sGpuEarlyAt;

/*
 * Residency is the consumer's to decide: each surface is asked for by what
 * draws from it, and a frame in which one went without (CTR_DENIED_*) says
 * so - in the frame profiler's record, and by consumer in the log every 600
 * frames - instead of the one line a first failure logged. Set from Present's
 * preparation on, before FrameBegin, and taken with the frame's figures.
 */
static uint8_t sDeniedNow;
static uint16_t sDeniedFrames[CTR_DENIED_KINDS];

static void SurfaceDenied(unsigned bits)
{
    sDeniedNow |= (uint8_t)bits;
}

/*
 * How far the frame has queued (CTR_STAGE_*), for its queue drains
 * (QueueDone): set right after the split that queued the stage, so a drain
 * tagged with it had everything up to there behind it; or, where that split
 * starts the queue (GpuStartEarly), right before it, or a drain at once would
 * be the stage before's. Measure only.
 */
static volatile uint8_t sGpuStage;

/*
 * settings.txt gpu_ops=N (measure only): what the GPU's time in a frame goes
 * on, by the kind of work. The queue runs its commands one at a time and each
 * ends in an interrupt - a memory fill (a clear), a transfer (a picture to
 * the screen, a copy) or a list of drawing commands (one per split of the
 * frame) - so with the queue never empty, which is the Old 3DS in the voxel
 * field, the time between two interrupts is the command that ended at the
 * second. Summed from the frame's early start (GpuStartEarly) and logged as
 * means every N frames of them, or when the voxel module asks
 * (CtrVideo_GpuOpsFlush: at each change of an A/B round). The lists are told
 * apart by their order in the frame: in the voxel field the world, then its
 * picture to the screen with what is laid over it, for each eye, then the
 * rest.
 */
enum { OPS_FILL, OPS_COPY, OPS_LIST, OPS_KINDS, OPS_LISTS = 8 };

static struct
{
    volatile bool live;
    uint32_t last;
    unsigned list;
    uint32_t frames;
    uint64_t ticks[OPS_KINDS];
    uint32_t count[OPS_KINDS];
    uint64_t lists[OPS_LISTS];
} sOps;
static int sOpsEvery = -1;

static void OpsEvent(void *arg)
{
    unsigned kind = (unsigned)(uintptr_t)arg;
    uint32_t now = (uint32_t)svcGetSystemTick(), spent;

    if (!sOps.live) return;
    spent = now - sOps.last;
    sOps.last = now;
    sOps.ticks[kind] += spent;
    sOps.count[kind]++;
    if (kind == OPS_LIST)
    {
        sOps.lists[sOps.list < OPS_LISTS ? sOps.list : OPS_LISTS - 1] += spent;
        sOps.list++;
    }
}

void CtrVideo_GpuOpsFlush(void)
{
    char lists[OPS_LISTS * 8 + 1];
    double ms = 1000.0 / SYSCLOCK_ARM11;
    unsigned at = 0;

    if (sOpsEvery <= 0 || sOps.frames == 0) return;
    sOps.live = false;
    ms /= sOps.frames;
    lists[0] = 0;
    for (unsigned i = 0; i < OPS_LISTS; ++i)
        at += (unsigned)snprintf(lists + at, sizeof(lists) - at, " %.2f", (double)sOps.lists[i] * ms);
    CtrLog_Write(CTR_LOG_VIDEO, "GPU OPS %lu frames to game frame %lu, ms a frame (commands): fills %.2f (%.1f) "
                 "transfers %.2f (%.1f) lists %.2f (%.1f), by list:%s",
                 (unsigned long)sOps.frames, (unsigned long)CtrPlatform_FrameCount(),
                 (double)sOps.ticks[OPS_FILL] * ms, (double)sOps.count[OPS_FILL] / sOps.frames,
                 (double)sOps.ticks[OPS_COPY] * ms, (double)sOps.count[OPS_COPY] / sOps.frames,
                 (double)sOps.ticks[OPS_LIST] * ms, (double)sOps.count[OPS_LIST] / sOps.frames, lists);
    sOps.frames = 0;
    memset(sOps.ticks, 0, sizeof(sOps.ticks));
    memset(sOps.count, 0, sizeof(sOps.count));
    memset(sOps.lists, 0, sizeof(sOps.lists));
}

/* The frame's queue is about to run. */
static void OpsFrameStart(void)
{
    if (sOpsEvery < 0)
    {
        sOpsEvery = CtrSettings_GetInt("gpu_ops", 0);
        if (sOpsEvery > 0)
        {
            gspSetEventCallback(GSPGPU_EVENT_PSC0, OpsEvent, (void *)(uintptr_t)OPS_FILL, false);
            gspSetEventCallback(GSPGPU_EVENT_PSC1, OpsEvent, (void *)(uintptr_t)OPS_FILL, false);
            gspSetEventCallback(GSPGPU_EVENT_PPF, OpsEvent, (void *)(uintptr_t)OPS_COPY, false);
            gspSetEventCallback(GSPGPU_EVENT_P3D, OpsEvent, (void *)(uintptr_t)OPS_LIST, false);
        }
    }
    if (sOpsEvery <= 0) return;
    if (sOps.frames >= (uint32_t)sOpsEvery) CtrVideo_GpuOpsFlush();
    sOps.live = false;
    sOps.list = 0;
    sOps.frames++;
    sOps.last = (uint32_t)svcGetSystemTick();
    sOps.live = true;
}

static void GpuStartEarly(unsigned at)
{
    if (!GpuEarlyThisFrame() || sGpuEarly || sFrameQueue == NULL) return;
    C2D_Flush();
    C3D_FrameSplit(0);
    FlushLinear();
    sGpuEarly = true;
    sGpuEarlyAt = (uint8_t)at;
    sGpuEarlyStart = svcGetSystemTick();
    OpsFrameStart();
    gxCmdQueueRun(sFrameQueue);
}

/*
 * Before C3D_FrameEnd. A queue that has caught up is stopped - idle, so
 * safely - and FrameEnd starts it as it always does. One still busy is left
 * running and FrameEnd's commands join it (true is returned: the heap is
 * then flushed by the wrapper before the last list, and FrameEnd is told
 * not to again). Waiting for it instead cost the CPU 3-5 ms a frame on an
 * Old 3DS that it used to spend on the game while the GPU finished.
 */
static bool GpuFinishEarly(void)
{
    if (!sGpuEarly) return false;
    if (sFrameQueue->lastEntry == sFrameQueue->numEntries)
    {
        gxCmdQueueStop(sFrameQueue);
        sGpuEarly = false;
        return false;
    }
    return true;
}

bool CtrVideo_TryVoxelUpload(void)
{
    if (sFrameQueue == NULL)
        return false;
    unsigned capacity = SharedCapacity();
    unsigned used = SharedUsed();

    if (used + 2u + sRenderReserve > capacity
     || sUploadCommands + 2u + sRenderReserve > capacity)
        return false;
    sUploadCommands += 2u;
    return true;
}

bool CtrVideo_TryVoxelAnimUpload(void)
{
    unsigned capacity;

    if (sFrameQueue == NULL)
        return false;
    capacity = sFrameQueue->maxEntries;
    if (capacity <= GX_QUEUE_SHARED)
        return CtrVideo_TryVoxelUpload(); /* not lengthened: the shared room */
    if (sAnimCommands + 2u > capacity - GX_QUEUE_SHARED
     || sFrameQueue->numEntries + 2u + sRenderReserve > capacity)
        return false;
    sAnimCommands += 2u;
    return true;
}

unsigned CtrVideo_VoxelUploadsLeft(void)
{
    unsigned capacity, used, room;

    if (sFrameQueue == NULL)
        return 0;
    capacity = SharedCapacity();
    used = SharedUsed();
    if (used < sUploadCommands)
        used = sUploadCommands;
    room = capacity > used + sRenderReserve ? capacity - used - sRenderReserve : 0;
    return room / 2u;
}
