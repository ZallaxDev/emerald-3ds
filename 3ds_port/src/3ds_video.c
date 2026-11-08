/* GBA compatibility compositor on PICA200. CPU work only decodes dirty tiles
 * into a texture atlas and submits geometry. All rasterization, transforms,
 * transparency, priority composition and scaling happen on the GPU.
 *
 * It is one translation unit: the .inc files of src/video are parts of it, in
 * the order they are included below, and each sees every static before it.
 * scripts/video_cut.py keeps the table of the cut. */
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
#include "3ds_field_terrain.h"
#include "3ds_dex_top.h"
#include "3ds_pass_top.h"
#include "3ds_corner_art.h"
#include "3ds_data.h"
#include "3ds_bottom.h"
#include "ctr_battle_layout.h"
#include "3ds_battle_hud.h"
#include "3ds_local.h"
#include "3ds_obj_list.h"
#include "../compat/port_prof.h"
#include "../compat/ctr_gba_battle.h"

/* A part of a long function, or a small helper its parts share, apart to be
 * read: always compiled into each caller, whatever the inliner's limits. */
#define VIDEO_PIECE static inline __attribute__((always_inline))

#include "video/gpu.inc"

/* The voxel overworld replaces this compositor's output while the player is
 * walking around; every other game state keeps the 2D path below. */
#include "voxel/ctr_voxel.h"
#include "voxel/voxel_battle.h"
#include "voxel/voxel_swoop.h"

#include "video/view.inc"

#include "video/vram.inc"

#include "video/banks.inc"

#include "video/frame.inc"

#include "video/tiles.inc"

#include "video/tiledraw.inc"

#include "video/stage.inc"

#include "video/corner.inc"

#include "video/edges.inc"

#include "video/hud.inc"

#include "video/boxtext.inc"

#include "video/battlebg.inc"

#include "video/centred.inc"

#include "video/layertex.inc"

#include "video/layerwalk.inc"

#include "video/leaves.inc"

#include "video/bg.inc"

#include "video/obj.inc"

#include "video/compose.inc"

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
    ChainQueueDone();
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

#include "video/scene.inc"

#include "video/toggles.inc"

#include "video/bscene.inc"

#include "video/bottom.inc"

#include "video/slots.inc"

#include "video/mono.inc"

#include "video/gather.inc"

#include "video/slotpack.inc"

#include "video/btext.inc"

#include "video/stereo.inc"

#include "video/voxel.inc"

#include "video/bworld.inc"

#include "video/transition.inc"

#include "video/bands.inc"

#include "video/overlays.inc"

#include "video/report.inc"

#include "video/voxvram.inc"

#include "video/framelog.inc"

#include "video/present.inc"

void CtrVideo_Present(void)
{
    PresentFrame f;

    /* The frame's entry tick: its preparation, before FrameBegin. */
    f.entry = svcGetSystemTick();
    FrameEntry(&f);
    if (PresentHeld(&f)) return;
    FrameHousekeeping();
    FrameView();
    FrameTrust();
    FrameObjList();
    FramePrepare();
    /* Wait for previous GPU work before editing the atlas. C3D owns VBlank
     * pacing and swap: no gfxSwapBuffers/gspWaitForVBlank in this path. */
    f.waitStart = svcGetSystemTick();
    CtrVoxel_NoteFrameWait(f.waitStart);
    if (!C3D_FrameBegin(C3D_FRAME_SYNCDRAW))
    {
        sReport.waitMs = (svcGetSystemTick() - f.waitStart) * 1000.0f / SYSCLOCK_ARM11;
        return;
    }
    FrameBegun(&f);
    FrameDump();
    FrameDrops();
    FrameCounters();
    PORT_PROF_BEGIN(palette);
    CtrDayNight_Update();
    UpdatePalette();
    PORT_PROF_END(palette, PORT_PROF_PALETTE);
    f.clear = FrameBackdrop();
    FrameVoxel(&f);
    FrameBattleWorld(&f);
    FrameStereo(&f);
    FramePlaneRamp(&f);
    FrameRoute(&f);
    FrameHud(&f);
    PORT_PROF_BEGIN(layers);
    FrameLayers(&f);
    PORT_PROF_END(layers, PORT_PROF_LAYERS);
    PORT_PROF_BEGIN(draw);
    FrameDraw(&f);
    FrameOverlays(&f);
    PORT_PROF_END(draw, PORT_PROF_DRAW);
    PORT_PROF_BEGIN(frameEnd);
    float gpuStartMs = FrameSubmit(f.start);
    PORT_PROF_END(frameEnd, PORT_PROF_FRAMEEND);
    FrameFinish(&f, gpuStartMs);
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

void CtrVideo_SetLineAlpha(const uint16_t *values, unsigned lines)
{
    bool wasOn = sLineAlphaOn, changed = false;

    sLineAlphaOn = false;
    if (values && lines != 0)
    {
        if (lines > CTR_GBA_LINES) lines = CTR_GBA_LINES;
        /* As the scroll above: line y shows entry y-1, the first line entry 0,
         * which the VBlank handler wrote into the register itself. */
        for (unsigned y = 0; y < CTR_GBA_LINES; ++y)
        {
            unsigned line = y < lines ? y : lines - 1;
            uint16_t value = values[line ? line - 1 : 0];

            changed |= sLineAlpha[y] != value;
            sLineAlpha[y] = value;
            if (sLineAlpha[y] != sLineAlpha[0]) sLineAlphaOn = true;
        }
    }
    /* What a kept blend layer is placed by (BattleScenePlace): the weights
     * while they are on, and whether they are. */
    if (sLineAlphaOn != wasOn || (sLineAlphaOn && changed)) ++sLineAlphaVersion;
}

void CtrVideo_Shutdown(void)
{
    BottomRelease();
    CtrVoxel_Shutdown();
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
