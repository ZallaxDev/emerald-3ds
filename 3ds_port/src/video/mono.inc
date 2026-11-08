/*
 * Part of 3ds_video.c. The battle without the 3D slider:
 * BattleSceneComposeMono and the cuts it makes.
 */

/*
 * The battle without the 3D slider, 2D or over the voxel world. Composed
 * whole, its scenery and its sprites at SCENE_ZOOM every frame, the scenery
 * was walked tile by tile each time - 4-5 ms of an Old 3DS's frame - though
 * most of a battle it stands still. Where the scene surface comes out the
 * same texel for texel, it is made instead:
 *
 * - without the backgrounds that are blank (SceneLayerBlank): one whose map
 *   names only clear tiles draws nothing, and walking it at 2x was most of
 *   the voxel battle's scene, BG1 and BG2 empty between moves and BG3 the
 *   world's;
 * - with no background left, of the sprites alone;
 * - otherwise of the scenery kept in the slot surface (sMonoKeeper*) while
 *   its key and what it is made of hold, as the 3D battle keeps its own:
 *   copied in texel for texel, the columns past the slot surface's width
 *   composed again, and the sprites composed over it. Both parts are the
 *   whole compose cut at that column (SceneComposeOver): its view, its
 *   windows, and what a layer decides from its clip (sClipWhole). Kept
 *   only once the scenery shows what it did the frame before: one that
 *   changes every frame is composed whole, which is less.
 *
 * Composed over the scenery the sprites come out as composed with it only
 * when every background drawn is behind every sprite (Layers draws a
 * priority's backgrounds and then its sprites), and without an OBJ window,
 * which the sprites shape and whose frontmost layer depends on the layers
 * drawn (ObjWindowDraw). Anything else is composed whole as before, counted
 * by reason (sQuality). No surface of its own: the slot surface, asked for in
 * the 2D battle out of free VRAM only and given back to anything short of
 * room (sSlotMono), used over the voxel world only while the 3D battle left
 * it there.
 */
static unsigned MonoSceneBlank(unsigned exclude)
{
    unsigned display = Reg(0), blank = 0, lines;

    if ((display & 7) != 0 || sObjWin.ready) return 0;
    /* A layer drawn line by line counts its runs as drawn, clear or not
     * (DrawLineBg), and the boxes' cover reads that (HudCoverUpdate): left in. */
    lines = LineBackgrounds();
    for (unsigned bg = 1; bg < 4; ++bg)
        if ((display & (0x100u << bg)) && !(exclude & (1u << bg)) && !(lines & (1u << bg)) && SceneLayerBlank(bg))
            blank |= 1u << bg;
    return blank;
}

/* The furthest priority a sprite can be drawn at, -1 with none. */
static int MonoObjFurthest(void)
{
    int furthest = -1;

    if (!(Reg(0) & 0x1000)) return -1;
    if (sObjList) return sObjList->furthest;
    for (unsigned i = 0; i < 128 && furthest < 3; ++i)
    {
        unsigned attr0 = sMemory.oam[i * 4], priority = (sMemory.oam[i * 4 + 2] >> 10) & 3;

        if ((attr0 & 0x300) == 0x200) continue;
        if ((int)priority > furthest) furthest = (int)priority;
    }
    return furthest;
}

/*
 * Whether a drawn layer could come out other than whole when cut: one drawn
 * line by line while magnified as a move's picture (DrawLineBg under
 * BattlePictureBegin). Its runs are quads as wide as the clip, sampled at the
 * picture's scale, not on the scene's quarter texels, so a run cut at the
 * keeper's edge interpolates each texel's place differently and a rounding
 * can pick the texel next to it. The tile walks draw the same quads cut or
 * not, and an unmagnified run samples its layer a quarter texel from any of
 * the layer's texel edges.
 */
static bool MonoCutUnsure(unsigned scenery)
{
    unsigned lines = LineBackgrounds() & scenery;

    for (unsigned bg = 1; bg < 4; ++bg)
        if ((lines & (1u << bg)) && BattlePicture(bg)) return true;
    return false;
}

/* As SceneComposeInto, over what `target` holds already: nothing cleared, and
 * with `cutLeft` only the GBA columns from x0 on, with `cutRight` only those
 * before x1, scissored there. A pass so cut is still the whole width's - its
 * view, its windows - and decides what the whole compose would (sClipWhole). */
static void SceneComposeCut(C3D_RenderTarget *target, int column, int left, int top, int width, int height,
                            unsigned exclude, bool cutLeft, int x0, bool cutRight, int x1)
{
    float zoom = sZoom, offX = sOffX, offY = sOffY;
    bool scissored = sScissored;

    SceneViewEnter(column, left, top, width, height);
    SceneOn(target);
    C2D_ViewReset();
    C2D_Flush();
    sClipWhole.on = cutLeft || cutRight;
    sClipWhole.x0 = sClipX0;
    sClipWhole.x1 = sClipX1;
    if (cutLeft) sClipX0 = x0;
    if (cutRight) sClipX1 = x1;
    if (sClipWhole.on)
    {
        Scissor(sClipX0, sClipY0, sClipX1, sClipY1);
        sScissored = true;
    }
    Blend(5, false, false);
    sLayerExclude = exclude;
    memset(sBgDrawn, 0, sizeof(sBgDrawn));
    if (!(Reg(0) & 128)) Compose();
    sLayerExclude = 0;
    sClipWhole.on = false;
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    sScissored = scissored;

    SceneViewLeave(zoom, offX, offY);
}

/* SceneComposeCut from the surface's first column, by texel columns: with
 * `from` past 0 only those from it on, with `to` past 0 only those before it
 * (both even: a whole GBA pixel). */
static void SceneComposeOver(C3D_RenderTarget *target, int left, int top, int width, int height,
                             unsigned exclude, int from, int to)
{
    SceneComposeCut(target, 0, left, top, width, height, exclude, from > 0, left + from / 2, to > 0,
                    left + to / 2);
}

/*
 * What the scenery's compose beside the voxel world draws outside its own
 * columns. Its view runs from the scene surface's first column (ClipToView
 * over the whole sTargetW, SceneComposeInto), so a layer that wraps - the
 * sandstorm's sand, a 64-column layer - is walked from there too and drawn
 * over the world's picture in the columns before the scenery's, after the
 * world (RenderBattleWorld, then RenderBattleStereo). A frame that keeps the
 * scenery draws that part alone again, over this frame's world, cut from the
 * whole compose (SceneComposeCut): the same tiles and blends where they land,
 * the scenery's columns left as they are. A layer that does not wrap keeps
 * to the GBA picture and draws nothing there. The same layers as the
 * compose's, blank ones too: what a pass decides from the layers it draws
 * (ComposeAlphaBands) is then the compose's. The measures they ask for were
 * taken as the compose's (StillKeep).
 */
static void BattleSceneStrip(void)
{
    int left, top, width, height;
    unsigned drawn[4];

    BattleSceneGeometry(BATTLE_STEREO_MARGIN, &left, &top, &width, &height);
    memcpy(drawn, sBgDrawn, sizeof(drawn));
    SceneComposeCut(sScene, sSceneColumn, left, top, width, height, BattleSceneOut(16u), false, 0, true, left);
    /* Rendered into, then sampled, as after SceneComposeInto. */
    GpuSplit();
    /* What the scenery drew is the kept compose's, not this part's. */
    memcpy(sBgDrawn, drawn, sizeof(sBgDrawn));
}

static bool BattleSceneComposeMono(void)
{
    unsigned base = BattleSceneOut(0), blank, exclude, scenery, front = 0, keep, apart, why;
    unsigned drawn[4];
    int left, top, width, height, kw, furthest;
    uint32_t key;
    SceneKeyParts parts;
    unsigned heldWhy;
    bool held, settled;

    /* The whole-per-eye 3D battle, or a battle's eyes composed apart. */
    if (sEyeParallax != 0.0f || sParallax != 0.0f) return false;
    why = CTR_MONO_WHY_COUNT;
#ifdef CTR_AUDIT_BITS
    if (CtrDumpNoCache()) why = CTR_MONO_WHY_OTHER;
#endif
    if (sTransition || sRegLine || (Reg(0) & 0x87)) why = CTR_MONO_WHY_OTHER;
    else if (sObjWin.ready) why = CTR_MONO_WHY_OBJWIN;
    if (why != CTR_MONO_WHY_COUNT)
    {
        ++sQuality.monoFallback[why];
        return false;
    }
    /* Both callers place the scene from the surface's first column; one left
     * past the voxel world's picture by a 3D battle is not this frame's. */
    sSceneColumn = 0;
    sSceneBlendLayer = -1;
    sSceneKeptNow = false;
    sSceneCached = false;
    sSceneEmpty = false;
    BattleSceneGeometry(0, &left, &top, &width, &height);
    blank = MonoSceneBlank(base);
    exclude = base | blank;
    sQuality.monoBlankLayers += (unsigned)__builtin_popcount(blank);
    scenery = (Reg(0) >> 8) & 14u & ~exclude;
    if (!scenery)
    {
        SceneComposeInto(sScene, 0, left, top, width, height, exclude);
        ++sQuality.monoEmpty;
        return true;
    }
    /*
     * The cut by priority. Layers draws each priority's backgrounds before
     * its sprites, as the GBA puts a background behind a sprite of the same
     * priority, so the layers at the furthest sprite's priority or further
     * back are behind every sprite: those are kept. The ones nearer than it
     * (a move's background over its target, the intro's entry picture over
     * the Pokemon sliding in) are composed with the sprites every frame, over
     * the kept part, in the same order the whole compose draws them - and
     * what they blend with is what that part holds, texel for texel. Nothing
     * behind every sprite: there is nothing to keep (front-tie).
     */
    furthest = MonoObjFurthest();
    for (unsigned bg = 1; bg < 4; ++bg)
        if ((scenery & (1u << bg)) && (int)(Reg(8 + bg * 2) & 3) < furthest) front |= 1u << bg;
    keep = scenery & ~front;
    /* The 2D battle asks for the slot surface for it, out of free VRAM or
     * the building pages' arena, given back to anything that needs the room
     * (sSlotMono, sSlotLent); over the voxel world it is the 3D battle's
     * alone to ask for (SlotsPrepare). */
    if (!sBattleWorld && !CtrVoxel_InBattle()) sSlotMonoWanted = true;
    kw = sSlotTarget ? (int)sSlotTex.width : 0;
    why = !keep ? CTR_MONO_WHY_FRONT
        : !sSlotTarget || sSlotTex.height != SCENE_H || (kw & 1) ? CTR_MONO_WHY_KEEPER
        : width > kw && MonoCutUnsure(keep) ? CTR_MONO_WHY_OTHER : CTR_MONO_WHY_COUNT;
    if (why != CTR_MONO_WHY_COUNT)
    {
        /* Whole, as before, but for the blank layers. */
        SceneComposeInto(sScene, 0, left, top, width, height, exclude);
        ++sQuality.monoFallback[why];
        sSceneWhy |= SCENE_WHY(PATH) | SCENE_WHY(COMPOSED);
        return true;
    }
    /* What the kept part leaves out: the sprites and the layers in front. */
    apart = exclude | 16 | front;
    /*
     * The key by what the kept part reads. BLDALPHA, its table by line and
     * BLDY reach a background only as a first target (Blend, the tiles'
     * fades), and the bands of ComposeAlphaBands change no texel of a layer
     * they do not reach. The sprites read BLDALPHA whatever BLDCNT names, but
     * they are apart here, drawn every frame over the kept part with the
     * layers in front. So with nothing this compose can draw a first target -
     * not the kept layers, a disabled one (ComposeAlphaBands asks about those
     * too) or the backdrop - a move's blend stepping in front of the scenery,
     * Surf's wave line by line each frame, the sand every four, is no change
     * of the scenery, which went whole for it (unsettled, below). BLDCNT and
     * sLineAlphaOn stay in the key, and the mask is made of terms in it.
     */
    {
        bool weightless = !(Reg(0x50) & 63u & ~apart);

#ifdef CTR_AUDIT_BITS
        /* nocache.txt bit 67108864: the whole key, as before (A/B frames). */
        if (CtrDumpOff() & 67108864u) weightless = false;
#endif
        sSceneKeyNoAlpha = sSceneKeyNoBright = weightless;
        key = SceneCacheKey(left, top, width, height, apart) ^ 0x68e31da4u;
        sSceneKeyNoAlpha = sSceneKeyNoBright = false;
        if (weightless)
        {
            uint32_t weights = Reg(0x52) | Reg(0x54) << 16;

            ++sMonoWeights.out;
            sMonoWeights.moved += weights != sMonoWeights.weights || sLineAlphaVersion != sMonoWeights.lines;
            sMonoWeights.weights = weights;
            sMonoWeights.lines = sLineAlphaVersion;
        }
    }
    parts = sSceneKeyLast;
    held = sMonoKeeperHeld && key == sMonoKeeperKey && SceneKeptSince(apart, sMonoKeeperToken)
        && StillHolds(sMonoKeeperStill);
    heldWhy = sHoldWhy;
    /*
     * Kept, or composed apart to be kept, only once the scenery holds: the
     * frame before looked at the same key, and nothing it is made of changed
     * since. A scenery changing frame after frame - a move's background
     * scrolling, a monbg copy taking its Pokemon's faded colours behind it -
     * was composed apart every frame, then copied in with the strip past it
     * composed again: more than the whole compose it was to save. Composed
     * whole then, as before. The keeper stays: nothing writes its surface
     * meanwhile, and a scenery back to it after a blip (a scroll step and
     * back) is kept again at once, by the same key and token check.
     */
    settled = held || (sMonoLastToken != 0 && sMonoLastToken == sStats.frames && key == sMonoLastKey
                       && SceneKeptSince(apart, sMonoLastToken));
    /* A scenery the frame before looked at, composed again for its key. */
    if (!held && sMonoLastToken != 0 && sMonoLastToken == sStats.frames && key != sMonoLastKey)
        SceneKeyWhy(&sMonoLastParts, &parts);
    else if (!held)
        SceneHeldWhy(sMonoKeeperHeld && key == sMonoKeeperKey, heldWhy);
    sSceneWhy |= held ? SCENE_WHY(KEPT) : SCENE_WHY(COMPOSED);
    sMonoLastKey = key;
    sMonoLastParts = parts;
    sMonoLastToken = sStats.frames + 1;
    if (!settled)
    {
        SceneComposeInto(sScene, 0, left, top, width, height, exclude);
        /* The characters the next frame's look checks the maps by
         * (SceneHoldsSince): those the maps name now. */
        SceneEmptyChars(apart);
        ++sQuality.monoFallback[CTR_MONO_WHY_UNSETTLED];
        return true;
    }
    if (held)
    {
        StillKeep(sMonoKeeperStill);
        ++sQuality.monoKept;
    }
    else
    {
        /* At the scene's own column 0, so its texels are the scene's: the
         * whole scene's compose, cut at the slot surface's width. Cleared
         * first, as SceneComposeInto clears a surface it composes from 0. */
        C2D_TargetClear(sSlotTarget, 0);
        ++sSlotSurface;
        SceneComposeOver(sSlotTarget, left, top, width, height, apart, 0, width > kw ? kw : 0);
        sMonoKeeperStill = StillTake();
        /* Rendered into, then sampled below, as after SceneComposeInto. */
        GpuSplit();
        SceneCacheSync(apart);
        memcpy(sMonoKeeperDrawn, sBgDrawn, sizeof(sBgDrawn));
        sMonoKeeperKey = key;
        sMonoKeeperToken = sStats.frames + 1;
        /* Not kept when its compose went past the geometry budget and
         * dropped quads (as SlotsCompose, LayerRenderCells). */
        sMonoKeeperHeld = sStats.tiles < DRAW_BUDGET;
        SceneEmptyChars(apart);
        ++sQuality.monoRecomposed;
    }
    {
        /*
         * The scene surface cleared whole first, as a memory fill, as the
         * whole compose clears it (SceneComposeInto): past the kept part, to
         * the last texel the bilinear placement reaches and beyond, nothing
         * of a frame before is left. Not a transparent rectangle drawn there:
         * drawn clears over a surface's columns did not always land on
         * hardware (RenderBattleStereo, RenderBattleWorld), which Azahar does
         * not show. Queued behind everything recorded so far and before what
         * follows (C2D_TargetClear splits first), as in that compose; nothing
         * this frame sampled the surface before. Then the kept part whole, its
         * rows past the scene's too (they are the scene's own: cleared, or a
         * tile's rest past the clip), unfiltered, texel for texel, alpha and all.
         */
        const Tex3DS_SubTexture all = {(u16)kw, (u16)SCENE_H, 0.0f, 1.0f, kw / (float)sSlotTex.width,
                                       1.0f - SCENE_H / (float)sSlotTex.height};

        C2D_TargetClear(sScene, 0);
        sSceneFilled = true;
        BlendForget();
        SceneOn(sScene);
        C2D_ViewReset();
        C2D_Flush();
        C3D_TexSetFilter(&sSlotTex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexBind(0, &sSlotTex);
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        C2D_DrawImageAt((C2D_Image){&sSlotTex, &all}, 0, 0, 0, NULL, 1, 1);
        C2D_Flush();
        C3D_TexSetFilter(&sSlotTex, GPU_LINEAR, GPU_LINEAR);
        C3D_TexBind(0, &sSlotTex);
        BlendForget();
    }
    memcpy(drawn, sMonoKeeperDrawn, sizeof(drawn));
    if (width > kw)
    {
        SceneComposeOver(sScene, left, top, width, height, apart, kw, 0);
        for (unsigned bg = 0; bg < 4; ++bg)
            drawn[bg] += sBgDrawn[bg];
    }
    /* The sprites, every frame, over it, and the layers in front of them. */
    SceneComposeOver(sScene, left, top, width, height, exclude | (15u & ~front), 0, 0);
    GpuSplit();
    for (unsigned bg = 1; bg < 4; ++bg)
        if (front & (1u << bg)) drawn[bg] += sBgDrawn[bg];
    memcpy(sBgDrawn, drawn, sizeof(sBgDrawn));
    if (held) BattleLayersKept();
    if (front) ++sQuality.monoFrontLayers;
    return true;
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
