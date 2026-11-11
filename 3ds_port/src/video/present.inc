/*
 * Part of 3ds_video.c. The pieces of CtrVideo_Present, in the frame's order,
 * and PresentFrame, what they hand on; RenderDexTop.
 */

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

/* What a piece of CtrVideo_Present decides and a later one reads. */
typedef struct
{
    uint64_t entry, waitStart, start; /* ticks: Present, before and after FrameBegin */
    bool wanted, native; /* a bottom screen asked for, and drawn by its own code */
    bool bottom, down;   /* its surface ready, and the top drawn as with it there */
    uint32_t clear;
    uint32_t voxelClear; /* the voxel world's clear: the map's ground under a wide camera */
    float slider;
    bool field, overworld, voxel, blank, warmupBlack, battleUpdated;
    bool voxelStereoWanted, voxelStereo, battleStereo, worldStereo;
    bool stereo, planes, planesMissing, planesDenied, dexTop;
    float fade2d, blur2d;
} PresentFrame;

/* The frame's counts from zero, and what the bottom screen shows: the rest
 * of the frame depends on it. */
VIDEO_PIECE void FrameEntry(PresentFrame *f)
{
    sFrameFlat = 0;
    sDeniedNow = 0;
    sObjList = NULL;
    sStats.stereoSampleMs = sStats.stereoEyesMs = 0.0f;
    if (!sMemory.regs) CtrPlatform_Fatal("VIDEO has no logical memory bound");
    /* The PokéNav, the PC's boxes and the bag are drawn on the bottom screen, whether
     * or not the top is held. */
    f->wanted = BottomScreen(sCentredRequested) && !sStageRequested;
    f->native = BottomNative(sCentredRequested);
    f->bottom = BottomReady(f->wanted && !f->native);
    if (f->wanted && !f->native && !f->bottom) SurfaceDenied(CTR_DENIED_BOTTOM);
    /* The Pokédex drawn natively down there still has its device up here:
     * the top screen is drawn as with its picture down there (down), the
     * bottom one's picture is not. */
    f->down = f->bottom || (f->wanted && f->native && CtrDexTop_Active());
    bool dexUp = f->down && CtrDexTop_Active();
    sBottomInUse = f->bottom;
    if (dexUp)
    {
        if (sHoldTop && !sHeldTop.dexShown && !sHeldTop.kept) HeldTopKeep();
        sHeldTop.dexShown = true;
    }
    else if (sHeldTop.kept && !sHoldTop)
        HeldTopForget();
    else if (!sHeldTop.kept)
        sHeldTop.dexShown = false;
}

/* A top screen that stays as it is, held or waiting for the bottom one: the
 * frame ends here with nothing new drawn. True when it does. */
VIDEO_PIECE bool PresentHeld(const PresentFrame *f)
{
    /* Nor is the shop's BUY screen ever shown on the top screen while its
     * surface down there is not ready yet, nor a screen the bottom one
     * draws natively. */
    if ((sHoldTop || sCentredRequested == CTR_CENTRED_SHOP || (f->wanted && f->native)) && !f->down)
    {
        uint64_t heldWait = svcGetSystemTick();

        sStats.mode = CTR_MODE_OTHER;
        sStats.route = CTR_ROUTE_BLANK;
        sStats.warmupBlack = false;
        sStats.gapMs = sStats.arriveMs = sStats.prepMs = 0.0f;
        sReport.kind = CTR_PRESENT_HELD;
        sPerfBoundary = true;
        sShownCrossed = true;
        bool putBack = sHeldTop.kept && sHoldTop;
        if (sHeldFrames < LOOK_DOWN_AFTER) ++sHeldFrames;
        /* The Pokédex gone with the top still held: its frame back, once,
         * darkened if it was not yet; or darkened once the hold has lasted.
         * The frame stays in the top's buffer, so it is drawn over once. */
        if ((putBack || (!sTopDark && sHeldFrames >= LOOK_DOWN_AFTER)) && C3D_FrameBegin(C3D_FRAME_SYNCDRAW))
        {
            sReport.waitMs = (svcGetSystemTick() - heldWait) * 1000.0f / SYSCLOCK_ARM11;
            uint64_t putBackTick = svcGetSystemTick();

            ReportGpu();
            if (putBack) HeldTopPutBack();
            if (!sTopDark)
            {
                sSceneOn = NULL;   /* the buffer read back afresh, not from the colour cache */
                DrawLookDown(sTop);
                if (sStereo && sTopRight) DrawLookDown(sTopRight);
                sTopDark = true;
            }
            C3D_FrameEnd(0);
            ReportSubmit(0.0f, putBackTick);
        }
        else
        {
            gspWaitForVBlank();
            sReport.waitMs = (svcGetSystemTick() - heldWait) * 1000.0f / SYSCLOCK_ARM11;
        }
        ++sStats.frames;
        return true;
    }
    sHeldFrames = 0;
    return false;
}

/* Outside the frame, where deleting a render target may wait for the GPU. */
VIDEO_PIECE void FrameHousekeeping(void)
{
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
}

/*
 * The view of this frame: what is asked, a stage before a centred screen, that
 * before a battle, that before a transition, and the zoom it is drawn at.
 */
VIDEO_PIECE void FrameView(void)
{
    sStage = sStageRequested;
    sStageFaded = sStage && sStageFadedRequested;
    sStageZoomed = sStage && sStageZoomedRequested;
    sCentred = sCentredRequested != CTR_CENTRED_NONE && !sStage;
    sCentredScreen = sCentred ? sCentredRequested : CTR_CENTRED_NONE;
    sBattle = sBattleRequested && !sStage && !sCentred;
    sInBattle = sBattleRequested;
#ifdef CTR_BATTLE_TOGGLES
    BattleTogglesFrame();
#endif
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
}

/* The field in 2D or in voxel: the audit found every write announced
 * there. A battle's OBJ blocks: so did the console's log (SyncBlock). */
VIDEO_PIECE void FrameTrust(void)
{
    bool fieldTrust = !sStage && !sCentred && !sBattle && CtrGame_IsOverworld() && !sTransition
                      && !sFieldBanner && !CtrGame_FieldRawVram();
    bool battleTrust = sBattle && !sTransition;

#ifdef CTR_AUDIT_BITS
    /* nocache.txt bit 512: the battle's OBJ blocks compared, as before (A/B frames). */
    if (CtrDumpOff() & 512) battleTrust = false;
#endif
    VramTrustFrame(!CTR_VRAM_TRUST ? VRAM_TRUST_NONE : fieldTrust ? VRAM_TRUST_ALL
                   : battleTrust ? VRAM_TRUST_OBJ : VRAM_TRUST_NONE);
}

/* The OAM's draw list, from the OAM this frame shows (3ds_obj_list.h). */
VIDEO_PIECE void FrameObjList(void)
{
    const CtrObjMarkSets marks = {sFogOam, sLatticeOam, sParticleOam};
    bool off = false;

#ifdef CTR_AUDIT_BITS
    /* nocache.txt bit 4096: every consumer walks the 128 entries, as before. */
    off = (CtrDumpOff() & 4096) != 0;
#endif
    sObjList = CtrObjList_Latch(sMemory.oam, &marks, off);
}

/* The surfaces this frame needs, made or given back outside the frame. */
VIDEO_PIECE void FramePrepare(void)
{
    LayersPrepare();
    ScenePrepare();
    if ((sBattle || sTransition) && !sScene) SurfaceDenied(CTR_DENIED_SCENE);
    SlotsPrepare();
    SceneBlendPrepare();
    ObjWindowPrepare();
    sVramAsked = false;
}

/* FrameBegin past: the time it waited, and what must go before any drawing. */
VIDEO_PIECE void FrameBegun(PresentFrame *f)
{
    sReport.kind = CTR_PRESENT_DRAWN;
    sReport.boundary = sPerfBoundary;
    sPerfBoundary = false;
    ReportGpu();
    /* A late clear wiped the scene surface's kept scenery, if it was that. */
    if (ClearsRun())
    {
        sSceneCached = sSceneEmpty = sMonoKeeperHeld = false;
        ++sSlotSurface;
    }
    /* Before anything samples what the CPU wrote since the last frame. */
    TexCacheClear();
    sUploadCommands = sAnimCommands = 0;
    sRenderReserve = sBattle ? RENDER_RESERVE_MOST : RENDER_RESERVE;
    Flat2D();
    f->start = svcGetSystemTick();
    sStats.waitMs = (f->start - f->waitStart) * 1000.0 / SYSCLOCK_ARM11;
    sReport.waitMs = sStats.waitMs;
    /* FrameBegin waited for the last frame's drawing: this is its time. Read
     * after FrameEnd, as it was, it was the frame before that one's. */
    sStats.gpuMs = C3D_GetDrawingTime();
    /* For the frame profiler and FrameDrops: the interval between two
     * FrameBegins (a frame shown twice is over 24 ms), and how far into it
     * the CPU came back (past 16.0 ms the CPU missed the VBlank). */
    static uint64_t sPerfLastBegin;

    sStats.gapMs = sPerfLastBegin ? (f->start - sPerfLastBegin) * 1000.0f / SYSCLOCK_ARM11 : 0.0f;
    sStats.arriveMs = sPerfLastBegin ? (f->waitStart - sPerfLastBegin) * 1000.0f / SYSCLOCK_ARM11 : 0.0f;
    sStats.prepMs = (f->waitStart - f->entry) * 1000.0f / SYSCLOCK_ARM11;
    sPerfLastBegin = f->start;
}

/* A dump build's pictures of the last frame. Nothing in any other build. */
VIDEO_PIECE void FrameDump(void)
{
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
    /* Counted from the battle's first picture, and numbered so in frames.raw:
     * two runs whose boots hid a different number of frames (pacing) still
     * dump the same moments of the battle. */
    if (sBattle && sStats.frames - battleStart + 1 > CTR_DUMP_FROM
     && (sStats.frames - battleStart) % CTR_DUMP_STEP == 0 && dumped++ < 40)
#elif defined(CTR_DUMP_SWOOP)
    /* Every frame the swoop plays, and the 150 after the option changed
     * (the frames before the swoop starts are where a black one would be),
     * up to 1600 in all; each dumped frame logged with the route of the
     * picture it holds (the frame before this one). The option as the game
     * starts is no change. Numbered by the game's frame, not the picture: two
     * runs whose pacing hid different pictures still pair the same moments. */
    static unsigned dumped, sinceToggle = 1000;
    static bool lastVoxelOption, optionSeen;
    if (!optionSeen) { optionSeen = true; lastVoxelOption = CtrSettings_Voxel(); }
    if (CtrSettings_Voxel() != lastVoxelOption) { lastVoxelOption = CtrSettings_Voxel(); sinceToggle = 0; }
    else if (sinceToggle < 1000) ++sinceToggle;
    bool dumping = VoxelSwoop_Active(&sSwoop) || sinceToggle < 150;
    if (dumping && dumped < 1600)
        CtrLog_Write(CTR_LOG_VIDEO, "DUMP f=%lu game=%lu route=%u mode=%u field=%u swoop=%u/%u",
                     (unsigned long)sStats.frames, (unsigned long)CtrPlatform_FrameCount(), (unsigned)sStats.route,
                     (unsigned)sStats.mode, sFieldMode, (unsigned)sSwoop.phase, sSwoop.frame);
    if (dumping && dumped++ < 1600)
#elif defined(CTR_DUMP_GAME_FRAMES)
    /* By the game's frame, not the picture's count: two runs whose pacing
     * hid different pictures still dump, and pair, the same moments. Every
     * CTR_DUMP_GAME_FRAMES frames of the game, up to 60 pictures. */
    static unsigned dumped;
    static uint32_t lastGame;
    uint32_t game = (uint32_t)CtrPlatform_FrameCount();
    bool due = game != lastGame && game % CTR_DUMP_GAME_FRAMES == 0;
    lastGame = game;
    if (due && dumped++ < 60)
#else
    if (sStats.frames >= 150 && (sStats.frames - 150) % 150 == 0 && sStats.frames <= 6000)
#endif
    {
        static FILE *dump;

        if (!dump) dump = fopen("sdmc:/3ds/emerald3ds/frames.raw", "wb");
        if (dump)
        {
            uint32_t frame = sStats.frames;

#if defined(CTR_DUMP_BATTLE) && !defined(CTR_DUMP_OBJWIN)
            frame -= battleStart;
#elif defined(CTR_DUMP_SWOOP) || defined(CTR_DUMP_GAME_FRAMES)
            frame = (uint32_t)CtrPlatform_FrameCount();
#endif
            fwrite(&frame, 4, 1, dump);
            fwrite(sTop->frameBuf.colorBuf, 4, 240 * 400, dump);
            if (sStereo && sTopRight) fwrite(sTopRight->frameBuf.colorBuf, 4, 240 * 400, dump);
            else { static uint32_t zero[240 * 400]; fwrite(zero, 4, 240 * 400, dump); }
            fflush(dump);
        }
    }
#endif
}

/* Last frame's counts summed up, this frame's started at zero. */
VIDEO_PIECE void FrameCounters(void)
{
    if (sStats.uploads) { ++sDecodeFrames; sDecodeFramesTiles += sStats.uploads; }
    if (sStats.cells) { ++sCellFrames; sCellsTotal += sStats.cells; }
    sStats.tiles = sStats.uploads = sStats.sprites = sStats.cells = 0;
    sFastUsed = 0;
    sBgTicks = sObjTicks = 0;
    sSceneWhy = sTextWhy = 0;
    sSlotsAgainNow = sSlotsUnkeptNow = 0;
    sGpuStage = CTR_STAGE_NONE;
    if (sUsed > CACHE_COUNT - 4096) { memset(sHash, 0, sizeof(sHash)); sUsed = 0; ++sCacheGeneration; }
}

/* The frame's backdrop, the colour its pictures are cleared to. */
VIDEO_PIECE uint32_t FrameBackdrop(void)
{
    if (sStage || sBattle) RecordScroll();
    /* The shown backdrop, faded: sPalette may hold the unfaded one. */
    uint16_t backdrop = (Reg(0) & 128) ? 0x7fff : sMemory.palette[0] & 0x7fff;
    uint32_t rgb = CtrVideo_RGBA8(backdrop, true);
    uint32_t clear = C2D_Color32(rgb >> 24, rgb >> 16, rgb >> 8, 255);
    sBackdropShown = clear;
    return clear;
}

/*
 * The voxel frame prepared, ahead of the frame's route (FrameRoute). If the
 * atlas or the mesh could not be built this frame, the 2D compositor draws
 * it: leaving the previous map's geometry on screen would show the wrong place.
 */
VIDEO_PIECE void FrameVoxel(PresentFrame *f)
{
    f->field = !sStage && !sCentred && !sBattle && CtrGame_IsOverworld();
    /* Azahar passes its slider only with factor_3d set in its config; a
     * profiling build can fix it (-DCTR_FORCE_SLIDER=1.0f) instead, and a
     * measured scenario can fix it too (3ds_scenario.c). Read before the
     * voxel update: the world is drawn per eye from it (CtrVoxel_SetStereo). */
#ifdef CTR_FORCE_SLIDER
    float slider = CTR_FORCE_SLIDER;
#else
    float slider = CtrScenario_Slider(osGet3DSliderState());
#endif
    f->slider = slider;
    /* The overworld, drawn in 3D or - while its first atlas or mesh is still
     * being made - by the 2D compositor. Either way it never takes the depth
     * planes: holding them there is what kept the overworld's atlas out of
     * VRAM for good, the 2D picture standing in for it frame after frame. */
    /* Opt-in from the bottom screen's options (CtrSettings_Voxel). */
    bool voxelMap = f->field && CtrVoxel_IsAvailable();
    bool wantVoxel = voxelMap && CtrSettings_Voxel();
    /*
     * The swoop between the renderers (sSwoop), decided before the update so
     * the camera's first frame is its own. The option gone off: the voxel
     * world stays up for the camera's rise. The 2D field about to become the
     * voxel world: the flash starts now, undone below if the world is not
     * drawn after all (still warming up).
     */
    if (!f->down && !VoxelSwoop_Active(&sSwoop))
    {
        if (sFieldMode == FIELD_MODE_VOXEL && f->field && !wantVoxel && voxelMap)
            VoxelSwoop_Begin(&sSwoop, false, true);
        else if (sFieldMode == FIELD_MODE_PIXEL && wantVoxel)
            VoxelSwoop_Begin(&sSwoop, true, true);
    }
    f->overworld = wantVoxel || (voxelMap && VoxelSwoop_ShowsVoxel(&sSwoop));
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
    f->voxelStereoWanted = f->overworld && !sTransition && sTopRight && slider > 0.02f && VoxelStereoWanted();
    CtrVoxel_SetStereo(f->voxelStereoWanted ? VoxelStereoSlider(slider) : 0.0f);
    if (f->voxelStereoWanted) sRenderReserve = RENDER_RESERVE_MOST;
    f->voxel = f->overworld && CtrVoxel_Update();
    /* Under a wide camera (the swoop's, the Fly's) the world is cleared to
     * the map's own ground colour, not the backdrop's black: what lies past
     * the squares the frame could build or fill passes for ground. The voxel
     * picture's clear only: the bottom screen and the 2D field keep theirs. */
    f->voxelClear = f->clear;
    if (f->voxel)
    {
        uint32_t ground;

        if (CtrVoxel_WideBackdrop(&ground)) f->voxelClear = ground;
    }
    /* A new map still being made - a frame or two, behind the fade - is
     * black rather than the 2D picture flashing up before the 3D one. */
    /* ... unless the 2D field was up: it stays until the voxel world is
     * drawn and the swoop crossfades into it (no black in between). */
    /* Nor is the 2D field's terrain shown stale: the field camera leaves it
     * undrawn while the voxel world is drawn alone (3ds_field_terrain.h). The
     * world not drawn after all is black for this frame, and the terrain is
     * drawn again in the next one's VBlank (FieldCamera_SyncTerrain). Only
     * the warm-up's black holds the field's fade in (sStats.warmupBlack). */
    f->warmupBlack = f->overworld && !f->voxel && CtrVoxel_IsWarmingUp() && sFieldMode != FIELD_MODE_PIXEL;
    f->blank = f->warmupBlack || (f->field && !f->down && !f->voxel && FieldCamera_TerrainStale());
    if (!f->down)
    {
        unsigned mode = !f->field ? FIELD_MODE_NONE : f->voxel ? FIELD_MODE_VOXEL
                      : f->blank ? sFieldMode : FIELD_MODE_PIXEL;

        if (mode == FIELD_MODE_NONE)
            VoxelSwoop_Cancel(&sSwoop);
        else if (sSwoop.phase == VOXEL_SWOOP_FLASH_IN && sSwoop.frame == 0 && mode != FIELD_MODE_VOXEL)
            VoxelSwoop_Cancel(&sSwoop); /* not drawn after all: next frame */
        else if (VoxelSwoop_ShowsVoxel(&sSwoop) && !f->voxel)
            VoxelSwoop_LoseVoxel(&sSwoop);
        else if (!VoxelSwoop_Active(&sSwoop) && sFieldMode == FIELD_MODE_VOXEL && mode == FIELD_MODE_PIXEL)
            VoxelSwoop_Begin(&sSwoop, false, false); /* the map's voxel world is gone: the flash alone */
        sFieldMode = mode;
    }
    /* Whether this frame shows the voxel world alone, with nothing of the 2D
     * field's terrain in it and no swoop to bring it in: the field camera
     * leaves the terrain undrawn meanwhile (3ds_field_terrain.h). */
#ifdef CTR_AUDIT_BITS
    /* nocache.txt bit 64: the terrain drawn on every frame, as before (A/B frames). */
    if (CtrDumpOff() & 64)
        CtrFieldTerrain_NoteFrame(false);
    else
#endif
    CtrFieldTerrain_NoteFrame(!f->down && f->field && f->voxel && !VoxelSwoop_Active(&sSwoop));
}

/*
 * A battle in front of the voxel world: the world updated as the battle's
 * scenery whenever the option is on, and drawn as it while BG3 shows the
 * scenery - a move's background on BG3 is the 2D battle's, whole.
 */
VIDEO_PIECE void FrameBattleWorld(PresentFrame *f)
{
    sBattleWorld = false;
    sWorldLayers = 0;
    f->battleUpdated = false;
    /* No cold mesh of the voxel world is kept through any battle: its memory
     * is the battle's (CtrVoxel_ColdRelease). */
    if (sBattle)
        CtrVoxel_ColdRelease();
    if (sBattle && CtrSettings_Voxel() && CtrSettings_VoxelBattle() && CtrVoxel_IsAvailableForBattle())
    {
        bool sliding = CtrBattleIntro_Sliding() != 0, moveBg = CtrBattleBg_MoveBgShown() != 0;

        if (!CtrVoxel_InBattle())
            CtrVoxel_BeginBattle();
        /* The intro slides the scenery in line by line: no shake there. */
        CtrVoxel_SetBattleFrame(sliding, sliding || moveBg ? 0.0f : BattleScenerySway(0x1c),
                                sliding || moveBg ? 0.0f : BattleScenerySway(0x1e));
        f->battleUpdated = CtrVoxel_Update();
        sBattleWorld = f->battleUpdated && !moveBg && (Reg(0) & 0x800) && !(Reg(0) & 128);
        if (sBattleWorld)
            sWorldLayers = (1u << 3) | (sliding && !VoxelBattle_IsLink() ? (1u << 1) | (1u << 2) : 0u);
    }
    /* Updated, it may be drawn: its VRAM stays until the GPU is past it
     * (VoxelVramManage). */
    if (f->voxel || f->battleUpdated) sWorldDrawnFrame = sStats.frames;
}

/* Whether the frame is drawn per eye, and how. */
VIDEO_PIECE void FrameStereo(PresentFrame *f)
{
    /* The 2D field centres its text windows as the voxel overlay does. */
    sFieldUi = f->field && !f->voxel;
    sFieldVoxel = f->voxel;
    /* The voxel world's depth is its own (RenderVoxelTo), from any slider
     * position up; the layer parallax of the 2D path means nothing for a 3D
     * scene. The 3D battle and the transitions stay flat. */
    f->voxelStereo = f->voxelStereoWanted && f->voxel;
    /* A battle with its scene surface takes any slider position: its depth
     * is not whole pixels (RenderBattleStereo). */
    f->battleStereo = sBattle && sScene && !f->voxel && !f->blank && !sBattleWorld && !sTransition
                      && sTopRight && f->slider > 0.02f;
    /* The same for the battle over the voxel world: its picture goes into
     * the scene surface (RenderBattleWorld), no surface of its own to wait
     * for. */
    f->worldStereo = sBattleWorld && sScene && !f->down && !sTransition && sTopRight && f->slider > 0.02f;
    /*
     * The 3D slider decides the separation, and at zero the right eye is not
     * composed at all: with 3D off this is the same single pass as before.
     * Whole pixels only, so every layer stays on the pixel grid in both eyes.
     */
    f->stereo = f->voxelStereo || f->battleStereo || f->worldStereo
                || (!f->voxel && !f->blank && !sBattleWorld && !sTransition && sTopRight && f->slider > 0.0f
                    && roundf(f->slider * CTR_STEREO_PIXELS) > 0.0f);
    /* A 2D screen composed per eye walks every layer twice, which on an Old
     * 3DS is 30 fps in a menu. Without its planes it stays flat until they
     * can be made (before the next frame, see sBandsWanted). */
    /* The battle scene is two passes of its own (RenderBattleScene), and cheap
     * enough to be composed per eye. A stage is not: the intro and the title
     * draw their waves line by line, and twice that is more than an Old 3DS
     * has in a frame, so they take the planes like any other 2D screen. */
    if (!sStage) sStageWithoutPlanes = false;
    f->planes = f->stereo && !f->overworld && !sBattle && !sStageWithoutPlanes && BandsUsable();
    /* ... and the 2D field drawn while the voxel world is still being made
     * (the option just on): flat as well, not per priority (RenderEye's
     * layer parallax splits the field's scrolled-together backgrounds that
     * its planes keep on one plane, and the swoop's crossfade is flat too). */
    f->planesMissing = f->stereo && !f->voxel && !f->blank && !sStage && !sBattle && !f->planes;
    /* A 2D screen in 3D without its planes: flat, or a stage per eye. */
    f->planesDenied = f->stereo && !f->overworld && !sBattle && !f->planes && !f->down && !sStageZoomed;
    if (f->planesMissing) f->stereo = false;
    if (f->down) f->stereo = f->planes = f->battleStereo = false;
    /* A magnified stage is one flat picture (RenderEye). */
    if (sStageZoomed) f->stereo = f->planes = false;
    /* With the Pokédex on the bottom screen the top shows the device, which
     * takes any slider position (RenderDexTop). */
    f->dexTop = f->down && CtrDexTop_Active();
    if (f->dexTop) f->stereo = sTopRight && f->slider > 0.02f;
    else CtrDexTop_Idle();
}

/* The planes' parallax ramp (sPlaneRamp): in while they are used, out
 * once the voxel world is wanted (its VRAM waits for it, VoxelVramManage). */
VIDEO_PIECE void FramePlaneRamp(const PresentFrame *f)
{
    if (!f->down)
    {
        bool planesLeaving = f->field && CtrSettings_Voxel();
        bool swooping = VoxelSwoop_Active(&sSwoop);
        float step = 1.0f / (float)CTR_PLANE_RAMP_FRAMES;

        if (f->planes && !planesLeaving) sPlaneRamp = sPlaneRamp + step > 1.0f ? 1.0f : sPlaneRamp + step;
        else if (f->planes) sPlaneRamp = sPlaneRamp - step < 0.0f ? 0.0f : sPlaneRamp - step;
        /* Not used and no swoop to keep them under: nothing to ease. The
         * swoop's crossfade takes the ramp where it stood (Field2DOver). */
        else if (planesLeaving && !swooping) sPlaneRamp = 0.0f;
        /* Leaving the voxel world with the slider up: the planes cannot be
         * made until the world's VRAM is parked, after the swoop, so the
         * crossfade's 2D field takes its layers to depth itself (Field2DOver
         * through sParallax), and the planes arrive at that depth instead
         * of sliding in afterwards as a second motion. */
        else if (!f->planes && !planesLeaving && f->field && swooping && sTopRight && f->slider > 0.0f)
            sPlaneRamp = sPlaneRamp + step > 1.0f ? 1.0f : sPlaneRamp + step;
    }
}

/* The 3D the screen is set to, and what the frame profiler is told. */
VIDEO_PIECE void FrameRoute(const PresentFrame *f)
{
    if (f->stereo != sStereo) { gfxSet3D(f->stereo); sStereo = f->stereo; }
    /* The voxel world reports the slider in tenths, the 2D path its pixels. */
    sStats.stereo = !f->stereo ? 0 : f->voxelStereo ? (uint32_t)ceilf(f->slider * 10.0f)
                  : f->battleStereo || f->worldStereo ? ceilf(f->slider * BATTLE_BACK_DEPTH - 0.001f)
                  : roundf(f->slider * CTR_STEREO_PIXELS);
    /* What this frame is and how it is drawn, for the frame profiler: the
     * same choice as FrameDraw's chain. */
    sStats.route = f->down ? CTR_ROUTE_BOTTOM
                 : sTransition && !f->blank ? CTR_ROUTE_TRANSITION
                 : f->voxel ? (f->voxelStereo && f->stereo ? CTR_ROUTE_VOXEL_STEREO : CTR_ROUTE_VOXEL)
                 : f->blank ? CTR_ROUTE_BLANK
                 : sBattleWorld ? (f->worldStereo ? CTR_ROUTE_BATTLE_WORLD_STEREO : CTR_ROUTE_BATTLE_WORLD)
                 : !f->stereo ? CTR_ROUTE_MONO
                 : f->battleStereo ? CTR_ROUTE_BATTLE_STEREO
                 : f->planes ? CTR_ROUTE_PLANES : CTR_ROUTE_EYES;
    /* The black shown is the warm-up's (3ds_game_full.c, HoldFieldFade): not
     * the stale terrain's, drawn black the same way but not holding the fade. */
    sStats.warmupBlack = f->warmupBlack && sStats.route == CTR_ROUTE_BLANK;
    if (f->planesDenied) SurfaceDenied(CTR_DENIED_PLANE);
    bool voxelBattle = sBattle && f->battleUpdated;
    sStats.mode = f->down ? CTR_MODE_BOTTOM
                : sBattle ? (voxelBattle ? CTR_MODE_BATTLE_VOXEL : CTR_MODE_BATTLE)
                : f->overworld ? CTR_MODE_FIELD_VOXEL
                : f->field ? CTR_MODE_FIELD
                : sStage ? CTR_MODE_STAGE
                : sCentred ? CTR_MODE_CENTRED : CTR_MODE_OTHER;
    sStats.sliderPct = (uint8_t)roundf(f->slider * 100.0f);
    if (f->slider > 0.02f && sStats.stereo == 0)
        sFrameFlat |= f->voxel ? CTR_FLAT_NO_VOXEL
                    : sBattleWorld && f->worldStereo ? CTR_FLAT_NO_SURFACE
                    : f->planesMissing ? CTR_FLAT_NO_PLANES : CTR_FLAT_OTHER;
}

/* The battle's info boxes and text box (DrawBattleTextHd, HudDrawCrisp). */
VIDEO_PIECE void FrameHud(const PresentFrame *f)
{
    HudFrame();
    BoxFrame();
    sHudCrisp = sHud && sScene && !f->down && !f->blank && !sTransition && !f->voxel && HudCrispPossible();
    /* Why the boxes are drawn into the scene (magnified with it) when they
     * are: each change, the first few hundred of a session. */
    static int lastCrisp = -1;
    static unsigned told;

    if (sHud && (int)sHudCrisp != lastCrisp && told < 200)
    {
        ++told;
        CtrLog_Write(CTR_LOG_VIDEO, "HUD boxes %s f=%lu covered=%d stereo=%d world=%d dispcnt=%04x winin=%04x winout=%04x bgcnt=%04x/%04x/%04x/%04x",
                     sHudCrisp ? "at screen pixels" : "in the scene", (unsigned long)sStats.frames,
                     sHudCovered, f->stereo, sBattleWorld, Reg(0), Reg(0x48), Reg(0x4A),
                     Reg(8), Reg(0xa), Reg(0xc), Reg(0xe));
        lastCrisp = sHudCrisp;
    }
}

/* The layer textures' cells and the OBJ window, before anything is composed. */
VIDEO_PIECE void FrameLayers(PresentFrame *f)
{
    /* The swoop's crossfade: the 2D field is composed over the voxel world. */
    f->fade2d = 0.0f;
    f->blur2d = 0.0f;
    if (f->voxel && !f->down)
    {
        VoxelSwoopFrame swoop;

        VoxelSwoop_Frame(&sSwoop, &swoop);
        f->fade2d = swoop.fade2d;
        f->blur2d = swoop.blur;
    }
    bool layersQueued = ((!f->voxel && !f->blank) || f->fade2d > 0.0f) && f->down == f->bottom && LayersRender();
    /* Before anything is composed: it is drawn from within (ComposeBand). */
    sObjWin.ready = false;
    if (!f->voxel && !f->blank && !f->down) ObjWindowCompose();
    /*
     * The layer textures' changed cells and the OBJ window are the first GPU
     * work of a 2D frame: started on them, the GPU draws them while the CPU
     * composes the frame from them, instead of waiting for all of it
     * (gpuStart was the whole present on every 2D route; console log of
     * 0b3b36d91). The routes of the voxel world and the 3D battle start at
     * their own points, after the world is drawn (a transition over it too);
     * the bottom screen's at FrameEnd (RenderEye).
     */
    if ((layersQueued || sObjWin.ready)
        && (sStats.route == CTR_ROUTE_MONO || sStats.route == CTR_ROUTE_EYES
            || sStats.route == CTR_ROUTE_PLANES || (sStats.route == CTR_ROUTE_TRANSITION && !f->voxel)))
    {
        sGpuStage = CTR_STAGE_LAYERS;
        GpuStartEarly(CTR_EARLY_LAYERS);
    }
}

/* The frame's picture, by the route FrameRoute named. */
VIDEO_PIECE void FrameDraw(const PresentFrame *f)
{
    sPlanes = 0;
    if (f->down)
    {
        /* The top screen is not drawn: it keeps the frame it showed last -
         * unless the Pokédex is open down there. */
        if (f->bottom) RenderEye(sBottom, f->clear, 0.0f);
        if (f->dexTop) RenderDexTop(f->stereo, f->slider);
        else if (sHeldTop.kept && sHoldTop) HeldTopPutBack();
        /* Drawn afresh: darkened again below if it is to be. */
        if (f->dexTop) sTopDark = false;
    }
    else if (sTransition && !f->blank) RenderTransition(f->voxel, f->clear);
    else if (f->voxel)
    {
        RenderVoxel(f->voxelClear, f->voxelStereo && f->stereo ? f->slider : 0.0f);
        if (f->fade2d > 0.004f)
        {
            /* The 2D field's own stereo pixels, as its planes would shift
             * them with the slider where it is. */
            float field2d = sTopRight && f->slider > 0.0f ? roundf(f->slider * CTR_STEREO_PIXELS) * sPlaneRamp : 0.0f;

            Field2DOver(f->fade2d, f->blur2d, f->clear, sTop, field2d);
            if (f->voxelStereo && f->stereo) Field2DOver(f->fade2d, f->blur2d, f->clear, sTopRight, -field2d);
        }
    }
    else if (f->blank) C2D_TargetClear(sTop, C2D_Color32(0, 0, 0, 255));
    else if (sBattleWorld) RenderBattleWorld(f->clear, f->worldStereo ? f->slider : 0.0f);
    else if (!f->stereo) RenderEye(sTop, f->clear, 0.0f);
    else if (f->battleStereo) RenderBattleStereo(f->clear, f->slider, NULL);
    else if (f->planes)
    {
        sPlanes = DepthPlanes();
        RenderBands(sPlanes, f->clear);
        BlitBands(sTop, sPlanes, sStats.stereo);
        BlitBands(sTopRight, sPlanes, -(float)sStats.stereo);
    }
    else
    {
        /* The battle, or a stage without memory for its planes. The GPU is
         * on the first eye before the second is composed (RenderEye). */
        RenderEye(sTop, f->clear, sStats.stereo);
        RenderEye(sTopRight, f->clear, -(float)sStats.stereo);
    }
}

/* The Fly cinematic over the finished picture, on each eye, and its step
 * (voxel_fly.c), paced by the game's frames as the swoop is. */
VIDEO_PIECE void FrameFly(const PresentFrame *f)
{
    if (!f->down && VoxelFly_Active(&sFly))
    {
        VoxelFlyFrame fly;
        uint64_t gameFrame = CtrPlatform_FrameCount();
        unsigned steps = sFlyLastGameFrame != 0 && gameFrame > sFlyLastGameFrame
                       ? (unsigned)(gameFrame - sFlyLastGameFrame) : 1u;

        VoxelFly_Frame(&sFly, &fly);
        /* The wind over the world while the camera is up, under the cover;
         * then the cloud cover over whatever the frame is - the world, the
         * warp's black - its sheets in front of the screen in 3D. */
        if (f->voxel && fly.lift > 0.005f && fly.whiteout < 0.995f)
        {
            float rush = fminf(fmaxf(fly.lift, 0.0f), 1.0f) * (1.0f - fly.whiteout);

            DrawWind(sTop, rush, f->stereo ? 0 : -1);
            if (f->stereo) DrawWind(sTopRight, rush, 1);
        }
        if (fly.whiteout > 0.005f)
        {
            DrawCloudCover(sTop, fly.whiteout, f->stereo ? 0 : -1);
            if (f->stereo) DrawCloudCover(sTopRight, fly.whiteout, 1);
        }
        /* The sky scene over the closed cover. */
        {
            unsigned skyFrame;

            if (VoxelFly_Sky(&sFly, &skyFrame) && sSky.loaded)
            {
                sFlySkyShown = true;
                SkySounds(skyFrame);
                DrawSkyScene(sTop, f->stereo ? 0 : -1, skyFrame);
                if (f->stereo) DrawSkyScene(sTopRight, 1, skyFrame);
            }
        }
        /* Paced by the game's frames, as the swoop is. Over the old map the
         * world must be there to fly over: gone early (the field left for
         * something else), the cinematic is dropped and Fly plays on as the
         * GBA's. Inside the cloud a frame with no field is the warp. */
        if (steps > VOXEL_SWOOP_STEPS_MAX) steps = VOXEL_SWOOP_STEPS_MAX;
        sFlyLastGameFrame = gameFrame;
        if (VoxelFly_Hovers(&sFly) && !f->voxel)
        {
            CtrLog_Write(CTR_LOG_VIDEO, "FLY cinematic dropped: no voxel field in phase %u", (unsigned)sFly.phase);
            VoxelFly_Cancel(&sFly);
        }
        else
        {
            VoxelFlyPhase was = sFly.phase;

            bool gone = !f->field || CtrMap_Version() != sFlyMapVersion;

            while (steps-- != 0 && VoxelFly_Active(&sFly))
                VoxelFly_Step(&sFly, f->voxel, gone);
            if (sFly.phase != was)
                CtrLog_Write(CTR_LOG_VIDEO, "FLY cinematic phase %u -> %u (frame %lu)", (unsigned)was,
                             (unsigned)sFly.phase, (unsigned long)sStats.frames);
        }
    }
    /* The scene's sprites, once the flight is over (or dropped). */
    if (sSky.loaded && !VoxelFly_Active(&sFly))
        SkySceneFree();
}

/* What goes over the finished picture, on each eye, and the swoop's step. */
VIDEO_PIECE void FrameOverlays(const PresentFrame *f)
{
    if (sStats.frames < sNoHomeUntil && !f->down)
    {
        DrawNoHome(sTop);
        if (f->stereo) DrawNoHome(sTopRight);
    }
    if (f->field && !f->down && !f->blank && CtrGame_FieldPoison() && PoisonVeil() != 0)
    {
        DrawPoisonVeil(sTop, PoisonVeil());
        if (f->stereo) DrawPoisonVeil(sTopRight, PoisonVeil());
    }
    FrameFly(f);
    if (!f->down && VoxelSwoop_Active(&sSwoop))
    {
        VoxelSwoopFrame swoop;

        VoxelSwoop_Frame(&sSwoop, &swoop);
        if (swoop.flash > 0.005f || swoop.bloom > 0.005f)
        {
            DrawSwoopLight(sTop, &swoop);
            if (f->stereo) DrawSwoopLight(sTopRight, &swoop);
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
            if (sSwoop.phase == VOXEL_SWOOP_FLASH_IN && f->voxel && CtrVoxel_GetStats()->chunksMissing != 0
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
    /* The top drawn afresh, or kept as it was (down): darkened on it once. */
    if (!f->down) sTopDark = false;
    if (sLookDown && !sTopDark)
    {
        sSceneOn = NULL;
        DrawLookDown(sTop);
        if (f->stereo || (f->down && !f->dexTop && sStereo && sTopRight)) DrawLookDown(sTopRight);
        /* The frame shown has it: a hold from here (X, then the party)
         * keeps this one, not to be darkened again. */
        sTopDark = true;
    }
#if CTR_SHOW_FPS
    if (CtrSettings_ShowFps())
    {
        if (!f->down || f->dexTop) DrawFps(sTop);
        if (f->stereo) DrawFps(sTopRight);
    }
#endif
    {
        int speedHalves = CtrPlatform_BattleSpeedBadge();

        if (speedHalves != 0)
        {
            if (!f->bottom || f->dexTop) DrawSpeedBadge(sTop, speedHalves);
            if (f->stereo) DrawSpeedBadge(sTopRight, speedHalves);
        }
    }
    /* Queued in the frame, behind the drawing into sBottom (BottomTransfer). */
    if (f->bottom) BottomTransfer();
}

/* The GPU given the frame: FrameEnd, the early start finished first.
 * Returns when it started, in ms into the frame; negative if it starts now. */
VIDEO_PIECE float FrameSubmit(uint64_t start)
{
    bool early = sGpuEarly, running = GpuFinishEarly();
    float gpuStartMs = early ? (sGpuEarlyStart - start) * 1000.0f / SYSCLOCK_ARM11 : -1.0f;
    /* Where it started, per picture: the frame profiler counts it by
     * route and point (3ds_prof.c, "PERF seg v4 gpu early"). */
    sStats.gpuEarly = early ? sGpuEarlyAt : CTR_EARLY_NONE;

    if (early) { sEarlyWaitSum += running; ++sEarlyFrames; }
    QualityFrameEnd();
    PictureLog600();
    StereoLog600();
    {
        /* The command buffer (C3D_Init): its fullest frame, for the
         * memory budget. */
        float used = C3D_GetCmdBufUsage();

        if (used > sCmdBufPeak) sCmdBufPeak = used;
    }
    sGpuStage = CTR_STAGE_END;
    C3D_FrameEnd(running ? GX_CMDLIST_FLUSH : 0);
    sGpuEarly = false;
    return gpuStartMs;
}

/* After FrameEnd: the frame's figures, and the voxel world's time while the
 * GPU draws. */
VIDEO_PIECE void FrameFinish(const PresentFrame *f, float gpuStartMs)
{
    sStats.flat = sFrameFlat;
    sStats.denied = sDeniedNow;
    sStats.bgMs = sBgTicks * 1000.0f / SYSCLOCK_ARM11;
    sStats.objMs = sObjTicks * 1000.0f / SYSCLOCK_ARM11;
    sStats.sceneWhy = sSceneWhy;
    sStats.textWhy = sTextWhy;
    sStats.slotsAgain = sSlotsAgainNow;
    sStats.slotsUnkept = sSlotsUnkeptNow;
    for (unsigned k = 0; k < CTR_DENIED_KINDS; ++k)
        sDeniedFrames[k] += (sDeniedNow >> k) & 1;
    SurfacesLog600();
    ++sStats.frames;
    ++sFpsFrames;
    sStats.cpuMs = (svcGetSystemTick() - f->start) * 1000.0 / SYSCLOCK_ARM11;
    sStats.gpuStartMs = gpuStartMs >= 0.0f ? gpuStartMs : sStats.cpuMs;
    sStats.voxelFrame = f->voxel || f->battleUpdated;
    sReport.image = ReportSubmit(sStats.gpuStartMs, f->start);
    /* The GPU draws the frame now: the voxel world builds what comes next in
     * the time the CPU would otherwise wait for the VBlank. */
    if (f->overworld || f->battleUpdated)
        CtrVoxel_AfterSubmit(f->start);
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
    FrameLog600(f->voxel);
}
