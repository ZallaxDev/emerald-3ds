/*
 * Part of 3ds_video.c. The frame's logs: a dropped frame (FrameDrops) and
 * those of every 600 frames. Each runs where Present calls it: moved, it
 * would count other frames, and perf_report.py reads their text.
 */

/* The GPU started early (GpuStartEarly): how many frames, and in how
 * many of them it was still busy when FrameEnd came. */
static float sEarlyWaitSum;
static unsigned sEarlyFrames;

/*
 * FrameBegin waits for the last frame's drawing and for a free screen
 * buffer, so the time between two returns is about a whole number of
 * display frames, but not exactly (gaps of 25-31 ms are seen): over 24 ms
 * is a frame the screen showed twice. Logged with what the frame before it spent - the present
 * (and the voxel builds in it), then the game and its VBlank handler - so
 * that a slow renderer can be told from a slow game on hardware.
 */
VIDEO_PIECE void FrameDrops(void)
{
    static unsigned sDropsLogged;
    /* A screen that runs at 30 for a while is one finding, not a line per
     * frame: a steady run is logged once a second with how many frames it
     * dropped, and only a hitch of three frames or more is always logged. */
    static uint32_t sLastDropLogged;
    static unsigned sDropsQuiet;
    float gap = sStats.gapMs;
    /*
     * Which of the two missed the VBlank: the CPU, if it came back to
     * FrameBegin after it (`arrive` past 16.0, the VBlank's own jitter
     * aside: 16.7 left a 16.7 arrival unnamed), or else the GPU, whose
     * last frame ended `gpuEnd` after that frame began - when the GPU
     * was started (early in the frame, or at FrameEnd) plus the drawing,
     * which FrameBegin has waited for.
     */
    float arrive = sStats.arriveMs;
    float gpuEnd = sStats.gpuStartMs + sStats.gpuMs;

    /* The first frame after a held top screen or a resume: its interval
     * spans the menu or the sleep, not a dropped frame. */
    if (sReport.boundary)
        gap = 0.0f;
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
                         timing->bottomMs, sStats.prepMs,
                         logMs, logAgo);
            sDropsQuiet = 0;
        }
    }
}

/* The picture's counts every 600 frames, cleared as logged: the battle's, and
 * the effects and the OBJ list, which are any frame's. */
VIDEO_PIECE void PictureLog600(void)
{
    if (sStats.frames % 600 == 599)
    {
        CtrQualityCounts q;
        uint32_t whole = 0;

        QualityDelta(&sQualityLogged, &q);
        for (unsigned i = 0; i < CTR_MONO_WHY_COUNT; ++i)
            whole += q.monoFallback[i];
        if (q.monoKept || q.monoRecomposed || q.monoEmpty || whole)
            CtrLog_Write(CTR_LOG_VIDEO, "battle frames without 3D: scenery kept in %u, composed apart in %u "
                         "(layers in front of a sprite composed with it in %u), sprites alone in %u, whole in %u "
                         "(objwin %u front-tie %u no-surface %u other %u unsettled %u), blank layers left out %u",
                         (unsigned)q.monoKept, (unsigned)q.monoRecomposed, (unsigned)q.monoFrontLayers,
                         (unsigned)q.monoEmpty, (unsigned)whole,
                         (unsigned)q.monoFallback[CTR_MONO_WHY_OBJWIN], (unsigned)q.monoFallback[CTR_MONO_WHY_FRONT],
                         (unsigned)q.monoFallback[CTR_MONO_WHY_KEEPER], (unsigned)q.monoFallback[CTR_MONO_WHY_OTHER],
                         (unsigned)q.monoFallback[CTR_MONO_WHY_UNSETTLED], (unsigned)q.monoBlankLayers);
        if (q.edgeBlankLayers)
            CtrLog_Write(CTR_LOG_VIDEO, "battle edges: blank layers measured, not walked, %u",
                         (unsigned)q.edgeBlankLayers);
        if (q.lineAlphaBanded || q.lineAlphaDropped || q.lineDropped || q.objWinIgnored || q.mosaicIgnored)
            CtrLog_Write(CTR_LOG_VIDEO, "effects in 600 frames: line BLDALPHA banded %u dropped %u, other line "
                         "registers dropped %u, OBJ window not drawn %u, mosaic not drawn %u",
                         (unsigned)q.lineAlphaBanded, (unsigned)q.lineAlphaDropped, (unsigned)q.lineDropped,
                         (unsigned)q.objWinIgnored, (unsigned)q.mosaicIgnored);
        {
            /* The OAM's draw list (3ds_obj_list.h): the frames that drew
             * from it, and those that walked all 128 entries, by why. */
            uint32_t listed, mismatched, unpublished, off;

            CtrObjList_Counts(&listed, &mismatched, &unpublished, &off);
            if (mismatched || unpublished || off)
                CtrLog_Write(CTR_LOG_VIDEO, "OBJ list in 600 frames: drawn from it %lu, all 128 walked %lu "
                             "(OAM not the one published %lu, nothing published %lu, off %lu)",
                             (unsigned long)listed, (unsigned long)(mismatched + unpublished + off),
                             (unsigned long)mismatched, (unsigned long)unpublished, (unsigned long)off);
            else if (listed)
                CtrLog_Write(CTR_LOG_VIDEO, "OBJ list in 600 frames: drawn from it %lu, all 128 walked 0",
                             (unsigned long)listed);
        }
        /* Fed by the scenery compose of frames without 3D too (BattleSceneCompose,
         * BattleSceneComposeMono): logged and cleared every 600 frames, not only
         * with the 3D block below, or its counts span several windows. */
        if (sSceneAgain[SCENE_PART_REG] || sSceneAgain[SCENE_PART_SCROLL] || sSceneAgain[SCENE_PART_LINES]
            || sSceneAgain[SCENE_PART_DRAWABLE] || sSceneAgain[SCENE_PART_VERSION])
            CtrLog_Write(CTR_LOG_VIDEO, "battle scenery composed again for its key (by part, a frame may count in "
                         "several): reg %u scroll %u linescroll %u drawable %u version %u",
                         sSceneAgain[SCENE_PART_REG], sSceneAgain[SCENE_PART_SCROLL],
                         sSceneAgain[SCENE_PART_LINES], sSceneAgain[SCENE_PART_DRAWABLE],
                         sSceneAgain[SCENE_PART_VERSION]);
        memset(sSceneAgain, 0, sizeof(sSceneAgain));
        if (sLineAlphaApart || sLineAlphaWhole || sBlendLayerNoSurface)
            CtrLog_Write(CTR_LOG_VIDEO, "battle blend layer: BLDALPHA by line apart in %u, line-alpha whole %u; "
                         "blend-layer no-surface %u", sLineAlphaApart, sLineAlphaWhole, sBlendLayerNoSurface);
        sLineAlphaApart = sLineAlphaWhole = sBlendLayerNoSurface = 0;
        if (sMonoWeights.out)
            CtrLog_Write(CTR_LOG_VIDEO, "battle frames without 3D: blend weights out of the kept scenery's key in "
                         "%u, moved in %u of them", sMonoWeights.out, sMonoWeights.moved);
        sMonoWeights.out = sMonoWeights.moved = 0;
        if (sBattleLayerCounts.drawn || sBattleLayerCounts.walked || sBattleLayerCounts.lent
            || sBattleLayerCounts.kept || sBattleLayerCounts.back)
            CtrLog_Write(CTR_LOG_VIDEO, "battle layers: drawn %u, walked %u (no-surface %u gap-small %u picture "
                         "%u not-ready %u), lent %u, kept %u, given back %u, asked %u (pages used=%luK largest "
                         "gap=%luK), terrain %u", sBattleLayerCounts.drawn, sBattleLayerCounts.walked,
                         sBattleLayerCounts.why[BATTLE_LAYER_NO_SURFACE],
                         sBattleLayerCounts.why[BATTLE_LAYER_GAP_SMALL],
                         sBattleLayerCounts.why[BATTLE_LAYER_PICTURE],
                         sBattleLayerCounts.why[BATTLE_LAYER_NOT_READY], sBattleLayerCounts.lent,
                         sBattleLayerCounts.kept, sBattleLayerCounts.back, sBattleLayerCounts.asks,
                         (unsigned long)(sBattleLayerCounts.pagesUsed >> 10),
                         (unsigned long)(sBattleLayerCounts.pagesGap >> 10), sBattleLayerCounts.terrain);
        memset(&sBattleLayerCounts, 0, sizeof(sBattleLayerCounts));
        if (sLayerPasses.walks || sLayerPasses.passes || sLayerPasses.missed)
            CtrLog_Write(CTR_LOG_VIDEO, "battle layer cells: walks %lu (seen %lu, changed %lu), colour passes %lu "
                         "(seen %lu, changed %lu), left stale %lu, back in view %lu, stale now %u, plans that "
                         "met one %lu", (unsigned long)sLayerPasses.walks, (unsigned long)sLayerPasses.walkSeen,
                         (unsigned long)sLayerPasses.walkChanged, (unsigned long)sLayerPasses.passes,
                         (unsigned long)sLayerPasses.passSeen, (unsigned long)sLayerPasses.passChanged,
                         (unsigned long)sLayerPasses.left, (unsigned long)sLayerPasses.back,
                         sCellsStale[0] + sCellsStale[1] + sCellsStale[2] + sCellsStale[3],
                         (unsigned long)sLayerPasses.missed);
        memset(&sLayerPasses, 0, sizeof(sLayerPasses));
        if (sBankColours.held || sBankColours.reached)
            CtrLog_Write(CTR_LOG_VIDEO, "battle tile banks' colours changed: nothing shown from them in %u frames, "
                         "a kept picture shown from them in %u", sBankColours.held, sBankColours.reached);
        sBankColours.held = sBankColours.reached = 0;
        if (sObjWinScene.kept || sObjWinScene.composed)
            CtrLog_Write(CTR_LOG_VIDEO, "objwin: scene kept %u, composed %u, hidden layers %u", sObjWinScene.kept,
                         sObjWinScene.composed, sObjWinScene.hidden);
        memset(&sObjWinScene, 0, sizeof(sObjWinScene));
    }
}

/* The GPU started early and the 3D battle, every 600 frames, the same way. */
VIDEO_PIECE void StereoLog600(void)
{
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
        if (sTextKept || sTextWhole[TEXT_WHOLE_KEY] || sTextWhole[TEXT_WHOLE_PICTURES]
            || sTextWhole[TEXT_WHOLE_STAMP] || sTextWhole[TEXT_WHOLE_VRAM] || sTextWhole[TEXT_WHOLE_FADE])
            CtrLog_Write(CTR_LOG_VIDEO, "3D battle text: kept in %u, whole %u (key %u [reg %u view %u palette %u "
                         "box %u] pictures %u stamps %u vram %u fade %u path %u)", sTextKept,
                         sTextWhole[TEXT_WHOLE_KEY] + sTextWhole[TEXT_WHOLE_PICTURES]
                         + sTextWhole[TEXT_WHOLE_STAMP] + sTextWhole[TEXT_WHOLE_VRAM]
                         + sTextWhole[TEXT_WHOLE_FADE] + sTextWhole[TEXT_WHOLE_PATH],
                         sTextWhole[TEXT_WHOLE_KEY], sTextAgain[TEXT_PART_REG], sTextAgain[TEXT_PART_VIEW],
                         sTextAgain[TEXT_PART_PALETTE], sTextAgain[TEXT_PART_BOX],
                         sTextWhole[TEXT_WHOLE_PICTURES], sTextWhole[TEXT_WHOLE_STAMP],
                         sTextWhole[TEXT_WHOLE_VRAM], sTextWhole[TEXT_WHOLE_FADE], sTextWhole[TEXT_WHOLE_PATH]);
        sTextKept = 0;
        memset(sTextWhole, 0, sizeof(sTextWhole));
        memset(sTextAgain, 0, sizeof(sTextAgain));
        if (sBarrierFrames[0] || sBarrierFrames[1] || sBarrierFrames[2] || sBarrierFrames[3] || sSceneryEyeFrames)
            CtrLog_Write(CTR_LOG_VIDEO, "3D battle frames: priority barrier in %u (apart %u, scenery per eye %u, "
                         "whole %u of which still straddling %u), scenery per eye in %u in all",
                         sBarrierFrames[0] + sBarrierFrames[1] + sBarrierFrames[2] + sBarrierFrames[3],
                         sBarrierFrames[0], sBarrierFrames[1], sBarrierFrames[2] + sBarrierFrames[3],
                         sBarrierFrames[3], sSceneryEyeFrames);
        memset(sBarrierFrames, 0, sizeof(sBarrierFrames));
        sSceneryEyeFrames = 0;
        if (sWorldScene.kept || sWorldScene.key || sWorldScene.vram || sWorldScene.still || sWorldScene.slots
            || sWorldScene.whole || sWorldScene.late || sWorldScene.other)
            CtrLog_Write(CTR_LOG_VIDEO, "3D battle over the world: scenery kept in %u; composed again for its "
                         "key %u, VRAM %u, measure %u, slots in its columns %u, whole %u, late %u, other %u",
                         sWorldScene.kept, sWorldScene.key, sWorldScene.vram, sWorldScene.still,
                         sWorldScene.slots, sWorldScene.whole, sWorldScene.late, sWorldScene.other);
        memset(&sWorldScene, 0, sizeof(sWorldScene));
        if (StraightCounted())
            CtrLog_Write(CTR_LOG_VIDEO, "3D battle scenery: straight onto the eyes in %u; composed: kept %u, path %u, "
                         "window %u, effect %u, layers %u, not ready %u, stale %u, still %u, no sprites %u, whole %u, "
                         "off %u", sStraightFrames, sStraightWhy[STRAIGHT_KEPT], sStraightWhy[STRAIGHT_PATH],
                         sStraightWhy[STRAIGHT_WINDOW], sStraightWhy[STRAIGHT_EFFECT],
                         sStraightWhy[STRAIGHT_LAYERS], sStraightWhy[STRAIGHT_READY], sStraightWhy[STRAIGHT_STALE],
                         sStraightWhy[STRAIGHT_STILL], sStraightWhy[STRAIGHT_SPRITES],
                         sStraightWhy[STRAIGHT_WHOLE], sStraightWhy[STRAIGHT_OFF]);
        sStraightFrames = 0;
        memset(sStraightWhy, 0, sizeof(sStraightWhy));
        if (sStereoWhy[0])
            CtrLog_Write(CTR_LOG_VIDEO, "3D battle blend kinds: st-obj %u bld-obj %u objwin-nogroup %u "
                         "objwin-cut %u bg-blend %u", sBlendWhy[0], sBlendWhy[1], sBlendWhy[2], sBlendWhy[3],
                         sBlendWhy[4]);
        if (sSlotsInScene || sSlotsCutTried)
            CtrLog_Write(CTR_LOG_VIDEO, "3D battle slots: in the scene surface's columns in %u frames, the scene "
                         "composed again for them in %u (not kept whole in %u), cut to what draws in %u frames "
                         "of %u tried (%u slots)", sSlotsInScene, sSlotsInSceneComposed, sSlotsInSceneUnkept,
                         sSlotsCutFrames, sSlotsCutTried, sSlotsCut);
        {
            unsigned unkept = 0;

            for (unsigned i = 0; i < SLOTS_UNKEPT_COUNT; ++i)
                unkept += sSlotsUnkept[i];
            if (sSlotsRegionsKept || sSlotsRegionsComposed || unkept || sGatherHits || sGatherMade)
                CtrLog_Write(CTR_LOG_VIDEO, "3D battle slots kept: regions kept %u, composed again %u (new %u "
                             "pack %u moved %u palette %u blend %u window %u tiles %u), not keepable %u (scene "
                             "%u bg %u objwin %u hud %u weather %u mosaic %u line %u size %u); groups kept %u, "
                             "gathered again %u (too many to keep %u)",
                             sSlotsRegionsKept, sSlotsRegionsComposed, sSlotsAgain[SLOTS_AGAIN_NEW],
                             sSlotsAgain[SLOTS_AGAIN_PACK], sSlotsAgain[SLOTS_AGAIN_MOVED],
                             sSlotsAgain[SLOTS_AGAIN_PALETTE], sSlotsAgain[SLOTS_AGAIN_BLEND],
                             sSlotsAgain[SLOTS_AGAIN_WINDOW], sSlotsAgain[SLOTS_AGAIN_TILES], unkept,
                             sSlotsUnkept[SLOTS_UNKEPT_SCENE], sSlotsUnkept[SLOTS_UNKEPT_BG],
                             sSlotsUnkept[SLOTS_UNKEPT_OBJWIN], sSlotsUnkept[SLOTS_UNKEPT_HUD],
                             sSlotsUnkept[SLOTS_UNKEPT_WEATHER], sSlotsUnkept[SLOTS_UNKEPT_MOSAIC],
                             sSlotsUnkept[SLOTS_UNKEPT_LINE], sSlotsUnkept[SLOTS_UNKEPT_SIZE],
                             sGatherHits, sGatherMade, sGatherBig);
            sSlotsRegionsKept = sSlotsRegionsComposed = 0;
            memset(sSlotsAgain, 0, sizeof(sSlotsAgain));
            memset(sSlotsUnkept, 0, sizeof(sSlotsUnkept));
            sGatherHits = sGatherMade = sGatherBig = 0;
        }
        sSlotsInScene = sSlotsInSceneComposed = sSlotsInSceneUnkept = 0;
        sSlotsCutFrames = sSlotsCutTried = sSlotsCut = 0;
        memset(sBlendWhy, 0, sizeof(sBlendWhy));
        memset(sStereoWhy, 0, sizeof(sStereoWhy));
        if (sEyesFilled || sEyesDrawn)
            CtrLog_Write(CTR_LOG_VIDEO, "3D battle clears: eyes filled %u, drawn %u", sEyesFilled, sEyesDrawn);
        sEyesFilled = sEyesDrawn = 0;
        sSceneReused = 0;
        sEdgesKept = 0;
        sSceneEmptyReused = 0;
        sFlushTicks = 0;
        sFlushes = 0;
        sEarlyWaitSum = 0;
        sEarlyFrames = 0;
        sStereoSplit[0] = sStereoSplit[1] = 0;
    }
}

/* Surfaces denied, every 600 frames, once this frame's are added up. */
VIDEO_PIECE void SurfacesLog600(void)
{
    if (sStats.frames % 600 == 599)
    {
        CtrLog_Write(CTR_LOG_VIDEO, "surfaces denied in 600 frames: layer %u plane %u bottom %u scene %u keeper %u "
                     "blend %u slots %u voxel %u (world claims %lu, failed %lu; building pages %s, "
                     "claimed %lu, failed %lu); VRAM free=%lu largest=%lu",
                     (unsigned)sDeniedFrames[0], (unsigned)sDeniedFrames[1], (unsigned)sDeniedFrames[2],
                     (unsigned)sDeniedFrames[3], (unsigned)sDeniedFrames[4], (unsigned)sDeniedFrames[5],
                     (unsigned)sDeniedFrames[6], (unsigned)sDeniedFrames[7], (unsigned long)sVoxelClaims,
                     (unsigned long)sVoxelClaimFails, CtrVoxel_PagesMissing() ? "missing" : "ok",
                     (unsigned long)sPageClaims, (unsigned long)sPageClaimFails, (unsigned long)vramSpaceFree(),
                     (unsigned long)CtrGpuMem_LargestVram());
        memset(sDeniedFrames, 0, sizeof(sDeniedFrames));
        CtrLog_Write(CTR_LOG_VIDEO, "texture cache dropped before a draw for texels written after the GPU "
                     "started: %lu in 600 frames", (unsigned long)sTexelsLateDrops);
        sTexelsLateDrops = 0;
    }
}

/* The frame line, past ++sStats.frames: it reads this frame's times. */
VIDEO_PIECE void FrameLog600(bool voxel)
{
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
                         "anim=%u/%u passes=%u/%uk/%ub/%uf/%uw refl=%u drafts=%u/%u ahead lost=%u skipped=%u",
                         stats->visibleChunks, stats->chunks, stats->chunksMissing,
                         stats->pendingBuilds, stats->meshRebuilds, stats->atlasRebuilds,
                         stats->vertices, stats->meshMs, stats->meshPeakMs,
                         stats->updateMs, stats->updatePeakMs, stats->dropped,
                         stats->animationUploads, stats->animatedMetatiles,
                         stats->animationPasses, stats->animationKept, stats->animationBegins,
                         stats->animationPeakFrames, stats->animationWaits,
                         stats->reflections, stats->draftsVisible, stats->draftsMade,
                         stats->aheadLost, stats->aheadSkipped);
        }
    }
}
