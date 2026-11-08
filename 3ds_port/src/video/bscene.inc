/*
 * Part of 3ds_video.c. The battle's scenery: BattleSceneCompose and its
 * blending layer apart, its place on the screen (BattleScenePlace), the
 * wireless indicator, the kept edges and RenderBattleScene.
 */

/*
 * The battle scene - everything but the text box - at SCENE_ZOOM into its own
 * surface, then that surface on the logical one at CTR_BATTLE_ZOOM. The scene
 * surface starts at the GBA pixel on the screen's top-left corner, so its
 * origin lands on screen (ox, oy), at most a screen pixel off the edge.
 */

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

#ifdef CTR_AUDIT_BITS
    if (CtrDumpOff() & 32) return -1;
    /* Audit builds: nocache.txt bit 1024, no layer apart with BLDALPHA by line. */
    if ((CtrDumpOff() & 1024) && sLineAlphaOn) return -1;
#endif
    /* With BLDALPHA line by line too: blended in bands of lines when placed
     * (BattleScenePlace), with the weights ComposeAlphaBands would give them;
     * not with a scanline register, whose lines it does not band. */
    if (!sBattle || sBattleWorld || !(layers & 16) || ((control >> 6) & 3) != 1 || (display & 7)
        || sObjWin.ready || sObjWinSlotted || sPaletteFade > 0.0f || sRegLine)
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
    SceneKeyParts restParts, aloneParts;
    unsigned drawn[4], restWhy, aloneWhy;
    bool restHeld, aloneHeld;

    sBlendLayerWanted = true;
    sBlendLayerUsedFrame = sStats.frames;
    if (!sBlendLayerTarget)
    {
        ++sBlendLayerNoSurface;
        return false;
    }
    sSceneKeyNoAlpha = true;
    restKey = SceneCacheKey(left, top, width, height, rest) ^ 0x1b873593u;
    restParts = sSceneKeyLast;
    aloneKey = SceneCacheKey(left, top, width, height, alone) ^ 0x2545f491u;
    aloneParts = sSceneKeyLast;
    sSceneKeyNoAlpha = false;
    restHeld = sSceneCached && restKey == sSceneCacheKey && SceneCacheHolds(rest) && StillHolds(sSceneCacheStill);
    restWhy = sHoldWhy;
    aloneHeld = sBlendLayerCached && aloneKey == sBlendLayerKey && SceneKeptSince(alone, sBlendLayerToken)
             && StillHolds(sBlendLayerStill);
    aloneWhy = sHoldWhy;
    if (restHeld)
    {
        StillKeep(sSceneCacheStill);
        ++sSceneReused;
        sSceneWhy |= SCENE_WHY(KEPT);
    }
    else
    {
        unsigned layerDrawn = sSceneCacheDrawn[blend];

        if (sSceneCached && restKey != sSceneCacheKey) SceneKeyWhy(&sSceneCacheParts, &restParts);
        else SceneHeldWhy(sSceneCached, restWhy);
        sSceneWhy |= SCENE_WHY(COMPOSED);

        SceneComposeInto(sScene, sSceneColumn, left, top, width, height, rest);
        sSceneCacheStill = StillTake();
        SceneCacheSync(rest);
        memcpy(sSceneCacheDrawn, sBgDrawn, sizeof(sBgDrawn));
        sSceneCacheDrawn[blend] = layerDrawn;
        sSceneCacheKey = restKey;
        sSceneCacheParts = restParts;
        sSceneCacheToken = sStats.frames + 1;
        sSceneCached = true;
        sSceneEmpty = false;
    }
    if (aloneHeld)
        StillKeep(sBlendLayerStill);
    else
    {
        if (sBlendLayerCached && aloneKey != sBlendLayerKey) SceneKeyParted(&sBlendLayerParts, &aloneParts);
        else SceneHeldWhy(sBlendLayerCached, aloneWhy);
        sSceneWhy |= SCENE_WHY(LAYER);
        SceneComposeInto(sBlendLayerTarget, 0, left, top, width, height, alone);
        sBlendLayerStill = StillTake();
        SceneCacheSync(alone);
        sSceneCacheDrawn[blend] = sBgDrawn[blend];
        sBlendLayerCached = true;
        sBlendLayerKey = aloneKey;
        sBlendLayerParts = aloneParts;
        sBlendLayerToken = sStats.frames + 1;
    }
    memcpy(drawn, sSceneCacheDrawn, sizeof(drawn));
    memcpy(sBgDrawn, drawn, sizeof(sBgDrawn));
    if (restHeld || aloneHeld) BattleLayersKept();
    if (!restHeld || !aloneHeld) SceneEmptyChars(layers);
    /* Kept whole only with the weights as they were, by line too: the edges
     * have the layer blended in them (BattleEdgesKeep). */
    sSceneKeptNow = restHeld && aloneHeld && sBlendLayerAlpha == Reg(0x52) && sBlendLayerLines == sLineAlphaVersion;
    sBlendLayerAlpha = Reg(0x52);
    sBlendLayerLines = sLineAlphaVersion;
    sSceneBlendLayer = (int)blend;
    if (sLineAlphaOn) ++sLineAlphaApart;
    return true;
}

/* Where the battle scene's surface goes on the logical one (sScenePut) and
 * what of the scene it holds: GBA (left, top) at its top-left texel,
 * width x height texels of it. */
static void BattleSceneGeometry(int margin, int *leftOut, int *topOut, int *widthOut, int *heightOut)
{
    float zoom = sZoom, offX = sOffX, offY = sOffY;
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
    sScenePut.top = top;
    sScenePut.width = width;
    sScenePut.height = height;
    *leftOut = left;
    *topOut = top;
    *widthOut = width;
    *heightOut = height;
}

/* What the scenery's compose leaves out: the text layer, the world's layers,
 * the boxes and `exclude`. The key, the strip and the compose must name the
 * same layers (BattleSceneStrip). */
static unsigned BattleSceneOut(unsigned exclude)
{
    return 1u | sWorldLayers | exclude | BattleBoxLayers();
}

static void BattleSceneCompose(int margin, unsigned exclude)
{
    int left, top, width, height;

    sSceneBlendLayer = -1;
    BattleSceneGeometry(margin, &left, &top, &width, &height);
#ifdef CTR_AUDIT_BITS
    if (CtrDumpNoCache()) sSceneCacheAllowed = false;
#endif
    if (sSceneCacheAllowed)
    {
        unsigned layers = BattleSceneOut(exclude);
        /* What the key and the checks look at; the compose keeps to `layers`. */
        unsigned seen = layers | SceneWindowHidden();
        uint32_t key = SceneViewKey(left, top, width, height, seen);
        bool empty;
        int blend = SceneBlendLayerOf(layers);

        SceneKeyParts parts = sSceneKeyLast;
        unsigned heldWhy;

        if (blend >= 0 && BattleSceneComposeApart(left, top, width, height, layers, (unsigned)blend))
            return;
        if (sLineAlphaOn) ++sLineAlphaWhole;
        if (sSceneCached && key == sSceneCacheKey && SceneCacheHolds(seen) && StillHolds(sSceneCacheStill))
        {
            StillKeep(sSceneCacheStill);
            memcpy(sBgDrawn, sSceneCacheDrawn, sizeof(sBgDrawn));
            BattleLayersKept();
            /* Over the world, counted once the frame's slots leave it be
             * (RenderBattleStereo). */
            if (!sSceneFillOwed) ++sSceneReused;
            sSceneKeptNow = true;
            sSceneWhy |= SCENE_WHY(KEPT);
            return;
        }
        heldWhy = sHoldWhy;
        /* The surface left unfilled over the world for a scenery that does
         * not hold after all: filled before anything is composed into it. */
        if (sSceneFillOwed)
        {
            ++sWorldScene.late;
            SceneFillPay();
        }
        empty = sBattleWorld && sSceneEmpty && key == sSceneEmptyKey && SceneHoldsSince(seen, sSceneEmptyToken)
             && StillHolds(sSceneEmptyStill);
        if (empty)
        {
            StillKeep(sSceneEmptyStill);
            memset(sBgDrawn, 0, sizeof(sBgDrawn));
            ++sSceneEmptyReused;
            sSceneWhy |= SCENE_WHY(KEPT);
            return;
        }
        if (sSceneCached && key != sSceneCacheKey) SceneKeyWhy(&sSceneCacheParts, &parts);
        else SceneHeldWhy(sSceneCached, heldWhy);
        sSceneWhy |= SCENE_WHY(COMPOSED);
        SceneComposeInto(sScene, sSceneColumn, left, top, width, height, layers);
        sSceneCacheStill = StillTake();
        SceneCacheSync(seen);
        memcpy(sSceneCacheDrawn, sBgDrawn, sizeof(sBgDrawn));
        sSceneCacheKey = key;
        sSceneCacheParts = parts;
        sSceneCacheToken = sStats.frames + 1;
        sSceneCached = true;
        sSceneEmpty = !sBgDrawn[1] && !sBgDrawn[2] && !sBgDrawn[3];
        sSceneEmptyKey = key;
        sSceneEmptyToken = sSceneCacheToken;
        sSceneEmptyStill = sSceneCacheStill;
        /* The characters both the kept and the empty scene are checked by. */
        SceneEmptyChars(seen);
        return;
    }
    if (sSceneFillOwed)
    {
        ++sWorldScene.late;
        SceneFillPay();
    }
    sSceneCached = false;
    sSceneWhy |= SCENE_WHY(PATH) | SCENE_WHY(COMPOSED);
    SceneComposeInto(sScene, sSceneColumn, left, top, width, height, BattleSceneOut(exclude));
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
    TexCacheDrop();
}

static void SceneOn(C3D_RenderTarget *target)
{
    if (target != sSceneOn)
    {
        ++sGpuCounts.switches;
        C2D_Flush();
        GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1);
        GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_INVALIDATE, 1);
        /* What was drawn into the last target is sampled from now on. */
        TexCacheDrop();
        sSceneOn = target;
    }
    C2D_SceneBegin(target);
}

static void SplitFrame(void)
{
    C3D_FrameSplit(0);
    sSceneOn = NULL;
    TexCacheDrop();
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
        const float u1 = width / (float)sBlendLayerTex.width, texH = sBlendLayerTex.height;

        /* As the layer's tiles are drawn over the rest (Layers). */
        BlendForget();
        if (!sLineAlphaOn || sRegLine)
        {
            const Tex3DS_SubTexture cut = {(u16)width, (u16)height, 0, 1, u1, 1 - height / texH};

            Blend((unsigned)sSceneBlendLayer, true, false);
            C2D_DrawImageAt((C2D_Image){&sBlendLayerTex, &cut}, ox, oy, 0, &sTint, scale, scale);
        }
        else
        {
            /*
             * In bands of the lines whose weights are the same, each with its
             * own: texel row r of the scene is GBA line top + r / SCENE_ZOOM
             * less the view's offset, as the scene's compose places a line
             * (Scissor) - so a band's rows are the ones ComposeAlphaBands
             * would compose with those weights, the lines above and below the
             * picture with its first line's and its last's. Each band is its
             * own quad over exactly its rows, its edges where the scene's
             * texel rows change line.
             */
            for (int r0 = 0, r1; r0 < height; r0 = r1)
            {
                int y = (int)(r0 / SCENE_ZOOM) + sScenePut.top - CTR_VIEW_Y;
                unsigned value = sLineAlpha[y < 0 ? 0 : y >= CTR_GBA_LINES ? CTR_GBA_LINES - 1 : y];

                for (r1 = r0 + 1; r1 < height; ++r1)
                {
                    int next = (int)(r1 / SCENE_ZOOM) + sScenePut.top - CTR_VIEW_Y;

                    if (sLineAlpha[next < 0 ? 0 : next >= CTR_GBA_LINES ? CTR_GBA_LINES - 1 : next] != value) break;
                }
                {
                    const Tex3DS_SubTexture band = {(u16)width, (u16)(r1 - r0), 0, 1 - r0 / texH, u1, 1 - r1 / texH};

                    /* Blend keeps its state while its key holds: not the weights. */
                    BlendForget();
                    sLineAlphaBand = (int)value;
                    Blend((unsigned)sSceneBlendLayer, true, false);
                    C2D_DrawImageAt((C2D_Image){&sBlendLayerTex, &band}, ox, oy + r0 * scale, 0, &sTint, scale, scale);
                    C2D_Flush();
                    sLineAlphaBand = -1;
                }
            }
            sLineAlphaBanded = true;
        }
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
static unsigned MonoSceneBlank(unsigned exclude);

static void BattleEdgesCompose(bool sprites, int widen, int lift)
{
    float offX = sOffX, offY = sOffY;
    int targetW = sTargetW, left, right, top, bottom;
    bool box = BattleSceneBottom() == BATTLE_SCENE_BOX_BOTTOM;

    if (Reg(0) & 128) return;
    /* The blank layers measured, not walked (sLayerMeasureOnly): every
     * caller, mono or with the 3D slider, over the world or not. Not with
     * nocache.txt bit 1, as the mono scene's own blank layers. */
    sLayerMeasureOnly = MonoSceneBlank(1u | BattleBoxLayers() | (sBattleWorld ? sWorldLayers : 0u));
#ifdef CTR_AUDIT_BITS
    if (CtrDumpNoCache()) sLayerMeasureOnly = 0;
#endif
    sQuality.edgeBlankLayers += (unsigned)__builtin_popcount(sLayerMeasureOnly);
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
    sLayerMeasureOnly = 0;
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
/* still: the layers the edges' compose asked MeasureStill about - lines the
 * scene does not have, so not in its record (StillHolds). */
static struct { bool on, held, bare; int topRows, storeY, rows, from; uint32_t frame; StillRecord still; } sEdgeStore;
static unsigned sEdgesKept;

/*
 * The edges stored last frame still there and still right: made of the
 * backgrounds alone (with sprites to slot, the lines above leave them out
 * too), as the scene kept this frame is (sSceneKeptNow), placed alike, and
 * the logical surface's rows above the text box not drawn on since - the
 * frame before took this same path (sEdgeStore.frame). The text box's clear
 * then leaves those rows (BattleTextCompose). Never over the voxel world,
 * which is drawn into the whole logical surface first every frame
 * (RenderBattleWorld), its scenery kept or not.
 */
static bool BattleEdgesKeep(bool sprites)
{
    bool box = BattleSceneBottom() == BATTLE_SCENE_BOX_BOTTOM;
    int topRows = (int)ceilf(sScenePut.oy);
    int from = (int)floorf((BATTLE_SCENE_BOX_BOTTOM + sViewY) * sZoom + sOffY);

    return !sprites && !sBattleWorld && sSceneKeptNow && sEdgeStore.held && sEdgeStore.bare && sEdgeStore.frame + 1 == sStats.frames
        && sEdgeStore.topRows == topRows && sEdgeStore.from == from && (sEdgeStore.rows > 0) == box
        && StillHolds(sEdgeStore.still);
}

static void BattleEdgesStore(bool sprites, bool keep)
{
    bool box = BattleSceneBottom() == BATTLE_SCENE_BOX_BOTTOM;
    int topRows = (int)ceilf(sScenePut.oy), storeY = topRows + 2;
    int from = (int)floorf((BATTLE_SCENE_BOX_BOTTOM + sViewY) * sZoom + sOffY), rows = CTR_GAME_HEIGHT - from;

    sEdgeStore.on = false;
    if (keep)
    {
        StillKeep(sEdgeStore.still);
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
    sStillAsked = 0;
    BattleEdgesCompose(sprites, BATTLE_EDGE_MARGIN, box ? from - storeY : 0);
    sEdgeStore.still = StillTake();
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

static bool BattleSceneComposeMono(void);

/*
 * The scene into the logical surface. Without the 3D slider its scenery may
 * be kept apart from its sprites (BattleSceneComposeMono). With the slider
 * up, in the whole-per-eye case, it is composed for this eye: the scenery
 * displaced behind the screen, each sprite at its depth, and placed
 * undisplaced.
 */
static void RenderBattleScene(void)
{
    float origin = sLayerOrigin;

    sLayerOrigin = -sEyeParallax * BATTLE_BACK_DEPTH;
    sSpriteDepth = sEyeParallax != 0.0f;
    {
        PORT_PROF_BEGIN(bmScene);
        if (!BattleSceneComposeMono())
            BattleSceneCompose(sEyeParallax != 0.0f ? BATTLE_STEREO_MARGIN : 0, 0);
        PORT_PROF_END(bmScene, PORT_PROF_BM_SCENE);
    }
    sSpriteDepth = false;
    sLayerOrigin = origin;
    PORT_PROF_BEGIN(bmPlace);
    HudCoverUpdate();
    BattleScenePlace(sLogical, 0.0f);
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
    PORT_PROF_END(bmPlace, PORT_PROF_BM_PLACE);
}
