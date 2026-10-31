/* GBA compatibility compositor on PICA200. CPU work only decodes dirty tiles
 * into a texture atlas and submits geometry. All rasterization, transforms,
 * transparency, priority composition and scaling happen on the GPU. */
#include <citro2d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "3ds_platform.h"
#include "3ds_daynight.h"
#include "3ds_gpumem.h"
#include "3ds_video.h"
#include "3ds_tile_banks.h"
#include "3ds_dex_top.h"
#include "3ds_corner_art.h"
#include "3ds_data.h"
#include "3ds_bottom.h"
#include "ctr_battle_layout.h"
#include "3ds_battle_hud.h"
#include "3ds_local.h"
#include "../compat/port_prof.h"
#include "../compat/ctr_gba_battle.h"

/* A battle is on, transition and all (CtrVideo_InBattle, at the end). */
static bool sInBattle;

/* Public queue passed by libctru, never a cast of Citro3D's private context.
 * Uploads reserve worst-case split+copy entries and leave the compositor's
 * clears, postprocessing, battle layers and final transfers room to finish. */
static gxCmdQueue_s *sFrameQueue;
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
 * eight (the bottom screen, the three planes, the stereo surface and the
 * battle's scene, slots and window); the layers need no clear (LayerCreate).
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

Result __real_GX_TextureCopy(u32 *inadr, u32 indim, u32 *outadr, u32 outdim, u32 size, u32 flags);
Result __wrap_GX_TextureCopy(u32 *inadr, u32 indim, u32 *outadr, u32 outdim, u32 size, u32 flags)
{
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

static void GpuStartEarly(void)
{
    if (!GpuEarlyThisFrame() || sGpuEarly || sFrameQueue == NULL) return;
    C2D_Flush();
    C3D_FrameSplit(0);
    FlushLinear();
    sGpuEarly = true;
    sGpuEarlyStart = svcGetSystemTick();
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

/* The voxel overworld replaces this compositor's output while the player is
 * walking around; every other game state keeps the 2D path below. */
#include "voxel/ctr_voxel.h"
#include "voxel/voxel_battle.h"
#include "voxel/voxel_swoop.h"
#include "voxel/voxel_fly.h"
#include "3ds_fly_sky.h"

/* The sky scene's supply (3ds_fly_sky.c, a game translation unit): absent
 * from a build without the game, the scene stays off. */
__attribute__((weak)) bool CtrFlySky_Enabled(void) { return false; }
__attribute__((weak)) bool CtrFlySky_MonSprite(uint16_t species, uint32_t personality, bool shiny, CtrSkySprite *out)
{ (void)species; (void)personality; (void)shiny; (void)out; return false; }
__attribute__((weak)) bool CtrFlySky_FlyerSprite(CtrSkySprite *out, int8_t *seatX, int8_t *seatY)
{ (void)out; (void)seatX; (void)seatY; return false; }
__attribute__((weak)) bool CtrFlySky_RiderSprite(CtrSkySprite *out) { (void)out; return false; }
__attribute__((weak)) unsigned CtrFlySky_Natives(uint16_t *species, unsigned max) { (void)species; (void)max; return 0; }
__attribute__((weak)) void CtrFlySky_Release(void) {}
#include "3ds_map_events.h"

/*
 * The game lays itself out over the whole 400x240 viewport, so screen and
 * logical coordinates are the same. A background only covers the part of the
 * screen its tilemap actually spans; see DrawTextBg.
 *
 * A GBA stage (CtrVideo_SetStage) is the exception: its 240x160 picture sits
 * 1:1 at (CTR_STAGE_X, CTR_STAGE_Y) and the rest of the screen is filled around
 * it. sViewX/Y is where the GBA origin lands on the screen; everything else
 * stays in GBA coordinates, so the space around the picture is negative or
 * past 240x160.
 *
 * A centred GBA screen (CtrVideo_SetCentred) sits at the same place, but its
 * margins are not filled from the picture: only the layers that wrap on the
 * GBA (an affine map with wrap-around) and sprites reach into them, and text
 * layers stop at the edge of the 240x160 screen - except the backgrounds a
 * screen names in sCentredFills, which carry on to the edges.
 *
 * The battle scene (CtrVideo_SetBattle) sits centred across and on the bottom
 * edge, so its text box is the bottom of the top screen. The text box is
 * stretched to the whole width, and the scene above and beside it comes from
 * its own layers as the GBA would wrap them (DrawBattleBg).
 */
static int sViewX, sViewY;
/*
 * Magnification of the frame being composed: a screen point is
 * (gba + view) * zoom + offset. The battle scene is composed at
 * CTR_BATTLE_ZOOM and its text box at 1 (DrawBattleTextLayer).
 */
static float sZoom = 1.0f, sOffX, sOffY;
/* Size of the surface being composed: the screen, or the battle scene's. */
static int sTargetW = CTR_GAME_WIDTH, sTargetH = CTR_GAME_HEIGHT;
/* Height of the texture behind it: scissor rows count from its far edge. */
static int sSurfaceH = 256;
/*
 * The battle scene at 1.5 is not composed at 1.5: nearest sampling at a
 * fractional scale makes GBA pixels alternately one and two screen pixels
 * wide, which shimmers on hardware as anything moves. It is composed at 2,
 * every GBA pixel an exact 2x2 block, into a surface of its own, and that
 * surface is drawn at 0.75 with bilinear filtering: every pixel the same
 * size, edges a pixel soft ("sharp bilinear"). The text box is composed
 * after it at 1:1, crisp. See RenderBattleScene.
 */
#define SCENE_ZOOM 2.0f
#define SCENE_W 1024
#define SCENE_H 256
/*
 * The voxel battle's world picture lives in the scene surface's first
 * WORLD_COLUMNS columns (RenderBattleWorld): no VRAM surface of its own,
 * which on hardware the battle found no room for in one battle of two
 * (404 KiB free in blocks under 140 KiB: "3D voxel surface not made", the
 * whole battle flat). The scenery and the slots then start past it, after
 * WORLD_GAP cleared columns: the filter at a slot's left edge reads the
 * column before it.
 */
#define WORLD_COLUMNS 400
#define WORLD_GAP 8
static int sSceneColumn;
/*
 * The GBA line the battle scene is composed down to: the top of the text box
 * frame (CTR_BATTLE_BOX_TOP), which floats a few lines lower than the GBA's
 * picture ends; below that, round the box's panel, the layers are composed
 * at the screen's scale apart (BattleEdgesCompose).
 */
#define BATTLE_SCENE_BOX_BOTTOM (112 + (int)ceilf((CTR_BATTLE_BOX_TOP - (CTR_GAME_HEIGHT - 48)) / CTR_BATTLE_ZOOM))
/* Or the whole picture, with no text box to go behind (BattleSceneBottom). */
static int BattleSceneBottom(void);
#define BATTLE_SCENE_BOTTOM BattleSceneBottom()
static C3D_Tex sSceneTex;
static C3D_RenderTarget *sScene;
static bool sSceneFailed;
static uint32_t sSceneUsedFrame, sSceneFailFrame;
/* Layers (Layers mask bits) the current composition pass leaves out. */
static unsigned sLayerExclude;
/* Screen pixels per GBA pixel that the stereo displacement is measured in. */
static float sShiftZoom = 1.0f;
#define CTR_VIEW_X sViewX
#define CTR_VIEW_Y sViewY
/* Requested by the game bridge, and what the current frame is composed as. */
static bool sStageRequested, sStage;
/* A stage whose every layer fades into the margins (CtrVideo_SetStageFaded). */
static bool sStageFadedRequested, sStageFaded;
/* A stage magnified to fit the screen (CtrVideo_SetStageZoomed). */
static bool sStageZoomedRequested, sStageZoomed;
static bool sCentred;
static unsigned sCentredRequested, sCentredScreen;
static bool sBattleRequested, sBattle;
/*
 * A battle in front of the voxel world (RenderBattleWorld): this frame's
 * scenery is the world, and the layers it stands in for are left out of the
 * battle's own picture - BG3, its scenery, and while the intro slides that
 * in, the entry picture on BG1 and BG2.
 */
static bool sBattleWorld;
static unsigned sWorldLayers;
/* battle_bg.c: BG3 holds a move's background rather than the scenery.
 * battle_intro.c: the intro is sliding the scenery in. */
unsigned char CtrBattleBg_MoveBgShown(void);
unsigned char CtrBattleIntro_Sliding(void);
/* Visible extent in GBA coordinates. */
#define VIEW_LEFT ((int)floorf(-sOffX / sZoom) - sViewX)
#define VIEW_TOP ((int)floorf(-sOffY / sZoom) - sViewY)
#define VIEW_RIGHT ((int)ceilf((sTargetW - sOffX) / sZoom) - sViewX)
#define VIEW_BOTTOM ((int)ceilf((sTargetH - sOffY) / sZoom) - sViewY)

/*
 * Scanline scroll (CtrVideo_SetLineScroll): the eight BG scroll registers,
 * one value per line, and which of them the frame drives line by line.
 */
#define LINE_MAX 240
static uint16_t sLineScroll[8][LINE_MAX];
static unsigned sLineMask, sLineCount;

#define ATLAS_SIZE 1024
#define CACHE_COUNT 16384
#define HASH_COUNT 32768
/* Citro2D quads a frame. Layer cells, the bulk of them once, have their own
 * buffer (FastCells), which takes the linear memory this used to. */
#define MAX_DRAWS 12288

typedef struct
{
    uint32_t key, checked, paletteVersion;
    uint8_t bytes[64];
    uint32_t colors[8];
    /* Bumped on every upload (new bytes or new palette), never reused. */
    uint32_t serial;
    bool valid, visible;
} Tile;
static uint32_t sTileSerial;

/*
 * Tiles are cached by palette: 0-15 the BG banks, 16-31 the OBJ banks, 32 and
 * 33 the BG and OBJ palettes as one 256-colour bank, and from PAL_EXT the OBJ
 * banks past the GBA's sixteen (CtrVideo_LatchExtPalettes), whose colours
 * follow the GBA's 512 here, and from PAL_TILE_BANK the field's banks of
 * other tilesets (CtrVideo_SetTileBank), seven palettes a bank, after those.
 */
#define PAL_BG256 32
#define PAL_OBJ256 33
#define PAL_EXT 34
#define PAL_TILE_BANK (PAL_EXT + CTR_EXT_PALETTES)
#define PAL_IDS (PAL_TILE_BANK + CTR_TILE_BANKS * CTR_TILE_BANK_PALETTES)
/* Where the banks' colours start in sPalette and sTexturePalette. */
#define PAL_TILE_BANK_COLOURS (512 + CTR_EXT_PALETTES * 16)
#define PAL_COLOURS (PAL_TILE_BANK_COLOURS + CTR_TILE_BANKS * CTR_TILE_BANK_PALETTES * 16)

static uint32_t sOamAnchored[4];
static int16_t sOamAnchor[128];

void CtrVideo_ClearOamAnchors(void)
{
    memset(sOamAnchored, 0, sizeof(sOamAnchored));
}

void CtrVideo_SetOamAnchor(unsigned first, unsigned end, int y)
{
    if (end > 128) end = 128;
    for (unsigned i = first; i < end; ++i)
    {
        sOamAnchored[i >> 5] |= 1u << (i & 31);
        sOamAnchor[i] = (int16_t)y;
    }
}

/* The OAM y (8 bits) moved by whole turns of 256 to lie nearest its sprite's
 * true y: an entry of a sprite is never 128 lines from where it is anchored. */
static int OamUnwrapY(unsigned index, int y)
{
    int anchor = sOamAnchor[index];

    while (y - anchor > 128) y -= 256;
    while (anchor - y > 128) y += 256;
    return y;
}

static uint32_t sVoxelWeatherOam[4];
static uint32_t sVoxelCloudOam[4];
/*
 * Which sprites the voxel view draws over itself: the weather, before its
 * text layer, and then the sprites placed on the screen rather than on the
 * map (sScreenOam), which on the GBA sit over the text layer: the mon shown
 * for a field move over its banner, Fly's bird and its rider. The map's own
 * sprites are cards in the 3D scene (voxel_entities.c).
 */
enum { VOXEL_OBJ_NONE, VOXEL_OBJ_WEATHER, VOXEL_OBJ_SCREEN, VOXEL_OBJ_FIELD };
static unsigned sVoxelObjPass = VOXEL_OBJ_NONE;
/* The swoop (voxel_swoop.c) has squashed the world's sprite cards away: the
 * game's own object sprites are laid over the world instead, where the 2D
 * field puts them (VOXEL_OBJ_FIELD). */
static bool sSwoopFieldObjs;

void CtrVideo_ClearVoxelWeatherOam(void)
{
    memset(sVoxelWeatherOam, 0, sizeof(sVoxelWeatherOam));
    memset(sVoxelCloudOam, 0, sizeof(sVoxelCloudOam));
}

void CtrVideo_MarkVoxelWeatherOam(unsigned first, unsigned end)
{
    if (end > 128) end = 128;
    for (unsigned i = first; i < end; ++i)
        sVoxelWeatherOam[i >> 5] |= 1u << (i & 31);
}

void CtrVideo_MarkVoxelCloudOam(unsigned first, unsigned end)
{
    if (end > 128) end = 128;
    for (unsigned i = first; i < end; ++i)
        sVoxelCloudOam[i >> 5] |= 1u << (i & 31);
}

/* OAM entries of sprites placed on the screen rather than on the map. */
static uint32_t sScreenOam[4];
static uint32_t sMachineOam[4];
static uint32_t sFlyOam[4];

void CtrVideo_ClearScreenOam(void)
{
    memset(sScreenOam, 0, sizeof(sScreenOam));
    memset(sMachineOam, 0, sizeof(sMachineOam));
    memset(sFlyOam, 0, sizeof(sFlyOam));
}

void CtrVideo_MarkScreenOam(unsigned first, unsigned end)
{
    if (end > 128) end = 128;
    for (unsigned i = first; i < end; ++i)
        sScreenOam[i >> 5] |= 1u << (i & 31);
}

/* Screen sprites that belong to the map's picture around the player. */
void CtrVideo_MarkMachineOam(unsigned first, unsigned end)
{
    if (end > 128) end = 128;
    for (unsigned i = first; i < end; ++i)
        sMachineOam[i >> 5] |= 1u << (i & 31);
}

/* Fly's bird and its rider, on that picture around the player too. */
void CtrVideo_MarkFlyOam(unsigned first, unsigned end)
{
    if (end > 128) end = 128;
    for (unsigned i = first; i < end; ++i)
        sFlyOam[i >> 5] |= 1u << (i & 31);
}

/* OAM entries of the fog's sprites (CtrVideo_MarkFogOam). */
static uint32_t sFogOam[4];

/* Weather carried over the whole view (CtrVideo_MarkWeatherOam). */
static uint32_t sLatticeOam[4], sParticleOam[4];

void CtrVideo_ClearFogOam(void)
{
    memset(sFogOam, 0, sizeof(sFogOam));
    memset(sLatticeOam, 0, sizeof(sLatticeOam));
    memset(sParticleOam, 0, sizeof(sParticleOam));
}

void CtrVideo_MarkWeatherOam(unsigned first, unsigned end, bool lattice)
{
    uint32_t *marks = lattice ? sLatticeOam : sParticleOam;

    if (end > 128) end = 128;
    for (unsigned i = first; i < end; ++i)
        marks[i >> 5] |= 1u << (i & 31);
}

void CtrVideo_MarkFogOam(unsigned first, unsigned end)
{
    if (end > 128) end = 128;
    for (unsigned i = first; i < end; ++i)
        sFogOam[i >> 5] |= 1u << (i & 31);
}

/*
 * The ext palettes (CtrVideo_LatchExtPalettes): their faded colours as the
 * game last handed them over, which are in use, and the one each OAM entry
 * is drawn with, plus one (0: attr2's own).
 */
static uint16_t sExtPltt[CTR_EXT_PALETTES * 16];
static unsigned sExtInUse;
static uint8_t sOamExt[128];
static bool sOamExtMarked;

void CtrVideo_LatchExtPalettes(const uint16_t *colors, unsigned inUse)
{
    sExtInUse = inUse & ((1u << CTR_EXT_PALETTES) - 1);
    if (sExtInUse)
        memcpy(sExtPltt, colors, sizeof(sExtPltt));
}

const uint16_t *CtrVideo_ExtPalette(unsigned index)
{
    return sExtPltt + (index % CTR_EXT_PALETTES) * 16;
}

void CtrVideo_ClearExtPaletteOam(void)
{
    if (!sOamExtMarked) return;
    memset(sOamExt, 0, sizeof(sOamExt));
    sOamExtMarked = false;
}

void CtrVideo_MarkExtPaletteOam(unsigned first, unsigned end, unsigned index)
{
    if (index >= CTR_EXT_PALETTES) return;
    if (end > 128) end = 128;
    for (unsigned i = first; i < end; ++i)
        sOamExt[i] = (uint8_t)(index + 1);
    sOamExtMarked = sOamExtMarked || first < end;
}

/* The palette bank OAM entry i is drawn with (GetTileSlot): attr2's, or the
 * ext palette it is marked with while that one is in use - a mark outlives a
 * scene that stops building OAM, the palettes do not. A 256-colour sprite has
 * the one bank. */
static unsigned ObjBank(unsigned i, unsigned attr2)
{
    unsigned ext = sOamExt[i];

    if (ext && (sExtInUse >> (ext - 1)) & 1)
        return PAL_EXT + ext - 1;
    return 16u + (attr2 >> 12);
}

/* A battle transition's sprite: on the screen, and not the weather's. */
static bool TransitionOam(unsigned i)
{
    bool screen = (sScreenOam[i >> 5] & (1u << (i & 31))) != 0;
    if (sVoxelWeatherOam[i >> 5] & (1u << (i & 31))) return false;
    return screen;
}

/* Which sprites a pass draws: all, only a transition's, or all but those. */
enum { OBJ_ALL, OBJ_TRANSITION, OBJ_FIELD };
static unsigned sObjFilter = OBJ_ALL;

static CtrVideoMemory sMemory;

const uint8_t *CtrVideo_GetBgVram(void) { return sMemory.vram; }

void CtrVideo_NotifyTilesetAnimWrite(const void *dest, unsigned bytes)
{
    CtrVideo_NotifyWrite(dest, bytes);
    uintptr_t base = (uintptr_t)sMemory.vram;
    uintptr_t address = (uintptr_t)dest;
    if (base == 0 || address < base || address - base >= 0x8000 || bytes == 0)
        return;
    unsigned offset = (unsigned)(address - base);
    if (bytes > 0x8000 - offset) bytes = 0x8000 - offset;
    CtrVoxel_NotifyTilesetAnimWrite(offset / 32, (offset % 32 + bytes + 31) / 32);
}
static CtrVideoStats sStats;
static C3D_Tex sAtlas, sSurface;
/* The voxel bloom's quarter-size target (VoxelBloomPrepare); absent, no bloom. */
static C3D_Tex sBloomTex;
static C3D_RenderTarget *sBloom;
static C3D_RenderTarget *sLogical, *sTop, *sTopRight;
static bool sStereo;
static Tile sTiles[CACHE_COUNT];
static uint16_t sHash[HASH_COUNT];
static uint16_t sPalette[PAL_COLOURS];
static uint16_t sTexturePalette[PAL_COLOURS];
static uint8_t sMorton[64];
/* A 4bpp tile's two pixels of a byte, (x, y) and (x + 1, y) for an even x,
 * are neighbouring texels in the tiled order: written as one word. */
static uint8_t sMortonPair[32];
static bool sMortonPairs;
static C2D_ImageTint sTint;
static uint32_t sPaletteVersion[PAL_IDS];
static uint32_t sPaletteChanges[PAL_IDS][8];
static unsigned sUsed;

/*
 * What changed in the logical VRAM, a kilobyte at a time. The game writes VRAM
 * in many ways (copies, DMA, decompression, plain pointers), so instead of
 * hooking them all a block is compared with a copy of what it held when it
 * was last looked at: against walking every cell of every layer and comparing
 * every tile drawn with its bytes, frame after frame, when nothing changed.
 * sVramStamp[b] is the frame token (sStats.frames + 1, as Tile.checked) at
 * which block b was last seen changed; something checked at token t still
 * holds while the stamps of its blocks are <= t.
 *
 * A block is compared the first time it is asked about in a frame (SyncBlock),
 * not every block every frame: all 96 KiB were 1.5 ms a frame on an Old 3DS,
 * in every mode, and the voxel field asks about its overlay alone. Whatever
 * records a token t for some VRAM syncs its blocks at t, so a block changed
 * and changed back while nobody looked is no change to anyone.
 */
#define VRAM_BYTES 0x18000
#define VRAM_BLOCK 1024
static uint32_t sVramShadow[VRAM_BYTES / 4];
static uint32_t sVramStamp[VRAM_BYTES / VRAM_BLOCK];
/* The frame token at which each block was last compared. */
static uint32_t sVramSynced[VRAM_BYTES / VRAM_BLOCK];
/* Blocks written through an announced copy since they were last compared
 * (CtrVideo_NotifyWrite), and what changed without one, for the log. */
static uint32_t sVramAnnounced[(VRAM_BYTES / VRAM_BLOCK + 31) / 32];
static uint32_t sUnannouncedBlocks, sUnannouncedLogged;
/* The token at which a background palette last changed. */
static uint32_t sBgPaletteStamp;
/* Bumped when the tile cache is emptied: every slot changes meaning. */
static uint32_t sCacheGeneration;

/* Eight words at a time, one branch for all eight: memcmp's byte loop cost
 * a millisecond a frame here. */
static bool BlockDiffers(const uint32_t *a, const uint32_t *b)
{
    for (unsigned i = 0; i < VRAM_BLOCK / 4; i += 8)
        if ((a[i] ^ b[i]) | (a[i + 1] ^ b[i + 1]) | (a[i + 2] ^ b[i + 2]) | (a[i + 3] ^ b[i + 3])
            | (a[i + 4] ^ b[i + 4]) | (a[i + 5] ^ b[i + 5]) | (a[i + 6] ^ b[i + 6]) | (a[i + 7] ^ b[i + 7]))
            return true;
    return false;
}

/* A writer the contract does not cover yet: its screen and block. */
static __attribute__((noinline, cold)) void UnannouncedBlock(unsigned b)
{
    ++sUnannouncedBlocks;
    if (sUnannouncedLogged < 200)
    {
        ++sUnannouncedLogged;
        CtrLog_Write(CTR_LOG_VIDEO, "CONTRACT unannounced vram block=%u (%05x) cb2=%08lx frame=%lu total=%lu",
                     b, b * VRAM_BLOCK, (unsigned long)CtrProf_Scene(), (unsigned long)sStats.frames,
                     (unsigned long)sUnannouncedBlocks);
    }
}

/*
 * The comparison every block goes through, kept as lean as before the
 * announcements: the log out of line and the announcement read after the
 * comparison. With both inside, the function saved three more registers
 * and lost the paired loads of its loop - 0.13 ms a frame of layer work on
 * an Old 3DS (F2e run).
 */
static void SyncBlockSlow(unsigned b, uint32_t now)
{
    const uint32_t *block = (const uint32_t *)sMemory.vram + b * (VRAM_BLOCK / 4);
    uint32_t *shadow = sVramShadow + b * (VRAM_BLOCK / 4);

    sVramSynced[b] = now;
    if (BlockDiffers(block, shadow))
    {
        memcpy(shadow, block, VRAM_BLOCK);
        sVramStamp[b] = now;
        if (!(sVramAnnounced[b / 32] & (1u << (b % 32))))
            UnannouncedBlock(b);
    }
    sVramAnnounced[b / 32] &= ~(1u << (b % 32));
}

void CtrVideo_NotifyWrite(const void *dest, unsigned bytes)
{
    uintptr_t base = (uintptr_t)sMemory.vram, address = (uintptr_t)dest;

    if (base == 0 || bytes == 0 || address < base || address - base >= VRAM_BYTES)
        return;
    {
        unsigned first = (unsigned)(address - base) / VRAM_BLOCK;
        unsigned end = (unsigned)(address - base) + bytes;
        unsigned last = ((end < VRAM_BYTES ? end : VRAM_BYTES) - 1) / VRAM_BLOCK;

        for (unsigned b = first; b <= last; ++b)
            sVramAnnounced[b / 32] |= 1u << (b % 32);
    }
}

/* Block b's stamp, compared this frame if it was not yet. */
/*
 * Where the announcements are trusted (VramTrustFrame): a block nothing wrote
 * through an announced copy is taken as unchanged, without comparing it. Only
 * on the field, 2D (where the comparing was a third of the layer work) or
 * voxel, and not while one of the field effects that store into VRAM
 * directly is up.
 */
#ifndef CTR_VRAM_TRUST
#define CTR_VRAM_TRUST 1
#endif
static bool sVramTrusted;

static inline uint32_t SyncBlock(unsigned b)
{
    uint32_t now = sStats.frames + 1;

    if (sVramSynced[b] != now)
    {
        if (sVramTrusted && !((sVramAnnounced[b / 32] >> (b % 32)) & 1))
            sVramSynced[b] = now;
        else
            SyncBlockSlow(b, now);
    }
    return sVramStamp[b];
}

/*
 * Once a frame, before VRAM is read. Trusted, two blocks a frame are still
 * compared, round the whole of VRAM in under a second: a write that came by
 * no announced way is logged (CONTRACT) and shown, late but shown. Every
 * block is compared on the first trusted frame, for what changed while the
 * announcements were not trusted.
 */
#define VRAM_SWEEP_BLOCKS 2u

static void VramTrustFrame(bool trusted)
{
    static unsigned sSweep;
    uint32_t now = sStats.frames + 1;

    if (trusted && !sVramTrusted)
        for (unsigned b = 0; b < VRAM_BYTES / VRAM_BLOCK; ++b)
            SyncBlockSlow(b, now);
    sVramTrusted = trusted;
    if (!trusted)
        return;
    for (unsigned i = 0; i < VRAM_SWEEP_BLOCKS; ++i)
    {
        if (sVramSynced[sSweep] != now)
            SyncBlockSlow(sSweep, now);
        sSweep = (sSweep + 1) % (VRAM_BYTES / VRAM_BLOCK);
    }
}

static void VramSyncRange(unsigned address, unsigned bytes)
{
    if (bytes == 0 || address >= VRAM_BYTES) return;
    if (bytes > VRAM_BYTES - address) bytes = VRAM_BYTES - address;
    for (unsigned b = address / VRAM_BLOCK; b <= (address + bytes - 1) / VRAM_BLOCK; ++b)
        SyncBlock(b);
}

/* Whether VRAM [address, address + bytes) changed after token t. */
static bool VramChangedAfter(unsigned address, unsigned bytes, uint32_t t)
{
    unsigned last;

    if (bytes == 0) return false;
    if (address >= VRAM_BYTES) return true;
    if (bytes > VRAM_BYTES - address) bytes = VRAM_BYTES - address;
    last = (address + bytes - 1) / VRAM_BLOCK;
    for (unsigned b = address / VRAM_BLOCK; b <= last; ++b)
        if (SyncBlock(b) > t) return true;
    return false;
}

/*
 * The field's banks of other tilesets (3ds_tile_banks.h): their tiles sit in
 * logical addresses past the GBA's VRAM, TILE_BANK_BASE on, CTR_TILE_BANK_BYTES
 * a bank, so the tile cache keys them as it keys any tile. A bank's tiles are
 * whole or not there: sTileBankStamp is the token at which they last changed.
 */
#define TILE_BANK_BASE VRAM_BYTES
static uint8_t sTileBankTiles[CTR_TILE_BANKS][CTR_TILE_BANK_BYTES];
static uint16_t sTileBankPalettes[CTR_TILE_BANKS][CTR_TILE_BANK_PALETTES * 16];
static uint32_t sTileBankStamp[CTR_TILE_BANKS];
static unsigned sTileBanksLive;
static bool sTileBanksChanged;

/*
 * The banks of the tilemap entries in VRAM TILEMAP_BANK_BASE on (the field's
 * screenblocks are 25-30), and the entry each was handed over with: a bank
 * holds only while VRAM holds that entry, so another screen writing there
 * without bg.c's copies never draws from a bank. sTilemapBankRow stamps a
 * row of 32 entries whose banks changed, which its VRAM need not have done
 * (a map left behind drawn again, the same entries now from a bank).
 */
#define TILEMAP_BANK_BASE 0x8000u
#define TILEMAP_BANK_ENTRIES ((0x10000u - TILEMAP_BANK_BASE) / 2)
static uint8_t sTilemapBank[TILEMAP_BANK_ENTRIES];
static uint16_t sTilemapBankEntry[TILEMAP_BANK_ENTRIES];
static uint32_t sTilemapBankRow[TILEMAP_BANK_ENTRIES / 32];
static uint32_t sTilemapBankStamp;
static unsigned sTilemapBanked;

#define TILEMAP_BANK_SOURCES 3
static struct
{
    const void *buffer;
    const uint8_t *banks;
    unsigned entries;
} sTilemapBankSources[TILEMAP_BANK_SOURCES];

void CtrVideo_SetTileBank(unsigned bank, const void *tiles, unsigned bytes, const uint16_t *palettes)
{
    if (bank >= CTR_TILE_BANKS) return;
    if (tiles == NULL)
    {
        sTileBanksLive &= ~(1u << bank);
        return;
    }
    if (bytes > CTR_TILE_BANK_BYTES) bytes = CTR_TILE_BANK_BYTES;
    memset(sTileBankTiles[bank], 0, sizeof(sTileBankTiles[bank]));
    memcpy(sTileBankTiles[bank], tiles, bytes);
    if (palettes != NULL)
        memcpy(sTileBankPalettes[bank], palettes, sizeof(sTileBankPalettes[bank]));
    else
        memset(sTileBankPalettes[bank], 0, sizeof(sTileBankPalettes[bank]));
    sTileBankStamp[bank] = sStats.frames + 1;
    sTileBanksLive |= 1u << bank;
    sTileBanksChanged = true;
    /* Every layer walks its cells again, as for new palettes. */
    sBgPaletteStamp = sStats.frames + 1;
}

void CtrVideo_RegisterTilemapBanks(const void *buffer, const uint8_t *banks, unsigned entries)
{
    const void *buffers[TILEMAP_BANK_SOURCES];
    const uint8_t *rows[TILEMAP_BANK_SOURCES];
    unsigned i;
    static bool sFullLogged;

    for (i = 0; i < TILEMAP_BANK_SOURCES; ++i)
    {
        buffers[i] = sTilemapBankSources[i].buffer;
        rows[i] = sTilemapBankSources[i].banks;
    }
    i = CtrTileBank_SourceSlot(buffers, rows, TILEMAP_BANK_SOURCES, buffer, banks);
    if (i == TILEMAP_BANK_SOURCES)
    {
        if (!sFullLogged) CtrLog_Write(CTR_LOG_VIDEO, "2D tile banks: no slot left for a tilemap");
        sFullLogged = true;
        return;
    }
    sTilemapBankSources[i].buffer = buffer;
    sTilemapBankSources[i].banks = banks;
    sTilemapBankSources[i].entries = entries;
}

void CtrVideo_UnregisterTilemapBanks(const void *buffer)
{
    for (unsigned i = 0; i < TILEMAP_BANK_SOURCES; ++i)
        if (sTilemapBankSources[i].buffer == buffer)
            sTilemapBankSources[i].buffer = NULL;
}

void CtrVideo_TilemapRequested(unsigned vramOffset, const void *src, unsigned bytes)
{
    const uint8_t *banks = NULL;
    const uint16_t *entries = src;
    unsigned first, count, available = 0;

    if (vramOffset >= 0x10000 || vramOffset + bytes <= TILEMAP_BANK_BASE) return;
    for (unsigned i = 0; i < TILEMAP_BANK_SOURCES; ++i)
        if (sTilemapBankSources[i].buffer == src && src != NULL)
        {
            banks = sTilemapBankSources[i].banks;
            available = sTilemapBankSources[i].entries;
            break;
        }
    /* Nothing banked there and nothing to bank: nothing to do. */
    if (banks == NULL && sTilemapBanked == 0) return;
    if (banks != NULL && sTilemapBanked == 0 && vramOffset >= TILEMAP_BANK_BASE)
    {
        /* The field's three layers come here at every camera step, most of
         * them with no neighbour banked: their rows read a word at a time
         * instead of every entry walked against the table. */
        unsigned n = bytes / 2 < available ? bytes / 2 : available, i = 0;
        uint32_t any = 0, word;

        for (; i + 4 <= n; i += 4)
        {
            memcpy(&word, banks + i, 4);
            any |= word;
        }
        for (; i < n; ++i)
            any |= banks[i];
        if (any == 0) return;
    }
    if (bytes > 0x10000 - vramOffset) bytes = 0x10000 - vramOffset;
    first = vramOffset >= TILEMAP_BANK_BASE ? (vramOffset - TILEMAP_BANK_BASE) / 2 : 0;
    count = (vramOffset + bytes - TILEMAP_BANK_BASE) / 2 - first;
    if (vramOffset < TILEMAP_BANK_BASE)
    {
        unsigned skipped = (TILEMAP_BANK_BASE - vramOffset) / 2;

        entries += skipped;
        if (banks != NULL) banks += skipped;
        available = available > skipped ? available - skipped : 0;
    }
    for (unsigned i = 0; i < count; ++i)
    {
        unsigned at = first + i;
        uint8_t bank = banks != NULL && i < available ? banks[i] : 0;
        uint16_t entry = bank ? entries[i] : 0;

        if (bank > CTR_TILE_BANKS) bank = 0;
        if (sTilemapBank[at] == bank && sTilemapBankEntry[at] == entry) continue;
        if ((sTilemapBank[at] != 0) != (bank != 0))
            sTilemapBanked += bank ? 1u : ~0u;
        sTilemapBank[at] = bank;
        sTilemapBankEntry[at] = entry;
        sTilemapBankRow[at / 32] = sStats.frames + 1;
        sTilemapBankStamp = sStats.frames + 1;
    }
}

/* Whether the banks of a row of the tilemap (its VRAM address) changed after t. */
static bool TilemapBanksChangedAfter(unsigned rowAddress, uint32_t t)
{
    if (rowAddress < TILEMAP_BANK_BASE || rowAddress >= 0x10000) return false;
    return sTilemapBankRow[(rowAddress - TILEMAP_BANK_BASE) / 64] > t;
}

/* The bank (1 + bank) of the entry at VRAM entryAddress, read as `entry`: 0 for none. */
static unsigned TilemapBankAt(unsigned entryAddress, unsigned entry)
{
    unsigned at, bank;

    if (sTilemapBanked == 0 || entryAddress < TILEMAP_BANK_BASE || entryAddress >= 0x10000) return 0;
    at = (entryAddress - TILEMAP_BANK_BASE) / 2;
    bank = sTilemapBank[at];
    if (bank == 0 || sTilemapBankEntry[at] != entry || !(sTileBanksLive & (1u << (bank - 1)))) return 0;
    return bank;
}

static int GetTileSlot(unsigned address, unsigned bank, bool color256);

/* The slot of a 4bpp entry of bank `bank` (1 + bank), its tiles from chars. */
static int BankedTileSlot(unsigned bank, unsigned entry, unsigned chars)
{
    unsigned tile, palette, address;
    bool fromBank, bankPalette;

    CtrTileBank_Resolve(entry, &tile, &fromBank, &palette, &bankPalette);
    --bank;
    address = fromBank ? TILE_BANK_BASE + bank * CTR_TILE_BANK_BYTES + tile * 32 : chars + tile * 32;
    if (!fromBank && address >= 0x10000) return -1;
    return GetTileSlot(address, bankPalette ? PAL_TILE_BANK + bank * CTR_TILE_BANK_PALETTES + palette : palette,
                       false);
}
static bool sC3d, sC2d;
static uint64_t sFpsStart;
static unsigned sFpsFrames;
static uint32_t sReported;

/*
 * Current clip rectangle. With windows enabled the frame is composed as a
 * partition of rectangles and every layer is visited once per rectangle, so
 * geometry outside the current one must not be submitted at all: the scissor
 * would discard it after paying for it, which is what pushed a windowed
 * overworld frame to three times the draw calls it needs.
 */
static int sClipX0, sClipY0, sClipX1 = CTR_GAME_WIDTH, sClipY1 = CTR_GAME_HEIGHT;

static void ClipToView(void)
{
    sClipX0 = VIEW_LEFT;
    sClipY0 = VIEW_TOP;
    sClipX1 = VIEW_RIGHT;
    sClipY1 = VIEW_BOTTOM;
}

/* Per-frame time in the layer walk and in the sprites, reported with fps. */
static uint64_t sBgTicks, sObjTicks;

/*
 * Stereoscopic depth. The GBA already orders everything by priority, 0 nearest
 * and 3 furthest, so that is the depth scale: priority 3 stays at the screen
 * plane and the rest come forward, which puts interface windows in front of
 * the map and the sprites between the two.
 *
 * The frame is composed once per eye with each layer displaced by whole
 * pixels; a fractional displacement would resample the very pixel grid this
 * renderer exists to preserve. At the widest slider setting the nearest layer
 * separates by CTR_STEREO_PIXELS in each eye.
 */
#define CTR_STEREO_PIXELS 1.0f

/*
 * Stereoscopy of the voxel world (RenderVoxel): the world drawn once per eye
 * from its own camera (CtrVoxel_Draw, eye 0 and 1). The second pass costs
 * the GPU the world's fill again, which an Old 3DS does not have in a frame
 * (an earlier attempt ran at 40-45 fps there), so OPTIONS' 3D WORLD on AUTO
 * leaves the world flat on one, slider or not, and takes the slider on a New
 * 3DS (sVoxelStereo, set at start-up; -DCTR_VOXEL_STEREO=1 or =0 forces
 * AUTO either way). ON and OFF decide outright. 3D DEPTH scales the slider.
 */
static bool sVoxelStereo;
/* An Old 3DS (CtrVideo_Init): CtrBattle_StillSandstorm. */
static bool sOldModel;

static bool VoxelStereoWanted(void)
{
    int mode = CtrSettings_GetInt("voxel_stereo", 0);

    return mode == 1 || (mode == 0 && sVoxelStereo);
}

/* The slider as the voxel world takes it: scaled by OPTIONS' 3D DEPTH. */
static float VoxelStereoSlider(float slider)
{
    static const float depths[] = {0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f};
    int depth = CtrSettings_GetInt("voxel_depth", 3);

    if (depth < 0 || depth >= (int)(sizeof(depths) / sizeof(depths[0]))) depth = 3;
    return slider * depths[depth];
}

/* Displacement per depth unit for the eye being composed, and the resulting
 * displacement of the layer being drawn, both in whole screen pixels. */
static float sParallax, sLayerShift;
/* The 3D battle (RenderBattleStereo): sprites at their own depths, and the
 * tiles each background drew in the last composition. */
static bool sSpriteDepth;
static unsigned sBgDrawn[4];
static unsigned sStereoSplit[2];
/* Why a 3D battle frame was composed whole per eye: a sprite blending, one in
 * mosaic, a background in front of a sprite, no room for the slots. */
static unsigned sStereoWhy[4];
/* The same, for this frame alone (CTR_FLAT_*, the frame profiler's record). */
static uint8_t sFrameFlat;
/* Diagnostic: which kind of blend (BlendWhy). */
static unsigned sBlendWhy[6];
static void StereoWhy(unsigned why)
{
    ++sStereoWhy[why];
    sFrameFlat |= 1u << why;
}
static float BattleSpriteShift(int centre);
static void SceneOn(C3D_RenderTarget *target);
static float BattleSceneryShift(void);
static bool sObjMaskOn;
/* The OBJ window's own composition (ObjWindowCompose): its sprites as a
 * mask, then the layers it shows over that mask. */
enum { OBJWIN_NONE, OBJWIN_MASK, OBJWIN_LAYERS };
static unsigned sObjWinPass;
/* The battle's BG0 drew something above its text box (DrawBattleText). */
static bool sTextAbove;
static uint32_t sObjMask[4];
/*
 * The Wireless Adapter's status indicator: link_rfu puts it in OAM entry 125
 * itself, at the GBA screen's top right. In the 2D battle (RenderEye) it is
 * left out of the scene and drawn over the finished picture
 * (BattleIndicatorCompose): the lines above the scene's surface are filled
 * from its top rows, repeated, and its top rows came out again there as a
 * stray line. The 3D battle's surface has no sprites in it.
 */
#define WIRELESS_INDICATOR_OAM 125
static bool sObjIndicatorPass, sIndicatorApartHere;
static bool WirelessIndicatorApart(void);
/* Fixed horizontal placement of every layer, on top of the depth parallax.
 * Zero for the 2D compositor, which reproduces the GBA frame as it is. */
static float sLayerOrigin;
/* The voxel overlay's text and prompts in this eye, in whole pixels: in
 * front of the nearest world (ComposeVoxelOverlay). Zero everywhere else. */
static int sOverlayEyeShift;

/*
 * The text layer is a 32-column tilemap, so it only addresses the left 256px
 * of the 400px viewport, and widening it does not fit in background VRAM (see
 * the note on sStandardTextBox_WindowTemplates in src/menu.c). That is a
 * limit on where the *game* can put a window, not on where this compositor can
 * draw one: on the field BG0 carries nothing but windows, so it is drawn moved
 * to where it belongs. The standard text box spans x=16..232 within the band,
 * so its centre reaches the middle of the viewport at +76. The voxel overlay
 * does the same (ComposeVoxelOverlay); sFieldUi asks for it in the 2D field.
 */
#define CTR_FIELD_UI_SHIFT 76.0f
static bool sFieldUi;
/*
 * The banner of a field move (src/field_effect.c, the mon shown for Surf, Cut,
 * Fly...) is BG0 too, but a pattern of streaks meant to wrap across the whole
 * screen as on the GBA, not a window: while it is up BG0 is drawn over the
 * full width and not moved to the text band.
 */
static bool sFieldBanner;

void CtrVideo_SetFieldBanner(bool banner)
{
    sFieldBanner = banner;
}
/* The 2D field this frame: its text backgrounds are drawn from layer
 * textures kept up to date cell by cell (LayerRenderCells). */
static bool sFieldLayers;

/*
 * Composing the whole frame twice costs twice the CPU, which is a frame an Old
 * 3DS does not have. Instead each depth plane is composed once into its own
 * surface and the two eyes are then four textured quads each: the layer walk,
 * which is where the time goes, runs once per frame however many eyes there
 * are.
 *
 * A layer that blends with what is underneath it needs that underneath in its
 * own surface: its plane is given the picture behind it, colour only, before
 * its layers are drawn (RenderBands). The last plane holds every remaining
 * priority.
 */
/*
 * Three planes rather than one per priority: each one costs a surface to clear
 * and a textured quad per eye, and the overworld has no frame to spare for a
 * fourth. The two furthest priorities share the last plane, which still leaves
 * the interface, the near layer and the background at three depths.
 */
#define CTR_PRIORITIES 4
#define CTR_BANDS 3
static C3D_Tex sBandTex[CTR_BANDS];
static C3D_RenderTarget *sBand[CTR_BANDS];
/* How many of them exist: two when the third gave its VRAM to a layer. */
static unsigned sBandCount;
static bool sBandsFailed;
/* A failed allocation is tried again this many frames later, not never: the
 * VRAM it needs may have been in use by the overworld only for a while. */
#define CTR_BANDS_RETRY_FRAMES 300u
static uint32_t sBandsRetryFrame;
/* Whether each plane drew anything; an empty one is not worth a quad. */
static bool sBandUsed[CTR_BANDS];
/* Depth planes the last frame used, 0 when it was composed per eye. */
static unsigned sPlanes;
/*
 * What the current composition pass may draw, by depth slot: bit 2p is the
 * sprites of priority p, bit 2p+1 its backgrounds - front to back in that
 * order, as the GBA stacks them.
 */
#define SLOTS_ALL 255u
#define SLOT_OBJ(p) (1u << ((p) * 2))
#define SLOT_BG(p) (2u << ((p) * 2))
static unsigned sPriorityMask = SLOTS_ALL;

/*
 * Last blend configuration submitted. Reset whenever something else may have
 * changed the GPU state, which is once per eye pass.
 */
static unsigned sBlendKey = ~0u;
static void BlendForget(void) { sBlendKey = ~0u; }

/*
 * The 2D compositor draws everything at depth 0, in submission order, so a
 * depth test can only ever pass. Citro2D still enables one (GEQUAL, writing
 * depth): every pixel of every layer read and wrote the 16-bit depth of the
 * logical surface and of the screens - a third more memory traffic per pixel
 * for nothing, and the 3D slider's planes multiply the pixels. Off, the
 * picture is the same.
 */
static void Flat2D(void)
{
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
}

/* C2D_TargetClear without the depth buffer, which the 2D frame never reads. */
static void ColourClear(C3D_RenderTarget *target, uint32_t color)
{
    C2D_Flush();
    C3D_FrameSplit(0);
    C3D_RenderTargetClear(target, C3D_CLEAR_COLOR, __builtin_bswap32(color), 0);
}

/*
 * A battle transition's frame, line by line (CtrVideo_SetLineRegisters): the
 * registers each of its 160 lines shows, and while a band of those lines is
 * composed, that band's values in place of the registers (Reg).
 */
static bool sTransitionRequested, sTransition;
/* Composing the transition's own picture: GBA geometry (RenderTransition). */
static bool sTransitionCompose;
static const uint16_t (*sLineRegs)[CTR_LINE_REGS];
static const uint16_t *sRegLine;

static unsigned Reg(unsigned offset)
{
    if (sCentredScreen == CTR_CENTRED_POKEDEX_CAUGHT && offset >= 0x18 && offset <= 0x1e)
        return 0;
    if (sRegLine && offset - CTR_LINE_REG_FIRST < CTR_LINE_REGS * 2)
        return sRegLine[(offset - CTR_LINE_REG_FIRST) / 2];
    return sMemory.regs[offset / 2];
}
static unsigned Min(unsigned a, unsigned b) { return a < b ? a : b; }

static void Error(unsigned bit, const char *reason)
{
    if (!(sReported & (1u << bit)))
    {
        sReported |= 1u << bit;
        ++sStats.errors;
        CtrLog_Write(CTR_LOG_ERROR, "VIDEO: %s", reason);
    }
}

static unsigned Read16(unsigned offset)
{
    if (offset >= 0x10000)
    {
        Error(0, "BG tilemap outside BG VRAM");
        return 0;
    }
    return sMemory.vram[offset] | (sMemory.vram[offset + 1] << 8);
}

/*
 * A palette fade as a tint. The game fades by rewriting every colour each
 * frame (BlendPalette, src/util.c), and every colour of a layer texture
 * changing is every cell of it redrawn: the intro's four 256x512 layers,
 * 8192 tiles decoded and drawn a frame, 30 ms on an Old 3DS for as long as a
 * fade lasts. When the backgrounds' palette is exactly the unfaded one faded
 * towards one colour - black, white, the grey a battle transition flashes
 * (BlendPalettes towards RGB(11, 11, 11)) - the tiles keep the unfaded
 * colours and the fade is
 * drawn as a tint on the layers (Blend, LayerBrightness), which costs
 * nothing. Only an exact match counts, so any other palette effect is drawn
 * as before. Sprites keep the faded colours: they are a few tiles.
 */
extern uint16_t gPlttBufferUnfaded[];
static float sPaletteFade;
/* The colour it fades towards, GBA BGR555. */
static uint16_t sPaletteFadeColor;
/* Set while a layer texture is drawn: it holds the unfaded colours. */
static bool sInLayerTexture;

static uint16_t FadeColor(unsigned color, unsigned to, unsigned y)
{
    int r = color & 31, g = (color >> 5) & 31, b = (color >> 10) & 31;
    int tr = to & 31, tg = (to >> 5) & 31, tb = (to >> 10) & 31;

    r += ((tr - r) * (int)y) >> 4;
    g += ((tg - g) * (int)y) >> 4;
    b += ((tb - b) * (int)y) >> 4;
    return (uint16_t)(r | g << 5 | b << 10);
}

/*
 * The value (0-31) one channel of every background colour is faded towards
 * by y, or -1. Tried value by value: a wrong one fails on the first colours,
 * so this is cheap unless it matches.
 */
static int FadeChannelTarget(const uint16_t *now, const uint16_t *unfaded, unsigned shift, unsigned y)
{
    for (int to = 0; to < 32; ++to)
    {
        unsigned i = 0;

        for (; i < 256; ++i)
        {
            int u = (unfaded[i] >> shift) & 31, f = (now[i] >> shift) & 31;
            if (u + (((to - u) * (int)y) >> 4) != f) break;
        }
        if (i == 256) return to;
    }
    return -1;
}

/* The coefficient (1-16) the background colours are faded by, or 0. */
static unsigned DetectPaletteFade(uint16_t *color)
{
    const uint16_t *now = sMemory.palette, *unfaded = gPlttBufferUnfaded;

    if (!memcmp(now, unfaded, 512)) return 0;
    for (unsigned w = 0; w < 2; ++w)
        for (unsigned y = 1; y <= 16; ++y)
        {
            unsigned i = 0;

            while (i < 256 && FadeColor(unfaded[i], w ? 0x7fff : 0, y) == (now[i] & 0x7fff)) ++i;
            if (i == 256)
            {
                *color = w ? 0x7fff : 0;
                return y;
            }
        }
    /* Any other colour, channel by channel: the transitions' grey flash. */
    for (unsigned y = 1; y <= 16; ++y)
    {
        int r = FadeChannelTarget(now, unfaded, 0, y), g, b;

        if (r < 0 || (g = FadeChannelTarget(now, unfaded, 5, y)) < 0
            || (b = FadeChannelTarget(now, unfaded, 10, y)) < 0)
            continue;
        *color = (uint16_t)(r | g << 5 | b << 10);
        return y;
    }
    return 0;
}

/*
 * The banks' palettes as the current map's are shown (3ds_tile_banks.h): in a
 * uniform fade, which the renderer applies itself to colours decoded from the
 * unfaded palettes, their own unfaded colours; otherwise their colours taken
 * channel by channel through what took the current map's palettes 0-12 from
 * unfaded to shown - nothing, most of the time.
 */
static void UpdateTileBankPalettes(bool fading)
{
    const uint16_t *shown = sMemory.palette, *unfaded = gPlttBufferUnfaded;
    unsigned count = (CTR_TILE_BANK_FIRST_PALETTE + CTR_TILE_BANK_PALETTES) * 16;
    bool same = fading || !memcmp(shown, unfaded, count * sizeof(uint16_t));
    uint8_t lut[3][32];

    if (!same)
        CtrTileBank_BuildLut(unfaded, shown, count, lut);
    for (unsigned b = 0; b < CTR_TILE_BANKS; ++b)
    {
        if (!(sTileBanksLive & (1u << b))) continue;
        for (unsigned p = 0; p < CTR_TILE_BANK_PALETTES; ++p)
        {
            unsigned id = PAL_TILE_BANK + b * CTR_TILE_BANK_PALETTES + p;
            unsigned base = PAL_TILE_BANK_COLOURS + (b * CTR_TILE_BANK_PALETTES + p) * 16;
            const uint16_t *source = sTileBankPalettes[b] + p * 16;
            bool changed = false;

            for (unsigned index = 0; index < 16; ++index)
            {
                uint16_t colour = same ? source[index] & 0x7fff : CtrTileBank_ApplyLut(lut, source[index]);

                if (sPalette[base + index] == colour) continue;
                if (!changed)
                    memset(sPaletteChanges[id], 0, sizeof(sPaletteChanges[id]));
                changed = true;
                sPalette[base + index] = colour;
                sTexturePalette[base + index] = CtrVideo_RGBA5551(colour);
                sPaletteChanges[id][0] |= 1u << index;
            }
            if (changed)
            {
                ++sPaletteVersion[id];
                sBgPaletteStamp = sStats.frames + 1;
            }
        }
    }
}

/*
 * A background colour drawn as transparent: the roulette paints the table's
 * colour (BG palette 5, colour 1) round its wheel and its grid, so it went on
 * the backdrop the Game Corner's felt replaces (CornerUnderlay) as flat
 * patches. -1 for none.
 */
static int sKeyedEntry = -1;

static void UpdatePalette(void)
{
    bool bgChanged = false, objChanged = false;
    unsigned fade = DetectPaletteFade(&sPaletteFadeColor);
    const uint16_t *bgSource = fade ? gPlttBufferUnfaded : sMemory.palette;
    int keyed = sCentred && sCentredScreen == CTR_CENTRED_ROULETTE ? 5 * 16 + 1 : -1;

    sPaletteFade = fade / 16.0f;
    if (keyed != sKeyedEntry)
    {
        /* Converted again below, with or without its colour. */
        if (sKeyedEntry >= 0) sPalette[sKeyedEntry] = ~bgSource[sKeyedEntry];
        if (keyed >= 0) sPalette[keyed] = ~bgSource[keyed];
        sKeyedEntry = keyed;
    }
    for (unsigned bank = 0; bank < 32; ++bank)
    {
        const uint16_t *source = bank < 16 ? bgSource : sMemory.palette;

        if (memcmp(sPalette + bank * 16, source + bank * 16, 32))
        {
            unsigned group = bank < 16 ? PAL_BG256 : PAL_OBJ256;
            if (bank < 16 ? !bgChanged : !objChanged)
                memset(sPaletteChanges[group], 0, sizeof(sPaletteChanges[group]));
            memset(sPaletteChanges[bank], 0, sizeof(sPaletteChanges[bank]));
            for (unsigned index = 0; index < 16; ++index)
            {
                unsigned p = bank * 16 + index;
                if (sPalette[p] == source[p]) continue;
                sPalette[p] = source[p];
                sTexturePalette[p] = (int)p == sKeyedEntry ? 0 : CtrVideo_RGBA5551(sPalette[p]);
                sPaletteChanges[bank][0] |= 1u << index;
                unsigned entry = p & 255;
                sPaletteChanges[group][entry / 32] |= 1u << (entry & 31);
            }
            ++sPaletteVersion[bank];
            if (bank < 16) bgChanged = true; else objChanged = true;
        }
    }
    if (bgChanged)
    {
        ++sPaletteVersion[PAL_BG256];
        sBgPaletteStamp = sStats.frames + 1;
    }
    if (objChanged) ++sPaletteVersion[PAL_OBJ256];
    /* The ext banks in use: no 256-colour bank holds them. */
    for (unsigned live = sExtInUse, ext = 0; live; live >>= 1, ++ext)
    {
        unsigned bank = PAL_EXT + ext, base = 512 + ext * 16;
        const uint16_t *source = sExtPltt + ext * 16;

        if (!(live & 1) || !memcmp(sPalette + base, source, 32)) continue;
        memset(sPaletteChanges[bank], 0, sizeof(sPaletteChanges[bank]));
        for (unsigned index = 0; index < 16; ++index)
        {
            if (sPalette[base + index] == source[index]) continue;
            sPalette[base + index] = source[index];
            sTexturePalette[base + index] = CtrVideo_RGBA5551(source[index]);
            sPaletteChanges[bank][0] |= 1u << index;
        }
        ++sPaletteVersion[bank];
    }
    if (sTileBanksLive && (bgChanged || sTileBanksChanged))
        UpdateTileBankPalettes(fade != 0);
    sTileBanksChanged = false;
}

/*
 * Why tiles were decoded into the atlas (phase F1's gate: a still scene
 * decodes nothing, and nothing is decoded that comes out as it was): new to
 * the cache or evicted, its bytes changed, or its palette. Totals per segment
 * of the profile (CtrVideo_DecodeCounts), with the decodes whose texels came
 * out identical to the slot's: those change nothing downstream.
 */
static uint32_t sDecodeNew, sDecodeBytes, sDecodePalette, sDecodeSame;
static uint32_t sDecodeFrames, sCellFrames, sDecodeFramesTiles, sCellsTotal;

void CtrVideo_DecodeCounts(uint32_t counts[8])
{
    counts[0] = sDecodeNew;
    counts[1] = sDecodeBytes;
    counts[2] = sDecodePalette;
    counts[3] = sDecodeSame;
    counts[4] = sDecodeFrames;
    counts[5] = sCellFrames;
    counts[6] = sCellsTotal;
    counts[7] = sDecodeFramesTiles;
    sDecodeNew = sDecodeBytes = sDecodePalette = sDecodeSame = 0;
    sDecodeFrames = sCellFrames = sCellsTotal = sDecodeFramesTiles = 0;
}

static bool TilePaletteChanged(const Tile *tile, unsigned paletteId)
{
    uint32_t version = sPaletteVersion[paletteId];
    if (tile->paletteVersion == version) return false;
    /* The last change mask is sufficient only for the next version. Tiles
     * returning after multiple palette changes are conservatively rebuilt. */
    if (tile->paletteVersion + 1 != version) return true;
    bool color256 = paletteId == PAL_BG256 || paletteId == PAL_OBJ256;

    for (unsigned i = 0; i < (color256 ? 8u : 1u); ++i)
        if (tile->colors[i] & sPaletteChanges[paletteId][i]) return true;
    return false;
}

/*
 * Resolves a tile to its atlas slot, uploading it if its bytes or its palette
 * changed. Returns -1 for a tile that is entirely transparent, which draws
 * nothing. The caller builds the subtexture: a slot is a fixed square of the
 * atlas, so that is four multiplications and no shared state.
 */
static int GetTileSlot(unsigned address, unsigned bank, bool color256)
{
    bool fromBank = address >= TILE_BANK_BASE;

    if (fromBank ? color256 || address + 32 > TILE_BANK_BASE + CTR_TILE_BANKS * CTR_TILE_BANK_BYTES
                 : address + (color256 ? 64 : 32) > 0x18000)
    {
        Error(1, "tile outside logical VRAM");
        address = 0;
        fromBank = false;
    }
    /* A bank's tiles stand in for VRAM past its end (sTileBankTiles). */
    const uint8_t *source = fromBank
        ? sTileBankTiles[(address - TILE_BANK_BASE) / CTR_TILE_BANK_BYTES] + (address - TILE_BANK_BASE) % CTR_TILE_BANK_BYTES
        : sMemory.vram + address;
    unsigned paletteId = color256 ? PAL_BG256 + (bank >= 16) : bank;
    uint32_t key = (address / 32) * PAL_IDS + paletteId + 1;
    unsigned hash = (key * 2654435761u) & (HASH_COUNT - 1);
    while (sHash[hash] && sTiles[sHash[hash] - 1].key != key)
        hash = (hash + 1) & (HASH_COUNT - 1);
    if (!sHash[hash])
    {
        if (sUsed == CACHE_COUNT)
            CtrPlatform_Fatal("VIDEO tile cache exhausted in a single frame");
        sHash[hash] = ++sUsed;
        sTiles[sUsed - 1] = (Tile){.key = key};
    }
    unsigned slot = sHash[hash] - 1;
    Tile *tile = &sTiles[slot];
    if (!tile->valid || tile->checked != sStats.frames + 1)
    {
        unsigned bytes = color256 ? 64 : 32;
        /* Compared now whatever follows: the tile records this token, so its
         * block must be (see SyncBlock). */
        uint32_t stamp = fromBank ? sTileBankStamp[(address - TILE_BANK_BASE) / CTR_TILE_BANK_BYTES]
                                  : SyncBlock(address / VRAM_BLOCK);
        /* A tile's bytes sit in one block: unchanged since it was last
         * checked, they need no comparing. */
        bool fresh = !tile->valid, palette = !fresh && TilePaletteChanged(tile, paletteId);

        if (fresh || palette
            || (stamp > tile->checked
                && memcmp(tile->bytes, source, bytes)))
        {
            if (fresh) ++sDecodeNew;
            else if (palette) ++sDecodePalette;
            else ++sDecodeBytes;
            memcpy(tile->bytes, source, bytes);
            unsigned paletteBase = color256 ? (bank >= 16 ? 256 : 0)
                                 : bank >= PAL_TILE_BANK ? PAL_TILE_BANK_COLOURS + (bank - PAL_TILE_BANK) * 16
                                 : bank >= PAL_EXT ? 512 + (bank - PAL_EXT) * 16 : bank * 16;
            uint16_t *dest = (uint16_t *)sAtlas.data + slot * 64;
            uint16_t before[64];

            /* What the slot held: a decode that comes out the same - a
             * palette changed and changed back, the cache's conservative
             * rebuild - keeps the tile's serial, so no layer cell holding it
             * is drawn again. */
            if (!fresh)
                memcpy(before, dest, sizeof(before));
            memset(tile->colors, 0, sizeof(tile->colors));
            if (!color256 && sMortonPairs)
            {
                /*
                 * Hundreds of these a frame when a map's tiles change (a
                 * warp, a connection to another tileset): the 16 colours in
                 * hand with index 0 clear, a byte's two pixels as one word,
                 * and the colours used as one mask.
                 */
                const uint16_t *source = sTexturePalette + paletteBase;
                uint32_t lut[16], used = 0;
                uint32_t *dest32 = (uint32_t *)dest;

                lut[0] = 0;
                for (unsigned i = 1; i < 16; ++i)
                    lut[i] = source[i];
                for (unsigned pair = 0; pair < 32; ++pair)
                {
                    unsigned byte = tile->bytes[pair], lo = byte & 15, hi = byte >> 4;

                    dest32[sMortonPair[pair]] = lut[lo] | lut[hi] << 16;
                    used |= (1u << lo) | (1u << hi);
                }
                used &= ~1u;
                tile->colors[0] = used;
                tile->visible = used != 0;
            }
            else
            {
                tile->visible = false;
                for (unsigned pixel = 0; pixel < 64; ++pixel)
                {
                    unsigned index = color256 ? tile->bytes[pixel]
                        : (tile->bytes[pixel / 2] >> ((pixel & 1) * 4)) & 15;
                    dest[sMorton[pixel]] = index ? sTexturePalette[paletteBase + index] : 0;
                    if (index)
                    {
                        tile->visible = true;
                        tile->colors[index / 32] |= 1u << (index & 31);
                    }
                }
            }
            /* C3D_FrameEnd(0) flushes linear memory once before submitting
             * the queue, including this atlas and Citro2D's geometry. A
             * separate GSP service call per tile costs hundreds of calls
             * per palette-animation frame without improving visibility. */
            tile->valid = true;
            if (!fresh && memcmp(before, dest, sizeof(before)) == 0)
                ++sDecodeSame;
            else
            {
                tile->serial = ++sTileSerial;
                ++sStats.uploads;
            }
        }
        tile->paletteVersion = sPaletteVersion[paletteId];
        tile->checked = sStats.frames + 1;
    }
    return tile->visible ? (int)slot : -1;
}

static void DrawSlotTinted(int slot, float x, float y, bool flipX, bool flipY, const C2D_ImageTint *tint)
{
    unsigned tileX = ((unsigned)slot % (ATLAS_SIZE / 8)) * 8;
    unsigned tileY = ((unsigned)slot / (ATLAS_SIZE / 8)) * 8;
    Tex3DS_SubTexture sub = {8, 8, tileX / (float)ATLAS_SIZE,
        1.0f - tileY / (float)ATLAS_SIZE,
        (tileX + 8) / (float)ATLAS_SIZE, 1.0f - (tileY + 8) / (float)ATLAS_SIZE};

    if (++sStats.tiles >= MAX_DRAWS - 32)
    {
        Error(2, "GPU geometry budget exceeded");
        return;
    }
    if (!C2D_DrawImageAt((C2D_Image){&sAtlas, &sub}, x, y, 0, tint,
                         flipX ? -1 : 1, flipY ? -1 : 1))
        Error(3, "Citro2D geometry submission failed");
}

static void DrawSlot(int slot, float x, float y, bool flipX, bool flipY)
{
    DrawSlotTinted(slot, x, y, flipX, flipY, &sTint);
}

static bool sDayNightObject;
static bool sDayNightEnv;
/*
 * A background that blends, in front of a group of sprites, drawn into the
 * group's slot over its sprites (SlotsCompose): the GBA's EVA x layer + EVB x
 * sprite, in two passes of the layer alone. SLOT_BLEND_UNDER scales what the
 * slot holds under the layer's pixels by EVB; SLOT_BLEND_OVER adds EVA x the
 * layer where the slot holds a sprite (its alpha, one bit), nothing where it
 * holds nothing - the scenery under the slot has the layer blended already.
 */
enum { SLOT_BLEND_NONE, SLOT_BLEND_UNDER, SLOT_BLEND_OVER };
static unsigned sSlotBlendPass;

static void Blend(unsigned layer, bool effects, bool semiTransparent)
{
    /*
     * This flushes the batch and rewrites the blend state, so it costs a GPU
     * draw call. The registers it reads are fixed for the frame, and nothing
     * between two calls touches the blend state, so asking for the same
     * configuration again is a no-op: an overworld frame with forty sprites
     * asked for it forty times.
     */
    const CtrDayNightLight *light = CtrDayNight_Get();
    bool night = (light->rgb[0] != 1.0f || light->rgb[1] != 1.0f || light->rgb[2] != 1.0f)
              && !sInLayerTexture && !sStage && !sCentred && !sBattle && CtrGame_IsOverworld()
              && ((layer >= 1 && layer <= 3) || (layer == 4 && sDayNightObject));
    unsigned pass = sInLayerTexture ? SLOT_BLEND_NONE : sSlotBlendPass;
    unsigned key = layer | (effects << 8) | (semiTransparent << 9) | (night << 10) | (pass << 11);

    if (key == sBlendKey) return;
    sBlendKey = key;

    C2D_Flush();
    if (sDayNightEnv)
    {
        C3D_TexEnvInit(C3D_GetTexEnv(4));
        C3D_TexEnvInit(C3D_GetTexEnv(5));
        sDayNightEnv = false;
    }
    C3D_AlphaTest(true, GPU_GREATER, 0);
    if (pass != SLOT_BLEND_NONE)
    {
        unsigned control = Reg(0x50);
        bool blends = effects && ((control >> 6) & 3) == 1 && (control & (1u << layer));
        unsigned eva = Min(Reg(0x52) & 31, 16), evb = Min((Reg(0x52) >> 8) & 31, 16);

        C2D_PlainImageTint(&sTint, C2D_Color32(0, 0, 0, 255), pass == SLOT_BLEND_OVER ? 1.0f - eva / 16.0f : 0.0f);
        if (!blends)
        {
            /* Where the window turns effects off, the layer as it is. */
            if (pass == SLOT_BLEND_UNDER)
                C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ZERO, GPU_ONE);
            else
                C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ZERO, GPU_ONE, GPU_ZERO, GPU_ONE);
        }
        else if (pass == SLOT_BLEND_UNDER)
        {
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ZERO, GPU_CONSTANT_COLOR, GPU_ZERO, GPU_ONE);
            C3D_BlendingColor(C2D_Color32(evb * 255 / 16, evb * 255 / 16, evb * 255 / 16, 255));
        }
        else
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_DST_ALPHA, GPU_ONE, GPU_ZERO, GPU_ONE);
        return;
    }
    if (sObjWinPass != OBJWIN_NONE)
    {
        /* The OBJ window's surface (ObjWindowCompose) keeps a coverage
         * count in alpha: half from the window's sprites, written alone
         * and the same however many overlap, then doubled by whatever a
         * layer draws. Only both together reach the threshold the surface
         * is drawn with (ObjWindowDraw). Colours as they are, untinted. */
        C2D_PlainImageTint(&sTint, C2D_Color32(0, 0, 0, 255), 0.0f);
        if (sObjWinPass == OBJWIN_MASK)
        {
            C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALPHA);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ZERO, GPU_ONE, GPU_CONSTANT_ALPHA, GPU_ZERO);
            C3D_BlendingColor(C2D_Color32(0, 0, 0, 0x80));
        }
        else
        {
            C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_DST_ALPHA, GPU_ONE);
        }
        return;
    }
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA,
                  GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ZERO);
    unsigned control = Reg(0x50), effect = (control >> 6) & 3;
    /* The palette fade (UpdatePalette) is on a background whatever the
     * windows say; the brightness effect of BLDY replaces it below. */
    bool paletteTint = sPaletteFade > 0 && !sInLayerTexture && (layer < 4 || layer == 5);
    uint32_t fadeTo = CtrVideo_RGBA8(sPaletteFadeColor, true);

    C2D_PlainImageTint(&sTint, C2D_Color32(fadeTo >> 24, fadeTo >> 16, fadeTo >> 8, 255),
                       paletteTint ? sPaletteFade : 0);
    if (night)
    {
        C3D_TexEnv *env = C3D_GetTexEnv(4);

        C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_CONSTANT);
        C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
        C3D_TexEnvColor(env, C2D_Color32((unsigned)(light->rgb[0] * 255 + 0.5f),
                                       (unsigned)(light->rgb[1] * 255 + 0.5f),
                                       (unsigned)(light->rgb[2] * 255 + 0.5f), 255));
        float fade = paletteTint ? sPaletteFade : 0.0f;
        uint32_t color = C2D_Color32(fadeTo >> 24, fadeTo >> 16, fadeTo >> 8, (unsigned)(fade * 255));

        if (effects && (control & (1u << layer)) && effect >= 2)
        {
            unsigned channel = effect == 2 ? 255 : 0;

            fade = Min(Reg(0x54), 16) / 16.0f;
            color = C2D_Color32(channel, channel, channel, (unsigned)(fade * 255));
        }
        float fadeRgb[3] = {(color & 255u) / 255.0f, ((color >> 8) & 255u) / 255.0f,
                            ((color >> 16) & 255u) / 255.0f};
        CtrDayNightLight moonLight = *light;

        if (!paletteTint)
            moonLight.glow *= 1.0f - sPaletteFade;
        CtrDayNightBlend moon = CtrDayNight_Moonlight(&moonLight, fade, fadeRgb);

        color = C2D_Color32((unsigned)(moon.rgb[0] * 255.0f + 0.5f),
                           (unsigned)(moon.rgb[1] * 255.0f + 0.5f),
                           (unsigned)(moon.rgb[2] * 255.0f + 0.5f),
                           (unsigned)(moon.amount * 255.0f + 0.5f));
        env = C3D_GetTexEnv(5);
        C3D_TexEnvSrc(env, C3D_RGB, GPU_CONSTANT, GPU_PREVIOUS, GPU_CONSTANT);
        C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
        C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
        C3D_TexEnvColor(env, color);
        sDayNightEnv = true;
        C2D_PlainImageTint(&sTint, C2D_Color32(0, 0, 0, 255), 0);
    }
    /* A semi-transparent sprite blends even where the window turns colour
     * effects off: the overworld keeps WIN0 over the whole screen without
     * them, and its fog and clouds are still see-through on the GBA. */
    if (!effects && !semiTransparent) return;
    if ((effect == 1 && (control & (1u << layer))) || semiTransparent)
    {
        /* GPU implements independently clamped EVA/EVB (not 1-EVA).
         * Valid for the title's BG1 over BG0/backdrop; arbitrary interleaved
         * target-2 masks are a phase-6 per-pixel effect, reported below. */
        if ((control >> 8) & 63)
        {
            unsigned eva = Min(Reg(0x52) & 31, 16) * 255 / 16;
            unsigned evb = Min((Reg(0x52) >> 8) & 31, 16) * 255 / 16;
            /*
             * Over the voxel world the layers it stands in for are left out,
             * so where nothing else of the scene is under this one the
             * blend's second target is transparent black, and the scene
             * surface keeps one bit of alpha: Surf's wave at its start
             * (EVA 0-1, EVB 16) came out as a black wave over the world
             * where the GBA shows the terrain all but untouched. The alpha
             * rounded instead: while this layer is the lesser part of the
             * blend, what it is drawn over keeps its alpha - the world shows
             * through, as the terrain would; from half on, as before.
             */
            bool underWorld = sBattleWorld && eva < evb;

            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_CONSTANT_COLOR,
                          GPU_CONSTANT_ALPHA, underWorld ? GPU_ZERO : GPU_ONE, underWorld ? GPU_ONE : GPU_ZERO);
            C3D_BlendingColor(C2D_Color32(eva, eva, eva, evb));
        }
    }
    else if ((control & (1u << layer)) && effect >= 2)
    {
        unsigned c = effect == 2 ? 255 : 0;
        if (!night)
            C2D_PlainImageTint(&sTint, C2D_Color32(c, c, c, 255), Min(Reg(0x54), 16) / 16.0f);
    }
}

static void DrawTile(unsigned address, unsigned bank, bool color256,
                     float x, float y, bool flipX, bool flipY)
{
    int slot = GetTileSlot(address, bank, color256);
    if (slot >= 0) DrawSlot(slot, x, y, flipX, flipY);
}

/*
 * Walks the tiles of a text background that cover [left, right) x [top, bottom)
 * of the screen. With a mirror axis (given doubled, -1 for none) each tile is
 * drawn reflected about it instead, flipped, so the region it lands on becomes
 * the mirror image of the one walked.
 */
/* Extra horizontal displacement of what DrawTextSpanAt draws, for a band that
 * is moved as a whole (the battle text box, DrawBattleText). */
static int sSpanShiftX, sSpanShiftY;

/* The base transform of everything composed: the current zoom. */
static void ViewBase(void)
{
    C2D_ViewReset();
    if (sZoom != 1.0f)
    {
        C2D_ViewTranslate(sOffX, sOffY);
        C2D_ViewScale(sZoom, sZoom);
    }
}

/* An affine layer's or sprite's own matrix, on top of the zoom. */
static void ViewAffine(C3D_Mtx *matrix)
{
    for (unsigned r = 0; r < 2; ++r)
        matrix->r[r] = FVec4_Scale(matrix->r[r], sZoom);
    matrix->r[0].w += sOffX;
    matrix->r[1].w += sOffY;
    C2D_ViewRestore(matrix);
}

/* The cells' and sprites' own vertex buffer (FastInit). */
typedef struct { int16_t x, y; float s, t; } FastVertex;
static FastVertex *sFastVertices;
static uint16_t *sFastIndices;
static unsigned sFastUsed;
static bool sFastReady;
static bool SpriteBatchRoom(unsigned tiles);
static void SpriteBatchFlush(void);
static void FastQuad(FastVertex *v, int x, int y, int slot, bool flipX, bool flipY);
static unsigned sSpriteCount;

/* The same tint at the four corners: what a batch can carry. */
static bool TintUniform(const C2D_ImageTint *tint)
{
    for (unsigned c = 1; c < 4; ++c)
        if (tint->corners[c].color != tint->corners[0].color || tint->corners[c].blend != tint->corners[0].blend)
            return false;
    return true;
}

/*
 * A walk's tiles in one draw, as sprites are (SpriteBatchFlush): the battle
 * scene's layers were ~1000 Citro2D quads a frame. The quads are in the
 * coordinates DrawSlot takes - Citro2D's view (ViewBase: a composition's zoom
 * and offset) is a shader uniform the GPU applies to both - so whole pixels
 * there batch, at any zoom. The tiles of one walk never overlap, so the few
 * left to Citro2D may be drawn in any order with them. TileWalkBegin says
 * whether this walk can batch at all; TileWalkSlot draws one tile;
 * TileWalkEnd sends them.
 */
static bool TileWalkBegin(void)
{
    return sFastReady && TintUniform(&sTint);
}

static void TileWalkSlot(bool batch, int slot, float x, float y, bool flipX, bool flipY)
{
    if (batch && (float)(int)x == x && (float)(int)y == y && SpriteBatchRoom(1))
    {
        if (++sStats.tiles >= MAX_DRAWS - 32)
        {
            Error(2, "GPU geometry budget exceeded");
            return;
        }
        FastQuad(sFastVertices + sFastUsed * 4, (int)x, (int)y, slot, flipX, flipY);
        ++sFastUsed;
        ++sSpriteCount;
        return;
    }
    DrawSlot(slot, x, y, flipX, flipY);
}

static void TileWalkEnd(bool batch)
{
    if (batch)
        SpriteBatchFlush();
}

static bool VramChangedAfter(unsigned address, unsigned bytes, uint32_t t);

/*
 * A walk that drew nothing - the field's text layer, most of the time - is
 * not walked again while nothing it read can have changed: the same layer,
 * scroll, rectangle and placement, and no change since to its tilemap, its
 * tiles or the background palettes. 1500 entries a frame for nothing was a
 * fifth of the voxel overlay.
 */
typedef struct
{
    bool valid;
    unsigned control, scrollX, scrollY;
    int left, right, top, bottom, mirrorX2, mirrorY2, spanX, spanY;
    float shift, zoom, offX, offY;
    uint32_t token;
} EmptyWalk;
static EmptyWalk sEmptyWalk[4];

static bool EmptyWalkHolds(const EmptyWalk *w, const EmptyWalk *now)
{
    unsigned control = now->control, size = control >> 14;
    unsigned map = ((control >> 8) & 31) * 0x800, chars = ((control >> 2) & 3) * 0x4000;
    unsigned mapBytes = 0x800 << ((size & 1) + ((size >> 1) & 1));
    unsigned charBytes = (control & 128) ? 0x10000 : 0x8000;

    if (!w->valid || w->control != now->control || w->scrollX != now->scrollX || w->scrollY != now->scrollY
        || w->left != now->left || w->right != now->right || w->top != now->top || w->bottom != now->bottom
        || w->mirrorX2 != now->mirrorX2 || w->mirrorY2 != now->mirrorY2 || w->spanX != now->spanX
        || w->spanY != now->spanY || w->shift != now->shift || w->zoom != now->zoom || w->offX != now->offX
        || w->offY != now->offY)
        return false;
    if (chars + charBytes > 0x10000) charBytes = 0x10000 - chars;
    return sBgPaletteStamp <= w->token && sTilemapBankStamp <= w->token
        && !VramChangedAfter(map, mapBytes, w->token) && !VramChangedAfter(chars, charBytes, w->token);
}

static void DrawTextSpanAt(unsigned bg, int left, int right, int top, int bottom,
                           int mirrorX2, int mirrorY2)
{
    unsigned control = Reg(8 + bg * 2), size = control >> 14;
    unsigned map = ((control >> 8) & 31) * 0x800;
    unsigned chars = ((control >> 2) & 3) * 0x4000;
    bool color256 = (control & 128) != 0;
    unsigned scrollX = Reg(0x10 + bg * 4) & 511, scrollY = Reg(0x12 + bg * 4) & 511;

    /*
     * A tilemap entry repeats across the layer: the field overlays are mostly
     * one blank entry, and a viewport of 400x240 walks 1500 of them per layer
     * per frame. This memo turns a repeat into one comparison instead of a
     * hash probe and a revalidation, which is what a 60fps overworld needs on
     * an Old 3DS. The stamp makes it valid for this call only, so every entry
     * is still revalidated once per layer per frame.
     */
    static uint32_t sMemoStamp[1024];
    static uint16_t sMemoEntry[1024];
    static int16_t sMemoSlot[1024];
    static uint32_t sStamp;
    uint32_t stamp = ++sStamp;
    bool batch;
    uint32_t tilesBefore = sStats.tiles;
    EmptyWalk key = {true, control, scrollX, scrollY, left, right, top, bottom, mirrorX2, mirrorY2,
                     sSpanShiftX, sSpanShiftY, sLayerShift, sZoom, sOffX, sOffY,
                     /* Conservative: a block changed later this frame is a change next frame. */
                     sStats.frames};
    /* Not in a battle: its text box is walked in several cuts a frame, and a
     * cut never meets the last one's key. */
    bool cached = bg < 4 && !sBattle;
    bool empty = cached && EmptyWalkHolds(&sEmptyWalk[bg], &key);

    if (empty)
        return;
    /* The key below records this frame's token for the tilemap, so its
     * blocks are compared now (the contract above): Read16 does not, and a
     * map changed and changed back unseen would keep a stale "empty". */
    if (cached)
        VramSyncRange(map, 0x800u << ((size & 1) + ((size >> 1) & 1)));
    batch = TileWalkBegin();

    ViewBase();
    for (int y = top / 8 - 1; y * 8 <= bottom; ++y)
    {
        int py = y * 8 - (int)(scrollY & 7);
        unsigned row = (unsigned)(y + (int)(scrollY / 8));
        unsigned rowBase;

        if (py + 8 <= top || py >= bottom) continue;
        rowBase = map + CtrVideo_TextMapOffset(0, row, size);
        for (int x = left / 8 - 1; x * 8 <= right; ++x)
        {
            int px = x * 8 - (int)(scrollX & 7);
            unsigned column = (unsigned)(x + (int)(scrollX / 8)) & ((size & 1) ? 63u : 31u);
            unsigned entry, index;
            int slot;

            /* Inside the GBA area the wrap-around is kept exactly as on
             * hardware; only the margins are denied to a narrow background. */
            if (px + 8 <= left || px >= right) continue;
            /* Columns past 31 live in the next screenblock, 1024 entries on. */
            unsigned entryAddress = rowBase + (column & 31) * 2 + (column >> 5) * 2048;
            unsigned bank;

            entry = Read16(entryAddress);
            index = entry & 1023;
            bank = color256 ? 0 : TilemapBankAt(entryAddress, entry);
            if (bank)
            {
                slot = BankedTileSlot(bank, entry, chars);
            }
            else if (sMemoStamp[index] == stamp && sMemoEntry[index] == entry)
            {
                slot = sMemoSlot[index];
            }
            else
            {
                unsigned address = chars + index * (color256 ? 64 : 32);
                if (address >= 0x10000) { Error(4, "BG character address exceeds 64 KiB"); continue; }
                slot = GetTileSlot(address, entry >> 12, color256);
                sMemoStamp[index] = stamp;
                sMemoEntry[index] = (uint16_t)entry;
                sMemoSlot[index] = (int16_t)slot;
            }
            if (slot < 0) continue;
            {
                bool flipX = (entry & 1024) != 0, flipY = (entry & 2048) != 0;
                int dx = px, dy = py;

                if (mirrorX2 >= 0) { dx = mirrorX2 - px - 8; flipX = !flipX; }
                if (mirrorY2 >= 0) { dy = mirrorY2 - py - 8; flipY = !flipY; }
                TileWalkSlot(batch, slot, dx + sSpanShiftX + CTR_VIEW_X + sLayerShift,
                             dy + sSpanShiftY + CTR_VIEW_Y, flipX, flipY);
            }
        }
    }
    TileWalkEnd(batch);
    if (cached)
    {
        key.valid = sStats.tiles == tilesBefore;
        sEmptyWalk[bg] = key;
    }
}

static void DrawTextSpan(unsigned bg, int left, int right, int top, int bottom)
{
    DrawTextSpanAt(bg, left, right, top, bottom, -1, -1);
}

/*
 * The scissor the window partition set for the rectangle being composed, so a
 * pass that narrows it can put it back.
 */
static bool sScissored;

static void Scissor(int x0, int y0, int x1, int y1)
{
    float sx0 = (x0 + CTR_VIEW_X) * sZoom + sOffX, sx1 = (x1 + CTR_VIEW_X) * sZoom + sOffX;
    float sy0 = (y0 + CTR_VIEW_Y) * sZoom + sOffY, sy1 = (y1 + CTR_VIEW_Y) * sZoom + sOffY;

    if (sx0 < 0) sx0 = 0;
    if (sy0 < 0) sy0 = 0;
    C3D_SetScissor(GPU_SCISSOR_NORMAL, (unsigned)roundf(sx0), (unsigned)(sSurfaceH - roundf(sy1)),
                   (unsigned)roundf(sx1), (unsigned)(sSurfaceH - roundf(sy0)));
}

static void RestoreScissor(void)
{
    if (sScissored) Scissor(sClipX0, sClipY0, sClipX1, sClipY1);
    else C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
}

/*
 * How a stage fills the screen around its 240x160 picture. A GBA background
 * is one picture drawn for 240x160, so outside that area there is nothing the
 * game drew for it: the port fills it from the picture itself.
 *
 * - Across a background that scrolls sideways - the scenery of the bike ride,
 *   drifting clouds - the tilemap is continuous art made to wrap around, so
 *   the margins show the wrap-around, exactly as the GBA brings those columns
 *   onto its own screen a moment later.
 * - A still picture - the leaves of the Game Freak logo, the legendaries, the
 *   title screen - is carried outwards from its own edge tiles and faded into
 *   black (DrawEdgeRegion), like the iris of a film frame. Mirroring was
 *   tried and rejected: it shows every character on an edge twice.
 *
 * Whether a background scrolls is decided from the last STILL_FRAMES frames
 * and held for SCROLL_HOLD_FRAMES after it stops, so a scene does not switch
 * filling halfway through. A new control register starts the judgement over.
 */
#define STILL_FRAMES 32
#define STILL_SPAN 4
static uint16_t sScrollHistory[4][STILL_FRAMES];
static uint32_t sSceneKey[4];
static bool sScrolls[4];
/* The last frame each layer was seen moving. */
static uint32_t sMovedFrame[4];
/*
 * How long a layer that moved keeps being treated as scrolling: long enough
 * that a scene pausing its scenery for a moment does not switch filling, short
 * enough that the next scene on the same registers starts from what it does.
 */
#define SCROLL_HOLD_FRAMES 120
/* When each still background was last measured (MeasureStill). */
static uint32_t sPictureFrame[4];

static void RecordScroll(void)
{
    for (unsigned bg = 0; bg < 4; ++bg)
    {
        uint32_t key = Reg(8 + bg * 2) | ((Reg(0) & 7) << 16) | ((Reg(0) >> (8 + bg) & 1) << 20);
        /* A horizontal scroll driven line by line is a wave, not a pan: the
         * register only holds the first line of it. */
        unsigned scroll = (sLineMask & (1u << (bg * 2))) ? 0 : Reg(0x10 + bg * 4) & 511;
        int low = 0, high = 0;

        if (key != sSceneKey[bg])
        {
            sSceneKey[bg] = key;
            sScrolls[bg] = false;
            sPictureFrame[bg] = 0;
            for (unsigned i = 0; i < STILL_FRAMES; ++i) sScrollHistory[bg][i] = scroll;
        }
        sScrollHistory[bg][sStats.frames % STILL_FRAMES] = scroll;
        for (unsigned i = 0; i < STILL_FRAMES; ++i)
        {
            /* Unwrapped against the current value, so 511 -> 0 is one step. */
            int delta = (((int)sScrollHistory[bg][i] - (int)scroll + 256) & 511) - 256;

            if (delta < low) low = delta;
            if (delta > high) high = delta;
        }
        /* A shake of a few pixels is not scrolling; a pan is. */
        if (high - low > STILL_SPAN)
        {
            sScrolls[bg] = true;
            sMovedFrame[bg] = sStats.frames;
        }
        else if (sScrolls[bg] && sStats.frames - sMovedFrame[bg] > SCROLL_HOLD_FRAMES)
        {
            sScrolls[bg] = false;
        }
    }
}

/* Clamps a cell to the clip rectangle; false when nothing of it is left. */
static bool ClipCell(int *x0, int *x1, int *y0, int *y1)
{
    if (*x0 < sClipX0) *x0 = sClipX0;
    if (*x1 > sClipX1) *x1 = sClipX1;
    if (*y0 < sClipY0) *y0 = sClipY0;
    if (*y1 > sClipY1) *y1 = sClipY1;
    return *x0 < *x1 && *y0 < *y1;
}

/*
 * The rows a background's picture really covers. The intro's cinematic scenes
 * are drawn only between their letterbox bars - the clouds and Rayquaza have
 * nothing above line 32 or below 128 - so when window 0 is a full-width band
 * that hides this layer outside it, the band is the picture, and the space
 * the stage opens above and below it is filled from the band's own edges.
 */
static void PictureRows(unsigned bg, int *top, int *bottom)
{
    unsigned display = Reg(0), across = Reg(0x40), rows = Reg(0x44);

    *top = 0;
    *bottom = 160;
    if (!(display & 0x2000) || (Reg(0x4a) & (1u << bg)) || !(Reg(0x48) & (1u << bg))) return;
    if ((across >> 8) != 0 || ((across & 255) < 240 && (across & 255) != 0)) return;
    if ((rows >> 8) >= (rows & 255) || (rows & 255) > 160) return;
    *top = (int)(rows >> 8);
    *bottom = (int)(rows & 255);
}

/*
 * One cell of the 3x3 partition of a stage around its picture: the part of
 * the current clip rectangle inside [x0, x1) x [y0, y1), drawn from the source
 * the cell calls for. A mirror axis is given doubled; -1 means drawn directly.
 * A mirrored cell only ever takes from the picture [0, 240) x [top, bottom).
 */
static void DrawStageCell(unsigned bg, int x0, int x1, int y0, int y1,
                          int mirrorX2, int mirrorY2, int top, int bottom)
{
    int sx0, sx1, sy0, sy1;

    if (!ClipCell(&x0, &x1, &y0, &y1)) return;
    sx0 = mirrorX2 < 0 ? x0 : mirrorX2 - x1;
    sx1 = mirrorX2 < 0 ? x1 : mirrorX2 - x0;
    sy0 = mirrorY2 < 0 ? y0 : mirrorY2 - y1;
    sy1 = mirrorY2 < 0 ? y1 : mirrorY2 - y0;
    if (mirrorX2 >= 0) { if (sx0 < 0) sx0 = 0; if (sx1 > 240) sx1 = 240; }
    if (mirrorY2 >= 0) { if (sy0 < top) sy0 = top; if (sy1 > bottom) sy1 = bottom; }
    if (sx0 >= sx1 || sy0 >= sy1) return;
    /* Whole tiles overhang a cell; the scissor keeps each cell to itself. */
    C2D_Flush();
    Scissor(x0, y0, x1, y1);
    DrawTextSpanAt(bg, sx0, sx1, sy0, sy1, mirrorX2, mirrorY2);
    C2D_Flush();
    RestoreScissor();
}

/*
 * Whether a still layer is a panorama: art in the columns the GBA screen never
 * reaches (30 and 31 of a 32-column map), as dense as just inside the edge,
 * means a picture made to wrap - the sky of the bike ride, which holds still
 * while the scenery in front of it scrolls. Measured every few frames, which
 * is plenty for art that does not move.
 */
static bool sPanorama[4];

static void MeasureStill(unsigned bg)
{
    unsigned control = Reg(8 + bg * 2), size = control >> 14;
    unsigned map = ((control >> 8) & 31) * 0x800;
    unsigned chars = ((control >> 2) & 3) * 0x4000;
    bool color256 = (control & 128) != 0;
    unsigned scrollX = Reg(0x10 + bg * 4) & 511, scrollY = Reg(0x12 + bg * 4) & 511;
    unsigned beyond = 0, inside = 0;

    /* Signed: on an even frame the stamp (frames | 1) is one ahead, and the
     * unsigned difference wrapped, measuring again on every call. */
    if (sPictureFrame[bg] && (int32_t)(sStats.frames - sPictureFrame[bg]) < 8) return;
    sPictureFrame[bg] = sStats.frames | 1;
    for (unsigned y = 0; y < 20 && !(size & 1); ++y)
    {
        unsigned rowBase = map + CtrVideo_TextMapOffset(0, y + scrollY / 8, size);

        for (unsigned x = 28; x < 32; ++x)
        {
            unsigned entry = Read16(rowBase + ((x + scrollX / 8) & 31) * 2);
            unsigned address = chars + (entry & 1023) * (color256 ? 64 : 32);

            if (address < 0x10000 && GetTileSlot(address, entry >> 12, color256) >= 0)
                ++*(x < 30 ? &inside : &beyond);
        }
    }
    sPanorama[bg] = beyond && beyond * 4 >= inside * 3;
}

/*
 * The margins of a still scene: the picture's own edge carried outwards and
 * faded into black, like the iris of a film frame. Each margin tile repeats
 * the tile at the edge of the picture in its row (or column, or corner), and
 * its four corners are tinted towards black by their distance from the
 * picture, so the fade is a smooth gradient, not steps of eight pixels.
 * Nothing is mirrored or duplicated, so a character on the edge of the art is
 * never seen twice.
 */
#define FADE_ACROSS 56.0f
#define FADE_DOWN 28.0f
static bool StageUnfaded(void);

static float EdgeFade(int x, int y, int top, int bottom)
{
    float across = x < 0 ? -x / FADE_ACROSS : x > 240 ? (x - 240) / FADE_ACROSS : 0.0f;
    float down = y < top ? (top - y) / FADE_DOWN : y > bottom ? (y - bottom) / FADE_DOWN : 0.0f;

    if (StageUnfaded()) return 0.0f;
    float fade = across > down ? across : down;

    return fade > 1.0f ? 1.0f : fade;
}

/*
 * The zones past the fade are black whatever the still layers hold there, so
 * those layers skip them and the frame starts with them black instead: a
 * rectangle each rather than a column of fully faded tiles per layer. Layers
 * that wrap and sprites still draw over them.
 */
static bool sUnderlaid;

/*
 * Rayquaza on the title screen stands on the bottom edge of the screen, as it
 * does on the GBA's, rather than 40 lines above it: its layer is drawn that
 * much lower, so nothing has to be made up under its coils, and the room it
 * leaves above is its sky, carried up from its top row. Nothing on this
 * screen fades: its margins are its edge tiles repeated at full light, and
 * no black is laid under them.
 */
#define TITLE_RAYQUAZA_DROP (CTR_GAME_HEIGHT - 160 - CTR_STAGE_Y)
int CtrTitleScreen_RayquazaBg(void);

static int StageLayerDrop(unsigned bg)
{
    return sStage && CtrTitleScreen_RayquazaBg() == (int)bg ? TITLE_RAYQUAZA_DROP : 0;
}

static bool StageUnfaded(void)
{
    return sStage && CtrTitleScreen_RayquazaBg() >= 0;
}

static void StageUnderlay(void)
{
    unsigned display = Reg(0), mode = display & 7;
    bool still = false;
    u32 black = C2D_Color32(0, 0, 0, 255);

    sUnderlaid = false;
    /* Under windows the margins may be the backdrop on purpose (the
     * letterbox, which flashes with it), so nothing is assumed there. */
    if (!sStage || StageUnfaded() || (display & 128) || (display & 0x6000)) return;
    for (unsigned bg = 0; bg < 4; ++bg)
        if ((display & (0x100u << bg)) && !sScrolls[bg] && mode != 2 && !(mode == 1 && bg == 2))
            still = true;
    if (!still) return;
    ViewBase();
    C2D_DrawRectSolid(0, 0, 0, CTR_VIEW_X - FADE_ACROSS, CTR_GAME_HEIGHT, black);
    C2D_DrawRectSolid(CTR_VIEW_X + 240 + FADE_ACROSS, 0, 0, CTR_VIEW_X - FADE_ACROSS, CTR_GAME_HEIGHT, black);
    C2D_DrawRectSolid(0, 0, 0, CTR_GAME_WIDTH, CTR_VIEW_Y - FADE_DOWN, black);
    C2D_DrawRectSolid(0, CTR_VIEW_Y + 160 + FADE_DOWN, 0, CTR_GAME_WIDTH, CTR_VIEW_Y - FADE_DOWN, black);
    sUnderlaid = true;
}

/*
 * The Game Corner's backdrops (3ds_corner_art.h): two small textures filled
 * once when the roulette or the slot machine opens, drawn repeated as the
 * backdrop - a dozen quads, faded with it (sTint, set by Blend(5)). Freed a
 * while after the screen closes, outside the frame (CornerArtRelease).
 */
static C3D_Tex sCornerTex[2];
static unsigned sCornerScreen;
static uint16_t sCornerTable;
static uint32_t sCornerUsed;
static bool sCornerFailed;

static void CornerArtRelease(void)
{
    for (unsigned i = 0; i < 2; ++i)
        if (sCornerTex[i].data) C3D_TexDelete(&sCornerTex[i]);
    memset(sCornerTex, 0, sizeof(sCornerTex));
    sCornerScreen = CTR_CENTRED_NONE;
}

/* The textures for the screen on show; false without memory for them. */
static bool CornerArtReady(unsigned screen)
{
    /* Green at either table, as the tables stand in the Game Corner: the
     * green the game gives its other table (roulette.c, bgColors[1]). */
    uint16_t table = screen == CTR_CENTRED_ROULETTE ? 10 | 19 << 5 | 6 << 10 : 0;

    sCornerUsed = sStats.frames;
    if (!sCornerTex[0].data)
    {
        if (sCornerFailed) return false;
        if (!C3D_TexInit(&sCornerTex[0], CTR_CORNER_PATTERN_DIM, CTR_CORNER_PATTERN_DIM, GPU_RGB565)
            || !C3D_TexInit(&sCornerTex[1], CTR_CORNER_TRIM_DIM, CTR_CORNER_TRIM_DIM, GPU_RGB565))
        {
            CornerArtRelease();
            sCornerFailed = true;
            CtrLog_Write(CTR_LOG_ERROR, "VIDEO: no linear memory for the Game Corner's backdrop "
                         "(linear free=%lu); plain backdrop", (unsigned long)linearSpaceFree());
            return false;
        }
        for (unsigned i = 0; i < 2; ++i)
        {
            C3D_TexSetFilter(&sCornerTex[i], GPU_NEAREST, GPU_NEAREST);
            C3D_TexSetWrap(&sCornerTex[i], GPU_REPEAT, GPU_REPEAT);
        }
        sCornerScreen = CTR_CENTRED_NONE;
    }
    if (sCornerScreen == screen && sCornerTable == table) return true;
    if (screen == CTR_CENTRED_ROULETTE)
        CtrCornerArt_Felt(sCornerTex[0].data, sCornerTex[1].data, table);
    else
        CtrCornerArt_Wall(sCornerTex[0].data, sCornerTex[1].data);
    C3D_TexFlush(&sCornerTex[0]);
    C3D_TexFlush(&sCornerTex[1]);
    sCornerScreen = screen;
    sCornerTable = table;
    return true;
}

/* Screen [x0, x1) x [y0, y1) from texture t, texel (s, u) at its top left,
 * repeated. */
static void CornerQuad(unsigned t, int x0, int y0, int x1, int y1, int s, int u)
{
    const C3D_Tex *tex = &sCornerTex[t];
    const float w = tex->width, h = tex->height;
    const Tex3DS_SubTexture cut = {(u16)(x1 - x0), (u16)(y1 - y0), s / w, 1.0f - u / h,
                                   (s + x1 - x0) / w, 1.0f - (u + y1 - y0) / h};

    if (x0 >= x1 || y0 >= y1) return;
    ++sStats.tiles;
    C2D_DrawImageAt((C2D_Image){(C3D_Tex *)tex, &cut}, x0, y0, 0, &sTint, 1, 1);
}

/* Screen [x0, x1) x [y0, y1) in the colour of texel (texel, texel) of the
 * trim texture. */
static void CornerSolid(int x0, int y0, int x1, int y1, unsigned texel)
{
    const float c = (texel + 0.5f) / CTR_CORNER_TRIM_DIM;
    const Tex3DS_SubTexture cut = {(u16)(x1 - x0), (u16)(y1 - y0), c, 1.0f - c, c, 1.0f - c};

    if (x0 >= x1 || y0 >= y1) return;
    ++sStats.tiles;
    C2D_DrawImageAt((C2D_Image){&sCornerTex[1], &cut}, x0, y0, 0, &sTint, 1, 1);
}

/* The four sides of a ring: rows y0 and y1 - 1 across, columns x0 and
 * x1 - 1 between them, d wide. */
static void CornerRing(int x0, int y0, int x1, int y1, int d, unsigned texel)
{
    CornerSolid(x0, y0, x1, y0 + d, texel);
    CornerSolid(x0, y1 - d, x1, y1, texel);
    CornerSolid(x0, y0 + d, x0 + d, y1 - d, texel);
    CornerSolid(x1 - d, y0 + d, x1, y1 - d, texel);
}

/*
 * The roulette's felt: green, with its Poké Balls, over the whole screen -
 * the picture's backdrop too - inside a dark band and a gold cord round the
 * screen's edge.
 */
static void DrawRouletteFelt(void)
{
    const int w = CTR_GAME_WIDTH, h = CTR_GAME_HEIGHT;

    CornerQuad(0, 8, 8, w - 8, h - 8, 8, 8);
    CornerRing(0, 0, w, h, 6, CTR_CORNER_FELT_BAND);
    /* The cord, dashed along (x + y) as row 0 and column 0 hold it. */
    CornerQuad(1, 6, 6, w - 6, 7, 12, 0);
    CornerQuad(1, 6, h - 7, w - 6, h - 6, 6 + h - 7, 0);
    CornerQuad(1, 6, 7, 7, h - 7, 0, 13);
    CornerQuad(1, w - 7, 7, w - 6, h - 7, 0, w - 7 + 7);
    CornerRing(7, 7, w - 7, h - 7, 1, CTR_CORNER_FELT_LINE);
}

/*
 * The slot machine in the casino's wall: the wall in the margins only (the
 * machine is the picture), and round the picture a gold frame, 13 pixels
 * wide, its red band lit with bulbs 16 apart - which is what makes the
 * corners' bulbs meet, and the wall's coins (8 + 16k across, 16k down) fall
 * clear of the frame's edges.
 */
static void DrawSlotsWall(void)
{
    const int w = CTR_GAME_WIDTH, h = CTR_GAME_HEIGHT, px = CTR_VIEW_X, py = CTR_VIEW_Y;
    const int x0 = px - 13, y0 = py - 13, x1 = px + 240 + 13, y1 = py + 160 + 13;
    const int half = CTR_CORNER_BULB_PERIOD / 2;

    CornerQuad(0, 0, 0, w, y0, 8, 0);
    CornerQuad(0, 0, y1, w, h, 8, y1);
    CornerQuad(0, 0, y0, x0, y1, 8, y0);
    CornerQuad(0, x1, y0, w, y1, x1 + 8, y0);
    CornerRing(x0, y0, x1, y1, 1, CTR_CORNER_FRAME_DARK);
    CornerRing(x0 + 1, y0 + 1, x1 - 1, y1 - 1, 1, CTR_CORNER_FRAME_GOLD);
    CornerRing(x0 + 8, y0 + 8, x1 - 8, y1 - 8, 1, CTR_CORNER_FRAME_GOLD);
    CornerRing(x0 + 9, y0 + 9, x1 - 9, y1 - 9, 1, CTR_CORNER_FRAME_DARK);
    CornerRing(x0 + 10, y0 + 10, x1 - 10, y1 - 10, 3, CTR_CORNER_FRAME_SHADOW);
    /* The band: lit bulbs first along the top and down the sides, dark
     * first along the bottom, so they alternate the whole way round. */
    CornerQuad(1, x0 + 2, y0 + 2, x1 - 2, y0 + 8, 0, 0);
    CornerQuad(1, x0 + 2, y1 - 8, x1 - 2, y1 - 2, half, 0);
    CornerQuad(1, x0 + 2, y0 + 8, x0 + 8, y1 - 8, 0, 6);
    CornerQuad(1, x1 - 8, y0 + 8, x1 - 2, y1 - 8, 0, 6);
}

/* Behind a Game Corner screen, instead of its backdrop or black margins. */
static void CornerUnderlay(void)
{
    if (!sCentred || (sCentredScreen != CTR_CENTRED_ROULETTE && sCentredScreen != CTR_CENTRED_SLOTS)
        || (Reg(0) & 128) || !CornerArtReady(sCentredScreen))
        return;
    ViewBase();
    if (sCentredScreen == CTR_CENTRED_ROULETTE) DrawRouletteFelt();
    else DrawSlotsWall();
}

/*
 * The GBA brightness effect a layer is under (BLDCNT effect 2 or 3), as the
 * colour it moves towards and how far; else a background's palette fade drawn
 * as a tint (UpdatePalette); 0 when there is neither.
 */
static float LayerBrightness(unsigned layer, bool *white)
{
    unsigned control = Reg(0x50), effect = (control >> 6) & 3;

    *white = effect == 2;
    if (!(control & (1u << layer)) || effect < 2)
    {
        /* The stages' own edge fades know black and white only. */
        *white = sPaletteFadeColor == 0x7fff;
        return sInLayerTexture || layer > 5 || layer == 4 ? 0.0f : sPaletteFade;
    }
    return Min(Reg(0x54) & 31, 16) / 16.0f;
}

/*
 * One corner of a fading tile. The fade to black f and the layer's own
 * brightness effect k are one tint: darkening gives c(1-k)(1-f), which is a
 * blend of 1-(1-k)(1-f) towards black; brightening gives
 * (c(1-k) + k)(1-f), a blend of the same amount towards a grey of
 * k(1-f) / blend. Either way a fully faded corner is black.
 */
static void FadeCorner(C2D_ImageTint *tint, C2D_Corner corner, float f, float k, bool white)
{
    float blend = 1.0f - (1.0f - k) * (1.0f - f);
    float grey = white && blend > 0.0f ? k * (1.0f - f) / blend : 0.0f;
    unsigned level = (unsigned)(grey * 255.0f + 0.5f);

    C2D_SetImageTint(tint, corner, C2D_Color32(level, level, level, 255), blend);
}

/*
 * Above and below a still picture each margin column repeats the tile on the
 * picture's edge row - unless that row is mostly one tile, a plain band with
 * something drawn over part of it: the title screen's bottom row is its
 * backdrop with the tail and coils of Rayquaza across the middle, and those
 * repeated downwards are streaks. Such a row carries on as its plain tile,
 * and the objects stay in the picture. The entry that fills at least 40% of
 * the row, or ~0u when none does: Rayquaza's coils take half of that row, its
 * backdrop tile only the other half, and anything spread over less than 40%
 * of a row is the picture itself, not a band behind it.
 */
static unsigned EdgeRowPlain(unsigned bg, int y)
{
    unsigned control = Reg(8 + bg * 2), size = control >> 14;
    unsigned map = ((control >> 8) & 31) * 0x800;
    unsigned scrollX = Reg(0x10 + bg * 4) & 511, scrollY = Reg(0x12 + bg * 4) & 511;
    unsigned rowBase = map + CtrVideo_TextMapOffset(0, (unsigned)(y + (int)scrollY) >> 3, size);
    unsigned entries[31], best = ~0u, bestCount = 0;

    if (scrollX & 7) return ~0u;
    for (unsigned c = 0; c < 30; ++c)
    {
        unsigned column = ((scrollX >> 3) + c) & ((size & 1) ? 63u : 31u);
        entries[c] = Read16(rowBase + (column & 31) * 2 + (column >> 5) * 2048);
    }
    for (unsigned c = 0; c < 30; ++c)
    {
        unsigned count = 0;

        for (unsigned k = 0; k < 30; ++k) count += entries[k] == entries[c];
        if (count > bestCount) { bestCount = count; best = entries[c]; }
    }
    return bestCount * 10 >= 30 * 4 ? best : ~0u;
}

static void DrawEdgeRegion(unsigned bg, int x0, int x1, int y0, int y1, int top, int bottom)
{
    unsigned control = Reg(8 + bg * 2), size = control >> 14;
    unsigned map = ((control >> 8) & 31) * 0x800;
    unsigned chars = ((control >> 2) & 3) * 0x4000;
    bool color256 = (control & 128) != 0;
    unsigned scrollX = Reg(0x10 + bg * 4) & 511, scrollY = Reg(0x12 + bg * 4) & 511;
    int ox = (int)(scrollX & 7), oy = (int)(scrollY & 7);
    /* The grid positions of the tiles on the picture's edges. */
    int firstX = -ox, lastX = ((239 + ox) & ~7) - ox;
    int firstY = ((top + oy) & ~7) - oy, lastY = ((bottom - 1 + oy) & ~7) - oy;
    bool white;
    float bright = LayerBrightness(bg, &white);
    /* A margin row repeats one edge tile, so the last lookup usually answers. */
    unsigned lastEntry = ~0u;
    int lastSlot = -1;
    unsigned plainTop = EdgeRowPlain(bg, top), plainBottom = EdgeRowPlain(bg, bottom - 1);

    if (!ClipCell(&x0, &x1, &y0, &y1)) return;
    C2D_Flush();
    Scissor(x0, y0, x1, y1);
    ViewBase();
    for (int py = ((y0 + oy) & ~7) - oy - 8; py < y1; py += 8)
    {
        int sy = py < firstY ? firstY : py > lastY ? lastY : py;
        unsigned rowBase = map + CtrVideo_TextMapOffset(0, (unsigned)(sy + (int)scrollY) >> 3, size);

        if (py + 8 <= y0) continue;
        for (int px = ((x0 + ox) & ~7) - ox - 8; px < x1; px += 8)
        {
            int sx = px < firstX ? firstX : px > lastX ? lastX : px;
            unsigned column = ((unsigned)(sx + (int)scrollX) >> 3) & ((size & 1) ? 63u : 31u);
            unsigned entry = Read16(rowBase + (column & 31) * 2 + (column >> 5) * 2048);
            unsigned address;
            int slot;

            if (py < firstY && plainTop != ~0u) entry = plainTop;
            else if (py > lastY && plainBottom != ~0u) entry = plainBottom;
            address = chars + (entry & 1023) * (color256 ? 64 : 32);
            if (px + 8 <= x0 || address >= 0x10000) continue;
            if (entry != lastEntry)
            {
                lastEntry = entry;
                lastSlot = GetTileSlot(address, entry >> 12, color256);
            }
            slot = lastSlot;
            if (slot < 0) continue;
            /* Past the fade the tile would be black: StageUnderlay drew that. */
            if (sUnderlaid && EdgeFade(px, py, top, bottom) >= 1.0f && EdgeFade(px + 8, py, top, bottom) >= 1.0f
                && EdgeFade(px, py + 8, top, bottom) >= 1.0f && EdgeFade(px + 8, py + 8, top, bottom) >= 1.0f)
                continue;
            {
                C2D_ImageTint fade;

                FadeCorner(&fade, C2D_TopLeft, EdgeFade(px, py, top, bottom), bright, white);
                FadeCorner(&fade, C2D_TopRight, EdgeFade(px + 8, py, top, bottom), bright, white);
                FadeCorner(&fade, C2D_BotLeft, EdgeFade(px, py + 8, top, bottom), bright, white);
                FadeCorner(&fade, C2D_BotRight, EdgeFade(px + 8, py + 8, top, bottom), bright, white);
                DrawSlotTinted(slot, px + CTR_VIEW_X + sLayerShift, py + CTR_VIEW_Y,
                               entry & 1024, entry & 2048, &fade);
            }
        }
    }
    C2D_Flush();
    RestoreScissor();
}

static bool DrawLeavesMargins(unsigned bg);
static bool LayerDrawable(unsigned bg);

static void DrawStageBg(unsigned bg)
{
    int top, bottom;
    bool wraps = sScrolls[bg];

    PictureRows(bg, &top, &bottom);
    /*
     * The cable car's ride: its trees, ground and pylons scroll, and wrapped
     * across the margins they showed the parts of their maps the GBA never
     * does (pieces of ground not laid yet, trees above the sky). Like the
     * intro, every layer is the picture with its edges faded into black.
     */
    if (sStageFaded) wraps = false;
    if (!wraps)
    {
        MeasureStill(bg);
        /* A still panorama wraps too when the scene around it scrolls. */
        if (sPanorama[bg] && !sStageFaded)
            for (unsigned other = 0; other < 4; ++other)
                if (other != bg && sScrolls[other] && (Reg(0) & (0x100u << other))) wraps = true;
    }
    if (wraps)
    {
        /*
         * A layer that scrolls - the bike ride - is continuous art made to
         * wrap around: across, one span wrapping as on the GBA; above and
         * below, the mirror image of the picture's own edge rows.
         */
        const int rows[3][3] = {{-512, top, 2 * top}, {top, bottom, -1}, {bottom, 512, 2 * bottom}};

        for (unsigned r = 0; r < 3; ++r)
            DrawStageCell(bg, VIEW_LEFT, VIEW_RIGHT, rows[r][0], rows[r][1], -1, rows[r][2],
                          top, bottom);
        return;
    }
    DrawStageCell(bg, 0, 240, top, bottom, -1, -1, top, bottom);
    if (DrawLeavesMargins(bg)) return;
    DrawEdgeRegion(bg, -512, 512, -512, top, top, bottom);
    DrawEdgeRegion(bg, -512, 512, bottom, 512, top, bottom);
    DrawEdgeRegion(bg, -512, 0, top, bottom, top, bottom);
    DrawEdgeRegion(bg, 240, 512, top, bottom, top, bottom);
}

/*
 * The battle scene. Its picture is 240x160 like any GBA screen, but it is
 * made of a scene that the GBA itself carries past the screen - the terrain is
 * a 512-pixel map whose sky continues on both sides, the entry grass and the
 * move backgrounds are tiles that repeat - and a text box that is one frame
 * of caps and a middle. So nothing is faded or mirrored here:
 *
 * - The text box (the last six tile rows of BG0) is moved to the left edge of
 *   the screen and its middle repeated up to a right cap on the right edge.
 *   Everything in it - the message, the prompt arrow, a menu page - keeps its
 *   place from the left cap, as it does on the GBA.
 * - A layer that wraps on its own - a 64-column map, or a 32-column one whose
 *   hidden columns hold art as dense as the visible ones (MeasureStill) - is
 *   drawn as the GBA wraps it across the sides. Above the picture, each tile
 *   row is taken where the map wraps it; where that row is empty, from the
 *   same row of a 32-row picture repeated in a 64-row map; and on the terrain,
 *   from the picture's own top row, which is what its map repeats above it
 *   (rows 30 and 31 of every battle terrain are its row 0).
 * - Anything else - windows, a battler copied into a background - stays in
 *   the 240x160 picture, where the game placed it.
 */
#define BATTLE_BAND_TOP 112
#define BATTLE_BAND_CAP 16

static unsigned MapEntry(unsigned map, unsigned size, unsigned column, unsigned row)
{
    column &= (size & 1) ? 63u : 31u;
    return Read16(map + CtrVideo_TextMapOffset(0, row, size) + (column & 31) * 2 + ((column >> 5) & 1) * 2048);
}

static int EntrySlot(unsigned chars, unsigned entry, bool color256)
{
    unsigned address = chars + (entry & 1023) * (color256 ? 64 : 32);

    if (address >= 0x10000) return -1;
    return GetTileSlot(address, entry >> 12, color256);
}

/* Part of the picture's own tiles drawn moved by shift, kept to [x0, x1) x [y0, y1). */
static void DrawBattleCut(unsigned bg, int x0, int x1, int y0, int y1, int shift)
{
    if (!ClipCell(&x0, &x1, &y0, &y1)) return;
    C2D_Flush();
    Scissor(x0, y0, x1, y1);
    sSpanShiftX = shift;
    DrawTextSpanAt(bg, x0 - shift, x1 - shift, y0, y1, -1, -1);
    sSpanShiftX = 0;
    C2D_Flush();
    RestoreScissor();
}

static void SpriteBatchFlush(void);

/*
 * The battle's info boxes and text box at the screen's resolution
 * (3ds_battle_hud.h).
 *
 * Each info box battle_interface.c makes has a picture drawn for the top
 * screen; its sprites - the two halves and the bar - are left out wherever
 * the GBA would draw them, and the picture drawn instead where the left half
 * is. Normally that is after the scene, at the screen's own pixels
 * (HudDrawCrisp): nothing the battle draws goes over its info boxes, which
 * moves raise to the front while they play (UpdateOamPriorityInAllHealthboxes).
 * When something does - a background or a sprite in front of a box, a window
 * shaping the sprites - or the scene is composed per eye for the 3D slider,
 * the picture is drawn into the scene in the left half's place instead
 * (HudDrawInScene), magnified with it.
 *
 * A picture is two RGBA8 textures, 128 and 32 texels wide (40 KiB of linear
 * memory, where one 256 wide would take 64), made for the boxes of a battle
 * and given back after it.
 */
#define HUD_TEX_H 64
static const unsigned sHudPartW[2] = {128, 32};
static C3D_Tex sHudTex[CTR_HUD_BOXES][2];
static uint32_t sHudVersion[CTR_HUD_BOXES];
static bool sHudReady[CTR_HUD_BOXES];
static const CtrHudBox *sHud;
static bool sHudCrisp, sHudFailLogged;
static uint32_t sHudLastBattleFrame;
/* When making a picture last failed (0: not since the last battle). */
static uint32_t sHudFailFrame;
#define HUD_RETRY_FRAMES 120
/* Whether a background in front of the boxes drew something last frame. */
static bool sHudCovered;
/* This frame's box of each OAM entry (HudBoxOfTile), or -1: read once in
 * HudFrame, for every pass that asks (the 3D slider's slots compose the
 * sprites once a group). */
static int8_t sHudOamBox[128];

static void HudRelease(void)
{
    for (unsigned k = 0; k < CTR_HUD_BOXES; ++k)
    {
        for (unsigned part = 0; part < 2; ++part)
        {
            if (sHudTex[k][part].data) C3D_TexDelete(&sHudTex[k][part]);
            memset(&sHudTex[k][part], 0, sizeof(sHudTex[k][part]));
        }
        sHudReady[k] = false;
        sHudVersion[k] = 0;
    }
    sHudFailFrame = 0;
}

/*
 * The picture's rows that changed (the box's changedTop/Bottom; all of them
 * into a texture just made) into its two textures, a tile row of 8 at a time:
 * a bar going down is two or three tile rows. Tile by tile, each texel's
 * place from the Morton table - CtrVideo_Texel for every texel divided by the
 * texture's width each time, and the whole picture went up every frame of a
 * bar's slide: 1.6 ms a box in Azahar.
 */
static bool HudUpload(unsigned k, const CtrHudBox *box)
{
    unsigned top = box->changedTop & ~7u, bottom = (box->changedBottom + 7u) & ~7u;

    if (!sHudTex[k][0].data || !sHudTex[k][1].data || bottom > HUD_TEX_H || top >= bottom)
    {
        top = 0;
        bottom = HUD_TEX_H;
    }
    for (unsigned part = 0, x0 = 0; part < 2; x0 += sHudPartW[part], ++part)
    {
        C3D_Tex *tex = &sHudTex[k][part];
        uint32_t *data;

        if (!tex->data)
        {
            /* Not every frame: tried again a couple of seconds on, as BallTexture. */
            if (sHudFailFrame && (int32_t)(sStats.frames - sHudFailFrame) < HUD_RETRY_FRAMES) return false;
            if (!C3D_TexInit(tex, sHudPartW[part], HUD_TEX_H, GPU_RGBA8))
            {
                memset(tex, 0, sizeof(*tex));
                /* Without its other part the box is not drawn: give back the
                 * 32 KiB of the first rather than refill it every frame. */
                if (part && sHudTex[k][0].data)
                {
                    C3D_TexDelete(&sHudTex[k][0]);
                    memset(&sHudTex[k][0], 0, sizeof(sHudTex[k][0]));
                }
                sHudFailFrame = sStats.frames | 1;
                if (!sHudFailLogged)
                    CtrLog_Write(CTR_LOG_ERROR, "video: no memory for an HD info box (linear free=%lu); "
                                 "the GBA's box is shown", (unsigned long)linearSpaceFree());
                sHudFailLogged = true;
                return false;
            }
            C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        }
        data = tex->data;
        for (unsigned ty = top; ty < bottom; ty += 8)
            for (unsigned tx = 0; tx < sHudPartW[part]; tx += 8)
            {
                /* Tiles run left to right, then down (CtrVideo_Texel). */
                uint32_t *tile = data + ((ty / 8) * (sHudPartW[part] / 8) + tx / 8) * 64;

                for (unsigned y = 0; y < 8; ++y)
                {
                    const uint32_t *row = box->pixels + (ty + y) * CTR_HUD_W + x0 + tx;
                    bool inside = ty + y < CTR_HUD_H;

                    for (unsigned x = 0; x < 8; ++x)
                        tile[sMorton[y * 8 + x]] = inside && x0 + tx + x < CTR_HUD_W ? row[x] : 0;
                }
            }
        C3D_TexFlush(tex);
    }
    sHudVersion[k] = box->version;
    CtrBattleHud_Uploaded(k);
    return true;
}

/*
 * Blend's state for a picture with soft edges: its colour over what is
 * there, and its coverage added to the surface's (Blend writes the source's
 * alone), so the edges and the shadows of the boxes and the text box do not
 * leave the surface see-through - through to the black under it - when it is
 * put on the screen. Not when the layer blends by the GBA's alpha effect,
 * which keeps Blend's own. BlendForget after drawing.
 */
static void HudBlend(unsigned layer, bool effects)
{
    unsigned control = Reg(0x50);

    Blend(layer, effects, false);
    if (effects && ((control >> 6) & 3) == 1 && (control & (1u << layer)) && ((control >> 8) & 63)) return;
    C2D_Flush();
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
}

/*
 * The tint a box's picture is drawn with: the palette fade its sprites are
 * under (the picture is unfaded), then the layer's own (Blend's, a brightness
 * effect). Two mixes towards a colour are one: strength 1 - (1-a)(1-b), the
 * colour the two weighted by what each adds.
 */
static void HudTint(unsigned k, C2D_ImageTint *tint)
{
    float a = sHud[k].fade;

    *tint = sTint;
    if (a <= 0) return;
    for (unsigned c = 0; c < 4; ++c)
    {
        float b = tint->corners[c].blend, both = 1 - (1 - a) * (1 - b);
        uint32_t layer = tint->corners[c].color, rgb[3];

        for (unsigned ch = 0; ch < 3; ++ch)
        {
            float box = (sHud[k].fadeColour >> (24 - ch * 8)) & 0xFF, own = (layer >> (ch * 8)) & 0xFF;

            rgb[ch] = (uint32_t)((box * a * (1 - b) + own * b) / both + 0.5f);
        }
        tint->corners[c].color = C2D_Color32(rgb[0], rgb[1], rgb[2], layer >> 24);
        tint->corners[c].blend = both;
    }
}

/* A box's picture at (x, y), `scale` times the screen's size. */
static void HudDrawPicture(unsigned k, float x, float y, float scale, bool smooth)
{
    C2D_ImageTint tint;

    HudTint(k, &tint);
    for (unsigned part = 0, x0 = 0; part < 2; x0 += sHudPartW[part], ++part)
    {
        const Tex3DS_SubTexture all = {(u16)sHudPartW[part], HUD_TEX_H, 0, 1, 1, 0};

        C3D_TexSetFilter(&sHudTex[k][part], smooth ? GPU_LINEAR : GPU_NEAREST, smooth ? GPU_LINEAR : GPU_NEAREST);
        C2D_DrawImageAt((C2D_Image){&sHudTex[k][part], &all}, x + x0 * scale, y, 0, &tint, scale, scale);
    }
}

/* The box whose sprites use OAM tile `tile`, drawn from its picture; or -1. */
static int HudBoxOfTile(unsigned tile)
{
    if (!sHud) return -1;
    for (unsigned k = 0; k < CTR_HUD_BOXES; ++k)
    {
        const CtrHudBox *box = &sHud[k];

        if (!sHudReady[k]) continue;
        if ((tile >= box->boxTile && tile < (unsigned)box->boxTile + box->boxTiles)
            || (box->barTiles && tile >= box->barTile && tile < (unsigned)box->barTile + box->barTiles))
            return (int)k;
    }
    return -1;
}

/* Whether OAM entry `i` is a box's left half, where its picture is drawn. */
static int HudLeftHalf(unsigned i)
{
    int k = sHudOamBox[i];

    return k >= 0 && (sMemory.oam[i * 4 + 2] & 1023) == sHud[k].boxTile ? k : -1;
}

/* An OBJ's size in pixels by shape (square, wide, tall) and size: w, h. */
static const uint8_t sObjDims[3][4][2] = {
    {{8,8},{16,16},{32,32},{64,64}},
    {{16,8},{32,8},{32,16},{64,32}},
    {{8,16},{8,32},{16,32},{32,64}}
};

/* A visible OAM entry's place in GBA coordinates, wrapped as the battle reads
 * it: a sprite drawn, or with `window` one in the OBJ window's mode. */
static bool OamPlace(unsigned i, bool window, unsigned *priority, int *x, int *y, int *w, int *h)
{
    const uint16_t *obj = sMemory.oam + i * 4;
    unsigned attr0 = obj[0], attr1 = obj[1], attr2 = obj[2], shape = attr0 >> 14;
    bool affine = (attr0 & 0x100) != 0;

    if ((!affine && (attr0 & 0x200)) || shape == 3 || (((attr0 >> 10) & 3) == 2) != window
        || ((attr0 >> 10) & 3) == 3) return false;
    *w = sObjDims[shape][attr1 >> 14][0] * (affine && (attr0 & 0x200) ? 2 : 1);
    *h = sObjDims[shape][attr1 >> 14][1] * (affine && (attr0 & 0x200) ? 2 : 1);
    *x = attr1 & 511;
    *y = attr0 & 255;
    if (*x + *w > 512) *x -= 512;
    if (*y + *h > 256) *y -= 256;
    /* Below the picture: where the GBA parks its unused entries (y 160),
     * tile 0 - an info box's first tile, as often as not. */
    if (*y >= 160 || *y + *h <= 0) return false;
    *priority = (attr2 >> 10) & 3;
    return true;
}

static bool HudEntry(unsigned i, unsigned *priority, int *x, int *y, int *w, int *h)
{
    return OamPlace(i, false, priority, x, y, w, h);
}

/*
 * Whether the boxes can be drawn after the scene: no window shapes the
 * sprites, no background in front of them drew anything, and no sprite in
 * front of a box overlaps its picture.
 */
/*
 * Whether the windows show a box's picture, (x, y) w x h in GBA pixels, as it
 * is: every part of the screen it covers shows sprites, as the compositor
 * reads the windows (WindowPartition). The battle keeps WIN0 and the OBJ
 * window on throughout (battle_bg.c); while its intro opens the scene through
 * WIN0 (BattleCurtain) a box inside the opening is as it would be anywhere.
 * A box the windows cut, or a sprite in window mode shaping the sprites, is
 * drawn into the scene with them.
 */
static unsigned WindowPartition(int top, int bottom, int rects[25][4], unsigned masks[25]);

static bool HudWindowsShow(int x, int y, int w, int h)
{
    unsigned control = Reg(0), count;
    int rects[25][4];
    unsigned masks[25];

    if (!(control & 0xE000)) return true;
    /* A sprite in window mode shapes the sprites only where it is (a stat
     * change's pattern over its battler): one apart from the box, or whose
     * window shows sprites too, leaves the box as it is. */
    if ((control & 0x8000) && !((Reg(0x4A) >> 8) & 0x10))
        for (unsigned i = 0; i < 128; ++i)
        {
            unsigned p;
            int ox, oy, ow, oh;

            if (OamPlace(i, true, &p, &ox, &oy, &ow, &oh) && ox < x + w && ox + ow > x && oy < y + h && oy + oh > y)
                return false;
        }
    if (y < 0) { h += y; y = 0; }
    if (y + h > 160) h = 160 - y;
    if (h <= 0) return true;
    count = WindowPartition(y, y + h, rects, masks);
    for (unsigned r = 0; r < count; ++r)
        if (rects[r][0] < x + w && rects[r][2] > x && !(masks[r] & 0x10)) return false;
    return true;
}

static bool HudCrispPossible(void)
{
    if (sHudCovered) return false;
    for (unsigned i = 0; i < 128; ++i)
    {
        unsigned p;
        int x, y, w, h, k = HudLeftHalf(i);

        if (k < 0 || !HudEntry(i, &p, &x, &y, &w, &h))
            continue;
        /* The picture's extent in GBA pixels. */
        w = (int)ceilf(CTR_HUD_W / CTR_BATTLE_ZOOM);
        h = (int)ceilf(CTR_HUD_H / CTR_BATTLE_ZOOM);
        if (!HudWindowsShow(x, y, w, h)) return false;
        for (unsigned j = 0; j < 128; ++j)
        {
            unsigned q;
            int ox, oy, ow, oh;

            if (j == i || sHudOamBox[j] >= 0) continue;
            if (!HudEntry(j, &q, &ox, &oy, &ow, &oh)) continue;
            if (q > p || (q == p && j > i)) continue;
            if (ox < x + w && ox + ow > x && oy < y + h && oy + oh > y) return false;
        }
    }
    return true;
}

static unsigned BattleBoxLayers(void);

/* After a scene: whether a background in front of the boxes drew in it. */
static void HudCoverUpdate(void)
{
    unsigned front = 4;

    sHudCovered = false;
    if (!sHud) return;
    for (unsigned i = 0; i < 128; ++i)
    {
        unsigned p;
        int x, y, w, h, k = HudLeftHalf(i);

        if (k >= 0 && HudEntry(i, &p, &x, &y, &w, &h)
            && p < front)
            front = p;
    }
    for (unsigned bg = 1; bg < 4; ++bg)
        if (sBgDrawn[bg] && (Reg(0) & (0x100u << bg)) && (Reg(8 + bg * 2) & 3) < front
            && !(BattleBoxLayers() & (1u << bg)))
            sHudCovered = true;
}

/* This frame's boxes, their pictures uploaded; given back once out of battle. */
static void HudFrame(void)
{
    sHud = NULL;
    memset(sHudOamBox, -1, sizeof(sHudOamBox));
    if (!sBattle)
    {
        if (sHudTex[0][0].data || sHudTex[1][0].data || sHudTex[2][0].data || sHudTex[3][0].data)
            if (sStats.frames - sHudLastBattleFrame > 60) HudRelease();
        return;
    }
    sHudLastBattleFrame = sStats.frames;
    sHud = CtrBattleHud_Boxes();
    if (!sHud) return;
    for (unsigned k = 0; k < CTR_HUD_BOXES; ++k)
    {
        if (!sHud[k].used) { sHudReady[k] = false; continue; }
        if (sHudReady[k] && sHudVersion[k] == sHud[k].version) continue;
        sHudReady[k] = HudUpload(k, &sHud[k]);
    }
    for (unsigned i = 0; i < 128; ++i)
        sHudOamBox[i] = (int8_t)HudBoxOfTile(sMemory.oam[i * 4 + 2] & 1023);
}

/*
 * The part of a box's picture over one of its halves, in the current view
 * (the scene's): each half draws what is over its own rectangle, so a box
 * whose halves the 3D slider's sprite groups put apart is still whole.
 */
static void HudDrawInScene(unsigned k, unsigned tile, int x, int y, int w, int h, bool effects)
{
    /* The right half is 64 pixels right of the left one (SpriteCB_HealthBoxOther). */
    int left = tile == sHud[k].boxTile ? x : x - 64;

    SpriteBatchFlush();
    HudBlend(4, effects);
    ViewBase();
    C2D_Flush();
    /* With each sprite at its depth (BattleSpriteShift), the half is moved too. */
    Scissor((int)floorf(x + sLayerShift), y, (int)ceilf(x + w + sLayerShift), y + h);
    HudDrawPicture(k, left + CTR_VIEW_X + sLayerShift, y + CTR_VIEW_Y, 1.0f / CTR_BATTLE_ZOOM, true);
    C2D_Flush();
    RestoreScissor();
    BlendForget();
    ++sStats.sprites;
}

/* Every box's picture over the scene at the screen's own pixels, the GBA's
 * order kept: the lower OAM entry in front. */
enum { HUD_FLAT, HUD_SPRITE_DEPTH, HUD_SCENERY_DEPTH };

static void HudDrawCrisp(unsigned depth)
{
    bool any = false;

    if (!sHud || !sHudCrisp) return;
    C2D_Flush();
    C2D_ViewReset();
    for (int i = 127; i >= 0; --i)
    {
        unsigned p;
        int x, y, w, h, k = HudLeftHalf((unsigned)i);
        float shift = 0;

        if (k < 0 || !HudEntry((unsigned)i, &p, &x, &y, &w, &h))
            continue;
        /* With the 3D slider up, this eye's: where the box's sprites would
         * be (BattleSpriteShift, as the slots place them), or the scenery's
         * when the scene is put behind the screen whole. Whole pixels. */
        if (depth == HUD_SPRITE_DEPTH) shift = BattleSpriteShift(y + h / 2);
        else if (depth == HUD_SCENERY_DEPTH) shift = BattleSceneryShift();
        if (!any) HudBlend(4, true);
        any = true;
        HudDrawPicture((unsigned)k, roundf((x + CTR_VIEW_X) * sZoom + sOffX + shift),
                       roundf((y + CTR_VIEW_Y) * sZoom + sOffY), 1, false);
        ++sStats.sprites;
    }
    C2D_Flush();
    if (any) BlendForget();
    ViewBase();
}

/*
 * The battle text box (3ds_battle_hud.h, gen_battle_hud.py): a panel floating
 * over the scene with margins at its sides, the game's message window over
 * it 1:1 - its text, the window's fill left out - moved a few pixels right
 * to keep its distance from the panel's edge. Only while BG0 shows the
 * message box where battle_bg.c puts it; anything else (the GBA's menus, in
 * a link battle) is drawn as the GBA has it.
 */
#define BOX_TEX_W 32       /* the panel's art: its left cap, and its middle column */
#define BOX_TEX_H 64
#define BOX_MIDDLE_X 28
#define TEXT_TEX_W 256     /* the message window's 208x32 */
#define TEXT_TEX_H 32
#define BOX_TEXT_SHIFT 4
#define BOX_SLIDE_FRAMES 32
#define BOX_HIDDEN 60.0f
/* The player's trainer under it meanwhile, in GBA pixels: below the screen. */
#define BOX_TRAINER_HIDDEN 90.0f
static C3D_Tex sBoxTex, sTextTex;
static bool sBoxTexFailed;
static int sBoxCapW, sBoxArtH, sBoxArtY;
static uint8_t sBoxWindow[26 * 4 * 32];
static uint16_t sBoxEntries[26 * 4], sBoxColours[16];
static bool sBoxText;     /* the message window shows something */
/* A battle over the voxel world: the box waits out of sight while the
 * camera flies in, and slides in when the first message comes. */
static bool sBoxHidden, sBoxDecided, sBoxBarsUp;
/* The frame the HD text box was last drawn (or kept out of sight) in. */
static uint32_t sBoxShown = ~0u;
/* The cinema bars this far out of their way: the box comes in with them. */
#define BOX_BARS_LEAVING 0.35f
static int sBoxSlide = -1;
static uint32_t sBoxBattleFrames;

static bool BoxTexture(void)
{
    const uint32_t *cap, *middle;
    int cx, cy, cw, ch, mx, my, mw, mh;
    uint32_t *data;

    if (sBoxTex.data && sTextTex.data) return true;
    if (sBoxTexFailed || !CtrBattleHud_Ready()) return false;
    cap = CtrBattleHud_BoxArt(false, &cx, &cy, &cw, &ch);
    middle = CtrBattleHud_BoxArt(true, &mx, &my, &mw, &mh);
    if (!cap || !middle || cw > BOX_MIDDLE_X || ch > BOX_TEX_H || mh != ch)
    {
        sBoxTexFailed = true;
        return false;
    }
    if (!C3D_TexInit(&sBoxTex, BOX_TEX_W, BOX_TEX_H, GPU_RGBA8)
        || !C3D_TexInit(&sTextTex, TEXT_TEX_W, TEXT_TEX_H, GPU_RGBA4))
    {
        if (sBoxTex.data) C3D_TexDelete(&sBoxTex);
        memset(&sBoxTex, 0, sizeof(sBoxTex));
        memset(&sTextTex, 0, sizeof(sTextTex));
        sBoxTexFailed = true;
        CtrLog_Write(CTR_LOG_ERROR, "video: no memory for the HD text box (linear free=%lu); the GBA's is shown",
                     (unsigned long)linearSpaceFree());
        return false;
    }
    C3D_TexSetFilter(&sBoxTex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&sBoxTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexSetFilter(&sTextTex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&sTextTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    data = sBoxTex.data;
    memset(data, 0, BOX_TEX_W * BOX_TEX_H * 4);
    for (int y = 0; y < ch; ++y)
    {
        for (int x = 0; x < cw; ++x)
            data[CtrVideo_Texel(x, y, BOX_TEX_W)] = cap[y * cw + x];
        data[CtrVideo_Texel(BOX_MIDDLE_X, y, BOX_TEX_W)] = middle[y * mw];
    }
    C3D_TexFlush(&sBoxTex);
    memset(sTextTex.data, 0, TEXT_TEX_W * TEXT_TEX_H * 2);
    C3D_TexFlush(&sTextTex);
    sBoxCapW = cw;
    sBoxArtH = ch;
    sBoxArtY = cy;
    memset(sBoxEntries, 0xFF, sizeof(sBoxEntries));
    return true;
}

/* The message window's 26x4 cells into the texture, when they changed. */
static void BoxWindowSync(unsigned map, unsigned size, unsigned chars)
{
    bool changed = memcmp(sBoxColours, (const uint16_t *)sTexturePalette, sizeof(sBoxColours)) != 0;
    uint16_t *data = sTextTex.data;

    for (unsigned i = 0; i < 26 * 4 && !changed; ++i)
    {
        unsigned entry = MapEntry(map, size, 2 + i % 26, 15 + i / 26);
        unsigned address = chars + (entry & 1023) * 32;

        if (entry != sBoxEntries[i] || address + 32 > 0x10000
            || memcmp(sBoxWindow + i * 32, sMemory.vram + address, 32))
            changed = true;
    }
    if (!changed) return;
    memcpy(sBoxColours, (const uint16_t *)sTexturePalette, sizeof(sBoxColours));
    sBoxText = false;
    for (unsigned i = 0; i < 26 * 4; ++i)
    {
        unsigned entry = MapEntry(map, size, 2 + i % 26, 15 + i / 26);
        unsigned address = chars + (entry & 1023) * 32, bank = entry >> 12;
        unsigned cx = (i % 26) * 8, cy = (i / 26) * 8;

        sBoxEntries[i] = (uint16_t)entry;
        if (address + 32 > 0x10000) address = 0;
        memcpy(sBoxWindow + i * 32, sMemory.vram + address, 32);
        for (unsigned p = 0; p < 64; ++p)
        {
            unsigned px = p & 7, py = p >> 3;
            unsigned sx = (entry & 1024) ? 7 - px : px, sy = (entry & 2048) ? 7 - py : py;
            unsigned index = (sBoxWindow[i * 32 + sy * 4 + sx / 2] >> ((sx & 1) * 4)) & 15;
            uint16_t texel = 0;

            /* The window's fill (index 15) and nothing (0) are the panel. */
            if (index && index != 15)
            {
                uint16_t c = sTexturePalette[bank * 16 + index];

                texel = (uint16_t)(((c >> 12) & 15) << 12 | ((c >> 7) & 15) << 8 | ((c >> 2) & 15) << 4 | 15);
                sBoxText = true;
            }
            data[CtrVideo_Texel(cx + px, cy + py, TEXT_TEX_W)] = texel;
        }
    }
    C3D_TexFlush(&sTextTex);
}

/* How far below its place the text box is this frame (its slide in). */
static float BoxOffset(void)
{
    float t;

    if (sBoxHidden) return BOX_HIDDEN;
    if (sBoxSlide < 0) return 0;
    /* An ease out, slowing to rest without overshoot. */
    t = 1.0f - (float)sBoxSlide / BOX_SLIDE_FRAMES;
    return BOX_HIDDEN * t * t * t;
}

/* Once a frame: whether the box waits out of sight, and its slide. */
static void BoxFrame(void)
{
    static bool sliding;
    bool slidingNow = false;

    if (!sBattle)
    {
        sBoxHidden = sliding = false;
        /* The party or the bag over the battle: back from them, the box is
         * where it was, not slid in again as for a new battle. */
        if (!sBoxDecided || !CtrVoxel_InBattle())
            sBoxDecided = sBoxBarsUp = false;
        sBoxSlide = -1;
        sBoxBattleFrames = 0;
        /* The next battle's message window is read afresh, and the last
         * one's text cleared: the next opens on the empty box, which only
         * clears the texture when it held a window. */
        sBoxText = false;
        if (sBoxEntries[0] != 0xFFFF && sTextTex.data)
        {
            memset(sTextTex.data, 0, TEXT_TEX_W * TEXT_TEX_H * 2);
            C3D_TexFlush(&sTextTex);
        }
        memset(sBoxEntries, 0xFF, sizeof(sBoxEntries));
        return;
    }
    ++sBoxBattleFrames;
    if (sBoxSlide >= 0 && ++sBoxSlide > BOX_SLIDE_FRAMES) sBoxSlide = -1;
    /* Only a battle over the voxel world, decided as it begins: the world is
     * there from its first frames, the camera's flight a little later. */
    if (!sBoxDecided)
    {
        /* Through the fade in, until there is a picture: the world, or a
         * message already in a 2D battle. */
        if (sBattleWorld && !sBoxText) sBoxHidden = true;
        if (sBattleWorld || sBoxText) sBoxDecided = true;
    }
    if (sBoxHidden)
    {
        bool barsGone = false;

        slidingNow = CtrBattleIntro_Sliding() != 0;
        {
            CtrVoxelBattleIntro intro;

            CtrVoxel_BattleIntro(&intro);
            if (intro.bars > 0.9f) sBoxBarsUp = true;
            barsGone = sBoxBarsUp && intro.bars < BOX_BARS_LEAVING;
        }
        /* In as the camera lands: the cinema bars leaving, the intro's
         * scenery done sliding, the first message begun, or, whatever
         * happens, after ten seconds. */
        if (barsGone || sBoxText || (sliding && !slidingNow) || sBoxBattleFrames > 600)
        {
            sBoxHidden = false;
            sBoxSlide = 0;
        }
        sliding = slidingNow;
    }
    /*
     * The player's trainer goes with it: its picture ends where the box
     * covers it. Out of sight while the box is (all of it: the picture is
     * taller than the box), up with it as it comes, arriving together.
     */
    {
        float offset = BoxOffset();

        CtrBattleHud_LiftPlayerTrainer((int)lroundf(offset > 0 ? offset * (BOX_TRAINER_HIDDEN / BOX_HIDDEN)
                                                               : offset / CTR_BATTLE_ZOOM));
    }
}

/*
 * The panel's art under the fade the message box's palette (BG palette 0) is
 * under, as a tint: the colour it goes towards and how far. A fade of every
 * palette in even steps is already a tint (UpdatePalette, sPaletteFade); the
 * battle's end fades by its own steps (BeginFastPaletteFade), which only the
 * palette shows - the text, drawn in the palette's colours, follows it, and
 * so must the panel. Read from white (colour 1) and the colour furthest from
 * it, as the info boxes are (3ds_battle_hud.c FadeOf).
 */
static void BoxFadeTint(C2D_ImageTint *tint)
{
    const uint16_t *unfaded = gPlttBufferUnfaded, *now = sMemory.palette;
    unsigned a = unfaded[1] & 0x7fff, fa = now[1] & 0x7fff, b = a, fb = fa;
    int best = 0;
    float k = 0, target[3];

    *tint = sTint;
    if (sPaletteFade > 0 || !memcmp(unfaded, now, 32)) return;
    for (unsigned i = 2; i < 16; ++i)
    {
        unsigned c = unfaded[i] & 0x7fff;
        int d = 0;

        for (int ch = 0; ch < 15; ch += 5)
        {
            int e = (int)((a >> ch) & 31) - (int)((c >> ch) & 31);
            d += e < 0 ? -e : e;
        }
        if (d > best)
        {
            best = d;
            b = c;
            fb = now[i] & 0x7fff;
        }
    }
    if (best == 0) return;
    /* How far, from the channel the two differ most in. */
    {
        int most = 0;

        for (int ch = 0; ch < 15; ch += 5)
        {
            int ua = (a >> ch) & 31, ub = (b >> ch) & 31, d = ua > ub ? ua - ub : ub - ua;

            if (d > most)
            {
                most = d;
                k = 1.0f - (float)((int)((fa >> ch) & 31) - (int)((fb >> ch) & 31)) / (float)(ua - ub);
            }
        }
    }
    if (k < 0.02f) return;
    if (k > 1) k = 1;
    /* Towards what: white's channels solved for it. */
    for (int ch = 0; ch < 3; ++ch)
    {
        float u = (a >> (ch * 5)) & 31, f = (fa >> (ch * 5)) & 31, t = (f - u * (1 - k)) / k;

        target[ch] = t < 0 ? 0 : t > 31 ? 255 : t * 255 / 31;
    }
    /*
     * How far the panel itself is, by its own colour (the window's fill,
     * colour 15): a fade in even steps takes every colour the same part of
     * the way, the battle's end takes the same amount off each channel, which
     * brings the panel's mid tones down well before white.
     */
    {
        unsigned u = unfaded[15] & 0x7fff, f = now[15] & 0x7fff;
        float moved = 0, way = 0;

        for (int ch = 0; ch < 3; ++ch)
        {
            float uc = (u >> (ch * 5)) & 31, fc = (f >> (ch * 5)) & 31, tc = target[ch] * 31 / 255;

            moved += fabsf(fc - uc);
            way += fabsf(tc - uc);
        }
        if (way > 0.5f)
            k = moved / way > 1 ? 1 : moved / way;
    }
    C2D_PlainImageTint(tint, C2D_Color32((uint8_t)target[0], (uint8_t)target[1], (uint8_t)target[2], 255), k);
}

/* The text box in HD, if BG0 shows the message box; false to draw the GBA's. */
static bool DrawBattleTextHd(unsigned bg)
{
    unsigned control = Reg(8 + bg * 2), size = control >> 14;
    unsigned map = ((control >> 8) & 31) * 0x800;
    unsigned chars = ((control >> 2) & 3) * 0x4000;
    unsigned scrollX = Reg(0x10 + bg * 4) & 511, scrollY = Reg(0x12 + bg * 4) & 511;
    int down = scrollY >= 256 ? 512 - (int)scrollY : -(int)scrollY;
    float y0, shift = sLayerShift;
    bool empty;

    if (bg != 0 || scrollX != 0 || down < 0 || down > 64 || (control & 128)) return false;
    /* The message window put on the box; or not yet, through the intro: the
     * box's own frame (textbox_map.bin) round its plain fill. */
    if ((MapEntry(map, size, 2, 15) & 1023) == 0x90 && (MapEntry(map, size, 27, 18) & 1023) == 0x90 + 103)
        empty = false;
    else if ((MapEntry(map, size, 2, 15) & 1023) == 0x0a && (MapEntry(map, size, 27, 18) & 1023) == 0x0a
             && (MapEntry(map, size, 0, 14) & 1023) == 0x03 && (MapEntry(map, size, 29, 19) & 1023) == 0x11)
        empty = true;
    else
        return false;
    if (!BoxTexture()) return false;
    if (empty)
    {
        /* Nothing to show; the window is read afresh when it is put there. */
        if (sBoxEntries[0] != 0xFFFF)
        {
            memset(sBoxEntries, 0xFF, sizeof(sBoxEntries));
            memset(sTextTex.data, 0, TEXT_TEX_W * TEXT_TEX_H * 2);
            C3D_TexFlush(&sTextTex);
        }
        sBoxText = false;
    }
    else
        BoxWindowSync(map, size, chars);

    /* Above the text box: windows, where the game put them. */
    {
        uint32_t before = sStats.tiles;
        DrawBattleCut(bg, 0, 240, 0, BATTLE_BAND_TOP, 0);
        if (sStats.tiles != before) sTextAbove = true;
    }
    y0 = sBoxArtY + down + BoxOffset();
    sBoxShown = sStats.frames;
    if (y0 >= CTR_GAME_HEIGHT) return true;
    /* Layers set the blend for BG0: its effects bit is in the key. */
    HudBlend(bg, (sBlendKey >> 8) & 1);
    ViewBase();
    {
        C2D_ImageTint art;
        const float cap = (float)sBoxCapW, h = (float)sBoxArtH;
        const Tex3DS_SubTexture left = {(u16)sBoxCapW, (u16)sBoxArtH, 0, 1, cap / BOX_TEX_W, 1 - h / BOX_TEX_H};
        const Tex3DS_SubTexture right = {(u16)sBoxCapW, (u16)sBoxArtH, cap / BOX_TEX_W, 1, 0, 1 - h / BOX_TEX_H};
        const float mu = (BOX_MIDDLE_X + 0.5f) / BOX_TEX_W;
        const Tex3DS_SubTexture middle = {1, (u16)sBoxArtH, mu, 1, mu, 1 - h / BOX_TEX_H};
        const Tex3DS_SubTexture text = {208, 32, 0, 1, 208.0f / TEXT_TEX_W, 0};

        BoxFadeTint(&art);

        C2D_DrawImageAt((C2D_Image){&sBoxTex, &left}, shift, y0, 0, &art, 1, 1);
        C2D_DrawImageAt((C2D_Image){&sBoxTex, &middle}, cap + shift, y0, 0, &art, CTR_GAME_WIDTH - 2 * cap, 1);
        C2D_DrawImageAt((C2D_Image){&sBoxTex, &right}, CTR_GAME_WIDTH - cap + shift, y0, 0, &art, 1, 1);
        /* The message window: GBA (16, 120), on the screen where the band puts it. */
        C2D_DrawImageAt((C2D_Image){&sTextTex, &text}, 16 + BOX_TEXT_SHIFT + shift,
                        (CTR_GAME_HEIGHT - 48) + 8 + down + BoxOffset(), 0, &sTint, 1, 1);
        sStats.tiles += 4;
    }
    C2D_Flush();
    BlendForget();
    return true;
}

static void DrawBattleText(unsigned bg)
{
    unsigned control = Reg(8 + bg * 2), size = control >> 14;
    unsigned map = ((control >> 8) & 31) * 0x800;
    unsigned chars = ((control >> 2) & 3) * 0x4000;
    bool color256 = (control & 128) != 0;
    unsigned scrollX = Reg(0x10 + bg * 4) & 511, scrollY = Reg(0x12 + bg * 4) & 511;
    unsigned rows = (size & 2) ? 64 : 32;
    /* The column under the left edge of the picture: the left cap. */
    unsigned first = scrollX >> 3;
    int ox = (int)(scrollX & 7), oy = (int)(scrollY & 7);
    int fill0 = 240 - BATTLE_BAND_CAP + VIEW_LEFT, fill1 = VIEW_RIGHT - BATTLE_BAND_CAP;
    int x0 = fill0, x1 = fill1, y0 = BATTLE_BAND_TOP, y1 = 160;

    if (DrawBattleTextHd(bg)) return;
    /* Above the text box: windows, where the game put them. */
    uint32_t before = sStats.tiles;
    DrawBattleCut(bg, 0, 240, 0, BATTLE_BAND_TOP, 0);
    if (sStats.tiles != before) sTextAbove = true;
    /* The box from its left cap, on the left edge of the screen... */
    DrawBattleCut(bg, VIEW_LEFT, fill0, BATTLE_BAND_TOP, 160, VIEW_LEFT);
    /* ...its right cap on the right edge... */
    DrawBattleCut(bg, fill1, VIEW_RIGHT, BATTLE_BAND_TOP, 160, VIEW_RIGHT - 240);
    /*
     * ...and its middle in between. A row is a frame of caps and a middle:
     * the five-tile rows of the message box have two cap tiles and then the
     * middle (numbered one after the cap), the three-tile rows of the menu
     * boxes one cap and then the middle itself. Column 2 tells which, since
     * the message window starts there and never holds the middle tile.
     */
    if (!ClipCell(&x0, &x1, &y0, &y1)) return;
    C2D_Flush();
    Scissor(x0, y0, x1, y1);
    ViewBase();
    for (int py = ((y0 + oy) & ~7) - oy; py < y1; py += 8)
    {
        unsigned row = ((unsigned)(py + (int)scrollY) >> 3) & (rows - 1);
        unsigned cap = MapEntry(map, size, first, row), inner = MapEntry(map, size, first + 1, row);
        unsigned middle, next = MapEntry(map, size, first + 2, row);
        int slot;

        if (!cap && !inner) continue;
        middle = next == inner ? inner : (inner & ~1023u) | ((inner + 1) & 1023);
        slot = EntrySlot(chars, middle, color256);
        if (slot < 0) continue;
        for (int px = ((x0 + ox) & ~7) - ox; px < x1; px += 8)
            DrawSlot(slot, px + CTR_VIEW_X + sLayerShift, py + CTR_VIEW_Y, middle & 1024, middle & 2048);
    }
    C2D_Flush();
    RestoreScissor();
}

/*
 * The level-up box (battle_script_commands.c, Cmd_drawlvlupbox): BG1 put in
 * front of BG0 while it is up. Magnified with the scene, it went down past
 * the screen's edge and the text box, drawn after the scene, covered it; it
 * goes with the text box instead, at 1:1 over it, its right edge on the
 * screen's as the GBA's is on the box's (DrawBattleTextLayer).
 */
static unsigned BattleBoxLayers(void)
{
    if (!sBattle || !(Reg(0) & 0x200) || (Reg(0xa) & 3) >= (Reg(8) & 3))
        return 0;
    return 1u << 1;
}

/*
 * The text box is not magnified: it keeps its own pixels on the bottom 48
 * lines, under a scene composed at CTR_BATTLE_ZOOM. The rectangle being
 * composed is carried from the scene's coordinates to the box's through the
 * screen, drawn there, and put back. The level-up box the same way, whole.
 */
static void DrawBattleTextLayer(unsigned bg)
{
    int viewX = sViewX, viewY = sViewY;
    float zoom = sZoom, offX = sOffX, offY = sOffY;
    int clipX0 = sClipX0, clipY0 = sClipY0, clipX1 = sClipX1, clipY1 = sClipY1;

    float shift = sLayerShift;

    sZoom = 1.0f;
    sOffX = sOffY = 0.0f;
    sLayerShift = shift * sShiftZoom;
    sViewX = CTR_BATTLE_X;
    sViewY = CTR_BATTLE_Y;
    sClipX0 = (int)roundf((clipX0 + viewX) * zoom + offX) - sViewX;
    sClipX1 = (int)roundf((clipX1 + viewX) * zoom + offX) - sViewX;
    sClipY0 = (int)roundf((clipY0 + viewY) * zoom + offY) - sViewY;
    sClipY1 = (int)roundf((clipY1 + viewY) * zoom + offY) - sViewY;
    if (bg == 0)
        DrawBattleText(bg);
    else
        DrawBattleCut(bg, VIEW_RIGHT - 240, VIEW_RIGHT, 0, 160, VIEW_RIGHT - 240);
    C2D_Flush();
    sZoom = zoom;
    sOffX = offX;
    sOffY = offY;
    sLayerShift = shift;
    sViewX = viewX;
    sViewY = viewY;
    sClipX0 = clipX0;
    sClipY0 = clipY0;
    sClipX1 = clipX1;
    sClipY1 = clipY1;
    RestoreScissor();
    ViewBase();
}

/* Whether a tile row of a background has anything to show across the screen. */
static bool BattleRowShown(unsigned map, unsigned size, unsigned chars, bool color256,
                           unsigned row, unsigned column, unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
        if (EntrySlot(chars, MapEntry(map, size, column + i, row), color256) >= 0)
            return true;
    return false;
}

/*
 * The battle terrain split at CTR_BATTLE_SIDE_SPLIT (ctr_battle_layout.h):
 * the opponent's half above it drawn CTR_BATTLE_ENEMY_RISE lines higher, the
 * player's half below it CTR_BATTLE_PLAYER_DROP lower, as the game moves the
 * two sides' sprites. Only while BG3 holds the terrain, not a move's
 * background, which is drawn for the whole screen.
 */
static bool BattleTerrainSplit(unsigned bg)
{
    return sBattle && bg == 3 && !CtrBattleBg_MoveBgShown();
}

/*
 * The terrain line shown on line y of the split terrain. Between the two
 * halves the lines are filled from the four just below the split - open
 * ground on every terrain, the opponent's platform ending above it and the
 * player's starting well below - repeated: the terrain's stripes repeat
 * every four lines, so they carry on the half below without a seam.
 */
static int BattleTerrainLine(int y)
{
    const int top = CTR_BATTLE_SIDE_SPLIT - CTR_BATTLE_ENEMY_RISE;
    const int bottom = CTR_BATTLE_SIDE_SPLIT + CTR_BATTLE_PLAYER_DROP;

    if (y < top) return y + CTR_BATTLE_ENEMY_RISE;
    if (y < bottom) return CTR_BATTLE_SIDE_SPLIT + ((y - bottom) & 3);
    /* The terrain's map is empty from line 112, where the GBA's text box
     * began: the scene goes on below it with its last four lines. */
    if (y - CTR_BATTLE_PLAYER_DROP >= BATTLE_BAND_TOP)
        return BATTLE_BAND_TOP - 4 + ((y - CTR_BATTLE_PLAYER_DROP - BATTLE_BAND_TOP) & 3);
    return y - CTR_BATTLE_PLAYER_DROP;
}

/*
 * A move's background that is one picture for the GBA's screen - Psychic,
 * Thunder, Ice, Dark, Solar Beam, the impacts: art over the 240x160 picture
 * and nothing in the rows the GBA never shows - has nothing of its own to
 * carry into the margins the battle has round that picture: the GBA's wrap
 * left bands above it and at its sides, or its top row repeated. Such a layer
 * is drawn magnified instead, uniformly, to cover the whole top screen with a
 * few pixels to spare for the 3D slider's displacement: a GBA point p at
 * f + k (p - f). The fixed point f is the lowest that still brings the
 * picture's top edge over the screen's, so the battlers' part of the art
 * moves as little as it can. Backgrounds made to wrap (Sandstorm, the
 * water, the fog) keep the wrap, which is art; a battler copied into a
 * background covers a corner of the picture, not all of it, and keeps its
 * place; the terrain is never one.
 */
#define BATTLE_PICTURE_SPARE 4.0f
#define BATTLE_PICTURE_SCALE ((CTR_GAME_WIDTH / CTR_BATTLE_ZOOM + 2 * BATTLE_PICTURE_SPARE) / 240.0f)
/* GBA line on the screen's top edge: (120, 112) is on screen (200, 192). */
#define BATTLE_PICTURE_SCREEN_TOP (112.0f - (CTR_GAME_HEIGHT - 48) / CTR_BATTLE_ZOOM)
#define BATTLE_PICTURE_FIX_Y ((BATTLE_PICTURE_SCREEN_TOP - BATTLE_PICTURE_SPARE) / (1.0f - BATTLE_PICTURE_SCALE))

static bool BattlePicture(unsigned bg)
{
    static uint32_t frame[4];
    static bool picture[4];
    unsigned control, size, map, chars, scrollX, scrollY, rows;
    unsigned shown = 0, hidden = 0, hiddenTiles = 0, lastEntry = ~0u;
    bool color256, lastFull = false;

    if (!sBattle || bg == 0 || bg > 3 || (bg == 3 && !CtrBattleBg_MoveBgShown())) return false;
    /* Once a frame: the frame's number, one on so that none is 0. */
    if (frame[bg] == sStats.frames + 1) return picture[bg];
    frame[bg] = sStats.frames + 1;
    control = Reg(8 + bg * 2);
    size = control >> 14;
    map = ((control >> 8) & 31) * 0x800;
    chars = ((control >> 2) & 3) * 0x4000;
    color256 = (control & 128) != 0;
    scrollX = Reg(0x10 + bg * 4) & 511;
    scrollY = Reg(0x12 + bg * 4) & 511;
    rows = (size & 2) ? 64 : 32;
    /* The picture's 30x20 tiles, and below it the rows the GBA never shows. */
    for (unsigned r = 0; r < rows; ++r)
        for (unsigned c = 0; c < 30; ++c)
        {
            unsigned entry = MapEntry(map, size, (scrollX >> 3) + c, (scrollY >> 3) + r);

            if (entry != lastEntry)
            {
                lastEntry = entry;
                lastFull = EntrySlot(chars, entry, color256) >= 0;
            }
            if (r < 20) shown += lastFull;
            else
            {
                hidden += lastFull;
                ++hiddenTiles;
            }
        }
    picture[bg] = shown * 5 >= 600 * 4 && hidden * 10 <= hiddenTiles;
    return picture[bg];
}

typedef struct
{
    float zoom, offX, offY;
    int clipX0, clipY0, clipX1, clipY1;
    bool scissored;
} BattlePictureView;

/*
 * The magnified view of a picture layer (BattlePicture), or false to draw it
 * as it is. The clip becomes the part of the picture that lands in it, and
 * the scissor keeps what is drawn to where it was; BattlePictureEnd after.
 */
static bool BattlePictureBegin(unsigned bg, BattlePictureView *saved)
{
    const float k = BATTLE_PICTURE_SCALE, fx = 120.0f, fy = BATTLE_PICTURE_FIX_Y;

    if (!BattlePicture(bg)) return false;
    *saved = (BattlePictureView){sZoom, sOffX, sOffY, sClipX0, sClipY0, sClipX1, sClipY1, sScissored};
    C2D_Flush();
    Scissor(sClipX0, sClipY0, sClipX1, sClipY1);
    sScissored = true;
    sClipX0 = (int)floorf(fx + (saved->clipX0 - fx) / k);
    sClipX1 = (int)ceilf(fx + (saved->clipX1 - fx) / k);
    sClipY0 = (int)floorf(fy + (saved->clipY0 - fy) / k);
    sClipY1 = (int)ceilf(fy + (saved->clipY1 - fy) / k);
    if (sClipX0 < 0) sClipX0 = 0;
    if (sClipY0 < 0) sClipY0 = 0;
    if (sClipX1 > 240) sClipX1 = 240;
    if (sClipY1 > 160) sClipY1 = 160;
    /* A point drawn at p + view lands at off + zoom (k (p + view) + (1 - k) (f + view)). */
    sOffX += sZoom * (1.0f - k) * (fx + CTR_VIEW_X);
    sOffY += sZoom * (1.0f - k) * (fy + CTR_VIEW_Y);
    sZoom *= k;
    return true;
}

static void BattlePictureEnd(const BattlePictureView *saved)
{
    C2D_Flush();
    sZoom = saved->zoom;
    sOffX = saved->offX;
    sOffY = saved->offY;
    sClipX0 = saved->clipX0;
    sClipY0 = saved->clipY0;
    sClipX1 = saved->clipX1;
    sClipY1 = saved->clipY1;
    sScissored = saved->scissored;
    ViewBase();
    RestoreScissor();
}

/* Lines the tile walk below shows moved by: the line it draws at y is y + this. */
static int sBattleBgLineShift;

static void DrawBattleBg(unsigned bg)
{
    unsigned control = Reg(8 + bg * 2), size = control >> 14;
    unsigned map = ((control >> 8) & 31) * 0x800;
    unsigned chars = ((control >> 2) & 3) * 0x4000;
    bool color256 = (control & 128) != 0;
    unsigned scrollX = Reg(0x10 + bg * 4) & 511, scrollY = (Reg(0x12 + bg * 4) + sBattleBgLineShift) & 511;
    unsigned columns = (size & 1) ? 64 : 32, rows = (size & 2) ? 64 : 32;
    int ox = (int)(scrollX & 7), oy = (int)(scrollY & 7);
    int x0 = sClipX0, x1 = sClipX1, y0 = sClipY0, y1 = sClipY1;
    unsigned firstColumn, count;
    unsigned lastEntry = ~0u;
    int lastSlot = -1;
    bool wraps = columns == 64;
    BattlePictureView picture;

    if (BattlePictureBegin(bg, &picture))
    {
        if (sClipX0 < sClipX1 && sClipY0 < sClipY1)
            DrawTextSpanAt(bg, sClipX0, sClipX1, sClipY0, sClipY1, -1, -1);
        BattlePictureEnd(&picture);
        return;
    }
    if (!wraps)
    {
        MeasureStill(bg);
        wraps = sPanorama[bg];
    }
    if (!wraps)
    {
        DrawBattleCut(bg, 0, 240, 0, 160, 0);
        return;
    }
    if (!ClipCell(&x0, &x1, &y0, &y1)) return;
    firstColumn = (unsigned)(((x0 + ox) & ~7) - ox + (int)scrollX) >> 3;
    count = (unsigned)(x1 - (((x0 + ox) & ~7) - ox) + 7) / 8 + 1;
    /*
     * Above the picture a layer only carries on if its art reaches the top
     * edge. The entry grass of the intro is a band that slides down out of
     * the picture: its map wraps it round to the top, which the GBA never
     * shows and which here repeated the grass along the top of the screen.
     */
    bool above = bg == 3 || BattleRowShown(map, size, chars, color256,
                                           (scrollY >> 3) & (rows - 1), firstColumn, count);
    bool batch;

    ViewBase();
    batch = TileWalkBegin();
    for (int py = ((y0 + oy) & ~7) - oy; py < y1; py += 8)
    {
        int row = (int)(((unsigned)(py + (int)scrollY) >> 3) & (rows - 1));

        if (py + 8 <= 0 && !above) continue;
        /* A row wholly above the picture: where its art comes from. */
        if (py + 8 <= 0 && !BattleRowShown(map, size, chars, color256, row, firstColumn, count))
        {
            if (rows == 64 && BattleRowShown(map, size, chars, color256, row ^ 32, firstColumn, count))
                row ^= 32;
            else if (bg == 3)
                row = (int)((scrollY >> 3) & (rows - 1));
            else
                continue;
        }
        for (int px = ((x0 + ox) & ~7) - ox; px < x1; px += 8)
        {
            unsigned column = ((unsigned)(px + (int)scrollX) >> 3) & (columns - 1);
            unsigned entry = MapEntry(map, size, column, (unsigned)row);

            if (entry != lastEntry)
            {
                lastEntry = entry;
                lastSlot = EntrySlot(chars, entry, color256);
            }
            if (lastSlot >= 0)
                TileWalkSlot(batch, lastSlot, px + CTR_VIEW_X + sLayerShift, py + CTR_VIEW_Y,
                             entry & 1024, entry & 2048);
        }
    }
    TileWalkEnd(batch);
}

/* Lines [y0, y1) of a battle background, each showing the line `shift` below it. */
static void DrawBattleBgBand(unsigned bg, int y0, int y1, int shift)
{
    int x0 = sClipX0, x1 = sClipX1, clipY0 = sClipY0, clipY1 = sClipY1;

    if (!ClipCell(&x0, &x1, &y0, &y1)) return;
    C2D_Flush();
    Scissor(x0, y0, x1, y1);
    sClipY0 = y0;
    sClipY1 = y1;
    sBattleBgLineShift = shift;
    DrawBattleBg(bg);
    sBattleBgLineShift = 0;
    sClipY0 = clipY0;
    sClipY1 = clipY1;
    C2D_Flush();
    RestoreScissor();
}

/* A battle background, the terrain split between the two sides (BattleTerrainLine). */
static void DrawBattleBgSplit(unsigned bg)
{
    int y0 = sClipY0, y1 = sClipY1;

    if (!BattleTerrainSplit(bg))
    {
        DrawBattleBg(bg);
        return;
    }
    /* In runs of lines drawn with the same shift: each half, and four lines
     * at a time between them and below the terrain's end. */
    for (int y = y0; y < y1;)
    {
        int shift = BattleTerrainLine(y) - y, end = y + 1;

        while (end < y1 && BattleTerrainLine(end) - end == shift) ++end;
        DrawBattleBgBand(bg, y, end, shift);
        y = end;
    }
}

/*
 * How a centred screen fills the top screen around its 240x160 picture.
 *
 * - layers: the backgrounds carried out to the edges, each margin tile
 *   repeating the tilemap's edge tile in its row, column or corner - moved
 *   inwards by inset (left, top, right, bottom, in tiles) past a border, so
 *   what repeats is the pattern inside it. The edge is the tilemap's, not the
 *   screen's: when the speech slides its background (the platform moving
 *   aside for the player), the margins keep showing the empty side of it
 *   rather than the platform that slid to the edge. across keeps a fill to
 *   the sides. Those screens' backgrounds are a
 *   plain field or a pattern of one tile, so it carries on as if the screen
 *   were wider: the clock's teal, the starter's meadow, the naming screen's
 *   stripes and title bar, the sky of the professor's speech.
 * - skyRows: the speech's sky, the first rows of its background, drawn from
 *   the top of the screen rather than the top of the picture, so it starts at
 *   the top-left corner and carries on to the right as the rest does.
 * - band: the text box of the professor's speech (BG0 from bandRow down)
 *   drawn moved by (bandX, bandY), to where the overworld shows its text box
 *   (menu.c, sStandardTextBox_WindowTemplates, and CTR_FIELD_UI_SHIFT). It
 *   shares its callback and its BG0 with the title menu, whose windows go
 *   down to the bottom of the screen; the speech is told apart by the
 *   character base its BG0 uses (main_menu.c, sBirchBgTemplate).
 */
typedef struct
{
    uint8_t layers;
    int8_t inset[4];
    /* Only beside the picture, nothing above or below it. */
    bool across;
    /* Instead of layers: the visible background drawn behind the others. */
    bool backmost;
    /* The layers' margins read on through the tilemap, as the GBA wraps it,
     * instead of repeating the picture's edge row and column. */
    bool wrap;
    /* Instead: one tile of the layer, at tileColumn, tileRow, over all the
     * margins - a backdrop of stripes the picture's edges only cut. */
    bool tile;
    uint8_t tileColumn, tileRow;
    /* Or that tilemap entry itself, whatever the tilemap holds now. */
    bool entry;
    uint16_t tileEntry;
    uint8_t skyRows, bandRow, bandChars;
    int8_t bandX, bandY;
} CentredFill;

static const CentredFill sCentredFills[CTR_CENTRED_SCREENS] =
{
    [CTR_CENTRED_MAIN_MENU] = {.layers = 1u << 1, .across = true, .skyRows = 4, .bandRow = 14, .bandChars = 3,
                               .bandX = -4, .bandY = 32},
    /* Inside the yellow border; the title bar is its top row. */
    [CTR_CENTRED_NAMING] = {.layers = 1u << 3, .inset = {2, 0, 2, 1}},
    [CTR_CENTRED_CLOCK] = {.layers = 1u << 3},
    /* The meadow round the bag: one plain green tile at every edge of it. */
    [CTR_CENTRED_STARTER] = {.layers = 1u << 2},
    /* Whatever background is at the back of the PokéNav screen on show
     * (CentredLayers): the dots, the Hoenn sea, the ribbons' wood. */
    [CTR_CENTRED_POKENAV] = {.backmost = true},
    /* The PC's scrolling pattern, which the GBA wraps round its 32x32 map. */
    [CTR_CENTRED_STORAGE] = {.layers = 1u << 3, .wrap = true},
    [CTR_CENTRED_SUMMARY] = {0},
    [CTR_CENTRED_BAG] = {0},
    [CTR_CENTRED_BAG_WHOLE] = {0},
    [CTR_CENTRED_SHOP] = {0},
    /* The Pokédex: a tile its screen on show names (CentredFillOf), or else
     * whatever is at the back of it. */
    [CTR_CENTRED_POKEDEX] = {.backmost = true},
    [CTR_CENTRED_POKEDEX_CAUGHT] = {.layers = 1u << 3, .tile = true, .tileColumn = 0, .tileRow = 2},
    [CTR_CENTRED_PARTY] = {0},
    [CTR_CENTRED_PARTY_WHOLE] = {0},
    /* The trade menu's two layers of stripes, which repeat across. */
    [CTR_CENTRED_TRADE_MENU] = {.layers = (1u << 2) | (1u << 3), .wrap = true},
    /* The trade itself on its black. */
    [CTR_CENTRED_TRADE] = {0},
};

int CtrPokenavList_Bg(void);
/* pokedex.c: the tile of a layer the Pokédex's screen on show carries out to
 * the edges, as bg << 16 | column << 8 | row, or with 1 << 20 as
 * bg << 16 | the tilemap entry; -1 when it names none. */
int CtrPokedex_Backdrop(void);

/* How a centred screen's margins are filled. The Pokédex's depend on which of
 * its screens is up: its stripes, the search's green, the area map's sea. */
static const CentredFill *CentredFillOf(unsigned screen)
{
    static CentredFill dex;
    int backdrop;

    if (screen != CTR_CENTRED_POKEDEX || (backdrop = CtrPokedex_Backdrop()) < 0) return &sCentredFills[screen];
    dex = (CentredFill){.layers = (uint8_t)(1u << ((backdrop >> 16) & 3)), .tile = true,
                        .tileColumn = (uint8_t)(backdrop >> 8), .tileRow = (uint8_t)backdrop,
                        .entry = (backdrop & (1 << 20)) != 0, .tileEntry = (uint16_t)backdrop};
    return &dex;
}

/* The backgrounds a centred screen carries out to its edges. */
static unsigned CentredLayers(const CentredFill *fill)
{
    unsigned display = Reg(0), best = 4, priority = 0;
    int list;

    if (!fill->backmost) return fill->layers;
    if ((display & 7) > 2) return 0;
    /* A PokéNav list scrolls behind the screen's frame: the frame is what
     * goes on past the picture, not the list's next entries. */
    list = CtrPokenavList_Bg();
    for (unsigned bg = 0; bg < 4; ++bg)
        if ((display & (0x100u << bg)) && (Reg(8 + bg * 2) & 3) >= priority && (int)bg != list
         && !((display & 7) == 1 && bg == 3) && !((display & 7) == 2 && bg < 2))
        {
            priority = Reg(8 + bg * 2) & 3;
            best = bg;
        }
    return best < 4 ? 1u << best : 0;
}

static void DrawCentredSpan(unsigned bg, int left, int right, int top, int bottom)
{
    if (left < sClipX0 - sSpanShiftX) left = sClipX0 - sSpanShiftX;
    if (right > sClipX1 - sSpanShiftX) right = sClipX1 - sSpanShiftX;
    if (top < sClipY0 - sSpanShiftY) top = sClipY0 - sSpanShiftY;
    if (bottom > sClipY1 - sSpanShiftY) bottom = sClipY1 - sSpanShiftY;
    if (left < right && top < bottom) DrawTextSpan(bg, left, right, top, bottom);
}

static int FloorDiv8(int value)
{
    return value >= 0 ? value / 8 : -((7 - value) / 8);
}

/* The margins of the picture's rows from, up to to (beside it: across). */
static void DrawCentredMargins(unsigned bg, const CentredFill *fill, int from, int to)
{
    unsigned control = Reg(8 + bg * 2), size = control >> 14;
    unsigned map = ((control >> 8) & 31) * 0x800;
    unsigned chars = ((control >> 2) & 3) * 0x4000;
    bool color256 = (control & 128) != 0;
    unsigned scrollX = Reg(0x10 + bg * 4) & 511, scrollY = Reg(0x12 + bg * 4) & 511;
    unsigned mask = (size & 1) ? 63u : 31u, rows = (size & 2) ? 63u : 31u;
    int ox = (int)(scrollX & 7), oy = (int)(scrollY & 7);
    bool batch;

    ViewBase();
    batch = TileWalkBegin();
    if (from < FloorDiv8(sClipY0 - sSpanShiftY + oy)) from = FloorDiv8(sClipY0 - sSpanShiftY + oy);
    for (int w = from; w < to && w * 8 - oy + sSpanShiftY < sClipY1; ++w)
    {
        bool beside = w >= 0 && w < 20;
        unsigned row = fill->tile ? fill->tileRow
                     : fill->wrap ? (unsigned)(w + (int)(scrollY >> 3)) & rows
                     : w < 0 ? (unsigned)fill->inset[1] : w >= 20 ? 19u - (unsigned)fill->inset[3]
                     : (unsigned)(w + (int)(scrollY >> 3));
        unsigned rowBase;

        if (!beside && fill->across) continue;
        rowBase = map + CtrVideo_TextMapOffset(0, row, size);
        for (int v = FloorDiv8(sClipX0 + ox); v * 8 - ox < sClipX1; ++v)
        {
            bool inside = v >= 0 && v < 30;
            unsigned column, entry, address;
            int slot;

            if (inside && beside) continue;
            column = fill->tile ? fill->tileColumn
                   : fill->wrap ? ((unsigned)v + (scrollX >> 3)) & mask
                   : v < 0 ? (unsigned)fill->inset[0] : v >= 30 ? 29u - (unsigned)fill->inset[2]
                   : ((unsigned)v + (scrollX >> 3)) & mask;
            entry = fill->entry ? fill->tileEntry : Read16(rowBase + (column & 31) * 2 + (column >> 5) * 2048);
            address = chars + (entry & 1023) * (color256 ? 64 : 32);
            if (address >= 0x10000) continue;
            slot = GetTileSlot(address, entry >> 12, color256);
            if (slot >= 0)
                TileWalkSlot(batch, slot, v * 8 - ox + CTR_VIEW_X + sLayerShift,
                             w * 8 - oy + sSpanShiftY + CTR_VIEW_Y, entry & 1024, entry & 2048);
        }
    }
    TileWalkEnd(batch);
}

static void DrawLayerRect(unsigned bg, int x0, int x1, int y0, int y1,
                          float sx, float sy, float dx, float dy, bool fade, int top, int bottom);

/* The PokéNav keeps to its tiles: no layer textures for it. */
static bool CentredTexture(unsigned bg)
{
    return sCentredScreen != CTR_CENTRED_POKENAV && LayerDrawable(bg);
}

/*
 * Lines [top, bottom) of a centred screen's picture: from the layer's texture
 * when it has one (LayersPrepare), one quad where the tiles are 30 per line.
 * Walked tile by tile, the PC's four layers were ~3000 quads a frame and 30
 * fps on an Old 3DS; the bag's three, 1800 and a frame dropped every second.
 */
static void DrawCentredPicture(unsigned bg, int top, int bottom)
{
    if (!CentredTexture(bg) || sSpanShiftX || sSpanShiftY)
    {
        DrawCentredSpan(bg, 0, 240, top, bottom);
        return;
    }
    ViewBase();
    DrawLayerRect(bg, 0, 240, top, bottom, 0, (float)top, 1, 1, false, 0, 0);
}

/*
 * The margins of a layer that reads on through its tilemap (fill->wrap): the
 * texture repeats as the tilemap wraps, so they are the four bands around the
 * picture cut from it at their own place.
 */
static bool DrawWrapMarginsTex(unsigned bg, const CentredFill *fill)
{
    if (!fill->wrap || fill->across || !CentredTexture(bg)) return false;
    ViewBase();
    DrawLayerRect(bg, VIEW_LEFT, VIEW_RIGHT, VIEW_TOP, 0, VIEW_LEFT, VIEW_TOP, 1, 1, false, 0, 0);
    DrawLayerRect(bg, VIEW_LEFT, VIEW_RIGHT, 160, VIEW_BOTTOM, VIEW_LEFT, 160, 1, 1, false, 0, 0);
    DrawLayerRect(bg, VIEW_LEFT, 0, 0, 160, VIEW_LEFT, 0, 1, 1, false, 0, 0);
    DrawLayerRect(bg, 240, VIEW_RIGHT, 0, 160, 240, 0, 1, 1, false, 0, 0);
    return true;
}

/* True when the centred screen draws this layer itself. */
static bool DrawCentredBg(unsigned bg)
{
    const CentredFill *fill = CentredFillOf(sCentredScreen);

    bool speech = fill->bandRow && ((Reg(8) >> 2) & 3) == fill->bandChars;

    if (CentredLayers(fill) & (1u << bg))
    {
        int sky = speech ? fill->skyRows : 0;

        if (sky)
        {
            sSpanShiftY = (int)(Reg(0x12 + bg * 4) & 511) - CTR_VIEW_Y;
            DrawCentredSpan(bg, 0, 240, 0, sky * 8);
            DrawCentredMargins(bg, fill, 0, sky);
            sSpanShiftY = 0;
        }
        DrawCentredPicture(bg, sky * 8, 160);
        if (sky || !DrawWrapMarginsTex(bg, fill))
            DrawCentredMargins(bg, fill, sky ? sky : -64, 64);
        return true;
    }
    if (bg == 0 && speech)
    {
        DrawCentredSpan(0, 0, 240, 0, fill->bandRow * 8);
        sSpanShiftX = fill->bandX;
        sSpanShiftY = fill->bandY;
        DrawCentredSpan(0, 0, 240, fill->bandRow * 8, 160);
        sSpanShiftX = sSpanShiftY = 0;
        return true;
    }
    return false;
}

static void DrawTextBg(unsigned bg)
{
    unsigned control = Reg(8 + bg * 2), size = control >> 14;
    /*
     * A background covers as many pixels as its tilemap spans: 256 for the
     * 32-column size the fixed screens use, 512 for the 64-column one the port
     * widened for the field camera. Inside that band the GBA wrap-around
     * applies exactly, so scrolling behaves as on hardware; past it there is
     * nothing to show on a 400px screen, and the line stays backdrop rather
     * than repeating the same image.
     */
    int right = (int)Min((size & 1) ? 512 : 256, CTR_GAME_WIDTH);
    int bottom = (int)Min((size & 2) ? 512 : 256, CTR_GAME_HEIGHT);
    int left = 0, top = 0;

    if (sStage)
    {
        DrawStageBg(bg);
        return;
    }
    if (sBattle)
    {
        if (bg == 0) DrawBattleText(bg);
        else DrawBattleBgSplit(bg);
        return;
    }
    if (sCentred)
    {
        if (DrawCentredBg(bg)) return;
        if (CentredTexture(bg))
        {
            DrawCentredPicture(bg, 0, 160);
            return;
        }
        if (right > 240) right = 240;
        if (bottom > 160) bottom = 160;
    }
    if (bg == 0 && sFieldBanner) right = CTR_GAME_WIDTH;
    if (right > sClipX1) right = sClipX1;
    if (bottom > sClipY1) bottom = sClipY1;
    if (left < sClipX0) left = sClipX0;
    if (top < sClipY0) top = sClipY0;
    if (right < left) right = left;
    if (bottom < top) bottom = top;
    DrawTextSpan(bg, left, right, top, bottom);
}

/*
 * Layer textures. A text background is composed once, unscrolled, into a
 * texture of its own that repeats exactly like its tilemap wraps, and kept
 * there, only the cells whose tile, entry or palette changed drawn again
 * (LayerRenderCells). Drawing it is then a handful of quads cut from that texture
 * instead of one quad per 8x8 tile:
 *
 * - a background that scrolls per line (the waves of the intro and the title)
 *   is one quad per run of lines sharing a scroll;
 * - a stage background (the intro, the title) is its picture plus the bands
 *   that fill the screen around it, about 35 quads for a whole 400x240 layer.
 *
 * On an Old 3DS a tile quad costs ~4.6 us of CPU, and the intro's leaves are
 * four 400x240 layers: 4500 quads and 22 ms a frame walked tile by tile. The
 * leaves never change while they are on screen, so the tilemap walk now
 * happens once for the scene.
 *
 * 16-bit colour is enough: the GBA has 15 bits and a transparent one.
 */
#define LAYER_IDLE_FRAMES 180
#define LAYER_RETRY_FRAMES 300u
/* In a battle, a layer waits for an atlas the battle's view leaves out: one
 * is there a frame or two after it starts. */
#define LAYER_BATTLE_RETRY_FRAMES 30u
typedef struct
{
    C3D_Tex tex;
    C3D_RenderTarget *target;
    uint32_t usedFrame;
    /* The token and tile cache generation of the last walk of its cells. */
    uint32_t walked, walkedGeneration;
    bool valid;
} LayerTexture;
static LayerTexture sLayers[4];
/* Whether each background is drawn from its texture this frame. */
static bool sLayerReady[4];
static uint32_t sLayerFail[4];
static bool sLayerFailLogged;
/*
 * A stage whose layer textures found no VRAM because the depth planes hold it.
 * Its layers drawn tile by tile cost 30 ms a frame on an Old 3DS, so the
 * planes give way to them: first the nearest (sPlaneShrinkAsked), which is
 * what the intro needs, and only with two left all of them, the stage then
 * composed per eye - twice the work, 30 fps - until it is over.
 */
static bool sStageWithoutPlanes;
static bool sPlaneShrinkAsked;
/* The overworld's ask for VRAM (CtrVideo_RequestPlaneRelease), for the frame
 * being prepared: the battle's surfaces, the voxel world's stereo surface,
 * the layer textures and the OBJ window of a screen that is gone give theirs
 * back too (LayersPrepare, ScenePrepare, SlotsPrepare, StereoSurfaceManage,
 * ObjWindowPrepare). Held after a battle in 3D, or back from the Fly map or
 * a menu, they kept the field's atlas out, and the field was drawn in 2D
 * until they went idle (LAYER_IDLE_FRAMES, three seconds). */
static bool sVramAsked;
static bool sBandsReady;
void CtrVideo_RequestPlaneRelease(void);
static bool ReleaseIdleSurfaces(void);

static unsigned LineValue(unsigned reg, int y, unsigned base)
{
    if (!(sLineMask & (1u << reg))) return base;
    /* A stage's picture has 160 lines; the lines above and below it carry
     * the wave on with the same period rather than stopping it flat. */
    if (sStage) y = ((y % 160) + 160) % 160;
    /* Around the battle scene, the lines next to it: the intro's two halves
     * slide in from the edges of the screen. */
    if (sBattle) y = y < 0 ? 0 : y > 159 ? 159 : y;
    if (y >= 0 && (unsigned)y < sLineCount)
        return sLineScroll[reg][y] & 511;
    return base;
}

/* The shown text backgrounds of the current display mode. */
static unsigned TextBackgrounds(void)
{
    unsigned display = Reg(0), mode = display & 7, found = 0;

    if (mode > 1) return 0;
    for (unsigned bg = 0; bg < 4; ++bg)
    {
        if (!(display & (0x100u << bg))) continue;
        if (mode == 1 && bg >= 2) continue;
        found |= 1u << bg;
    }
    return found;
}

/* Which text backgrounds this frame scrolls per line. */
static unsigned LineBackgrounds(void)
{
    unsigned found = 0, text = TextBackgrounds();

    if (sCentredScreen == CTR_CENTRED_POKEDEX_CAUGHT) return 0;
    for (unsigned bg = 0; bg < 4; ++bg)
        if ((text & (1u << bg)) && (sLineMask & (3u << (bg * 2)))) found |= 1u << bg;
    return found;
}

static void LayerRelease(unsigned bg)
{
    LayerTexture *layer = &sLayers[bg];

    if (layer->target) { ClearWhenMadeForget(layer->target); C3D_RenderTargetDelete(layer->target); }
    if (layer->tex.data) C3D_TexDelete(&layer->tex);
    memset(layer, 0, sizeof(*layer));
}

static void LayersRelease(void)
{
    for (unsigned bg = 0; bg < 4; ++bg) LayerRelease(bg);
}

/* The backgrounds the last LayersPrepare drew from a texture. */
static unsigned sLayersWanted;

/* The layer textures of a screen that is not up any more (the bag's, the
 * Fly map's), before they go idle: outside the frame, as LayerRelease. */
static bool ReleaseIdleLayers(void)
{
    bool any = false;

    for (unsigned bg = 0; bg < 4; ++bg)
        if (!(sLayersWanted & (1u << bg)) && sLayers[bg].tex.data)
        {
            LayerRelease(bg);
            any = true;
        }
    return any;
}

/*
 * Outside the frame, where creating or deleting a target cannot stall one in
 * flight: a texture for every background drawn from one this frame, as large
 * as its tilemap. One that cannot be placed falls back to the tile walk and
 * is retried after a while, logged once.
 */
static bool LineWindows(void);

/* A texture and render target for background bg at its tilemap's size. */
static bool LayerCreate(unsigned bg, unsigned width, unsigned height)
{
    LayerTexture *layer = &sLayers[bg];

    if (C3D_TexInitVRAM(&layer->tex, width, height, GPU_RGBA5551)
        && (layer->target = C3D_RenderTargetCreateFromTex(&layer->tex, GPU_TEXFACE_2D, 0, -1)))
    {
        /* Not cleared when made (ClearWhenMade): a new layer is not valid,
         * and its first draw covers every cell, the empty ones too. */
        return true;
    }
    LayerRelease(bg);
    return false;
}

static void LayersPrepare(void)
{
    /* A centred screen too (DrawCentredPicture), the PokéNav only when its
     * line windows need them. */
    bool centred = sCentred && sCentredScreen != CTR_CENTRED_POKENAV;
    unsigned want = LineBackgrounds()
                  | (sStage || sFieldLayers || centred || LineWindows() ? TextBackgrounds() : 0);

    sLayersWanted = want;

    /* The wanted ones of the wrong size go first, so what they held is
     * free for any of this frame's new ones. */
    for (unsigned bg = 0; bg < 4; ++bg)
    {
        unsigned size = Reg(8 + bg * 2) >> 14;

        if ((want & (1u << bg)) && sLayers[bg].tex.data
            && (sLayers[bg].tex.width != ((size & 1) ? 512 : 256)
                || sLayers[bg].tex.height != ((size & 2) ? 512 : 256)))
            LayerRelease(bg);
    }
    for (unsigned bg = 0; bg < 4; ++bg)
    {
        LayerTexture *layer = &sLayers[bg];
        unsigned size = Reg(8 + bg * 2) >> 14;
        unsigned width = (size & 1) ? 512 : 256, height = (size & 2) ? 512 : 256;

        sLayerReady[bg] = false;
        if (!(want & (1u << bg)))
        {
            if (layer->tex.data && (sVramAsked || sStats.frames - layer->usedFrame > LAYER_IDLE_FRAMES))
                LayerRelease(bg);
            continue;
        }
        if (!layer->tex.data)
        {
            if (sLayerFail[bg] && (int32_t)(sStats.frames - sLayerFail[bg])
                                  < (int32_t)(sBattle ? LAYER_BATTLE_RETRY_FRAMES : LAYER_RETRY_FRAMES)) continue;
            if (!LayerCreate(bg, width, height))
            {
                /*
                 * VRAM is taken and given back by every screen (the bottom
                 * surface, the voxel atlases, the other layers), so a block
                 * this size may be missing with enough free in pieces. The
                 * layers kept for a screen that is not up, and the voxel
                 * world's idle memory, go before this one gives up.
                 */
                unsigned freed = 0;

                for (unsigned other = 0; other < 4; ++other)
                    if (!(want & (1u << other)) && sLayers[other].tex.data)
                    {
                        LayerRelease(other);
                        ++freed;
                    }
                /* For a battle's layer, not the atlases on screen: walked
                 * tile by tile meanwhile, it waits for the block an atlas the
                 * battle's view leaves out makes. Taken at the battle's start,
                 * they went from its scenery, one no longer fitted back and
                 * the battle showed its classic background (port.log
                 * 2026-10-22). */
                if (!sStage) freed += CtrVoxel_ReleaseIdleVramFor(width * height * 2u, !sBattle) > 0;
                /* Outside a stage, the 3D planes of a screen that is gone
                 * and an idle bottom surface too: back from a hidden menu
                 * they held VRAM for 180 frames, and the field walked its
                 * layers tile by tile (5 ms a frame) meanwhile. */
                if ((!freed || !LayerCreate(bg, width, height))
                    && !(!sStage && ReleaseIdleSurfaces() && LayerCreate(bg, width, height)))
                {
                    if (sStage && sBandsReady)
                    {
                        /* Tile by tile for this one frame; a plane goes, or with
                         * only two left, all of them (sPlaneShrinkAsked). */
                        if (sBandCount > 2)
                            sPlaneShrinkAsked = true;
                        else
                        {
                            sStageWithoutPlanes = true;
                            CtrVideo_RequestPlaneRelease();
                        }
                        continue;
                    }
                    if (!sLayerFailLogged)
                        CtrLog_Write(CTR_LOG_ERROR, "VIDEO: no VRAM for a %ux%u layer texture (free=%lu); "
                                     "tile walk", width, height, (unsigned long)vramSpaceFree());
                    sLayerFailLogged = true;
                    /* Only this layer waits; the others still try. */
                    sLayerFail[bg] = sStats.frames | 1;
                    continue;
                }
            }
            sLayerFail[bg] = 0;
            C3D_TexSetFilter(&layer->tex, GPU_NEAREST, GPU_NEAREST);
            C3D_TexSetWrap(&layer->tex, GPU_REPEAT, GPU_REPEAT);
        }
        layer->usedFrame = sStats.frames;
        sLayerReady[bg] = true;
    }
}

/*
 * Every layer texture - the field's, the stages', the line windows' - is kept
 * cell by cell.
 *
 * Walked tile by tile, the 2D field is three 400x240 layers, about 4700 quads
 * a frame: 20 ms of CPU on an Old 3DS, so it never made 60 fps. Composed into
 * textures it is a quad per layer, but the whole-layer hash would recompose
 * all three every time a tile animates (water, flowers), since they share
 * their tiles. So each cell keeps what was drawn in it - the upload serial of
 * its tile (which covers the tile's bytes and palette) and its flips - and
 * only the cells whose tile, tilemap entry or palette changed are cleared and
 * drawn again: a row or a column as the camera moves, the animated tiles when
 * they animate. A frame where most cells changed (a new map, a palette fade)
 * is recomposed whole.
 *
 * The stages had a hash of the whole layer instead, and the title screen
 * cycles the colour of Rayquaza's markings every fourth frame: the whole
 * 1024-tile layer was drawn again each time, a dropped frame on an Old 3DS
 * for a dozen tiles that changed.
 */
#define LAYER_CELLS 4096
static uint32_t sCellSig[4][LAYER_CELLS];
/* How many of those cells draw something: none, and the layer's quad covers
 * the screen with transparent texels (the field's text layer, most of the
 * time), so it is not drawn at all. */
static unsigned sCellsShown[4];
static unsigned sCellControl[4];

static bool LayerDrawable(unsigned bg);

/*
 * Layer cells without Citro2D's per-quad work.
 *
 * Drawn through C2D_DrawImageAt a cell cost ~4.6 us of CPU on an Old 3DS: a
 * scene that opens on four 256x512 layers drew 8192 of them, 37 ms, and every
 * new screen paid it for each of its layers. A cell is always one whole atlas
 * tile (or nothing) over one 8x8 square of the texture, so it needs no tint,
 * no blending and no transform: four vertices of two shorts and two floats,
 * written straight into a buffer, and one indexed draw per layer. The tile
 * replaces what the square held, transparent texels included, so a changed
 * cell needs no clearing first; an empty one is drawn in the constant 0.
 *
 * It runs on Citro2D's own program, set up for the layer by an empty Citro2D
 * draw, and only borrows the vertex layout, the buffer and the first texture
 * stage, which it puts back: rebinding Citro2D (C2D_Prepare) mid-frame left
 * its tinted draws unfaded for the rest of the frame.
 *
 * The buffers are refilled from the start each frame: the GPU has finished
 * the last frame's draws once C3D_FrameBegin returns, and C3D_FrameEnd flushes
 * the linear heap before the GPU reads any of it. A frame that would need more
 * than FAST_QUADS cells keeps the Citro2D path.
 */
#define FAST_QUADS 8192

static void FastInit(void)
{
    sFastVertices = linearAlloc(FAST_QUADS * 4 * sizeof(FastVertex));
    sFastIndices = linearAlloc(FAST_QUADS * 6 * sizeof(uint16_t));
    if (sFastVertices == NULL || sFastIndices == NULL)
    {
        linearFree(sFastVertices);
        linearFree(sFastIndices);
        sFastVertices = NULL;
        sFastIndices = NULL;
        CtrLog_Write(CTR_LOG_ERROR, "VIDEO: no linear memory for layer cells; through Citro2D");
        return;
    }
    for (unsigned q = 0; q < FAST_QUADS; ++q)
    {
        uint16_t *index = sFastIndices + q * 6, base = (uint16_t)(q * 4);

        index[0] = base;
        index[1] = base + 1;
        index[2] = base + 2;
        index[3] = base + 2;
        index[4] = base + 1;
        index[5] = base + 3;
    }
    sFastReady = true;
}

/* One cell's square: corners top-left, top-right, bottom-left, bottom-right,
 * with texcoords as Citro2D gives an atlas slot (DrawSlotTinted). */
static void FastQuad(FastVertex *v, int x, int y, int slot, bool flipX, bool flipY)
{
    unsigned tileX = ((unsigned)slot % (ATLAS_SIZE / 8)) * 8, tileY = ((unsigned)slot / (ATLAS_SIZE / 8)) * 8;
    float s0 = tileX / (float)ATLAS_SIZE, s1 = (tileX + 8) / (float)ATLAS_SIZE;
    float t0 = 1.0f - tileY / (float)ATLAS_SIZE, t1 = 1.0f - (tileY + 8) / (float)ATLAS_SIZE;

    if (flipX) { float swap = s0; s0 = s1; s1 = swap; }
    if (flipY) { float swap = t0; t0 = t1; t1 = swap; }
    v[0] = (FastVertex){(int16_t)x, (int16_t)y, s0, t0};
    v[1] = (FastVertex){(int16_t)(x + 8), (int16_t)y, s1, t0};
    v[2] = (FastVertex){(int16_t)x, (int16_t)(y + 8), s0, t1};
    v[3] = (FastVertex){(int16_t)(x + 8), (int16_t)(y + 8), s1, t1};
}

static void FastDraw(unsigned first, unsigned count)
{
    if (count)
        C3D_DrawElements(GPU_TRIANGLES, (int)count * 6, C3D_UNSIGNED_SHORT, sFastIndices + first * 6);
}

static bool FastCells(unsigned bg, const uint16_t *cells, unsigned count, const int16_t *cellSlot,
                      const uint16_t *cellEntry, unsigned columns)
{
    LayerTexture *layer = &sLayers[bg];
    unsigned tiles = 0, first = sFastUsed;
    C3D_AttrInfo savedAttr, *attr;
    C3D_BufInfo savedBuf, *buf;
    C3D_TexEnv savedEnv, *env;
    Tex3DS_SubTexture none = {0, 0, 0.0f, 1.0f, 0.0f, 1.0f};

    if (!sFastReady || count > FAST_QUADS - sFastUsed)
        return false;
    /* Tiles first, then the empty cells after them. */
    for (unsigned i = 0; i < count; ++i)
        if (cellSlot[cells[i]] >= 0) ++tiles;
    for (unsigned i = 0, t = 0, e = tiles; i < count; ++i)
    {
        unsigned cell = cells[i], entry = cellEntry[cell];
        /* columns is 32 or 64: no library division per cell (no divider on the ARM11) */
        int x = (int)(cell & (columns - 1)) * 8, y = (int)(cell >> __builtin_ctz(columns)) * 8;

        if (cellSlot[cell] >= 0)
            FastQuad(sFastVertices + (first + t++) * 4, x, y, cellSlot[cell], entry & 1024, entry & 2048);
        else
            FastQuad(sFastVertices + (first + e++) * 4, x, y, 0, false, false);
    }
    sFastUsed += count;

    /* Citro2D sets up the target, its projection and the atlas, with a quad
     * of no size, and sends it: its state is then the GPU's. */
    sInLayerTexture = true;
    BlendForget();
    SceneOn(layer->target);
    C2D_ViewReset();
    Blend(bg, false, false);
    C2D_DrawImageAt((C2D_Image){&sAtlas, &none}, 0, 0, 0, &sTint, 1, 1);
    C2D_Flush();

    attr = C3D_GetAttrInfo();
    savedAttr = *attr;
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_SHORT, 2);
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);
    AttrInfo_AddFixed(attr, 2);
    AttrInfo_AddFixed(attr, 3);
    C3D_FixedAttribSet(2, 0.0f, 0.0f, 0.0f, 0.0f);
    C3D_FixedAttribSet(3, 255.0f, 255.0f, 255.0f, 255.0f);
    buf = C3D_GetBufInfo();
    savedBuf = *buf;
    BufInfo_Init(buf);
    BufInfo_Add(buf, sFastVertices, sizeof(FastVertex), 2, 0x10);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);

    env = C3D_GetTexEnv(0);
    savedEnv = *env;
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    FastDraw(first, tiles);
    if (count > tiles)
    {
        env = C3D_GetTexEnv(0);
        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, 0, 0);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        C3D_TexEnvColor(env, 0);
        FastDraw(first + tiles, count - tiles);
    }

    /* Citro2D's layout, buffer and stage back, as it left them. */
    *C3D_GetTexEnv(0) = savedEnv;
    C3D_SetAttrInfo(&savedAttr);
    C3D_SetBufInfo(&savedBuf);
    sInLayerTexture = false;
    BlendForget();
    sStats.cells += count;
    return true;
}

static bool LayerRenderCells(unsigned bg)
{
    static uint16_t dirty[LAYER_CELLS];
    static int16_t cellSlot[LAYER_CELLS];
    static uint16_t cellEntry[LAYER_CELLS];
    static uint32_t memoStamp[1024], stamp;
    static uint16_t memoEntry[1024];
    static int16_t memoSlot[1024];
    LayerTexture *layer = &sLayers[bg];
    unsigned control = Reg(8 + bg * 2), size = control >> 14;
    unsigned map = ((control >> 8) & 31) * 0x800;
    unsigned chars = ((control >> 2) & 3) * 0x4000;
    bool color256 = (control & 128) != 0;
    unsigned columns = (size & 1) ? 64 : 32, rows = (size & 2) ? 64 : 32;
    unsigned cells = rows * columns;
    bool full = !layer->valid || sCellControl[bg] != control;
    unsigned count = 0;
    bool sameTiles = !full && layer->walkedGeneration == sCacheGeneration && sBgPaletteStamp <= layer->walked
        && !VramChangedAfter(chars, Min(1024u * (color256 ? 64 : 32), 0x10000 - chars), layer->walked);
    uint32_t walked = layer->walked;

    /* Nothing it is made of changed since its cells were last walked: the
     * texture is what a walk would draw. */
    if (sameTiles && !VramChangedAfter(map, rows * columns * 2, walked) && sTilemapBankStamp <= walked)
        return false;
    /* Recorded at this token: its VRAM is compared now (see SyncBlock). */
    VramSyncRange(chars, Min(1024u * (color256 ? 64 : 32), 0x10000 - chars));
    VramSyncRange(map, rows * columns * 2);
    layer->walked = sStats.frames + 1;
    layer->walkedGeneration = sCacheGeneration;
    ++stamp;
    for (unsigned row = 0; row < rows; ++row)
    {
        unsigned rowBase = map + CtrVideo_TextMapOffset(0, row, size);

        /*
         * Same tiles, same palettes, same slots: a cell can only have changed
         * if its tilemap entry did, or its bank. The camera redraws a row or
         * a column of the map at a time, so most rows lie in VRAM that did
         * not change and are not walked at all (both screenblocks of a
         * 64-wide row).
         */
        if (sameTiles && !VramChangedAfter(rowBase, 64, walked) && !TilemapBanksChangedAfter(rowBase, walked)
            && (columns < 64 || (!VramChangedAfter(rowBase + 2048, 64, walked)
                                 && !TilemapBanksChangedAfter(rowBase + 2048, walked))))
            continue;

        for (unsigned column = 0; column < columns; ++column)
        {
            unsigned cell = row * columns + column;
            unsigned entryAddress = rowBase + (column & 31) * 2 + (column >> 5) * 2048;
            unsigned entry = Read16(entryAddress);
            unsigned index = entry & 1023;
            unsigned bank = color256 ? 0 : TilemapBankAt(entryAddress, entry);
            int slot;
            uint32_t sig;

            if (bank)
                slot = BankedTileSlot(bank, entry, chars);
            else if (memoStamp[index] == stamp && memoEntry[index] == entry)
                slot = memoSlot[index];
            else
            {
                unsigned address = chars + index * (color256 ? 64 : 32);

                slot = address < 0x10000 ? GetTileSlot(address, entry >> 12, color256) : -1;
                memoStamp[index] = stamp;
                memoEntry[index] = (uint16_t)entry;
                memoSlot[index] = (int16_t)slot;
            }
            cellSlot[cell] = (int16_t)slot;
            cellEntry[cell] = (uint16_t)entry;
            /* Nothing drawn is 0; anything drawn has the top bit set. */
            sig = slot < 0 ? 0 : (((sTiles[slot].serial << 2) | ((entry >> 10) & 3)) | 1u << 31);
            if (!full && sCellSig[bg][cell] == sig) continue;
            if ((sCellSig[bg][cell] != 0) != (sig != 0))
                sCellsShown[bg] += sig != 0 ? 1u : ~0u;
            sCellSig[bg][cell] = sig;
            dirty[count++] = (uint16_t)cell;
        }
    }
    if (count == 0) return false;
    layer->valid = true;
    sCellControl[bg] = control;

    {
        /* Mostly changed: the whole layer, which needs no clearing either.
         * Not after a walk that skipped rows: their cells were not read. */
        bool whole = full || (!sameTiles && count > cells / 2);

        if (whole)
        {
            count = 0;
            for (unsigned cell = 0; cell < cells; ++cell)
                dirty[count++] = (uint16_t)cell;
        }
        if (FastCells(bg, dirty, count, cellSlot, cellEntry, columns))
            return true;
        full = whole;
    }
    BlendForget();
    sInLayerTexture = true;
    if (full)
    {
        C2D_TargetClear(layer->target, 0);
        SceneOn(layer->target);
        C2D_ViewReset();
        count = 0;
        for (unsigned cell = 0; cell < cells; ++cell)
            dirty[count++] = (uint16_t)cell;
    }
    else
    {
        /* Clear the changed cells to transparent first: a tile's transparent
         * pixels must not keep what was drawn there before. */
        SceneOn(layer->target);
        C2D_ViewReset();
        C2D_Flush();
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        for (unsigned i = 0; i < count; ++i)
            C2D_DrawRectSolid((dirty[i] & (columns - 1)) * 8, (dirty[i] >> __builtin_ctz(columns)) * 8, 0, 8, 8, 0);
        C2D_Flush();
        BlendForget();
    }
    Blend(bg, false, false);
    for (unsigned i = 0; i < count; ++i)
    {
        unsigned cell = dirty[i];

        if (cellSlot[cell] >= 0)
            DrawSlot(cellSlot[cell], (cell & (columns - 1)) * 8, (cell >> __builtin_ctz(columns)) * 8,
                     cellEntry[cell] & 1024, cellEntry[cell] & 2048);
    }
    C2D_Flush();
    /* DrawSlot drops quads past the geometry budget, and their cells already
     * hold their new signatures: draw the whole layer again next frame, or
     * they stay holes until their tiles change. */
    if (sStats.tiles >= MAX_DRAWS - 32)
        layer->valid = false;
    sInLayerTexture = false;
    BlendForget();
    return true;
}

/* A field background from its texture: one quad, wrapping as its tilemap
 * does, over the band the tilemap spans (DrawTextBg's limits). */
static bool DrawFieldBgTex(unsigned bg)
{
    LayerTexture *layer = &sLayers[bg];
    float width, height, sx, sy;
    int x0 = sClipX0 > 0 ? sClipX0 : 0, y0 = sClipY0 > 0 ? sClipY0 : 0, x1, y1;

    if (!sFieldLayers || !LayerDrawable(bg)) return false;
    if (sCellsShown[bg] == 0) return true;
    width = layer->tex.width;
    height = layer->tex.height;
    x1 = sClipX1 < (int)width ? sClipX1 : (int)width;
    /* The texture repeats as the tilemap wraps. */
    if (bg == 0 && sFieldBanner) x1 = sClipX1;
    y1 = sClipY1 < (int)height ? sClipY1 : (int)height;
    if (x0 >= x1 || y0 >= y1) return true;
    sx = Reg(0x10 + bg * 4) & 511;
    sy = Reg(0x12 + bg * 4) & 511;
    ViewBase();
    {
        const Tex3DS_SubTexture run = {(u16)(x1 - x0), (u16)(y1 - y0),
            (sx + x0) / width, 1.0f - (sy + y0) / height,
            (sx + x1) / width, 1.0f - (sy + y1) / height};

        ++sStats.tiles;
        C2D_DrawImageAt((C2D_Image){&layer->tex, &run}, x0 + CTR_VIEW_X + sLayerShift,
                        y0 + CTR_VIEW_Y, 0, &sTint, 1, 1);
    }
    return true;
}

/* Inside the frame, before the logical surface: the maps that changed. */
static void LayersRender(void)
{
    bool any = false;

    for (unsigned bg = 0; bg < 4; ++bg)
        if (sLayerReady[bg] && LayerRenderCells(bg)) any = true;
    /* Rendering to a texture and then sampling it needs a command split. */
    if (any) C3D_FrameSplit(0);
}

static bool LayerDrawable(unsigned bg)
{
    return sLayerReady[bg] && sLayers[bg].valid;
}

static bool DrawBandBgTex(unsigned bg);

/*
 * One rectangle [x0, x1) x [y0, y1) of the screen, in GBA coordinates, cut
 * from a layer texture: the screen point (x0, y0) shows the picture point
 * (sx, sy), and each screen pixel steps (dx, dy) through the picture - 1 to
 * copy, -1 to mirror, a fraction to stretch. Clipped to the current clip
 * rectangle by moving its texture coordinates, so it needs no scissor. With
 * fade, its corners are tinted by EdgeFade.
 */
static void DrawLayerRect(unsigned bg, int x0, int x1, int y0, int y1,
                          float sx, float sy, float dx, float dy, bool fade, int top, int bottom)
{
    LayerTexture *layer = &sLayers[bg];
    int cx0 = x0 > sClipX0 ? x0 : sClipX0, cx1 = x1 < sClipX1 ? x1 : sClipX1;
    int cy0 = y0 > sClipY0 ? y0 : sClipY0, cy1 = y1 < sClipY1 ? y1 : sClipY1;
    float width = layer->tex.width, height = layer->tex.height;
    float ox = Reg(0x10 + bg * 4) & 511, oy = Reg(0x12 + bg * 4) & 511;
    float s0, s1, t0, t1;
    C2D_ImageTint tint = sTint;

    if (cx0 >= cx1 || cy0 >= cy1) return;
    s0 = sx + (cx0 - x0) * dx + ox;
    s1 = sx + (cx1 - x0) * dx + ox;
    t0 = sy + (cy0 - y0) * dy + oy;
    t1 = sy + (cy1 - y0) * dy + oy;
    if (fade)
    {
        bool white;
        float bright = LayerBrightness(bg, &white);

        FadeCorner(&tint, C2D_TopLeft, EdgeFade(cx0, cy0, top, bottom), bright, white);
        FadeCorner(&tint, C2D_TopRight, EdgeFade(cx1, cy0, top, bottom), bright, white);
        FadeCorner(&tint, C2D_BotLeft, EdgeFade(cx0, cy1, top, bottom), bright, white);
        FadeCorner(&tint, C2D_BotRight, EdgeFade(cx1, cy1, top, bottom), bright, white);
    }
    {
        /*
         * A subtexture whose top is below its bottom means "rotated" to
         * Tex3DS, so a mirror is drawn as the plain cut flipped by a negative
         * scale, the same way flipped tiles are. (Only unfaded cuts mirror.)
         */
        bool flipY = t1 < t0;
        float ta = flipY ? t1 : t0, tb = flipY ? t0 : t1;
        const Tex3DS_SubTexture cut = {(u16)(cx1 - cx0), (u16)(cy1 - cy0),
            s0 / width, 1.0f - ta / height, s1 / width, 1.0f - tb / height};

        ++sStats.tiles;
        C2D_DrawImageAt((C2D_Image){&layer->tex, &cut}, cx0 + CTR_VIEW_X + sLayerShift,
                        cy0 + CTR_VIEW_Y, 0, &tint, 1, flipY ? -1 : 1);
    }
}

/* One corner cell: the corner block stretched to it, faded, skipped if black. */
static void DrawCornerCell(unsigned bg, int x0, int x1, int y0, int y1, float sx, float sy,
                           int top, int bottom)
{
    if (y0 < VIEW_TOP) y0 = VIEW_TOP;
    if (y1 > VIEW_BOTTOM) y1 = VIEW_BOTTOM;
    if (y0 < top && y1 > top) y1 = top;
    if (y0 < bottom && y1 > bottom) y0 = bottom;
    if (x0 >= x1 || y0 >= y1) return;
    if (sUnderlaid && EdgeFade(x0, y0, top, bottom) >= 1.0f && EdgeFade(x1, y0, top, bottom) >= 1.0f
        && EdgeFade(x0, y1, top, bottom) >= 1.0f && EdgeFade(x1, y1, top, bottom) >= 1.0f)
        return;
    DrawLayerRect(bg, x0, x1, y0, y1, sx, sy, 8.0f / (x1 - x0), 8.0f / (y1 - y0), true, top, bottom);
}

/*
 * One 8-line band above or below a still picture, from its texture: the
 * picture's row src repeated, or, when that row is plain (EdgeRowPlain), a
 * column of its plain tile wherever the row holds something else.
 */
static void DrawEdgeBand(unsigned bg, int y0, int y1, int src, int top, int bottom)
{
    unsigned plain = EdgeRowPlain(bg, src);
    unsigned control = Reg(8 + bg * 2), size = control >> 14;
    unsigned map = ((control >> 8) & 31) * 0x800;
    unsigned scrollX = Reg(0x10 + bg * 4) & 511, scrollY = Reg(0x12 + bg * 4) & 511;
    unsigned rowBase = map + CtrVideo_TextMapOffset(0, (unsigned)(src + (int)scrollY) >> 3, size);
    int plainColumn = -1, run = 0;

    bool isPlain[30];

    if (plain == ~0u)
    {
        DrawLayerRect(bg, 0, 240, y0, y1, 0, src, 1, 1, true, top, bottom);
        return;
    }
    for (int c = 0; c < 30; ++c)
    {
        unsigned column = ((scrollX >> 3) + (unsigned)c) & ((size & 1) ? 63u : 31u);

        isPlain[c] = Read16(rowBase + (column & 31) * 2 + (column >> 5) * 2048) == plain;
        if (isPlain[c] && plainColumn < 0) plainColumn = c;
    }
    for (int c = 0; c <= 30; ++c)
    {
        if (c < 30 && isPlain[c]) continue;
        /* The plain columns up to here as one cut, then this one as plain. */
        if (c > run) DrawLayerRect(bg, run * 8, c * 8, y0, y1, run * 8, src, 1, 1, true, top, bottom);
        if (c < 30)
            DrawLayerRect(bg, c * 8, c * 8 + 8, y0, y1, plainColumn * 8, src, 1, 1, true, top, bottom);
        run = c + 1;
    }
}

/*
 * The intro's leaves scene, widened with art of its own.
 *
 * Behind the Game Freak logo the intro pans up a still scene of four layers.
 * Its art ends at the GBA screen's edges, and neither repeating the edge
 * tiles nor mirroring them gives a scene: they read as copies. So its
 * margins are new art made for them (scripts/gen_intro_margins.py): every
 * shape the edge cuts carried on from the edge's own pixels and closed, new
 * grass, bushes, plants and hills behind, and the plants layer's darker first
 * eight columns - the GBA screen's edge, where the pit ends - drawn anew. It
 * is made by the builder from the player's ROM, like the voxel data, and read
 * here from stage/leaves.bin: per layer the colour above the map's first row
 * and two strips of palette entries, 64x256 on the left (screen x -56..7) and
 * 56x256 on the right (x 240..295), a row per map row.
 *
 * Each strip becomes a texture of the current palette, remade when a palette
 * the scene uses changes (the fades in and out), and is drawn beside the
 * picture moving with the layer's scroll, faded like any stage margin. Above
 * and below the picture the layers' own rows show - the art the GBA scrolls
 * in as the scene pans - and above the sky's first row, its colour.
 *
 * The scene is known by its backgrounds' control registers (intro.c,
 * Task_Scene1_Load): four 256x512 4bpp maps on character base 0 at screen
 * bases 16, 18, 20 and 22, BG n at priority n.
 */
#define LEAVES_LEFT 64
#define LEAVES_RIGHT 56
#define LEAVES_ROWS 256
#define LEAVES_LAYER (1 + (LEAVES_LEFT + LEAVES_RIGHT) * LEAVES_ROWS)
#define LEAVES_BYTES (8 + 4 * LEAVES_LAYER)
static uint8_t *sLeaves;
static bool sLeavesTried;
/* Every drawn pixel of a layer's strips: its texel and its palette entry, so
 * a palette change is one store per pixel (a fade changes it every frame). */
typedef struct { uint16_t texel; uint8_t color; } LeavesPixel;
static LeavesPixel *sLeavesPixels[4];
static unsigned sLeavesCount[4];
static C3D_Tex sLeavesTex[4];
static uint32_t sLeavesKey[4], sLeavesUsed;
/* The scene has drawn its margins since the textures were made: only then
 * are they given back when idle (they are made at start-up, before it). */
static bool sLeavesShown;

/*
 * The strips' art, read and laid out once at start-up: done on the first frame
 * of the leaves scene it was a 120 KiB read and a walk of every strip pixel in
 * the middle of the intro, the frame the scene fades in on.
 */
static void LeavesLoad(void)
{
    if (!sLeavesTried)
    {
        uint32_t size = 0;

        sLeavesTried = true;
        sLeaves = CtrData_Load("stage/leaves.bin", &size);
        if (sLeaves && (size != LEAVES_BYTES || memcmp(sLeaves, "EM3DLVS1", 8)))
        {
            CtrLog_Write(CTR_LOG_ERROR, "VIDEO: stage/leaves.bin is not what this build reads (%lu bytes)",
                         (unsigned long)size);
            free(sLeaves);
            sLeaves = NULL;
        }
        for (unsigned bg = 0; sLeaves && bg < 4; ++bg)
        {
            const uint8_t *layer = sLeaves + 8 + bg * LEAVES_LAYER + 1;
            unsigned count = 0;

            for (unsigned pass = 0; pass < 2; ++pass)
            {
                for (unsigned i = 0; i < (LEAVES_LEFT + LEAVES_RIGHT) * LEAVES_ROWS; ++i)
                {
                    unsigned color = layer[i], x, y;

                    if (color == 0xFF || !(color & 15)) continue;
                    if (pass == 0) { ++count; continue; }
                    if (i < LEAVES_LEFT * LEAVES_ROWS) { x = i % LEAVES_LEFT; y = i / LEAVES_LEFT; }
                    else
                    {
                        unsigned j = i - LEAVES_LEFT * LEAVES_ROWS;

                        x = 64 + j % LEAVES_RIGHT;
                        y = j / LEAVES_RIGHT;
                    }
                    sLeavesPixels[bg][sLeavesCount[bg]++] = (LeavesPixel){(uint16_t)CtrVideo_Texel(x, y, 128),
                                                                          (uint8_t)color};
                }
                if (pass == 0 && !(sLeavesPixels[bg] = malloc(count * sizeof(LeavesPixel))))
                {
                    /* No strips at all then: the layers already laid out too. */
                    for (unsigned made = 0; made < bg; ++made)
                    {
                        free(sLeavesPixels[made]);
                        sLeavesPixels[made] = NULL;
                        sLeavesCount[made] = 0;
                    }
                    free(sLeaves);
                    sLeaves = NULL;
                    break;
                }
            }
        }
    }
}

static bool LeavesScene(void)
{
    if (!sStage) return false;
    for (unsigned bg = 0; bg < 4; ++bg)
        if (Reg(8 + bg * 2) != (bg | ((16 + 2 * bg) << 8) | 0x8000)) return false;
    LeavesLoad();
    return sLeaves != NULL;
}

static void LeavesRelease(void)
{
    for (unsigned bg = 0; bg < 4; ++bg)
        if (sLeavesTex[bg].data)
        {
            C3D_TexDelete(&sLeavesTex[bg]);
            memset(&sLeavesTex[bg], 0, sizeof(sLeavesTex[bg]));
        }
}

/* A layer's strip texture in linear memory, its pixels not yet drawn. */
static bool LeavesAlloc(unsigned bg)
{
    C3D_Tex *tex = &sLeavesTex[bg];

    if (tex->data) return true;
    if (!C3D_TexInit(tex, 128, LEAVES_ROWS, GPU_RGBA5551))
    {
        static bool logged;

        memset(tex, 0, sizeof(*tex));
        if (!logged)
            CtrLog_Write(CTR_LOG_ERROR, "VIDEO: no linear memory for the intro's margins "
                         "(linear free=%lu); black margins", (unsigned long)linearSpaceFree());
        logged = true;
        return false;
    }
    C3D_TexSetFilter(tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    memset(tex->data, 0, 128 * LEAVES_ROWS * 2);
    sLeavesKey[bg] = 0;
    return true;
}

/* The layer's strips in the current palette; false without memory for them. */
static bool LeavesTexture(unsigned bg)
{
    C3D_Tex *tex = &sLeavesTex[bg];
    uint32_t key = 1;
    uint16_t *data;

    sLeavesUsed = sStats.frames;
    sLeavesShown = true;
    for (unsigned bank = 0; bank < 16; ++bank) key = key * 31 + sPaletteVersion[bank];
    if (!tex->data && !LeavesAlloc(bg)) return false;
    if (sLeavesKey[bg] == key) return true;
    sLeavesKey[bg] = key;
    data = tex->data;
    for (unsigned i = 0; i < sLeavesCount[bg]; ++i)
        data[sLeavesPixels[bg][i].texel] = sTexturePalette[sLeavesPixels[bg][i].color];
    C3D_TexFlush(tex);
    return true;
}

/* A faded cut of a strip texture: screen [x0, x1) x [y0, y1), texel (s, t). */
static void DrawLeavesCut(C3D_Tex *tex, unsigned bg, int x0, int x1, int y0, int y1, float s, float t)
{
    bool white;
    float bright = LayerBrightness(bg, &white);
    C2D_ImageTint tint;

    if (x0 < sClipX0) { s += sClipX0 - x0; x0 = sClipX0; }
    if (y0 < sClipY0) { t += sClipY0 - y0; y0 = sClipY0; }
    if (x1 > sClipX1) x1 = sClipX1;
    if (y1 > sClipY1) y1 = sClipY1;
    if (x0 >= x1 || y0 >= y1) return;
    if (sUnderlaid && EdgeFade(x0, y0, 0, 160) >= 1.0f && EdgeFade(x1, y0, 0, 160) >= 1.0f
        && EdgeFade(x0, y1, 0, 160) >= 1.0f && EdgeFade(x1, y1, 0, 160) >= 1.0f)
        return;
    FadeCorner(&tint, C2D_TopLeft, EdgeFade(x0, y0, 0, 160), bright, white);
    FadeCorner(&tint, C2D_TopRight, EdgeFade(x1, y0, 0, 160), bright, white);
    FadeCorner(&tint, C2D_BotLeft, EdgeFade(x0, y1, 0, 160), bright, white);
    FadeCorner(&tint, C2D_BotRight, EdgeFade(x1, y1, 0, 160), bright, white);
    {
        const Tex3DS_SubTexture cut = {(u16)(x1 - x0), (u16)(y1 - y0),
            s / 128.0f, 1.0f - t / LEAVES_ROWS, (s + x1 - x0) / 128.0f, 1.0f - (t + y1 - y0) / LEAVES_ROWS};

        ++sStats.tiles;
        C2D_DrawImageAt((C2D_Image){tex, &cut}, x0 + CTR_VIEW_X + sLayerShift, y0 + CTR_VIEW_Y, 0, &tint, 1, 1);
    }
}

/* The sky above the map's first row, faded, over [x0, x1) x [y0, y1). */
static void DrawLeavesSky(unsigned bg, uint8_t sky, int x0, int x1, int y0, int y1)
{
    bool white;
    float bright = LayerBrightness(bg, &white);
    uint32_t rgb = CtrVideo_RGBA8(sPalette[sky], true);
    float corners[4];
    u32 colors[4];

    if (x0 < sClipX0) x0 = sClipX0;
    if (y0 < sClipY0) y0 = sClipY0;
    if (x1 > sClipX1) x1 = sClipX1;
    if (y1 > sClipY1) y1 = sClipY1;
    if (x0 >= x1 || y0 >= y1) return;
    corners[0] = EdgeFade(x0, y0, 0, 160);
    corners[1] = EdgeFade(x1, y0, 0, 160);
    corners[2] = EdgeFade(x0, y1, 0, 160);
    corners[3] = EdgeFade(x1, y1, 0, 160);
    if (sUnderlaid && corners[0] >= 1.0f && corners[1] >= 1.0f && corners[2] >= 1.0f && corners[3] >= 1.0f)
        return;
    for (unsigned i = 0; i < 4; ++i)
    {
        float c[3] = {(rgb >> 24) / 255.0f, ((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f};

        for (unsigned k = 0; k < 3; ++k)
        {
            c[k] = white ? c[k] + (1.0f - c[k]) * bright : c[k] * (1.0f - bright);
            c[k] *= 1.0f - corners[i];
        }
        colors[i] = C2D_Color32((u8)(c[0] * 255.0f), (u8)(c[1] * 255.0f), (u8)(c[2] * 255.0f), 255);
    }
    ++sStats.tiles;
    C2D_DrawRectangle(x0 + CTR_VIEW_X + sLayerShift, y0 + CTR_VIEW_Y, 0, x1 - x0, y1 - y0,
                      colors[0], colors[1], colors[2], colors[3]);
}

/*
 * The margins of a leaves layer, the picture itself already drawn: in cells a
 * quarter of the fade apart (the fade is linear inside each), cut where the
 * map's rows wrap. False when the scene is not up or has no strips.
 */
static bool DrawLeavesMargins(unsigned bg)
{
    static const int xs[] = {-56, -42, -28, -14, 0, 8, 240, 254, 268, 282, 296};
    static const int ys[] = {-40, -28, -21, -14, -7, 0, 160, 167, 174, 181, 188, 200};
    unsigned scrollY = Reg(0x12 + bg * 4) & 511;
    uint8_t sky;

    if (!LeavesScene()) return false;
    /* Without its textures the scene's margins stay black: never the edge
     * tiles repeated, which read as copies (the rule for this scene). */
    if (!LeavesTexture(bg)) return true;
    sky = sLeaves[8 + bg * LEAVES_LAYER];
    ViewBase();
    for (unsigned j = 0; j + 1 < sizeof(ys) / sizeof(ys[0]); ++j)
    {
        int y0 = ys[j], y1 = ys[j + 1];

        /* The layer's own rows above and below the picture. */
        if ((y1 <= 0 || y0 >= 160) && LayerDrawable(bg))
            DrawLayerRect(bg, 0, 240, y0, y1, 0, y0, 1, 1, true, 0, 160);
        for (int a = y0; a < y1;)
        {
            /* Map rows 0..255 hold the art, 256..511 only the sky. */
            unsigned row = (unsigned)(a + (int)scrollY) & 511;
            int b = a + (int)((row < 256 ? 256 : 512) - row);

            if (b > y1) b = y1;
            if (row < 256)
            {
                for (unsigned i = 0; i + 1 < sizeof(xs) / sizeof(xs[0]); ++i)
                {
                    int x0 = xs[i], x1 = xs[i + 1];

                    if (x0 == 8) continue;
                    DrawLeavesCut(&sLeavesTex[bg], bg, x0, x1, a, b, x0 < 240 ? x0 + 56 : x0 - 240 + 64,
                                  (float)row);
                }
            }
            else if (sky != 0xFF)
            {
                for (unsigned i = 0; i + 1 < sizeof(xs) / sizeof(xs[0]); ++i)
                    if (xs[i] != 8) DrawLeavesSky(bg, sky, xs[i], xs[i + 1], a, b);
                if (a < 0) DrawLeavesSky(bg, sky, 8, 240, a, b < 0 ? b : 0);
            }
            a = b;
        }
    }
    return true;
}

/*
 * A stage background from its texture, filled around the picture exactly as
 * DrawStageBg does it tile by tile: a layer that scrolls wraps across and is
 * mirrored above and below; a still one is carried out from its edges - the
 * 8-pixel band along each edge repeated outwards, the corners stretched from
 * the corner block - and faded to black.
 */
/*
 * A faded stage's margins (the cable car): the picture mirrored about its
 * edges and faded into black, as the intro's carried edges are. Carried, the
 * edge columns of its dirt, trees and pylons smeared into streaks; mirrored,
 * the margins hold the scene's own texture. Drawn in strips so the fade stays
 * a gradient: a mirrored cut is drawn flipped, which flips its tint too, and
 * across a strip that thin the difference does not show. Above the picture
 * only a still layer (the sky) is mirrored: the scrolling ones hold nothing
 * there, and a pylon's head would hang upside down.
 */
static bool MarginBlack(int x, int y)
{
    return sUnderlaid && EdgeFade(x, y, 0, 160) >= 1.0f;
}

static void DrawFadedStageTex(unsigned bg)
{
    DrawLayerRect(bg, 0, 240, 0, 160, 0, 0, 1, 1, false, 0, 160);
    for (int x = 0; x > VIEW_LEFT && !MarginBlack(x, 80); x -= 8)
        DrawLayerRect(bg, x - 8, x, 0, 160, (float)(8 - x), 0, -1, 1, true, 0, 160);
    for (int x = 240; x < VIEW_RIGHT && !MarginBlack(x, 80); x += 8)
        DrawLayerRect(bg, x, x + 8, 0, 160, (float)(480 - x), 0, -1, 1, true, 0, 160);
    for (int y = 160; y < VIEW_BOTTOM && !MarginBlack(120, y); y += 4)
    {
        float sy = (float)(320 - y);

        DrawLayerRect(bg, 0, 240, y, y + 4, 0, sy, 1, -1, true, 0, 160);
        for (int x = 0; x > VIEW_LEFT && !MarginBlack(x, y); x -= 16)
            DrawLayerRect(bg, x - 16, x, y, y + 4, (float)(16 - x), sy, -1, -1, true, 0, 160);
        for (int x = 240; x < VIEW_RIGHT && !MarginBlack(x, y); x += 16)
            DrawLayerRect(bg, x, x + 16, y, y + 4, (float)(480 - x), sy, -1, -1, true, 0, 160);
    }
    if (sScrolls[bg]) return;
    for (int y = 0; y > VIEW_TOP && !MarginBlack(120, y); y -= 4)
    {
        float sy = (float)(4 - y);

        DrawLayerRect(bg, 0, 240, y - 4, y, 0, sy, 1, -1, true, 0, 160);
        for (int x = 0; x > VIEW_LEFT && !MarginBlack(x, y); x -= 16)
            DrawLayerRect(bg, x - 16, x, y - 4, y, (float)(16 - x), sy, -1, -1, true, 0, 160);
        for (int x = 240; x < VIEW_RIGHT && !MarginBlack(x, y); x += 16)
            DrawLayerRect(bg, x, x + 16, y - 4, y, (float)(480 - x), sy, -1, -1, true, 0, 160);
    }
}

static bool DrawStageBgTex(unsigned bg)
{
    int top, bottom;
    bool wraps;

    if (!sStage || !LayerDrawable(bg)) return false;
    if (sStageFaded)
    {
        ViewBase();
        DrawFadedStageTex(bg);
        return true;
    }
    PictureRows(bg, &top, &bottom);
    wraps = sScrolls[bg];
    if (!wraps)
    {
        MeasureStill(bg);
        /* A still panorama wraps too when the scene around it scrolls. */
        if (sPanorama[bg])
            for (unsigned other = 0; other < 4; ++other)
                if (other != bg && sScrolls[other] && (Reg(0) & (0x100u << other))) wraps = true;
    }
    ViewBase();
    if (wraps)
    {
        DrawLayerRect(bg, VIEW_LEFT, VIEW_RIGHT, top, bottom, VIEW_LEFT, top, 1, 1, false, top, bottom);
        DrawLayerRect(bg, VIEW_LEFT, VIEW_RIGHT, VIEW_TOP, top, VIEW_LEFT, 2 * top - VIEW_TOP, 1, -1,
                      false, top, bottom);
        DrawLayerRect(bg, VIEW_LEFT, VIEW_RIGHT, bottom, VIEW_BOTTOM, VIEW_LEFT, bottom, 1, -1,
                      false, top, bottom);
        return true;
    }
    DrawLayerRect(bg, 0, 240, top, bottom, 0, top, 1, 1, false, top, bottom);
    if (DrawLeavesMargins(bg)) return true;
    for (int x = 0; x > VIEW_LEFT; x -= 8)
        DrawLayerRect(bg, x - 8, x, top, bottom, 0, top, 1, 1, true, top, bottom);
    for (int x = 240; x < VIEW_RIGHT; x += 8)
        DrawLayerRect(bg, x, x + 8, top, bottom, 232, top, 1, 1, true, top, bottom);
    for (int y = top; y > VIEW_TOP; y -= 8)
        DrawEdgeBand(bg, y - 8, y, top, top, bottom);
    for (int y = bottom; y < VIEW_BOTTOM; y += 8)
        DrawEdgeBand(bg, y, y + 8, bottom - 8, top, bottom);
    /*
     * The corners: the 8x8 corner block of the picture, in 16-pixel cells
     * each with its own fade, so the gradient bends round the corner instead
     * of being smeared across one quad. Cells already black are left to
     * StageUnderlay when it drew.
     */
    for (int y = top; y > VIEW_TOP; y -= 16)
        for (int x = 0; x > VIEW_LEFT; x -= 16)
        {
            DrawCornerCell(bg, x - 16, x, y - 16, y, 0, top, top, bottom);
            DrawCornerCell(bg, 240 - x, 256 - x, y - 16, y, 232, top, top, bottom);
        }
    for (int y = bottom; y < VIEW_BOTTOM; y += 16)
        for (int x = 0; x > VIEW_LEFT; x -= 16)
        {
            DrawCornerCell(bg, x - 16, x, y, y + 16, 0, bottom - 8, top, bottom);
            DrawCornerCell(bg, 240 - x, 256 - x, y, y + 16, 232, bottom - 8, top, bottom);
        }
    return true;
}

static bool DrawLineBg(unsigned bg)
{
    unsigned baseX = Reg(0x10 + bg * 4) & 511, baseY = Reg(0x12 + bg * 4) & 511;
    /* A still stage fades its margins (DrawEdgeRegion); so do these layers,
     * and every layer of a faded one (the cable car). */
    bool fade = sStage && (!sScrolls[bg] || sStageFaded);
    int top = 0, bottom = 160;
    int clipX0 = sClipX0, clipX1 = sClipX1, clipY0 = sClipY0, clipY1 = sClipY1;
    /* The battle terrain repeats its top tile row above the picture, as
     * DrawBattleBg does; this is the line where that starts. */
    bool clampTop = sBattle && bg == 3;
    C3D_Tex *tex;
    float width, height;
    BattlePictureView view;
    bool picture;

    if (!(LineBackgrounds() & (1u << bg)) || !LayerDrawable(bg)) return false;
    tex = &sLayers[bg].tex;
    width = tex->width;
    height = tex->height;
    if (fade) PictureRows(bg, &top, &bottom);
    /* A move's picture, magnified to the screen (BattlePicture). */
    picture = BattlePictureBegin(bg, &view);
    if (picture)
    {
        clipX0 = sClipX0;
        clipX1 = sClipX1;
        clipY0 = sClipY0;
        clipY1 = sClipY1;
        clampTop = false;
    }
    else if (sBattle && !(Reg(8 + bg * 2) & 0x4000))
    {
        /* A 32-column layer only wraps into the sides when it is made to. */
        MeasureStill(bg);
        if (!sPanorama[bg])
        {
            if (clipX0 < 0) clipX0 = 0;
            if (clipX1 > 240) clipX1 = 240;
            if (clipY0 < 0) clipY0 = 0;
            if (clipY1 > 160) clipY1 = 160;
            clampTop = false;
        }
    }
    ViewBase();
    for (int y = clipY0; y < clipY1;)
    {
        /* The split battle terrain (BattleTerrainLine): line y shows line sy. */
        bool split = BattleTerrainSplit(bg);
        int sy = split ? BattleTerrainLine(y) : y;
        unsigned x = LineValue(bg * 2, sy, baseX), v = LineValue(bg * 2 + 1, sy, baseY);
        int end = y + 1;
        /* The texture row line y shows. */
        int ty = (int)v + sy;
        int rowTop = -(int)(v & 7);

        if (clampTop && sy < rowTop)
        {
            /* The tile row under line 0, once per eight lines above it. */
            int phase = ((sy - rowTop) % 8 + 8) % 8;

            ty = (int)(v - (v & 7)) + phase;
            end = y + 8 - phase;
            if (end > clipY1) end = clipY1;
        }
        else while (end < clipY1 && (!split || BattleTerrainLine(end) - end == sy - y)
               && LineValue(bg * 2, split ? BattleTerrainLine(end) : end, baseX) == x
               && LineValue(bg * 2 + 1, split ? BattleTerrainLine(end) : end, baseY) == v)
        {
            /* The fade is linear between these lines, so a run stops at them. */
            if (fade && (end == top || end == bottom || end == top - (int)FADE_DOWN
                         || end == bottom + (int)FADE_DOWN))
                break;
            ++end;
        }
        /* Left margin, picture, right margin: each piece gets its own fade. */
        for (unsigned piece = 0; piece < 3; ++piece)
        {
            int x0 = fade ? (piece == 0 ? clipX0 : piece == 1 ? 0 : 240) : clipX0;
            int x1 = fade ? (piece == 0 ? 0 : piece == 1 ? 240 : clipX1) : clipX1;

            if (x0 < clipX0) x0 = clipX0;
            if (x1 > clipX1) x1 = clipX1;
            if (x0 >= x1) { if (!fade) break; continue; }
            {
                /* The texture repeats, so coordinates past its edges wrap. */
                const Tex3DS_SubTexture run = {(u16)(x1 - x0), (u16)(end - y),
                    ((int)x + x0) / width, 1.0f - ty / height,
                    ((int)x + x1) / width, 1.0f - (ty + end - y) / height};
                C2D_ImageTint tint = sTint;

                if (fade)
                {
                    bool white;
                    float bright = LayerBrightness(bg, &white);

                    FadeCorner(&tint, C2D_TopLeft, EdgeFade(x0, y, top, bottom), bright, white);
                    FadeCorner(&tint, C2D_TopRight, EdgeFade(x1, y, top, bottom), bright, white);
                    FadeCorner(&tint, C2D_BotLeft, EdgeFade(x0, end, top, bottom), bright, white);
                    FadeCorner(&tint, C2D_BotRight, EdgeFade(x1, end, top, bottom), bright, white);
                }
                ++sStats.tiles;
                C2D_DrawImageAt((C2D_Image){tex, &run}, x0 + CTR_VIEW_X + sLayerShift,
                                y + CTR_VIEW_Y, 0, &tint, 1, 1);
            }
            if (!fade) break;
        }
        y = end;
    }
    if (picture) BattlePictureEnd(&view);
    return true;
}

static void DrawAffineBg(unsigned bg)
{
    /*
     * The Berry Blender's bowl turns on an affine layer that wraps round: in
     * the margins, where centred screens carry their affine maps on (below),
     * copies of the bowl went round the screen's edges. Kept to the picture.
     */
    if (sCentred && sCentredScreen == CTR_CENTRED_BLENDER
     && (sClipX0 < 0 || sClipY0 < 0 || sClipX1 > 240 || sClipY1 > 160))
    {
        int clipX0 = sClipX0, clipY0 = sClipY0, clipX1 = sClipX1, clipY1 = sClipY1;
        bool scissored = sScissored;

        if (sClipX0 < 0) sClipX0 = 0;
        if (sClipY0 < 0) sClipY0 = 0;
        if (sClipX1 > 240) sClipX1 = 240;
        if (sClipY1 > 160) sClipY1 = 160;
        if (sClipX0 < sClipX1 && sClipY0 < sClipY1)
        {
            C2D_Flush();
            sScissored = true;
            Scissor(sClipX0, sClipY0, sClipX1, sClipY1);
            DrawAffineBg(bg);
            C2D_Flush();
        }
        sClipX0 = clipX0;
        sClipY0 = clipY0;
        sClipX1 = clipX1;
        sClipY1 = clipY1;
        sScissored = scissored;
        RestoreScissor();
        return;
    }
    unsigned control = Reg(8 + bg * 2), base = bg == 2 ? 0x20 : 0x30;
    float a = (int16_t)Reg(base) / 256.0f, b = (int16_t)Reg(base + 2) / 256.0f;
    float c = (int16_t)Reg(base + 4) / 256.0f, d = (int16_t)Reg(base + 6) / 256.0f;
    float rx = CtrVideo_AffineReference(Reg(base + 8) | (Reg(base + 10) << 16)) / 256.0f;
    float ry = CtrVideo_AffineReference(Reg(base + 12) | (Reg(base + 14) << 16)) / 256.0f;
    float det = a * d - b * c;
    if (fabsf(det) < 0.00001f) { Error(5, "singular affine BG matrix"); return; }
    C3D_Mtx matrix;
    Mtx_Identity(&matrix);
    matrix.r[0] = FVec4_New(d / det, -b / det, 0, (b * ry - d * rx) / det + CTR_VIEW_X + sLayerShift);
    matrix.r[1] = FVec4_New(-c / det, a / det, 0, (c * rx - a * ry) / det + CTR_VIEW_Y);
    ViewAffine(&matrix);
    /* The box of the map under the clip rectangle's four corners. */
    float minX = INFINITY, maxX = -INFINITY, minY = INFINITY, maxY = -INFINITY;
    for (unsigned corner = 0; corner < 4; ++corner)
    {
        float x = (corner & 1) ? sClipX1 : sClipX0;
        float y = (corner & 2) ? sClipY1 : sClipY0;
        float sx = rx + a * x + b * y, sy = ry + c * x + d * y;
        minX = fminf(minX, sx); maxX = fmaxf(maxX, sx);
        minY = fminf(minY, sy); maxY = fmaxf(maxY, sy);
    }
    int firstX = (int)floorf(minX / 8), lastX = (int)floorf(maxX / 8);
    int firstY = (int)floorf(minY / 8), lastY = (int)floorf(maxY / 8);
    unsigned tiles = 16u << (control >> 14), mask = tiles - 1;
    unsigned map = ((control >> 8) & 31) * 0x800, chars = ((control >> 2) & 3) * 0x4000;
    /*
     * A centred GBA screen fills its margins with its affine map repeated,
     * wrap-around or not: without it the space above and left of the picture
     * would be empty while the map's own sea shows below and to the right.
     * Not the roulette's wheel: repeated, copies of it turned round the
     * margins beside the table.
     */
    bool wrap = (control & 0x2000) || (sCentred && sCentredScreen != CTR_CENTRED_ROULETTE);
    /*
     * Under minification the screen maps to far more of the map than exists.
     * Without wrap-around nothing outside the map is drawn anyway, so clamping
     * the scan to it bounds the work by the map instead of by how small the
     * layer was scaled: the title logo shrinks enough to blow any fixed budget
     * and would disappear for those frames.
     */
    if (!wrap)
    {
        if (firstX < 0) firstX = 0;
        if (firstY < 0) firstY = 0;
        if (lastX >= (int)tiles) lastX = (int)tiles - 1;
        if (lastY >= (int)tiles) lastY = (int)tiles - 1;
    }
    if (lastX < firstX || lastY < firstY) return;
    if ((lastX - firstX + 1) * (lastY - firstY + 1) > 16384)
    { Error(6, "affine BG minification exceeds baseline budget"); return; }
    for (int y = firstY; y <= lastY; ++y)
        for (int x = firstX; x <= lastX; ++x)
        {
            if (!wrap && ((unsigned)x >= tiles || (unsigned)y >= tiles)) continue;
            unsigned entry = map + ((unsigned)y & mask) * tiles + ((unsigned)x & mask);
            if (entry >= 0x10000) { Error(7, "affine map outside BG VRAM"); continue; }
            DrawTile(chars + sMemory.vram[entry] * 64, 0, true, x * 8, y * 8, false, false);
        }
    ViewBase();
}

/*
 * Where a sprite placed on the screen, not on the map, goes in the field view
 * (400x240, the 240x160 picture's own layout on it). Those of a window - the
 * item shown by the PC, the mon shown for a move - sit where their window
 * layer does: BG0 is drawn moved by CTR_FIELD_UI_SHIFT (the field banner's
 * streaks excepted), and they follow it. The healing machine's balls and
 * monitors were placed by hand on the picture around the player (the player's
 * tile centre at GBA (120, 80)), so they go where the map's machine is: in
 * the 2D field the picture's offset on the view, in the voxel one the
 * machine's tile seen by the camera. Fly's bird and its rider fly round the
 * player on that picture: they are moved with it, whole, in the voxel field
 * to where the camera sees the player (the GBA's (120, 80), as the circle of
 * a dark cave) - not projected, which would bend the bird's arc.
 */
static void PlaceScreenObject(int i, unsigned boxW, unsigned boxH, bool voxel, int *x, int *y)
{
    if (sFlyOam[i >> 5] & (1u << (i & 31)))
    {
        float px, py, scaleX, scaleY;

        if (voxel && CtrVoxel_PlayerLightSpot(&px, &py, &scaleX, &scaleY))
        {
            *x += (int)lroundf(px) - 120;
            *y += (int)lroundf(py) - 80;
            return;
        }
        *x += CTR_STAGE_X;
        *y += CTR_STAGE_Y - 8;
        return;
    }
    if (sMachineOam[i >> 5] & (1u << (i & 31)))
    {
        if (voxel)
        {
            float sx, sy;

            if (CtrVoxel_ProjectPictureTile((*x + boxW / 2.0f - 120.0f) / 16.0f,
                                            (*y + boxH / 2.0f - 80.0f) / 16.0f, &sx, &sy))
            {
                *x = (int)(sx - boxW / 2.0f);
                *y = (int)(sy - boxH / 2.0f);
            }
            return;
        }
        if (!voxel)
        {
            /* The 2D field shows the picture 8 lines above the stage's
             * centre: the player's tile, GBA (112, 72), is at (192, 104). */
            *x += CTR_STAGE_X;
            *y += CTR_STAGE_Y - 8;
        }
        return;
    }
    /* With the text band, and in the voxel overlay at its depth too; the
     * machine's sprites above are at the world's own. */
    if (!sFieldBanner) *x += (int)CTR_FIELD_UI_SHIFT + (voxel ? sOverlayEyeShift : 0);
}

/*
 * The fog's picture repeated over the whole clip area, on the lattice the
 * sprite at (x, y) sits on. The game lays twenty sprites for a 240x160
 * screen and lets OAM wrap them round its edges; on this view that left the
 * right of the screen bare, and the 8-bit y of a row past the bottom read as
 * a row further down instead of the one at the top. The lattice does not
 * care which sprite it starts from, so any wrap of x or y is harmless here.
 * The 64 tiles are looked up once and drawn at every place, batched as the
 * sprites are (TileWalkSlot): the sandstorm's and the ash's lattice is some
 * two thousand tiles over the wide view, and one Citro2D quad each was most
 * of an Old 3DS frame.
 */
static void DrawFogLattice(unsigned attr2, unsigned bank, unsigned width, unsigned height, bool color256,
                           int x, int y, bool flipX, bool flipY)
{
    int slots[8 * 8];
    unsigned columns = width / 8, rows = height / 8;
    int x0 = x % (int)width, y0 = y % (int)height;
    bool batch;

    if (columns * rows > sizeof(slots) / sizeof(slots[0])) return;
    for (unsigned ty = 0; ty < rows; ++ty)
        for (unsigned tx = 0; tx < columns; ++tx)
        {
            unsigned sx = flipX ? columns - 1 - tx : tx;
            unsigned sy = flipY ? rows - 1 - ty : ty;
            unsigned tile = CtrVideo_ObjTile(attr2 & 1023, sx, sy, width, color256, Reg(0) & 0x40);
            slots[ty * columns + tx] = GetTileSlot(0x10000 + tile * 32, bank, color256);
        }
    if (x0 > sClipX0) x0 -= (int)width * ((x0 - sClipX0 + (int)width - 1) / (int)width);
    if (y0 > sClipY0) y0 -= (int)height * ((y0 - sClipY0 + (int)height - 1) / (int)height);
    batch = TileWalkBegin();
    for (int py = y0; py < sClipY1; py += (int)height)
        for (int px = x0; px < sClipX1; px += (int)width)
        {
            if (px + (int)width <= sClipX0 || py + (int)height <= sClipY0) continue;
            for (unsigned ty = 0; ty < rows; ++ty)
            {
                int top = py + (int)ty * 8;

                /* Only the tiles in the clip: a band of a window partition
                 * is a strip of the view. */
                if (top >= sClipY1 || top + 8 <= sClipY0) continue;
                for (unsigned tx = 0; tx < columns; ++tx)
                {
                    int slot = slots[ty * columns + tx], left = px + (int)tx * 8;

                    if (slot >= 0 && left < sClipX1 && left + 8 > sClipX0)
                        TileWalkSlot(batch, slot, left + CTR_VIEW_X + sLayerShift, top + CTR_VIEW_Y, flipX, flipY);
                }
            }
        }
    TileWalkEnd(batch);
}

/*
 * Sprite tiles without Citro2D's per-quad work, as layer cells are drawn
 * (FastCells): a sprite was up to 64 C2D_DrawImageAt calls, ~4.6 us of CPU
 * each on an Old 3DS. The tiles of consecutive sprites that share a blend
 * state go into the cells' vertex buffer and out in one indexed draw, on
 * Citro2D's own program and texture combiner - set up for the atlas and the
 * current tint by an empty Citro2D draw - with the tint, which is the same at
 * every corner, as fixed attributes: a Citro2D vertex carries its tint's blend
 * in its second point coordinate and its colour as four bytes. Only plain
 * sprites at 1:1 and whole pixels take this path; the rest are drawn as before.
 */
static unsigned sSpriteFirst;
static bool sSpritePending;

static void SpriteBatchFlush(void)
{
    C3D_AttrInfo savedAttr, *attr;
    C3D_BufInfo savedBuf, *buf;
    Tex3DS_SubTexture none = {0, 0, 0.0f, 1.0f, 0.0f, 1.0f};
    uint32_t color = sTint.corners[0].color;

    if (!sSpritePending)
        return;
    sSpritePending = false;
    if (sSpriteCount == 0)
        return;
    /* The batch's vertices are in the base view. An affine sprite drawn just
     * before it opened (the bag) leaves its own matrix set, which carried the
     * whole batch - the bag's item icon - off by that sprite's place. */
    ViewBase();
    C2D_DrawImageAt((C2D_Image){&sAtlas, &none}, 0, 0, 0, &sTint, 1, 1);
    C2D_Flush();
    attr = C3D_GetAttrInfo();
    savedAttr = *attr;
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_SHORT, 2);
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);
    AttrInfo_AddFixed(attr, 2);
    AttrInfo_AddFixed(attr, 3);
    C3D_FixedAttribSet(2, 0.0f, sTint.corners[0].blend, 0.0f, 0.0f);
    C3D_FixedAttribSet(3, (float)(color & 255), (float)((color >> 8) & 255),
                       (float)((color >> 16) & 255), (float)(color >> 24));
    buf = C3D_GetBufInfo();
    savedBuf = *buf;
    BufInfo_Init(buf);
    BufInfo_Add(buf, sFastVertices, sizeof(FastVertex), 2, 0x10);
    FastDraw(sSpriteFirst, sSpriteCount);
    C3D_SetAttrInfo(&savedAttr);
    C3D_SetBufInfo(&savedBuf);
    sSpriteCount = 0;
}

/* Room for a sprite of `tiles` tiles in the batch, opening one if needed. */
static bool SpriteBatchRoom(unsigned tiles)
{
    if (!sFastReady || sFastUsed + tiles > FAST_QUADS)
        return false;
    if (!sSpritePending)
    {
        sSpritePending = true;
        sSpriteFirst = sFastUsed;
        sSpriteCount = 0;
    }
    return true;
}

/*
 * One sprite, already placed: its tiles batched when they can be, drawn one by
 * one otherwise (an affine sprite, a fractional position, a full batch).
 */
static void DrawObjectAt(int x, int y, unsigned attr1, unsigned attr2, unsigned bank, unsigned width,
                         unsigned height, unsigned boxW, unsigned boxH, bool affine, bool color256, bool effects,
                         unsigned mode)
{
    if (x >= sClipX1 || x + (int)boxW <= sClipX0
     || y >= sClipY1 || y + (int)boxH <= sClipY0) return;
    ++sStats.sprites;
    {
        /* A new blend state ends the batch: Blend changes the GPU state. */
        static unsigned batchKey = ~0u;
        unsigned key = (effects ? 1u : 0u) | (mode == 1 ? 2u : 0u) | (sDayNightObject ? 4u : 0u);
        float shiftX = x + CTR_VIEW_X + sLayerShift;
        int whole = (int)shiftX;

        if (sSpritePending && key != batchKey) SpriteBatchFlush();
        /* At any zoom: the view is the GPU's, for these quads as for
         * Citro2D's; the battle scene composes its sprites at 2x. */
        if (!affine && (float)whole == shiftX
            && SpriteBatchRoom((width / 8) * (height / 8)))
        {
            bool flipX = (attr1 & 0x1000) != 0, flipY = (attr1 & 0x2000) != 0;

            /* A batch just opened: its blend state and tint. */
            if (sSpriteCount == 0) Blend(4, effects, mode == 1);
            batchKey = key;
            sSpritePending = true;
            for (unsigned ty = 0; ty < height / 8; ++ty)
                for (unsigned tx = 0; tx < width / 8; ++tx)
                {
                    unsigned sx = flipX ? width / 8 - 1 - tx : tx;
                    unsigned sy = flipY ? height / 8 - 1 - ty : ty;
                    unsigned tile = CtrVideo_ObjTile(attr2 & 1023, sx, sy, width, color256, Reg(0) & 0x40);
                    int slot = GetTileSlot(0x10000 + tile * 32, bank, color256);

                    if (slot < 0) continue;
                    ++sStats.tiles;
                    FastQuad(sFastVertices + sFastUsed * 4, whole + (int)tx * 8, y + CTR_VIEW_Y + (int)ty * 8,
                             slot, flipX, flipY);
                    ++sFastUsed;
                    ++sSpriteCount;
                }
            return;
        }
        SpriteBatchFlush();
        batchKey = ~0u;
    }
    Blend(4, effects, mode == 1);
    ViewBase();
    if (affine)
    {
        unsigned index = ((attr1 >> 9) & 31) * 16;
        float a = (int16_t)sMemory.oam[index + 3] / 256.0f;
        float b = (int16_t)sMemory.oam[index + 7] / 256.0f;
        float c = (int16_t)sMemory.oam[index + 11] / 256.0f;
        float d = (int16_t)sMemory.oam[index + 15] / 256.0f;
        float det = a * d - b * c;
        if (fabsf(det) < 0.00001f) { Error(9, "singular OBJ affine matrix"); return; }
        C3D_Mtx matrix;
        Mtx_Identity(&matrix);
        matrix.r[0] = FVec4_New(d / det, -b / det, 0, x + CTR_VIEW_X + sLayerShift + boxW / 2.0f - (d * width - b * height) / (2 * det));
        matrix.r[1] = FVec4_New(-c / det, a / det, 0, y + CTR_VIEW_Y + boxH / 2.0f - (a * height - c * width) / (2 * det));
        ViewAffine(&matrix);
    }
    bool flipX = !affine && (attr1 & 0x1000), flipY = !affine && (attr1 & 0x2000);
    for (unsigned ty = 0; ty < height / 8; ++ty)
        for (unsigned tx = 0; tx < width / 8; ++tx)
        {
            unsigned sx = flipX ? width / 8 - 1 - tx : tx;
            unsigned sy = flipY ? height / 8 - 1 - ty : ty;
            unsigned tile = CtrVideo_ObjTile(attr2 & 1023, sx, sy, width, color256, Reg(0) & 0x40);
            DrawTile(0x10000 + tile * 32, bank, color256,
                     (affine ? 0 : x + CTR_VIEW_X + sLayerShift) + (int)tx * 8,
                     (affine ? 0 : y + CTR_VIEW_Y) + (int)ty * 8, flipX, flipY);
        }
}

static void DrawObjects(unsigned priority, bool effects)
{
    bool fogDrawn = false, latticeDrawn = false;
    float layerShift = sLayerShift;

    for (int i = 127; i >= 0; --i)
    {
        bool fog = (sFogOam[i >> 5] & (1u << (i & 31))) != 0;
        bool lattice = (sLatticeOam[i >> 5] & (1u << (i & 31))) != 0;
        bool particle = (sParticleOam[i >> 5] & (1u << (i & 31))) != 0;

        /* The fog is one picture: drawn once, over the whole view. So are
         * the ash and the sandstorm's sand. */
        if ((fog && fogDrawn) || (lattice && latticeDrawn)) continue;
        /* The voxel world draws a fog of its own, in the scene. */
        if (fog && sVoxelObjPass != VOXEL_OBJ_NONE && CtrVoxel_DrawsFog()) continue;
        /* ... and its own rain, sand and ash (DrawWeather): the sand's
         * swirls too, which are particles. */
        if ((lattice || particle) && sVoxelObjPass != VOXEL_OBJ_NONE && CtrVoxel_DrawsWeather()) continue;
        /* The clouds lie on the voxel world's water (voxel_entities.c). */
        if (sVoxelObjPass != VOXEL_OBJ_NONE && (sVoxelCloudOam[i >> 5] & (1u << (i & 31))))
            continue;
        if (sVoxelObjPass != VOXEL_OBJ_NONE)
        {
            bool weather = (sVoxelWeatherOam[i >> 5] & (1u << (i & 31))) != 0;
            bool screen = (sScreenOam[i >> 5] & (1u << (i & 31))) != 0;

            if (sVoxelObjPass == VOXEL_OBJ_WEATHER ? !weather
                : sVoxelObjPass == VOXEL_OBJ_FIELD ? (weather || screen) : (weather || !screen))
                continue;
        }
        if (sObjFilter != OBJ_ALL && TransitionOam((unsigned)i) != (sObjFilter == OBJ_TRANSITION))
            continue;
        if (sObjMaskOn && !(sObjMask[i >> 5] & (1u << (i & 31)))) continue;
        if (i == WIRELESS_INDICATOR_OAM && !sObjIndicatorPass && WirelessIndicatorApart()) continue;
        const uint16_t *obj = sMemory.oam + i * 4;
        unsigned attr0 = obj[0], attr1 = obj[1], attr2 = obj[2];
        bool affine = (attr0 & 0x100) != 0, color256 = (attr0 & 0x2000) != 0;
        if ((!affine && (attr0 & 0x200)) || ((attr2 >> 10) & 3) != priority) continue;
        unsigned mode = (attr0 >> 10) & 3, shape = attr0 >> 14;
        /* A window's sprites are its shape, drawn only into its mask. */
        if ((mode == 2) != (sObjWinPass == OBJWIN_MASK)) continue;
        if (mode == 3 || shape == 3) continue;
        /* Only a mosaic that shows: the game sets the bit with a size of 0. */
        if ((attr0 & 0x1000) && (Reg(0x4c) & 0xff00)) Error(8, "OBJ mosaic not supported");
        unsigned width = sObjDims[shape][attr1 >> 14][0], height = sObjDims[shape][attr1 >> 14][1];
        unsigned boxW = affine && (attr0 & 0x200) ? width * 2 : width;
        unsigned boxH = affine && (attr0 & 0x200) ? height * 2 : height;
        int x = attr1 & 511, y = attr0 & 255;
        if (sStage || sCentred || sBattle || sTransitionCompose)
        {
            /*
             * The GBA's own reading: a sprite only comes back from the other
             * side when it crosses the end of the coordinate range. Unused
             * entries sit at y=240 x=464, which the wide view would otherwise
             * turn into a column of blank tiles at (-48, -16), in its margin.
             */
            if (x + (int)boxW > 512) x -= 512;
            if (y + (int)boxH > 256) y -= 256;
        }
        else
        {
            if (x >= VIEW_RIGHT) x -= 512;
            if (sOamAnchored[i >> 5] & (1u << (i & 31))) y = OamUnwrapY((unsigned)i, y);
            else if (y >= VIEW_BOTTOM) y -= 256;
            if (sVoxelObjPass == VOXEL_OBJ_WEATHER)
            {
                x += CTR_STAGE_X;
                y += CTR_STAGE_Y;
            }
            else if (sVoxelObjPass == VOXEL_OBJ_SCREEN)
                PlaceScreenObject(i, boxW, boxH, true, &x, &y);
            if (sFieldUi && (sScreenOam[i >> 5] & (1u << (i & 31))))
                PlaceScreenObject(i, boxW, boxH, false, &x, &y);
        }
        /* A battle in 3D: each sprite at its own depth (BattleSpriteShift). */
        if (sSpriteDepth) sLayerShift = BattleSpriteShift(y + (int)boxH / 2) / sShiftZoom;
        /* An info box drawn from its picture: its sprites are not drawn. */
        if (sHud && sBattle && sObjWinPass == OBJWIN_NONE)
        {
            int box = sHudOamBox[i];

            if (box >= 0)
            {
                unsigned tile = attr2 & 1023;

                /* The halves, not the bar: it lies over them. */
                /* Only one in sight: the GBA's unused entries are tile 0 too. */
                if (!sHudCrisp && tile >= sHud[box].boxTile && tile < (unsigned)sHud[box].boxTile + sHud[box].boxTiles
                    && x < sClipX1 && x + (int)boxW > sClipX0 && y < sClipY1 && y + (int)boxH > sClipY0)
                    HudDrawInScene((unsigned)box, tile, x, y, (int)boxW, (int)boxH, effects);
                continue;
            }
        }
        /* Below the PokeNav's picture is its background, not the space the
         * GBA parks its unused sprites in. */
        if (sCentredScreen == CTR_CENTRED_POKENAV && y >= 160) continue;
        bool nightObject = !sStage && !sCentred && !sBattle
                        && CtrDayNight_Get()->daylight != 1.0f
                        && (sScreenOam[i >> 5] & (1u << (i & 31))) == 0;

        if (sDayNightObject != nightObject)
            SpriteBatchFlush();
        sDayNightObject = nightObject;
        /*
         * Any one of the fog's sprites gives the lattice its phase, on the
         * screen or not: as a map is entered the game can have all twenty
         * of them outside it until the camera first moves.
         */
        if ((fog || lattice) && !affine)
        {
            ++sStats.sprites;
            SpriteBatchFlush();
            Blend(4, effects, mode == 1);
            ViewBase();
            DrawFogLattice(attr2, ObjBank((unsigned)i, attr2), width, height, color256, x, y,
                           (attr1 & 0x1000) != 0, (attr1 & 0x2000) != 0);
            if (fog) fogDrawn = true;
            else latticeDrawn = true;
            continue;
        }
        /*
         * Rain, snow, the sand swirls and the bubbles fall over the GBA's
         * 240x160 screen and wrap round its edges: copies of the screen side
         * by side carry them over the whole view, without a seam.
         */
        if (particle && !affine)
        {
            int left = x, top = y;

            while (left + (int)boxW > sClipX0) left -= 240;
            while (top + (int)boxH > sClipY0) top -= 160;
            for (int cy = top; cy < sClipY1; cy += 160)
                for (int cx = left; cx < sClipX1; cx += 240)
                    DrawObjectAt(cx, cy, attr1, attr2, ObjBank((unsigned)i, attr2), width, height, boxW, boxH,
                                 false, color256, effects, mode);
            continue;
        }
        DrawObjectAt(x, y, attr1, attr2, ObjBank((unsigned)i, attr2), width, height, boxW, boxH, affine, color256,
                     effects, mode);
    }
    SpriteBatchFlush();
    sLayerShift = layerShift;
    ViewBase();
}

/*
 * The circle of light of a dark cave (CtrVideo_SetFieldLight). The game opens
 * window 0 round the player line by line and leaves only the text layer and
 * the backdrop outside it; here the field keeps window 0 whole and the dark is
 * drawn over the field under the text layer instead. The 2D field draws the
 * GBA's own circle, row for row; the voxel world a soft one (VoxelFlashLight).
 */
static struct
{
    bool on;
    int x, y, radius;
    /* For the voxel light: the radius it shows, eased towards the game's,
     * the glow of Flash being used, and a frame count for the flicker. */
    float shown, burst;
    uint32_t ticks;
} sFieldLight;
/* The backdrop shown this frame, faded: what lies outside the window. */
static uint32_t sBackdropShown = 0xFF000000u;
/* The field is the voxel world this frame: its dark is VoxelFlashLight's. */
static bool sFieldVoxel;
/*
 * The swoop between the two renderers of the field (voxel_swoop.c). Which
 * renderer drew the last field frame - the voxel world or the 2D layers -
 * is kept across the frames that draw neither (a map warming up is black),
 * and a change of it starts the swoop: the option toggled, a map with or
 * without voxel data entered. Frames off the field (a battle, a menu, the
 * title) forget it, so coming back from those is the cut it always was.
 */
enum { FIELD_MODE_NONE, FIELD_MODE_PIXEL, FIELD_MODE_VOXEL };
/* How long the 2D field may wait for the voxel world's chunks (1.5 s). */
#define VOXEL_SWOOP_HOLD_FRAMES 90u
/* The most game frames one shown picture may step the swoop by. */
#define VOXEL_SWOOP_STEPS_MAX 4u
static unsigned sFieldMode;
static VoxelSwoop sSwoop;
/*
 * The Fly cinematic (voxel_fly.c), started by the game's Fly through
 * 3ds_game_full.c (CtrVideo_FlyBegin) once the player is on the bird, and
 * stepped here beside the swoop. Only from a voxel field frame: in the 2D
 * field, or with the option off, Fly plays as the GBA's.
 */
static VoxelFly sFly;
/* The map's version (CtrMap_Version) as the flight began: the warp loads
 * the map again, whatever the frames in between show (the load's frame is
 * often dropped, so a frame with no field is not to be counted on). */
static uint32_t sFlyMapVersion;

static bool SkySceneLoad(float dirX, float dirZ);
static void SkySceneFree(void);

bool CtrVideo_FlyBegin(float dirX, float dirZ)
{
    bool sky;

    if (sFieldMode != FIELD_MODE_VOXEL || VoxelFly_Active(&sFly) || VoxelSwoop_Active(&sSwoop)
        || CtrVoxel_CloudTexture() == NULL)
        return false;
    sky = CtrFlySky_Enabled() && SkySceneLoad(dirX, dirZ);
    VoxelFly_Begin(&sFly, dirX, dirZ, sky);
    sFlyMapVersion = CtrMap_Version();
    CtrLog_Write(CTR_LOG_VIDEO, "FLY cinematic begins towards (%.2f, %.2f)%s", (double)sFly.dirX, (double)sFly.dirZ,
                 sky ? " with the sky scene" : "");
    return true;
}

bool CtrVideo_FlyHovers(void)
{
    return VoxelFly_Hovers(&sFly);
}

bool CtrVideo_FlyBusy(void)
{
    return VoxelFly_Busy(&sFly);
}

bool CtrVideo_FlyLanding(float *share)
{
    return VoxelFly_Landing(&sFly, share);
}

void CtrVideo_SetFieldLight(bool on, int x, int y, int radius)
{
    if (on && (!sFieldLight.on || sFieldLight.radius < 0))
        sFieldLight.shown = (float)radius;
    else if (on && radius > sFieldLight.radius && sFieldLight.shown < radius - 6.0f)
        sFieldLight.burst = 1.0f;
    sFieldLight.on = on;
    if (!on) { sFieldLight.burst = 0.0f; return; }
    sFieldLight.x = x;
    sFieldLight.y = y;
    sFieldLight.radius = radius;
    ++sFieldLight.ticks;
    /* The game grows the circle a pixel every other frame; eased, it opens
     * smoothly instead, and catches up within a few frames once it stops. */
    sFieldLight.shown += ((float)radius - sFieldLight.shown) * 0.25f;
    if (radius > sFieldLight.shown - 0.5f && radius < sFieldLight.shown + 0.5f) sFieldLight.shown = (float)radius;
    sFieldLight.burst *= 0.955f;
}

static bool FieldLightShown(void)
{
    return sFieldLight.on && !sTransitionCompose && !sStage && !sBattle && !sCentred;
}

/*
 * The 2D dark: the GBA's own circle, made by the midpoint walk the game uses
 * (SetFlashScanlineEffectWindowBoundaries), so every row opens exactly as
 * wide as it did there - the edges [x - half, x + half) - and outside it the
 * backdrop. Rows of the same width are one rectangle each side.
 */
static void FieldDarkness(void)
{
    static int16_t half[256];
    static int cached = -1;
    int r = sFieldLight.radius, cx = sFieldLight.x, cy = sFieldLight.y;
    int left = VIEW_LEFT, right = VIEW_RIGHT, top = VIEW_TOP, bottom = VIEW_BOTTOM;
    float ox = CTR_VIEW_X + sLayerShift, oy = CTR_VIEW_Y;
    uint32_t colour = sBackdropShown;

    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (cached != r)
    {
        int x = r, d = r, y = 0;

        for (int i = 0; i <= r; ++i) half[i] = -1;
        while (x >= y)
        {
            half[y] = (int16_t)x;
            half[x] = (int16_t)y;
            d -= (y * 2) - 1;
            y++;
            if (d < 0)
            {
                d += 2 * (x - 1);
                x--;
            }
        }
        cached = r;
    }
    ViewBase();
    Blend(5, false, false);
    if (cy - r > top)
        C2D_DrawRectSolid(ox + left, oy + top, 0, right - left, cy - r - top, colour);
    if (cy + r + 1 < bottom)
        C2D_DrawRectSolid(ox + left, oy + cy + r + 1, 0, right - left, bottom - cy - r - 1, colour);
    for (int dy = -r, next; dy <= r; dy = next)
    {
        int h = half[dy < 0 ? -dy : dy], y0 = cy + dy, y1;

        for (next = dy + 1; next <= r && half[next < 0 ? -next : next] == h; ++next) {}
        y1 = cy + next;
        if (y1 <= top || y0 >= bottom) continue;
        if (h < 0)
        {
            C2D_DrawRectSolid(ox + left, oy + y0, 0, right - left, y1 - y0, colour);
            continue;
        }
        if (cx - h > left)
            C2D_DrawRectSolid(ox + left, oy + y0, 0, cx - h - left, y1 - y0, colour);
        if (cx + h < right)
            C2D_DrawRectSolid(ox + cx + h, oy + y0, 0, right - cx - h, y1 - y0, colour);
    }
}

static void Layers(unsigned mask)
{
    unsigned display = Reg(0), mode = display & 7;
    mask &= ~sLayerExclude;
    if (mode > 2) { Error(10, "bitmap display modes are not part of baseline"); return; }
    for (int priority = 3; priority >= 0; --priority)
    {
        /* One depth plane at a time while composing them separately. */
        if (!(sPriorityMask & (SLOT_OBJ(priority) | SLOT_BG(priority)))) continue;
        /* Priority is the depth scale: 3 stays at the screen plane. */
        sLayerShift = (sLayerOrigin + sParallax * (3 - priority)) / sShiftZoom;
        /* The dark of a cave over the field, under its text. */
        if (priority == 0 && (mask & 1) && (sPriorityMask & SLOT_BG(0)) && !sFieldVoxel && FieldLightShown())
            FieldDarkness();
        for (int bg = 3; bg >= 0 && (sPriorityMask & SLOT_BG(priority)); --bg)
        {
            if (!(mask & (1u << bg)) || !(display & (0x100u << bg)) || (Reg(8 + bg * 2) & 3) != (unsigned)priority) continue;
            if ((mode == 1 && bg == 3) || (mode == 2 && bg < 2)) continue;
            /* Only a mosaic that shows: the field sets the bit on every BG
             * (InitOverworldBgs) and leaves REG_MOSAIC at 0. */
            if ((Reg(8 + bg * 2) & 0x40) && (Reg(0x4c) & 0xff)) Error(11, "BG mosaic not supported");
            int clipY0 = sClipY0, clipY1 = sClipY1, viewY = sViewY, drop = StageLayerDrop(bg);

            sViewY += drop;
            sClipY0 -= drop;
            sClipY1 -= drop;
            Blend(bg, mask & 32, false);
            /* Which stage a slow frame is in: the layer walk, the sprites or
             * the GPU. Guessing that from fps alone costs a hardware run. */
            uint64_t start = svcGetSystemTick();
            float shift = sLayerShift;
            uint32_t tilesBefore = sStats.tiles;
            if (sFieldUi && bg == 0 && !sFieldBanner) sLayerShift += CTR_FIELD_UI_SHIFT / sShiftZoom;
            if ((mode == 1 && bg == 2) || mode == 2) DrawAffineBg(bg);
            else if (sBattle && (bg == 0 || (BattleBoxLayers() & (1u << bg)))) DrawBattleTextLayer(bg);
            else if (!DrawLineBg(bg) && !DrawFieldBgTex(bg) && !DrawStageBgTex(bg) && !DrawBandBgTex(bg))
                DrawTextBg(bg);
            sBgDrawn[bg] += sStats.tiles - tilesBefore;
            sLayerShift = shift;
            sViewY = viewY;
            sClipY0 = clipY0;
            sClipY1 = clipY1;
            sBgTicks += svcGetSystemTick() - start;
        }
        if ((mask & 16) && (display & 0x1000) && (sPriorityMask & SLOT_OBJ(priority)))
        {
            uint64_t start = svcGetSystemTick();
            DrawObjects(priority, mask & 32);
            sObjTicks += svcGetSystemTick() - start;
        }
    }
}

/*
 * A window edge is an 8-bit field, so 255 is the largest value the game can
 * write. On a 240x160 screen that already meant "past the edge"; on this
 * viewport it has to keep meaning the edge of the screen, or everything beyond
 * x=255 falls outside every window. The overworld sets WIN0H=0x00FF with
 * WININ showing all layers and WINOUT showing only the empty text layer, which
 * is exactly the black band on the right of the map.
 *
 * A window whose first edge is also saturated is empty on hardware (WIN1H is
 * set to 0xFFFF for that purpose) and stays empty here.
 *
 * A wide unit's WIN_RANGE says what each edge passes 255 by (`over`, left in
 * bits 8-15, right in 0-7; gba/io_reg.h): such an edge is exact, 384 being
 * 384 and not the edge of the screen.
 */
static int WindowEdge(unsigned limits, unsigned over, bool last, unsigned extent)
{
    unsigned first = (limits >> 8) + (over >> 8), end = (limits & 255) + (over & 255);

    if (last)
        return (end == 255 && first < 255) ? (int)extent : (int)Min(end, extent);
    return (int)Min(first, extent);
}

/* What window w's edges pass 255 by, while WINxH still holds the value
 * SetGpuReg wrote with it (gpu_regs.c; the offsets are gba/io_reg.h's). */
static unsigned WindowOver(unsigned w, unsigned across)
{
    if (Reg(0x5a + 2 * w) != across) return 0;
    return Reg(0x56 + 2 * w);
}

/*
 * One axis of a window, in GBA coordinates. A stage keeps the GBA's own
 * reading of the registers (an end past the screen, or before the start, means
 * the screen edge), and a window that reaches an edge of the GBA screen reaches
 * the edge of the stage: that is what the game meant by it, and the letterbox
 * of the intro would otherwise leave its margins outside every window.
 */
/*
 * The battle intro opens its scene through window 0: a band across the whole
 * screen, with nothing shown outside it, growing from the middle line to the
 * full height. That is a curtain over the whole picture, so on the battle
 * screen it keeps the picture's proportions like the stage's letterbox does;
 * any other battle window (a spotlight, a move's band around a battler) marks
 * a place in the scene and stays 1:1.
 */
static bool BattleCurtain(void)
{
    unsigned across = Reg(0x40);

    return sBattle && sZoom == 1.0f && (Reg(0) & 0x2000) && !(Reg(0x4a) & 63)
        && (across >> 8) == 0 && (across & 255) >= 240;
}

static void WindowSpan(unsigned limits, unsigned over, bool vertical, int *first, int *last)
{
    bool gba = sStage || sBattle || sTransitionCompose;
    unsigned extent = gba ? (vertical ? 160u : 240u)
                          : (vertical ? (unsigned)CTR_GAME_HEIGHT : (unsigned)CTR_GAME_WIDTH);

    if (gba) over = 0;
    *first = WindowEdge(limits, over, false, extent);
    *last = WindowEdge(limits, over, true, extent);
    /*
     * A left edge past the right one wraps round the line on the GBA: that is
     * how a window starts left of the screen. The label of the starter on the
     * left (starter_choose.c, its column 0 less 4 pixels) is WIN0H 252..108,
     * which on a centred screen is -4..108, just into the margin.
     */
    if (sCentred && !vertical && *first > *last) *first -= 256;
    if (!gba) return;
    if ((limits & 255) > extent || (limits >> 8) > (limits & 255)) *last = (int)extent;
    /* A transition is composed as the GBA picture, margins added after; a
     * magnified stage is the GBA picture alone (RenderEye), its windows - the
     * cutscenes' black bars, the descent's ray of light - the GBA's own. */
    if (*first >= *last || sTransitionCompose || sStageZoomed) return;
    if (vertical && sBattle && !BattleCurtain())
    {
        if (*first == 0) *first = VIEW_TOP;
        if (*last >= (int)extent) *last = VIEW_BOTTOM;
        return;
    }
    if (vertical)
    {
        /*
         * Vertically the stage keeps the picture's proportions: the intro's
         * cinematic bars close in to 32 lines of 160, and on a 240-line screen
         * that has to be 48 lines of 240, or the scene between them is a thin
         * strip in the middle of a black screen.
         */
        *first = VIEW_TOP + *first * (VIEW_BOTTOM - VIEW_TOP) / (int)extent;
        *last = VIEW_TOP + *last * (VIEW_BOTTOM - VIEW_TOP) / (int)extent;
        return;
    }
    if (*first == 0) *first = VIEW_LEFT;
    if (*last >= (int)extent) *last = VIEW_RIGHT;
}

/*
 * The window edges an HBlank DMA writes line by line (CtrVideo_SetLineWindow):
 * WIN0H, or WIN0H and WIN1H, for each of the picture's 160 lines. The PokéNav
 * lights its chosen option with one, and its condition graph is a polygon
 * drawn as two of them. Only the PokéNav's screens use them here (LineWindows):
 * the frame is composed in bands of lines whose windows are the same, each
 * band partitioned as a frame with fixed windows is.
 */
static struct
{
    unsigned windows;
    uint16_t across[2][160];
} sLineWindows;

/* The band being composed: its lines, and the line-driven windows' edges there. */
static struct
{
    bool on;
    int top, bottom;
    uint16_t across[2];
} sLineBand;

/*
 * A text background in a band of line windows (Compose): the picture's part
 * of the cell from the layer texture, and the margins as a centred screen
 * fills them. A band is a line or two tall, so walking its tiles would cost
 * far more than the quad.
 */
static bool DrawBandBgTex(unsigned bg)
{
    const CentredFill *fill = CentredFillOf(sCentredScreen);

    if (!sLineBand.on || !LayerDrawable(bg)) return false;
    ViewBase();
    DrawLayerRect(bg, 0, 240, 0, 160, 0, 0, 1, 1, false, 0, 0);
    if (CentredLayers(fill) & (1u << bg)) DrawCentredMargins(bg, fill, -64, 64);
    return true;
}

void CtrVideo_SetLineWindow(const uint16_t *values, unsigned lines, bool both)
{
    sLineWindows.windows = values ? (both ? 3u : 1u) : 0u;
    if (!values) return;
    for (unsigned y = 0; y < 160; ++y)
    {
        sLineWindows.across[0][y] = y < lines ? values[both ? y * 2 : y] : 0;
        sLineWindows.across[1][y] = y < lines && both ? values[y * 2 + 1] : 0;
    }
}

static bool LineWindows(void)
{
    return sLineWindows.windows && sCentredScreen == CTR_CENTRED_POKENAV && (Reg(0) & 0x6000);
}

/* Window w's rectangle in this band, empty when x0 >= x1 or y0 >= y1. */
static void WindowRect(unsigned w, int *x0, int *x1, int *y0, int *y1)
{
    unsigned across = Reg(0x40 + 2 * w), over = WindowOver(w, across);

    if (sLineBand.on && (sLineWindows.windows & (1u << w))) across = sLineBand.across[w], over = 0;
    /* The cave's circle is drawn as such (FieldDarkness): window 0 stays whole. */
    if (w == 0 && FieldLightShown()) across = 0x00FF, over = 0;
    WindowSpan(across, over, false, x0, x1);
    WindowSpan(Reg(0x44 + 2 * w), 0, true, y0, y1);
    if (sLineBand.on)
    {
        if (*y0 < sLineBand.top) *y0 = sLineBand.top;
        if (*y1 > sLineBand.bottom) *y1 = sLineBand.bottom;
    }
}

static bool Inside(int px, int py, unsigned w)
{
    int x0, x1, y0, y1;

    WindowRect(w, &x0, &x1, &y0, &y1);
    /* Same reading of an empty window as the partition uses. */
    return x0 < x1 && y0 < y1 && px >= x0 && px < x1 && py >= y0 && py < y1;
}

/*
 * The rectangles lines [top, bottom) split into by the windows, each with the
 * layer mask it shows: WIN0 > WIN1 > outside. At most 5 x 5 of them.
 */
#define WINDOW_RECTS 25
static unsigned WindowPartition(int top, int bottom, int rects[WINDOW_RECTS][4],
                                unsigned masks[WINDOW_RECTS])
{
    unsigned display = Reg(0), count = 0;
    /* Window rectangles are GBA coordinates; the partition covers the whole
     * viewport so the margins keep the "outside" mask. */
    bool gba = sTransitionCompose;
    int xs[6] = {gba ? 0 : VIEW_LEFT, gba ? 240 : VIEW_RIGHT};
    int ys[6] = {top, bottom};
    unsigned nx = 2, ny = 2;
    for (unsigned w = 0; w < 2; ++w)
        if (display & (0x2000u << w))
        {
            int x0, x1, y0, y1;

            WindowRect(w, &x0, &x1, &y0, &y1);
            if (y0 < top) y0 = top;
            if (y1 > bottom) y1 = bottom;
            if (sTransitionCompose)
            {
                if (x0 < xs[0]) x0 = xs[0];
                if (x1 > xs[1]) x1 = xs[1];
            }

            /*
             * A window left enabled with both edges past the screen is how the
             * game turns one off: the overworld runs with WIN1H = 0xFFFF all
             * the time. It covers nothing, so its edges must not cut the
             * frame. Cutting there split every overworld frame in two at
             * x=255 - the end of the GBA's addressable width, which is exactly
             * where the seam showed - and walked every layer twice to do it.
             */
            if (x0 >= x1 || y0 >= y1) continue;
            xs[nx++] = x0;
            xs[nx++] = x1;
            ys[ny++] = y0;
            ys[ny++] = y1;
        }
    for (unsigned i = 0; i < nx; ++i) for (unsigned j = i + 1; j < nx; ++j)
        if (xs[j] < xs[i]) { int t=xs[j]; xs[j]=xs[i]; xs[i]=t; }
    for (unsigned i = 0; i < ny; ++i) for (unsigned j = i + 1; j < ny; ++j)
        if (ys[j] < ys[i]) { int t=ys[j]; ys[j]=ys[i]; ys[i]=t; }
    for (unsigned y = 0; y + 1 < ny; ++y) for (unsigned x = 0; x + 1 < nx; ++x)
    {
        if (xs[x] == xs[x+1] || ys[y] == ys[y+1]) continue;
        /* Outside every window: WINOUT once any window is on, the OBJ
         * window too (a stat change after the bag has only that one). */
        unsigned mask = (display & 0xE000) ? Reg(0x4a) & 63 : 63;
        for (int w = 1; w >= 0; --w)
            if ((display & (0x2000u << w)) && Inside(xs[x], ys[y], (unsigned)w))
                mask = (Reg(0x48) >> (8*w)) & 63;
        rects[count][0] = xs[x];
        rects[count][1] = ys[y];
        rects[count][2] = xs[x+1];
        rects[count][3] = ys[y+1];
        masks[count++] = mask;
    }
    return count;
}

/* Layers a transition's band composes through its windows (TransitionCompose). */
static unsigned sTransitionLayers;

/*
 * The OBJ window (DISPCNT bit 15): where a sprite in window mode (OAM mode 2)
 * has an opaque pixel, and neither window 0 nor 1 is, the layers shown are
 * WINOUT's upper byte instead of its lower. The battle keeps it on throughout
 * with no such sprite, so it changes nothing, until a stat change
 * (StatsChangeAnimation_Step1, src/battle_anim_utility_funcs.c): an invisible
 * copy of the Pokemon in window mode, and BG1 - the rising or falling
 * pattern - shown only through it, blended over the Pokemon.
 *
 * Per pixel is not what the window rectangles (WindowPartition) can cut, so
 * the layers the window adds are composed once a frame, at 1:1, into a small
 * surface of their own over the window's sprites as a mask (ObjWindowCompose),
 * and that surface is drawn where the outside of the windows is, after that
 * rectangle's own layers (ObjWindowDraw). It exists, and the frame pays for
 * it, only while a window sprite is on screen: a stat change, about a second.
 * The layers drawn over the mask go on top of that rectangle - the stat
 * pattern at priority 0 is; a window taking layers away is not reproduced.
 */
#define OBJWIN_SIZE 128
/* The link battle's VS letters, two 64x64 sprites at double size moving
 * apart, are a window wider than that (they were cut off): 256 KB of VRAM
 * then, for the second they last. */
#define OBJWIN_SIZE_LARGE 256
static C3D_Tex sObjWinTex;
static C3D_RenderTarget *sObjWinTarget;
static bool sObjWinWanted, sObjWinFailed;
static uint32_t sObjWinUsedFrame, sObjWinFailFrame;
/* The side the window's box needed last frame, and when a surface that
 * large could not be made. */
static int sObjWinNeed;
static uint32_t sObjWinLargeFailFrame;
static bool sObjWinLargeFailed;
/* This frame's: its box in GBA pixels and the layers it adds. */
static struct { bool ready; int x0, y0, x1, y1; unsigned layers; } sObjWin;
/*
 * The 3D battle's: the window's layers left out of the scenery, to be drawn
 * in the slot of the sprite group under them instead (SlotsCompose). A stat
 * change's pattern is on its Pokemon and blends with it; in the scenery it
 * was a background in front of the sprites that blends, which can only be
 * composed whole - per eye over the voxel world, 15 ms of drawing for the
 * second it lasts, or flat in the 2D battle, the Pokemon losing its depth.
 */
static bool sObjWinSlotted;
/* Drawing it over an eye's picture, at its group's displacement (ObjWindowEye). */
static bool sObjWinEye;
static float sObjWinEyeShift;

/* Outside the frame, like the battle's other surfaces (SlotsPrepare). */
static void ObjWindowRelease(void)
{
    if (sObjWinTarget) { ClearWhenMadeForget(sObjWinTarget); C3D_RenderTargetDelete(sObjWinTarget); }
    if (sObjWinTex.data) C3D_TexDelete(&sObjWinTex);
    sObjWinTarget = NULL;
    memset(&sObjWinTex, 0, sizeof(sObjWinTex));
}

static void ObjWindowPrepare(void)
{
    bool wanted = sObjWinWanted;
    bool large = sObjWinNeed > OBJWIN_SIZE
              && !(sObjWinLargeFailed && sStats.frames - sObjWinLargeFailFrame < LAYER_RETRY_FRAMES);

    sObjWinWanted = false;
    if (!wanted) sObjWinNeed = 0;
    if (wanted) sObjWinUsedFrame = sStats.frames;
    else if (sObjWinTarget && (sTransitionRequested || sVramAsked
                               || sStats.frames - sObjWinUsedFrame > LAYER_IDLE_FRAMES))
        ObjWindowRelease();
    /* Too small for the window now on screen: made again, larger. */
    if (wanted && sObjWinTarget && large && sObjWinTex.width < OBJWIN_SIZE_LARGE)
        ObjWindowRelease();
    if (!wanted || sObjWinTarget || (sObjWinFailed && sStats.frames - sObjWinFailFrame < LAYER_RETRY_FRAMES))
        return;
    /* RGBA8: the mask's count needs more than one bit of alpha. 64 KB of
     * VRAM; a Pokemon's 64x64 fits the half size too. */
    for (unsigned size = large ? OBJWIN_SIZE_LARGE : OBJWIN_SIZE; size >= OBJWIN_SIZE / 2 && !sObjWinTarget;
         size /= 2)
    {
        if (C3D_TexInitVRAM(&sObjWinTex, size, size, GPU_RGBA8)
            && (sObjWinTarget = C3D_RenderTargetCreateFromTex(&sObjWinTex, GPU_TEXFACE_2D, 0, -1)))
        {
            ClearWhenMade(sObjWinTarget);
            break;
        }
        ObjWindowRelease();
    }
    sObjWinFailed = sObjWinTarget == NULL;
    if (large && (!sObjWinTarget || sObjWinTex.width < OBJWIN_SIZE_LARGE))
    {
        sObjWinLargeFailed = true;
        sObjWinLargeFailFrame = sStats.frames;
    }
    else if (large)
        sObjWinLargeFailed = false;
    if (sObjWinTarget)
    {
        /* Drawn at the scene's whole-number zoom: every texel a block. */
        C3D_TexSetFilter(&sObjWinTex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&sObjWinTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    }
    else
        sObjWinFailFrame = sStats.frames;
    CtrLog_Write(sObjWinTarget ? CTR_LOG_VIDEO : CTR_LOG_ERROR, "VIDEO: OBJ window surface %s, %u texels (VRAM free=%lu)",
                 sObjWinTarget ? "made" : "not made", sObjWinTarget ? (unsigned)sObjWinTex.width : 0u,
                 (unsigned long)vramSpaceFree());
}

/* The box of the window's sprites, in GBA pixels; false if there is none. */
static bool ObjWindowBounds(int *x0, int *y0, int *x1, int *y1)
{
    bool any = false;

    for (unsigned i = 0; i < 128; ++i)
    {
        unsigned attr0 = sMemory.oam[i * 4], attr1 = sMemory.oam[i * 4 + 1];
        unsigned shape = attr0 >> 14;
        bool affine = (attr0 & 0x100) != 0, twice = affine && (attr0 & 0x200);
        int x, y, w, h;

        if (((attr0 >> 10) & 3) != 2 || (!affine && (attr0 & 0x200)) || shape == 3) continue;
        w = sObjDims[shape][attr1 >> 14][0] << twice;
        h = sObjDims[shape][attr1 >> 14][1] << twice;
        /* The GBA's reading, as DrawObjects has it for these screens. */
        x = (int)(attr1 & 511);
        y = (int)(attr0 & 255);
        if (x + w > 512) x -= 512;
        if (y + h > 256) y -= 256;
        if (!any || x < *x0) *x0 = x;
        if (!any || y < *y0) *y0 = y;
        if (!any || x + w > *x1) *x1 = x + w;
        if (!any || y + h > *y1) *y1 = y + h;
        any = true;
    }
    return any;
}

/*
 * In the frame, before anything is composed (CtrVideo_Present): the layers
 * the OBJ window adds, over its sprites as a mask, into its surface. Only on
 * the screens that keep the GBA's geometry, where the window is used.
 */
static void ObjWindowCompose(void)
{
    unsigned display = Reg(0), out = Reg(0x4a) & 63, in = (Reg(0x4a) >> 8) & 63;
    /* Not the battle's text box: it is composed apart (DrawBattleTextLayer). */
    unsigned layers = in & ~out & (display >> 8) & (sBattle ? 14u : 15u);
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0, size;

    sObjWin.ready = false;
    if (!sBattle && !sStage) return;
    if ((display & 0x9000) != 0x9000 || (display & 7) > 2 || !layers) return;
    if (!ObjWindowBounds(&x0, &y0, &x1, &y1)) return;
    /* Only what can be on screen: the VS letters end sliding off it as they
     * grow, a box far wider than any surface. */
    if (x0 < VIEW_LEFT) x0 = VIEW_LEFT;
    if (y0 < VIEW_TOP) y0 = VIEW_TOP;
    if (x1 > VIEW_RIGHT) x1 = VIEW_RIGHT;
    if (y1 > VIEW_BOTTOM) y1 = VIEW_BOTTOM;
    if (x0 >= x1 || y0 >= y1) return;
    if (out & 31 & ~in) Error(12, "OBJ window hiding layers not supported");
    sObjWinWanted = true;
    sObjWinNeed = x1 - x0 > y1 - y0 ? x1 - x0 : y1 - y0;
    if (!sObjWinTarget) return;
    size = (int)sObjWinTex.width;
    if (x1 - x0 > size || y1 - y0 > size)
    {
        Error(13, "OBJ window larger than its surface");
        if (x1 - x0 > size) x1 = x0 + size;
        if (y1 - y0 > size) y1 = y0 + size;
    }

    float zoom = sZoom, offX = sOffX, offY = sOffY, origin = sLayerOrigin, parallax = sParallax;
    float shift = sLayerShift, fade = sPaletteFade;
    int viewX = sViewX, viewY = sViewY, targetW = sTargetW, targetH = sTargetH, surfaceH = sSurfaceH;
    unsigned exclude = sLayerExclude, priorities = sPriorityMask;
    bool depth = sSpriteDepth, objMask = sObjMaskOn;

    /* GBA (x0, y0) at the surface's corner, at 1:1, flat. */
    sZoom = 1.0f;
    sOffX = sOffY = 0.0f;
    sViewX = -x0;
    sViewY = -y0;
    sTargetW = x1 - x0;
    sTargetH = y1 - y0;
    sSurfaceH = size;
    sLayerOrigin = sParallax = sLayerShift = 0.0f;
    sPaletteFade = 0.0f;
    sLayerExclude = 0;
    sPriorityMask = SLOTS_ALL;
    sSpriteDepth = sObjMaskOn = false;
    ClipToView();

    ColourClear(sObjWinTarget, 0);
    BlendForget();
    SceneOn(sObjWinTarget);
    C2D_ViewReset();
    sObjWinPass = OBJWIN_MASK;
    for (int priority = 3; priority >= 0; --priority)
        DrawObjects((unsigned)priority, false);
    C2D_Flush();
    BlendForget();
    sObjWinPass = OBJWIN_LAYERS;
    /* Without the effect bit: the blend is the surface's (Blend). */
    Layers(layers);
    C2D_Flush();
    sObjWinPass = OBJWIN_NONE;
    BlendForget();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    /* Rendering to a texture and then sampling it needs a command split. */
    C3D_FrameSplit(0);

    sZoom = zoom;
    sOffX = offX;
    sOffY = offY;
    sViewX = viewX;
    sViewY = viewY;
    sTargetW = targetW;
    sTargetH = targetH;
    sSurfaceH = surfaceH;
    sLayerOrigin = origin;
    sParallax = parallax;
    sLayerShift = shift;
    sPaletteFade = fade;
    sLayerExclude = exclude;
    sPriorityMask = priorities;
    sSpriteDepth = depth;
    sObjMaskOn = objMask;
    ClipToView();

    sObjWin.ready = true;
    sObjWin.x0 = x0;
    sObjWin.y0 = y0;
    sObjWin.x1 = x1;
    sObjWin.y1 = y1;
    sObjWin.layers = layers;
}

/*
 * The OBJ window's surface over [x0, x1) x [y0, y1), a rectangle of the
 * outside of windows 0 and 1, already scissored: drawn as its frontmost layer
 * would be, with that layer's blend and depth.
 */
static void ObjWindowDraw(int x0, int y0, int x1, int y1)
{
    unsigned layers = sObjWin.layers & (sObjWinEye ? ~0u : ~sLayerExclude), bg = 0, priority = 4;
    float shift = sLayerShift;

    if (sObjWinSlotted && !sObjWinEye) return;

    if (x0 < sObjWin.x0) x0 = sObjWin.x0;
    if (y0 < sObjWin.y0) y0 = sObjWin.y0;
    if (x1 > sObjWin.x1) x1 = sObjWin.x1;
    if (y1 > sObjWin.y1) y1 = sObjWin.y1;
    if (x0 >= x1 || y0 >= y1) return;
    for (unsigned b = 0; b < 4; ++b)
        if ((layers & (1u << b)) && (Reg(8 + b * 2) & 3) < priority)
        {
            priority = Reg(8 + b * 2) & 3;
            bg = b;
        }
    if (priority > 3 || !(sPriorityMask & SLOT_BG(priority))) return;

    int w = sObjWin.x1 - sObjWin.x0, h = sObjWin.y1 - sObjWin.y0;
    float size = (float)sObjWinTex.width;
    const Tex3DS_SubTexture cut = {(u16)w, (u16)h, 0.0f, 1.0f, w / size, 1.0f - h / size};

    Blend(bg, ((Reg(0x4a) >> 8) & 32) != 0, false);
    /* Only where the window's mask and a layer's pixel both are. */
    C3D_AlphaTest(true, GPU_GREATER, 0xc0);
    ViewBase();
    /* Over an eye, displaced like the group the window is over (ObjWindowEye). */
    sLayerShift = sObjWinEye ? sObjWinEyeShift : (sLayerOrigin + sParallax * (3 - (int)priority)) / sShiftZoom;
    C2D_DrawImageAt((C2D_Image){&sObjWinTex, &cut}, sObjWin.x0 + CTR_VIEW_X + sLayerShift,
                    sObjWin.y0 + CTR_VIEW_Y, 0, &sTint, 1, 1);
    C2D_Flush();
    BlendForget();
    sLayerShift = shift;
    /* A background in front of the sprites (RenderBattleStereo). */
    if (!sObjWinEye) sBgDrawn[bg] += 1;
}

/*
 * The OBJ window's layers over an eye's picture, after the sprites' groups:
 * at the displacement of the group its box is in, blended with what is
 * there as on the GBA, and at the screen's zoom with the window's own
 * nearest filtering - inside the group's slot they came out smoothed, the
 * slot being composed at twice the size and drawn down bilinearly.
 */
static void ObjWindowEye(C3D_RenderTarget *picture, int centre)
{
    unsigned priorities = sPriorityMask, exclude = sLayerExclude;

    SceneOn(picture);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    sPriorityMask = SLOTS_ALL;
    sLayerExclude = 0;
    sObjWinEye = true;
    sObjWinEyeShift = BattleSpriteShift(centre) / sZoom;
    ObjWindowDraw(sObjWin.x0, sObjWin.y0, sObjWin.x1, sObjWin.y1);
    sObjWinEye = false;
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    sPriorityMask = priorities;
    sLayerExclude = exclude;
}

static void ComposeBand(int top, int bottom)
{
    int rects[WINDOW_RECTS][4];
    unsigned masks[WINDOW_RECTS];
    unsigned count = WindowPartition(top, bottom, rects, masks);
    /* What the pass composes into: the view, or less (RenderBands' box). */
    int clipX0 = sClipX0, clipY0 = sClipY0, clipX1 = sClipX1, clipY1 = sClipY1;

    /* Each rectangle has a uniform mask. GPU scissor clips transformed primitives. */
    for (unsigned i = 0; i < count; ++i)
    {
        unsigned mask = sTransitionCompose ? masks[i] & sTransitionLayers : masks[i];
        int x0 = rects[i][0] > clipX0 ? rects[i][0] : clipX0, x1 = rects[i][2] < clipX1 ? rects[i][2] : clipX1;
        int y0 = rects[i][1] > clipY0 ? rects[i][1] : clipY0, y1 = rects[i][3] < clipY1 ? rects[i][3] : clipY1;

        if (sTransitionCompose && !(mask & 17u)) continue;
        if (x0 >= x1 || y0 >= y1) continue;
        C2D_Flush();
        Scissor(x0, y0, x1, y1);
        sScissored = true;
        sClipX0 = x0; sClipX1 = x1;
        sClipY0 = y0; sClipY1 = y1;
        Layers(mask);
        /* The OBJ window opens only where windows 0 and 1 are not. */
        if (sObjWin.ready && !sTransitionCompose
            && !((Reg(0) & 0x2000) && Inside(x0, y0, 0)) && !((Reg(0) & 0x4000) && Inside(x0, y0, 1)))
            ObjWindowDraw(x0, y0, x1, y1);
    }
    sClipX0 = clipX0; sClipY0 = clipY0;
    sClipX1 = clipX1; sClipY1 = clipY1;
}

static void Compose(void)
{
    unsigned display = Reg(0);
    int top = VIEW_TOP, bottom = VIEW_BOTTOM;
    int first = top > 0 ? top : 0, end = bottom < 160 ? bottom : 160;

    /* No window at all. With only the OBJ window on - a stat change after
     * the bag or party menu, which leave windows 0 and 1 off - outside it is
     * WINOUT's: composed as if there were no windows, the stat pattern
     * covered the whole screen. */
    if (!(display & 0xE000)) { Layers(63); return; }
    if (!LineWindows())
        ComposeBand(top, bottom);
    else
    {
        /* Above and below the picture no line opens a window. */
        sLineBand.on = true;
        sLineBand.across[0] = sLineBand.across[1] = 0;
        sLineBand.top = top;
        sLineBand.bottom = bottom < 0 ? bottom : 0;
        if (top < 0) ComposeBand(top, sLineBand.bottom);
        for (int y = first, next; y < end; y = next)
        {
            sLineBand.across[0] = sLineWindows.across[0][y];
            sLineBand.across[1] = sLineWindows.across[1][y];
            for (next = y + 1; next < end; ++next)
                if (sLineWindows.across[0][next] != sLineBand.across[0]
                 || sLineWindows.across[1][next] != sLineBand.across[1]) break;
            sLineBand.top = y;
            sLineBand.bottom = next;
            ComposeBand(y, next);
        }
        sLineBand.across[0] = sLineBand.across[1] = 0;
        sLineBand.top = top > 160 ? top : 160;
        sLineBand.bottom = bottom;
        if (bottom > 160) ComposeBand(sLineBand.top, bottom);
        sLineBand.on = false;
    }
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    sScissored = false;
    ClipToView();
}

/*
 * A top screen's target as C2D_CreateScreenTarget makes it - RGBA8, sent to
 * the screen as RGB8 - without its 16-bit depth buffer (187.5 KiB of VRAM an
 * eye). Nothing drawn there tests depth: the voxel world is drawn into the
 * logical surface, which has one, and everything that reaches a screen is
 * 2D at depth 0, as on the layer and scene surfaces, which have none.
 */
static bool SceneKept(void);
static bool SceneMake(void);
static void SceneReady(void);

static C3D_RenderTarget *TopScreenTarget(gfx3dSide_t side)
{
    C3D_RenderTarget *target = C3D_RenderTargetCreate(240, gfxIsWide() ? 800 : 400, GPU_RB_RGBA8, -1);

    if (target)
        C3D_RenderTargetSetOutput(target, GFX_TOP, side, GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8));
    return target;
}

bool CtrVideo_Init(void)
{
    const char *step = "C3D_Init command buffer";
    /* 512 KiB, down from 1 MiB: a frame's fullest use is 4% of 1 MiB (the
     * stats line's cmdbuf=, Azahar: the field, the voxel battle with the
     * slider up), so the half given back to the linear heap - full in a
     * battle on hardware - still leaves twelve times the headroom. */
    sC3d = C3D_Init(0x80000);
    if (!sC3d) goto fail;
    step = "C2D_Init geometry buffers";
    sC2d = C2D_Init(MAX_DRAWS);
    if (!sC2d) goto fail;
    step = "tile atlas in linear memory";
    if (!C3D_TexInit(&sAtlas, ATLAS_SIZE, ATLAS_SIZE, GPU_RGBA5551)) goto fail;
    /* Citro3D rejects render-to-texture targets outside VRAM. The atlas is
     * CPU-updated linear memory, but the GPU-written surface MUST be VRAM. */
    step = "logical surface in VRAM";
    if (!C3D_TexInitVRAM(&sSurface, 512, 256, GPU_RGBA8)) goto fail;
    C3D_TexSetFilter(&sAtlas, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetFilter(&sSurface, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&sAtlas, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexSetWrap(&sSurface, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    step = "logical render target";
    sLogical = C3D_RenderTargetCreateFromTex(&sSurface, GPU_TEXFACE_2D, 0, GPU_RB_DEPTH16);
    if (!sLogical) goto fail;
    /* Not fatal: without it the voxel picture has no bloom. */
    if (C3D_TexInitVRAM(&sBloomTex, 128, 64, GPU_RGB565))
    {
        C3D_TexSetFilter(&sBloomTex, GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&sBloomTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        sBloom = C3D_RenderTargetCreateFromTex(&sBloomTex, GPU_TEXFACE_2D, 0, -1);
    }
    step = "top screen render target";
    sTop = TopScreenTarget(GFX_LEFT);
    if (!sTop) goto fail;
    /* The right eye is created once and only rendered while the 3D slider is
     * up; a console without it simply never composes a second pass. */
    sTopRight = TopScreenTarget(GFX_RIGHT);
    if (!sTopRight) CtrLog_Write(CTR_LOG_VIDEO, "no right eye target: 3D disabled");
    {
        bool newModel = false;

        if (R_FAILED(APT_CheckNew3DS(&newModel))) newModel = false;
        /* The voxel world per eye: a New 3DS has the frame for it. */
#ifdef CTR_VOXEL_STEREO
        sVoxelStereo = CTR_VOXEL_STEREO != 0;
#else
        sVoxelStereo = newModel;
#endif
        sOldModel = !newModel;
        CtrLog_Write(CTR_LOG_VIDEO, "voxel 3D %s (%s 3DS)", sVoxelStereo ? "on" : "off",
                     newModel ? "New" : "Old");
    }
    for (unsigned i = 0; i < 64; ++i)
        sMorton[i] = CtrVideo_Texel(i & 7, i / 8, 8);
    sMortonPairs = true;
    for (unsigned i = 0; i < 64; i += 2)
    {
        sMortonPair[i / 2] = sMorton[i] / 2;
        if ((sMorton[i] & 1) || sMorton[i + 1] != sMorton[i] + 1) sMortonPairs = false;
    }
    for (unsigned i = 0; i < sizeof(sPalette) / sizeof(sPalette[0]); ++i)
        sTexturePalette[i] = CtrVideo_RGBA5551(sPalette[i]);
    C2D_Prepare();
    Flat2D();
    FastInit();
    LeavesLoad();
    /* The intro's margins are made now, before the voxel world takes its
     * share of linear memory: made on the scene's first frame they could
     * find none left. Given back once the scene is over. */
    for (unsigned bg = 0; sLeaves && bg < 4; ++bg)
        LeavesAlloc(bg);
    /* Citro3D and Citro2D are up; the voxel module only adds its own shader,
     * textures and buffers on top of them. */
    step = "voxel overworld renderer";
    if (!CtrVoxel_Init()) goto fail;
    /* Next to the voxel world's arenas, before any screen cuts the VRAM up
     * (SceneKept). */
    if (SceneKept() && SceneMake())
        SceneReady();
    sFpsStart = CtrPlatform_Milliseconds();
    CtrLog_Write(CTR_LOG_VIDEO, "GPU: RGBA5551 atlas, 8 MiB linear heap, native 400x240 viewport");
    return true;
fail:
    CtrLog_Write(CTR_LOG_ERROR, "VIDEO init failed: %s (linear free=%lu, VRAM free=%lu)",
                 step, (unsigned long)linearSpaceFree(), (unsigned long)vramSpaceFree());
    CtrVideo_Shutdown();
    return false;
}

void CtrVideo_Bind(CtrVideoMemory memory)
{
    sMemory = memory;
    memset(sHash, 0, sizeof(sHash));
    sUsed = 0;
}

/*
 * Composes the logical frame with this eye's parallax and blits it to one
 * screen buffer. Render-to-texture followed by sampling that texture needs a
 * GPU command split, and so does composing the surface again for the other
 * eye, so each pass ends with one.
 */
/*
 * Outside the frame: the battle scene's surface while the battle is up, given
 * back a few seconds after. Without VRAM for it the scene is composed at 1.5
 * directly, and the allocation is tried again later.
 */
static bool sSceneCached;
/* A battle without its scene surface has no 3D: tried again soon. */
#define SCENE_RETRY_FRAMES 60u

static void SceneRelease(void)
{
    sSceneCached = false;
    if (sScene) { ClearWhenMadeForget(sScene); C3D_RenderTargetDelete(sScene); }
    if (sSceneTex.data) C3D_TexDelete(&sSceneTex);
    sScene = NULL;
    memset(&sSceneTex, 0, sizeof(sSceneTex));
}

static bool SceneMake(void)
{
    if (C3D_TexInitVRAM(&sSceneTex, SCENE_W, SCENE_H, GPU_RGBA5551)
        && (sScene = C3D_RenderTargetCreateFromTex(&sSceneTex, GPU_TEXFACE_2D, 0, -1)))
    {
        ClearWhenMade(sScene);
        return true;
    }
    SceneRelease();
    return false;
}

static void SceneReady(void)
{
    CtrLog_Write(CTR_LOG_VIDEO, "battle scene surface ready: filtered zoom (VRAM free=%lu)",
                 (unsigned long)vramSpaceFree());
    sSceneFailed = false;
    C3D_TexSetFilter(&sSceneTex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&sSceneTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
}

/*
 * With the battle over the world, the scene surface is kept from the field
 * on, made while the VRAM is still in one piece. Made at a battle's start, it
 * needed 512 KiB in one block when the field's atlases had cut the VRAM into
 * smaller ones: atlases on screen went for it under the transition, blocks
 * missing there, and the battle's own layer at its start found no block
 * either - the atlases went again, one no longer fitted back, and the battle
 * stood on its classic background for over a second (port.log 2026-10-22:
 * 460 KiB free, the largest block 205 KiB, 76 frames drawn flat).
 */
static bool SceneKept(void)
{
    return CtrSettings_Voxel() && CtrSettings_VoxelBattle() && CtrVoxel_HoldsVram();
}

static void ScenePrepare(void)
{
    if (!sBattle && !sTransition)
    {
        if (SceneKept())
        {
            /* A map on screen that found no VRAM for its atlas comes first:
             * the layers and surfaces no screen draws go, and if there were
             * none, the scene surface, made again once the room is free. */
            if (sScene && sVramAsked)
            {
                bool freed = ReleaseIdleLayers();

                if (!(ReleaseIdleSurfaces() || freed))
                {
                    SceneRelease();
                    sSceneFailFrame = sStats.frames;
                }
            }
            /* From free VRAM and the atlases out of view only, nothing on
             * screen giving way for it - the option switched on in a game,
             * the field's atlases already had the VRAM (port.log
             * 2026-10-22: settings battle=0 at start-up, the surface made at
             * the first battle instead); the largest block looked for twice a
             * second, not every frame. */
            else if (!sScene && sStats.frames - sSceneFailFrame >= 30u)
            {
                /* At most every ten seconds: freed for nothing - the VRAM
                 * in pieces still - the ring's atlases were made again. */
                static uint32_t sSceneFreedFrame;

                if (CtrGpuMem_LargestVram() < SCENE_W * SCENE_H * 2u
                    && (sSceneFreedFrame == 0 || sStats.frames - sSceneFreedFrame >= 600u))
                {
                    sSceneFreedFrame = sStats.frames | 1u;
                    CtrVoxel_ReleaseIdleVramFor(SCENE_W * SCENE_H * 2u, false);
                }
                if (CtrGpuMem_LargestVram() >= SCENE_W * SCENE_H * 2u && SceneMake())
                    SceneReady();
                else
                    sSceneFailFrame = sStats.frames;
            }
            return;
        }
        /* Kept for a battle that follows another, but not over the voxel
         * field: its atlases want the room back, and an atlas that found
         * none drew the field in 2D for a frame and asked for it then. */
        if (sScene && (sVramAsked || sFieldVoxel || sStats.frames - sSceneUsedFrame > LAYER_IDLE_FRAMES))
            SceneRelease();
        return;
    }
    sSceneUsedFrame = sStats.frames;
    if (sScene || (sSceneFailed && sStats.frames - sSceneFailFrame < SCENE_RETRY_FRAMES)) return;
    for (unsigned attempt = 0; attempt < 3 && !sScene; ++attempt)
    {
        bool freed = false;

        if (SceneMake())
            break;
        /*
         * Back from the bag or the party (a centred screen), what it held is
         * not idle yet: its layer textures, its depth planes, the bottom
         * surface. Kept, they left no 512 KiB gap, the battle had no scene
         * surface and so no 3D until the retry - the turn was over by then
         * (port.log, 2026-10-15: 187 frames with the slider up, none in 3D).
         * Then the overworld's atlases of other maps, as for the depth planes.
         */
        if (attempt == 0)
        {
            freed = ReleaseIdleLayers();
            freed = ReleaseIdleSurfaces() || freed;
        }
        if (!freed && attempt < 2)
            freed = CtrVoxel_ReleaseIdleVramFor(SCENE_W * SCENE_H * 2u, true) > 0;
        if (!freed)
            break;
    }
    if (!sScene)
    {
        if (!sSceneFailed)
            CtrLog_Write(CTR_LOG_ERROR, "VIDEO: no VRAM for the battle scene surface (free=%lu); "
                         "unfiltered zoom", (unsigned long)vramSpaceFree());
        sSceneFailed = true;
        sSceneFailFrame = sStats.frames;
        return;
    }
    SceneReady();
}

static void GpuSplit(void);

/*
 * The battle with the 3D slider up (RenderBattleStereo). The scenery - the
 * terrain, the sky, a move's background - is one picture behind the screen,
 * BATTLE_BACK_DEPTH in each eye, the same all over. The sprites stand in
 * front of it at a depth from where they are: the opponent and its info box
 * at the top of the scene BATTLE_SPRITE_DEPTH behind the screen, the player's
 * Pokemon and its info box at the bottom on the screen itself, and a move's
 * sprites in between as they cross (BattleSpriteShift). The text box, and any
 * window over the scene, is the screen too, never displaced: its caps and
 * middle (DrawBattleText) stay joined.
 *
 * The scenery is composed once into the scene surface without the sprites,
 * and the sprites per eye into the columns of that same surface the scenery
 * leaves unused (it is 1024 texels wide, the scenery takes about 590): no
 * VRAM of their own, which beside the voxel battle's world there is not; the
 * text box once into the logical surface. Composing the whole
 * scene per eye was 40 fps on an Old 3DS. What that split cannot keep - a
 * sprite blending with what is under it, a background in front of a sprite,
 * sprites spread wider than their surface - is composed whole, for as long
 * as it lasts: once, at the scenery's depth, in the 2D battle; per eye, with
 * the same depths, over the voxel world. The scene is composed
 * BATTLE_STEREO_MARGIN GBA pixels wider on each side so that its displaced
 * edge still has picture under it. Depths in screen pixels per eye at the
 * widest slider setting.
 */
#ifdef CTR_STEREO_ZERO
#define BATTLE_BACK_DEPTH 0.0f
#define BATTLE_SPRITE_DEPTH 0.0f
#else
#define BATTLE_BACK_DEPTH 4.0f
#define BATTLE_SPRITE_DEPTH 2.0f
#endif
/* GBA lines: a sprite centred above the first is at BATTLE_SPRITE_DEPTH, one
 * centred below the second at the screen. */
#define BATTLE_SPRITE_FAR 44
#define BATTLE_SPRITE_NEAR 80
#define BATTLE_STEREO_MARGIN 3
/* Where the scene surface goes on the logical one (BattleScenePlace). */
static struct { float ox, oy, scale; int left, top, width, height; } sScenePut;
/* The scene is composed for both eyes. */
static bool sSceneComposed;
/* The eye being composed: +slider for the left, -slider for the right. */
static float sEyeParallax;


/* How near a sprite centred on GBA line `centre` stands: 0 at
 * BATTLE_SPRITE_DEPTH, 1 on the screen. */
static float BattleSpriteNear(int centre)
{
    float near = (centre - BATTLE_SPRITE_FAR) / (float)(BATTLE_SPRITE_NEAR - BATTLE_SPRITE_FAR);

    return near < 0 ? 0 : near > 1 ? 1 : near;
}

/* A sprite centred on GBA line `centre`, in screen pixels for this eye. */
/* This eye's displacement of the scenery, put behind the screen. */
static float BattleSceneryShift(void)
{
    return -sEyeParallax * BATTLE_BACK_DEPTH;
}

static float BattleSpriteShift(int centre)
{
    return -sEyeParallax * BATTLE_SPRITE_DEPTH * (1 - BattleSpriteNear(centre));
}

/*
 * Composes the layers not in `exclude` (Layers mask bits) into `target`, a
 * SCENE_ZOOM surface whose top-left texel is GBA (left, top), width x height
 * texels of it used.
 */
static void SceneComposeInto(C3D_RenderTarget *target, int column, int left, int top, int width,
                             int height, unsigned exclude)
{
    float zoom = sZoom, offX = sOffX, offY = sOffY;

    sZoom = SCENE_ZOOM;
    sOffX = column - left * SCENE_ZOOM;
    sOffY = -top * SCENE_ZOOM;
    sTargetW = column + width;
    sTargetH = height;
    sSurfaceH = SCENE_H;
    ClipToView();
    BlendForget();
    /* Cleared transparent: the backdrop is the logical surface's own clear,
     * which is RGBA8. A colour cleared into this 5551 surface comes out
     * wrong - black was a bright blue. From a column on, only that part is
     * cleared, by drawing it transparent: the rest is the scenery. */
    if (column == 0)
    {
        C2D_TargetClear(target, 0);
        SceneOn(target);
    }
    else
    {
        SceneOn(target);
        C2D_ViewReset();
        C2D_Flush();
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        C2D_DrawRectSolid(column, 0, 0, width, height, 0);
        C2D_Flush();
        BlendForget();
    }
    Blend(5, false, false);
    sLayerExclude = exclude;
    memset(sBgDrawn, 0, sizeof(sBgDrawn));
    if (!(Reg(0) & 128)) Compose();
    sLayerExclude = 0;
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    /* Seal the world list; FrameEnd owns queue submission and completion. */
    GpuSplit();

    sZoom = zoom;
    sOffX = offX;
    sOffY = offY;
    sTargetW = CTR_GAME_WIDTH;
    sTargetH = CTR_GAME_HEIGHT;
    sSurfaceH = 256;
    ClipToView();
    BlendForget();
}

/*
 * The scenery of the 3D battle, kept in the scene surface from one frame to
 * the next while nothing it is made of changes: most of a battle the terrain
 * stands still, and composing it was 1-3 ms of CPU a frame on an Old 3DS. It
 * is made of the registers (and the line-by-line scroll), the BG palettes
 * and the VRAM of the layers it draws; anything else that draws into the
 * scene surface's scenery columns drops it (sSceneCached). Only the stereo
 * battle's scenery pass asks for it (sSceneCacheAllowed); a scene with the
 * OBJ window on is never kept, as the sprites shape it.
 */
static bool sSceneCacheAllowed;
static uint32_t sSceneCacheKey, sSceneCacheToken;
static unsigned sSceneCacheDrawn[4];
/*
 * A scene that came out empty: no background layer drew anything in it (the
 * battle over the voxel world, whose world stands in for the scenery). Its
 * surface is then lent to the sprite slots and the cache above is dropped,
 * so every frame walked those empty layers at 2x again: 1.1 ms in Azahar of
 * the 3D voxel battle's frame. Made of the same, unchanged VRAM, it is empty
 * again and is not composed.
 */
static bool sSceneEmpty;
static uint32_t sSceneEmptyKey, sSceneEmptyToken;
static unsigned sSceneEmptyReused;
static unsigned sSceneReused;
/* The scene kept this frame (BattleSceneCompose): its edges may be too. */
static bool sSceneKeptNow;
/*
 * A background blending over the rest of the 2D battle's scenery (Sandstorm's
 * sand, BG1 at EVA 1-7 over all): the game steps BLDALPHA every four frames
 * while it fades in and out, and each step composed the whole scenery again -
 * 6.4 ms of an Old 3DS's frame, a dropped frame every four (port.log
 * 2026-10-28). Composed apart instead, that layer alone into a surface of its
 * own (sBlendLayerTarget, raw, its effects off) and the rest into the scene
 * surface, both kept while only the blend's weights change; the layer is then
 * blended over the scenery as the scenery is put on the screen
 * (BattleScenePlace), with Blend's own state for it.
 */
static C3D_Tex sBlendLayerTex;
static C3D_RenderTarget *sBlendLayerTarget;
static bool sBlendLayerWanted, sBlendLayerCached;
static uint32_t sBlendLayerKey, sBlendLayerToken, sBlendLayerFailFrame, sBlendLayerUsedFrame;
static unsigned sBlendLayerAlpha;
/* The layer blended at BattleScenePlace this frame, or -1. */
static int sSceneBlendLayer = -1;
/* SceneCacheKey without BLDALPHA, for the two parts above. */
static bool sSceneKeyNoAlpha;
static uint32_t SceneCacheMix(uint32_t h, uint32_t v)
{
    h ^= v;
    h *= 0x01000193u;
    return h ^ (h >> 15);
}

static void SceneLayerVram(unsigned bg, unsigned *map, unsigned *mapBytes, unsigned *chars, unsigned *charBytes);

/*
 * A layer whose whole map names only clear tiles: whatever its scroll, it
 * draws nothing. Looked at again only when its map, or the characters the
 * map names, change. The battle's BG1 is such a layer between the moves, and
 * its scroll changed on every frame.
 */
static struct { uint32_t token; unsigned control, mode, start, bytes; bool blank; } sLayerBlank[4];

static bool SceneLayerBlank(unsigned bg)
{
    unsigned control = Reg(8 + bg * 2), mode = Reg(0) & 7, map, mapBytes, chars, charBytes;
    bool affine = (mode == 1 && bg == 2) || (mode == 2 && bg >= 2);
    unsigned tileBytes = affine || (control & 0x80) ? 64 : 32, lo = ~0u, hi = 0;
    static uint32_t seen[1024 / 32];
    bool blank = true;

    if (mode > 2) return false;
    SceneLayerVram(bg, &map, &mapBytes, &chars, &charBytes);
    if (map + mapBytes > 0x10000) mapBytes = 0x10000 - map;
    if (sLayerBlank[bg].token && sLayerBlank[bg].control == control && sLayerBlank[bg].mode == mode
        && !VramChangedAfter(map, mapBytes, sLayerBlank[bg].token)
        && !VramChangedAfter(sLayerBlank[bg].start, sLayerBlank[bg].bytes, sLayerBlank[bg].token))
        return sLayerBlank[bg].blank;
    memset(seen, 0, sizeof(seen));
    for (unsigned i = 0; i < mapBytes && blank; i += affine ? 1 : 2)
    {
        unsigned tile = affine ? sMemory.vram[map + i] : (sMemory.vram[map + i] | sMemory.vram[map + i + 1] << 8) & 0x3ff;
        unsigned at = chars + tile * tileBytes;

        if (seen[tile >> 5] & (1u << (tile & 31))) continue;
        seen[tile >> 5] |= 1u << (tile & 31);
        if (tile < lo) lo = tile;
        if (tile > hi) hi = tile;
        if (at + tileBytes > 0x10000) { blank = false; break; }
        for (unsigned k = 0; k < tileBytes; k += 4)
            if (sMemory.vram[at + k] | sMemory.vram[at + k + 1] | sMemory.vram[at + k + 2] | sMemory.vram[at + k + 3])
            {
                blank = false;
                break;
            }
    }
    VramSyncRange(map, mapBytes);
    sLayerBlank[bg].token = sStats.frames + 1;
    sLayerBlank[bg].control = control;
    sLayerBlank[bg].mode = mode;
    sLayerBlank[bg].blank = blank;
    sLayerBlank[bg].start = lo > hi ? 0 : chars + lo * tileBytes;
    sLayerBlank[bg].bytes = lo > hi ? 0 : (hi - lo + 1) * tileBytes;
    if (sLayerBlank[bg].start + sLayerBlank[bg].bytes > 0x10000)
        sLayerBlank[bg].bytes = 0x10000 - sLayerBlank[bg].start;
    VramSyncRange(sLayerBlank[bg].start, sLayerBlank[bg].bytes);
    return blank;
}

static uint32_t SceneCacheKey(int left, int top, int width, int height, unsigned exclude)
{
    uint32_t h = 0x811c9dc5u;
    union { float f; uint32_t u; } zoom = {sZoom}, offX = {sOffX}, offY = {sOffY};
    union { float f; uint32_t u; } fade = {sPaletteFade}, origin = {sLayerOrigin};

    h = SceneCacheMix(h, (uint32_t)left);
    h = SceneCacheMix(h, (uint32_t)sSceneColumn);
    h = SceneCacheMix(h, (uint32_t)top);
    h = SceneCacheMix(h, (uint32_t)width);
    h = SceneCacheMix(h, (uint32_t)height);
    h = SceneCacheMix(h, exclude);
    h = SceneCacheMix(h, zoom.u);
    h = SceneCacheMix(h, offX.u);
    h = SceneCacheMix(h, offY.u);
    h = SceneCacheMix(h, (uint32_t)sViewX);
    h = SceneCacheMix(h, (uint32_t)sViewY);
    h = SceneCacheMix(h, origin.u);
    /* A palette fade is drawn as a tint (Blend), not in the palette. */
    h = SceneCacheMix(h, fade.u);
    h = SceneCacheMix(h, sPaletteFadeColor);
    /* DISPCNT, the BG registers, windows, mosaic and blending: 0x00-0x55;
     * the wide windows' edges past 255 beside them, 0x56-0x5D. Not the
     * scroll of a layer the scene does not draw: the text box's BG1 scrolls
     * on through the battle, and the scene was composed again every frame
     * for it - 4-5 ms of an Old 3DS's 2D battle frame in 3D. */
    unsigned drawn = (Reg(0) >> 8) & 15 & ~exclude;

    h = SceneCacheMix(h, Reg(0));
    for (unsigned offset = 0x08; offset < 0x5e; offset += 2)
    {
        /* BGnHOFS/VOFS at 0x10 + 4n, BG2's and BG3's affine sets at 0x20 and 0x30. */
        unsigned bg = offset >= 0x10 && offset < 0x20 ? (offset - 0x10) / 4
                    : offset >= 0x20 && offset < 0x40 ? 2 + (offset - 0x20) / 16 : 4;

        if (bg < 4 && !(drawn & (1u << bg))) continue;
        if (bg < 4 && SceneLayerBlank(bg)) continue;
        if (offset == 0x52 && sSceneKeyNoAlpha) continue;
        h = SceneCacheMix(h, Reg(offset));
    }
    h = SceneCacheMix(h, sLineCount);
    for (unsigned r = 0; r < 8; ++r)
        if ((sLineMask & (1u << r)) && (drawn & (1u << (r / 2))) && !SceneLayerBlank(r / 2))
        {
            h = SceneCacheMix(h, r);
            for (unsigned y = 0; y < sLineCount && y < LINE_MAX; y += 2)
                h = SceneCacheMix(h, sLineScroll[r][y] | (y + 1 < LINE_MAX ? (uint32_t)sLineScroll[r][y + 1] << 16 : 0));
        }
    return h;
}

/* The VRAM a layer's picture is made of: its map and its characters. */
static void SceneLayerVram(unsigned bg, unsigned *map, unsigned *mapBytes, unsigned *chars, unsigned *charBytes)
{
    unsigned control = Reg(8 + bg * 2), size = control >> 14, mode = Reg(0) & 7;
    bool affine = (mode == 1 && bg == 2) || (mode == 2 && bg >= 2);

    *map = ((control >> 8) & 31) * 0x800;
    *chars = ((control >> 2) & 3) * 0x4000;
    *mapBytes = affine ? (16u << size) * (16u << size) : 0x800u * (size == 0 ? 1 : size == 3 ? 4 : 2);
    *charBytes = 0x10000 - *chars;
}

/*
 * The characters a scene's layers can show, kept or empty: from the lowest to
 * the highest tile their whole maps name (SceneEmptyChars), not all of VRAM
 * from their base. The voxel battle's empty layers share their character block
 * with the text box, whose printing changed it nearly every frame: the empty
 * scene was composed again in two frames of three.
 */
static struct { unsigned start, bytes, control, mode; uint32_t token; } sSceneEmptyChars[4];

static void SceneEmptyChars(unsigned exclude)
{
    for (unsigned bg = 0; bg < 4; ++bg)
    {
        unsigned map, mapBytes, chars, charBytes, control = Reg(8 + bg * 2), mode = Reg(0) & 7;
        bool affine = (mode == 1 && bg == 2) || (mode == 2 && bg >= 2);
        unsigned tileBytes = affine || (control & 0x80) ? 64 : 32, lo = ~0u, hi = 0;

        if (!(Reg(0) & (0x100u << bg)) || (exclude & (1u << bg)))
        {
            sSceneEmptyChars[bg].token = 0;
            continue;
        }
        SceneLayerVram(bg, &map, &mapBytes, &chars, &charBytes);
        if (map + mapBytes > 0x10000)
            mapBytes = 0x10000 - map;
        /* The same map as at the last look: the same tiles named. */
        if (sSceneEmptyChars[bg].token && sSceneEmptyChars[bg].control == control
            && sSceneEmptyChars[bg].mode == mode && !VramChangedAfter(map, mapBytes, sSceneEmptyChars[bg].token))
            continue;
        sSceneEmptyChars[bg].start = sSceneEmptyChars[bg].bytes = 0;
        sSceneEmptyChars[bg].control = control;
        sSceneEmptyChars[bg].mode = mode;
        sSceneEmptyChars[bg].token = sStats.frames + 1;
        if (affine)
            for (unsigned i = 0; i < mapBytes; ++i)
            {
                unsigned tile = sMemory.vram[map + i];

                if (tile < lo) lo = tile;
                if (tile > hi) hi = tile;
            }
        else
            for (unsigned i = 0; i + 1 < mapBytes; i += 2)
            {
                unsigned tile = (sMemory.vram[map + i] | sMemory.vram[map + i + 1] << 8) & 0x3ff;

                if (tile < lo) lo = tile;
                if (tile > hi) hi = tile;
            }
        if (lo > hi || chars + lo * tileBytes >= 0x10000)
            continue;
        sSceneEmptyChars[bg].start = chars + lo * tileBytes;
        sSceneEmptyChars[bg].bytes = (hi - lo + 1) * tileBytes;
        if (sSceneEmptyChars[bg].start + sSceneEmptyChars[bg].bytes > 0x10000)
            sSceneEmptyChars[bg].bytes = 0x10000 - sSceneEmptyChars[bg].start;
    }
}

/* The empty scene's layers unchanged since `token`: their maps, and the
 * characters those name. Whether a layer draws anything does not depend on
 * its palette, which is not looked at. */
static bool SceneHoldsSince(unsigned exclude, uint32_t token)
{
    /* A bitmap mode's picture is not reached through map entries; and with
     * sprites in the OBJ window, those shape it. Not DISPCNT's OBJ window bit
     * alone: the battle keeps it on throughout (0xbf40) with no such sprite,
     * and no battle scene was ever kept. */
    if ((Reg(0) & 7) > 2 || sObjWin.ready)
        return false;
    for (unsigned bg = 0; bg < 4; ++bg)
    {
        unsigned map, mapBytes, chars, charBytes;

        if (!(Reg(0) & (0x100u << bg)) || (exclude & (1u << bg)))
            continue;
        SceneLayerVram(bg, &map, &mapBytes, &chars, &charBytes);
        if (VramChangedAfter(map, mapBytes, token)
            || (sSceneEmptyChars[bg].bytes
                && VramChangedAfter(sSceneEmptyChars[bg].start, sSceneEmptyChars[bg].bytes, token)))
            return false;
    }
    return true;
}

/*
 * The kept scene still holds: the palettes, and the layers as SceneHoldsSince
 * looks at them. A layer's picture is its map and the characters that map
 * names, so with the map unchanged only those characters can change it; all
 * of VRAM from the character base was 48 blocks compared every battle frame.
 */
static bool SceneCacheHolds(unsigned exclude)
{
    if (sBgPaletteStamp > sSceneCacheToken || sTilemapBankStamp > sSceneCacheToken)
        return false;
    return SceneHoldsSince(exclude, sSceneCacheToken);
}

/* What the scenery was made of, compared now: so a change and a change back
 * before the next look is still a change (see SyncBlock). */
static void SceneCacheSync(unsigned exclude)
{
    for (unsigned bg = 0; bg < 4; ++bg)
    {
        unsigned map, mapBytes, chars, charBytes;

        if (!(Reg(0) & (0x100u << bg)) || (exclude & (1u << bg)))
            continue;
        SceneLayerVram(bg, &map, &mapBytes, &chars, &charBytes);
        VramSyncRange(map, mapBytes);
        VramSyncRange(chars, charBytes);
    }
}

/*
 * The battle scene - everything but the text box - at SCENE_ZOOM into its own
 * surface, then that surface on the logical one at CTR_BATTLE_ZOOM. The scene
 * surface starts at the GBA pixel on the screen's top-left corner, so its
 * origin lands on screen (ox, oy), at most a screen pixel off the edge.
 */
#ifdef CTR_FRAME_DUMP
/* Audit builds: nocache.txt on the card turns the kept scene, its edges and
 * the whole frames composed at once off, for frames to compare against. */
static unsigned CtrDumpOff(void)
{
    static int read;
    static unsigned off;

    if (!read)
    {
        FILE *file = fopen("sdmc:/3ds/emerald3ds/nocache.txt", "r");

        read = 1;
        if (file)
        {
            if (fscanf(file, "%u", &off) != 1) off = 15;
            fclose(file);
        }
    }
    return off;
}
#define CtrDumpNoCache() (CtrDumpOff() & 1)
#endif

/*
 * The background composed apart from the scenery (sBlendLayerTex), or -1: the
 * only first target of an alpha blend, in front of every other layer of the
 * scenery, over all of them and the backdrop, with no window or fade between.
 * Blended over the scenery afterwards it then comes out as composed with it.
 */
static int SceneBlendLayerOf(unsigned layers)
{
    unsigned control = Reg(0x50), display = Reg(0), drawn = (display >> 8) & 15 & ~layers, blend = control & 63;
    int layer = -1;

#ifdef CTR_FRAME_DUMP
    if (CtrDumpOff() & 32) return -1;
#endif
    if (!sBattle || sBattleWorld || !(layers & 16) || ((control >> 6) & 3) != 1 || (display & 7)
        || sObjWin.ready || sObjWinSlotted || sPaletteFade > 0.0f)
        return -1;
    for (unsigned bg = 1; bg < 4; ++bg)
        if (blend == (1u << bg)) layer = (int)bg;
    if (layer < 0 || !(drawn & (1u << layer))) return -1;
    /* Over everything else there: the second target is all of it. */
    if ((((control >> 8) & 63) | (1u << layer) | 16) != 63) return -1;
    for (unsigned bg = 1; bg < 4; ++bg)
    {
        unsigned mine = Reg(8 + layer * 2) & 3, theirs = Reg(8 + bg * 2) & 3;

        /* One with nothing in it draws nothing in front. */
        if ((int)bg == layer || !(drawn & (1u << bg)) || SceneLayerBlank(bg)) continue;
        if (theirs < mine || (theirs == mine && (int)bg < layer)) return -1;
    }
    /* Every window shows every layer with its effects. */
    if ((display & 0x2000) && (Reg(0x48) & 0x3f) != 0x3f) return -1;
    if ((display & 0x4000) && ((Reg(0x48) >> 8) & 0x3f) != 0x3f) return -1;
    if ((display & 0xe000) && (Reg(0x4a) & 0x3f) != 0x3f) return -1;
    if ((display & 0x8000) && ((Reg(0x4a) >> 8) & 0x3f) != 0x3f) return -1;
    return layer;
}

/* The scenery without the blending layer into the scene surface and that
 * layer alone into its own, each composed again only when it changed. False
 * while there is no surface for the layer (asked for, SceneBlendPrepare). */
static bool BattleSceneComposeApart(int left, int top, int width, int height, unsigned layers, unsigned blend)
{
    unsigned rest = layers | (1u << blend), alone = 63u & ~(1u << blend);
    uint32_t restKey, aloneKey;
    unsigned drawn[4];
    bool restHeld, aloneHeld;

    sBlendLayerWanted = true;
    sBlendLayerUsedFrame = sStats.frames;
    if (!sBlendLayerTarget) return false;
    sSceneKeyNoAlpha = true;
    restKey = SceneCacheKey(left, top, width, height, rest) ^ 0x1b873593u;
    aloneKey = SceneCacheKey(left, top, width, height, alone) ^ 0x2545f491u;
    sSceneKeyNoAlpha = false;
    restHeld = sSceneCached && restKey == sSceneCacheKey && SceneCacheHolds(rest);
    aloneHeld = sBlendLayerCached && aloneKey == sBlendLayerKey && sBgPaletteStamp <= sBlendLayerToken
             && sTilemapBankStamp <= sBlendLayerToken && SceneHoldsSince(alone, sBlendLayerToken);
    if (restHeld)
        ++sSceneReused;
    else
    {
        unsigned layerDrawn = sSceneCacheDrawn[blend];

        SceneComposeInto(sScene, sSceneColumn, left, top, width, height, rest);
        SceneCacheSync(rest);
        memcpy(sSceneCacheDrawn, sBgDrawn, sizeof(sBgDrawn));
        sSceneCacheDrawn[blend] = layerDrawn;
        sSceneCacheKey = restKey;
        sSceneCacheToken = sStats.frames + 1;
        sSceneCached = true;
        sSceneEmpty = false;
    }
    if (!aloneHeld)
    {
        SceneComposeInto(sBlendLayerTarget, 0, left, top, width, height, alone);
        SceneCacheSync(alone);
        sSceneCacheDrawn[blend] = sBgDrawn[blend];
        sBlendLayerCached = true;
        sBlendLayerKey = aloneKey;
        sBlendLayerToken = sStats.frames + 1;
    }
    memcpy(drawn, sSceneCacheDrawn, sizeof(drawn));
    memcpy(sBgDrawn, drawn, sizeof(sBgDrawn));
    if (!restHeld || !aloneHeld) SceneEmptyChars(layers);
    /* Kept whole only with the weights as they were: the edges have the
     * layer blended in them (BattleEdgesKeep). */
    sSceneKeptNow = restHeld && aloneHeld && sBlendLayerAlpha == Reg(0x52);
    sBlendLayerAlpha = Reg(0x52);
    sSceneBlendLayer = (int)blend;
    return true;
}

static void BattleSceneCompose(int margin, unsigned exclude)
{
    float zoom = sZoom, offX = sOffX, offY = sOffY;

    sSceneBlendLayer = -1;
    /*
     * The surface holds the lowest SCENE_H / SCENE_ZOOM lines of the scene.
     * The dozen lines the screen shows above them, and those round the text
     * box below, are composed at the screen's scale apart
     * (BattleEdgesCompose) rather than doubling the surface for them.
     */
    int left = VIEW_LEFT - margin, top = VIEW_TOP, lowest = BATTLE_SCENE_BOTTOM - (int)(SCENE_H / SCENE_ZOOM);
    float ox, oy;

    if (top < lowest) top = lowest;
    ox = left * zoom + offX;
    oy = top * zoom + offY;
    float scale = zoom / SCENE_ZOOM;
    int width = (int)ceilf((CTR_GAME_WIDTH + margin * zoom - ox) / scale);
    int height = (int)ceilf((BATTLE_SCENE_BOTTOM * zoom + offY - oy) / scale);

    if (width > SCENE_W) width = SCENE_W;
    if (height > SCENE_H) height = SCENE_H;
    sScenePut.ox = ox;
    sScenePut.oy = oy;
    sScenePut.scale = scale;
    sScenePut.left = left;
    sScenePut.top = top;
    sScenePut.width = width;
    sScenePut.height = height;
#ifdef CTR_FRAME_DUMP
    if (CtrDumpNoCache()) sSceneCacheAllowed = false;
#endif
    if (sSceneCacheAllowed)
    {
        uint32_t key = SceneCacheKey(left, top, width, height, 1 | sWorldLayers | exclude | BattleBoxLayers())
            ^ (sObjWinSlotted ? 0x5bd1e995u : 0u);

        unsigned layers = 1 | sWorldLayers | exclude | BattleBoxLayers();
        bool empty;
        int blend = SceneBlendLayerOf(layers);

        if (blend >= 0 && BattleSceneComposeApart(left, top, width, height, layers, (unsigned)blend))
            return;
        if (sSceneCached && key == sSceneCacheKey && SceneCacheHolds(layers))
        {
            memcpy(sBgDrawn, sSceneCacheDrawn, sizeof(sBgDrawn));
            ++sSceneReused;
            sSceneKeptNow = true;
            return;
        }
        empty = sBattleWorld && sSceneEmpty && key == sSceneEmptyKey && SceneHoldsSince(layers, sSceneEmptyToken);
        if (empty)
        {
            memset(sBgDrawn, 0, sizeof(sBgDrawn));
            ++sSceneEmptyReused;
            return;
        }
        SceneComposeInto(sScene, sSceneColumn, left, top, width, height, layers);
        SceneCacheSync(layers);
        memcpy(sSceneCacheDrawn, sBgDrawn, sizeof(sBgDrawn));
        sSceneCacheKey = key;
        sSceneCacheToken = sStats.frames + 1;
        sSceneCached = true;
        sSceneEmpty = !sBgDrawn[1] && !sBgDrawn[2] && !sBgDrawn[3];
        sSceneEmptyKey = key;
        sSceneEmptyToken = sSceneCacheToken;
        /* The characters both the kept and the empty scene are checked by. */
        SceneEmptyChars(layers);
        return;
    }
    sSceneCached = false;
    SceneComposeInto(sScene, sSceneColumn, left, top, width, height, 1 | sWorldLayers | exclude | BattleBoxLayers());
}

/*
 * The target drawn on next, inside one command list. Citro3D flushes the
 * colour cache when the target changes but invalidates it only at a split,
 * so a target begun without one blended with what the cache still held of
 * the last: on hardware the sprite slots came out again over the text box.
 * Azahar does not model the cache. Reset to NULL after a split.
 *
 * Every switch goes through here: one made straight with C2D_SceneBegin, or
 * the voxel world's C3D_FrameDrawOn, left this naming a target no longer
 * drawn on, and the next SceneOn back to it skipped the invalidate - ghosts
 * of the battle's sprites over the 3D battle on hardware (2026-10-15).
 */
static C3D_RenderTarget *sSceneOn;

/*
 * The PICA's texture cache dropped. Citro3D only asks for that when a
 * texture binding changes, and Citro2D only rebinds a texture other than the
 * one it drew with last: the tile atlas and the HD info boxes are the same
 * texture frame after frame, updated by the CPU, and on hardware the GPU
 * went on sampling what their texels were - a sprite's tiles came out as
 * the sand path that had the atlas slots before (a hardware dump of the
 * frame). Azahar has no such cache. The same bit Citro3D's own context
 * update writes (GPUREG_TEXUNIT_CONFIG bit 16).
 */
static void TexCacheClear(void)
{
    C2D_Flush();
    GPUCMD_AddMaskedWrite(GPUREG_TEXUNIT_CONFIG, 0x4, 1u << 16);
}

static void SceneOn(C3D_RenderTarget *target)
{
    if (target != sSceneOn)
    {
        C2D_Flush();
        GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1);
        GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_INVALIDATE, 1);
        /* What was drawn into the last target is sampled from now on. */
        GPUCMD_AddMaskedWrite(GPUREG_TEXUNIT_CONFIG, 0x4, 1u << 16);
        sSceneOn = target;
    }
    C2D_SceneBegin(target);
}

static void SplitFrame(void)
{
    C3D_FrameSplit(0);
    sSceneOn = NULL;
    GPUCMD_AddMaskedWrite(GPUREG_TEXUNIT_CONFIG, 0x4, 1u << 16);
}

/* A scene composed into `tex` from texel column `column` on `target`, `shift`
 * screen pixels across. */
static void ScenePlaceFrom(C3D_RenderTarget *target, float shift, C3D_Tex *tex, int column)
{
    float ox = sScenePut.ox + shift, oy = sScenePut.oy, scale = sScenePut.scale;
    int width = sScenePut.width, height = sScenePut.height;
    float u0 = column / (float)tex->width, u1 = (column + width) / (float)tex->width;
    const Tex3DS_SubTexture cut = {(u16)width, (u16)height, u0, 1, u1, 1 - height / (float)tex->height};

    SceneOn(target);
    C2D_ViewReset();
    Blend(5, false, false);
    C2D_DrawImageAt((C2D_Image){tex, &cut}, ox, oy, 0, NULL, scale, scale);
    C2D_Flush();
}

/* As SurfaceFilter, for the battle scene's surface. */
static void SceneTexFilter(GPU_TEXTURE_FILTER_PARAM filter)
{
    C2D_Flush();
    C3D_TexSetFilter(&sSceneTex, filter, filter);
    C3D_TexBind(0, &sSceneTex);
}

/* The composed scene on `target`, `shift` screen pixels across. */
static void BattleScenePlace(C3D_RenderTarget *target, float shift)
{
    ScenePlaceFrom(target, shift, &sSceneTex, sSceneColumn);
    if (sSceneBlendLayer >= 0 && sBlendLayerTarget)
    {
        float ox = sScenePut.ox + shift, oy = sScenePut.oy, scale = sScenePut.scale;
        int width = sScenePut.width, height = sScenePut.height;
        const Tex3DS_SubTexture cut = {(u16)width, (u16)height, 0, 1, width / (float)sBlendLayerTex.width,
                                       1 - height / (float)sBlendLayerTex.height};

        /* As the layer's tiles are drawn over the rest (Layers). */
        BlendForget();
        Blend((unsigned)sSceneBlendLayer, true, false);
        C2D_DrawImageAt((C2D_Image){&sBlendLayerTex, &cut}, ox, oy, 0, &sTint, scale, scale);
        C2D_Flush();
        BlendForget();
    }
}

/*
 * The lines the scene surface does not hold: it holds 128 lines and the
 * screen shows 140 above the text box and a couple of dozen more behind it.
 * Its top rows were repeated up to the screen's edge and its last line
 * carried down - which repeated whatever reached them, the heads of tall
 * Pokemon, a battler rising or sinking into them - and over the voxel world
 * nothing was shown there at all, so a move's backdrop (Sandstorm's sand on
 * BG1) stopped in a band at the top and bottom of the screen. They are
 * composed here instead, from the layers at the screen's scale: above,
 * everything of the scene; below, behind the text box, the backgrounds
 * alone. Into the logical surface, at
 * the frame's own view, without the layers the world stands in for, and with
 * the scene's own colour effects: left out (the effects bit, 32, excluded as
 * the text box's pass includes it), a move's background that blends - the
 * sand at EVA over the scenery - came out at full strength in the bands, a
 * seam against the scene round them.
 *
 * What they cost is a pass of Compose each, more than the tiles they cover
 * (the layer walks keep to the clip: ClipCell, DrawTextSpanAt's rows,
 * DrawObjects' boxes): each pass walks the window bands, the sprites and
 * the blend set-up again. Split round the panel into four passes, to leave
 * out the tiles under it, the 2D battle with the 3D slider up spent 2.5-4.6
 * ms a frame on the text box and the edges (bs.text, port.log 2026-10-28),
 * more than the tiles saved. Two passes, then: above, and behind the box.
 * With the 3D slider up they are composed once a frame into the logical
 * surface's rows above the text layers and put in each eye behind the screen
 * (BattleEdgesStore, BattleEdgesPlace): composed per eye straight onto the
 * screen target they were twice the work, and wrong - Scissor's rows are the
 * logical surface's, and a screen target is tilted, so the bands came out
 * black with a scrap of them standing at x ~150 (port.log 2026-10-27, 38.8
 * fps, bg=7.25 ms).
 */
static void Compose(void);

/* Whether this frame's battle draws the wireless indicator apart: in a
 * session, while the adapter's sprite is on screen. */
static bool WirelessIndicatorApart(void)
{
    static uint32_t frame = ~0u;
    static bool apart;

    if (frame != sStats.frames)
    {
        unsigned attr0 = sMemory.oam[WIRELESS_INDICATOR_OAM * 4];

        frame = sStats.frames;
        apart = sBattle && (attr0 & 0x300) != 0x200 && LocalLink_InSession();
    }
    return apart && sIndicatorApartHere;
}

/* The wireless indicator over the battle, at the frame's own view. */
static void BattleIndicatorCompose(void)
{
    int clipX0 = sClipX0, clipY0 = sClipY0, clipX1 = sClipX1, clipY1 = sClipY1;
    uint32_t mask[4];
    bool maskOn = sObjMaskOn;

    if (!WirelessIndicatorApart() || (Reg(0) & 128) || !(Reg(0) & 0x1000)) return;
    memcpy(mask, sObjMask, sizeof(mask));
    memset(sObjMask, 0, sizeof(sObjMask));
    sObjMask[WIRELESS_INDICATOR_OAM >> 5] = 1u << (WIRELESS_INDICATOR_OAM & 31);
    sObjMaskOn = true;
    sObjIndicatorPass = true;
    ClipToView();
    C2D_Flush();
    BlendForget();
    for (int priority = 3; priority >= 0; --priority)
        DrawObjects((unsigned)priority, false);
    C2D_Flush();
    BlendForget();
    sObjIndicatorPass = false;
    sObjMaskOn = maskOn;
    memcpy(sObjMask, mask, sizeof(mask));
    sClipX0 = clipX0;
    sClipY0 = clipY0;
    sClipX1 = clipX1;
    sClipY1 = clipY1;
}

/* GBA rectangle [x0, x1) x [y0, y1) of the scene, without `leave`; the caller
 * keeps it inside the view. */
static void BattleRectCompose(int x0, int y0, int x1, int y1, unsigned leave)
{
    int clipX0 = sClipX0, clipY0 = sClipY0, clipX1 = sClipX1, clipY1 = sClipY1;
    bool scissored = sScissored;
    unsigned exclude = sLayerExclude;

    if (x0 >= x1 || y0 >= y1) return;
    sClipX0 = x0;
    sClipY0 = y0;
    sClipX1 = x1;
    sClipY1 = y1;
    sScissored = true;
    C2D_Flush();
    Scissor(sClipX0, sClipY0, sClipX1, sClipY1);
    BlendForget();
    /* As the scene's own pass (BattleSceneCompose): not the text box's
     * layers, nor the world's; the effects bit (32) stays in. */
    sLayerExclude = 1u | leave | BattleBoxLayers() | (sBattleWorld ? sWorldLayers : 0u);
    Compose();
    C2D_Flush();
    sLayerExclude = exclude;
    sClipX0 = clipX0;
    sClipY0 = clipY0;
    sClipX1 = clipX1;
    sClipY1 = clipY1;
    sScissored = scissored;
    RestoreScissor();
    BlendForget();
}

/*
 * The edges: GBA lines above the surface, everything; below it, the
 * backgrounds where they show round the text box - the sprites there are
 * under the box on the GBA, and round it would only show as cut-off scraps.
 * Composed `widen` screen pixels past each side of the screen, the lines
 * below the surface `lift` screen pixels higher up the target
 * (BattleEdgesStore); 0 and 0 to compose them where they are.
 */
static void BattleEdgesCompose(bool sprites, int widen, int lift)
{
    float offX = sOffX, offY = sOffY;
    int targetW = sTargetW, left, right, top, bottom;
    bool box = BattleSceneBottom() == BATTLE_SCENE_BOX_BOTTOM;

    if (Reg(0) & 128) return;
    sOffX += (float)widen;
    sTargetW += 2 * widen;
    left = VIEW_LEFT;
    right = VIEW_RIGHT;
    top = VIEW_TOP;
    bottom = VIEW_BOTTOM;
    BattleRectCompose(left, top, right, sScenePut.top, sprites ? 0u : 16u);
    if (box)
    {
        int from = BATTLE_SCENE_BOX_BOTTOM;

        sOffY -= (float)lift;
        BattleRectCompose(left, from, right, bottom, 16u);
    }
    sOffX = offX;
    sOffY = offY;
    sTargetW = targetW;
}

/*
 * The edges for both eyes of the 3D battle, composed once a frame into the
 * logical surface after the text box (BattleTextCompose): into its rows above
 * the text layers, which never reach above the battle's GBA picture on the
 * screen (DrawBattleTextLayer: CTR_BATTLE_Y down) - the lines above the
 * scene where they are, those below it lifted into the rows under them - and
 * BATTLE_EDGE_MARGIN screen pixels wider each side, for the displacement.
 * Each eye puts them behind the screen with the scenery (BattleEdgesPlace).
 */
#define BATTLE_EDGE_MARGIN 8
#define BATTLE_EDGE_STORE_END CTR_BATTLE_Y
static struct { bool on, held, bare; int topRows, storeY, rows, from; uint32_t frame; } sEdgeStore;
static unsigned sEdgesKept;

/*
 * The edges stored last frame still there and still right: made of the
 * backgrounds alone (with sprites to slot, the lines above leave them out
 * too), as the scene kept this frame is (sSceneKeptNow), placed alike, and
 * the logical surface's rows above the text box not drawn on since - the
 * frame before took this same path (sEdgeStore.frame). The text box's clear
 * then leaves those rows (BattleTextCompose).
 */
static bool BattleEdgesKeep(bool sprites)
{
    bool box = BattleSceneBottom() == BATTLE_SCENE_BOX_BOTTOM;
    int topRows = (int)ceilf(sScenePut.oy);
    int from = (int)floorf((BATTLE_SCENE_BOX_BOTTOM + sViewY) * sZoom + sOffY);

    return !sprites && sSceneKeptNow && sEdgeStore.held && sEdgeStore.bare && sEdgeStore.frame + 1 == sStats.frames
        && sEdgeStore.topRows == topRows && sEdgeStore.from == from && (sEdgeStore.rows > 0) == box;
}

static void BattleEdgesStore(bool sprites, bool keep)
{
    bool box = BattleSceneBottom() == BATTLE_SCENE_BOX_BOTTOM;
    int topRows = (int)ceilf(sScenePut.oy), storeY = topRows + 2;
    int from = (int)floorf((BATTLE_SCENE_BOX_BOTTOM + sViewY) * sZoom + sOffY), rows = CTR_GAME_HEIGHT - from;

    sEdgeStore.on = false;
    if (keep)
    {
        sEdgeStore.on = true;
        sEdgeStore.frame = sStats.frames;
        ++sEdgesKept;
        return;
    }
    sEdgeStore.held = false;
    if (Reg(0) & 128) return;
    /* The OBJ window of a text layer is drawn wherever the window is, and
     * so are those rows then the text's: no edges for that frame. */
    if (sObjWin.ready && (sObjWin.layers & (1u | BattleBoxLayers()))) return;
    /* The lifted lines' scissor rounds a row past them at most. */
    if (topRows < 0 || topRows > BATTLE_EDGE_STORE_END || rows < 0
        || (box && storeY + rows + 2 > BATTLE_EDGE_STORE_END)) return;
    SceneOn(sLogical);
    C2D_ViewReset();
    BattleEdgesCompose(sprites, BATTLE_EDGE_MARGIN, box ? from - storeY : 0);
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    sScissored = false;
    sEdgeStore.on = sEdgeStore.held = true;
    sEdgeStore.bare = !sprites;
    sEdgeStore.frame = sStats.frames;
    sEdgeStore.topRows = topRows;
    sEdgeStore.storeY = storeY;
    sEdgeStore.rows = box ? rows : 0;
    sEdgeStore.from = from;
}

/* The stored edges on `target`, `shift` screen pixels across. */
static void BattleEdgesPlace(C3D_RenderTarget *target, float shift)
{
    const int w = CTR_GAME_WIDTH + 2 * BATTLE_EDGE_MARGIN;
    const float u = w / 512.0f, x = -BATTLE_EDGE_MARGIN + shift;

    if (!sEdgeStore.on) return;
    BlendForget();
    SceneOn(target);
    C2D_ViewReset();
    Blend(5, false, false);
    if (sEdgeStore.topRows > 0)
    {
        const Tex3DS_SubTexture above = {(u16)w, (u16)sEdgeStore.topRows, 0, 1, u,
                                         1 - sEdgeStore.topRows / 256.0f};

        C2D_DrawImageAt((C2D_Image){&sSurface, &above}, x, 0, 0, NULL, 1, 1);
    }
    if (sEdgeStore.rows > 0)
    {
        const Tex3DS_SubTexture below = {(u16)w, (u16)sEdgeStore.rows, 0, 1 - sEdgeStore.storeY / 256.0f, u,
                                         1 - (sEdgeStore.storeY + sEdgeStore.rows) / 256.0f};

        C2D_DrawImageAt((C2D_Image){&sSurface, &below}, x, (float)sEdgeStore.from, 0, NULL, 1, 1);
    }
    C2D_Flush();
    BlendForget();
}

/*
 * Where the battle scene is composed down to. With the text box hidden - a
 * link battle's VS screen, its windows showing only the players' boxes - the
 * whole picture: carried down from the box's top, the boxes' sides ran on to
 * the screen's bottom edge and their own bottom edge was never drawn.
 */
static int BattleSceneBottom(void)
{
    unsigned display = Reg(0), in = Reg(0x48), out = Reg(0x4a);
    bool shown;

    if (!(display & 0x100))
        shown = false;
    else if (!(display & 0xE000))
        shown = true;
    else
        shown = ((display & 0x2000) && (in & 1)) || ((display & 0x4000) && (in & 0x100)) || (out & 1)
             || ((display & 0x8000) && (out & 0x100));
    return shown ? BATTLE_SCENE_BOX_BOTTOM : 160;
}

/*
 * The scene into the logical surface. With the slider up and the scene not
 * composed for both eyes (the whole-per-eye case), it is composed for this
 * eye: the scenery displaced behind the screen, each sprite at its depth.
 */
static void RenderBattleScene(uint32_t clear)
{
    (void)clear;
    if (!sSceneComposed)
    {
        float origin = sLayerOrigin;

        sLayerOrigin = -sEyeParallax * BATTLE_BACK_DEPTH;
        sSpriteDepth = sEyeParallax != 0.0f;
        BattleSceneCompose(sEyeParallax != 0.0f ? BATTLE_STEREO_MARGIN : 0, 0);
        sSpriteDepth = false;
        sLayerOrigin = origin;
        HudCoverUpdate();
    }
    BattleScenePlace(sLogical, sSceneComposed ? -sEyeParallax * BATTLE_BACK_DEPTH : 0.0f);
    {
        float origin = sLayerOrigin;

        /* The lines past the surface, at the depths the scene has. */
        sLayerOrigin = -sEyeParallax * BATTLE_BACK_DEPTH;
        sSpriteDepth = sEyeParallax != 0.0f;
        /* Into the logical surface, where Scissor's rows are. */
        SceneOn(sLogical);
        BattleEdgesCompose(true, 0, 0);
        sSpriteDepth = false;
        sLayerOrigin = origin;
    }
}

/*
 * The bottom screen's surface, the whole 320x240 screen. The PokéNav uses
 * the 240x240 area left of the button column; the PC's boxes all of it. It
 * is not linked to the screen, since a linked target is copied over the
 * whole of it and would cover the column the bottom screen draws itself;
 * after the frame the PokéNav's part is copied into the first 240 columns of
 * the framebuffer, which are exactly that area (the framebuffer runs column
 * by column from the left edge), and the boxes' into all of it. Made when
 * one of them opens, given back a few seconds after it closes - or at once
 * when the voxel world asks for VRAM (CtrVideo_RequestPlaneRelease): kept
 * after a bag opened from the voxel field, its 150 KiB in the middle of VRAM
 * left no 256 KiB for an atlas, and the world waited seconds for its tiles.
 */
static bool sPlaneReleaseAsked;
#define BOTTOM_SIZE 240
#define BOTTOM_WIDTH 320
static C3D_RenderTarget *sBottom;
static uint32_t sBottomUsed, sBottomFailFrame;
static bool sBottomFailed;

/*
 * The canvas round the window, over the window's columns: the header's 40
 * rows above it and the dock's 40 below, drawn into the bottom surface as a
 * texture, so the window's columns go to the framebuffer whole, one display
 * transfer, with nothing of the picture outside the window. (A copy that
 * skipped those rows, a texture copy with gaps, came out shifted 40 rows in
 * Azahar and is gone.) 3ds_bottom_ui.c names its canvas when it
 * puts it on the screen (CtrVideo_BottomStrips); the texture is filled from it
 * before the bottom is drawn, the same frame. Rows 0-39 are the header's,
 * 64-103 the dock's.
 */
#define STRIP_TEX_W 256
#define STRIP_TEX_H 128
#define STRIP_DOCK_ROW 64
static C3D_Tex sStripTex;
static const u16 *sStripCanvas;
static bool sStripDirty, sStripReady, sStripFailed;

void CtrVideo_BottomStrips(const uint16_t *canvas)
{
    sStripCanvas = canvas;
    sStripDirty = true;
}

static void StripRelease(void)
{
    if (sStripTex.data) C3D_TexDelete(&sStripTex);
    memset(&sStripTex, 0, sizeof(sStripTex));
    sStripReady = sStripFailed = false;
}

/* The canvas is the framebuffer's layout: column x, row y at x * 240 + 239 - y. */
static void StripUpload(void)
{
    u16 *data;

    if (!sStripCanvas || sStripFailed) return;
    if (!sStripTex.data)
    {
        if (!C3D_TexInit(&sStripTex, STRIP_TEX_W, STRIP_TEX_H, GPU_RGB565))
        {
            memset(&sStripTex, 0, sizeof(sStripTex));
            sStripFailed = true;
            CtrLog_Write(CTR_LOG_ERROR, "video: no memory for the bottom window's surroundings (linear free=%lu); "
                         "the window's columns go whole, the header and the dock over them black",
                         (unsigned long)linearSpaceFree());
            return;
        }
        C3D_TexSetFilter(&sStripTex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&sStripTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        sStripDirty = true;
    }
    if (!sStripDirty) return;
    data = sStripTex.data;
    for (int strip = 0; strip < 2; ++strip)
    {
        int screenY = strip ? CTR_BOTTOM_WIN_Y + 160 : 0, texY = strip ? STRIP_DOCK_ROW : 0;

        for (int ty = 0; ty < CTR_BOTTOM_WIN_Y; ty += 8)
            for (int tx = 0; tx < 240; tx += 8)
            {
                u16 *tile = data + (((texY + ty) / 8) * (STRIP_TEX_W / 8) + tx / 8) * 64;

                for (int x = 0; x < 8; ++x)
                {
                    const u16 *column = sStripCanvas + (CTR_BOTTOM_WIN_X + tx + x) * BOTTOM_SIZE
                                      + (BOTTOM_SIZE - 1 - (screenY + ty));

                    for (int y = 0; y < 8; ++y)
                        tile[sMorton[y * 8 + x]] = column[-y];
                }
            }
    }
    C3D_TexFlush(&sStripTex);
    sStripDirty = false;
    sStripReady = true;
}

static void BottomRelease(void)
{
    if (sBottom) { ClearWhenMadeForget(sBottom); C3D_RenderTargetDelete(sBottom); }
    sBottom = NULL;
    StripRelease();
}

/* Outside the frame, where deleting a render target may wait for the GPU. */
static bool BottomReady(bool wanted)
{
    if (!wanted)
    {
        if (sBottom && (sPlaneReleaseAsked || sStats.frames - sBottomUsed > LAYER_IDLE_FRAMES))
        {
            BottomRelease();
            CtrLog_Write(CTR_LOG_VIDEO, "bottom screen surface released (VRAM free=%lu)",
                         (unsigned long)vramSpaceFree());
        }
        return false;
    }
    sBottomUsed = sStats.frames;
    if (sBottom) return true;
    if (sBottomFailed && sStats.frames - sBottomFailFrame < LAYER_RETRY_FRAMES) return false;
    sBottom = C3D_RenderTargetCreate(BOTTOM_SIZE, BOTTOM_WIDTH, GPU_RB_RGB565, -1);
    if (!sBottom && CtrVoxel_ReleaseIdleVramFor(BOTTOM_SIZE * BOTTOM_WIDTH * 2u, true) > 0)
        sBottom = C3D_RenderTargetCreate(BOTTOM_SIZE, BOTTOM_WIDTH, GPU_RB_RGB565, -1);
    ClearWhenMade(sBottom);
    if (!sBottom)
    {
        if (!sBottomFailed)
            CtrLog_Write(CTR_LOG_ERROR, "VIDEO: no VRAM for the bottom screen surface (free=%lu)",
                         (unsigned long)vramSpaceFree());
        sBottomFailed = true;
        sBottomFailFrame = sStats.frames;
        return false;
    }
    sBottomFailed = false;
    CtrLog_Write(CTR_LOG_VIDEO, "bottom screen surface ready (VRAM free=%lu)", (unsigned long)vramSpaceFree());
    return true;
}

static bool sBottomInUse;
/* 3ds_bottom_ui.c draws the game's screen itself, from its state: its
 * picture is not composed at all (CtrVideo_SetBottomNative). */
static bool sBottomNative;

bool CtrVideo_BottomInUse(void) { return sBottomInUse; }

/*
 * The battle's sandstorm picture (AnimTask_LoadSandstormBackground): none
 * over a voxel world that draws the sandstorm itself (CtrVoxel_DrawsSand) -
 * the route's own sand already blows through the battle, and BG1 over the
 * world, in front of the sprites, made the 3D slider compose the scene
 * whole per eye (bs.whole 10 ms, 27 fps, port.log 2026-10-28); its sprites
 * still fly. Otherwise held still on an Old 3DS: scrolled, BG1 changes every
 * frame, so the scene surface and the lines past it are composed afresh
 * each one, ~30-40 fps with the 3D slider up; still, the scene is kept
 * between frames (SceneCacheKey) but for those its blend steps on.
 */
unsigned char CtrBattle_Sandstorm(void)
{
    if (sBattleWorld && CtrVoxel_DrawsSand()) return CTR_SANDSTORM_NONE;
    return sOldModel ? CTR_SANDSTORM_STILL : CTR_SANDSTORM_BLOWN;
}
void CtrVideo_SetBottomNative(bool native) { sBottomNative = native; }

/* Without the classic menus the bottom screen draws the Pokédex, the bag and
 * the party menu opened from the field itself: their pictures are never
 * composed, not even the frames they open or close in while it has no view
 * of them (the old screen flashed there). */
static bool sNativeMenus;

void CtrVideo_SetNativeMenus(bool on) { sNativeMenus = on; }

static bool BottomNative(unsigned screen)
{
    return sBottomNative
        || (sNativeMenus && (screen == CTR_CENTRED_POKEDEX || screen == CTR_CENTRED_BAG
                             || screen == CTR_CENTRED_PARTY));
}

/* The screens drawn over the whole bottom screen, rather than in its window. */
static bool BottomWhole(unsigned screen)
{
    return screen == CTR_CENTRED_STORAGE || screen == CTR_CENTRED_SUMMARY || screen == CTR_CENTRED_BAG_WHOLE
        || screen == CTR_CENTRED_PARTY_WHOLE || screen == CTR_CENTRED_SHOP || screen == CTR_CENTRED_TRADE_MENU
        || screen == CTR_CENTRED_TRADE || screen == CTR_CENTRED_POKEDEX_CAUGHT;
}

/* The screens drawn on the bottom screen at all. */
static bool BottomScreen(unsigned screen)
{
    return screen == CTR_CENTRED_POKENAV || screen == CTR_CENTRED_BAG || screen == CTR_CENTRED_POKEDEX
        || screen == CTR_CENTRED_PARTY || screen == CTR_CENTRED_CARD || BottomWhole(screen);
}

bool CtrVideo_BottomWhole(void) { return sBottomInUse && BottomWhole(sCentredScreen); }

/*
 * The composed picture into the bottom screen's framebuffer, inside the
 * frame: there citro3d queues the transfer after the drawing (a split) instead
 * of waiting for it. Called after C3D_FrameEnd it waited for the GPU to finish
 * the whole frame and then for the copy, every frame a bottom screen is up:
 * the PC, the bag and the Pokédex spent ~5 ms of CPU a frame on an Old 3DS
 * beyond their layers and sprites, and dropped frames. The framebuffer is
 * single-buffered (3ds_log.c).
 *
 * A game screen sits in the bottom screen's window, the GBA picture 1:1 at
 * (CTR_BOTTOM_WIN_X, CTR_BOTTOM_WIN_Y); 3ds_bottom_ui.c's canvas owns the
 * rest (the header, the dock, the window's frame). One with the whole screen
 * (BottomWhole) is the picture at the same place and its margins round it,
 * all of it transferred. A display transfer copies whole framebuffer lines,
 * which are screen columns: the window's 240 columns go whole, with the
 * header and the dock drawn over them (StripUpload). Without that texture
 * they are black over the window's columns, and that is logged once.
 */

static void BottomTransfer(void)
{
    u16 *fb = (u16 *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
    u16 *in;
    const u32 flags = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0)
                    | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565)
                    | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);

    if (!sBottom || !fb || gfxGetScreenFormat(GFX_BOTTOM) != GSP_RGB565_OES) return;
    /* A screen with the whole screen (the boxes, a summary, the bag from a
     * battle): all of it, margins included. */
    if (BottomWhole(sCentredScreen))
    {
        C3D_SyncDisplayTransfer((u32 *)sBottom->frameBuf.colorBuf, GX_BUFFER_DIM(BOTTOM_SIZE, BOTTOM_WIDTH),
                                (u32 *)fb, GX_BUFFER_DIM(BOTTOM_SIZE, BOTTOM_WIDTH), flags);
        return;
    }
    /* The target is BOTTOM_WIDTH lines of BOTTOM_SIZE (8x8 tiles): the
     * window's first column is line CTR_BOTTOM_WIN_X, a whole tile row. */
    in = (u16 *)sBottom->frameBuf.colorBuf + CTR_BOTTOM_WIN_X * BOTTOM_SIZE;
    /* The header and the dock drawn over the columns (StripUpload): whole. */
    C3D_SyncDisplayTransfer((u32 *)in, GX_BUFFER_DIM(BOTTOM_SIZE, 240),
                            (u32 *)(fb + CTR_BOTTOM_WIN_X * BOTTOM_SIZE), GX_BUFFER_DIM(BOTTOM_SIZE, 240), flags);
}

/*
 * A game screen on the bottom screen (the PC's boxes, a summary, the bag, the
 * party menu, the Pokédex, the PokéNav): the GBA picture 1:1, cut at its
 * edges, where the GBA screen ends and parks what it hides. In the window
 * only the picture reaches the screen (BottomTransfer), everything round it
 * is 3ds_bottom_ui.c's; with the whole screen its margins are the layers'
 * own past the picture (CentredFill), the sprites kept to the picture.
 */
static void StorageComposePart(int left, int right, int top, int bottom, unsigned exclude)
{
    ClipToView();
    sClipX0 = left;
    sClipX1 = right;
    sClipY0 = top;
    sClipY1 = bottom;
    sLayerExclude = exclude;
    C2D_Flush();
    sScissored = true;
    RestoreScissor();
    Compose();
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    sScissored = false;
    sLayerExclude = 0;
}

static void WindowCompose(void)
{
    if (BottomWhole(sCentredScreen))
    {
        unsigned back = CentredLayers(CentredFillOf(sCentredScreen)) ? CentredLayers(CentredFillOf(sCentredScreen))
                                                                       : 1u << 3;

        StorageComposePart(-CTR_BOTTOM_WIN_X, 240 + CTR_BOTTOM_WIN_X, -CTR_BOTTOM_WIN_Y, 160 + CTR_BOTTOM_WIN_Y,
                           63u & ~back);
    }
    StorageComposePart(0, 240, 0, 160, 0);
    ClipToView();
}

int CtrVideo_BottomPictureY(int y)
{
    y -= CTR_BOTTOM_WIN_Y;
    return y >= 0 && y < 160 ? y : -1;
}

static void SurfaceFilter(GPU_TEXTURE_FILTER_PARAM filter)
{
    /* Batched draws sample with the state at the flush: flush first, and
     * rebind so the new filter reaches the GPU. */
    C2D_Flush();
    C3D_TexSetFilter(&sSurface, filter, filter);
    C3D_TexBind(0, &sSurface);
}

static void RenderEye(C3D_RenderTarget *target, uint32_t clear, float parallax)
{
    const Tex3DS_SubTexture logical = {CTR_GAME_WIDTH, CTR_GAME_HEIGHT, 0, 1,
        CTR_GAME_WIDTH / 512.0f, 1 - CTR_GAME_HEIGHT / 256.0f};
    bool scene = sBattle && sScene;

    sParallax = parallax;
    sLayerShift = 0;
    BlendForget();
    ColourClear(sLogical, clear);
    sIndicatorApartHere = scene;
    if (scene) RenderBattleScene(clear);
    SceneOn(sLogical);
    Blend(5, false, false);
    StageUnderlay();
    if (target != sBottom) CornerUnderlay();
    if (scene) BattleIndicatorCompose();
    sIndicatorApartHere = false;
    if (scene) HudDrawCrisp(HUD_SPRITE_DEPTH);
    /* After a scene composed on its own, only the text box is left. */
    if (scene) sLayerExclude = 63 & ~(1u | 32u | BattleBoxLayers());
    if (!(Reg(0) & 128))
    {
        if (target != sBottom) Compose();
        else WindowCompose();
    }
    sLayerExclude = 0;
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    C3D_FrameSplit(0);

    BlendForget();
    ColourClear(target, C2D_Color32(0, 0, 0, 255));
    SceneOn(target);
    if (target == sBottom)
    {
        /* The middle 320 columns of the logical frame, the picture in the
         * window's place (BottomTransfer keeps only that). Tilted as a screen
         * target is. */
        int width = BOTTOM_WIDTH;
        const Tex3DS_SubTexture middle = {(u16)width, BOTTOM_SIZE,
            (CTR_GAME_WIDTH - width) / 2 / 512.0f, 1,
            (CTR_GAME_WIDTH + width) / 2 / 512.0f, 1 - BOTTOM_SIZE / 256.0f};

        /* The framebuffer's own measures, as for a screen target. */
        C2D_SceneSize(BOTTOM_SIZE, BOTTOM_WIDTH, true);
        C2D_ViewReset();
        Blend(5, false, false);
        C2D_DrawImageAt((C2D_Image){&sSurface, &middle}, 0, 0, 0, NULL, 1, 1);
        /* In the window: the header and the dock over its columns, so they
         * go whole to the screen (BottomTransfer). */
        if (!BottomWhole(sCentredScreen))
        {
            StripUpload();
            if (sStripReady)
            {
                const float w = 240.0f / STRIP_TEX_W, h = (float)CTR_BOTTOM_WIN_Y / STRIP_TEX_H,
                            dock = (float)STRIP_DOCK_ROW / STRIP_TEX_H;
                const Tex3DS_SubTexture head = {240, CTR_BOTTOM_WIN_Y, 0, 1, w, 1 - h};
                const Tex3DS_SubTexture foot = {240, CTR_BOTTOM_WIN_Y, 0, 1 - dock, w, 1 - dock - h};

                C2D_DrawImageAt((C2D_Image){&sStripTex, &head}, CTR_BOTTOM_WIN_X, 0, 0, NULL, 1, 1);
                C2D_DrawImageAt((C2D_Image){&sStripTex, &foot}, CTR_BOTTOM_WIN_X, CTR_BOTTOM_WIN_Y + 160, 0, NULL, 1, 1);
            }
        }
    }
    else if (sStageZoomed)
    {
        /* The GBA picture alone, out of the margins composed round it,
         * magnified and centred, smooth as no whole number of pixels fits.
         * A scene shown through window 0 alone with a bar at its top and its
         * bottom (the charge) fills the width, the bars' outer lines cut off;
         * one whose window is open at an edge (the chase, its top bar left
         * where the art cuts Rayquaza's tail off: rayquaza_scene.c,
         * DUO_LOWER) is the window's lines alone, covering the screen, a few
         * columns at its sides cut off; one without is shown whole, fitted
         * to the height. */
        int y0 = 0, y1 = 160;
        float scale = (float)CTR_GAME_HEIGHT / 160.0f;

        if ((Reg(0) & 0xE000) == 0x2000 && !(Reg(0x4a) & 31))
        {
            unsigned v = Reg(0x44);
            int top = (int)(v >> 8), bottom = (int)(v & 255);

            if (bottom > 160 || top > bottom) bottom = 160;
            scale = (float)CTR_GAME_WIDTH / 240.0f;
            if ((top == 0) != (bottom == 160) && bottom - top >= 16)
            {
                y0 = top;
                y1 = bottom;
                scale = fmaxf(scale, (float)CTR_GAME_HEIGHT / (float)(y1 - y0));
            }
        }
        {
            const Tex3DS_SubTexture picture = {240, (u16)(y1 - y0),
                CTR_STAGE_X / 512.0f, 1 - (CTR_STAGE_Y + y0) / 256.0f,
                (CTR_STAGE_X + 240) / 512.0f, 1 - (CTR_STAGE_Y + y1) / 256.0f};

            C2D_ViewReset();
            Blend(5, false, false);
            SurfaceFilter(GPU_LINEAR);
            C2D_DrawImageAt((C2D_Image){&sSurface, &picture}, (CTR_GAME_WIDTH - 240 * scale) / 2,
                            (CTR_GAME_HEIGHT - (y1 - y0) * scale) / 2, 0, NULL, scale, scale);
            SurfaceFilter(GPU_NEAREST);
        }
    }
    else
    {
        C2D_ViewReset();
        Blend(5, false, false);
        C2D_DrawImageAt((C2D_Image){&sSurface, &logical}, 0, 0, 0, NULL, 1, 1);
    }
    C2D_Flush();
    C3D_FrameSplit(0);
}

/*
 * The battle's sprites in 3D (RenderBattleStereo). The sprites on screen are
 * gathered into groups - those that touch or overlap at about the same
 * depth, like the halves of an info box and its bars - and each group is composed
 * once a frame at SCENE_ZOOM into a slot of its own, packed in shelves into
 * the free part of a surface. Each eye then draws every slot where its group
 * is, displaced by the group's depth. Apart, two sprites that touch each
 * fade to transparent over the bilinear draw's half pixel, which showed on
 * hardware as a line between them; a group is one picture, as the 2D battle
 * composes it, with a GBA pixel of transparent border for that draw, and
 * groups never overlap, so the order they are drawn in does not matter.
 *
 * Where the slots go (sRegions): in the voxel battle, the scene surface's
 * columns past the world's picture and the scenery (RenderBattleWorld), and
 * what does not fit there into a surface of their own made for it when VRAM
 * allows (sSlotWorld); in the 2D battle, a surface of its own (sSlotTarget,
 * there is VRAM beside the 2D battle), and what does not fit there, or all
 * of them without it, into the scene surface's columns after the scenery.
 */
typedef struct
{
    uint32_t members[4];    /* OAM entries of the group */
    int16_t x, y, w, h;     /* GBA box, clipped to what can be seen */
    int16_t sx, sy;         /* texel of the slot's top-left corner */
    int16_t centre;         /* GBA line the group's depth is taken at */
    uint8_t region;         /* sRegions entry it is in */
    uint8_t nearest, furthest;  /* its members' priorities */
    bool front;             /* in front of the backgrounds before sprites */
    uint8_t patch;          /* nonzero: those backgrounds (Layers bits) over a group */
    uint8_t first;          /* its frontmost OAM entry */
    bool objWin;            /* the OBJ window is over it: drawn at its depth (ObjWindowEye) */
    uint8_t blendBg;        /* backgrounds in front of it that blend with it, in its slot (SlotsCompose) */
} BattleSlot;
static BattleSlot sSlots[128];
static unsigned sSlotCount;
/*
 * Semi-transparent sprites (OBJ mode 1, a move's haze or glow): in no group.
 * In a slot their blend had nothing under it - the slot is cleared, and one
 * bit of alpha cannot keep them see-through - so their frames were composed
 * whole, every sprite flat (26 frames of a move in a test battle). Each is
 * drawn on its own over each eye's picture instead, after the slots, at its
 * depth, blended with what is there as the 2D battle blends it (LooseEye).
 */
#define LOOSE_MAX 16
static struct { uint8_t index, priority; int16_t x, y, w, h, centre; } sLoose[LOOSE_MAX];
static unsigned sLooseCount;
/* The order the slots are drawn in (SlotsPack): back to front. */
static uint8_t sPlaceOrder[128];
/*
 * The widest and tallest group, in GBA pixels: a move's sprite across both
 * Pokemon would otherwise merge everything on screen into one group wider
 * than any surface has room for. Past it, sprites stay apart and are drawn
 * in the GBA's order (sPlaceOrder). An info box is 128 wide, its two halves
 * and its bars; split, each half was one picture drawn whole over the other,
 * so the empty bar of the right half covered the HP bar and the name.
 */
#define SLOT_GROUP_MAX 136
/*
 * Sprites that touch are one group only at about the same depth
 * (BattleSpriteNear): the opponent's feet touch the player's info box, and
 * merged they took the box's left half to the Pokemon's depth, apart from
 * its right half, and the box out of one piece.
 */
#define SLOT_GROUP_DEPTH 0.25f
static C3D_Tex sSlotTex;
static C3D_RenderTarget *sSlotTarget;
static bool sSlotWanted, sSlotFailed;
/*
 * The slots of the battle over the voxel world, which share the scene surface
 * with the world's picture: with a move's background in it as well (Surf's
 * wave) they did not fit beside both, and every frame of the move was
 * composed whole per eye - 27-38 fps on an Old 3DS. Then they are given the
 * surface of their own too, out of VRAM nobody holds or an atlas out of view
 * (SlotsPrepare) - never one on screen, the field came back in 2D when one
 * went for them - and given back as soon as the voxel world asks for VRAM.
 */
static bool sSlotWorld;
#define SLOT_WORLD_RESERVE (128u * 1024u)
static uint32_t sSlotFailFrame;
/* Where this frame's slots can go: target, texture and first free column. */
typedef struct
{
    C3D_RenderTarget *target;
    C3D_Tex *tex;
    int column;
    int end;    /* the first column past the region */
} SlotRegion;
static SlotRegion sRegions[2];
static unsigned sRegionCount;

/* Outside the frame, beside the scene surface: the blending layer's surface
 * (BattleSceneComposeApart), for the 2D battle only, out of free VRAM. */
static void SceneBlendRelease(void)
{
    if (sBlendLayerTarget) { ClearWhenMadeForget(sBlendLayerTarget); C3D_RenderTargetDelete(sBlendLayerTarget); }
    if (sBlendLayerTex.data) C3D_TexDelete(&sBlendLayerTex);
    sBlendLayerTarget = NULL;
    sBlendLayerCached = false;
    memset(&sBlendLayerTex, 0, sizeof(sBlendLayerTex));
}

static void SceneBlendPrepare(void)
{
    bool wanted = sBlendLayerWanted;

    sBlendLayerWanted = false;
    if (sBlendLayerTarget && (!sBattle || sVramAsked || sTransitionRequested
                              || sStats.frames - sBlendLayerUsedFrame > LAYER_IDLE_FRAMES))
    {
        SceneBlendRelease();
        return;
    }
    if (!wanted || sBlendLayerTarget
        || (sBlendLayerFailFrame && sStats.frames - sBlendLayerFailFrame < LAYER_RETRY_FRAMES))
        return;
    /* Never at an atlas's cost, and with room left over: an atlas out of
     * view given for it left the VRAM in pieces still, no 512 KiB block,
     * in Azahar with 450 KiB free as on hardware (2026-10-28) - the atlas
     * lost for nothing. Without it the steps compose the scenery again. */
    if (vramSpaceFree() >= SCENE_W * SCENE_H * 2u + SLOT_WORLD_RESERVE
        && C3D_TexInitVRAM(&sBlendLayerTex, SCENE_W, SCENE_H, GPU_RGBA5551)
        && (sBlendLayerTarget = C3D_RenderTargetCreateFromTex(&sBlendLayerTex, GPU_TEXFACE_2D, 0, -1)))
    {
        C3D_TexSetFilter(&sBlendLayerTex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&sBlendLayerTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        ClearWhenMade(sBlendLayerTarget);
        sBlendLayerFailFrame = 0;
    }
    else
    {
        SceneBlendRelease();
        sBlendLayerFailFrame = sStats.frames | 1u;
    }
    CtrLog_Write(CTR_LOG_VIDEO, "VIDEO: battle blend layer surface %s (VRAM free=%lu)",
                 sBlendLayerTarget ? "made" : "not made", (unsigned long)vramSpaceFree());
}

/* Outside the frame, beside the scene surface (ScenePrepare). */
static void SlotsRelease(void)
{
    if (sSlotTarget) { ClearWhenMadeForget(sSlotTarget); C3D_RenderTargetDelete(sSlotTarget); }
    if (sSlotTex.data) C3D_TexDelete(&sSlotTex);
    sSlotTarget = NULL;
    sSlotWorld = false;
    memset(&sSlotTex, 0, sizeof(sSlotTex));
}

static void SlotsPrepare(void)
{
    static uint32_t usedFrame;
    bool wanted = sSlotWanted;

    sSlotWanted = false;
    if (wanted) usedFrame = sStats.frames;
    else if (sSlotTarget && (sTransitionRequested || (sVramAsked && (!sBattle || sSlotWorld)) || (sSlotWorld && !sBattle)
                             || sStats.frames - usedFrame > LAYER_IDLE_FRAMES))
    {
        SlotsRelease();
        CtrLog_Write(CTR_LOG_VIDEO, "VIDEO: 3D battle sprite slots released (VRAM free=%lu)",
                     (unsigned long)vramSpaceFree());
    }
    if (!wanted || sSlotTarget || (sSlotFailed && sStats.frames - sSlotFailFrame < LAYER_RETRY_FRAMES))
        return;
    /*
     * VRAM only (Citro3D draws into nothing else). The 2D battle shows no
     * voxel world, so the atlases of maps off screen may go, as for the scene
     * surface (ScenePrepare). Half the scene surface's width, which holds
     * the usual four groups; the rest go into the scene surface's free
     * columns (SlotsPack). Without it the slots share the scene surface.
     * Never the whole 1024: made so on hardware (after an atlas was released
     * for it), the slots came out as stale VRAM - an old atlas where the
     * info boxes should be - for the whole battle; Azahar does not show it.
     */
    /*
     * Over the world, an atlas out of view may give way for them, as for the
     * scene surface (ScenePrepare), at most every ten seconds: on a map of
     * five atlases nothing else was free (port.log 2026-10-28: 194 KiB), and
     * Surf's frames went whole per eye, 30 fps.
     */
    if (sSlotWorld && vramSpaceFree() < SCENE_W / 2 * SCENE_H * 2u + SLOT_WORLD_RESERVE)
    {
        static uint32_t freedFrame;

        if (freedFrame == 0 || sStats.frames - freedFrame >= 600u)
        {
            freedFrame = sStats.frames | 1u;
            CtrVoxel_ReleaseIdleVramFor(SCENE_W / 2 * SCENE_H * 2u, false);
        }
        if (vramSpaceFree() < SCENE_W / 2 * SCENE_H * 2u + SLOT_WORLD_RESERVE)
        {
            sSlotFailed = true;
            sSlotFailFrame = sStats.frames;
            return;
        }
    }
    bool overWorld = sSlotWorld;

    for (unsigned attempt = 0; attempt < 2 && !sSlotTarget; ++attempt)
    {
        unsigned width = SCENE_W / 2;

        if (C3D_TexInitVRAM(&sSlotTex, width, SCENE_H, GPU_RGBA5551)
            && (sSlotTarget = C3D_RenderTargetCreateFromTex(&sSlotTex, GPU_TEXFACE_2D, 0, -1)))
        {
            ClearWhenMade(sSlotTarget);
            break;
        }
        /* SlotsRelease forgets sSlotWorld: over the world, no atlas on
         * screen is ever given for them (above). */
        SlotsRelease();
        sSlotWorld = overWorld;
        if (overWorld) break;
        if (attempt == 0) CtrVoxel_ReleaseIdleVramFor(SCENE_W / 2 * SCENE_H * 2u, true);
    }
    if (sSlotTarget)
    {
        C3D_TexSetFilter(&sSlotTex, GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&sSlotTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        sSlotFailed = false;
    }
    else
    {
        SlotsRelease();
        sSlotFailed = true;
        sSlotFailFrame = sStats.frames;
    }
    CtrLog_Write(sSlotTarget ? CTR_LOG_VIDEO : CTR_LOG_ERROR,
                 "VIDEO: 3D battle sprite slots %s, %u texels wide (VRAM free=%lu)",
                 sSlotTarget ? "made" : "not made, sharing the scene surface",
                 sSlotTarget ? (unsigned)sSlotTex.width : 0u, (unsigned long)vramSpaceFree());
}

/*
 * The battle's sprites this frame, clipped to what the scene can show, and
 * whether they can be drawn apart from the scenery: none blends with what is
 * under it, and no window cuts them. `furthest` is the
 * priority of the one furthest back.
 */
static void BlendWhy(unsigned kind)
{
    static unsigned logged[6];
    static const char *const names[6] = {"semi-transparent OBJ", "BLDCNT OBJ target", "OBJ window under no group",
                                         "OBJ window cut by WIN0/1", "blended BG in front", "?"};

    ++sBlendWhy[kind];
    if (logged[kind]++ < 6)
        CtrLog_Write(CTR_LOG_VIDEO, "3D battle blend: %s (frame %lu, DISPCNT %04x BLDCNT %04x BLDALPHA %04x "
                     "WININ %04x WINOUT %04x BGCNT %04x %04x %04x %04x drawn %u %u %u)", names[kind],
                     (unsigned long)sStats.frames, Reg(0), Reg(0x50), Reg(0x52), Reg(0x48), Reg(0x4a), Reg(8),
                     Reg(10), Reg(12), Reg(14), sBgDrawn[1], sBgDrawn[2], sBgDrawn[3]);
}

static bool BattleSpritesGather(unsigned *furthest, bool *apart, bool boxes)
{
    unsigned display = Reg(0), control = Reg(0x50);
    bool alpha = ((control >> 6) & 3) == 1 && (control & 16);
    int reach = (int)ceilf(BATTLE_SPRITE_DEPTH / sShiftZoom) + 1;
    int top = (int)lroundf((sScenePut.oy - sOffY) / sZoom), bottom = BATTLE_SCENE_BOTTOM;
    int left = VIEW_LEFT - reach, right = VIEW_RIGHT + reach;

    sSlotCount = 0;
    sLooseCount = 0;
    *furthest = 0;
    /* The OBJ window is on through every battle (DISPCNT 0xbf40); its own
     * sprites (mode 2) are not drawn by this renderer anyway (DrawObjects).
     * Windows 0 and 1 cut the slots as they cut the screen (SlotsCompose). */
    *apart = true;
    if (!(display & 0x1000)) return false;
    for (unsigned i = 0; i < 128; ++i)
    {
        unsigned attr0 = sMemory.oam[i * 4], attr1 = sMemory.oam[i * 4 + 1], attr2 = sMemory.oam[i * 4 + 2];
        unsigned mode = (attr0 >> 10) & 3, shape = attr0 >> 14;
        bool affine = (attr0 & 0x100) != 0, twice = affine && (attr0 & 0x200);
        int x, y, w, h, full;
        BattleSlot *slot;

        if ((!affine && (attr0 & 0x200)) || shape == 3 || mode >= 2) continue;
        if (!boxes && sHudOamBox[i] >= 0) continue;
        w = sObjDims[shape][attr1 >> 14][0] << twice;
        h = sObjDims[shape][attr1 >> 14][1] << twice;
        x = (int)(attr1 & 511);
        y = (int)(attr0 & 255);
        if (x + w > 512) x -= 512;
        if (y + h > 256) y -= 256;
        full = y;
        /* Under the text box and above the scene nothing of it shows. */
        if (x < left) { w -= left - x; x = left; }
        if (y < top) { h -= top - y; y = top; }
        if (x + w > right) w = right - x;
        if (y + h > bottom) h = bottom - y;
        if (w <= 0 || h <= 0) continue;
        if (mode == 1 && !alpha && sLooseCount < LOOSE_MAX)
        {
            sLoose[sLooseCount].index = (uint8_t)i;
            sLoose[sLooseCount].priority = (uint8_t)((attr2 >> 10) & 3);
            sLoose[sLooseCount].x = (int16_t)x;
            sLoose[sLooseCount].y = (int16_t)y;
            sLoose[sLooseCount].w = (int16_t)w;
            sLoose[sLooseCount].h = (int16_t)h;
            sLoose[sLooseCount].centre = (int16_t)(full + (int)(sObjDims[shape][attr1 >> 14][1] << twice) / 2);
            ++sLooseCount;
            if (((attr2 >> 10) & 3) > *furthest) *furthest = (attr2 >> 10) & 3;
            continue;
        }
        if (mode == 1 || alpha) { if (*apart) { StereoWhy(0); BlendWhy(alpha ? 1 : 0); } *apart = false; }
        /* Mosaic is not drawn (DrawObjects), composed whole or apart: kept
         * apart, a sprite in it keeps its depth. */
        if (((attr2 >> 10) & 3) > *furthest) *furthest = (attr2 >> 10) & 3;
        slot = &sSlots[sSlotCount++];
        memset(slot->members, 0, sizeof(slot->members));
        slot->members[i >> 5] = 1u << (i & 31);
        slot->x = (int16_t)x;
        slot->y = (int16_t)y;
        slot->w = (int16_t)w;
        slot->h = (int16_t)h;
        slot->nearest = slot->furthest = (uint8_t)((attr2 >> 10) & 3);
        slot->front = false;
        slot->patch = 0;
        slot->objWin = false;
        slot->blendBg = 0;
        slot->first = (uint8_t)i;
        /* The depth of the whole sprite, not of the part on show. */
        slot->centre = (int16_t)(full + (int)(sObjDims[shape][attr1 >> 14][1] << twice) / 2);
    }
    /* Groups: merged while any two touch, each at the depth of its middle. */
    for (bool merged = true; merged; )
    {
        merged = false;
        for (unsigned a = 0; a < sSlotCount && !merged; ++a)
            for (unsigned b = a + 1; b < sSlotCount && !merged; ++b)
            {
                BattleSlot *p = &sSlots[a], *q = &sSlots[b];
                int x0, y0, x1, y1;

                if (p->x > q->x + q->w || q->x > p->x + p->w || p->y > q->y + q->h || q->y > p->y + p->h)
                    continue;
                x0 = p->x < q->x ? p->x : q->x;
                y0 = p->y < q->y ? p->y : q->y;
                x1 = p->x + p->w > q->x + q->w ? p->x + p->w : q->x + q->w;
                y1 = p->y + p->h > q->y + q->h ? p->y + p->h : q->y + q->h;
                if (x1 - x0 > SLOT_GROUP_MAX || y1 - y0 > SLOT_GROUP_MAX) continue;
                if (fabsf(BattleSpriteNear(p->centre) - BattleSpriteNear(q->centre)) > SLOT_GROUP_DEPTH)
                    continue;
                if (q->first < p->first) p->first = q->first;
                for (unsigned m = 0; m < 4; ++m) p->members[m] |= q->members[m];
                if (q->nearest < p->nearest) p->nearest = q->nearest;
                if (q->furthest > p->furthest) p->furthest = q->furthest;
                p->centre = (int16_t)((p->centre * p->w * p->h + q->centre * q->w * q->h)
                                      / (p->w * p->h + q->w * q->h));
                p->x = (int16_t)x0;
                p->y = (int16_t)y0;
                p->w = (int16_t)(x1 - x0);
                p->h = (int16_t)(y1 - y0);
                sSlots[b] = sSlots[--sSlotCount];
                merged = true;
            }
    }
    /*
     * The info boxes drawn from their pictures (HudDrawCrisp): their sprites
     * draw nothing (DrawObjects), and a group of them alone is an empty slot
     * - in a double battle four of them 128 wide, which with the four
     * Pokemon did not fit the slots' own surface. Groups with any other
     * sprite in them stay as they are, at the depth they have. The furthest
     * priority is then that of what is left.
     */
    bool drop = sHud && sHudCrisp;

#ifdef CTR_FRAME_DUMP
    if (CtrDumpOff() & 2) drop = false;
#endif
    if (drop)
    {
        bool dropped = false;

        for (unsigned k = 0; k < sSlotCount; )
        {
            bool boxes = true;

            for (unsigned i = 0; i < 128 && boxes; ++i)
                if ((sSlots[k].members[i >> 5] & (1u << (i & 31))) && sHudOamBox[i] < 0) boxes = false;
            if (boxes)
            {
                sSlots[k] = sSlots[--sSlotCount];
                dropped = true;
            }
            else ++k;
        }
        if (dropped)
        {
            *furthest = 0;
            for (unsigned k = 0; k < sSlotCount; ++k)
                if (sSlots[k].furthest > *furthest) *furthest = sSlots[k].furthest;
            for (unsigned k = 0; k < sLooseCount; ++k)
                if (sLoose[k].priority > *furthest) *furthest = sLoose[k].priority;
        }
    }
    /* Back to front among themselves, as the GBA layers them: the furthest
     * priority first, then the higher OAM entries (SlotsPlace interleaves
     * them with the groups). */
    for (unsigned i = 1; i < sLooseCount; ++i)
        for (unsigned j = i; j > 0; --j)
        {
            unsigned a = sLoose[j - 1].priority << 8 | sLoose[j - 1].index, b = sLoose[j].priority << 8 | sLoose[j].index;

            if (a >= b) break;
            {
                __typeof__(sLoose[0]) t = sLoose[j];

                sLoose[j] = sLoose[j - 1];
                sLoose[j - 1] = t;
            }
        }
    return sSlotCount > 0 || sLooseCount > 0;
}

/*
 * A group on both sides of a background in front of sprites cannot be put
 * apart, and the frame is composed whole, every Pokemon flat. With the info
 * boxes drawn from their pictures, their sprites draw nothing, but they
 * still joined a Pokemon's group: the opponent with its box (priority 0 over
 * its 2), which Surf's wave (BG1, priority 1) comes between - all of the
 * move flat. For such a frame alone the groups are gathered again without
 * those sprites; any other frame keeps the groups, and depths, it had.
 */
static bool BattleSlotsStraddle(unsigned furthest)
{
    unsigned nearestFront = 3, furthestFront = 0;
    bool front = false;

    for (unsigned bg = 1; bg < 4; ++bg)
        if (sBgDrawn[bg] && (Reg(8 + bg * 2) & 3) < furthest)
        {
            unsigned priority = Reg(8 + bg * 2) & 3;

            front = true;
            if (priority < nearestFront) nearestFront = priority;
            if (priority > furthestFront) furthestFront = priority;
        }
    for (unsigned k = 0; front && k < sSlotCount; ++k)
        if (sSlots[k].furthest > nearestFront && sSlots[k].nearest <= furthestFront) return true;
    return false;
}

static bool BattleSprites(unsigned *furthest, bool *apart)
{
    bool any = BattleSpritesGather(furthest, apart, true);

    bool regroup = true;

#ifdef CTR_FRAME_DUMP
    regroup = !(CtrDumpOff() & 4);
#endif
    if (regroup && any && *apart && sHud && sHudCrisp && BattleSlotsStraddle(*furthest))
        any = BattleSpritesGather(furthest, apart, false);
    return any;
}

/* Loose sprite `l` over an eye's picture, at its depth (sLoose). */
static void LooseDraw(C3D_RenderTarget *picture, unsigned l)
{
    float offX = sOffX;
    unsigned exclude = sLayerExclude;
    uint32_t mask[4];

    memcpy(mask, sObjMask, sizeof(mask));
    BlendForget();
    SceneOn(picture);
    ClipToView();
    ViewBase();
    sLayerExclude = 63u & ~16u;
    sObjMaskOn = true;
    memset(sObjMask, 0, sizeof(sObjMask));
    sObjMask[sLoose[l].index >> 5] = 1u << (sLoose[l].index & 31);
    sOffX = offX + BattleSpriteShift(sLoose[l].centre);
    Compose();
    C2D_Flush();
    sObjMaskOn = false;
    memcpy(sObjMask, mask, sizeof(mask));
    sLayerExclude = exclude;
    sOffX = offX;
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    ClipToView();
    BlendForget();
}

/*
 * Each region's skyline: runs of columns and how far down each is taken.
 * A slot goes where it lies highest, the leftmost of those; it covers the
 * runs under it at its bottom. One slot adds two runs at most.
 */
typedef struct
{
    int16_t x, w, y;
} SlotRun;
static SlotRun sRuns[2][2 * 128 + 2];
static unsigned sRunCount[2];

/* Where a tw x th slot lies highest in the region, or false. */
static bool SlotRunFind(unsigned region, int tw, int th, int *bestX, int *bestY)
{
    const SlotRun *runs = sRuns[region];
    unsigned count = sRunCount[region];
    int end = sRegions[region].end, height = sRegions[region].tex->height;
    bool found = false;

    for (unsigned i = 0; i < count; ++i)
    {
        int x = runs[i].x, y = 0;

        if (x + tw > end) break;
        for (unsigned j = i; j < count && runs[j].x < x + tw; ++j)
            if (runs[j].y > y) y = runs[j].y;
        if (y + th > height) continue;
        if (!found || y < *bestY)
        {
            *bestX = x;
            *bestY = y;
            found = true;
        }
    }
    return found;
}

/* The runs under [x, x + tw) now down to y. */
static void SlotRunCover(unsigned region, int x, int tw, int y)
{
    SlotRun *runs = sRuns[region], kept[2 * 128 + 2];
    unsigned count = sRunCount[region], n = 0;

    for (unsigned i = 0; i < count; ++i)
    {
        int x0 = runs[i].x, x1 = runs[i].x + runs[i].w;

        if (x1 <= x || x0 >= x + tw) { kept[n++] = runs[i]; continue; }
        if (x0 < x) kept[n++] = (SlotRun){(int16_t)x0, (int16_t)(x - x0), runs[i].y};
        if (x0 <= x) kept[n++] = (SlotRun){(int16_t)x, (int16_t)tw, (int16_t)y};
        if (x1 > x + tw) kept[n++] = (SlotRun){(int16_t)(x + tw), (int16_t)(x1 - x - tw), runs[i].y};
    }
    /* Next to each other at the same depth: one run. */
    count = 0;
    for (unsigned i = 0; i < n; ++i)
    {
        if (count && runs[count - 1].y == kept[i].y && runs[count - 1].x + runs[count - 1].w == kept[i].x)
            runs[count - 1].w = (int16_t)(runs[count - 1].w + kept[i].w);
        else
            runs[count++] = kept[i];
    }
    sRunCount[region] = count;
}

/*
 * The slots, tallest first, each into the first region with room for it,
 * as high up as it goes there (the skyline above). Shelves the width of the
 * region needed 652 columns for the second battle's opening - a 128-pixel
 * Pokémon, a 64-pixel one and an info box - where the world beside them
 * leaves 616: the box fits under the smaller Pokémon. Then ordered by
 * region, the order they are composed in (SlotsCompose).
 */
/* One attempt at placing the slots, in `order`. */
static bool SlotsPackIn(const uint8_t *order)
{
    for (unsigned r = 0; r < sRegionCount; ++r)
    {
        sRuns[r][0] = (SlotRun){(int16_t)sRegions[r].column, (int16_t)(sRegions[r].end - sRegions[r].column), 0};
        sRunCount[r] = sRegions[r].end > sRegions[r].column;
    }
    for (unsigned k = 0; k < sSlotCount; ++k)
    {
        BattleSlot *slot = &sSlots[order[k]];
        int tw = (slot->w + 2) * (int)SCENE_ZOOM, th = (slot->h + 2) * (int)SCENE_ZOOM;
        unsigned region = 0;
        int x = 0, y = 0;

        while (region < sRegionCount && !SlotRunFind(region, tw, th, &x, &y)) ++region;
        if (region == sRegionCount) return false;
        SlotRunCover(region, x, tw, y + th);
        slot->region = (uint8_t)region;
        slot->sx = (int16_t)x;
        slot->sy = (int16_t)y;
    }
    return true;
}

/*
 * The slots, tallest first, each into the first region with room for it,
 * as high up as it goes there (the skyline above). Shelves the width of the
 * region needed 652 columns for the second battle's opening - a 128-pixel
 * Pokémon, a 64-pixel one and an info box - where the world beside them
 * leaves 616: the box fits under the smaller Pokémon. Tallest first left
 * Surf over the voxel world without room - two Pokemon and two info boxes
 * 128 wide in the 512 columns of the slots' own surface, which hold them
 * with the boxes stacked - so the widest first is tried next, then the
 * largest. Then ordered by region, the order they are composed in
 * (SlotsCompose).
 */
static bool SlotsPack(void)
{
    uint8_t order[128];
    bool packed = false;
    unsigned all = sRegionCount;
    /* In the 2D battle, the slots' own surface alone first: in the scene
     * surface's free columns they need it composed again (RenderBattleStereo). */
    bool own = all > 1 && sRegions[0].target != sScene && sRegions[all - 1].target == sScene;

    for (unsigned attempt = 0; attempt < 6 && !packed; ++attempt)
    {
        unsigned key = attempt % 3;

        if (attempt < 3 && !own) continue;
        sRegionCount = attempt < 3 ? all - 1 : all;
        for (unsigned i = 0; i < sSlotCount; ++i)
        {
            unsigned j = i;
            int mine = key == 0 ? sSlots[i].h : key == 1 ? sSlots[i].w : sSlots[i].w * sSlots[i].h;

            while (j > 0)
            {
                const BattleSlot *before = &sSlots[order[j - 1]];
                int theirs = key == 0 ? before->h : key == 1 ? before->w : before->w * before->h;

                if (theirs >= mine) break;
                order[j] = order[j - 1];
                --j;
            }
            order[j] = (uint8_t)i;
        }
        packed = SlotsPackIn(order);
    }
    sRegionCount = all;
    if (!packed) return false;
    for (unsigned i = 1; i < sSlotCount; ++i)
    {
        BattleSlot slot = sSlots[i];
        unsigned j = i;

        while (j > 0 && sSlots[j - 1].region > slot.region)
        {
            sSlots[j] = sSlots[j - 1];
            --j;
        }
        sSlots[j] = slot;
    }
    /*
     * Back to front as the GBA layers sprites: the furthest priority first,
     * and within one the higher OAM entries, which lower ones cover. Groups
     * only overlap when one would have been too big; the patches of
     * backgrounds come in their own pass (SlotsPlace).
     */
    for (unsigned i = 0; i < sSlotCount; ++i)
    {
        unsigned j = i;
        const BattleSlot *slot = &sSlots[i];

        while (j > 0)
        {
            const BattleSlot *before = &sSlots[sPlaceOrder[j - 1]];

            if (before->furthest > slot->furthest
                || (before->furthest == slot->furthest && before->first >= slot->first))
                break;
            sPlaceOrder[j] = sPlaceOrder[j - 1];
            --j;
        }
        sPlaceOrder[j] = (uint8_t)i;
    }
    return true;
}

/* A rectangle of a target set to one colour by drawing it, not clearing:
 * no command split, which with the GPU started early costs a cache flush. */
static void DrawnClear(C3D_RenderTarget *target, uint32_t color, float x, float y, float w, float h)
{
    BlendForget();
    SceneOn(target);
    C2D_ViewReset();
    C2D_Flush();
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C2D_DrawRectSolid(x, y, 0, w, h, color);
    C2D_Flush();
    BlendForget();
}

/* Every slot's sprite into it, undisplaced. */
static void SlotsCompose(void)
{
    float zoom = sZoom, offX = sOffX, offY = sOffY;

    sZoom = SCENE_ZOOM;
    sLayerExclude = 63 & ~(16u | 32u);
    for (unsigned k = 0; k < sSlotCount; ++k)
    {
        const BattleSlot *slot = &sSlots[k];
        const SlotRegion *region = &sRegions[slot->region];
        int tw = (slot->w + 2) * (int)SCENE_ZOOM, th = (slot->h + 2) * (int)SCENE_ZOOM;

        /* A region cleared before its first slot (SlotsPack orders them). */
        if (k == 0 || slot->region != sSlots[k - 1].region)
        {
            C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
            DrawnClear(region->target, 0, region->column, 0, region->end - region->column,
                       region->tex->height);
            SceneOn(region->target);
            sTargetW = region->tex->width;
            sTargetH = region->tex->height;
            sSurfaceH = region->tex->height;
        }

        /* GBA (x - 1, y - 1) on the slot's corner. */
        sOffX = slot->sx - (slot->x - 1) * SCENE_ZOOM;
        sOffY = slot->sy - (slot->y - 1) * SCENE_ZOOM;
        BlendForget();
        Blend(5, false, false);
        sClipX0 = slot->x;
        sClipY0 = slot->y;
        sClipX1 = slot->x + slot->w;
        sClipY1 = slot->y + slot->h;
        C2D_Flush();
        C3D_SetScissor(GPU_SCISSOR_NORMAL, (unsigned)slot->sx, (unsigned)(sSurfaceH - slot->sy - th),
                       (unsigned)(slot->sx + tw), (unsigned)(sSurfaceH - slot->sy));
        memcpy(sObjMask, slot->members, sizeof(sObjMask));
        sObjMaskOn = !slot->patch;
        sLayerExclude = slot->patch ? 63u & ~(slot->patch | 32u) : 63u & ~(16u | 32u);
        /* Through windows 0 and 1, each part of the slot with what its
         * window shows (ComposeBand scissors inside the slot's box). */
        Compose();
        sObjMaskOn = false;
        C2D_Flush();
        if (slot->blendBg && !slot->patch)
        {
            /* The layers alone, with the windows' colour effects bit:
             * without it the layer was drawn as it is over the sprites -
             * the sandstorm's sand filling them (port.log 2026-10-28). */
            sLayerExclude = 63u & ~(slot->blendBg | 32u);
            for (unsigned pass = SLOT_BLEND_UNDER; pass <= SLOT_BLEND_OVER; ++pass)
            {
                /* The slot's box again: Compose leaves the clip at the whole
                 * target, and the layer went over the slot beside this one
                 * in the same surface - twice blended there, Surf's wave
                 * came out as a pale band over the other Pokemon. */
                sClipX0 = slot->x;
                sClipY0 = slot->y;
                sClipX1 = slot->x + slot->w;
                sClipY1 = slot->y + slot->h;
                C3D_SetScissor(GPU_SCISSOR_NORMAL, (unsigned)slot->sx, (unsigned)(sSurfaceH - slot->sy - th),
                               (unsigned)(slot->sx + tw), (unsigned)(sSurfaceH - slot->sy));
                sSlotBlendPass = pass;
                BlendForget();
                Compose();
                C2D_Flush();
            }
            sSlotBlendPass = SLOT_BLEND_NONE;
            BlendForget();
        }
    }
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    sLayerExclude = 0;
    sZoom = zoom;
    sOffX = offX;
    sOffY = offY;
    sTargetW = CTR_GAME_WIDTH;
    sTargetH = CTR_GAME_HEIGHT;
    sSurfaceH = 256;
    ClipToView();
    BlendForget();
}

/* The slots on `target` for the eye being drawn, each at its group's depth:
 * those behind the backgrounds in front of sprites, or those before them. */
static void SlotsPlaceBegin(C3D_RenderTarget *target)
{
    BlendForget();
    SceneOn(target);
    C2D_ViewReset();
    Blend(5, false, false);
    C2D_Flush();
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
}

static void SlotsPlace(C3D_RenderTarget *target, bool front, bool patches)
{
    float scale = sZoom / SCENE_ZOOM;
    /* The loose sprites, among the groups behind the backgrounds in front. */
    bool loose = !front && !patches && sLooseCount;
    unsigned next = 0;

    BlendForget();
    SceneOn(target);
    C2D_ViewReset();
    Blend(5, false, false);
    /*
     * Premultiplied: a slot's clear texels are 0 in colour as in alpha, so
     * where the bilinear draw takes half a sprite's edge texel and half a
     * clear one, it brings half the colour at half the coverage. Weighed by
     * its alpha again, that half colour came out darkened, a dark outline
     * round every sprite that the 2D battle, filtering sprite and scenery
     * together, never had. The alpha is kept whole for the same reason.
     */
    C2D_Flush();
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
    for (unsigned k = 0; k < sSlotCount; ++k)
    {
        const BattleSlot *slot = &sSlots[sPlaceOrder[k]];
        int tw = (slot->w + 2) * (int)SCENE_ZOOM, th = (slot->h + 2) * (int)SCENE_ZOOM;
        C3D_Tex *tex = sRegions[slot->region].tex;

        if (slot->front != front || (slot->patch != 0) != patches) continue;
        /* Those behind the group's frontmost sprite before it. */
        if (loose && next < sLooseCount)
        {
            unsigned key = (unsigned)slot->nearest << 8 | slot->first;
            bool drew = false;

            while (next < sLooseCount && ((unsigned)sLoose[next].priority << 8 | sLoose[next].index) > key)
            {
                C2D_Flush();
                LooseDraw(target, next++);
                drew = true;
            }
            if (drew) SlotsPlaceBegin(target);
        }
        const Tex3DS_SubTexture cut = {(u16)tw, (u16)th, slot->sx / (float)tex->width,
            1 - slot->sy / (float)tex->height, (slot->sx + tw) / (float)tex->width,
            1 - (slot->sy + th) / (float)tex->height};

        C2D_DrawImageAt((C2D_Image){tex, &cut},
                        (slot->x - 1) * sZoom + sOffX
                            + (slot->patch ? -sEyeParallax * BATTLE_BACK_DEPTH : BattleSpriteShift(slot->centre)),
                        (slot->y - 1) * sZoom + sOffY, 0, NULL, scale, scale);
    }
    C2D_Flush();
    while (loose && next < sLooseCount)
        LooseDraw(target, next++);
    BlendForget();
}

/*
 * The voxel battle's world for one eye: its finished picture (`world`,
 * RenderBattleWorld) copied - not blended: the blur, glow and shadows left
 * its alpha below one, which blended came out as black shadows and a dark
 * band - BATTLE_BACK_DEPTH behind the screen in whole pixels, its edge
 * columns as they are under where the displaced edge leaves none.
 */
static void BattleWorldEye(C3D_RenderTarget *target, C3D_Tex *world)
{
    const Tex3DS_SubTexture whole = {CTR_GAME_WIDTH, CTR_GAME_HEIGHT, 0, 1,
        CTR_GAME_WIDTH / (float)world->width, 1 - CTR_GAME_HEIGHT / (float)world->height};
    /* Stretched across by the shift, so that both edges stay covered: the
     * strip the shift uncovered used to be filled with the picture's edge
     * columns, which stood out as a line down the screen's side whenever
     * the picture's edge held something dark (a map's end, a wall). A few
     * per cent across, nothing at the middle where the Pokémon stand. */
    float shift = roundf(-sEyeParallax * BATTLE_BACK_DEPTH), reach = fabsf(shift);
    float scaleX = (CTR_GAME_WIDTH + 2.0f * reach) / CTR_GAME_WIDTH;

    BlendForget();
    SceneOn(target);
    C2D_ViewReset();
    C2D_Flush();
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    /* Nearest: the picture stays pixel-sharp, a column doubled now and then. */
    if (world == &sSceneTex) SceneTexFilter(GPU_NEAREST);
    C2D_DrawImageAt((C2D_Image){world, &whole}, shift - reach, 0, 0, NULL, scaleX, 1);
    if (world == &sSceneTex) SceneTexFilter(GPU_LINEAR);
    C2D_Flush();
    BlendForget();
}

/* The text box (BG0) alone into the logical surface, cleared transparent. */
static void BattleTextCompose(bool drawnClear, int keepRows)
{
    sTextAbove = false;
    if (drawnClear) DrawnClear(sLogical, 0, 0, keepRows, 512, 256 - keepRows);
    else
    {
        BlendForget();
        ColourClear(sLogical, 0);
        sSceneOn = NULL;
    }
    SceneOn(sLogical);
    Blend(5, false, false);
    sLayerExclude = 63 & ~(1u | 32u | BattleBoxLayers());
    if (!(Reg(0) & 128)) Compose();
    sLayerExclude = 0;
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
}

/*
 * Both eyes of a battle (see BATTLE_BACK_DEPTH). `world` is the voxel
 * battle's finished world, NULL for the 2D battle.
 */
/* The stereo battle's scene composed whole, its sprites at the scenery's depth. */
static bool sHudFlatScene;
/* The last 2D stereo frame was composed whole (RenderBattleStereo). */
static bool sFlatBefore;

static void RenderBattleStereo(uint32_t clear, float slider, C3D_Tex *world)
{
    const Tex3DS_SubTexture logical = {CTR_GAME_WIDTH, CTR_GAME_HEIGHT, 0, 1,
        CTR_GAME_WIDTH / 512.0f, 1 - CTR_GAME_HEIGHT / 256.0f};
    unsigned control = Reg(0x50), furthest;
    bool apart, any, scenery = true;
    /* The scenery composed into each eye rather than placed from the scene
     * surface, whose columns the slots have then (below). */
    bool sceneryEye = false;
    /* The text box once, unless it blends with the scene under it. */
    bool textOnce = !(((control >> 6) & 3) == 1 && (control & 1));

    /* Not for the battle over the voxel world, which shares the scene surface
     * on its few frames here (a move's background, its last frame): made then,
     * the slots took the field's atlases with them, and the field came back
     * in 2D. Nor for a frame showing no sprite. */
    if (!world && (Reg(0) & 0x1000) && !CtrVoxel_InBattle()) sSlotWanted = true;
    PORT_PROF_BEGIN(bsPrep);

    sSceneOn = NULL;
    sParallax = 0;
    sLayerShift = 0;
    sHudFlatScene = false;
    sSceneKeptNow = false;
    /* The scenery and the slots past the world's picture (RenderBattleWorld). */
    sSceneColumn = world ? WORLD_COLUMNS + WORLD_GAP : 0;
    BlendForget();
    /* The scene's placement first: the sprites are clipped to it. */
    sObjWinSlotted = sObjWin.ready;
    /*
     * A 2D frame that cannot be split (below) composes the scene again with
     * its sprites: Surf's wave in front of the Pokemon, every frame of the
     * move, the scene twice. After a frame like that the next is taken to be
     * one too and composed whole at once - what decides it, the layers drawn
     * and the sprites, is the same either way - and only composed again
     * without its sprites if it can be split after all. Never kept: the
     * cache does not look at the sprites.
     */
    bool whole = !world && sFlatBefore && !sObjWin.ready;
#ifdef CTR_FRAME_DUMP
    if (CtrDumpOff() & 8) whole = false;
#endif

    sSceneCacheAllowed = !whole;
    BattleSceneCompose(BATTLE_STEREO_MARGIN, whole ? 0u : 16u);
    sSceneCacheAllowed = false;
    HudCoverUpdate();
    any = BattleSprites(&furthest, &apart);
    if (sObjWinSlotted)
    {
        /* The group the window's box is inside of, as far as it shows; with
         * none, all composed whole as before, the window with the scenery. */
        BattleSlot *under = NULL;
        int wx0 = sObjWin.x0, wy0 = sObjWin.y0, wx1 = sObjWin.x1, wy1 = sObjWin.y1;
        int top = (int)lroundf((sScenePut.oy - sOffY) / sZoom);

        if (wy0 < top) wy0 = top;
        if (wy1 > BATTLE_SCENE_BOTTOM) wy1 = BATTLE_SCENE_BOTTOM;
        for (unsigned k = 0; k < sSlotCount && wx0 < wx1 && wy0 < wy1; ++k)
        {
            BattleSlot *slot = &sSlots[k];

            if (wx0 >= slot->x && wy0 >= slot->y && wx1 <= slot->x + slot->w && wy1 <= slot->y + slot->h)
            {
                under = slot;
                break;
            }
        }
        /* Drawn over each eye whole (ObjWindowEye): not through windows 0 and
         * 1 where they cut its box. Merely on is not enough to refuse it: a
         * stat change sets WIN0 to nothing (WIN0H = WIN0V = 0) with DISPCNT
         * keeping it on through the battle (0xbf40), and its 59 frames a
         * battle were composed whole, the Pokemon flat, for no window. */
        bool cut = false;

        for (unsigned w = 0; w < 2 && under; ++w)
            if (Reg(0) & (0x2000u << w))
            {
                int x0, x1, y0, y1;

                WindowRect(w, &x0, &x1, &y0, &y1);
                if (x0 < x1 && y0 < y1 && x0 < wx1 && wx0 < x1 && y0 < wy1 && wy0 < y1) cut = true;
            }
        if (under && !cut) under->objWin = true;
        else
        {
            if (apart) { StereoWhy(0); BlendWhy(under ? 3 : 2); }
            apart = false;
            sObjWinSlotted = false;
        }
    }
    /*
     * Backgrounds in front of a sprite - the intro's entry picture over the
     * Pokemon sliding in, a move's background over its target. The scenery
     * has them where they are; over each group behind them they are composed
     * again, a patch of them alone as wide as the group can be displaced, and
     * drawn at the scenery's depth after those groups, before the groups in
     * front of them. A group with such a background between its own sprites,
     * or one that blends with what is under it, cannot be split so.
     */
    unsigned front = 0, nearestFront = 3, furthestFront = 0;
    /*
     * Those that blend with the sprites under them (the wild battle's
     * opening, BattleIntroSlide3; a move's background): blended into each
     * group's slot behind them (SlotsCompose), at the group's depth, rather
     * than a patch over it - 108 frames of a wild battle's opening were
     * composed whole, the Pokemon flat. Only with the sprites a second target
     * and no palette fade (the passes have the tint to themselves); the
     * scenery has them blended already, outside the sprites.
     */
    unsigned blendFront = 0;

    for (unsigned bg = 1; bg < 4 && any; ++bg)
        if (sBgDrawn[bg] && (Reg(8 + bg * 2) & 3) < furthest)
        {
            unsigned priority = Reg(8 + bg * 2) & 3;

            front |= 1u << bg;
            if (priority < nearestFront) nearestFront = priority;
            if (priority > furthestFront) furthestFront = priority;
            if (((control >> 6) & 3) == 1 && (control & (1u << bg)) && apart)
            {
                if ((control & 0x1000) && sPaletteFade <= 0.0f && !(sObjWin.ready && (sObjWin.layers & (1u << bg))))
                    blendFront |= 1u << bg;
                else { StereoWhy(0); BlendWhy(4); apart = false; }
            }
        }
    /* Loose sprites with a background in front of sprites: their place
     * among the patches is not worked out; composed whole. */
    if (sLooseCount && front && apart) { StereoWhy(0); BlendWhy(0); apart = false; }
    /* Both kinds over the same group, in either order, are not kept apart:
     * a patch drawn over the blended slot would cover the blend. */
    if (blendFront && (front & ~blendFront) && apart) { StereoWhy(0); BlendWhy(4); apart = false; }
    for (unsigned k = 0, groups = sSlotCount; front && apart && k < groups; ++k)
    {
        BattleSlot *slot = &sSlots[k];
        int reach = (int)ceilf(BATTLE_BACK_DEPTH / sShiftZoom) + 1;

        if (slot->furthest <= nearestFront) { slot->front = true; continue; }
        if (slot->nearest <= furthestFront) { StereoWhy(2); apart = false; break; }
        if (blendFront) { slot->blendBg = (uint8_t)blendFront; continue; }
        if (sSlotCount == sizeof(sSlots) / sizeof(sSlots[0])) { StereoWhy(3); apart = false; break; }
        BattleSlot *patch = &sSlots[sSlotCount++];

        *patch = *slot;
        patch->patch = (uint8_t)front;
        patch->blendBg = 0;
        patch->objWin = false;
        patch->x = (int16_t)(slot->x - reach);
        patch->w = (int16_t)(slot->w + 2 * reach);
    }
    if (world && !sBgDrawn[1] && !sBgDrawn[2] && !sBgDrawn[3]) scenery = false;
    if (apart && any)
    {
        int after = (sScenePut.width + 7) & ~7;

        sRegionCount = 0;
        bool lent = false;

        if (scenery && !world && sSlotTarget)
            sRegions[sRegionCount++] = (SlotRegion){sSlotTarget, &sSlotTex, 0, sSlotTex.width};
        sRegions[sRegionCount++] = (SlotRegion){sScene, &sSceneTex, sSceneColumn + (scenery ? after : 0), SCENE_W};
        /* Over the world, the scene surface first: its own surface only for
         * what does not fit beside the world's picture. */
        if (world && sSlotTarget)
            sRegions[sRegionCount++] = (SlotRegion){sSlotTarget, &sSlotTex, 0, sSlotTex.width};
        if (!scenery) sSceneCached = false;   /* the slots go over its columns */
        if (!SlotsPack())
        {
            /*
             * Over the world with a move's background (Surf's wave) and no
             * surface of their own - on a map of five atlases nothing was
             * free for it (port.log 2026-10-28: 194 KiB) - the scenery took
             * 584 of the 616 columns past the world's picture, and every
             * frame of the move went whole per eye, 24 ms on an Old 3DS.
             * The slots take the scenery's columns then, and the scenery is
             * composed into each eye at the screen's scale, as the edges are:
             * into the logical surface, each eye's picture put together there
             * (textOnce off), where the scissor's rows are (Scissor).
             */
            bool packed = false;

            if (world && scenery)
            {
                sRegions[0].column = sSceneColumn;
                packed = SlotsPack();
            }
            if (packed)
            {
                sceneryEye = true;
                textOnce = false;
                sSceneCached = false;
                lent = true;
            }
            else { StereoWhy(3); apart = false; }
        }
        for (unsigned k = 0; apart && world && sSlotTarget && k < sSlotCount; ++k)
            if (sRegions[sSlots[k].region].target == sSlotTarget) lent = true;

        /* No room beside the world (a double battle's four Pokemon, a move's
         * background as well): a surface of their own from the next frame
         * on (sSlotWorld), kept while such frames go on. */
        if (world && (!apart || lent))
        {
            sSlotWorld = true;
            sSlotWanted = true;
        }
    }
    sFlatBefore = !apart && !world;
    if (whole && apart)
    {
        sSceneCacheAllowed = true;
        BattleSceneCompose(BATTLE_STEREO_MARGIN, 16u);
        sSceneCacheAllowed = false;
    }
    /*
     * Slots in the kept scene surface's free columns: cleared there by
     * drawing, which on hardware did not always land (RenderBattleWorld),
     * where the scenery's own composing clears the whole surface as a
     * memory fill. Composed again then, as every frame was before.
     */
    for (unsigned k = 0; apart && any && !world && sSceneKeptNow && k < sSlotCount; ++k)
        if (sRegions[sSlots[k].region].target == sScene)
        {
            sSceneCached = false;
            sSceneCacheAllowed = true;
            BattleSceneCompose(BATTLE_STEREO_MARGIN, 16u);
            sSceneCacheAllowed = false;
            sSceneKeptNow = false;
            break;
        }
    GpuStartEarly();
    ++sStereoSplit[apart ? 0 : 1];
    /* Composed whole below, the window with the rest. */
    if (!apart) sObjWinSlotted = false;
    PORT_PROF_END(bsPrep, PORT_PROF_BS_PREP);

    if (!apart && !world)
    {
        /*
         * The 2D battle that cannot be split: the whole scene, sprites and
         * all, composed once and put behind the screen at the scenery's depth
         * in each eye, the text box over it as below. Composed per eye, with
         * each sprite at its depth, it was the scene and the text box twice a
         * frame - 15-18 ms of drawing, 30 fps on an Old 3DS for the second a
         * stat change's animation lasts. The sprites lose their depth for as
         * long as it lasts; the scenery keeps its.
         */
        if (!whole) BattleSceneCompose(BATTLE_STEREO_MARGIN, 0);
        any = false;
        front = 0;
        scenery = true;
        sHudFlatScene = true;
    }
    else if (!apart)
    {
        PORT_PROF_BEGIN(bsWhole);

        /* Whole per eye, as RenderEye composes it, at the same depths. */
        for (int eye = 0; eye < 2; ++eye)
        {
            C3D_RenderTarget *target = eye ? sTopRight : sTop;

            sEyeParallax = eye ? -slider : slider;
            {
                BlendForget();
                ColourClear(sLogical, 0);
                RenderBattleScene(0);
                SceneOn(sLogical);
                Blend(5, false, false);
                HudDrawCrisp(HUD_SPRITE_DEPTH);
                sLayerExclude = 63 & ~(1u | 32u | BattleBoxLayers());
                if (!(Reg(0) & 128)) Compose();
                sLayerExclude = 0;
                C2D_Flush();
                C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
                C3D_FrameSplit(0);
                BattleWorldEye(target, world);
                Blend(5, false, false);
                C2D_DrawImageAt((C2D_Image){&sSurface, &logical}, 0, 0, 0, NULL, 1, 1);
                C2D_Flush();
                C3D_FrameSplit(0);
            }
        }
        sEyeParallax = 0;
        PORT_PROF_END(bsWhole, PORT_PROF_BS_WHOLE);
        return;
    }

    /* Everything both eyes take, in one command list. */
    sSceneOn = NULL;
    {
        PORT_PROF_BEGIN(bsSlots);
        if (any && !(Reg(0) & 128)) SlotsCompose();
        PORT_PROF_END(bsSlots, PORT_PROF_BS_SLOTS);
    }
    {
        PORT_PROF_BEGIN(bsText);
        bool keep = textOnce && scenery && BattleEdgesKeep(!any);

        sEdgeStore.on = false;
        if (textOnce) BattleTextCompose(true, keep ? BATTLE_EDGE_STORE_END : 0);
        /* The edges for both eyes, after the text box's clear. */
        if (textOnce && scenery)
        {
            float origin = sLayerOrigin;

            sLayerOrigin = 0.0f;
            BattleEdgesStore(!any, keep);
            sLayerOrigin = origin;
        }
        else sEdgeStore.held = false;
        SplitFrame();
        PORT_PROF_END(bsText, PORT_PROF_BS_TEXT);
    }
    PORT_PROF_BEGIN(bsEyes);
    for (int eye = 0; eye < 2; ++eye)
    {
        C3D_RenderTarget *target = eye ? sTopRight : sTop;
        /* Where the eye's picture is put together: the screen, or with the
         * text box composed over it, the logical surface. */
        C3D_RenderTarget *picture = textOnce ? target : sLogical;

        sEyeParallax = eye ? -slider : slider;
        if (world && textOnce) BattleWorldEye(target, world);
        else DrawnClear(picture, world ? 0 : clear, 0, 0, CTR_GAME_WIDTH, CTR_GAME_HEIGHT);
        if (scenery)
        {
            float origin = sLayerOrigin;

            /* The scenery alone, the sprites being the slots': unfiltered,
             * as the 2D field draws its layers; the slots keep the filter. */
            if (sceneryEye)
            {
                float offX = sOffX;
                int targetW = sTargetW;

                /* The view widened each side, the layers moved back by as
                 * much: some layers stop at the view's edge (VIEW_LEFT,
                 * VIEW_RIGHT) before they are moved by the eye's shift,
                 * which left the screen's edge columns of the far side
                 * uncovered, the world showing through. */
                sOffX += BATTLE_EDGE_MARGIN;
                sTargetW += 2 * BATTLE_EDGE_MARGIN;
                sLayerOrigin = BattleSceneryShift() - BATTLE_EDGE_MARGIN;
                SceneOn(picture);
                C2D_ViewReset();
                BattleRectCompose(VIEW_LEFT, sScenePut.top, VIEW_RIGHT, BATTLE_SCENE_BOTTOM, 16u);
                C2D_Flush();
                C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
                sScissored = false;
                sOffX = offX;
                sTargetW = targetW;
                sLayerOrigin = origin;
            }
            else
            {
                SceneTexFilter(GPU_NEAREST);
                BattleScenePlace(picture, -sEyeParallax * BATTLE_BACK_DEPTH);
                SceneTexFilter(GPU_LINEAR);
            }
            /* The lines past the surface at the scenery's depth; the
             * sprites are the slots', at their own, unless they are in the
             * scene composed whole. Composed once for both eyes; with the
             * eye's picture in the logical surface, into it per eye. */
            if (textOnce)
                BattleEdgesPlace(picture, BattleSceneryShift());
            else
            {
                /* Widened as the scenery above, moved back by as much. */
                sLayerOrigin = BattleSceneryShift() - BATTLE_EDGE_MARGIN;
                SceneOn(sLogical);
                BattleEdgesCompose(!any, BATTLE_EDGE_MARGIN, 0);
                sLayerOrigin = origin;
            }
        }
        if (any && !(Reg(0) & 128))
        {
            SlotsPlace(picture, false, false);
            if (front)
            {
                SlotsPlace(picture, false, true);
                SlotsPlace(picture, true, false);
            }
            if (sObjWinSlotted)
                for (unsigned k = 0; k < sSlotCount; ++k)
                    if (sSlots[k].objWin)
                    {
                        ObjWindowEye(picture, sSlots[k].centre);
                        break;
                    }
        }
        /* The info boxes over them at the screen's pixels: at their sprites'
         * depth, or the scenery's when the sprites are composed with it. */
        if (!(Reg(0) & 128))
        {
            SceneOn(picture);
            HudDrawCrisp(sHudFlatScene ? HUD_SCENERY_DEPTH : HUD_SPRITE_DEPTH);
        }
        if (!textOnce)
        {
            SceneOn(sLogical);
            Blend(5, false, false);
            sLayerExclude = 63 & ~(1u | 32u | BattleBoxLayers());
            if (!(Reg(0) & 128)) Compose();
            sLayerExclude = 0;
            C2D_Flush();
            C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
            SplitFrame();
            if (world) BattleWorldEye(target, world);
            else DrawnClear(target, C2D_Color32(0, 0, 0, 255), 0, 0, CTR_GAME_WIDTH, CTR_GAME_HEIGHT);
        }
        BlendForget();
        SceneOn(target);
        C2D_ViewReset();
        Blend(5, false, false);
        /* Only the text box's lines when nothing of BG0 is above it. */
        if (textOnce && !sTextAbove)
        {
            const float top = BATTLE_BAND_TOP * sZoom + sOffY;
            const Tex3DS_SubTexture band = {CTR_GAME_WIDTH, (u16)(CTR_GAME_HEIGHT - top), 0, 1 - top / 256.0f,
                CTR_GAME_WIDTH / 512.0f, 1 - CTR_GAME_HEIGHT / 256.0f};

            C2D_DrawImageAt((C2D_Image){&sSurface, &band}, 0, top, 0, NULL, 1, 1);
        }
        else if (sEdgeStore.on)
        {
            /* Not the rows the edges are stored in (BattleEdgesStore). */
            const float top = BATTLE_EDGE_STORE_END;
            const Tex3DS_SubTexture text = {CTR_GAME_WIDTH, (u16)(CTR_GAME_HEIGHT - top), 0, 1 - top / 256.0f,
                CTR_GAME_WIDTH / 512.0f, 1 - CTR_GAME_HEIGHT / 256.0f};

            C2D_DrawImageAt((C2D_Image){&sSurface, &text}, 0, top, 0, NULL, 1, 1);
        }
        else
            C2D_DrawImageAt((C2D_Image){&sSurface, &logical}, 0, 0, 0, NULL, 1, 1);
        C2D_Flush();
    }
    sEdgeStore.on = false;
    sSceneComposed = false;
    sEyeParallax = 0;
    sObjWinSlotted = false;
    PORT_PROF_END(bsEyes, PORT_PROF_BS_EYES);
}

/* Seals a command list. Before the GPU is started early it is submitted at
 * FrameEnd, whose linear-heap flush covers all producers, Citro2D included;
 * after, the GX wrappers flush before it is queued. */
static void GpuSplit(void)
{
    C3D_FrameSplit(0);
}

/*
 * HD-2D diorama: the tilt-shift of a miniature. The camera always looks north,
 * so the top of the screen is the distance and the bottom the nearest ground;
 * both are blurred, most at the screen's edge and not at all by the focus band
 * around the player. Each band is two copies of the world surface drawn over
 * it with bilinear filtering half a texel off either way, so each copy is the
 * average of four texels and the two together are centred on the pixel;
 * fading in from the focus band to the edge. A wider
 * blur than the four one-pixel taps it replaces, for about the same fill.
 * The surface is sampled NEAREST again afterwards: the picture itself stays
 * pixel-sharp.
 */
#define DIORAMA_TOP 100    /* rows blurred at the top */
#define DIORAMA_BOTTOM 56  /* ... and at the bottom */

static void DioramaTap(int y0, int rows, bool top, float dx, float dy, float alpha)
{
    /* One column short at each side: the surface past the picture is not
     * part of it. */
    const Tex3DS_SubTexture region = {CTR_GAME_WIDTH - 2, (u16)rows,
        (1.0f + dx) / 512.0f, 1.0f - (y0 + dy) / 256.0f,
        (1.0f + dx + CTR_GAME_WIDTH - 2) / 512.0f, 1.0f - (y0 + dy + rows) / 256.0f};
    C2D_ImageTint tint;

    C2D_AlphaImageTint(&tint, 0.0f);
    if (top)
        C2D_TopImageTint(&tint, C2D_Color32f(1.0f, 1.0f, 1.0f, alpha), 0.0f);
    else
        C2D_BottomImageTint(&tint, C2D_Color32f(1.0f, 1.0f, 1.0f, alpha), 0.0f);
    C2D_DrawImageAt((C2D_Image){&sSurface, &region}, 1, y0, 0, &tint, 1, 1);
}

/* The strength chosen in OPTIONS: 1 is the full blur, the taps' own alphas. */
static float DioramaStrength(void)
{
    return CtrSettings_VoxelBlurStrength() / 100.0f;
}

static void VoxelDiorama(void)
{
    /* The swoop takes the blur off with the grade (CtrVoxel_SwoopNeutral). */
    float strength = DioramaStrength() * (1.0f - CtrVoxel_SwoopNeutral());

    if (strength <= 0.005f) return;

    SurfaceFilter(GPU_LINEAR);
    /* Half a texel either way: two 2x2 averages on either side of each
     * pixel, together centred on it - a blur that does not shift the picture. */
    DioramaTap(0, DIORAMA_TOP, true, 0.5f, 0.5f, 0.67f * strength);
    DioramaTap(0, DIORAMA_TOP, true, -0.5f, -0.5f, 0.50f * strength);
    DioramaTap(CTR_GAME_HEIGHT - DIORAMA_BOTTOM, DIORAMA_BOTTOM, false, 0.5f, 0.5f, 0.60f * strength);
    DioramaTap(CTR_GAME_HEIGHT - DIORAMA_BOTTOM, DIORAMA_BOTTOM, false, -0.5f, -0.5f, 0.45f * strength);
    SurfaceFilter(GPU_NEAREST);
}

/*
 * Bloom: the brightest parts of the picture - sunlit sand, white walls, water
 * catching the light - glow softly into their surroundings. The world surface
 * is drawn at a quarter of its size into a small target, keeping only what is
 * brighter than BLOOM_THRESHOLD (texture environment 4, which citro2d leaves
 * free), then stretched back over the screen with bilinear filtering and
 * added. Two passes: 6000 pixels, then one additive screen.
 */
#define BLOOM_W (CTR_GAME_WIDTH / 4)
#define BLOOM_H (CTR_GAME_HEIGHT / 4)
#define BLOOM_THRESHOLD 0.85f

static void VoxelBloomPrepare(void)
{
    const Tex3DS_SubTexture logical = {CTR_GAME_WIDTH, CTR_GAME_HEIGHT, 0, 1,
        CTR_GAME_WIDTH / 512.0f, 1 - CTR_GAME_HEIGHT / 256.0f};
    unsigned t = (unsigned)(BLOOM_THRESHOLD * 255.0f + 0.5f);
    C3D_TexEnv *env;

    C2D_TargetClear(sBloom, C2D_Color32(0, 0, 0, 255));
    SceneOn(sBloom);
    C2D_ViewReset();
    Blend(5, false, false);
    SurfaceFilter(GPU_LINEAR);
    /* Replace, not blend: the surface's alpha is not the picture's. */
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    /* (colour - threshold) x 4: black below it. */
    env = C3D_GetTexEnv(4);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_SUBTRACT);
    C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_4);
    C3D_TexEnvColor(env, 0xFF000000u | t << 16 | t << 8 | t);
    C2D_DrawImageAt((C2D_Image){&sSurface, &logical}, 0, 0, 0, NULL, 0.25f, 0.25f);
    C2D_Flush();
    C3D_TexEnvInit(C3D_GetTexEnv(4));
    SurfaceFilter(GPU_NEAREST);
    BlendForget();
}

static void VoxelBloomCompose(float strength)
{
    const Tex3DS_SubTexture region = {BLOOM_W, BLOOM_H, 0, 1,
        BLOOM_W / 128.0f, 1 - BLOOM_H / 64.0f};
    C2D_ImageTint tint;

    C2D_Flush();
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_ZERO, GPU_ONE);
    C2D_AlphaImageTint(&tint, strength);
    C2D_DrawImageAt((C2D_Image){&sBloomTex, &region}, 0, 0, 0, &tint, 4.0f, 4.0f);
    C2D_Flush();
    BlendForget();
}

/*
 * The game's own UI and weather over the voxel world. BG0 carries text; only
 * OAM entries tagged by the sprite sorter as weather are composed. Drawing all
 * OBJ here would duplicate the player and every NPC over their billboards.
 */

/*
 * The dark of a cave: a soft ring of shadow closing in round the player, over
 * the world and under the game's text. One textured quad (CtrVoxel_Gloom).
 */
static void VoxelGloom(void)
{
    float x, y, size, amount;
    const C3D_Tex *tex = CtrVoxel_Gloom(&x, &y, &size, &amount);
    C2D_ImageTint tint;

    if (tex == NULL || amount <= 0.0f) return;
    const Tex3DS_SubTexture whole = {tex->width, tex->height, 0.0f, 1.0f, 1.0f, 0.0f};
    unsigned alpha = (unsigned)(amount * 255.0f + 0.5f);

    Blend(5, false, false);
    C2D_PlainImageTint(&tint, C2D_Color32(0, 0, 0, alpha > 255 ? 255 : alpha), 1.0f);
    C2D_DrawImageAt((C2D_Image){(C3D_Tex *)tex, &whole}, x - size * 0.5f, y - size * 0.5f, 0,
                    &tint, size / tex->width, size / tex->height);
    C2D_Flush();
    BlendForget();
}

/*
 * The circle of light of a dark cave in the voxel world: the same ground lit
 * as on the GBA - the game's radius, measured on the floor round the player,
 * so the camera's foreshortening makes it an ellipse - but with a soft edge
 * that warms to amber as it falls into the dark, a faint warm pool round the
 * player, a slow flicker, and a glow when Flash is used, while the circle
 * opens. Two textured quads and the dark round them (four rectangles): no
 * cost to speak of. Faded with the screen like everything the game fades.
 */
#define FLASH_LIGHT_DIM 64
#define FLASH_LIGHT_QUAD 1.25f  /* the quad's half size, in radii */
#define FLASH_LIGHT_INNER 0.82f /* where the edge starts to darken, in radii */
#define FLASH_LIGHT_OUTER 1.18f /* where it is dark, in radii */
#define FLASH_LIGHT_POOL 0.85f  /* how far the warm pool reaches, in radii */
static C3D_Tex sFlashDarkTex, sFlashPoolTex;
static bool sFlashTexTried, sFlashTexReady;

static void MakeFlashLight(void)
{
    uint32_t *dark;
    uint8_t *pool;
    const float inner = FLASH_LIGHT_INNER / FLASH_LIGHT_QUAD, outer = FLASH_LIGHT_OUTER / FLASH_LIGHT_QUAD;
    const float reach = FLASH_LIGHT_POOL / FLASH_LIGHT_QUAD;

    sFlashTexTried = true;
    if (!C3D_TexInit(&sFlashDarkTex, FLASH_LIGHT_DIM, FLASH_LIGHT_DIM, GPU_RGBA8))
        goto fail;
    if (!C3D_TexInit(&sFlashPoolTex, FLASH_LIGHT_DIM, FLASH_LIGHT_DIM, GPU_A8))
    {
        C3D_TexDelete(&sFlashDarkTex);
        goto fail;
    }
    dark = sFlashDarkTex.data;
    pool = sFlashPoolTex.data;
    for (unsigned y = 0; y < FLASH_LIGHT_DIM; ++y)
        for (unsigned x = 0; x < FLASH_LIGHT_DIM; ++x)
        {
            float dx = ((float)x + 0.5f) / (FLASH_LIGHT_DIM * 0.5f) - 1.0f;
            float dy = ((float)y + 0.5f) / (FLASH_LIGHT_DIM * 0.5f) - 1.0f;
            float d = sqrtf(dx * dx + dy * dy);
            float t = (d - inner) / (outer - inner), p = 1.0f - d / reach, warm;
            unsigned texel = CtrVideo_Texel(x, y, FLASH_LIGHT_DIM), a;

            t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
            t = t * t * (3.0f - 2.0f * t);
            /* Amber where the edge is half dark, black beyond: blended by
             * alpha, the rim gives about a fifth of amber at its middle. */
            warm = 0.72f * (1.0f - t);
            a = (unsigned)(t * 255.0f + 0.5f);
            dark[texel] = (uint32_t)(warm * 255.0f) << 24 | (uint32_t)(warm * 150.0f) << 16
                        | (uint32_t)(warm * 60.0f) << 8 | a;
            p = p < 0.0f ? 0.0f : p;
            pool[texel] = (uint8_t)(p * p * 255.0f + 0.5f);
        }
    C3D_TexSetFilter(&sFlashDarkTex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&sFlashDarkTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexFlush(&sFlashDarkTex);
    C3D_TexSetFilter(&sFlashPoolTex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&sFlashPoolTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexFlush(&sFlashPoolTex);
    sFlashTexReady = true;
    return;
fail:
    CtrLog_Write(CTR_LOG_VIDEO, "flash light textures not made (linear free=%lu)",
                 (unsigned long)linearSpaceFree());
}

static void VoxelFlashLight(void)
{
    const Tex3DS_SubTexture whole = {FLASH_LIGHT_DIM, FLASH_LIGHT_DIM, 0.0f, 1.0f, 1.0f, 0.0f};
    float x = CTR_GAME_WIDTH * 0.5f, y = CTR_GAME_HEIGHT * 0.5f, sx = 1.0f, sy = 1.0f;
    float t = (float)sFieldLight.ticks, radius, w, h, x0, y0, x1, y1, fade = sPaletteFade;
    uint32_t fadeTo = CtrVideo_RGBA8(sPaletteFadeColor, true);
    uint32_t dark = C2D_Color32((uint8_t)((fadeTo >> 24) * fade), (uint8_t)(((fadeTo >> 16) & 255) * fade),
                                (uint8_t)(((fadeTo >> 8) & 255) * fade), 255);
    C2D_ImageTint tint;

    if (!FieldLightShown()) return;
    if (!sFlashTexTried) MakeFlashLight();
    CtrVoxel_PlayerLightSpot(&x, &y, &sx, &sy);
    /* A flame's slow breathing: two slow waves, a pixel or two. */
    radius = sFieldLight.shown * (1.0f + 0.010f * sinf(t * 0.071f) + 0.006f * sinf(t * 0.173f + 1.3f));
    w = radius * FLASH_LIGHT_QUAD * sx;
    h = radius * FLASH_LIGHT_QUAD * sy;
    x0 = x - w; x1 = x + w;
    y0 = y - h; y1 = y + h;

    C2D_Flush();
    Blend(5, false, false);
    if (sFlashTexReady && radius > 0.5f)
    {
        float glow = (0.10f + 0.03f * sinf(t * 0.21f) + 0.55f * sFieldLight.burst) * (1.0f - fade);

        if (glow > 0.004f)
        {
            unsigned a = (unsigned)(glow * 255.0f + 0.5f);

            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_ZERO, GPU_ONE);
            C2D_PlainImageTint(&tint, C2D_Color32(255, 196, 128, a > 255 ? 255 : a), 1.0f);
            C2D_DrawImageAt((C2D_Image){&sFlashPoolTex, &whole}, x0, y0, 0, &tint,
                            2.0f * w / FLASH_LIGHT_DIM, 2.0f * h / FLASH_LIGHT_DIM);
            C2D_Flush();
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ZERO);
        }
        C2D_PlainImageTint(&tint, dark, fade);
        C2D_DrawImageAt((C2D_Image){&sFlashDarkTex, &whole}, x0, y0, 0, &tint,
                        2.0f * w / FLASH_LIGHT_DIM, 2.0f * h / FLASH_LIGHT_DIM);
    }
    else
        x0 = x1 = x, y0 = y1 = y;
    /* The dark round the quad. */
    if (y0 > 0.0f) C2D_DrawRectSolid(0, 0, 0, CTR_GAME_WIDTH, y0, dark);
    if (y1 < CTR_GAME_HEIGHT) C2D_DrawRectSolid(0, y1, 0, CTR_GAME_WIDTH, CTR_GAME_HEIGHT - y1, dark);
    if (x0 > 0.0f) C2D_DrawRectSolid(0, y0, 0, x0, y1 - y0, dark);
    if (x1 < CTR_GAME_WIDTH) C2D_DrawRectSolid(x1, y0, 0, CTR_GAME_WIDTH - x1, y1 - y0, dark);
    C2D_Flush();
    BlendForget();
}

static void ComposeVoxelOverlay(void)
{
    /* The text band and the prompts beside it are moved together, as the GBA
     * lays them out; a move's banner, which wraps over the whole width, and
     * the weather stay on the glass. */
    int eyeShift = sFieldBanner ? 0 : (int)CtrVoxel_OverlayParallax();

    sPriorityMask = SLOTS_ALL;
    sParallax = 0;
    sLayerOrigin = 0.0f;
    sLayerShift = 0.0f;
    sClipX0 = sClipY0 = 0;
    sClipX1 = CTR_GAME_WIDTH;
    sClipY1 = CTR_GAME_HEIGHT;
    /* A pass over all 128 sprites, four priorities each, costs even with
     * none of its sprites up (0.1 ms each in Azahar): only those with some. */
    bool weather = (sVoxelWeatherOam[0] | sVoxelWeatherOam[1] | sVoxelWeatherOam[2] | sVoxelWeatherOam[3]
                    | sFogOam[0] | sFogOam[1] | sFogOam[2] | sFogOam[3]) != 0;
    bool screen = ((sScreenOam[0] & ~sVoxelWeatherOam[0]) | (sScreenOam[1] & ~sVoxelWeatherOam[1])
                   | (sScreenOam[2] & ~sVoxelWeatherOam[2]) | (sScreenOam[3] & ~sVoxelWeatherOam[3])) != 0;

    if (sSwoopFieldObjs)
    {
        /* The object events as the 2D field draws them, under everything
         * else of the overlay (the swoop, with the cards squashed away). */
        sVoxelObjPass = VOXEL_OBJ_FIELD;
        Layers(1u << 4);
        BlendForget();
    }
    sVoxelObjPass = VOXEL_OBJ_WEATHER;
    /* With the blending: clouds and fog are semi-transparent sprites, and
     * drawn without it they hid the world under them. */
    if (weather) Layers(1u << 4 | 32u);
    BlendForget();
    /* Text windows and prompts must stay above the precipitation. */
    sLayerOrigin = (sFieldBanner ? 0.0f : CTR_FIELD_UI_SHIFT) + (float)eyeShift;
    Layers(1u << 0);
    sLayerOrigin = 0.0f;
    sOverlayEyeShift = eyeShift;
    sVoxelObjPass = VOXEL_OBJ_SCREEN;
    if (screen) Layers(1u << 4);
    sVoxelObjPass = VOXEL_OBJ_NONE;
    sOverlayEyeShift = 0;
}

/*
 * The finished world of the 3D battle for both eyes - the picture with its
 * blur and glow, composed once - which each eye then takes moved by the
 * scenery's depth (RenderBattleWorld, BattleWorldEye). Made when the slider
 * is up in such a battle and given back when it has not been used for a
 * while, or for a battle transition, which needs the VRAM (before the frame:
 * deleting a target inside one is a panic). Without it the battle stays flat.
 */
#define STEREO_SURFACE_IDLE_FRAMES 60u
#define STEREO_SURFACE_RETRY_FRAMES 120u
static C3D_Tex sStereoTex;
static C3D_RenderTarget *sStereoTarget;
static uint32_t sStereoUsedFrame, sStereoRetryFrame;
static bool sStereoWanted;

static void StereoSurfaceRelease(void)
{
    if (sStereoTarget) { ClearWhenMadeForget(sStereoTarget); C3D_RenderTargetDelete(sStereoTarget); }
    if (sStereoTex.data) C3D_TexDelete(&sStereoTex);
    sStereoTarget = NULL;
    memset(&sStereoTex, 0, sizeof(sStereoTex));
}

static void StereoSurfaceManage(void)
{
    bool wanted = sStereoWanted && !sTransitionRequested;

    sStereoWanted = false;
    if (sStereoTarget && sVramAsked)
    {
        /* The field's atlas comes first: without it there is no world. */
        StereoSurfaceRelease();
        sStereoRetryFrame = sStats.frames + STEREO_SURFACE_RETRY_FRAMES;
        CtrLog_Write(CTR_LOG_VIDEO, "3D voxel surface released for an atlas (VRAM free=%lu)",
                     (unsigned long)vramSpaceFree());
    }
    else if (sStereoTarget && !wanted
        && (sTransitionRequested || sStats.frames - sStereoUsedFrame > STEREO_SURFACE_IDLE_FRAMES))
    {
        StereoSurfaceRelease();
        CtrLog_Write(CTR_LOG_VIDEO, "3D voxel surface released (VRAM free=%lu)",
                     (unsigned long)vramSpaceFree());
    }
    else if (!sStereoTarget && wanted && sStats.frames >= sStereoRetryFrame)
    {
        /*
         * RGBA8 when 512 KiB in one piece is there; else RGB565, half that.
         * The picture is opaque - the world with its effects - so only its
         * colour depth is less. In the voxel battle the free VRAM comes in
         * atlas pages, 256 KiB each, and on an Old 3DS the RGBA8 surface
         * never fitted: a 3D battle without any 3D (F0, 2254 frames).
         */
        static const GPU_TEXCOLOR formats[] = {GPU_RGBA8, GPU_RGB565};
        unsigned made = 0;

        for (unsigned round = 0; round < 2 && !sStereoTarget; ++round)
        {
            for (made = 0; made < 2; ++made)
            {
                if (C3D_TexInitVRAM(&sStereoTex, 512, 256, formats[made])
                    && (sStereoTarget = C3D_RenderTargetCreateFromTex(&sStereoTex, GPU_TEXFACE_2D, 0, -1)))
                {
                    ClearWhenMade(sStereoTarget);
                    break;
                }
                StereoSurfaceRelease();
            }
            /* As the scene surface: what a screen left behind goes first. */
            if (!sStereoTarget && round == 0)
            {
                bool freed = ReleaseIdleLayers();

                if (!(ReleaseIdleSurfaces() || freed))
                    break;
            }
        }
        if (sStereoTarget)
        {
            C3D_TexSetFilter(&sStereoTex, GPU_NEAREST, GPU_NEAREST);
            C3D_TexSetWrap(&sStereoTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
            sStereoUsedFrame = sStats.frames;
        }
        else
            sStereoRetryFrame = sStats.frames + STEREO_SURFACE_RETRY_FRAMES;
        CtrLog_Write(CTR_LOG_VIDEO, "3D voxel surface %s (VRAM free=%lu)",
                     !sStereoTarget ? "not made, effects per eye" : made == 0 ? "made" : "made in RGB565",
                     (unsigned long)vramSpaceFree());
    }
}

/*
 * The world to one screen target: the logical surface cleared and the world
 * drawn into it from `eye` (-1: the middle camera; 0, 1: one eye of the
 * pair), then the picture to the target with its tilt-shift, glow and the
 * dark of a cave, and the game's text and weather over it. With the slider
 * up this runs once per eye, the logical surface and the bloom's target
 * reused in turn: the GPU takes the frame's commands in order, so the second
 * eye's clear comes after the first eye's picture has been copied out. What
 * is laid over the world is placed as that eye saw it (CtrVoxel_Draw sets
 * the eye the voxel module projects by); the weather is the same in both
 * eyes, on the glass, and the text in front of the nearest world
 * (ComposeVoxelOverlay).
 */
static void RenderVoxelTo(C3D_RenderTarget *target, int eye, uint32_t clear, bool first)
{
    const Tex3DS_SubTexture logical = {CTR_GAME_WIDTH, CTR_GAME_HEIGHT, 0, 1,
        CTR_GAME_WIDTH / 512.0f, 1 - CTR_GAME_HEIGHT / 256.0f};
    float bloom;

    /* C2D_TargetClear clears colour and depth, which the 3D pass needs. */
    C2D_TargetClear(sLogical, clear);
    CtrVoxel_Draw(sLogical, eye);
    sSceneOn = NULL; /* drawn on with C3D_FrameDrawOn, behind SceneOn's back */

    /* Finish the world before sampling it. UI is drawn on top after blur.
     * The GPU draws it from here while the rest is recorded (GpuStartEarly):
     * until FrameEnd it waited, and every field frame took CPU + GPU, its
     * 6-9 ms of world after the whole present (DROP ... gpu late). The
     * second eye's world is split off the same way, for the same reason. */
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    if (first)
        GpuStartEarly();
    else
        GpuSplit();

    /* Back to the 2D compositor, which assumes its own program and no depth. */
    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    BlendForget();
    bloom = sBloom != NULL ? CtrVoxel_Bloom() : 0.0f;
    if (bloom > 0.005f)
        VoxelBloomPrepare();
    C2D_TargetClear(target, C2D_Color32(0, 0, 0, 255));
    SceneOn(target);
    C2D_ViewReset();
    Blend(5, false, false);
    C2D_DrawImageAt((C2D_Image){&sSurface, &logical}, 0, 0, 0, NULL, 1, 1);
    if (CtrSettings_VoxelBlur())
        VoxelDiorama();
    if (bloom > 0.005f)
        VoxelBloomCompose(bloom);
    VoxelGloom();
    VoxelFlashLight();
    if (!(Reg(0) & 128)) ComposeVoxelOverlay();
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    GpuSplit();
}

/*
 * The voxel path of the single Citro3D frame opened by CtrVideo_Present.
 * Same shape as RenderEye: compose the logical surface, split, blit it to the
 * screen. What changes is who composes it. `stereo` is the 3D slider, 0 for
 * one picture; with it up the world is drawn to each eye from its own camera
 * (CtrVoxel_SetStereo was given the slider before the update).
 */
/*
 * The swoop's crossfade (voxel_swoop.c): the 2D field, whole, composed as the
 * battle transition composes it (RenderTransition), laid over the finished
 * voxel picture at `alpha`. The text layer is left out: the voxel overlay
 * has drawn it. Into the logical surface, which the voxel picture has left.
 */
static unsigned DepthPlanes(void);
static unsigned BandMask(unsigned band, unsigned count);
static float BandDepth(unsigned band, unsigned count);

/* The whole logical picture laid over the target, moved by a fraction of a
 * pixel with the linear filter on: one tap of a blur (see DioramaTap). */
static void SwoopBlurTap(float dx, float dy, float alpha)
{
    const Tex3DS_SubTexture region = {CTR_GAME_WIDTH - 2, CTR_GAME_HEIGHT - 2,
        (1.0f + dx) / 512.0f, 1.0f - (1.0f + dy) / 256.0f,
        (1.0f + dx + CTR_GAME_WIDTH - 2) / 512.0f, 1.0f - (1.0f + dy + CTR_GAME_HEIGHT - 2) / 256.0f};
    C2D_ImageTint tint;

    C2D_AlphaImageTint(&tint, alpha);
    C2D_DrawImageAt((C2D_Image){&sSurface, &region}, 1, 1, 0, &tint, 1, 1);
}

/* The logical picture softened over the target: four taps round each pixel
 * at `blur` (0..1) of their full strength. */
static void SwoopBlur(float blur)
{
    float wide = blur > 1.0f ? blur - 1.0f : 0.0f;

    if (blur > 1.0f) blur = 1.0f;
    SurfaceFilter(GPU_LINEAR);
    SwoopBlurTap(0.5f, 0.5f, 0.55f * blur);
    SwoopBlurTap(-0.5f, -0.5f, 0.45f * blur);
    SwoopBlurTap(1.5f, -0.5f, 0.35f * blur);
    SwoopBlurTap(-1.5f, 0.5f, 0.30f * blur);
    /* Past 1: wider taps on top, for a softer picture still. */
    if (wide > 0.0f)
    {
        SwoopBlurTap(2.5f, 1.5f, 0.35f * wide);
        SwoopBlurTap(-2.5f, -1.5f, 0.35f * wide);
        SwoopBlurTap(1.5f, -2.5f, 0.30f * wide);
        SwoopBlurTap(-1.5f, 2.5f, 0.30f * wide);
    }
    SurfaceFilter(GPU_NEAREST);
}

static void Field2DOver(float alpha, float blur, uint32_t clear, C3D_RenderTarget *target, float parallax)
{
    const Tex3DS_SubTexture logical = {CTR_GAME_WIDTH, CTR_GAME_HEIGHT, 0, 1,
        CTR_GAME_WIDTH / 512.0f, 1 - CTR_GAME_HEIGHT / 256.0f};
    C2D_ImageTint tint;

    /* The voxel picture, still in the logical surface, softened in place
     * on the target before the 2D field goes over it. */
    if (blur > 0.005f)
    {
        C2D_Prepare();
        C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
        BlendForget();
        SceneOn(target);
        C2D_ViewReset();
        Blend(5, false, false);
        SwoopBlur(blur);
        C2D_Flush();
        C3D_FrameSplit(0);
    }

    /* The eye's parallax, as the field's depth planes give its layers
     * (DepthPlanes, BlitBands: the planes it would cut, the furthest at the
     * screen and the rest forward by whole pixels, the backgrounds scrolled
     * together on one plane): composed flat, every layer jumped by it on
     * the frame the swoop began and the frame it ended. Without planes the
     * field is composed per priority (RenderEye), and so is this. */
    unsigned count = parallax != 0.0f && sBandsReady ? DepthPlanes() : 0;

    sParallax = count >= 2 ? 0.0f : parallax;
    sLayerShift = 0;
    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    BlendForget();
    ColourClear(sLogical, clear);
    SceneOn(sLogical);
    Blend(5, false, false);
    StageUnderlay();
    /* As RenderEye composes the 2D field, without its text layer. */
    sLayerExclude = 1;
    if (!(Reg(0) & 128))
    {
        if (count < 2)
            Compose();
        else
            for (int band = (int)count - 1; band >= 0; --band)
            {
                sPriorityMask = BandMask(band, count);
                sLayerOrigin = parallax * BandDepth(band, count);
                Compose();
            }
        sPriorityMask = SLOTS_ALL;
        sLayerOrigin = 0.0f;
    }
    sLayerExclude = 0;
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    C3D_FrameSplit(0);
    BlendForget();
    SceneOn(target);
    C2D_ViewReset();
    Blend(5, false, false);
    C2D_AlphaImageTint(&tint, alpha > 1.0f ? 1.0f : alpha);
    C2D_DrawImageAt((C2D_Image){&sSurface, &logical}, 0, 0, 0, &tint, 1, 1);
    /* ... and softened likewise, as far as it is there. */
    if (blur > 0.005f) SwoopBlur(blur * (alpha > 1.0f ? 1.0f : alpha));
    C2D_Flush();
    C3D_FrameSplit(0);
}

static void RenderVoxel(uint32_t clear, float stereo)
{
    /* The brightness effect (BLDY) on the field's backgrounds and sprites. */
    unsigned control = Reg(0x50), effect = (control >> 6) & 3;
    float bright = effect >= 2 ? Min(Reg(0x54) & 31, 16) / 16.0f : 0.0f;
    VoxelSwoopFrame swoop;

    VoxelSwoop_Frame(&sSwoop, &swoop);
    /* Arriving from the 2D field, the world's light rings down from white -
     * unless the game has an effect of its own on. */
    if (swoop.whiteout > 0.0f && bright <= 0.0f)
        CtrVoxel_SetBrightness(swoop.whiteout, swoop.whiteout, true);
    else
        CtrVoxel_SetBrightness((control & 0x0e) ? bright : 0.0f, (control & 0x10) ? bright : 0.0f,
                               effect == 2);
    if (stereo > 0.0f)
    {
        uint64_t eyesStart = svcGetSystemTick();

        RenderVoxelTo(sTop, 0, clear, true);
        RenderVoxelTo(sTopRight, 1, clear, false);
        sStats.stereoEyesMs = (svcGetSystemTick() - eyesStart) * 1000.0f / SYSCLOCK_ARM11;
    }
    else
        RenderVoxelTo(sTop, -1, clear, true);
}

/*
 * The 3D battle: the battle in front of the voxel world (CtrSettings_
 * VoxelBattle). The world is the battle's scenery - drawn where the GBA draws
 * BG3, from the stage the voxel module chose near the player - and the rest
 * of the battle is the game's own picture over it, composed as the 2D battle
 * is (RenderBattleScene and the text box) into the logical surface cleared
 * transparent, once the world has gone to the screen with its blur and
 * glow. Whatever the GBA does to its scenery the world takes: BG3's
 * brightness (a move darkening the field), its palette fading
 * (VoxelWorld_ScreenFade), its scroll (a move shaking it,
 * CtrVoxel_SetBattleFrame) and the windows that hide it (the intro's curtain).
 */

/* Where the windows hide BG3 - the intro's curtain opening from the middle -
 * the world is hidden too: the backdrop is there. */
static void BattleWorldCurtain(uint32_t backdrop)
{
    int rects[WINDOW_RECTS][4];
    unsigned masks[WINDOW_RECTS], count;

    if (!(Reg(0) & 0x6000))
        return;
    count = WindowPartition(VIEW_TOP, VIEW_BOTTOM, rects, masks);
    for (unsigned i = 0; i < count; ++i)
    {
        float x0, y0, x1, y1;

        if (masks[i] & 8)
            continue;
        x0 = (rects[i][0] + sViewX) * sZoom + sOffX;
        x1 = (rects[i][2] + sViewX) * sZoom + sOffX;
        y0 = (rects[i][1] + sViewY) * sZoom + sOffY;
        y1 = (rects[i][3] + sViewY) * sZoom + sOffY;
        C2D_DrawRectSolid(x0, y0, 0, x1 - x0, y1 - y0, backdrop);
    }
}

/*
 * The GBA's scenery has a base under each side, which the world has not: a
 * soft shadow on the ground under each battler and trainer instead, the dark
 * blue of the world's own shadows fading out from the middle. One small
 * texture (linear memory, made once), stretched to each shadow.
 */
#define BATTLE_SHADOW_W 64
#define BATTLE_SHADOW_H 16
#define BATTLE_SHADOW_ALPHA 0.50f
static C3D_Tex sBattleShadowTex;
static bool sBattleShadowFailed;

static bool BattleShadowTexture(void)
{
    uint32_t *texels;

    if (sBattleShadowTex.data || sBattleShadowFailed)
        return sBattleShadowTex.data != NULL;
    if (!C3D_TexInit(&sBattleShadowTex, BATTLE_SHADOW_W, BATTLE_SHADOW_H, GPU_RGBA8))
    {
        memset(&sBattleShadowTex, 0, sizeof(sBattleShadowTex));
        sBattleShadowFailed = true;
        return false;
    }
    C3D_TexSetFilter(&sBattleShadowTex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&sBattleShadowTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    texels = sBattleShadowTex.data;
    for (unsigned y = 0; y < BATTLE_SHADOW_H; ++y)
        for (unsigned x = 0; x < BATTLE_SHADOW_W; ++x)
        {
            float u = (x + 0.5f) / (BATTLE_SHADOW_W / 2) - 1.0f, v = (y + 0.5f) / (BATTLE_SHADOW_H / 2) - 1.0f;
            float d = 1.0f - (u * u + v * v), a = d > 0.0f ? BATTLE_SHADOW_ALPHA * d * sqrtf(d) : 0.0f;

            texels[CtrVideo_Texel(x, y, BATTLE_SHADOW_W)] = 8u << 24 | 20u << 16 | 40u << 8
                                                           | (uint32_t)(a * 255.0f + 0.5f);
        }
    C3D_TexFlush(&sBattleShadowTex);
    return true;
}

static void BattleWorldShadows(void)
{
    static const Tex3DS_SubTexture whole = {BATTLE_SHADOW_W, BATTLE_SHADOW_H, 0, 1, 1, 0};
    VoxelBattleShadow shadows[4];
    unsigned count = VoxelBattle_Shadows(shadows, 4);

    if (count == 0 || !BattleShadowTexture())
        return;
    for (unsigned i = 0; i < count; ++i)
    {
        float x = (shadows[i].x + sViewX) * sZoom + sOffX, y = (shadows[i].y + sViewY) * sZoom + sOffY;
        float rx = shadows[i].rx * sZoom, ry = shadows[i].ry * sZoom;

        C2D_DrawImageAt((C2D_Image){&sBattleShadowTex, &whole}, x - rx, y - ry, 0, NULL,
                        rx * 2.0f / BATTLE_SHADOW_W, ry * 2.0f / BATTLE_SHADOW_H);
    }
}

/* BG3's scroll from rest, GBA pixels, either way round its 256-pixel turn:
 * the scenery rests at 0 (or 256, the same picture). */
static float BattleScenerySway(unsigned reg)
{
    return (float)((int)((Reg(reg) + 128) & 255) - 128);
}

/*
 * The 3D battle's entrance (CtrVoxel_BattleIntro): while the camera flies in
 * from the field the world is drawn as a shot - a zoom blur and speed lines
 * as fast as it travels, a dark indigo vignette as high as it climbs, cinema
 * bars framing it, a white flash and a burst of glow where it lands. A few
 * quads and, while it travels, two copies of the world surface: about what
 * the diorama blur costs, for a second and a half, and nothing once landed.
 * All of it is the world's, under the battle's own picture.
 */
/* The world's lowest line on screen: the text box covers the rest. */
#define BATTLE_INTRO_FLOOR (CTR_BATTLE_Y + BATTLE_BAND_TOP)
#define BATTLE_INTRO_BAR 22.0f      /* each cinema bar, closed */
#define BATTLE_INTRO_BAR_EDGE 10.0f /* its soft edge */
#define BATTLE_INTRO_STREAKS 12u

/* One copy of the world magnified about the target (the screen's middle,
 * where the camera looks), see-through: a step of the zoom blur. */
static void BattleIntroZoomTap(float scale, float alpha)
{
    const Tex3DS_SubTexture region = {CTR_GAME_WIDTH - 2, CTR_GAME_HEIGHT,
        1.0f / 512.0f, 1.0f, (CTR_GAME_WIDTH - 1) / 512.0f, 1.0f - CTR_GAME_HEIGHT / 256.0f};
    const float cx = CTR_GAME_WIDTH * 0.5f, cy = CTR_GAME_HEIGHT * 0.5f;
    C2D_ImageTint tint;

    C2D_AlphaImageTint(&tint, alpha);
    C2D_DrawImageAt((C2D_Image){&sSurface, &region}, cx + (1.0f - cx) * scale, cy - cy * scale, 0,
                    &tint, scale, scale);
}

/* Before the bloom is added: the zoom blur, from the world surface itself. */
static void BattleIntroBlur(const CtrVoxelBattleIntro *intro)
{
    float s = intro->speed;

    if (s < 0.04f || (Reg(0) & 128))
        return;
    Blend(5, false, false);
    SurfaceFilter(GPU_LINEAR);
    BattleIntroZoomTap(1.0f + 0.035f * s, 0.50f * s);
    BattleIntroZoomTap(1.0f + 0.080f * s, 0.32f * s);
    SurfaceFilter(GPU_NEAREST);
}

/* Over the world, under the curtain: the speed lines and the vignette. */
static void BattleIntroAtmosphere(const CtrVoxelBattleIntro *intro)
{
    const float w = CTR_GAME_WIDTH, bottom = BATTLE_INTRO_FLOOR;
    float shade = fmaxf(0.60f * intro->flight, 0.30f * intro->bars);

    if (intro->speed > 0.04f)
    {
        /* Streaks racing right to left, none across the middle band where
         * the battlers stand; each its own height, length and pace. */
        for (unsigned i = 0; i < BATTLE_INTRO_STREAKS; ++i)
        {
            uint32_t h = (i + 1) * 2654435761u;
            float y = 6.0f + (float)((h >> 8) % (unsigned)(bottom - 12.0f));
            float length = 70.0f + (float)((h >> 16) % 110u);
            float pace = 26.0f + (float)((h >> 20) % 18u);
            float x = w - fmodf(intro->time * pace + (float)(h % 400u), w + length);
            float alpha = intro->speed * (0.35f + 0.35f * (float)((h >> 4) & 3) / 3.0f);
            uint32_t head = C2D_Color32(225, 236, 255, (uint8_t)(alpha * 255.0f));
            uint32_t tail = C2D_Color32(225, 236, 255, 0);

            if (fabsf(y - bottom * 0.55f) < 24.0f)
                y = y < bottom * 0.55f ? y - 24.0f : y + 24.0f;
            C2D_DrawRectangle(x, y, 0, length, (h & 16) ? 2.0f : 1.0f, head, tail, head, tail);
        }
    }
    if (shade > 0.01f)
    {
        uint32_t dark = C2D_Color32(8, 6, 34, (uint8_t)(fminf(shade, 1.0f) * 230.0f));
        uint32_t none = C2D_Color32(8, 6, 34, 0);
        float band = 64.0f, side = 96.0f;
        /* Down to the text box, which covered the rest; the HD one floats
         * with the scene showing round it, and is not there yet. */
        float low = sStats.frames - sBoxShown <= 1 || sBoxHidden ? CTR_GAME_HEIGHT : bottom;

        C2D_DrawRectangle(0, 0, 0, w, band, dark, dark, none, none);
        C2D_DrawRectangle(0, low - band, 0, w, band, none, none, dark, dark);
        C2D_DrawRectangle(0, 0, 0, side, low, dark, none, dark, none);
        C2D_DrawRectangle(w - side, 0, 0, side, low, none, dark, none, dark);
    }
}

/* Over everything of the world: the landing's flash. */
static void BattleIntroFlash(const CtrVoxelBattleIntro *intro)
{
    if (intro->impact > 0.01f)
        C2D_DrawRectSolid(0, 0, 0, CTR_GAME_WIDTH, CTR_GAME_HEIGHT,
                          C2D_Color32(255, 250, 235, (uint8_t)(fminf(intro->impact, 1.0f) * 170.0f)));
}

/*
 * The cinema bars, over everything on the top screen - the battlers and the
 * text box too: drawn last into each finished picture (one eye's, or both at
 * the screen's depth), the bottom one on the screen's own edge. Four quads.
 */
static void BattleIntroBars(const CtrVoxelBattleIntro *intro, bool stereo)
{
    const float w = CTR_GAME_WIDTH, h = CTR_GAME_HEIGHT;
    float bar, edge;
    uint32_t black = C2D_Color32(0, 0, 0, 255), none = C2D_Color32(0, 0, 0, 0);

    if (intro->bars <= 0.01f || (Reg(0) & 128))
        return;
    bar = BATTLE_INTRO_BAR * intro->bars;
    edge = BATTLE_INTRO_BAR_EDGE * intro->bars;
    /* The right eye was drawn last: no target switch before it. */
    for (int eye = stereo ? 1 : 0; eye >= 0; --eye)
    {
        if (stereo)
        {
            /* Whatever RenderBattleStereo began last, flushed and switched
             * properly (SceneOn), as any change of target must be. */
            sSceneOn = NULL;
            SceneOn(eye ? sTopRight : sTop);
        }
        C2D_ViewReset();
        BlendForget();
        Blend(5, false, false);
        C2D_DrawRectSolid(0, 0, 0, w, bar, black);
        C2D_DrawRectangle(0, bar, 0, w, edge, black, black, none, none);
        C2D_DrawRectSolid(0, h - bar, 0, w, bar, black);
        C2D_DrawRectangle(0, h - bar - edge, 0, w, edge, none, none, black, black);
        C2D_Flush();
    }
}

/*
 * With the 3D slider up (`slider` > 0, the scene surface and the shared
 * stereo surface there), the world is the battle's scenery as the 2D one is
 * (BATTLE_BACK_DEPTH): finished once with its blur, glow, shadows and curtain
 * into sStereoTarget, then copied into each eye behind the screen, the
 * battle's sprites in front of it (RenderBattleStereo). The world is drawn
 * once; the GPU starts on it while the rest of the frame is recorded.
 */
static void RenderBattleWorld(uint32_t clear, float slider)
{
    const Tex3DS_SubTexture logical = {CTR_GAME_WIDTH, CTR_GAME_HEIGHT, 0, 1,
        CTR_GAME_WIDTH / 512.0f, 1 - CTR_GAME_HEIGHT / 256.0f};
    unsigned control = Reg(0x50), effect = (control >> 6) & 3;
    float bright = effect >= 2 ? Min(Reg(0x54) & 31, 16) / 16.0f : 0.0f;
    float bloom;
    bool stereo = slider > 0.0f && sScene;
    /* With the slider up the picture goes into the scene surface's first
     * columns, where RenderBattleStereo copies it behind each eye. */
    C3D_RenderTarget *world = stereo ? sScene : sTop;

    sSceneColumn = stereo ? WORLD_COLUMNS + WORLD_GAP : 0;
    CtrVoxelBattleIntro intro;

    /* The world is BG3: its brightness is BG3's. */
    PORT_PROF_BEGIN(bwWorld);
    CtrVoxel_SetBrightness((control & 0x08) ? bright : 0.0f, 0.0f, effect == 2);
    C2D_TargetClear(sLogical, clear);
    CtrVoxel_Draw(sLogical, -1);
    sSceneOn = NULL; /* drawn on with C3D_FrameDrawOn, behind SceneOn's back */
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    GpuSplit();
    PORT_PROF_END(bwWorld, PORT_PROF_BW_WORLD);
    PORT_PROF_BEGIN(bwPost);

    /* The world to the screen, as in the field. */
    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    BlendForget();
    CtrVoxel_BattleIntro(&intro);
    bloom = sBloom != NULL ? CtrVoxel_Bloom() : 0.0f;
    /* The landing glows: a burst of the bloom dying away with its flash. */
    if (sBloom != NULL && intro.impact > 0.01f)
        bloom = fminf(bloom + 0.7f * intro.impact, 1.0f);
    if (bloom > 0.005f)
        VoxelBloomPrepare();
    if (stereo)
    {
        /* The whole surface, as a memory fill, every frame: a drawn clear
         * of the slots' columns did not land on hardware (a slot composed
         * over what was there frames before came back as a lit copy of a
         * Pokémon at a battle's end), so nothing of a frame before is left
         * for a slot to sample. The scenery beside the world is composed
         * again when there is one (a move's background, a few frames). */
        ColourClear(sScene, 0);
        sSceneOn = NULL;
        sSceneCached = false;
    }
    else
        C2D_TargetClear(world, C2D_Color32(0, 0, 0, 255));
    SceneOn(world);
    C2D_ViewReset();
    Blend(5, false, false);
    C2D_DrawImageAt((C2D_Image){&sSurface, &logical}, 0, 0, 0, NULL, 1, 1);
    if (CtrSettings_VoxelBlur())
        VoxelDiorama();
    BattleIntroBlur(&intro);
    if (bloom > 0.005f)
        VoxelBloomCompose(bloom);
    if (!(Reg(0) & 128))
    {
        /* Blended over the world, not added as the glow was. */
        Blend(5, false, false);
        BattleWorldShadows();
        BattleIntroAtmosphere(&intro);
        BattleWorldCurtain(clear);
        BattleIntroFlash(&intro);
    }
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    GpuSplit();
    PORT_PROF_END(bwPost, PORT_PROF_BW_POST);

    GpuStartEarly();
    PORT_PROF_BEGIN(bwScene);
    if (stereo)
    {
        RenderBattleStereo(clear, slider, &sSceneTex);
        BattleIntroBars(&intro, true);
        PORT_PROF_END(bwScene, PORT_PROF_BW_SCENE);
        return;
    }

    /* The battle's own picture over it: the logical surface again, cleared
     * transparent, composed as RenderEye composes the 2D battle. */
    sParallax = 0.0f;
    sLayerShift = 0.0f;
    BlendForget();
    C2D_TargetClear(sLogical, 0);
    if (sScene) RenderBattleScene(0);
    SceneOn(sLogical);
    Blend(5, false, false);
    if (sScene) HudDrawCrisp(HUD_FLAT);
    /* After a scene composed on its own only the text box is left; without
     * its surface, everything but what the world stands in for. */
    sLayerExclude = sScene ? 63 & ~(1u | 32u | BattleBoxLayers()) : sWorldLayers;
    if (!(Reg(0) & 128)) Compose();
    sLayerExclude = 0;
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    GpuSplit();

    BlendForget();
    SceneOn(sTop);
    C2D_ViewReset();
    Blend(5, false, false);
    C2D_DrawImageAt((C2D_Image){&sSurface, &logical}, 0, 0, 0, NULL, 1, 1);
    C2D_Flush();
    BattleIntroBars(&intro, false);
    PORT_PROF_END(bwScene, PORT_PROF_BW_SCENE);
}

/*
 * A battle transition over the field (CtrVideo_SetTransition).
 *
 * The transitions are compiled for the GBA screen (ctr_gba_transition.h) and
 * shown at the battle scene's scale, 1.5, centred across, 20 pixels of margin
 * each side carrying their edge columns: the battle that follows them is
 * framed the same way. What they do falls in two parts:
 *
 * - The field - the 2D layers or the voxel world, whole, in the logical
 *   surface - moved line by line as their scroll registers say (the swirl,
 *   the slice, the ripple...) and dimmed or brightened as BLDY says, so it is
 *   drawn to the screen in bands of lines that share those.
 * - Their own picture: BG0, their sprites and the backdrop wherever their
 *   windows hide the field. Composed like any GBA screen, band by band of
 *   lines whose registers match (sLineRegs), at 2x into the battle scene's
 *   surface and drawn from it at 0.75 with filtering, as the battle scene
 *   is. 160 lines at 2x do not fit its 256, so the two halves of the picture
 *   lie side by side, a line of overlap each so the filter reads real lines
 *   across the seam.
 */
#define TRANSITION_ZOOM 1.5f
#define TRANSITION_X ((CTR_GAME_WIDTH - 240 * TRANSITION_ZOOM) / 2)
#define TRANSITION_HALF 80
/* Where each half starts in the scene surface: across, and rows down. */
#define TRANSITION_HALF_X 512
#define TRANSITION_HALF_TOP 2

/* The first screen line of GBA line g: ceil(g * 1.5). */
static int TransitionLine(int g)
{
    return (g * 3 + 1) / 2;
}

/* The field's displacement and brightness on GBA line g of the transition. */
typedef struct
{
    int dx, dy;
    float bright;
    bool white;
} TransitionBand;

static TransitionBand TransitionBandAt(int g, unsigned targets)
{
    const uint16_t *line = sLineRegs[g], *base = sLineRegs[0];
    /* BG1: the transitions move the field's three layers together. */
    unsigned control = line[(0x50 - CTR_LINE_REG_FIRST) / 2], effect = (control >> 6) & 3;
    TransitionBand band = {
        (int16_t)(line[(0x14 - CTR_LINE_REG_FIRST) / 2] - base[(0x14 - CTR_LINE_REG_FIRST) / 2]),
        (int16_t)(line[(0x16 - CTR_LINE_REG_FIRST) / 2] - base[(0x16 - CTR_LINE_REG_FIRST) / 2]),
        0.0f, effect == 2};

    if (effect >= 2 && (control & targets))
        band.bright = Min(line[(0x54 - CTR_LINE_REG_FIRST) / 2] & 31, 16) / 16.0f;
    return band;
}

static bool SameBand(const TransitionBand *a, const TransitionBand *b)
{
    return a->dx == b->dx && a->dy == b->dy && a->bright == b->bright && a->white == b->white;
}

/* The field from the logical surface, band by band, onto the target. */
static void TransitionField(void)
{
    for (int g = 0, next; g < CTR_GBA_LINES; g = next)
    {
        TransitionBand band = TransitionBandAt(g, 0x1e);
        C2D_ImageTint tint;
        int sx = (int)roundf(band.dx * TRANSITION_ZOOM), sy = (int)roundf(band.dy * TRANSITION_ZOOM);
        int y0 = TransitionLine(g), y1, x0, x1;

        for (next = g + 1; next < CTR_GBA_LINES; ++next)
        {
            TransitionBand other = TransitionBandAt(next, 0x1e);
            if (!SameBand(&band, &other)) break;
        }
        y1 = TransitionLine(next);
        /* Only what the surface holds: past its edges is the backdrop. */
        x0 = sx < 0 ? -sx : 0;
        x1 = sx > 0 ? CTR_GAME_WIDTH - sx : CTR_GAME_WIDTH;
        if (y0 + sy < 0) y0 = -sy;
        if (y1 + sy > CTR_GAME_HEIGHT) y1 = CTR_GAME_HEIGHT - sy;
        if (x0 >= x1 || y0 >= y1) continue;
        {
            const Tex3DS_SubTexture run = {(u16)(x1 - x0), (u16)(y1 - y0),
                (x0 + sx) / 512.0f, 1.0f - (y0 + sy) / 256.0f,
                (x1 + sx) / 512.0f, 1.0f - (y1 + sy) / 256.0f};
            unsigned c = band.white ? 255 : 0;

            C2D_PlainImageTint(&tint, C2D_Color32(c, c, c, 255), band.bright);
            C2D_DrawImageAt((C2D_Image){&sSurface, &run}, x0, y0, 0,
                            band.bright > 0.0f ? &tint : NULL, 1, 1);
        }
    }
}

/*
 * Groups of registers a band of the transition's lines must share, as
 * [first, end) offsets: BG0's scroll, the windows, the blending.
 */
static const uint8_t sBg0Regs[2] = {0x10, 0x14};
static const uint8_t sWindowRegs[2] = {0x40, 0x4c};
/* BLDY is left out: brightness is applied as the picture is drawn. */
static const uint8_t sBlendRegs[2] = {0x50, 0x54};

static bool SameRegs(int a, int b, const uint8_t group[2])
{
    unsigned first = (group[0] - CTR_LINE_REG_FIRST) / 2, count = (group[1] - group[0]) / 2;

    return !memcmp(&sLineRegs[a][first], &sLineRegs[b][first], count * sizeof(uint16_t));
}

/* The end of the band of lines from y, before end, that shares these groups. */
static int BandEnd(int y, int end, const uint8_t (*const groups[])[2], unsigned count)
{
    int next = y + 1;

    for (; next < end; ++next)
        for (unsigned g = 0; g < count; ++g)
            if (!SameRegs(y, next, *groups[g])) return next;
    return next;
}

/* Line y's registers for composing the picture: the brightness effect is
 * drawn later, per line, over the picture and the field alike. */
static const uint16_t *PictureRegs(int y)
{
    static uint16_t regs[CTR_LINE_REGS];
    unsigned index = (0x50 - CTR_LINE_REG_FIRST) / 2;

    memcpy(regs, sLineRegs[y], sizeof(regs));
    if (((regs[index] >> 6) & 3) >= 2) regs[index] &= ~0xc0u;
    return regs;
}

/*
 * Which of the transition's own layers hold anything this frame: BG0 (a tile
 * that is not blank anywhere in its map) and its sprites. A wipe or a slice
 * is only windows over the field: their BG0 is blank and they have no
 * sprites, and composing those through 160 bands of windows is what cost a
 * frame and a half on an Old 3DS.
 */
static unsigned TransitionLayersPresent(void)
{
    unsigned display = Reg(0), present = 0;

    if (display & 0x100)
    {
        unsigned control = Reg(8), size = control >> 14;
        unsigned map = ((control >> 8) & 31) * 0x800, chars = ((control >> 2) & 3) * 0x4000;
        unsigned entries = 1024u * (size == 0 ? 1 : size == 3 ? 4 : 2);
        bool color256 = (control & 128) != 0;
        unsigned last = 0x10000;

        for (unsigned i = 0; i < entries && !(present & 1); ++i)
        {
            unsigned entry = Read16(map + i * 2);
            unsigned address = chars + (entry & 1023) * (color256 ? 64 : 32);

            if (entry == last || address >= 0x10000) continue;
            last = entry;
            if (GetTileSlot(address, entry >> 12, color256) >= 0) present |= 1;
        }
    }
    if (display & 0x1000)
        for (unsigned i = 0; i < 128 && !(present & 16); ++i)
        {
            unsigned attr0 = sMemory.oam[i * 4];

            /* An entry the game parks hidden is not a sprite on screen. */
            if (TransitionOam(i) && ((attr0 & 0x100) || !(attr0 & 0x200))) present |= 16;
        }
    return present;
}

/* One half of the picture: its lines and where they go in the surface. */
static void TransitionHalf(int h, int *first, int *end)
{
    *first = h * TRANSITION_HALF - 1;
    *end = (h + 1) * TRANSITION_HALF + 1;
    if (*first < 0) *first = 0;
    if (*end > CTR_GBA_LINES) *end = CTR_GBA_LINES;
    sOffX = (float)(h * TRANSITION_HALF_X);
    sOffY = TRANSITION_HALF_TOP - h * TRANSITION_HALF * SCENE_ZOOM;
}

/*
 * The transition's own picture into the scene surface (see above).
 *
 * Composed band by band as windows dictate, every band walks BG0 and every
 * sprite again behind a scissor of its own; a wipe moves its window edges on
 * every line, and 160 bands of that was a frame and more on an Old 3DS. So
 * the work is split by what actually changes line by line:
 *
 * 1. The backdrop where the windows hide the field: solid rectangles, band
 *    by band of the window registers, no scissor needed.
 * 2. BG0 and the sprites where the windows show them everywhere they are
 *    seen: drawn once, or once per band of BG0's scroll and the blending
 *    (the mugshots' two halves) - a handful of passes, not 160.
 * 3. Only a layer the windows show in some places and hide in others is
 *    composed band by band through them.
 */
static void TransitionCompose(void)
{
    static const uint8_t (*const windowGroups[])[2] = {&sWindowRegs};
    static const uint8_t (*const layerGroups[])[2] = {&sBg0Regs, &sBlendRegs};
    static const uint8_t (*const allGroups[])[2] = {&sBg0Regs, &sWindowRegs, &sBlendRegs};
    float zoom = sZoom, offX = sOffX, offY = sOffY;
    int viewX = sViewX, viewY = sViewY, surfaceH = sSurfaceH;
    bool fieldLayers = sFieldLayers, fieldUi = sFieldUi;
    /* Per layer (BG0, sprites): 1 seen shown, 2 seen hidden. */
    unsigned bg0 = 0, obj = 0, uniform, mixed, present;
    uint32_t rgb = CtrVideo_RGBA8(sMemory.palette[0] & 0x7fff, true);
    uint32_t backdrop = C2D_Color32(rgb >> 24, rgb >> 16, rgb >> 8, 255);

    sTransitionCompose = true;
    sFieldLayers = sFieldUi = false;
    sViewX = sViewY = 0;
    sZoom = SCENE_ZOOM;
    sSurfaceH = SCENE_H;
    sObjFilter = OBJ_TRANSITION;
    BlendForget();
    sSceneCached = false;
    C2D_TargetClear(sScene, 0);
    SceneOn(sScene);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    sScissored = false;
    Blend(5, false, false);

    for (int h = 0, first, end; h < 2; ++h)
    {
        TransitionHalf(h, &first, &end);
        ViewBase();
        for (int y = first, next; y < end; y = next)
        {
            int rects[WINDOW_RECTS][4];
            unsigned masks[WINDOW_RECTS], count;

            next = BandEnd(y, end, windowGroups, 1);
            sRegLine = sLineRegs[y];
            if (Reg(0) & 128) continue;
            count = WindowPartition(y, next, rects, masks);
            for (unsigned i = 0; i < count; ++i)
            {
                bg0 |= (masks[i] & 1) ? 1 : 2;
                obj |= (masks[i] & 16) ? 1 : 2;
                if (!(masks[i] & 0x0e))
                    C2D_DrawRectSolid(rects[i][0], rects[i][1], 0, rects[i][2] - rects[i][0],
                                      rects[i][3] - rects[i][1], backdrop);
            }
        }
    }

    present = TransitionLayersPresent();
    uniform = ((bg0 == 1 ? 1u : 0u) | (obj == 1 ? 16u : 0u)) & present;
    mixed = ((bg0 == 3 ? 1u : 0u) | (obj == 3 ? 16u : 0u)) & present;
    for (int h = 0, first, end; uniform && h < 2; ++h)
    {
        TransitionHalf(h, &first, &end);
        for (int y = first, next; y < end; y = next)
        {
            next = BandEnd(y, end, layerGroups, 2);
            sRegLine = PictureRegs(y);
            if (Reg(0) & 128) continue;
            /* Blend() keeps its state between calls; BLDY may differ here. */
            BlendForget();
            C2D_Flush();
            Scissor(0, y, 240, next);
            sScissored = true;
            sClipX0 = 0; sClipX1 = 240;
            sClipY0 = y; sClipY1 = next;
            Layers(uniform | 32u);
        }
    }
    sTransitionLayers = mixed | 32u;
    for (int h = 0, first, end; mixed && h < 2; ++h)
    {
        TransitionHalf(h, &first, &end);
        for (int y = first, next; y < end; y = next)
        {
            next = BandEnd(y, end, allGroups, 3);
            sRegLine = PictureRegs(y);
            if (Reg(0) & 128) continue;
            BlendForget();
            ComposeBand(y, next);
        }
    }
    sRegLine = NULL;
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    sScissored = false;
    C3D_FrameSplit(0);

    sObjFilter = OBJ_ALL;
    sTransitionCompose = false;
    sFieldLayers = fieldLayers;
    sFieldUi = fieldUi;
    sViewX = viewX;
    sViewY = viewY;
    sZoom = zoom;
    sOffX = offX;
    sOffY = offY;
    sSurfaceH = surfaceH;
    ClipToView();
    BlendForget();
}

/* The transition's picture from the scene surface onto the target. */
static void TransitionPicture(void)
{
    const uint16_t *line = sLineRegs[0];
    unsigned control = line[(0x50 - CTR_LINE_REG_FIRST) / 2];
    const float scale = TRANSITION_ZOOM / SCENE_ZOOM;

    /* BG0 blended over the field (the big Poké Ball): the picture holds BG0
     * already weighted by EVA; the field underneath keeps its EVB. */
    if (((control >> 6) & 3) == 1 && (control & 1) && (control & 0x0e00))
    {
        unsigned evb = Min((line[(0x52 - CTR_LINE_REG_FIRST) / 2] >> 8) & 31, 16) * 255 / 16;

        C2D_Flush();
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_CONSTANT_ALPHA, GPU_ONE, GPU_ZERO);
        C3D_BlendingColor(evb << 24);
    }
    for (int g = 0, next; g < CTR_GBA_LINES; g = next)
    {
        /* The picture's brightness: BG0 and the sprites, line by line. */
        TransitionBand band = TransitionBandAt(g, 0x11);
        int h = g / TRANSITION_HALF, halfEnd = (h + 1) * TRANSITION_HALF;
        C2D_ImageTint tint;
        unsigned c = band.white ? 255 : 0;

        for (next = g + 1; next < halfEnd; ++next)
        {
            TransitionBand other = TransitionBandAt(next, 0x11);
            if (other.bright != band.bright || other.white != band.white) break;
        }
        C2D_PlainImageTint(&tint, C2D_Color32(c, c, c, 255), band.bright);
        {
            /* Rows of this band in the half, and on the screen. */
            float top = TRANSITION_HALF_TOP + (g - h * TRANSITION_HALF) * SCENE_ZOOM;
            float bottom = TRANSITION_HALF_TOP + (next - h * TRANSITION_HALF) * SCENE_ZOOM;
            float v0 = 1.0f - top / SCENE_H, v1 = 1.0f - bottom / SCENE_H;
            float left = (float)(h * TRANSITION_HALF_X) / SCENE_W;
            float right = (float)(h * TRANSITION_HALF_X + 240 * SCENE_ZOOM) / SCENE_W;
            float y = g * TRANSITION_ZOOM;
            const Tex3DS_SubTexture part = {(u16)(240 * SCENE_ZOOM), (u16)(bottom - top),
                left, v0, right, v1};
            /* The margins: the picture's first and last columns, stretched. */
            float edge0 = left + 0.5f / SCENE_W, edge1 = right - 0.5f / SCENE_W;
            const Tex3DS_SubTexture leftEdge = {1, (u16)(bottom - top), edge0, v0, edge0, v1};
            const Tex3DS_SubTexture rightEdge = {1, (u16)(bottom - top), edge1, v0, edge1, v1};
            const C2D_ImageTint *t = band.bright > 0.0f ? &tint : NULL;

            C2D_DrawImageAt((C2D_Image){&sSceneTex, &part}, TRANSITION_X, y, 0, t, scale, scale);
            C2D_DrawImageAt((C2D_Image){&sSceneTex, &leftEdge}, 0, y, 0, t, TRANSITION_X, scale);
            C2D_DrawImageAt((C2D_Image){&sSceneTex, &rightEdge}, CTR_GAME_WIDTH - TRANSITION_X, y, 0,
                            t, TRANSITION_X, scale);
        }
    }
    C2D_Flush();
    BlendForget();
}

static void RenderTransition(bool voxel, uint32_t clear)
{
    /* The field, whole: its layers as they stand before the first line. */
    static uint16_t fieldRegs[CTR_LINE_REGS];

    memcpy(fieldRegs, sLineRegs[0], sizeof(fieldRegs));
    /* Its brightness is the bands' (TransitionField). */
    fieldRegs[(0x50 - CTR_LINE_REG_FIRST) / 2] = 0;
    sParallax = 0;
    sLayerShift = 0;
    BlendForget();
    if (voxel)
    {
        CtrVoxel_SetBrightness(0.0f, 0.0f, false);
        C2D_TargetClear(sLogical, clear);
        CtrVoxel_Draw(sLogical, -1);
        sSceneOn = NULL; /* drawn on with C3D_FrameDrawOn, behind SceneOn's back */
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
        /* The GPU on the world while the transition is recorded, as in the
         * field (RenderVoxelTo): split off only, it waited for FrameEnd, and
         * the transition's frames were the field's 6-9 ms of world after the
         * whole present (port.log 2026-10-21: DROP ... gpuStart=7.8 gpu
         * late, under the transition into a battle). */
        {
            bool started = sGpuEarly;

            GpuStartEarly();
            if (started || !sGpuEarly) C3D_FrameSplit(0);
        }
        C2D_Prepare();
        C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
        BlendForget();
    }
    else
    {
        C2D_TargetClear(sLogical, clear);
        SceneOn(sLogical);
        Blend(5, false, false);
        sRegLine = fieldRegs;
        sLayerExclude = 1;
        sObjFilter = OBJ_FIELD;
        ClipToView();
        if (!(Reg(0) & 128)) Layers(63);
        sObjFilter = OBJ_ALL;
        sLayerExclude = 0;
        sRegLine = NULL;
        C2D_Flush();
        C3D_FrameSplit(0);
        BlendForget();
    }

    if (sScene) TransitionCompose();

    C2D_TargetClear(sTop, clear);
    SceneOn(sTop);
    C2D_ViewReset();
    Blend(5, false, false);
    TransitionField();
    if (sScene) TransitionPicture();
    C2D_Flush();
    C3D_FrameSplit(0);
}

/*
 * How many depth planes this frame can be split into.
 *
 * A layer that blends with whatever lies beneath it has to keep that beneath
 * in the same surface, so everything from its priority down stays together.
 * That is GBA alpha blending, which needs a target-1 layer, a target-2 layer
 * and a nonzero second coefficient (the overworld leaves the effect enabled
 * with no target-1 layer at all, which blends nothing), and a sprite in the
 * semi-transparent OBJ mode, which blends whatever its own priority allows.
 *
 * The result is planes 0..count-2 holding one priority each and the last
 * holding the rest, so the planes never change what a frame looks like.
 */
/* The depth slots this frame actually draws something in. */
static unsigned UsedSlots(void)
{
    unsigned display = Reg(0), mode = display & 7, used = 0;

    for (unsigned bg = 0; bg < 4; ++bg)
    {
        if (!(display & (0x100u << bg))) continue;
        if ((mode == 1 && bg == 3) || (mode == 2 && bg < 2)) continue;
        used |= SLOT_BG(Reg(8 + bg * 2) & 3);
    }
    if (display & 0x1000)
        for (unsigned i = 0; i < 128; ++i)
        {
            unsigned attr0 = sMemory.oam[i * 4];

            if (!(attr0 & 0x100) && (attr0 & 0x200)) continue;
            if (((attr0 >> 10) & 3) >= 2) continue;
            used |= SLOT_OBJ((sMemory.oam[i * 4 + 2] >> 10) & 3);
        }
    return used;
}

/*
 * The slots of each plane, front to back: every slot in use its own plane,
 * from the nearest, while planes last; the last plane takes all the rest, and
 * everything from priority `merge` back (layers scrolled together) is one.
 * So the depths go where the picture has something - the intro's bike scene
 * is its sprites, its near layer and its far ones, not three priorities of
 * which the first is empty.
 */
static unsigned sBandSlots[CTR_BANDS];

static unsigned BandsFromSlots(unsigned used, unsigned merge)
{
    unsigned count = 0, cut = merge < 4 ? SLOT_OBJ(merge) : 256u, done = 0;
    /*
     * What is in front of layers scrolled together keeps a plane of its own
     * even when planes are short: the field's text window shares priority 0
     * with sprites, and with two planes the sprites took the near one and
     * left the window flat on the map.
     */
    unsigned front = sBandCount - (cut < 256 && (used & ~(cut - 1)) ? 1 : 0);

    if (!sBandCount) return 0;
    for (unsigned bit = 1; bit < cut; bit <<= 1)
    {
        if (!(used & bit)) continue;
        if (count + 1 < front) sBandSlots[count++] = bit;
        else
            /* The last plane before the cut: this slot and all up to it. */
            sBandSlots[count++] = (cut - 1) & ~(bit - 1);
        done = (bit << 1) - 1;
        if (count == front) { done = cut - 1; break; }
    }
    /* The last plane: everything behind what has a plane already. */
    if (count < sBandCount && (used & ~done)) sBandSlots[count++] = SLOTS_ALL & ~done;
    else if (count) sBandSlots[count - 1] |= SLOTS_ALL & ~done;
    if (count == 0) sBandSlots[count++] = SLOTS_ALL;
    /* The first plane also takes the empty slots in front of it. */
    sBandSlots[0] |= (sBandSlots[0] & -sBandSlots[0]) - 1;
    return count;
}

static unsigned DepthPlanes(void)
{
    unsigned display = Reg(0);
    unsigned merge = 4;

    /*
     * Blending no longer merges planes: a plane with something that blends
     * gets what lies behind it underneath, colour only (RenderBands).
     */
    /*
     * Backgrounds scrolled together are one image cut into layers, like the
     * three metatile layers of the field, and giving them different depths
     * pulls their tiles a pixel apart. Only a shared nonzero scroll means
     * that: on a still screen every layer sits at zero and they are separate
     * pictures stacked on each other, which is exactly where depth belongs.
     */
    for (unsigned bg = 0; bg + 1 < 4 && merge; ++bg)
        for (unsigned other = bg + 1; other < 4 && merge; ++other)
        {
            unsigned scroll = Reg(0x10 + bg * 4) | (Reg(0x12 + bg * 4) << 16);
            unsigned priority, otherPriority;

            if (!(display & (0x100u << bg)) || !(display & (0x100u << other))) continue;
            if (!scroll || scroll != (Reg(0x10 + other * 4) | (Reg(0x12 + other * 4) << 16)))
                continue;
            priority = Reg(8 + bg * 2) & 3;
            otherPriority = Reg(8 + other * 2) & 3;
            if (otherPriority < priority) priority = otherPriority;
            if (priority < merge) merge = priority;
        }
    /*
     * The slots stay in use for the rest of the scene: a sprite that comes
     * and goes - the logo's letters, a sparkle - would otherwise move every
     * layer behind it from one plane to the next and back, the depth of the
     * whole picture flickering with it. A scene is its display control and
     * background priorities; when they change, so may the depths.
     */
    {
        static unsigned sticky, stickyKey;
        unsigned key = (display & 0x1f07) | (Reg(8) & 3) << 16 | (Reg(10) & 3) << 18
                     | (Reg(12) & 3) << 20 | (Reg(14) & 3) << 22 | (unsigned)sStage << 24;
        unsigned used;

        if (key != stickyKey) sticky = 0;
        stickyKey = key;
        sticky |= UsedSlots();
        used = sticky;
        /*
         * Title screen: the version banner (OBJ priority 0) sits on top of the
         * Pokémon logo (BG2 priority 1). On the GBA those are one flat composite
         * — transparent banner texels show the katakana. Stereo depth planes
         * would put the banner on a nearer surface, so its outline reads as an
         * opaque plate (worst on Japanese, where エメラルド rests higher on the
         * logo). Keep those two slots in one plane; clouds and Rayquaza keep
         * their own depths behind.
         */
        if (sStage && CtrTitleScreen_RayquazaBg() >= 0 && sBandCount >= 2)
        {
            unsigned logo = (SLOT_OBJ(0) | SLOT_BG(1)) & used;
            unsigned rest = used & ~logo;
            unsigned count = 0;

            if (logo)
                sBandSlots[count++] = logo;
            if (rest && count < sBandCount)
            {
                /* Prefer a middle plane for the clouds (BG priority 2). */
                unsigned clouds = SLOT_BG(2) & rest;

                if (clouds && count + 1 < sBandCount)
                {
                    sBandSlots[count++] = clouds;
                    rest &= ~clouds;
                }
                if (rest)
                    sBandSlots[count++] = rest | (SLOTS_ALL & ~used);
                else if (count)
                    sBandSlots[count - 1] |= SLOTS_ALL & ~used;
            }
            else if (count)
                sBandSlots[count - 1] |= SLOTS_ALL & ~used;
            else
                sBandSlots[count++] = SLOTS_ALL;
            sBandSlots[0] |= (sBandSlots[0] & -sBandSlots[0]) - 1;
            return count;
        }
        return BandsFromSlots(used, merge);
    }
}

/*
 * The depth slots holding something that blends with what lies beneath it:
 * a first target of BLDCNT's alpha blend, or a semi-transparent sprite.
 */
static unsigned BlendingPriorities(void)
{
    unsigned display = Reg(0), control = Reg(0x50), found = 0;
    unsigned target1 = control & 63, target2 = (control >> 8) & 63;
    bool alpha = ((control >> 6) & 3) == 1 && target1 && target2 && ((Reg(0x52) >> 8) & 31);

    if (alpha)
        for (unsigned bg = 0; bg < 4; ++bg)
            if ((target1 & (1u << bg)) && (display & (0x100u << bg)))
                found |= SLOT_BG(Reg(8 + bg * 2) & 3);
    if (display & 0x1000)
        for (unsigned i = 0; i < 128; ++i)
        {
            unsigned attr0 = sMemory.oam[i * 4], mode = (attr0 >> 10) & 3;

            if (!(attr0 & 0x100) && (attr0 & 0x200)) continue;
            if (mode == 1 || (mode == 0 && alpha && (target1 & 16)))
                found |= SLOT_OBJ((sMemory.oam[i * 4 + 2] >> 10) & 3);
        }
    return found;
}

/*
 * Released again once unused for a few seconds - the slider lowered, or the
 * voxel overworld on screen, which never composes planes. They are 768 KiB of
 * VRAM, and kept for good after the title screen was shown in 3D they left
 * the overworld's atlases, pages and chunks starving for it.
 */
#define CTR_BANDS_IDLE_FRAMES 180
static uint32_t sBandsUsedFrame;
/*
 * The planes' parallax eased in over this many frames once they are made,
 * and out again before the voxel world takes their VRAM back
 * (VoxelVramManage): around the swoop the field went from flat to its full
 * depth in one frame - every layer jumping by its whole pixels - as the
 * planes came a second after the world parked, or went the frame the
 * option came on. 0..1, BlitBands scales by it.
 */
#define CTR_PLANE_RAMP_FRAMES 20u
static float sPlaneRamp;
/* Asked by the overworld when an atlas found no VRAM (sPlaneReleaseAsked,
 * declared with the bottom screen's surface); honoured before the next frame
 * opens, since deleting a render target may wait for the GPU. */
void CtrVideo_RequestPlaneRelease(void)
{
    sPlaneReleaseAsked = true;
}

/*
 * sPlaneShrinkAsked (declared with the stage's layer textures): asked by a
 * stage whose layer textures did not fit beside three planes: the intro's four
 * 256x512 layers (1 MiB) and three planes (768 KiB) are more than an Old 3DS
 * has free. Giving up the nearest plane is enough for them, and the stage
 * keeps two depths; giving up all of them had it composed per eye, and at
 * 30 fps.
 */

/*
 * Set inside a frame that wanted the planes and found none. They are made
 * before the next frame opens: a failed attempt frees the planes it did get,
 * and C3D_RenderTargetDelete inside an open frame is svcBreak(USERBREAK_PANIC)
 * in citro3d - on an Old 3DS, whose VRAM often holds only two of the three,
 * that was opening a menu with the slider up. Until then, per eye.
 */
static bool sBandsWanted;

static void BandsRelease(void)
{
    sPlaneRamp = 0.0f;
    for (unsigned i = 0; i < CTR_BANDS; ++i)
    {
        if (sBand[i]) { ClearWhenMadeForget(sBand[i]); C3D_RenderTargetDelete(sBand[i]); }
        if (sBandTex[i].data) C3D_TexDelete(&sBandTex[i]);
        sBand[i] = NULL;
        memset(&sBandTex[i], 0, sizeof(sBandTex[i]));
    }
    sBandCount = 0;
    sBandsReady = false;
}

/*
 * Outside the frame (LayersPrepare): the depth planes, unless a stage is
 * using them, and the bottom screen's surface when no screen wants it.
 * Whether anything went.
 */
static bool ReleaseIdleSurfaces(void)
{
    bool any = false;

    if (sBandsReady && !sStage)
    {
        BandsRelease();
        any = true;
    }
    if (sBottom && !sBottomInUse)
    {
        BottomRelease();
        any = true;
    }
    if (any)
        CtrLog_Write(CTR_LOG_VIDEO, "VRAM short: idle 3D planes / bottom surface released "
                     "(VRAM free=%lu)", (unsigned long)vramSpaceFree());
    return any;
}

/* The last plane goes; outside the frame, like BandsRelease. */
static void BandsShrink(void)
{
    unsigned last = sBandCount - 1;

    ClearWhenMadeForget(sBand[last]);
    C3D_RenderTargetDelete(sBand[last]);
    C3D_TexDelete(&sBandTex[last]);
    sBand[last] = NULL;
    memset(&sBandTex[last], 0, sizeof(sBandTex[last]));
    --sBandCount;
}

/* Allocated on first use: a console that never opens the 3D slider never pays
 * the VRAM. A failure here is not fatal, it just keeps the direct path. */
static bool BandsCreate(void);

/* In the frame: whether the planes are there, asking for them if not. */
static bool BandsUsable(void)
{
    sBandsUsedFrame = sStats.frames;
    if (!sBandsReady) sBandsWanted = true;
    return sBandsReady;
}

/* Outside the frame only; see sBandsWanted. */
static bool BandsReady(void)
{
    sBandsUsedFrame = sStats.frames;
    if (sBandsReady) return true;
    if (sBandsFailed && sStats.frames < sBandsRetryFrame) return false;
    for (unsigned attempt = 0; attempt < 3 && !sBandsReady; ++attempt)
    {
        if (BandsCreate())
            sBandsReady = true;
        /*
         * The overworld keeps an atlas per tileset pair it has met, and a
         * 2D screen - a menu, a battle - is where that VRAM is wanted back:
         * the atlases of maps not on screen go, the current map's stay.
         */
        else if (attempt == 0)
        {
            CtrVoxel_ReleaseIdleVram();
        }
        /*
         * Back from a battle, its scene surface is kept a few seconds in case
         * another one follows, and the field has taken its layer textures
         * back meanwhile: the planes did not fit beside both and stayed away,
         * flat. On the field the planes come first.
         */
        else if (attempt == 1 && sScene && !sBattle && !sTransition)
            SceneRelease();
        else
            break;
    }
    if (!sBandsReady)
    {
        if (!sBandsFailed)
            CtrLog_Write(CTR_LOG_ERROR, "VIDEO: no VRAM for 3D depth planes (free=%lu); composing "
                         "per eye, retrying later", (unsigned long)vramSpaceFree());
        sBandsFailed = true;
        sBandsRetryFrame = sStats.frames + CTR_BANDS_RETRY_FRAMES;
        return false;
    }
    sBandsFailed = false;
    CtrLog_Write(CTR_LOG_VIDEO, "3D depth planes ready (VRAM free=%lu)",
                 (unsigned long)vramSpaceFree());
    return true;
}

/* The three planes and their targets, or nothing at all. */
static bool BandsCreate(void)
{
    for (unsigned i = 0; i < CTR_BANDS; ++i)
    {
        /* 5551: the GBA's own 15 bits and the one bit of cover a plane
         * needs, at half the VRAM of RGBA8 - 768 KiB for all three, which an
         * Old 3DS has beside the overworld's arenas; 1.5 MiB it did not. */
        if (!C3D_TexInitVRAM(&sBandTex[i], 512, 256, GPU_RGBA5551)) goto fail;
        C3D_TexSetFilter(&sBandTex[i], GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&sBandTex[i], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        /* No depth buffer: Citro2D draws in submission order, and clearing one
         * per plane per frame is memory traffic this path exists to avoid. */
        sBand[i] = C3D_RenderTargetCreateFromTex(&sBandTex[i], GPU_TEXFACE_2D, 0, -1);
        if (!sBand[i]) goto fail;
        ClearWhenMade(sBand[i]);
        sBandCount = i + 1;
    }
    return true;
fail:
    /* Two planes still give the interface its depth (see BandsShrink). */
    if (sBandCount >= 2)
    {
        unsigned i = sBandCount;

        if (sBand[i]) { ClearWhenMadeForget(sBand[i]); C3D_RenderTargetDelete(sBand[i]); }
        if (sBandTex[i].data) C3D_TexDelete(&sBandTex[i]);
        sBand[i] = NULL;
        memset(&sBandTex[i], 0, sizeof(sBandTex[i]));
        return true;
    }
    BandsRelease();
    return false;
}

/*
 * Plane i holds priority i on its own, except the last, which holds every
 * remaining priority so that anything blending with what is beneath it keeps
 * that beneath in the same surface. The last plane therefore always reaches
 * priority 3, sits at the screen plane and carries the backdrop; the others
 * start transparent and are stacked on top of it.
 */
static unsigned BandMask(unsigned band, unsigned count)
{
    (void)count;
    return sBandSlots[band];
}

static float BandDepth(unsigned band, unsigned count)
{
    return band + 1 < count ? (float)(CTR_PRIORITIES - 1 - band) : 0.0f;
}

/*
 * C2D_TargetClear for a plane. The GPU fills a 16-bit surface with the low
 * half of the value it is given, and C2D_TargetClear gives it the RGBA8 word,
 * whose low half is blue and alpha: opaque black came out blue, and any
 * backdrop some other colour. The colour is packed as RGBA5551 instead.
 */
static void PlaneClear(C3D_RenderTarget *target, uint32_t color)
{
    unsigned r = color & 255, g = (color >> 8) & 255, b = (color >> 16) & 255, a = color >> 24;

    C2D_Flush();
    C3D_FrameSplit(0);
    C3D_RenderTargetClear(target, C3D_CLEAR_ALL,
                          (r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | (a >= 128), 0);
}

/* Composes every depth plane into its own surface, with no displacement. */
/*
 * Where in a plane of these slots something blends, in GBA coordinates: the
 * box round its blending sprites, or false when a background blends, or the
 * screen is one whose sprites this cannot place - then the whole view.
 */
static bool BlendBox(unsigned slots, int *x0, int *y0, int *x1, int *y1)
{
    unsigned display = Reg(0), control = Reg(0x50);
    unsigned target1 = control & 63;
    bool alpha = ((control >> 6) & 3) == 1;
    /*
     * The 2D field's sprites are placed on its 400x240 view as DrawObjects
     * places them; the window partition keeps to the box (ComposeBand). The
     * fog, the ash and the sand are one picture over the whole view, the
     * weather's particles are repeated across it (DrawObjects): those take
     * the whole view.
     */
    bool field = !sStage && !sCentred && !sBattle && !sTransitionCompose;

    if (!(sStage || sCentred || field) || ((display & 0x6000) && !field)) return false;
    for (unsigned bg = 0; bg < 4 && alpha; ++bg)
        if ((target1 & (1u << bg)) && (display & (0x100u << bg)) && (slots & SLOT_BG(Reg(8 + bg * 2) & 3)))
            return false;
    *x0 = *y0 = 1 << 20;
    *x1 = *y1 = -(1 << 20);
    for (unsigned i = 0; i < 128; ++i)
    {
        unsigned attr0 = sMemory.oam[i * 4], attr1 = sMemory.oam[i * 4 + 1];
        unsigned mode = (attr0 >> 10) & 3, shape = attr0 >> 14;
        bool affine = (attr0 & 0x100) != 0, twice = affine && (attr0 & 0x200);
        int x, y, w, h;

        if ((!affine && (attr0 & 0x200)) || shape == 3) continue;
        if (!(slots & SLOT_OBJ((sMemory.oam[i * 4 + 2] >> 10) & 3))) continue;
        if (!(mode == 1 || (mode == 0 && alpha && (target1 & 16)))) continue;
        w = sObjDims[shape][attr1 >> 14][0] << twice;
        h = sObjDims[shape][attr1 >> 14][1] << twice;
        x = (int)(attr1 & 511);
        y = (int)(attr0 & 255);
        if (field)
        {
            if ((sFogOam[i >> 5] | sLatticeOam[i >> 5] | sParticleOam[i >> 5]) & (1u << (i & 31))) return false;
            if (x >= VIEW_RIGHT) x -= 512;
            if (sOamAnchored[i >> 5] & (1u << (i & 31))) y = OamUnwrapY(i, y);
            else if (y >= VIEW_BOTTOM) y -= 256;
            if (sFieldUi && (sScreenOam[i >> 5] & (1u << (i & 31))))
                PlaceScreenObject((int)i, (unsigned)w, (unsigned)h, false, &x, &y);
        }
        else
        {
            if (x + w > 512) x -= 512;
            if (y + h > 256) y -= 256;
        }
        if (x < *x0) *x0 = x;
        if (y < *y0) *y0 = y;
        if (x + w > *x1) *x1 = x + w;
        if (y + h > *y1) *y1 = y + h;
    }
    return *x0 < *x1;
}

/*
 * A plane whose layers blend needs what lies behind them to blend with, and
 * the planes behind it are other surfaces. So that plane is first given the
 * whole picture behind it - the backdrop and every further priority -
 * written to its colour but not its alpha, which stays clear: where its own
 * layers draw, they blend with the right colours and make the pixel opaque;
 * everywhere else it stays transparent and the plane behind shows at its own
 * depth. The logo over the intro's leaves, a sprite's shadow on the field,
 * keep their depth instead of flattening the frame to one plane.
 */
static void RenderBands(unsigned count, uint32_t backdrop)
{
    unsigned blending = BlendingPriorities();

    sParallax = 0;
    for (int band = (int)count - 1; band >= 0; --band)
    {
        unsigned mask = BandMask(band, count);
        bool last = band + 1 == (int)count, behind = !last && (blending & mask);
        uint32_t before;

        sLayerShift = 0;
        BlendForget();
        PlaneClear(sBand[band], last ? backdrop : behind ? backdrop & 0x00ffffff : 0);
        SceneOn(sBand[band]);
        if (behind)
        {
            C2D_Flush();
            C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_RED | GPU_WRITE_GREEN | GPU_WRITE_BLUE);
            unsigned top = mask;

            while (top & (top - 1)) top &= top - 1;
            int x0, y0, x1, y1;

            /* Every slot behind this plane's. */
            sPriorityMask = SLOTS_ALL & ~((top << 1) - 1);
            Blend(5, false, false);
            StageUnderlay();
            /* Only under what blends: the logo, not the whole scene again. */
            if (BlendBox(mask, &x0, &y0, &x1, &y1))
            {
                if (x0 > sClipX0) sClipX0 = x0;
                if (y0 > sClipY0) sClipY0 = y0;
                if (x1 < sClipX1) sClipX1 = x1;
                if (y1 < sClipY1) sClipY1 = y1;
                C2D_Flush();
                Scissor(sClipX0, sClipY0, sClipX1, sClipY1);
                sScissored = true;
            }
            if (sClipX0 < sClipX1 && sClipY0 < sClipY1)
            {
                /* The Game Corner's backdrop as well, for what blends over
                 * it - inside the scissor, so only under what blends. */
                CornerUnderlay();
                if (!(Reg(0) & 128)) Compose();
            }
            ClipToView();
            sScissored = false;
            C2D_Flush();
            C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
            C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
            BlendForget();
        }
        before = sStats.tiles;
        sPriorityMask = mask;
        if (last)
        {
            Blend(5, false, false);
            StageUnderlay();
            /* The Game Corner's backdrop (RenderEye draws it flat): on the
             * furthest plane, at the screen in both eyes, drawn once for
             * the two of them. */
            CornerUnderlay();
        }
        if (!(Reg(0) & 128)) Compose();
        C2D_Flush();
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
        /* The furthest plane carries the backdrop, so it is never empty. */
        sBandUsed[band] = sStats.tiles != before || band + 1 == (int)count;
    }
    sPriorityMask = SLOTS_ALL;
    /* Rendering to a texture and then sampling it needs a command split. */
    C3D_FrameSplit(0);
}

/* Stacks the depth planes on one screen buffer, each displaced by its depth. */
static void BlitBands(C3D_RenderTarget *target, unsigned count, float parallax)
{
    const Tex3DS_SubTexture logical = {CTR_GAME_WIDTH, CTR_GAME_HEIGHT, 0, 1,
        CTR_GAME_WIDTH / 512.0f, 1 - CTR_GAME_HEIGHT / 256.0f};

    BlendForget();
    SceneOn(target);
    C2D_ViewReset();
    /*
     * The furthest plane sits at the screen plane in both eyes and is opaque
     * all over (it carries the backdrop): it is copied, not blended, and
     * nothing needs clearing under it. The nearer planes blend over it.
     */
    C2D_Flush();
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C2D_DrawImageAt((C2D_Image){&sBandTex[count - 1], &logical}, 0, 0, 0, NULL, 1, 1);
    Blend(5, false, false);
    for (int band = (int)count - 2; band >= 0; --band)
        if (sBandUsed[band])
            C2D_DrawImageAt((C2D_Image){&sBandTex[band], &logical},
                            parallax * sPlaneRamp * BandDepth(band, count), 0, 0, NULL, 1, 1);
    C2D_Flush();
}

#ifndef CTR_SHOW_FPS
#define CTR_SHOW_FPS 1
#endif
#if CTR_SHOW_FPS
/*
 * The FPS counter (SHOW_FPS=1), shown when the options turn it on
 * (CtrSettings_ShowFps): a 3x5 pixel font, each pixel a 1x1 solid
 * rectangle, over a translucent box in the top-left corner. Drawn last, over
 * whatever the frame composed, and at zero parallax in both eyes.
 */
#define FPS_PIXEL 1.0f
#define FPS_ADVANCE (4 * FPS_PIXEL)

static const char *const sFpsGlyphs[] =
{
    "111101101101111", "010110010010111", "111001111100111", "111001111001111",
    "101101111001001", "111100111001111", "111100111101111", "111001001001001",
    "111101111101111", "111101111001111",
    "111100111100100", /* F */
    "111101111100100", /* P */
    "001001010100100", /* / */
};
#endif

/* Game frames a second, hidden ones included (CtrPlatform_FrameCount): the
 * game's speed, beside the pictures a second the counter always showed.
 * With real-time pacing (3ds_platform.c) the two differ on a slow frame. */
static float sGameFps;
static uint64_t sGameFpsMark;

/*
 * The field's poison step (fldeff_misc.c, Task_FieldPoisonEffect) is a mosaic
 * that swells to 10 pixels and back, and the mosaic is not composed here: the
 * step had a sound and nothing to see. It is a violet veil over the view
 * instead, as deep as the mosaic is coarse - on whatever drew the field, the
 * 2D layers or the voxel world, which reads no register at all.
 */
static unsigned PoisonVeil(void)
{
    unsigned size = Reg(0x4c) & 15;

    return size > 10 ? 150 : size * 15;
}

/*
 * The HOME button refused in a local wireless session (3ds_platform.c): the
 * system draws nothing, so the game shows it was pressed - a house crossed
 * out, in the top screen's top right corner, for a second and a half.
 */
static uint32_t sNoHomeUntil;

void CtrVideo_ShowNoHome(void)
{
    sNoHomeUntil = sStats.frames + 90;
}

static void DrawNoHome(C3D_RenderTarget *target)
{
    const float x = CTR_GAME_WIDTH - 34, y = 6, w = 28;
    const u32 white = C2D_Color32(255, 255, 255, 255), red = C2D_Color32(230, 40, 40, 255);

    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    BlendForget();
    SceneOn(target);
    C2D_ViewReset();
    Blend(5, false, false);
    C2D_DrawCircleSolid(x + w / 2, y + w / 2, 0, w / 2, C2D_Color32(0, 0, 0, 170));
    /* The house: its roof and its walls, a door cut out of them. */
    C2D_DrawTriangle(x + 6, y + 14, white, x + 14, y + 6, white, x + 22, y + 14, white, 0);
    C2D_DrawRectSolid(x + 8, y + 14, 0, 12, 8, white);
    C2D_DrawRectSolid(x + 12, y + 17, 0, 4, 5, C2D_Color32(0, 0, 0, 255));
    /* Crossed out. */
    C2D_DrawLine(x + 6, y + 6, red, x + 22, y + 22, red, 3, 0);
    C2D_Flush();
}

/*
 * The swoop's light over a finished picture of the top screen, the same in
 * both eyes (on the glass): the flat flash and a radial bloom from the middle
 * as it dies away. Four triangles and a rectangle at most, nothing allocated.
 */
static void DrawSwoopLight(C3D_RenderTarget *target, const VoxelSwoopFrame *fx)
{
    const float w = CTR_GAME_WIDTH, h = CTR_GAME_HEIGHT, cx = w * 0.5f, cy = h * 0.5f;
    uint32_t rgb = fx->colour & 0x00FFFFFFu;

    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    BlendForget();
    SceneOn(target);
    C2D_ViewReset();
    Blend(5, false, false);
    if (fx->bloom > 0.005f)
    {
        /* A diamond, bright in the middle and gone at the screen's edges. */
        uint32_t core = rgb | ((uint32_t)(fx->bloom * 0.9f * 255.0f) << 24), edge = rgb;
        float rx = w * 0.75f, ry = h * 0.9f;

        C2D_DrawTriangle(cx, cy, core, cx - rx, cy, edge, cx, cy - ry, edge, 0);
        C2D_DrawTriangle(cx, cy, core, cx, cy - ry, edge, cx + rx, cy, edge, 0);
        C2D_DrawTriangle(cx, cy, core, cx + rx, cy, edge, cx, cy + ry, edge, 0);
        C2D_DrawTriangle(cx, cy, core, cx, cy + ry, edge, cx - rx, cy, edge, 0);
    }
    if (fx->flash > 0.005f)
        C2D_DrawRectSolid(0, 0, 0, w, h, rgb | ((uint32_t)(fminf(fx->flash, 1.0f) * 255.0f) << 24));
    C2D_Flush();
}

/*
 * The Fly cinematic's cloud over a finished picture of the top screen
 * (voxel_fly.c): a wash of white, the whole cover once closed (whatever is
 * under it - the warp's black - must not show through), and two sheets of
 * the world's cloud coverage (CtrVoxel_CloudTexture, tileable A8) at
 * different scales drifting across each other, so the cloud churns rather
 * than fades. The texture repeats under the picture through its own wrap
 * mode: the image's texture rectangle just runs past 1. In 3D the sheets
 * stand in front of the screen: the fine one nearest the eye, the broad one
 * behind it, shifted apart by the slider (to the right in the left eye, as
 * StereoParallax has what is nearer than the screen).
 */
/*
 * ── The Fly sky scene ────────────────────────────────────────────────────
 *
 * Inside the cloud, with the SKY FLIGHT option on (3ds_fly_sky.h), the
 * flight is seen instead of hidden: the player on their Pokemon bursts out
 * over the cloud deck into the open sky, soars across it with the wind
 * streaking past and the region's wild flyers passing by, slower than us -
 * a flock or two, now and then a legendary tearing past - and dives back
 * into the deck as the picture goes white for the descent. The game's warp
 * and the new map's build happen under it (voxel_fly.c, VOXEL_FLY_SKY).
 *
 * All of it is 2D over the closed cloud cover: a sky gradient, three sheets
 * of the world's cloud texture as the deck in parallax, the sprites from
 * one atlas texture packed at the flight's start (the Pokemon's and the
 * natives' overworld frames from the follower data, the player's riding
 * frame), the wind as streaks. The way of travel sets the side: east and
 * we cross left to right; south and the whole path slopes down. In 3D the
 * layers stand at their depths about the screen.
 */
#define SKY_W ((float)CTR_GAME_WIDTH)
#define SKY_H ((float)CTR_GAME_HEIGHT)
#define SKY_ATLAS 256u
#define SKY_NATIVES 4u
#define SKY_FLOCKS 8u
#define SKY_FLOCK_MAX 5u
#define SKY_MARKS 26u
#define SKY_FLYER_SCALE 2.0f
#define SKY_FADE_IN 14u
#define SKY_FADE_OUT 16u
/* Emerald's own species numbers of the legendaries that may pass. */
#define SKY_SPECIES_RAYQUAZA 406
#define SKY_SPECIES_LATIAS 407
#define SKY_SPECIES_LATIOS 408

typedef struct
{
    Tex3DS_SubTexture sub[2]; /* two wingbeat frames */
    uint16_t width;
    bool ok;
} SkyActor;

typedef struct
{
    uint8_t actor;     /* index into native[], or SKY_NATIVES for the legend */
    uint8_t count;
    bool crossing;     /* flying the other way (drawn flipped) */
    uint16_t start;    /* scene frame it sets out */
    float y, speed, depth, phase;
} SkyFlock;

static struct
{
    bool loaded;
    C3D_Tex atlas;
    unsigned packX, packY, packRow;
    SkyActor flyer, rider, native[SKY_NATIVES], legend;
    unsigned natives;
    int8_t seatX, seatY;
    int sx;            /* +1: left to right (east), -1: right to left */
    float slope;       /* -1..1, the path's fall down the picture (south) */
    SkyFlock flock[SKY_FLOCKS];
    unsigned flocks;
    struct { float y, len, speed, phase; uint8_t alpha, thick; } mark[SKY_MARKS];
    uint32_t seed;
} sSky;

static uint32_t SkyRandom(void)
{
    sSky.seed = sSky.seed * 1664525u + 1013904223u;
    return sSky.seed >> 8;
}

static float SkyUnit(void)
{
    return (float)(SkyRandom() & 0xFFFF) / 65535.0f;
}

/* One frame of a 4bpp sprite into the atlas; false when it has no room. */
static bool SkyPackFrame(const CtrSkySprite *sprite, unsigned frame, Tex3DS_SubTexture *sub)
{
    unsigned w = sprite->width;
    uint16_t *data = sSky.atlas.data;
    const uint8_t *tiles = sprite->tiles + (size_t)frame * sprite->frameBytes;

    if (frame >= sprite->frames || (w != 32 && w != 64))
        return false;
    if (sSky.packX + w > SKY_ATLAS)
    {
        sSky.packX = 0;
        sSky.packY += sSky.packRow;
        sSky.packRow = 0;
    }
    if (sSky.packY + w > SKY_ATLAS)
        return false;
    for (unsigned y = 0; y < w; ++y)
        for (unsigned x = 0; x < w; ++x)
        {
            const uint8_t *tile = tiles + ((y / 8) * (w / 8) + x / 8) * 32;
            unsigned index = (tile[(y & 7) * 4 + (x & 7) / 2] >> ((x & 1) * 4)) & 15;

            data[CtrVideo_Texel(sSky.packX + x, sSky.packY + y, SKY_ATLAS)] =
                index ? CtrVideo_RGBA5551(sprite->palette[index]) : 0;
        }
    sub->width = (uint16_t)w;
    sub->height = (uint16_t)w;
    sub->left = (float)sSky.packX / SKY_ATLAS;
    sub->right = (float)(sSky.packX + w) / SKY_ATLAS;
    sub->top = 1.0f - (float)sSky.packY / SKY_ATLAS;
    sub->bottom = 1.0f - (float)(sSky.packY + w) / SKY_ATLAS;
    sSky.packX += w;
    if (w > sSky.packRow) sSky.packRow = w;
    return true;
}

/* An actor's two frames facing the way of travel. */
static void SkyPackActor(SkyActor *actor, const CtrSkySprite *sprite, unsigned firstFrame)
{
    actor->width = sprite->width;
    actor->ok = SkyPackFrame(sprite, firstFrame, &actor->sub[0])
             && SkyPackFrame(sprite, firstFrame + 1, &actor->sub[1]);
}

static void SkySceneFree(void)
{
    if (!sSky.loaded) return;
    C3D_TexDelete(&sSky.atlas);
    CtrFlySky_Release();
    memset(&sSky, 0, sizeof(sSky));
}

/* Readies the scene for a flight that way: false and the flight stays
 * inside the cloud (no sprite for the Pokemon, no memory). */
static bool SkySceneLoad(float dirX, float dirZ)
{
    CtrSkySprite sprite;
    uint16_t natives[SKY_NATIVES];
    unsigned facing;

    SkySceneFree();
    if (!CtrFlySky_FlyerSprite(&sprite, &sSky.seatX, &sSky.seatY))
        return false;
    if (!C3D_TexInit(&sSky.atlas, SKY_ATLAS, SKY_ATLAS, GPU_RGBA5551))
    {
        CtrLog_Write(CTR_LOG_ERROR, "FLY sky: no linear memory for the sprite atlas (free=%lu)",
                     (unsigned long)linearSpaceFree());
        memset(&sSky, 0, sizeof(sSky));
        return false;
    }
    sSky.loaded = true;
    C3D_TexSetFilter(&sSky.atlas, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&sSky.atlas, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    memset(sSky.atlas.data, 0, SKY_ATLAS * SKY_ATLAS * 2);
    sSky.seed = (uint32_t)sStats.frames * 2654435761u + 12345u;
    sSky.sx = dirX >= 0.0f ? 1 : -1;
    sSky.slope = fmaxf(-1.0f, fminf(1.0f, dirZ));
    facing = sSky.sx > 0 ? CTR_SKY_FRAME_RIGHT : CTR_SKY_FRAME_LEFT;
    SkyPackActor(&sSky.flyer, &sprite, facing);
    if (!sSky.flyer.ok)
    {
        SkySceneFree();
        return false;
    }
    if (CtrFlySky_RiderSprite(&sprite))
    {
        sSky.rider.width = sprite.width;
        sSky.rider.ok = SkyPackFrame(&sprite, sSky.sx > 0 ? 1 : 0, &sSky.rider.sub[0]);
        sSky.rider.sub[1] = sSky.rider.sub[0];
    }
    sSky.natives = CtrFlySky_Natives(natives, SKY_NATIVES);
    for (unsigned i = 0; i < sSky.natives; ++i)
        if (CtrFlySky_MonSprite(natives[i], 0, false, &sprite))
            SkyPackActor(&sSky.native[i], &sprite, facing);
    /* Now and then a legendary tears past. */
    if ((SkyRandom() & 7) == 0)
    {
        static const uint16_t legends[3] = {SKY_SPECIES_LATIOS, SKY_SPECIES_LATIAS, SKY_SPECIES_RAYQUAZA};

        if (CtrFlySky_MonSprite(legends[SkyRandom() % 3], 0, false, &sprite))
            SkyPackActor(&sSky.legend, &sprite, facing);
    }
    C3D_TexFlush(&sSky.atlas);
    /* The flocks: set out over the soar, each from the far edge, drifting
     * back past us (we are the faster), some crossing the other way. */
    {
        unsigned count = 3 + SkyRandom() % 3, usable = 0;

        for (unsigned i = 0; i < sSky.natives; ++i) usable += sSky.native[i].ok;
        sSky.flocks = 0;
        if (usable != 0)
            for (unsigned i = 0; i < count && sSky.flocks < SKY_FLOCKS - 1; ++i)
            {
                SkyFlock *f = &sSky.flock[sSky.flocks];
                unsigned pick = SkyRandom() % usable, actor = 0;

                for (unsigned j = 0; j < sSky.natives; ++j)
                    if (sSky.native[j].ok && pick-- == 0) { actor = j; break; }
                f->actor = (uint8_t)actor;
                f->count = (uint8_t)(1 + SkyRandom() % SKY_FLOCK_MAX);
                f->crossing = (SkyRandom() & 3) == 0;
                f->depth = SkyUnit();
                f->start = (uint16_t)(VOXEL_FLY_SKY_ENTER / 2
                                      + (unsigned)((float)i / (float)count * (VOXEL_FLY_SKY_SOAR - 30u))
                                      + SkyRandom() % 20);
                f->y = SKY_H * (0.12f + 0.55f * SkyUnit());
                f->speed = -(1.4f + 2.5f * f->depth + 1.1f * SkyUnit()) * (f->crossing ? 2.2f : 1.0f);
                f->phase = SkyUnit() * 100.0f;
                ++sSky.flocks;
            }
        if (sSky.legend.ok)
        {
            SkyFlock *f = &sSky.flock[sSky.flocks++];

            f->actor = SKY_NATIVES;
            f->count = 1;
            f->crossing = false;
            f->depth = 0.85f;
            f->start = (uint16_t)(VOXEL_FLY_SKY_ENTER + VOXEL_FLY_SKY_SOAR / 3);
            f->y = SKY_H * 0.26f;
            f->speed = 8.5f; /* overtaking us from behind */
            f->phase = 0.0f;
        }
    }
    for (unsigned i = 0; i < SKY_MARKS; ++i)
    {
        sSky.mark[i].y = SkyUnit() * SKY_H;
        sSky.mark[i].len = 30.0f + SkyUnit() * 90.0f;
        sSky.mark[i].speed = 12.0f + SkyUnit() * 12.0f;

        sSky.mark[i].phase = SkyUnit() * (SKY_W + 120.0f);
        sSky.mark[i].alpha = (uint8_t)(70 + SkyRandom() % 110);
        sSky.mark[i].thick = (SkyRandom() & 3) == 0 ? 2 : 1;
    }
    CtrLog_Write(CTR_LOG_VIDEO, "FLY sky: scene ready, %s, %u natives, %u flocks%s, rider %s",
                 sSky.sx > 0 ? "left to right" : "right to left", sSky.natives, sSky.flocks,
                 sSky.legend.ok ? " and a legendary" : "", sSky.rider.ok ? "on" : "missing");
    return true;
}

/* A travel-space x (forward positive, from the left edge) to the screen. */
static float SkyScreenX(float x)
{
    return sSky.sx > 0 ? x : SKY_W - x;
}

static float SkySmooth(float t)
{
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

/* Our Pokemon's place along the scene: in fast, easing to a soar past the
 * middle, then away fast; `speed` 0..1 how fast it is going. */
static void SkyFlyerPath(unsigned n, float *x, float *y, float *tilt, float *speed)
{
    const float E = VOXEL_FLY_SKY_ENTER, S = VOXEL_FLY_SKY_SOAR, X = VOXEL_FLY_SKY_EXIT;
    float p = (float)n / (float)VOXEL_FLY_SKY_FRAMES;
    float bob = sinf((float)n * (6.2831853f / 70.0f));
    float tx, ty = SKY_H * 0.46f + sSky.slope * SKY_H * 0.14f * (2.0f * p - 1.0f), dive = 0.0f, lift = 0.0f;

    if ((float)n < E)
    {
        float u = (float)n / E, ease = 1.0f - (1.0f - u) * (1.0f - u) * (1.0f - u);

        tx = -0.3f * SKY_W + (0.44f * SKY_W + 0.3f * SKY_W) * ease;
        ty -= (1.0f - u) * (1.0f - u) * 24.0f;
        lift = 1.0f - u;
        *speed = 1.0f - 0.65f * ease;
    }
    else if ((float)n < E + S)
    {
        float t = ((float)n - E) / S;

        tx = 0.44f * SKY_W + 0.10f * SKY_W * t;
        *speed = 0.35f;
    }
    else
    {
        float u = ((float)n - E - S) / X, ease = u * u * u;

        tx = 0.54f * SKY_W + (1.45f * SKY_W - 0.54f * SKY_W) * ease;
        ty += u * u * 30.0f;
        dive = u;
        *speed = 0.35f + 0.65f * SkySmooth(u * 1.5f);
    }
    *x = SkyScreenX(tx);
    *y = ty + 8.0f * bob;
    *tilt = (float)sSky.sx * (0.09f * cosf((float)n * (6.2831853f / 70.0f)) + 0.28f * dive - 0.12f * lift);
}

static void SkyDrawActor(const SkyActor *actor, unsigned frame, float cx, float cy, float angle,
                         float scale, bool flipped, uint8_t alpha)
{
    C2D_Image image = {&sSky.atlas, &actor->sub[frame & 1]};
    C2D_ImageTint tint;

    C2D_PlainImageTint(&tint, C2D_Color32(255, 255, 255, alpha), 0.0f);
    C2D_DrawImageAtRotated(image, cx, cy, 0.0f, angle, &tint, flipped ? -scale : scale, scale);
}

/* A sheet of the cloud texture as a band of the deck, scrolled back along
 * the way at its own pace (the nearer the faster), shifted for the eye,
 * its edges faded (`fadeTop`, `fadeBottom`: the colour's alpha there, as a
 * share of it) so no band shows a line. */
static void SkyDrawDeck(C3D_Tex *tex, unsigned n, float top, float bottom, float repeats, float speed,
                        uint32_t colour, float shift, float fadeTop, float fadeBottom)
{
    float h = bottom - top;
    float ru = repeats, rv = repeats * h / SKY_W;
    float u0 = (float)fmod((speed * (float)n * (float)sSky.sx - shift) * repeats / SKY_W, 1.0);
    Tex3DS_SubTexture sub = {(uint16_t)SKY_W, (uint16_t)h, u0, 0.73f, u0 + ru, 0.73f - rv};
    C2D_Image image = {tex, &sub};
    C2D_ImageTint tint;
    unsigned alpha = colour >> 24;
    uint32_t rgb = colour & 0x00FFFFFFu;
    uint32_t topColour = rgb | ((uint32_t)(alpha * fadeTop + 0.5f) << 24);
    uint32_t bottomColour = rgb | ((uint32_t)(alpha * fadeBottom + 0.5f) << 24);

    C2D_SetImageTint(&tint, C2D_TopLeft, topColour, 1.0f);
    C2D_SetImageTint(&tint, C2D_TopRight, topColour, 1.0f);
    C2D_SetImageTint(&tint, C2D_BotLeft, bottomColour, 1.0f);
    C2D_SetImageTint(&tint, C2D_BotRight, bottomColour, 1.0f);
    C2D_DrawImageAt(image, 0.0f, top, 0.0f, &tint, 1.0f, 1.0f);
}

/* The flocks at one depth: `near` those nearer than the middle plane. */
static void SkyDrawFlocks(unsigned n, float par, bool near)
{
    for (unsigned i = 0; i < sSky.flocks; ++i)
    {
        const SkyFlock *f = &sSky.flock[i];
        const SkyActor *actor = f->actor < SKY_NATIVES ? &sSky.native[f->actor] : &sSky.legend;
        float scale = 1.0f + 0.9f * f->depth, dir = f->speed < 0.0f ? -1.0f : 1.0f;
        float shift = (f->depth - 0.55f) * 6.0f * par;
        float x0 = f->speed < 0.0f ? 1.15f * SKY_W : -0.3f * SKY_W;
        float headX;

        if ((f->depth >= 0.5f) != near || !actor->ok || n < f->start)
            continue;
        headX = x0 + f->speed * (float)(n - f->start);
        for (unsigned m = 0; m < f->count; ++m)
        {
            float mx = headX - dir * (float)m * (14.0f + 10.0f * f->depth) * (actor->width / 32.0f);
            float my = f->y + (float)((m + 1) / 2) * 11.0f * ((m & 1) ? -1.0f : 1.0f) * scale
                     + 5.0f * sinf(((float)n + f->phase) * 0.09f + (float)m);
            float sxp = SkyScreenX(mx) + shift;
            unsigned wing;

            if (sxp < -80.0f || sxp > SKY_W + 80.0f)
                continue;
            wing = ((n + m * 3) / (f->actor < SKY_NATIVES ? 7u : 4u)) & 1;
            SkyDrawActor(actor, wing, sxp, my, 0.0f, scale, f->crossing, 255);
        }
        /* The legendary's wake. */
        if (f->actor == SKY_NATIVES)
            for (unsigned k = 0; k < 5; ++k)
            {
                float lx = SkyScreenX(headX - dir * (30.0f + 22.0f * (float)k)) + shift;
                uint32_t head = C2D_Color32(255, 255, 255, (uint8_t)(150 - 25 * k)), tail = C2D_Color32(255, 255, 255, 0);

                C2D_DrawRectangle(sSky.sx > 0 ? lx - 40.0f : lx, f->y - 6.0f + 3.0f * (float)k, 0, 40.0f, 1.0f,
                                  sSky.sx > 0 ? tail : head, sSky.sx > 0 ? head : tail,
                                  sSky.sx > 0 ? tail : head, sSky.sx > 0 ? head : tail);
            }
    }
}

static void DrawSkyScene(C3D_RenderTarget *target, int eye, unsigned n)
{
    C3D_Tex *cloud = CtrVoxel_CloudTexture();
    float slider = eye < 0 ? 0.0f : CtrScenario_Slider(osGet3DSliderState());
    float eyeSign = eye == 0 ? 1.0f : -1.0f; /* in front of the screen: right in the left eye */
    float par = eyeSign * slider;
    float fx, fy, tilt, speed;
    unsigned wing;

    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    BlendForget();
    SceneOn(target);
    C2D_ViewReset();
    Blend(5, false, false);
    /* The sky: deep above, pale at the deck. */
    {
        uint32_t zenith = C2D_Color32(58, 118, 212, 255), mid = C2D_Color32(132, 182, 236, 255);
        uint32_t horizon = C2D_Color32(196, 222, 246, 255);

        C2D_DrawRectangle(0, 0, 0, SKY_W, SKY_H * 0.36f, zenith, zenith, mid, mid);
        C2D_DrawRectangle(0, SKY_H * 0.36f, 0, SKY_W, SKY_H * 0.64f, mid, mid, horizon, horizon);
    }
    SkyFlyerPath(n, &fx, &fy, &tilt, &speed);
    /* The planes, far to near, with the flyers between them: high thin
     * cloud far behind, the far deck, the far flocks, the middle deck, the
     * near flocks and us, and a thin veil of the nearest cloud over all,
     * seen through. Every sheet fades in from its top edge. */
    if (cloud != NULL)
    {
        SkyDrawDeck(cloud, n, 0.0f, SKY_H * 0.34f, 2.6f, 1.3f, C2D_Color32(236, 244, 254, 110), -4.0f * par, 0.7f, 0.0f);
        SkyDrawDeck(cloud, n, SKY_H * 0.48f, SKY_H * 1.0f, 2.2f, 1.0f, C2D_Color32(222, 234, 250, 150), -3.0f * par, 0.0f, 1.0f);
    }
    SkyDrawFlocks(n, par, false);
    if (cloud != NULL)
        SkyDrawDeck(cloud, n, SKY_H * 0.60f, SKY_H * 1.0f, 1.6f, 2.4f, C2D_Color32(232, 241, 252, 190), 0.0f, 0.0f, 1.0f);
    SkyDrawFlocks(n, par, true);
    /* Us: the Pokemon, the player on its back. */
    {
        float cx = fx + 3.0f * par, cy = fy;
        float seatX = (float)(sSky.sx > 0 ? -sSky.seatX : sSky.seatX) * SKY_FLYER_SCALE;
        float seatY = (float)sSky.seatY * SKY_FLYER_SCALE;
        float c = cosf(tilt), s = sinf(tilt);

        wing = (n / (speed > 0.6f ? 4u : 7u)) & 1;
        SkyDrawActor(&sSky.flyer, wing, cx, cy, tilt, SKY_FLYER_SCALE, false, 255);
        if (sSky.rider.ok)
            SkyDrawActor(&sSky.rider, 0, cx + seatX * c - seatY * s, cy + seatX * s + seatY * c, tilt,
                         SKY_FLYER_SCALE, false, 255);
    }
    if (cloud != NULL)
        SkyDrawDeck(cloud, n, SKY_H * 0.70f, SKY_H * 1.0f, 1.1f, 4.6f, C2D_Color32(244, 248, 254, 120), 6.0f * par, 0.0f, 0.85f);
    /* The wind: streaks running back past us, thick and fast as we go. */
    for (unsigned i = 0; i < SKY_MARKS; ++i)
    {
        float span = SKY_W + 140.0f;
        float x = (float)fmod(sSky.mark[i].phase - sSky.mark[i].speed * (0.4f + speed) * (float)n, span);
        float len = sSky.mark[i].len * (0.5f + speed);
        uint8_t a = (uint8_t)((float)sSky.mark[i].alpha * (0.25f + 0.75f * speed));
        uint32_t bright = C2D_Color32(255, 255, 255, a), clear = C2D_Color32(255, 255, 255, 0);
        float sx0;

        if (x < 0.0f) x += span;
        x -= 120.0f;
        sx0 = sSky.sx > 0 ? x : SKY_W - x - len;
        sx0 += 7.0f * par;
        /* The head (forward) end bright, the tail clear. */
        if (sSky.sx > 0)
            C2D_DrawRectangle(sx0, sSky.mark[i].y, 0, len, sSky.mark[i].thick, clear, bright, clear, bright);
        else
            C2D_DrawRectangle(sx0, sSky.mark[i].y, 0, len, sSky.mark[i].thick, bright, clear, bright, clear);
    }
    /* Out of the white, and back into it. */
    {
        float white = 0.0f;

        if (n < SKY_FADE_IN)
            white = 1.0f - (float)n / (float)SKY_FADE_IN;
        else if (n + SKY_FADE_OUT >= VOXEL_FLY_SKY_FRAMES)
            white = (float)(n + SKY_FADE_OUT - VOXEL_FLY_SKY_FRAMES + 1) / (float)SKY_FADE_OUT;
        if (white > 0.0f)
            C2D_DrawRectSolid(0, 0, 0, SKY_W, SKY_H, C2D_Color32(238, 242, 248, (uint8_t)(fminf(white, 1.0f) * 255.0f + 0.5f)));
    }
    C2D_Flush();
}

/*
 * The flight's wind: wisps of cloud streaming out from the picture's centre
 * to its edges, over the world while the camera is up, as the air rushing
 * past. Each is a long thin strip of the world's cloud texture (tileable A8
 * noise, CtrVoxel_CloudTexture) stretched along its way and scrolled
 * outwards, clear at its inner end and whitest at the edge; under them the
 * edges are hazed white. Together they hide what the edges of the high view
 * show before it is built (the black of unbuilt chunks) and sell the rush.
 * Drawn under the cloud cover, a little in front of the screen in 3D.
 */
#define WIND_WISPS 30u
#define WIND_PARALLAX 3.0f
#define WIND_HAZE 0.6f     /* the edges' white haze, at full strength */
#define WIND_SPEED 0.9f    /* texture repeats a second, outwards */
static uint32_t WindRandom(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state >> 8;
}

static float WindUnit(uint32_t *state)
{
    return (float)(WindRandom(state) & 0xFFFF) / 65535.0f;
}

static void DrawWind(C3D_RenderTarget *target, float strength, int eye)
{
    const float w = CTR_GAME_WIDTH, h = CTR_GAME_HEIGHT, cx = w * 0.5f, cy = h * 0.5f;
    C3D_Tex *tex = CtrVoxel_CloudTexture();
    double seconds = (double)svcGetSystemTick() / SYSCLOCK_ARM11;
    float k = fminf(strength, 1.0f);
    float parallax = eye < 0 ? 0.0f : (eye == 0 ? 1.0f : -1.0f) * WIND_PARALLAX * CtrScenario_Slider(osGet3DSliderState());
    uint32_t state = 0x9E3779B9u; /* one set: the motion is the texture's */
    unsigned i;

    if (k <= 0.005f) return;
    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    BlendForget();
    SceneOn(target);
    C2D_ViewReset();
    Blend(5, false, false);
    /* The haze: the picture's edges whitened, clear towards the middle. */
    {
        uint32_t edge = C2D_Color32(240, 244, 250, (uint8_t)(k * WIND_HAZE * 255.0f + 0.5f));
        uint32_t none = C2D_Color32(240, 244, 250, 0);
        float band = h * 0.45f;

        C2D_DrawRectangle(0, 0, 0, w, band, edge, edge, none, none);
        C2D_DrawRectangle(0, h - band, 0, w, band, none, none, edge, edge);
        C2D_DrawRectangle(0, 0, 0, band, h, edge, none, edge, none);
        C2D_DrawRectangle(w - band, 0, 0, band, h, none, edge, none, edge);
    }
    if (tex == NULL) { C2D_Flush(); return; }
    for (i = 0; i < WIND_WISPS; ++i)
    {
        float angle = ((float)i + WindUnit(&state) * 0.8f) * (6.2831853f / (float)WIND_WISPS);
        float dx = cosf(angle), dy = sinf(angle);
        float inner = 50.0f + WindUnit(&state) * 80.0f;  /* clear of the middle */
        float outer = 280.0f;                             /* past the corners */
        float length = outer - inner;
        float thick = 8.0f + WindUnit(&state) * 16.0f;
        float alpha = k * (0.3f + WindUnit(&state) * 0.45f);
        float speed = WIND_SPEED * (0.7f + WindUnit(&state) * 0.6f);
        float v0 = WindUnit(&state);
        /* The noise drawn out along the wisp (few repeats over its length,
         * a sliver of it across), scrolled outwards. */
        float ru = 0.45f + WindUnit(&state) * 0.35f, rv = thick / 128.0f * 0.5f;
        float u0 = (float)fmod(-seconds * speed, 1.0);
        Tex3DS_SubTexture sub = {(uint16_t)length, (uint16_t)thick, u0, v0, u0 + ru, v0 + rv};
        C2D_Image image = {tex, &sub};
        C2D_ImageTint tint;
        uint32_t base = C2D_Color32(255, 255, 255, (uint8_t)(alpha * 255.0f + 0.5f));
        uint32_t tip = C2D_Color32(255, 255, 255, 0);
        float mx = cx + parallax + dx * (inner + length * 0.5f), my = cy + dy * (inner + length * 0.5f);

        C2D_SetImageTint(&tint, C2D_TopLeft, tip, 1.0f);
        C2D_SetImageTint(&tint, C2D_BotLeft, tip, 1.0f);
        C2D_SetImageTint(&tint, C2D_TopRight, base, 1.0f);
        C2D_SetImageTint(&tint, C2D_BotRight, base, 1.0f);
        C2D_DrawImageAtRotated(image, mx, my, 0.0f, angle, &tint, 1.0f, 1.0f);
    }
    C2D_Flush();
}

#define CLOUD_COVER_PARALLAX 7.0f /* the fine sheet's shift at the slider's top, pixels */

static void DrawCloudCover(C3D_RenderTarget *target, float amount, int eye)
{
    const float w = CTR_GAME_WIDTH, h = CTR_GAME_HEIGHT;
    C3D_Tex *tex = CtrVoxel_CloudTexture();
    double seconds = (double)svcGetSystemTick() / SYSCLOCK_ARM11;
    float k = fminf(amount, 1.0f);
    float parallax = eye < 0 ? 0.0f : (eye == 0 ? 1.0f : -1.0f) * CLOUD_COVER_PARALLAX * CtrScenario_Slider(osGet3DSliderState());
    C2D_ImageTint tint;

    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    BlendForget();
    SceneOn(target);
    C2D_ViewReset();
    Blend(5, false, false);
    /* The wash: a cool white. */
    C2D_DrawRectSolid(0, 0, 0, w, h, C2D_Color32(238, 242, 248, (uint8_t)(k * k * 255.0f + 0.5f)));
    if (tex != NULL)
    {
        /* Two sheets: a broad slow one and a finer quicker one over it. */
        static const struct { float repeats, speedU, speedV, depth; uint8_t alpha; } sheets[2] = {
            {1.6f, 0.030f, 0.012f, 0.45f, 225},
            {3.1f, -0.055f, 0.028f, 1.0f, 150},
        };

        for (unsigned s = 0; s < 2; ++s)
        {
            /* The eye's shift, in the sheet's own texture units (it is drawn
             * over the whole picture, so it is scrolled rather than moved). */
            float shift = parallax * sheets[s].depth * sheets[s].repeats / w;
            float u = (float)fmod(seconds * sheets[s].speedU - shift, 1.0), v = (float)fmod(seconds * sheets[s].speedV, 1.0);
            float ru = sheets[s].repeats, rv = sheets[s].repeats * h / w;
            Tex3DS_SubTexture sub = {(uint16_t)w, (uint16_t)h, u, v, u + ru, v + rv};
            C2D_Image image = {tex, &sub};

            C2D_PlainImageTint(&tint, C2D_Color32(255, 255, 255, (uint8_t)(k * (float)sheets[s].alpha + 0.5f)), 1.0f);
            C2D_DrawImageAt(image, 0.0f, 0.0f, 0.0f, &tint, 1.0f, 1.0f);
        }
    }
    C2D_Flush();
}

static void DrawPoisonVeil(C3D_RenderTarget *target, unsigned alpha)
{
    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    BlendForget();
    SceneOn(target);
    C2D_ViewReset();
    Blend(5, false, false);
    C2D_DrawRectSolid(0, 0, 0, CTR_GAME_WIDTH, CTR_GAME_HEIGHT, C2D_Color32(150, 60, 200, alpha));
}

#if CTR_SHOW_FPS
static void DrawFps(C3D_RenderTarget *target)
{
    unsigned fps = (unsigned)(sStats.fps + 0.5f), game = (unsigned)(sGameFps + 0.5f);
    unsigned glyphs[12], count = 0;
    char digits[8];
    int length = snprintf(digits, sizeof(digits), "%u/%u", fps > 999 ? 999 : fps, game > 999 ? 999 : game);

    glyphs[count++] = 10;   /* F */
    glyphs[count++] = 11;   /* P */
    glyphs[count++] = 5;    /* S, which is a 5 */
    glyphs[count++] = ~0u;  /* space */
    for (int i = 0; i < length; ++i)
        glyphs[count++] = digits[i] == '/' ? 12u : (unsigned)(digits[i] - '0');

    /* Whatever path composed the frame, the counter draws with the 2D
     * program and no depth test, as RenderVoxel's UI pass does. */
    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    BlendForget();
    SceneOn(target);
    C2D_ViewReset();
    Blend(5, false, false);
    C2D_DrawRectSolid(2, 2, 0, count * FPS_ADVANCE + FPS_PIXEL * 3, 5 * FPS_PIXEL + FPS_PIXEL * 4,
                      C2D_Color32(0, 0, 0, 160));
    for (unsigned i = 0; i < count; ++i)
    {
        if (glyphs[i] == ~0u) continue;
        const char *bits = sFpsGlyphs[glyphs[i]];
        float x = 2 + FPS_PIXEL * 2 + i * FPS_ADVANCE, y = 2 + FPS_PIXEL * 2;
        for (unsigned p = 0; p < 15; ++p)
            if (bits[p] == '1')
                C2D_DrawRectSolid(x + (p % 3) * FPS_PIXEL, y + (p / 3) * FPS_PIXEL, 0,
                                  FPS_PIXEL, FPS_PIXEL, C2D_Color32(255, 255, 255, 255));
    }
    C2D_Flush();
}
#endif

/*
 * The battle speed badge (CtrPlatform_BattleSpeedBadge): ">> X2", ">> X1.5"
 * in the top right corner, in the FPS counter's 3x5 pixel font at 3x, over a
 * translucent box. Drawn last, like the counter, at zero parallax.
 */
#define BADGE_PIXEL 3.0f
static const char *const sBadgeGlyphs[] =
{
    "111101101101111", "010110010010111", "111001111100111", "111001111001111",
    "101101111001001", "111100111001111", "111100111101111", "111001001001001",
    "111101111101111", "111101111001111",
    "101101010101101", /* X */
    "000000000000010", /* . */
};

static void DrawSpeedBadge(C3D_RenderTarget *target, int halves)
{
    char text[16];
    unsigned glyphs[6], count = 0;
    float width = 0.0f, x, y;
    const float arrows = 2 * (3 * BADGE_PIXEL + BADGE_PIXEL), pad = 2 * BADGE_PIXEL;
    const u32 white = C2D_Color32(255, 255, 255, 255);

    halves = halves < 2 ? 2 : halves > 8 ? 8 : halves;
    snprintf(text, sizeof(text), "X%d%s", halves / 2, halves & 1 ? ".5" : "");
    for (const char *c = text; *c && count < 6; ++c)
        glyphs[count++] = *c == 'X' ? 10u : *c == '.' ? 11u : (unsigned)(*c - '0');
    for (unsigned i = 0; i < count; ++i)
        width += (glyphs[i] == 11 ? 2 : 4) * BADGE_PIXEL;
    x = CTR_GAME_WIDTH - 4 - (pad + arrows + width + pad);
    y = 4;

    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    BlendForget();
    SceneOn(target);
    C2D_ViewReset();
    Blend(5, false, false);
    C2D_DrawRectSolid(x, y, 0, pad + arrows + width + pad, 5 * BADGE_PIXEL + 2 * pad, C2D_Color32(0, 0, 0, 160));
    x += pad;
    y += pad;
    /* Two arrowheads: fast forward. */
    for (int i = 0; i < 2; ++i)
    {
        float ax = x + i * 4 * BADGE_PIXEL;

        C2D_DrawTriangle(ax, y, white, ax, y + 5 * BADGE_PIXEL, white, ax + 3 * BADGE_PIXEL, y + 2.5f * BADGE_PIXEL, white, 0);
    }
    x += arrows;
    for (unsigned i = 0; i < count; ++i)
    {
        const char *bits = sBadgeGlyphs[glyphs[i]];

        for (unsigned p = 0; p < 15; ++p)
            if (bits[p] == '1')
                C2D_DrawRectSolid(x + (p % 3) * BADGE_PIXEL, y + (p / 3) * BADGE_PIXEL, 0, BADGE_PIXEL, BADGE_PIXEL, white);
        x += (glyphs[i] == 11 ? 2 : 4) * BADGE_PIXEL;
    }
    C2D_Flush();
}

/*
 * The bottom screen runs some of the game's menus without showing them (the
 * party menu, the bag): while it does, the top screen keeps the last frame it
 * presented, the world as it was when the menu opened. Nothing is rendered;
 * the frame is only paced to the display.
 */
static bool sHoldTop;

/*
 * The Pokédex draws its device over the held frame, and a screen chosen from
 * it opens with the top still held (CtrBottom_LeaveTo): the world's frame is
 * kept aside as the device comes up and put back as it goes, the top still
 * held. Each eye's colour buffer, copied whole, in linear memory meanwhile.
 */
static struct
{
    void *eye[2];
    bool kept, dexShown, failed;
} sHeldTop;

static void HeldTopForget(void)
{
    for (int i = 0; i < 2; ++i)
    {
        if (sHeldTop.eye[i]) linearFree(sHeldTop.eye[i]);
        sHeldTop.eye[i] = NULL;
    }
    sHeldTop.kept = sHeldTop.dexShown = false;
}

static u32 TargetBytes(const C3D_RenderTarget *target)
{
    const C3D_FrameBuf *fb = &target->frameBuf;

    return (u32)fb->width * fb->height * (fb->colorFmt == GPU_RB_RGBA8 ? 4 : fb->colorFmt == GPU_RB_RGB8 ? 3 : 2);
}

/* Outside a frame, the last one having left the top screen alone (held). */
static void HeldTopKeep(void)
{
    C3D_RenderTarget *eyes[2] = { sTop, sStereo ? sTopRight : NULL };

    if (!sTop) return;
    for (int i = 0; i < 2; ++i)
    {
        u32 size;

        if (!eyes[i]) continue;
        size = TargetBytes(eyes[i]);
        sHeldTop.eye[i] = linearAlloc(size);
        if (!sHeldTop.eye[i])
        {
            if (!sHeldTop.failed)
                CtrLog_Write(CTR_LOG_ERROR, "video: no linear memory to keep the held top screen (free=%lu); "
                             "the Pokédex's device stays on it", (unsigned long)linearSpaceFree());
            sHeldTop.failed = true;
            HeldTopForget();
            return;
        }
        C3D_SyncTextureCopy((u32 *)eyes[i]->frameBuf.colorBuf, 0, (u32 *)sHeldTop.eye[i], 0, size,
                            GX_TRANSFER_RAW_COPY(1));
    }
    sHeldTop.kept = true;
}

/* In a frame: the kept frame back in the top's buffers, which go out at its end. */
static void HeldTopPutBack(void)
{
    C3D_RenderTarget *eyes[2] = { sTop, sTopRight };

    for (int i = 0; i < 2; ++i)
        if (sHeldTop.eye[i] && eyes[i])
        {
            C3D_SyncTextureCopy((u32 *)sHeldTop.eye[i], 0, (u32 *)eyes[i]->frameBuf.colorBuf, 0,
                                TargetBytes(eyes[i]), GX_TRANSFER_RAW_COPY(1));
            C3D_FrameDrawOn(eyes[i]);
        }
    bool stereo = sHeldTop.eye[1] != NULL && sTopRight;
    if (stereo != sStereo) { gfxSet3D(stereo); sStereo = stereo; }
    CtrLog_Write(CTR_LOG_VIDEO, "top screen: the held frame back from under the Pokédex");
    HeldTopForget();
}

void CtrVideo_HoldTop(bool hold)
{
    if (hold != sHoldTop)
        CtrLog_Write(CTR_LOG_VIDEO, "top screen %s at frame %lu", hold ? "held" : "released",
                     (unsigned long)sStats.frames);
    sHoldTop = hold;
    if (!hold) HeldTopForget();
}

bool CtrVideo_TopHeld(void) { return sHoldTop; }

#ifdef CTR_FRAME_DUMP
#define CTR_DUMP_SLIDER CtrDumpSlider()
/* Measurement builds: the 3D slider from sdmc:/3ds/emerald3ds/slider.txt. */
static __attribute__((unused)) float CtrDumpSlider(void)
{
    static int read;
    static float value;

    if (!read)
    {
        FILE *file = fopen("sdmc:/3ds/emerald3ds/slider.txt", "r");

        read = 1;
        if (file) { value = 1.0f; fclose(file); }
    }
    return value;
}
#endif

/*
 * The voxel world's VRAM follows its option (CtrVoxel_ParkVram): given back a
 * second after the world was last wanted, claimed again as soon as it is.
 * Claimed again after the 2D surfaces held for other screens go - they come
 * back on demand - so that the arenas find VRAM in one piece, as at start-up.
 * Outside the frame, like every release here.
 */
#define VOXEL_PARK_FRAMES 60u
#define VOXEL_UNPARK_RETRY_FRAMES 120u

static void VoxelVramManage(void)
{
    static uint32_t sUnwanted, sRetryFrame;
    bool wanted = CtrSettings_Voxel();

    if (!wanted)
    {
        /* A voxel battle ends when the field's world next updates: with the
         * world switched off on the way back, never, and its VRAM stayed
         * held (the scene cycling test: the next 2D battle found no room for
         * its scene surface). No battle on show: it is over. */
        if (CtrVoxel_InBattle() && !sBattle && !sBattleRequested)
            CtrVoxel_EndBattle();
        /* Not under the swoop: parking stalls the frame (a quarter second on
         * a New 3DS), and the world is still on show through it. */
        if (++sUnwanted >= VOXEL_PARK_FRAMES && CtrVoxel_HoldsVram() && !CtrVoxel_InBattle()
            && !VoxelSwoop_Active(&sSwoop))
        {
            StereoSurfaceRelease();
            CtrVoxel_ParkVram();
            /* The planes the 2D field asked for when the world still held
             * this VRAM wait for their retry no longer: with the world just
             * switched off they waited 300 frames, flat. */
            if (sBandsFailed)
                sBandsRetryFrame = sStats.frames;
        }
        return;
    }
    sUnwanted = 0;
    if (CtrVoxel_HoldsVram() || sStats.frames < sRetryFrame)
        return;
    /* The field's planes still at depth: their parallax eases out first
     * (CTR_PLANE_RAMP_FRAMES), and the world comes back to a flat field. */
    if (sBandsReady && sPlaneRamp > 0.0f)
        return;
    StereoSurfaceRelease();
    if (sBandsReady) BandsRelease();
    LayersRelease();
    if (!sBattleRequested && !sTransitionRequested) { SceneRelease(); SlotsRelease(); }
    ObjWindowRelease();
    if (sBottom && !sBottomInUse) BottomRelease();
    if (!CtrVoxel_UnparkVram())
    {
        sRetryFrame = sStats.frames + VOXEL_UNPARK_RETRY_FRAMES;
        CtrLog_Write(CTR_LOG_ERROR, "VOXEL: no VRAM to bring the world back (free=%lu, largest=%lu); retrying",
                     (unsigned long)vramSpaceFree(), (unsigned long)CtrGpuMem_LargestVram());
    }
}

/*
 * The Pokédex on the top screen (3ds_dex_top.c) while its pages are on the
 * bottom: its textures filled now that FrameBegin has waited for the GPU,
 * then drawn per eye with the 2D program and no depth test, as the FPS
 * counter is.
 */
static void RenderDexTop(bool stereo, float slider)
{
    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    BlendForget();
    CtrDexTop_Upload();
    Blend(5, false, false);
    SceneOn(sTop);
    C2D_ViewReset();
    CtrDexTop_DrawEye(stereo ? 1.0f : 0.0f, slider);
    if (stereo)
    {
        SceneOn(sTopRight);
        C2D_ViewReset();
        CtrDexTop_DrawEye(-1.0f, slider);
    }
    C2D_Flush();
    BlendForget();
}

void CtrVideo_Present(void)
{
    /* The frame's entry tick: its preparation, before FrameBegin. */
    uint64_t entry = svcGetSystemTick();

    sFrameFlat = 0;
    sStats.stereoSampleMs = sStats.stereoEyesMs = 0.0f;
    if (!sMemory.regs) CtrPlatform_Fatal("VIDEO has no logical memory bound");
    /* The PokéNav, the PC's boxes and the bag are drawn on the bottom screen, whether
     * or not the top is held. */
    bool wanted = BottomScreen(sCentredRequested) && !sStageRequested;
    bool native = BottomNative(sCentredRequested);
    bool bottom = BottomReady(wanted && !native);
    /* The Pokédex drawn natively down there still has its device up here:
     * the top screen is drawn as with its picture down there (down), the
     * bottom one's picture is not. */
    bool down = bottom || (wanted && native && CtrDexTop_Active());
    bool dexUp = down && CtrDexTop_Active();
    sBottomInUse = bottom;
    if (dexUp)
    {
        if (sHoldTop && !sHeldTop.dexShown && !sHeldTop.kept) HeldTopKeep();
        sHeldTop.dexShown = true;
    }
    else if (sHeldTop.kept && !sHoldTop)
        HeldTopForget();
    else if (!sHeldTop.kept)
        sHeldTop.dexShown = false;
    /* Nor is the shop's BUY screen ever shown on the top screen while its
     * surface down there is not ready yet, nor a screen the bottom one
     * draws natively. */
    if ((sHoldTop || sCentredRequested == CTR_CENTRED_SHOP || (wanted && native)) && !down)
    {
        sStats.mode = CTR_MODE_OTHER;
        sStats.route = CTR_ROUTE_BLANK;
        sStats.gapMs = sStats.arriveMs = sStats.prepMs = 0.0f;
        /* The Pokédex gone with the top still held: its frame back, once. */
        if (sHeldTop.kept && sHoldTop && C3D_FrameBegin(C3D_FRAME_SYNCDRAW))
        {
            HeldTopPutBack();
            C3D_FrameEnd(0);
        }
        else
            gspWaitForVBlank();
        ++sStats.frames;
        return;
    }
    /* Wait for previous GPU work before editing the atlas. C3D owns VBlank
     * pacing and swap: no gfxSwapBuffers/gspWaitForVBlank in this path. */
    /* Outside the frame, where deleting a render target may wait for the GPU. */
    /* A battle transition is flat and needs the battle scene's surface, which
     * the planes' VRAM is what keeps out on the 2D field: they go first. */
    if (sBandsReady && (sPlaneReleaseAsked || sTransitionRequested
                        || sStats.frames - sBandsUsedFrame > CTR_BANDS_IDLE_FRAMES))
    {
        BandsRelease();
        CtrLog_Write(CTR_LOG_VIDEO, "3D depth planes released (VRAM free=%lu)",
                     (unsigned long)vramSpaceFree());
    }
    else if (sBandsReady && sPlaneShrinkAsked && sBandCount > 2)
    {
        BandsShrink();
        CtrLog_Write(CTR_LOG_VIDEO, "3D depth planes: %u, the rest to a stage layer (VRAM free=%lu)",
                     sBandCount, (unsigned long)vramSpaceFree());
    }
    sVramAsked = sPlaneReleaseAsked;
    sPlaneReleaseAsked = sPlaneShrinkAsked = false;
    if (sLeavesShown && sLeavesTex[0].data && sStats.frames - sLeavesUsed > 120)
    {
        LeavesRelease();
        sLeavesShown = false;
    }
    if (sStats.frames - sCornerUsed > 120)
    {
        if (sCornerTex[0].data) CornerArtRelease();
        sCornerFailed = false;
    }
    if (sBandsWanted && !sTransitionRequested)
    {
        sBandsWanted = false;
        BandsReady();
    }
    VoxelVramManage();
    StereoSurfaceManage();
    /*
     * The view of this frame. The voxel overworld is never a stage, so a stage
     * only waits on the voxel decision in the unlikely case both are asked.
     */
    sStage = sStageRequested;
    sStageFaded = sStage && sStageFadedRequested;
    sStageZoomed = sStage && sStageZoomedRequested;
    sCentred = sCentredRequested != CTR_CENTRED_NONE && !sStage;
    sCentredScreen = sCentred ? sCentredRequested : CTR_CENTRED_NONE;
    sBattle = sBattleRequested && !sStage && !sCentred;
    sInBattle = sBattleRequested;
    sTransition = sTransitionRequested && sLineRegs && !sStage && !sCentred && !sBattle;
    sZoom = sBattle ? CTR_BATTLE_ZOOM : 1.0f;
    /* GBA (120, 112) - the middle of the scene's bottom edge - on screen (200, 192). */
    sOffX = sBattle ? CTR_GAME_WIDTH / 2 - 120 * sZoom : 0.0f;
    sOffY = sBattle ? CTR_GAME_HEIGHT - 48 - 112 * sZoom : 0.0f;
    sShiftZoom = sZoom;
    if (sStage || sCentred)
    {
        sViewX = CTR_STAGE_X;
        /* The bottom screen's game screens, the PokeNav too, are the
         * picture 1:1 in its window. */
        sViewY = CTR_STAGE_Y;
    }
    else if (sBattle)
    {
        sViewX = sViewY = 0;
    }
    else
    {
        sViewX = sViewY = 0;
    }
    ClipToView();
    /* The voxel overworld draws the field itself when it is switched on. */
    /* ... and through the swoop (voxel_swoop.c), from the frame the option
     * goes on: its crossfade composes the 2D field over the voxel world. */
    sFieldLayers = !sStage && !sCentred && !sBattle && CtrGame_IsOverworld()
                   && (!CtrSettings_Voxel() || VoxelSwoop_Active(&sSwoop) || sFieldMode == FIELD_MODE_PIXEL);
    /* The field in 2D or in voxel: the audit found every write announced there. */
    VramTrustFrame(CTR_VRAM_TRUST && !sStage && !sCentred && !sBattle && CtrGame_IsOverworld() && !sTransition
                   && !sFieldBanner && !CtrGame_FieldRawVram());
    LayersPrepare();
    ScenePrepare();
    SlotsPrepare();
    SceneBlendPrepare();
    ObjWindowPrepare();
    sVramAsked = false;
    uint64_t waitStart = svcGetSystemTick();
    CtrVoxel_NoteFrameWait(waitStart);
    if (!C3D_FrameBegin(C3D_FRAME_SYNCDRAW)) return;
    /* A late clear wiped the scene surface's kept scenery, if it was that. */
    if (ClearsRun()) sSceneCached = sSceneEmpty = false;
    /* Before anything samples what the CPU wrote since the last frame. */
    TexCacheClear();
    sUploadCommands = sAnimCommands = 0;
    sRenderReserve = sBattle ? RENDER_RESERVE_MOST : RENDER_RESERVE;
    Flat2D();
    uint64_t start = svcGetSystemTick();
    sStats.waitMs = (start - waitStart) * 1000.0 / SYSCLOCK_ARM11;
    /* FrameBegin waited for the last frame's drawing: this is its time. Read
     * after FrameEnd, as it was, it was the frame before that one's. */
    sStats.gpuMs = C3D_GetDrawingTime();
    {
        /* For the frame profiler: the interval between two FrameBegins (a
         * frame shown twice is over 24 ms), and how far into it the CPU
         * came back (past 16.0 ms the CPU missed the VBlank). */
        static uint64_t sPerfLastBegin;

        sStats.gapMs = sPerfLastBegin ? (start - sPerfLastBegin) * 1000.0f / SYSCLOCK_ARM11 : 0.0f;
        sStats.arriveMs = sPerfLastBegin ? (waitStart - sPerfLastBegin) * 1000.0f / SYSCLOCK_ARM11 : 0.0f;
        sStats.prepMs = (waitStart - entry) * 1000.0f / SYSCLOCK_ARM11;
        sPerfLastBegin = start;
    }
#ifdef CTR_FRAME_DUMP
    /* The GPU has finished the last frame: its screens, raw, at fixed frames. */
#ifdef CTR_DUMP_OBJWIN
    static unsigned dumped;
    if (sObjWin.ready && sStats.frames % 4 == 0 && dumped++ < 60)
#elif defined(CTR_DUMP_BATTLE)
    static unsigned dumped;
    static uint32_t battleStart;
    if (!sBattle) battleStart = 0;
    else if (!battleStart) battleStart = sStats.frames;
#ifndef CTR_DUMP_FROM
#define CTR_DUMP_FROM 0
#define CTR_DUMP_STEP 30
#endif
    if (sBattle && sStats.frames - battleStart >= CTR_DUMP_FROM && sStats.frames % CTR_DUMP_STEP == 0 && dumped++ < 40)
#elif defined(CTR_DUMP_SWOOP)
    /* Every frame the swoop plays, and the 150 after the option changed
     * (the frames before the swoop starts are where a black one would be),
     * up to 1600 in all; each dumped frame logged with the route of the
     * picture it holds (the frame before this one). */
    static unsigned dumped, sinceToggle = 1000;
    static bool lastVoxelOption;
    if (CtrSettings_Voxel() != lastVoxelOption) { lastVoxelOption = CtrSettings_Voxel(); sinceToggle = 0; }
    else if (sinceToggle < 1000) ++sinceToggle;
    bool dumping = VoxelSwoop_Active(&sSwoop) || sinceToggle < 150;
    if (dumping && dumped < 1600)
        CtrLog_Write(CTR_LOG_VIDEO, "DUMP f=%lu route=%u mode=%u field=%u swoop=%u/%u", (unsigned long)sStats.frames,
                     (unsigned)sStats.route, (unsigned)sStats.mode, sFieldMode, (unsigned)sSwoop.phase, sSwoop.frame);
    if (dumping && dumped++ < 1600)
#else
    if (sStats.frames >= 150 && (sStats.frames - 150) % 150 == 0 && sStats.frames <= 6000)
#endif
    {
        static FILE *dump;

        if (!dump) dump = fopen("sdmc:/3ds/emerald3ds/frames.raw", "wb");
        if (dump)
        {
            uint32_t frame = sStats.frames;

            fwrite(&frame, 4, 1, dump);
            fwrite(sTop->frameBuf.colorBuf, 4, 240 * 400, dump);
            if (sStereo && sTopRight) fwrite(sTopRight->frameBuf.colorBuf, 4, 240 * 400, dump);
            else { static uint32_t zero[240 * 400]; fwrite(zero, 4, 240 * 400, dump); }
            fflush(dump);
        }
    }
#endif
    /*
     * FrameBegin waits for the last frame's drawing and for a free screen
     * buffer, so the time between two returns is about a whole number of
     * display frames, but not exactly (gaps of 25-31 ms are seen): over 24 ms
     * is a frame the screen showed twice. Logged with what the frame before it spent - the present
     * (and the voxel builds in it), then the game and its VBlank handler - so
     * that a slow renderer can be told from a slow game on hardware.
     */
    {
        static uint64_t sLastBegin;
        static unsigned sDropsLogged;
        /* A screen that runs at 30 for a while is one finding, not a line per
         * frame: a steady run is logged once a second with how many frames it
         * dropped, and only a hitch of three frames or more is always logged. */
        static uint32_t sLastDropLogged;
        static unsigned sDropsQuiet;
        float gap = sLastBegin ? (start - sLastBegin) * 1000.0f / SYSCLOCK_ARM11 : 0.0f;
        /*
         * Which of the two missed the VBlank: the CPU, if it came back to
         * FrameBegin after it (`arrive` past 16.0, the VBlank's own jitter
         * aside: 16.7 left a 16.7 arrival unnamed), or else the GPU, whose
         * last frame ended `gpuEnd` after that frame began - when the GPU
         * was started (early in the frame, or at FrameEnd) plus the drawing,
         * which FrameBegin has waited for.
         */
        float arrive = sLastBegin ? (waitStart - sLastBegin) * 1000.0f / SYSCLOCK_ARM11 : 0.0f;
        float gpuEnd = sStats.gpuStartMs + sStats.gpuMs;

        if (gap > 24.0f && sStats.frames > 120 && sDropsLogged < 3000)
        {
            if (gap < 45.0f && sStats.frames - sLastDropLogged < 60)
                ++sDropsQuiet;
            else
            {
                const CtrTiming *timing = CtrPlatform_GetTiming();
                const CtrVoxelStats *voxel = CtrVoxel_GetStats();
                float logMs, logAgo;

                CtrLog_LastDrain(&logMs, &logAgo);

                char voxelPart[224];

                ++sDropsLogged;
                sLastDropLogged = sStats.frames;
                /* The voxel figures only for a frame that updated the voxel
                 * world: on a 2D frame they were the last voxel frame's, and
                 * read as the battle's. Drafts are their own figure, not part
                 * of chunks=. */
                if (sStats.voxelFrame)
                    snprintf(voxelPart, sizeof(voxelPart),
                             "voxel=%.1f: world=%.1f atlas=%.1f chunks=%.1f sprites=%.1f "
                             "stream=%.1f anim=%.1f draft=%.1f) after=%.1f/%.1f tail=%.1f/%.1f",
                             voxel->updateMs, voxel->worldMs, voxel->atlasMs,
                             voxel->meshMs - voxel->atlasMs - voxel->draftMs, voxel->spritesMs,
                             voxel->streamMs, voxel->animMs, voxel->draftMs,
                             voxel->afterMs, voxel->afterBudgetMs, voxel->tailMs, voxel->tailExpectedMs);
                else
                    snprintf(voxelPart, sizeof(voxelPart), "voxel=-)");
                CtrLog_Write(CTR_LOG_VIDEO, "DROP frame=%lu gap=%.1fms (+%u quiet): present=%.1f "
                             "(%s gpu=%.1f gpuStart=%.1f game=%.1f audio+vblank=%.1f "
                             "arrive=%.1f gpuEnd=%.1f%s bottom=%.1f pre=%.1f log=%.1f@%.0f",
                             (unsigned long)sStats.frames, gap, sDropsQuiet, sStats.cpuMs,
                             voxelPart, sStats.gpuMs, sStats.gpuStartMs,
                             timing->gameMs, timing->vblankMs, arrive, gpuEnd,
                             arrive > 16.0f ? " (cpu late)" : gpuEnd > 15.5f ? " (gpu late)" : "",
                             timing->bottomMs, (waitStart - entry) * 1000.0f / SYSCLOCK_ARM11,
                             logMs, logAgo);
                sDropsQuiet = 0;
            }
        }
        sLastBegin = start;
    }
    if (sStats.uploads) { ++sDecodeFrames; sDecodeFramesTiles += sStats.uploads; }
    if (sStats.cells) { ++sCellFrames; sCellsTotal += sStats.cells; }
    sStats.tiles = sStats.uploads = sStats.sprites = sStats.cells = 0;
    sFastUsed = 0;
    sBgTicks = sObjTicks = 0;
    sStats.display = Reg(0);
    if (sUsed > CACHE_COUNT - 4096) { memset(sHash, 0, sizeof(sHash)); sUsed = 0; ++sCacheGeneration; }
    PORT_PROF_BEGIN(palette);
    CtrDayNight_Update();
    UpdatePalette();
    PORT_PROF_END(palette, PORT_PROF_PALETTE);
    if (sStage || sBattle) RecordScroll();
    /* The shown backdrop, faded: sPalette may hold the unfaded one. */
    uint16_t backdrop = (Reg(0) & 128) ? 0x7fff : sMemory.palette[0] & 0x7fff;
    uint32_t rgb = CtrVideo_RGBA8(backdrop, true);
    uint32_t clear = C2D_Color32(rgb >> 24, rgb >> 16, rgb >> 8, 255);
    sBackdropShown = clear;
    /*
     * The 3D slider decides the separation, and at zero the right eye is not
     * composed at all: with 3D off this is the same single pass as before.
     * Whole pixels only, so every layer stays on the pixel grid in both eyes.
     */
    bool field = !sStage && !sCentred && !sBattle && CtrGame_IsOverworld();
    /* Azahar passes its slider only with factor_3d set in its config; a
     * profiling build can fix it (-DCTR_FORCE_SLIDER=1.0f) instead, and a
     * measured scenario can fix it too (3ds_scenario.c). Read before the
     * voxel update: the world is drawn per eye from it (CtrVoxel_SetStereo). */
#ifdef CTR_FORCE_SLIDER
    float slider = CTR_FORCE_SLIDER;
#else
    float slider = CtrScenario_Slider(osGet3DSliderState());
#endif
    /*
     * Preparing the voxel frame is part of the decision. If the atlas or the
     * mesh could not be built this frame, the 2D compositor draws it: leaving
     * the previous map's geometry on screen would show the wrong place.
     */
    /* The overworld, drawn in 3D or - while its first atlas or mesh is still
     * being made - by the 2D compositor. Either way it never takes the depth
     * planes: holding them there is what kept the overworld's atlas out of
     * VRAM for good, the 2D picture standing in for it frame after frame. */
    /* Opt-in from the bottom screen's options (CtrSettings_Voxel). */
    bool voxelMap = field && CtrVoxel_IsAvailable();
    bool wantVoxel = voxelMap && CtrSettings_Voxel();
    /*
     * The swoop between the renderers (sSwoop), decided before the update so
     * the camera's first frame is its own. The option gone off: the voxel
     * world stays up for the camera's rise. The 2D field about to become the
     * voxel world: the flash starts now, undone below if the world is not
     * drawn after all (still warming up).
     */
    if (!down && !VoxelSwoop_Active(&sSwoop))
    {
        if (sFieldMode == FIELD_MODE_VOXEL && field && !wantVoxel && voxelMap)
            VoxelSwoop_Begin(&sSwoop, false, true);
        else if (sFieldMode == FIELD_MODE_PIXEL && wantVoxel)
            VoxelSwoop_Begin(&sSwoop, true, true);
    }
    bool overworld = wantVoxel || (voxelMap && VoxelSwoop_ShowsVoxel(&sSwoop));
    {
        VoxelSwoopFrame swoop;

        VoxelSwoop_Frame(&sSwoop, &swoop);
        CtrVoxel_SetSwoop(swoop.camera, swoop.height, swoop.cards, swoop.shadows);
        sSwoopFieldObjs = !swoop.cards;
    }
    /* The Fly cinematic's camera, before the update too. */
    {
        VoxelFlyFrame fly;
        CtrVoxelFly share;

        VoxelFly_Frame(&sFly, &fly);
        share.active = VoxelFly_Active(&sFly);
        share.oldMap = fly.oldMap;
        share.noFade = fly.noFade;
        share.lift = fly.lift;
        share.rise = fly.rise;
        share.wisps = fly.wisps;
        share.travel = fly.travel;
        share.dirX = fly.dirX;
        share.dirZ = fly.dirZ;
        CtrVoxel_SetFly(&share);
    }
    /* The slider before the update: the world's eyes are set, and its
     * frustum widened to what they see, before it picks this frame's chunks.
     * Per eye the frame clears and splits twice: the chunk uploads leave the
     * GX queue the room for it. */
    bool voxelStereoWanted = overworld && !sTransition && sTopRight && slider > 0.02f && VoxelStereoWanted();
    CtrVoxel_SetStereo(voxelStereoWanted ? VoxelStereoSlider(slider) : 0.0f);
    if (voxelStereoWanted) sRenderReserve = RENDER_RESERVE_MOST;
    bool voxel = overworld && CtrVoxel_Update();
    /* A new map still being made - a frame or two, behind the fade - is
     * black rather than the 2D picture flashing up before the 3D one. */
    /* ... unless the 2D field was up: it stays until the voxel world is
     * drawn and the swoop crossfades into it (no black in between). */
    bool blank = overworld && !voxel && CtrVoxel_IsWarmingUp() && sFieldMode != FIELD_MODE_PIXEL;
    if (!down)
    {
        unsigned mode = !field ? FIELD_MODE_NONE : voxel ? FIELD_MODE_VOXEL
                      : blank ? sFieldMode : FIELD_MODE_PIXEL;

        if (mode == FIELD_MODE_NONE)
            VoxelSwoop_Cancel(&sSwoop);
        else if (sSwoop.phase == VOXEL_SWOOP_FLASH_IN && sSwoop.frame == 0 && mode != FIELD_MODE_VOXEL)
            VoxelSwoop_Cancel(&sSwoop); /* not drawn after all: next frame */
        else if (VoxelSwoop_ShowsVoxel(&sSwoop) && !voxel)
            VoxelSwoop_LoseVoxel(&sSwoop);
        else if (!VoxelSwoop_Active(&sSwoop) && sFieldMode == FIELD_MODE_VOXEL && mode == FIELD_MODE_PIXEL)
            VoxelSwoop_Begin(&sSwoop, false, false); /* the map's voxel world is gone: the flash alone */
        sFieldMode = mode;
    }
    /*
     * A battle in front of the voxel world: the world updated as the battle's
     * scenery whenever the option is on, and drawn as it while BG3 shows the
     * scenery - a move's background on BG3 is the 2D battle's, whole.
     */
    sBattleWorld = false;
    sWorldLayers = 0;
    bool battleUpdated = false;
    if (sBattle && CtrSettings_Voxel() && CtrSettings_VoxelBattle() && CtrVoxel_IsAvailableForBattle())
    {
        bool sliding = CtrBattleIntro_Sliding() != 0, moveBg = CtrBattleBg_MoveBgShown() != 0;

        if (!CtrVoxel_InBattle())
            CtrVoxel_BeginBattle();
        /* The intro slides the scenery in line by line: no shake there. */
        CtrVoxel_SetBattleFrame(sliding, sliding || moveBg ? 0.0f : BattleScenerySway(0x1c),
                                sliding || moveBg ? 0.0f : BattleScenerySway(0x1e));
        battleUpdated = CtrVoxel_Update();
        sBattleWorld = battleUpdated && !moveBg && (Reg(0) & 0x800) && !(Reg(0) & 128);
        if (sBattleWorld)
            sWorldLayers = (1u << 3) | (sliding && !VoxelBattle_IsLink() ? (1u << 1) | (1u << 2) : 0u);
    }
    /* The 2D field centres its text windows as the voxel overlay does. */
    sFieldUi = field && !voxel;
    sFieldVoxel = voxel;
    /* The voxel world's depth is its own (RenderVoxelTo), from any slider
     * position up; the layer parallax of the 2D path means nothing for a 3D
     * scene. The 3D battle and the transitions stay flat. */
    bool voxelStereo = voxelStereoWanted && voxel;
    /* A battle with its scene surface takes any slider position: its depth
     * is not whole pixels (RenderBattleStereo). */
    bool battleStereo = sBattle && sScene && !voxel && !blank && !sBattleWorld && !sTransition
                        && sTopRight && slider > 0.02f;
    /* The same for the battle over the voxel world, once it has the shared
     * stereo surface (asked for here, made before the next frame). */
    bool worldStereo = sBattleWorld && sScene && !down && !sTransition && sTopRight && slider > 0.02f;
    /* The world's picture goes into the scene surface (RenderBattleWorld):
     * no surface of its own to wait for. */
    bool worldWanted = worldStereo;
    bool stereo = voxelStereo || battleStereo || worldStereo
                  || (!voxel && !blank && !sBattleWorld && !sTransition && sTopRight && slider > 0.0f
                      && roundf(slider * CTR_STEREO_PIXELS) > 0.0f);
    /* A 2D screen composed per eye walks every layer twice, which on an Old
     * 3DS is 30 fps in a menu. Without its planes it stays flat until they
     * can be made (before the next frame, see sBandsWanted). */
    /* The battle scene is two passes of its own (RenderBattleScene), and cheap
     * enough to be composed per eye. A stage is not: the intro and the title
     * draw their waves line by line, and twice that is more than an Old 3DS
     * has in a frame, so they take the planes like any other 2D screen. */
    if (!sStage) sStageWithoutPlanes = false;
    bool planes = stereo && !overworld && !sBattle && !sStageWithoutPlanes && BandsUsable();
    /* ... and the 2D field drawn while the voxel world is still being made
     * (the option just on): flat as well, not per priority (RenderEye's
     * layer parallax splits the field's scrolled-together backgrounds that
     * its planes keep on one plane, and the swoop's crossfade is flat too). */
    bool planesMissing = stereo && !voxel && !blank && !sStage && !sBattle && !planes;
    if (planesMissing) stereo = false;
    if (down) stereo = planes = battleStereo = false;
    /* A magnified stage is one flat picture (RenderEye). */
    if (sStageZoomed) stereo = planes = false;
    /* With the Pokédex on the bottom screen the top shows the device, which
     * takes any slider position (RenderDexTop). */
    bool dexTop = down && CtrDexTop_Active();
    if (dexTop) stereo = sTopRight && slider > 0.02f;
    else CtrDexTop_Idle();
    /* The planes' parallax ramp (sPlaneRamp): in while they are used, out
     * once the voxel world is wanted (its VRAM waits for it, VoxelVramManage). */
    if (!down)
    {
        bool planesLeaving = field && CtrSettings_Voxel();
        bool swooping = VoxelSwoop_Active(&sSwoop);
        float step = 1.0f / (float)CTR_PLANE_RAMP_FRAMES;

        if (planes && !planesLeaving) sPlaneRamp = sPlaneRamp + step > 1.0f ? 1.0f : sPlaneRamp + step;
        else if (planes) sPlaneRamp = sPlaneRamp - step < 0.0f ? 0.0f : sPlaneRamp - step;
        /* Not used and no swoop to keep them under: nothing to ease. The
         * swoop's crossfade takes the ramp where it stood (Field2DOver). */
        else if (planesLeaving && !swooping) sPlaneRamp = 0.0f;
        /* Leaving the voxel world with the slider up: the planes cannot be
         * made until the world's VRAM is parked, after the swoop, so the
         * crossfade's 2D field takes its layers to depth itself (Field2DOver
         * through sParallax), and the planes arrive at that depth instead
         * of sliding in afterwards as a second motion. */
        else if (!planes && !planesLeaving && field && swooping && sTopRight && slider > 0.0f)
            sPlaneRamp = sPlaneRamp + step > 1.0f ? 1.0f : sPlaneRamp + step;
    }
    if (stereo != sStereo) { gfxSet3D(stereo); sStereo = stereo; }
    /* The voxel world reports the slider in tenths, the 2D path its pixels. */
    sStats.stereo = !stereo ? 0 : voxelStereo ? (uint32_t)ceilf(slider * 10.0f)
                  : battleStereo || worldStereo ? ceilf(slider * BATTLE_BACK_DEPTH - 0.001f)
                  : roundf(slider * CTR_STEREO_PIXELS);
    /* What this frame is and how it is drawn, for the frame profiler: the
     * same choice as the chain below. */
    sStats.route = down ? CTR_ROUTE_BOTTOM
                 : sTransition && !blank ? CTR_ROUTE_TRANSITION
                 : voxel ? (voxelStereo && stereo ? CTR_ROUTE_VOXEL_STEREO : CTR_ROUTE_VOXEL)
                 : blank ? CTR_ROUTE_BLANK
                 : sBattleWorld ? (worldStereo ? CTR_ROUTE_BATTLE_WORLD_STEREO : CTR_ROUTE_BATTLE_WORLD)
                 : !stereo ? CTR_ROUTE_MONO
                 : battleStereo ? CTR_ROUTE_BATTLE_STEREO
                 : planes ? CTR_ROUTE_PLANES : CTR_ROUTE_EYES;
    bool voxelBattle = sBattle && battleUpdated;
    sStats.mode = down ? CTR_MODE_BOTTOM
                : sBattle ? (voxelBattle ? CTR_MODE_BATTLE_VOXEL : CTR_MODE_BATTLE)
                : overworld ? CTR_MODE_FIELD_VOXEL
                : field ? CTR_MODE_FIELD
                : sStage ? CTR_MODE_STAGE
                : sCentred ? CTR_MODE_CENTRED : CTR_MODE_OTHER;
    sStats.sliderPct = (uint8_t)roundf(slider * 100.0f);
    if (slider > 0.02f && sStats.stereo == 0)
        sFrameFlat |= voxel ? CTR_FLAT_NO_VOXEL
                    : sBattleWorld && worldWanted ? CTR_FLAT_NO_SURFACE
                    : planesMissing ? CTR_FLAT_NO_PLANES : CTR_FLAT_OTHER;
    /* The battle's info boxes and text box (DrawBattleTextHd, HudDrawCrisp). */
    HudFrame();
    BoxFrame();
    sHudCrisp = sHud && sScene && !down && !blank && !sTransition && !voxel && HudCrispPossible();
    /* Why the boxes are drawn into the scene (magnified with it) when they
     * are: each change, the first few hundred of a session. */
    {
        static int lastCrisp = -1;
        static unsigned told;

        if (sHud && (int)sHudCrisp != lastCrisp && told < 200)
        {
            ++told;
            CtrLog_Write(CTR_LOG_VIDEO, "HUD boxes %s f=%lu covered=%d stereo=%d world=%d dispcnt=%04x winin=%04x winout=%04x bgcnt=%04x/%04x/%04x/%04x",
                         sHudCrisp ? "at screen pixels" : "in the scene", (unsigned long)sStats.frames,
                         sHudCovered, stereo, sBattleWorld, Reg(0), Reg(0x48), Reg(0x4A),
                         Reg(8), Reg(0xa), Reg(0xc), Reg(0xe));
            lastCrisp = sHudCrisp;
        }
    }
    PORT_PROF_BEGIN(layers);
    /* The swoop's crossfade: the 2D field is composed over the voxel world. */
    float fade2d = 0.0f, blur2d = 0.0f;
    if (voxel && !down)
    {
        VoxelSwoopFrame swoop;

        VoxelSwoop_Frame(&sSwoop, &swoop);
        fade2d = swoop.fade2d;
        blur2d = swoop.blur;
    }
    if (((!voxel && !blank) || fade2d > 0.0f) && down == bottom) LayersRender();
    /* Before anything is composed: it is drawn from within (ComposeBand). */
    sObjWin.ready = false;
    if (!voxel && !blank && !down) ObjWindowCompose();
    PORT_PROF_END(layers, PORT_PROF_LAYERS);
    PORT_PROF_BEGIN(draw);

    if (down)
    {
        /* The top screen is not drawn: it keeps the frame it showed last -
         * unless the Pokédex is open down there. */
        sPlanes = 0;
        if (bottom) RenderEye(sBottom, clear, 0.0f);
        if (dexTop) RenderDexTop(stereo, slider);
        else if (sHeldTop.kept && sHoldTop) HeldTopPutBack();
    }
    else if (sTransition && !blank)
    {
        sPlanes = 0;
        RenderTransition(voxel, clear);
    }
    else if (voxel)
    {
        sPlanes = 0;
        RenderVoxel(clear, voxelStereo && stereo ? slider : 0.0f);
        if (fade2d > 0.004f)
        {
            /* The 2D field's own stereo pixels, as its planes would shift
             * them with the slider where it is. */
            float field2d = sTopRight && slider > 0.0f ? roundf(slider * CTR_STEREO_PIXELS) * sPlaneRamp : 0.0f;

            Field2DOver(fade2d, blur2d, clear, sTop, field2d);
            if (voxelStereo && stereo) Field2DOver(fade2d, blur2d, clear, sTopRight, -field2d);
        }
    }
    else if (blank)
    {
        sPlanes = 0;
        C2D_TargetClear(sTop, C2D_Color32(0, 0, 0, 255));
    }
    else if (sBattleWorld)
    {
        sPlanes = 0;
        RenderBattleWorld(clear, worldStereo ? slider : 0.0f);
    }
    else if (!stereo)
    {
        sPlanes = 0;
        RenderEye(sTop, clear, 0.0f);
    }
    else if (battleStereo)
    {
        sPlanes = 0;
        RenderBattleStereo(clear, slider, NULL);
    }
    else if (planes)
    {
        sPlanes = DepthPlanes();
        RenderBands(sPlanes, clear);
        BlitBands(sTop, sPlanes, sStats.stereo);
        BlitBands(sTopRight, sPlanes, -(float)sStats.stereo);
    }
    else
    {
        /* The battle, or a stage without memory for its planes. */
        sPlanes = 0;
        RenderEye(sTop, clear, sStats.stereo);
        RenderEye(sTopRight, clear, -(float)sStats.stereo);
    }
    if (sStats.frames < sNoHomeUntil && !down)
    {
        DrawNoHome(sTop);
        if (stereo) DrawNoHome(sTopRight);
    }
    if (field && !down && !blank && CtrGame_FieldPoison() && PoisonVeil() != 0)
    {
        DrawPoisonVeil(sTop, PoisonVeil());
        if (stereo) DrawPoisonVeil(sTopRight, PoisonVeil());
    }
    if (!down && VoxelFly_Active(&sFly))
    {
        VoxelFlyFrame fly;
        static uint64_t lastGameFrame;
        uint64_t gameFrame = CtrPlatform_FrameCount();
        unsigned steps = lastGameFrame != 0 && gameFrame > lastGameFrame ? (unsigned)(gameFrame - lastGameFrame) : 1u;

        VoxelFly_Frame(&sFly, &fly);
        /* The wind over the world while the camera is up, under the cover;
         * then the cloud cover over whatever the frame is - the world, the
         * warp's black - its sheets in front of the screen in 3D. */
        if (voxel && fly.lift > 0.005f && fly.whiteout < 0.995f)
        {
            float rush = fminf(fmaxf(fly.lift, 0.0f), 1.0f) * (1.0f - fly.whiteout);

            DrawWind(sTop, rush, stereo ? 0 : -1);
            if (stereo) DrawWind(sTopRight, rush, 1);
        }
        if (fly.whiteout > 0.005f)
        {

            DrawCloudCover(sTop, fly.whiteout, stereo ? 0 : -1);
            if (stereo) DrawCloudCover(sTopRight, fly.whiteout, 1);
        }
        /* The sky scene over the closed cover. */
        {
            unsigned skyFrame;

            if (VoxelFly_Sky(&sFly, &skyFrame) && sSky.loaded)
            {
                DrawSkyScene(sTop, stereo ? 0 : -1, skyFrame);
                if (stereo) DrawSkyScene(sTopRight, 1, skyFrame);
            }
        }

        /* Paced by the game's frames, as the swoop is. Over the old map the
         * world must be there to fly over: gone early (the field left for
         * something else), the cinematic is dropped and Fly plays on as the
         * GBA's. Inside the cloud a frame with no field is the warp. */
        if (steps > VOXEL_SWOOP_STEPS_MAX) steps = VOXEL_SWOOP_STEPS_MAX;
        lastGameFrame = gameFrame;
        if (VoxelFly_Hovers(&sFly) && !voxel)
        {
            CtrLog_Write(CTR_LOG_VIDEO, "FLY cinematic dropped: no voxel field in phase %u", (unsigned)sFly.phase);
            VoxelFly_Cancel(&sFly);
        }
        else
        {
            VoxelFlyPhase was = sFly.phase;

            bool gone = !field || CtrMap_Version() != sFlyMapVersion;

            while (steps-- != 0 && VoxelFly_Active(&sFly))
                VoxelFly_Step(&sFly, voxel, gone);
            if (sFly.phase != was)
                CtrLog_Write(CTR_LOG_VIDEO, "FLY cinematic phase %u -> %u (frame %lu)", (unsigned)was,
                             (unsigned)sFly.phase, (unsigned long)sStats.frames);
        }
    }
    /* The scene's sprites, once the flight is over (or dropped). */
    if (sSky.loaded && !VoxelFly_Active(&sFly))
        SkySceneFree();
    if (!down && VoxelSwoop_Active(&sSwoop))
    {
        VoxelSwoopFrame swoop;

        VoxelSwoop_Frame(&sSwoop, &swoop);
        if (swoop.flash > 0.005f || swoop.bloom > 0.005f)
        {
            DrawSwoopLight(sTop, &swoop);
            if (stereo) DrawSwoopLight(sTopRight, &swoop);
        }
        /* Into the voxel world: the 2D field stays whole over it until every
         * chunk the oblique view needs is built (a black hole in the fade
         * otherwise), for up to VOXEL_SWOOP_HOLD_FRAMES. */
        {
            static unsigned held;
            static uint64_t lastGameFrame;
            /* Paced by the game's frames, not the pictures shown: on an Old
             * 3DS the swoop shows at 40 fps while the game runs 60, and
             * stepped once a picture it took half as long again. */
            uint64_t gameFrame = CtrPlatform_FrameCount();
            unsigned steps = lastGameFrame != 0 && gameFrame > lastGameFrame ? (unsigned)(gameFrame - lastGameFrame) : 1u;

            if (steps > VOXEL_SWOOP_STEPS_MAX) steps = VOXEL_SWOOP_STEPS_MAX;
            lastGameFrame = gameFrame;
            if (sSwoop.phase == VOXEL_SWOOP_FLASH_IN && voxel && CtrVoxel_GetStats()->chunksMissing != 0
                && held < VOXEL_SWOOP_HOLD_FRAMES)
                ++held;
            else
            {
                held = 0;
                while (steps-- != 0 && VoxelSwoop_Active(&sSwoop))
                    VoxelSwoop_Step(&sSwoop);
            }
        }
    }
#if CTR_SHOW_FPS
    if (CtrSettings_ShowFps())
    {
        if (!down || dexTop) DrawFps(sTop);
        if (stereo) DrawFps(sTopRight);
    }
#endif
    {
        int speedHalves = CtrPlatform_BattleSpeedBadge();

        if (speedHalves != 0)
        {
            if (!bottom || dexTop) DrawSpeedBadge(sTop, speedHalves);
            if (stereo) DrawSpeedBadge(sTopRight, speedHalves);
        }
    }
    /* Queued in the frame, behind the drawing into sBottom (BottomTransfer). */
    if (bottom) BottomTransfer();
    PORT_PROF_END(draw, PORT_PROF_DRAW);
    PORT_PROF_BEGIN(frameEnd);
    float gpuStartMs;
    {
        /* The GPU started early (GpuStartEarly): how many frames, and in how
         * many of them it was still busy when FrameEnd came. */
        static float sEarlyWaitSum;
        static unsigned sEarlyFrames;
        bool early = sGpuEarly, running = GpuFinishEarly();

        gpuStartMs = early ? (sGpuEarlyStart - start) * 1000.0f / SYSCLOCK_ARM11 : -1.0f;

        if (early) { sEarlyWaitSum += running; ++sEarlyFrames; }
        if (sStats.frames % 600 == 599 && (sEarlyFrames || sStereoSplit[0] || sStereoSplit[1]))
        {
            CtrLog_Write(CTR_LOG_VIDEO, "GPU early %s (fps %.1f): in %u frames, still busy at FrameEnd in %.0f%%, "
                         "%.1f cache flushes of %.3f ms a frame; "
                         "3D battle frames: %u scenery and sprites apart, %u whole "
                         "(blend %u mosaic %u bg-front %u slots %u), scenery kept in %u, edges in %u",
                         GpuEarlyThisFrame() ? "on" : "off", sStats.fps,
                         sEarlyFrames, sEarlyFrames ? 100.0f * sEarlyWaitSum / sEarlyFrames : 0.0f,
                         sEarlyFrames ? sFlushes / (float)sEarlyFrames : 0.0f,
                         sFlushes ? sFlushTicks * 1000.0f / SYSCLOCK_ARM11 / sFlushes : 0.0f,
                         sStereoSplit[0], sStereoSplit[1], sStereoWhy[0], sStereoWhy[1], sStereoWhy[2],
                         sStereoWhy[3], sSceneReused, sEdgesKept);
            if (sSceneEmptyReused)
                CtrLog_Write(CTR_LOG_VIDEO, "3D battle frames: empty scenery not composed in %u", sSceneEmptyReused);
            if (sStereoWhy[0])
                CtrLog_Write(CTR_LOG_VIDEO, "3D battle blend kinds: st-obj %u bld-obj %u objwin-nogroup %u "
                             "objwin-cut %u bg-blend %u", sBlendWhy[0], sBlendWhy[1], sBlendWhy[2], sBlendWhy[3],
                             sBlendWhy[4]);
            memset(sBlendWhy, 0, sizeof(sBlendWhy));
            memset(sStereoWhy, 0, sizeof(sStereoWhy));
            sSceneReused = 0;
            sEdgesKept = 0;
            sSceneEmptyReused = 0;
            sFlushTicks = 0;
            sFlushes = 0;
            sEarlyWaitSum = 0;
            sEarlyFrames = 0;
            sStereoSplit[0] = sStereoSplit[1] = 0;
        }
        {
            /* The command buffer (C3D_Init): its fullest frame, for the
             * memory budget. */
            float used = C3D_GetCmdBufUsage();

            if (used > sCmdBufPeak) sCmdBufPeak = used;
        }
        C3D_FrameEnd(running ? GX_CMDLIST_FLUSH : 0);
        sGpuEarly = false;
    }
    PORT_PROF_END(frameEnd, PORT_PROF_FRAMEEND);
    sStats.flat = sFrameFlat;
    ++sStats.frames;
    ++sFpsFrames;
    sStats.cpuMs = (svcGetSystemTick() - start) * 1000.0 / SYSCLOCK_ARM11;
    sStats.gpuStartMs = gpuStartMs >= 0.0f ? gpuStartMs : sStats.cpuMs;
    sStats.voxelFrame = voxel || battleUpdated;
    /* The GPU draws the frame now: the voxel world builds what comes next in
     * the time the CPU would otherwise wait for the VBlank. */
    if (overworld || battleUpdated)
        CtrVoxel_AfterSubmit(start);
    else
        CtrVoxel_Idle();
    uint64_t now = CtrPlatform_Milliseconds();
    if (now - sFpsStart >= 1000)
    {
        uint64_t gameFrames = CtrPlatform_FrameCount();

        sStats.fps = sFpsFrames * 1000.0f / (now - sFpsStart);
        sGameFps = (gameFrames - sGameFpsMark) * 1000.0f / (now - sFpsStart);
        sGameFpsMark = gameFrames;
        sFpsStart = now;
        sFpsFrames = 0;
    }
    if (sStats.frames % 600 == 0)
    {
        CtrLog_Write(CTR_LOG_VIDEO, "frames=%lu fps=%.1f cpu=%.2fms bg=%.2fms obj=%.2fms gpu=%.2fms "
                     "3d=%lupx%s x%lu quads=%lu cells=%lu sprites=%lu errors=%lu cache=%u linear=%lu vram=%lu cmdbuf=%.0f%%",
                     (unsigned long)sStats.frames, sStats.fps, sStats.cpuMs,
                     sBgTicks * 1000.0 / SYSCLOCK_ARM11, sObjTicks * 1000.0 / SYSCLOCK_ARM11,
                     sStats.gpuMs, (unsigned long)sStats.stereo,
                     sStats.stereo ? (sPlanes ? "/planes" : "/eyes") : "", (unsigned long)sPlanes,
                     (unsigned long)sStats.tiles, (unsigned long)sStats.cells,
                     (unsigned long)sStats.sprites, (unsigned long)sStats.errors, sUsed,
                     (unsigned long)linearSpaceFree(), (unsigned long)vramSpaceFree(), sCmdBufPeak * 100.0f);
        sCmdBufPeak = 0.0f;
        if (voxel || sBattleWorld)
        {
            const CtrVoxelStats *stats = CtrVoxel_GetStats();
            CtrLog_Write(CTR_LOG_VIDEO,
                         "VOXEL chunks=%u/%u missing=%u pending=%u builds=%u atlas=%u verts=%u "
                         "mesh=%.2fms peak=%.2fms update=%.2fms peak=%.2fms dropped=%u "
                         "anim=%u/%u passes=%u/%uk/%ub/%uf/%uw refl=%u drafts=%u/%u",
                         stats->visibleChunks, stats->chunks, stats->chunksMissing,
                         stats->pendingBuilds, stats->meshRebuilds, stats->atlasRebuilds,
                         stats->vertices, stats->meshMs, stats->meshPeakMs,
                         stats->updateMs, stats->updatePeakMs, stats->dropped,
                         stats->animationUploads, stats->animatedMetatiles,
                         stats->animationPasses, stats->animationKept, stats->animationBegins,
                         stats->animationPeakFrames, stats->animationWaits,
                         stats->reflections, stats->draftsVisible, stats->draftsMade);
        }
    }
}

const CtrVideoStats *CtrVideo_GetStats(void) { return &sStats; }

void CtrVideo_SetStage(bool stage) { sStageRequested = stage; }
void CtrVideo_SetStageFaded(bool faded) { sStageFadedRequested = faded; }
void CtrVideo_SetStageZoomed(bool zoomed) { sStageZoomedRequested = zoomed; }
bool CtrVideo_StageZoomed(void) { return sStageZoomedRequested; }
void CtrVideo_SetCentred(unsigned screen) { sCentredRequested = screen < CTR_CENTRED_SCREENS ? screen : CTR_CENTRED_NONE; }
void CtrVideo_SetBattle(bool battle) { sBattleRequested = battle; }
void CtrVideo_SetTransition(bool transition) { sTransitionRequested = transition; }

void CtrVideo_SetLineRegisters(const uint16_t *regs, unsigned lines)
{
    /* The bridge's table, filled for this frame just before it is presented. */
    sLineRegs = regs && lines >= CTR_GBA_LINES ? (const uint16_t (*)[CTR_LINE_REGS])regs : NULL;
}

void CtrVideo_SetLineScroll(unsigned reg, bool wide, const void *values, unsigned lines)
{
    const uint16_t *source = values;
    unsigned regs = wide ? 2 : 1;

    sLineMask = 0;
    sLineCount = 0;
    if (!values || reg + regs * 2 > 16) return;
    if (lines > LINE_MAX) lines = LINE_MAX;
    /*
     * The first line gets the register the VBlank handler wrote from entry 0.
     * The DMA then fires at the end of every line, so line y shows entry y-1.
     */
    for (unsigned r = 0; r < regs; ++r)
        for (unsigned y = 0; y < lines; ++y)
            sLineScroll[reg / 2 + r][y] = source[(y ? y - 1 : 0) * regs + r];
    sLineMask = ((1u << regs) - 1) << (reg / 2);
    sLineCount = lines;
}

void CtrVideo_Shutdown(void)
{
    BottomRelease();
    CtrVoxel_Shutdown();
    StereoSurfaceRelease();
    LayersRelease();
    SceneRelease();
    /* Target deletion waits for queued GPU work before buffers are freed. */
    if (sLogical) C3D_RenderTargetDelete(sLogical);
    if (sTop) C3D_RenderTargetDelete(sTop);
    if (sTopRight) C3D_RenderTargetDelete(sTopRight);
    BandsRelease();
    sBandsFailed = false;
    if (sC2d) C2D_Fini();
    if (sBloom) C3D_RenderTargetDelete(sBloom);
    sBloom = NULL;
    if (sBloomTex.data) C3D_TexDelete(&sBloomTex);
    memset(&sBloomTex, 0, sizeof(sBloomTex));
    if (sSurface.data) C3D_TexDelete(&sSurface);
    if (sBattleShadowTex.data) C3D_TexDelete(&sBattleShadowTex);
    memset(&sBattleShadowTex, 0, sizeof(sBattleShadowTex));
    LeavesRelease();
    CornerArtRelease();
    if (sAtlas.data) C3D_TexDelete(&sAtlas);
    if (sC3d) C3D_Fini();
    sLogical = sTop = sTopRight = NULL;
    sC2d = sC3d = false;
    memset(&sSurface, 0, sizeof(sSurface));
    memset(&sAtlas, 0, sizeof(sAtlas));
}

bool CtrVideo_InBattle(void) { return sInBattle; }
