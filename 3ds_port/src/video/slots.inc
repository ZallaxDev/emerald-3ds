/*
 * Part of 3ds_video.c. The 3D battle's memory: the sprite slots, the blending
 * layer, the large layers and who gives way to whom (BattleLayersYield,
 * BattleLayerMake).
 */

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
 * depth, blended with what is there as the 2D battle blends it (LooseDraw).
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
/* Bumped whenever the slot surface's texels stop being what its slots left
 * there (SlotsRegionKept): made, given back, cleared late, or written by the
 * battle's kept scenery without the slider. */
static uint32_t sSlotSurface;
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
/* The battle's scenery kept in the slot surface without the 3D slider
 * (BattleSceneComposeMono): its key, when it was composed, the tiles each
 * background drew. Dropped whenever the slots draw there. */
static bool sMonoKeeperHeld;
static uint32_t sMonoKeeperKey, sMonoKeeperToken;
static unsigned sMonoKeeperDrawn[4];
static StillRecord sMonoKeeperStill;
/* The last frame that looked at the kept scenery's key: that key, and the
 * token of the look (sStats.frames + 1 then; 0, none), and the key's parts
 * (SceneKeyWhy). */
static uint32_t sMonoLastKey, sMonoLastToken;
static SceneKeyParts sMonoLastParts;
/*
 * The weights left out of the keeper's key (BattleSceneComposeMono), counted
 * per 600 frames: the looks at it without them, and those of them where the
 * weights had moved since the last such look (`weights`, `lines`) - frames
 * the whole key would have called a scenery changing.
 */
static struct { unsigned out, moved; uint32_t weights, lines; } sMonoWeights;
#define SLOT_WORLD_RESERVE (128u * 1024u)
/*
 * The 2D battle without the 3D slider asks for the slot surface for its kept
 * scenery only (sSlotMonoWanted), and holds it then as a speed-up alone
 * (sSlotMono): asked for every frame, it would hold 256 KiB for the whole
 * battle, made at an atlas's cost - in view too - while a move's line layer
 * (Dig, Extrasensory) found no room and was drawn flat, with the voxel field
 * kept beside a 2D battle (port.log: 210 KiB free once the scene surface
 * is made). So it is made only out of VRAM nobody holds, with a move's layer's
 * worth left over in one block, never at anything's cost, and it gives way to
 * any surface that finds no room (SlotsYield), retried after LAYER_RETRY_FRAMES.
 */
static bool sSlotMonoWanted, sSlotMono;
static uint32_t sSlotMonoFailFrame;
#define SLOT_MONO_RESERVE (256u * 1024u)
/*
 * Back from the voxel field VRAM is in pieces - on hardware 642 KiB free and
 * no block of 310 KiB (port.log 2026-11-01) - and the keeper was never made:
 * a mono 2D battle composed its scenery whole, 381 frames of 600. The world's
 * arenas stay held through a 2D battle, and its building pages' arena has a
 * gap then (640 KiB there): the keeper is made over a gap of it first, as an
 * atlas page borrows one (CtrVoxel_LendPagesGap) - no VRAM anyone else could
 * have, so it needs no reserve and gives way to no VRAM request. Never over
 * the world or in a voxel battle (the 2D battle alone asks for the keeper, and
 * the voxel battle's gap is its large layers'), never for the 3D battle's slots,
 * which make their own (SlotsPrepare); given back outside the frame as soon as
 * the arena wants the room (CtrVoxel_PagesGapWanted), a voxel battle begins, or
 * before the world parks (CtrVideo_PagesGapRevoke). sSlotLent: sSlotTex is such
 * a gap.
 */
static bool sSlotLent;
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
    /* Asked for by the last frame's composition: denied unless it is there
     * once this has run (the ask is the frame before's, so is the denial). */
    if (wanted && !sBlendLayerTarget && sBattle && !sTransitionRequested
        && sBlendLayerFailFrame && sStats.frames - sBlendLayerFailFrame < LAYER_RETRY_FRAMES)
        SurfaceDenied(CTR_DENIED_BLEND);
    if (sBlendLayerTarget && (!sBattle || sVramAsked || sTransitionRequested
                              || sStats.frames - sBlendLayerUsedFrame > LAYER_IDLE_FRAMES))
    {
        SceneBlendRelease();
        return;
    }
    if (!wanted || sBlendLayerTarget
        || (sBlendLayerFailFrame && sStats.frames - sBlendLayerFailFrame < LAYER_RETRY_FRAMES))
        return;
    /* With room left over, and at an atlas's cost only if that atlas alone
     * makes the 512 KiB block: the oldest atlas out of view given for it
     * left the VRAM in pieces still, in Azahar with 450 KiB free as on
     * hardware (2026-10-28) - the atlas lost for nothing. Back from the
     * voxel field the block was missing with 706 KiB free, and the sandstorm
     * of a 2D battle in 3D composed its scenery again, 15 to 22 ms a frame
     * (port.log 2026-11-01). Without it the steps compose the scenery again;
     * not tried without the block, which only logged a failure. */
    if (vramSpaceFree() >= SCENE_W * SCENE_H * 2u + SLOT_WORLD_RESERVE
        && CtrGpuMem_LargestVram() < SCENE_W * SCENE_H * 2u)
        CtrVoxel_ReleaseAtlasMaking(SCENE_W * SCENE_H * 2u);
    if (vramSpaceFree() >= SCENE_W * SCENE_H * 2u + SLOT_WORLD_RESERVE
        && CtrGpuMem_LargestVram() >= SCENE_W * SCENE_H * 2u
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
        SurfaceDenied(CTR_DENIED_BLEND);
    }
    CtrLog_Write(CTR_LOG_VIDEO, "VIDEO: battle blend layer surface %s (VRAM free=%lu)",
                 sBlendLayerTarget ? "made" : "not made", (unsigned long)vramSpaceFree());
}

/* Outside the frame, beside the scene surface (ScenePrepare). */
static void SlotsRelease(void)
{
    /* The target first: deleting it waits for the GPU, which may still read
     * the texture. A gap of the arena goes back there, not to VRAM. */
    if (sSlotTarget) { ClearWhenMadeForget(sSlotTarget); C3D_RenderTargetDelete(sSlotTarget); }
    if (sSlotLent) CtrVoxel_ReturnPagesGap(sSlotTex.data);
    else if (sSlotTex.data) C3D_TexDelete(&sSlotTex);
    sSlotTarget = NULL;
    sSlotLent = false;
    ++sSlotSurface;
    sSlotWorld = false;
    sSlotMono = false;
    sMonoKeeperHeld = false;
    memset(&sSlotTex, 0, sizeof(sSlotTex));
}

/* The world about to park (CtrVoxel_ParkVram), outside the frame: a keeper
 * or a large layer over its pages' arena goes first. */
void CtrVideo_PagesGapRevoke(void)
{
    for (unsigned bg = 0; bg < 4; ++bg)
        if (sLayerLent[bg])
        {
            LayerRelease(bg);
            CtrLog_Write(CTR_LOG_VIDEO, "VIDEO: battle layer BG%u given back to the building pages' arena "
                         "(world parked)", bg);
        }
    if (!sSlotLent) return;
    SlotsRelease();
    CtrLog_Write(CTR_LOG_VIDEO, "VIDEO: battle scenery keeper given back to the building pages' arena (world parked)");
}

/* Outside the frame: the slot surface given back when only the battle's kept
 * scenery holds it, for a surface that found no room. Before SlotsPrepare
 * the last frame's asks are still up and say who used it last; sSlotMono
 * says it for the frames before. Whether it went. */
static bool SlotsYield(void)
{
    /* Over the pages' arena it holds no VRAM to give. */
    if (!sSlotTarget || sSlotLent || sSlotWanted || !(sSlotMono || sSlotMonoWanted)) return false;
    SlotsRelease();
    sSlotMonoFailFrame = sStats.frames | 1u;
    CtrLog_Write(CTR_LOG_VIDEO, "VIDEO: battle scenery keeper surface given back (VRAM free=%lu)",
                 (unsigned long)vramSpaceFree());
    return true;
}

/* The scenery keeper's surface, just made. */
static void SlotMonoMade(void)
{
    ClearWhenMade(sSlotTarget);
    ++sSlotSurface;
    C3D_TexSetFilter(&sSlotTex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&sSlotTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    sSlotMono = true;
    sSlotMonoFailFrame = 0;
}

/* The slot surface for the kept scenery alone (sSlotMono): from VRAM nobody
 * holds, with SLOT_MONO_RESERVE left over in one block. Not made, the scenery
 * is composed whole as before (counted, CTR_MONO_WHY_KEEPER), and nothing is
 * logged. */
static void SlotsMakeForKeeper(void)
{
    const uint32_t bytes = SCENE_W / 2 * SCENE_H * 2u;
    void *lent;

    /* Signed: the mark is the frame with its low bit set, one ahead of an
     * even frame, and a yield earlier in this same frame waits too. */
    if (sSlotMonoFailFrame && (int32_t)(sStats.frames - sSlotMonoFailFrame) < (int32_t)LAYER_RETRY_FRAMES) return;
    /*
     * A gap of the building pages' arena first (sSlotLent): a texture over
     * it as the arena's own pages are made (VRAM, its piece the whole
     * 512x256 texture), the target made from it as from VRAM's.
     */
    if (!CtrVoxel_InBattle() && (lent = CtrVoxel_LendPagesGap(bytes)) != NULL)
    {
        memset(&sSlotTex, 0, sizeof(sSlotTex));
        sSlotTex.data = lent;
        sSlotTex.fmt = GPU_RGBA5551;
        sSlotTex.size = bytes;
        sSlotTex.width = SCENE_W / 2;
        sSlotTex.height = SCENE_H;
        sSlotTex.param = GPU_TEXTURE_MODE(GPU_TEX_2D);
        sSlotLent = true;
        if (!(sSlotTarget = C3D_RenderTargetCreateFromTex(&sSlotTex, GPU_TEXFACE_2D, 0, -1)))
            SlotsRelease();
        else
        {
            SlotMonoMade();
            CtrLog_Write(CTR_LOG_VIDEO, "VIDEO: battle scenery keeper surface made over the building pages' arena, "
                         "%u texels wide (VRAM free=%lu)", (unsigned)sSlotTex.width, (unsigned long)vramSpaceFree());
            return;
        }
    }
    /*
     * The reserve left in one block, as the texture is placed: VRAM free in
     * pieces was enough before, and the next move's 256 KiB layer
     * (LayerCreate) or the large OBJ window then failed its first allocation
     * - an error, logged with a map of VRAM - before the keeper was given
     * back for it (SlotsYield). Looked at once the texture holds its block,
     * as where it went decides what is left; given back at once otherwise,
     * before a target is made of it. Both looks probe VRAM without asking for
     * it (CtrGpuMem_LargestVram), so the keeper's own allocation never fails
     * for room.
     */
    if (vramSpaceFree() < bytes + SLOT_MONO_RESERVE || CtrGpuMem_LargestVram() < bytes
        || !C3D_TexInitVRAM(&sSlotTex, SCENE_W / 2, SCENE_H, GPU_RGBA5551)
        || CtrGpuMem_LargestVram() < SLOT_MONO_RESERVE
        || !(sSlotTarget = C3D_RenderTargetCreateFromTex(&sSlotTex, GPU_TEXFACE_2D, 0, -1)))
    {
        SlotsRelease();
        sSlotMonoFailFrame = sStats.frames | 1u;
        return;
    }
    SlotMonoMade();
    CtrLog_Write(CTR_LOG_VIDEO, "VIDEO: battle scenery keeper surface made, %u texels wide (VRAM free=%lu)",
                 (unsigned)sSlotTex.width, (unsigned long)vramSpaceFree());
}

static void SlotsPrepare(void)
{
    static uint32_t usedFrame;
    bool wanted = sSlotWanted, mono = sSlotMonoWanted && !wanted;

    sSlotWanted = sSlotMonoWanted = false;
    /* Whoever used it last holds it: the 3D battle's slots, or the kept
     * scenery alone, which gives way to anything (SlotsYield). */
    if (wanted || mono)
    {
        usedFrame = sStats.frames;
        sSlotMono = mono && sSlotTarget;
    }
    /* A keeper over the pages' arena gives way to the arena, to a voxel
     * battle, whose large layers the gap is for, and to the 3D battle's slots,
     * which take VRAM of their own (below); never to VRAM. */
    if (sSlotLent && sSlotTarget && (wanted || CtrVoxel_PagesGapWanted() || CtrVoxel_InBattle()))
    {
        bool arena = !wanted;

        SlotsRelease();
        if (arena) sSlotMonoFailFrame = sStats.frames | 1u;
        CtrLog_Write(CTR_LOG_VIDEO, "VIDEO: battle scenery keeper given back to the building pages' arena (%s)",
                     arena ? "the arena wants it" : "3D battle slots asked for");
    }
    if (!wanted && sSlotTarget
        && (sTransitionRequested || (sVramAsked && !sSlotLent && (!sBattle || sSlotWorld || sSlotMono))
            || (sSlotWorld && !sBattle) || (sSlotMono && !sBattle) || sStats.frames - usedFrame > LAYER_IDLE_FRAMES))
    {
        bool keeper = sSlotMono;

        SlotsRelease();
        /* Given back for the room someone asked for, not made again at once. */
        if (keeper && sVramAsked) sSlotMonoFailFrame = sStats.frames | 1u;
        CtrLog_Write(CTR_LOG_VIDEO, "VIDEO: %s released (VRAM free=%lu)",
                     keeper ? "battle scenery keeper surface" : "3D battle sprite slots",
                     (unsigned long)vramSpaceFree());
    }
    if (mono && !sSlotTarget && sBattle && !sTransitionRequested) SlotsMakeForKeeper();
    if (mono && !sSlotTarget && sBattle && !sTransitionRequested) SurfaceDenied(CTR_DENIED_KEEPER);
    if (wanted && !sSlotTarget && sSlotFailed && sStats.frames - sSlotFailFrame < LAYER_RETRY_FRAMES)
        SurfaceDenied(CTR_DENIED_SLOTS);
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
            SurfaceDenied(CTR_DENIED_SLOTS);
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
            ++sSlotSurface;
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
        SurfaceDenied(CTR_DENIED_SLOTS);
    }
    CtrLog_Write(sSlotTarget ? CTR_LOG_VIDEO : CTR_LOG_ERROR,
                 "VIDEO: 3D battle sprite slots %s, %u texels wide (VRAM free=%lu)",
                 sSlotTarget ? "made" : "not made, sharing the scene surface",
                 sSlotTarget ? (unsigned)sSlotTex.width : 0u, (unsigned long)vramSpaceFree());
}

/* Whether a voxel battle's layers are all walked, as they were. */
static bool BattleLayersBarred(void)
{
#ifdef CTR_AUDIT_BITS
    /* nocache.txt bit 134217728: no loan of the pages' gap (A/B frames). */
    return CtrVoxel_InBattle() && (CtrDumpOff() & 134217728u) != 0;
#else
    return false;
#endif
}

/* Whether the terrain is walked whatever it costs: a voxel battle holds no VRAM,
 * and BG3 there is the world's. */
static bool BattleTerrainBarred(void)
{
#ifdef CTR_AUDIT_BITS
    /* nocache.txt bit 536870912: the terrain walked, as before (A/B frames). */
    if (CtrDumpOff() & 536870912u) return true;
#endif
    return CtrVoxel_InBattle();
}

/* The frame before's layers, into the 600 frames' counts. */
static void BattleLayersCount(void)
{
    for (unsigned bg = 1; bg < 4; ++bg)
    {
        unsigned bit = 1u << bg, why;

        if (sLayerLent[bg]) ++sBattleLayerCounts.lent;
        if (sBattleLayerDrawn & bit)
        {
            ++sBattleLayerCounts.drawn;
            sBattleLayerCounts.terrain += bg == 3;
            continue;
        }
        if (sBattleLayerKept & bit) ++sBattleLayerCounts.kept;
        if (!sBattleWalk[bg] || (sBattleWalk[bg] < BATTLE_LAYER_QUADS && !(sBattleLayerWanted & bit))) continue;
        why = (sBattleLayerPicture & bit) ? BATTLE_LAYER_PICTURE
            : BattleLayersBarred() || (bg == 3 && BattleTerrainBarred()) ? BATTLE_LAYER_NO_SURFACE
            : (sBattleLayerWanted & bit) && !sLayerReady[bg] ? sBattleLayerDenied[bg] : BATTLE_LAYER_NOT_READY;
        ++sBattleLayerCounts.walked;
        ++sBattleLayerCounts.why[why];
    }
}

/* The large layers this frame draws from a texture, from what the last one
 * walked and drew (sBattleLayerWanted). */
static unsigned BattleLayersAsked(void)
{
    unsigned asked = 0, text, boxes;

    if (!sBattle || sTransitionRequested || BattleLayersBarred()) return 0;
#ifdef CTR_AUDIT_BITS
    /* nocache.txt bit 8388608: every layer walked, as before (A/B frames). */
    if (CtrDumpOff() & 8388608u) return 0;
#endif
    text = TextBackgrounds();
    boxes = BattleBoxLayers();
    for (unsigned bg = 1; bg < 4; ++bg)
    {
        unsigned bit = 1u << bg;
        bool walked = sBattleWalk[bg] >= BATTLE_LAYER_QUADS;
        bool held = (sBattleLayerWanted & bit) && sLayers[bg].valid && sCellsShown[bg] >= BATTLE_LAYER_QUADS
                 && (int32_t)(sStats.frames - sBattleLayerUsed[bg]) <= BATTLE_LAYER_HOLD_FRAMES;

        if ((!walked && !held) || !(text & bit) || (boxes & bit) || (sBattleLayerPicture & bit)
            || SceneLayerBlank(bg))
            continue;
        /* On BG3 only the terrain's map, 64 columns wide as battle_bg.c makes it. */
        if (bg == 3 && (!BattleTerrainSplit(bg) || !(Reg(8 + bg * 2) & 0x4000) || BattleTerrainBarred())) continue;
        if (walked) sBattleLayerUsed[bg] = sStats.frames;
        asked |= bit;
    }
    return asked;
}

/* Whether the slots or the blending layer are to make a surface of VRAM in this
 * preparation (their asks are the last frame's). */
static bool BattleSlotsAsking(void)
{
    return sSlotWanted && (!sSlotTarget || sSlotLent)
        && !(sSlotFailed && sStats.frames - sSlotFailFrame < LAYER_RETRY_FRAMES);
}

static bool BattleBlendAsking(void)
{
    return sBlendLayerWanted && !sBlendLayerTarget
        && !(sBlendLayerFailFrame && sStats.frames - sBlendLayerFailFrame < LAYER_RETRY_FRAMES);
}

/* VRAM a large layer holds, given back to a line layer short of it. */
static bool BattleLayersYield(unsigned lines)
{
    bool any = false;

    for (unsigned bg = 1; bg < 4; ++bg)
        if ((sBattleLayerHeld & (1u << bg)) && !sLayerLent[bg] && !(lines & (1u << bg)))
        {
            LayerRelease(bg);
            any = true;
        }
    return any;
}

/*
 * Outside the frame, before this frame's layers are made. A large layer is a
 * speed-up, and walked it is what it was: its texture gives way to what would
 * otherwise go without. A gap of the arena goes back when the arena wants it,
 * out of the battle, or to the keeper once nothing draws from it (the 3D
 * slider down); VRAM to the 3D battle's slots and to the blending layer, whose
 * lack changes how a frame is composed, and to a voxel battle begun with it
 * held, which holds none. Remade over the gap the next frame, from its cells
 * up, if that is what is left (the slider raised in a move).
 */
static void BattleLayersGiveWay(unsigned lines)
{
    bool over = !sBattle || sTransitionRequested, leave = over || CtrVoxel_InBattle();
    uint32_t held = 0, need = SCENE_W * SCENE_H * 2u + SLOT_WORLD_RESERVE;
    bool slots, blend;

    for (unsigned bg = 1; bg < 3; ++bg)
    {
        bool arena = CtrVoxel_PagesGapWanted();

        if (!sLayerLent[bg]) continue;
        if (arena || over || (!(sBattleLayerWanted & (1u << bg)) && sSlotMonoWanted && !sSlotTarget))
        {
            LayerRelease(bg);
            if (arena) sBattleLayerFail[bg] = sStats.frames | 1u;
            CtrLog_Write(CTR_LOG_VIDEO, "VIDEO: battle layer BG%u given back to the building pages' arena (%s)", bg,
                         arena ? "the arena wants it" : over ? "the battle is over" : "for the keeper");
        }
    }
    for (unsigned bg = 1; bg < 4; ++bg)
        if ((sBattleLayerHeld & (1u << bg)) && !sLayerLent[bg] && !(lines & (1u << bg)))
            held += sLayers[bg].tex.size;
    if (!held) return;
    slots = BattleSlotsAsking() && CtrGpuMem_LargestVram() < SCENE_W / 2 * SCENE_H * 2u;
    blend = BattleBlendAsking() && vramSpaceFree() + held >= need
         && (vramSpaceFree() < need || CtrGpuMem_LargestVram() < SCENE_W * SCENE_H * 2u);
    if (!leave && !slots && !blend) return;
    for (unsigned bg = 1; bg < 4; ++bg)
        if ((sBattleLayerHeld & (1u << bg)) && !sLayerLent[bg] && !(lines & (1u << bg)))
        {
            LayerRelease(bg);
            CtrLog_Write(CTR_LOG_VIDEO, "VIDEO: battle layer BG%u gave its VRAM back (%s; VRAM free=%lu)", bg,
                         over ? "the battle is over" : leave ? "a voxel battle holds none"
                         : slots ? "for the 3D battle's slots" : "for the blending layer",
                         (unsigned long)vramSpaceFree());
        }
}

/* A large layer's surface out of VRAM, leaving BATTLE_LAYER_VRAM_LEFT in one block. */
static bool BattleLayerMakeVram(unsigned bg, unsigned width, unsigned height)
{
    const uint32_t bytes = width * height * 2u;

    if (vramSpaceFree() < bytes + BATTLE_LAYER_VRAM_LEFT || CtrGpuMem_LargestVram() < bytes
        || !LayerCreate(bg, width, height))
        return false;
    if (CtrGpuMem_LargestVram() < BATTLE_LAYER_VRAM_LEFT)
    {
        LayerRelease(bg);
        return false;
    }
    sBattleLayerHeld |= 1u << bg;
    CtrLog_Write(CTR_LOG_VIDEO, "VIDEO: battle layer BG%u texture made, %ux%u (VRAM free=%lu)", bg, width, height,
                 (unsigned long)vramSpaceFree());
    return true;
}

/*
 * A large layer's texture and target: from VRAM with BATTLE_LAYER_VRAM_LEFT
 * left over in one block, both looked at without asking for it, as the keeper's
 * (SlotsMakeForKeeper); not while the slots or the blending layer are still to
 * make theirs, nor in a voxel battle, which holds none. Else over a gap of the
 * building pages' arena, as the arena's own pages are made: not the terrain.
 * Else, a move's layer, from the VRAM the terrain holds, which waits for its
 * next try: walked it is as it was, and the wave is most of a frame. Not made,
 * it is walked and tried again a while later: the next frame if what stood in
 * its way is settled by then (the others' surfaces made, a keeper's gap on its
 * way back: SlotsPrepare). How the arena stood is logged for each ask of a layer
 * that can use it: a gap is measured here, in the move, not where the mode's
 * first line left it.
 */
static bool BattleLayerMake(unsigned bg, unsigned width, unsigned height)
{
    LayerTexture *layer = &sLayers[bg];
    const uint32_t bytes = width * height * 2u;
    bool asking = BattleSlotsAsking() || BattleBlendAsking(), world = CtrVoxel_InBattle();
    uint32_t chunkUsed, chunkLargest, pageUsed, gap;
    static unsigned told;
    void *lent;

    CtrVoxel_ArenaUse(&chunkUsed, &chunkLargest, &pageUsed, &gap);
    ++sBattleLayerCounts.asks;
    if (bg != 3)
    {
        sBattleLayerCounts.pagesUsed = pageUsed;
        sBattleLayerCounts.pagesGap = gap;
    }
    if (told < 200 && bg != 3)
    {
        ++told;
        CtrLog_Write(CTR_LOG_VIDEO, "VIDEO: battle layer BG%u asks for %ux%u at f=%lu: building pages used=%luK "
                     "largest gap=%luK (voxel battle %d, VRAM free=%lu)", bg, width, height,
                     (unsigned long)sStats.frames, (unsigned long)(pageUsed >> 10), (unsigned long)(gap >> 10),
                     world, (unsigned long)vramSpaceFree());
    }
    if (!world && !asking && BattleLayerMakeVram(bg, width, height)) return true;
    if (bg != 3 && (lent = CtrVoxel_LendPagesGap(bytes)) != NULL)
    {
        memset(&layer->tex, 0, sizeof(layer->tex));
        layer->tex.data = lent;
        layer->tex.fmt = GPU_RGBA5551;
        layer->tex.size = bytes;
        layer->tex.width = (u16)width;
        layer->tex.height = (u16)height;
        layer->tex.param = GPU_TEXTURE_MODE(GPU_TEX_2D);
        sLayerLent[bg] = true;
        /* Not cleared when made either (LayerCreate). */
        if ((layer->target = C3D_RenderTargetCreateFromTex(&layer->tex, GPU_TEXFACE_2D, 0, -1)) != NULL)
        {
            sBattleLayerHeld |= 1u << bg;
            CtrLog_Write(CTR_LOG_VIDEO, "VIDEO: battle layer BG%u texture made over the building pages' arena, "
                         "%ux%u (VRAM free=%lu)", bg, width, height, (unsigned long)vramSpaceFree());
            return true;
        }
        LayerRelease(bg);
    }
    if (bg != 3 && !world && !asking && (sBattleLayerHeld & 8u) && !(LineBackgrounds() & 8u)
        && vramSpaceFree() + sLayers[3].tex.size >= bytes + BATTLE_LAYER_VRAM_LEFT)
    {
        LayerRelease(3);
        sBattleLayerFail[3] = sStats.frames | 1u;
        CtrLog_Write(CTR_LOG_VIDEO, "VIDEO: battle layer BG3 gave its VRAM back (for BG%u; VRAM free=%lu)", bg,
                     (unsigned long)vramSpaceFree());
        if (BattleLayerMakeVram(bg, width, height)) return true;
    }
    sBattleLayerDenied[bg] = bg != 3 && gap != 0 && gap < bytes ? BATTLE_LAYER_GAP_SMALL : BATTLE_LAYER_NO_SURFACE;
    sBattleLayerFail[bg] = asking || (sSlotLent && sSlotWanted) ? 0 : sStats.frames | 1u;
    return false;
}
