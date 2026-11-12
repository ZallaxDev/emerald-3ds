/*
 * Part of 3ds_video.c. The voxel world's VRAM and building pages, once a
 * frame: VoxelVramManage and VoxelPagesManage.
 */

/*
 * The voxel world's VRAM follows its option (CtrVoxel_ParkVram): given back a
 * second after the world was last wanted, claimed again as soon as it is.
 * Claimed again after the 2D surfaces held for other screens go - they come
 * back on demand - so that the arenas find VRAM in one piece, as at start-up.
 * Outside the frame, like every release here.
 *
 * Wanted is the option and a screen the world can be shown on having been
 * reached: the field, a battle or a transition into one (sVoxelReached). The
 * world starts parked (CtrVoxel_Init), so the intro, the title and the main
 * menu have its 2.6 MiB for their layers and planes instead of holding them
 * for nothing (console log of 0b3b36d91), and its arenas are claimed on the
 * first frame of the field - before it is drawn, in the same preparation,
 * from VRAM the screens before have given back. Reached stays reached: a
 * soft reset back to the intro keeps the world as it is, rather than park
 * it there (a stall, a quarter second on a New 3DS). A claim that finds no
 * room leaves the world parked, and the field is the 2D one meanwhile, never
 * black (CtrVoxel_IsAvailable is false while parked): counted (claims
 * failed, CTR_DENIED_VOXEL every frame it waits) and tried again every
 * VOXEL_UNPARK_RETRY_FRAMES, each time after the 2D surfaces held for other
 * screens go; the swoop brings the world in once it is there.
 *
 * Given back, it is never while a picture the GPU may still be drawing reads
 * it: the last frame that drew the world is two frames back or more
 * (FrameBegin of the frame between waited for it).
 *
 * Brought back without its building pages (the unpark's last try,
 * CtrVoxel_UnparkVram), the world is up but degraded - every house its cells'
 * flat drawing - and that is a denial like the others: CTR_DENIED_VOXEL
 * every frame of it, the pages' claims and failures in the 600-frame line.
 * Nothing would park the world again with the option on, so the pages are
 * asked for again every VOXEL_UNPARK_RETRY_FRAMES with the world up
 * (VoxelPagesManage): only on the field with the world drawn the frame
 * before - never in a battle (no new VRAM is held there), a transition or
 * the swoop (the buildings' loading stalls the frame) - each time after the
 * surfaces held for other screens go, as for the whole claim.
 */
#define VOXEL_PARK_FRAMES 60u
#define VOXEL_UNPARK_RETRY_FRAMES 120u
static bool sVoxelReached;
static uint32_t sWorldDrawnFrame;
static uint32_t sVoxelClaims, sVoxelClaimFails;
static uint32_t sPageClaims, sPageClaimFails;

/* With the world up: its building pages, if it came back without them. */
static void VoxelPagesManage(void)
{
    static uint32_t sPageRetryFrame;

    if (!CtrVoxel_PagesMissing())
        return;
    SurfaceDenied(CTR_DENIED_VOXEL);
    if (sStats.frames < sPageRetryFrame || CtrVoxel_InBattle() || sBattle || sBattleRequested
        || sTransition || sTransitionRequested || VoxelSwoop_Active(&sSwoop)
        || sStats.frames - sWorldDrawnFrame > 1u)
        return;
    /* None of these is drawn from on the voxel field; they come back on
     * demand (the layers and the OBJ window are the field's own and
     * stay). */
    if (sBandsReady) BandsRelease();
    SceneRelease();
    SlotsRelease();
    if (sBottom && !sBottomInUse) BottomRelease();
    if (CtrVoxel_ClaimPages())
    {
        ++sPageClaims;
        CtrLog_Write(CTR_LOG_VIDEO, "VOXEL: building pages back at frame %lu (page claims %lu, failed %lu)",
                     (unsigned long)sStats.frames, (unsigned long)sPageClaims, (unsigned long)sPageClaimFails);
        return;
    }
    if (sPageClaimFails++ == 0)
        CtrLog_Write(CTR_LOG_ERROR, "VOXEL: the world up without its building pages, no room for them yet "
                     "(free=%lu, largest=%lu); retrying every %u frames, counted in the 600-frame line",
                     (unsigned long)vramSpaceFree(), (unsigned long)CtrGpuMem_LargestVram(),
                     VOXEL_UNPARK_RETRY_FRAMES);
    sPageRetryFrame = sStats.frames + VOXEL_UNPARK_RETRY_FRAMES;
}

/* With the world up: SHADOW QUALITY HIGH's map, if the world was claimed
 * without it (the option changed since). The quality is the option's all
 * the while; only its shadows wait for the room. */
static void VoxelShadowManage(void)
{
    static uint32_t sShadowRetryFrame, sShadowClaimFails;

    if (!CtrVoxel_ShadowMapMissing() || sStats.frames < sShadowRetryFrame)
        return;
    /* None of these is drawn from on the voxel field; they come back on
     * demand. Not a battle's or a transition's own. */
    if (!CtrVoxel_InBattle() && !sBattle && !sBattleRequested && !sTransition && !sTransitionRequested)
    {
        if (sBandsReady) BandsRelease();
        SceneRelease();
        SlotsRelease();
        if (sBottom && !sBottomInUse) BottomRelease();
    }
    if (CtrVoxel_ClaimShadowMap())
        return;
    if (sShadowClaimFails++ == 0)
        CtrLog_Write(CTR_LOG_ERROR, "VOXEL: no room for the shadow map yet (free=%lu, largest=%lu); "
                     "retrying every %u frames", (unsigned long)vramSpaceFree(),
                     (unsigned long)CtrGpuMem_LargestVram(), VOXEL_UNPARK_RETRY_FRAMES);
    sShadowRetryFrame = sStats.frames + VOXEL_UNPARK_RETRY_FRAMES;
}

static void VoxelVramManage(void)
{
    static uint32_t sUnwanted, sRetryFrame;
    bool wanted;

    if (!sVoxelReached && (CtrGame_IsOverworld() || sBattleRequested || sTransitionRequested))
    {
        sVoxelReached = true;
        CtrLog_Write(CTR_LOG_VIDEO, "VOXEL: a screen the world shows on reached at frame %lu (option %s)",
                     (unsigned long)sStats.frames, CtrSettings_Voxel() ? "on" : "off");
    }
    wanted = CtrSettings_Voxel() && sVoxelReached;
    if (!wanted)
    {
        /* A voxel battle ends when the field's world next updates: with the
         * world switched off on the way back, never, and its VRAM stayed
         * held (the scene cycling test: the next 2D battle found no room for
         * its scene surface). No battle on show: it is over. */
        if (CtrVoxel_InBattle() && !sBattle && !sBattleRequested)
            CtrVoxel_EndBattle();
        /* Not under the swoop: parking stalls the frame (a quarter second on
         * a New 3DS), and the world is still on show through it. At once
         * when a layer of the 2D field waits for this VRAM (sLayersWaitWorld):
         * the world is not drawn any more, and every frame it keeps the VRAM
         * that layer is walked tile by tile. */
        ++sUnwanted;
        if ((sUnwanted >= VOXEL_PARK_FRAMES || sLayersWaitWorld) && CtrVoxel_HoldsVram()
            && !CtrVoxel_InBattle() && !VoxelSwoop_Active(&sSwoop) && sStats.frames - sWorldDrawnFrame >= 2u)
        {
            CtrVoxel_ParkVram();
            /* The planes the 2D field asked for when the world still held
             * this VRAM wait for their retry no longer: with the world just
             * switched off they waited 300 frames, flat. */
            if (sBandsFailed)
                sBandsRetryFrame = sStats.frames;
            /* Nor the layers that asked for it (the anticipated request):
             * made in this same preparation (LayersPrepare runs next), so the
             * first frame without the world has them. Waiting their retry,
             * the field was walked tile by tile for up to 300 frames more. */
            for (unsigned bg = 0; bg < 4; ++bg)
                if (sLayersWaitWorld & (1u << bg))
                    sLayerFail[bg] = 0;
            if (sLayersWaitWorld)
                CtrLog_Write(CTR_LOG_VIDEO, "VOXEL: VRAM parked for the 2D field's layers (%x)", sLayersWaitWorld);
        }
        if (!CtrVoxel_HoldsVram())
            sLayersWaitWorld = 0;
        return;
    }
    sUnwanted = 0;
    sLayersWaitWorld = 0;
    if (CtrVoxel_HoldsVram())
    {
        VoxelPagesManage();
        VoxelShadowManage();
        return;
    }
    if (sStats.frames < sRetryFrame)
    {
        SurfaceDenied(CTR_DENIED_VOXEL);
        return;
    }
    /* The field's planes still at depth: their parallax eases out first
     * (CTR_PLANE_RAMP_FRAMES), and the world comes back to a flat field.
     * Likewise another screen's planes in use: they are not the world's to
     * take while that screen is up. Coming into the field from a screen that
     * was not the 2D field - the title, a menu - the planes on show were that
     * screen's, gone now: the world is claimed at once, as it was held at
     * start-up, and the field comes up in it rather than in 2D first. */
    {
        bool fieldNow = CtrGame_IsOverworld() && !sStageRequested && sCentredRequested == CTR_CENTRED_NONE
                        && !sBattleRequested && !sTransitionRequested;

        if (sBandsReady && sPlaneRamp > 0.0f && (sFieldMode == FIELD_MODE_PIXEL || !fieldNow))
        {
            SurfaceDenied(CTR_DENIED_VOXEL);
            return;
        }
    }
    if (sBandsReady) BandsRelease();
    LayersRelease();
    if (!sBattleRequested && !sTransitionRequested) { SceneRelease(); SlotsRelease(); }
    ObjWindowRelease();
    if (sBottom && !sBottomInUse) BottomRelease();
    if (!CtrVoxel_UnparkVram())
    {
        ++sVoxelClaimFails;
        SurfaceDenied(CTR_DENIED_VOXEL);
        sRetryFrame = sStats.frames + VOXEL_UNPARK_RETRY_FRAMES;
        CtrLog_Write(CTR_LOG_ERROR, "VOXEL: no VRAM to bring the world back (free=%lu, largest=%lu); the field "
                     "in 2D, retrying in %u frames (claims %lu, failed %lu)",
                     (unsigned long)vramSpaceFree(), (unsigned long)CtrGpuMem_LargestVram(),
                     VOXEL_UNPARK_RETRY_FRAMES, (unsigned long)sVoxelClaims, (unsigned long)sVoxelClaimFails);
    }
    else
    {
        ++sVoxelClaims;
        CtrLog_Write(CTR_LOG_VIDEO, "VOXEL: arenas claimed at frame %lu (claims %lu, failed %lu)",
                     (unsigned long)sStats.frames, (unsigned long)sVoxelClaims, (unsigned long)sVoxelClaimFails);
    }
}
