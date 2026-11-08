/*
 * Part of 3ds_video.c. The scenery straight to the eyes
 * (BattleSceneryStraight), RenderBattleStereo and its pieces, with
 * StereoPlan, what they hand on.
 */

/* The stereo battle's scene composed whole, its sprites at the scenery's depth. */
static bool sHudFlatScene;
/* The last 2D stereo frame was composed whole (RenderBattleStereo). */
static bool sFlatBefore;
/* How the eyes of the 2D battle were cleared, per 600 frames (RenderBattleStereo). */
static unsigned sEyesFilled, sEyesDrawn;
/* The stereo frames with the OBJ window on, per 600 frames: their scenery kept
 * or empty and not composed; composed, whole frames among them; and the layers
 * left out of the key, frame by frame (SceneWindowHidden). */
static struct { unsigned kept, composed, hidden; } sObjWinScene;

/*
 * Over the voxel world a move's large layer changes every frame (Surf's wave
 * scrolls, Haze's fog drifts), so its scenery was composed every frame: the
 * surface's columns cleared by drawing them, the layer drawn at twice the size,
 * the lines past the surface composed apart, and all of it placed in each eye -
 * about 300,000 pixels and a cache flush more than the frames beside it, on a
 * GPU that does not catch up in those frames. With such a layer and nothing else
 * in the scenery it is drawn from its texture straight onto each eye instead,
 * over the world's picture, with the bands of BLDALPHA, the windows' masks and
 * the priority walk of the same Compose. What the scenery asks for that this
 * cannot give it stays composed, counted by why (sStraightWhy):
 *
 * - KEPT: the frame keeps the scenery (BattleWorldSceneHeld), nothing to save;
 * - PATH: a bitmap mode, the OBJ window, a scanline register, a line layer, a
 *   palette fade (its tint is rounded to the surface's 5 bits), or the text box
 *   blending with the scene (the eyes are put together elsewhere);
 * - WINDOW: window 0 or 1 open, or the outside of them without the layer;
 * - EFFECT: BLDY on the layer (its tint, as the fade's);
 * - LAYERS: more than one layer shown: the second target of a blend is what
 *   the scenery has under it, and an eye has the world there;
 * - READY: walked this frame, or a move's picture, which is sampled apart;
 * - STALE: a cell of the view not drawn (MapPlanFresh), or a plan that does not
 *   fit;
 * - STILL: the layer stands still (the scenery's key the same for
 *   SCENERY_STILL_FRAMES frames): composed once and kept from then on;
 * - SPRITES: no sprite to slot: the lines above the scene take the sprites
 *   there (BattleEdgesCompose), which this does not draw;
 * - WHOLE: composed whole per eye all the same (RenderBattleStereo);
 * - OFF: nocache.txt bit 1073741824 or bit 1: composed, as before (A/B frames).
 *
 * The picture is the composed one's. A layer that wraps goes on being drawn
 * over the world's picture as that compose drew it, at twice its size from the
 * surface's first column (BattleSceneStrip): below half of the blend that is all
 * that shows of it over the world, Haze's fog among them. Only a layer blended
 * from half on keeps 8 bits a colour here where the surface, on hardware, has 5
 * (Azahar's holds 8: its pictures cannot show that rounding).
 *
 * No key of its own: nothing is kept. The texture's own key (LayerRenderCells)
 * and the frame's registers are what is drawn from; the scene surface is filled
 * as on any frame not keeping the scenery, and holds no scenery after it.
 */
enum
{
    STRAIGHT_KEPT, STRAIGHT_PATH, STRAIGHT_WINDOW, STRAIGHT_EFFECT, STRAIGHT_LAYERS, STRAIGHT_READY,
    STRAIGHT_STALE, STRAIGHT_STILL, STRAIGHT_SPRITES, STRAIGHT_WHOLE, STRAIGHT_OFF, STRAIGHT_WHY_COUNT
};
#define SCENERY_STILL_FRAMES 8
static unsigned sStraightFrames, sStraightWhy[STRAIGHT_WHY_COUNT];
/* The scenery's key over the frames that looked at it, and how long it stood. */
static uint32_t sStraightKey;
static unsigned sStraightStill;
/* The frame's layer wraps: it has a part over the world's picture. */
static bool sStraightStrip;

/* Any frame the scenery's way was decided on since the counters' last line. */
static bool StraightCounted(void)
{
    unsigned counted = sStraightFrames;

    for (unsigned why = 0; why < STRAIGHT_WHY_COUNT; ++why) counted += sStraightWhy[why];
    return counted != 0;
}

/* The view of the scenery on an eye, past the sides as the edges' is. */
static void StraightViewWiden(int sign)
{
    sOffX += (float)(sign * BATTLE_EDGE_MARGIN);
    sTargetW += sign * 2 * BATTLE_EDGE_MARGIN;
}

/* The last line of the eyes' scenery: below the surface the box's round. */
static int StraightBottom(void)
{
    return BattleSceneBottom() == BATTLE_SCENE_BOX_BOTTOM ? VIEW_BOTTOM : BATTLE_SCENE_BOTTOM;
}

/*
 * Whether this frame's scenery goes straight onto the eyes; asked in place of
 * its compose, and with the compose's measures of the layer taken, what is drawn
 * set as the compose sets it (sBgDrawn) and sScenePut made. Blank layers are no
 * part of it, and a frame with nothing else is not counted: the empty scenery
 * has its own keeping.
 */
static bool BattleSceneryStraight(bool textOnce)
{
    unsigned display = Reg(0), control = Reg(0x50), bg = 1;
    unsigned layers = BattleSceneOut(16u);
    unsigned shown = (display >> 8) & 14u & ~layers & ~MonoSceneBlank(layers);
    int left, top, width, height;
    uint32_t key;
    MapPlan plan;
    bool wraps;

    BattleSceneGeometry(BATTLE_STEREO_MARGIN, &left, &top, &width, &height);
    if (!shown || (display & 128)) return false;
#ifdef CTR_AUDIT_BITS
    if (CtrDumpOff() & (1073741824u | 1u))
    {
        ++sStraightWhy[STRAIGHT_OFF];
        return false;
    }
#endif
    if (sSceneFillOwed)
    {
        ++sStraightWhy[STRAIGHT_KEPT];
        return false;
    }
    if ((display & 7) || sObjWin.ready || sRegLine || (LineBackgrounds() & shown) || sPaletteFade > 0.0f || !textOnce)
    {
        ++sStraightWhy[STRAIGHT_PATH];
        return false;
    }
    if (shown & (shown - 1))
    {
        ++sStraightWhy[STRAIGHT_LAYERS];
        return false;
    }
    while (!(shown & (1u << bg))) ++bg;
    for (unsigned w = 0; w < 2; ++w)
        if (display & (0x2000u << w))
        {
            int x0, x1, y0, y1;

            WindowRect(w, &x0, &x1, &y0, &y1);
            if (x0 < x1 && y0 < y1)
            {
                ++sStraightWhy[STRAIGHT_WINDOW];
                return false;
            }
        }
    if ((display & 0xE000) && !(Reg(0x4a) & (1u << bg)))
    {
        ++sStraightWhy[STRAIGHT_WINDOW];
        return false;
    }
    if (((control >> 6) & 3) >= 2 && (control & (1u << bg)))
    {
        ++sStraightWhy[STRAIGHT_EFFECT];
        return false;
    }
    if (!BattleLayerReady(bg) || BattlePicture(bg) || BattleTerrainSplit(bg))
    {
        ++sStraightWhy[STRAIGHT_READY];
        return false;
    }
    /* As DrawBattleBgTex measures and decides, once for all the frame's draws:
     * each of them keeps to rows of this plan. */
    wraps = LayerWrapsMeasured(bg);
    StraightViewWiden(1);
    {
        const MapClip clip = {VIEW_LEFT, VIEW_TOP, VIEW_RIGHT, StraightBottom(), VIEW_LEFT, VIEW_RIGHT};

        StraightViewWiden(-1);
        if (!BattleMapPlan(bg, &clip, wraps, &plan) || !MapPlanFresh(bg, &plan))
        {
            ++sStraightWhy[STRAIGHT_STALE];
            return false;
        }
    }
    if (plan.count == 0) return false;
    key = SceneViewKey(left, top, width, height, layers);
    sStraightStill = key == sStraightKey ? sStraightStill + 1 : 0;
    sStraightKey = key;
    if (sStraightStill >= SCENERY_STILL_FRAMES)
    {
        /* From over again: what the key leaves out may not let it be kept. */
        sStraightStill = 0;
        ++sStraightWhy[STRAIGHT_STILL];
        return false;
    }
    memset(sBgDrawn, 0, sizeof(sBgDrawn));
    sBgDrawn[bg] = plan.count;
    sBattleLayerDrawn |= 1u << bg;
    sBattleLayerUsed[bg] = sStats.frames;
    sSceneBlendLayer = -1;
    sStraightStrip = wraps;
    return true;
}

/* The scenery's lines from `top` to `bottom` onto an eye, its view widened. */
VIDEO_PIECE void EyeSceneryLines(C3D_RenderTarget *target, int top, int bottom)
{
    SceneOn(target);
    C2D_ViewReset();
    BattleRectCompose(VIEW_LEFT, top, VIEW_RIGHT, bottom, 16u);
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    sScissored = false;
}

/*
 * The scenery on an eye holding the world's picture: every line of it, those
 * the surface would hold and those past it, in one pass at the screen's scale.
 * The view widened each side and the layer moved back by as much, as the
 * scenery composed into each eye (RenderBattleStereo). The GPU reads the layer's
 * texture from here on: drawn before the frame's world, and sealed by the
 * split after LayersRender.
 */
static void BattleSceneryStraightDraw(C3D_RenderTarget *target)
{
    float origin = sLayerOrigin;

    StraightViewWiden(1);
    sLayerOrigin = BattleSceneryShift() - BATTLE_EDGE_MARGIN;
    sLayerOnEye = true;
    /* The blank layers measured, not walked, as the edges' (BattleEdgesCompose). */
    sLayerMeasureOnly = MonoSceneBlank(BattleSceneOut(0));
    EyeSceneryLines(target, VIEW_TOP, StraightBottom());
    sLayerMeasureOnly = 0;
    sLayerOnEye = false;
    sLayerOrigin = origin;
    StraightViewWiden(-1);
}

/* The scenery without its sprites, kept while it holds. */
static void BattleSceneComposeKept(void)
{
    sSceneCacheAllowed = true;
    BattleSceneCompose(BATTLE_STEREO_MARGIN, 16u);
    sSceneCacheAllowed = false;
}

/* What a piece of RenderBattleStereo decides and a later one reads. */
typedef struct
{
    unsigned control;  /* BLDCNT */
    unsigned furthest; /* the sprites' furthest priority (BattleSprites) */
    unsigned front;    /* the backgrounds in front of sprites */
    bool any;          /* sprites shown */
    bool apart;        /* the sprites in slots, not composed with the scene */
    bool scenery;      /* a scenery to put on the eyes */
    /* The scenery composed into each eye rather than placed from the scene
     * surface, whose columns the slots have then (below). */
    bool sceneryEye;
    /* The text box once, unless it blends with the scene under it. */
    bool textOnce;
    bool whole;      /* composed whole at once, the frame before having been */
    bool straight;   /* the layer straight onto the eyes */
    bool eyesFilled; /* the eyes cleared as memory fills */
} StereoPlan;

/* What the frame starts from. */
VIDEO_PIECE void StereoBegin(StereoPlan *p, const C3D_Tex *world)
{
    p->control = Reg(0x50);
    p->scenery = true;
    p->sceneryEye = false;
    p->textOnce = !(((p->control >> 6) & 3) == 1 && (p->control & 1));

    /* Not for the battle over the voxel world, which shares the scene surface
     * on its few frames here (a move's background, its last frame): made then,
     * the slots took the field's atlases with them, and the field came back
     * in 2D. Nor for a frame showing no sprite. */
    if (!world && (Reg(0) & 0x1000) && !CtrVoxel_InBattle()) sSlotWanted = true;
    /* The slots go where the battle without the slider keeps its scenery. */
    sMonoKeeperHeld = false;
}

/* The frame's state from zero, then the scenery and the sprites. */
VIDEO_PIECE void StereoScene(StereoPlan *p, C3D_Tex *world)
{
    sSceneOn = NULL;
    sParallax = 0;
    sLayerShift = 0;
    sHudFlatScene = false;
    sSceneKeptNow = false;
    sSceneFilled = false;
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
    p->whole = !world && sFlatBefore && !sObjWin.ready;
#ifdef CTR_AUDIT_BITS
    if (CtrDumpOff() & 8) p->whole = false;
#endif

    /* The layer straight onto the eyes, in place of the scenery's compose. */
    p->straight = world && BattleSceneryStraight(p->textOnce);

    sSceneCacheAllowed = !p->whole;
    if (!p->straight) BattleSceneCompose(BATTLE_STEREO_MARGIN, p->whole ? 0u : 16u);
    sSceneCacheAllowed = false;
    HudCoverUpdate();
    p->any = BattleSprites(&p->furthest, &p->apart, world != NULL);
    if (p->straight && !p->any)
    {
        ++sStraightWhy[STRAIGHT_SPRITES];
        p->straight = false;
        BattleSceneComposeKept();
    }
}

/* The sprites go with the scene, for a blend the slots cannot give (BlendWhy). */
VIDEO_PIECE void StereoNotApart(StereoPlan *p, unsigned why)
{
    StereoWhy(0);
    BlendWhy(why);
    p->apart = false;
}

/* The OBJ window in a slot: the group the window's box is inside of, as far as
 * it shows; with none, all composed whole as before, the window with the
 * scenery. */
VIDEO_PIECE void StereoObjWindowSlot(StereoPlan *p)
{
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
        if (p->apart) StereoNotApart(p, under ? 3 : 2);
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
VIDEO_PIECE void StereoFrontLayers(StereoPlan *p)
{
    unsigned nearestFront = 3, furthestFront = 0;

    p->front = 0;
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

    for (unsigned bg = 1; bg < 4 && p->any; ++bg)
        if (sBgDrawn[bg] && (Reg(8 + bg * 2) & 3) < p->furthest)
        {
            unsigned priority = Reg(8 + bg * 2) & 3;

            p->front |= 1u << bg;
            if (priority < nearestFront) nearestFront = priority;
            if (priority > furthestFront) furthestFront = priority;
            if (((p->control >> 6) & 3) == 1 && (p->control & (1u << bg)) && p->apart)
            {
                if ((p->control & 0x1000) && sPaletteFade <= 0.0f && !(sObjWin.ready && (sObjWin.layers & (1u << bg))))
                    blendFront |= 1u << bg;
                else StereoNotApart(p, 4);
            }
        }
    /* Loose sprites with a background in front of sprites: their place
     * among the patches is not worked out; composed whole. */
    if (sLooseCount && p->front && p->apart) StereoNotApart(p, 0);
    /* Both kinds over the same group, in either order, are not kept apart:
     * a patch drawn over the blended slot would cover the blend. */
    if (blendFront && (p->front & ~blendFront) && p->apart) StereoNotApart(p, 4);
    for (unsigned k = 0, groups = sSlotCount; p->front && p->apart && k < groups; ++k)
    {
        BattleSlot *slot = &sSlots[k];
        int reach = (int)ceilf(BATTLE_BACK_DEPTH / sShiftZoom) + 1;

        if (slot->furthest <= nearestFront) { slot->front = true; continue; }
        if (slot->nearest <= furthestFront) { StereoWhy(2); p->apart = false; break; }
        if (blendFront) { slot->blendBg = (uint8_t)blendFront; continue; }
        if (sSlotCount == sizeof(sSlots) / sizeof(sSlots[0])) { StereoWhy(3); p->apart = false; break; }
        BattleSlot *patch = &sSlots[sSlotCount++];

        *patch = *slot;
        patch->patch = (uint8_t)p->front;
        patch->blendBg = 0;
        patch->objWin = false;
        patch->x = (int16_t)(slot->x - reach);
        patch->w = (int16_t)(slot->w + 2 * reach);
    }
}

/* Any of the frame's slots packed into this target. */
VIDEO_PIECE bool SlotsPackedIn(const C3D_RenderTarget *target)
{
    for (unsigned k = 0; k < sSlotCount; ++k)
        if (sRegions[sSlots[k].region].target == target) return true;
    return false;
}

/* The slots' places: the regions they may go in, and what is free there. */
VIDEO_PIECE void StereoRegions(StereoPlan *p, C3D_Tex *world)
{
    if (world && !sBgDrawn[1] && !sBgDrawn[2] && !sBgDrawn[3]) p->scenery = false;
    if (p->apart && p->any)
    {
        int after = (sScenePut.width + 7) & ~7;

        sRegionCount = 0;
        bool lent = false;

        if (p->scenery && !world && sSlotTarget)
            sRegions[sRegionCount++] = (SlotRegion){sSlotTarget, &sSlotTex, 0, sSlotTex.width};
        sRegions[sRegionCount++] = (SlotRegion){sScene, &sSceneTex, sSceneColumn + (p->scenery ? after : 0), SCENE_W};
        /* Over the world, the scene surface first: its own surface only for
         * what does not fit beside the world's picture. */
        if (world && sSlotTarget)
            sRegions[sRegionCount++] = (SlotRegion){sSlotTarget, &sSlotTex, 0, sSlotTex.width};
        if (!p->scenery) sSceneCached = false;   /* the slots go over its columns */
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

            if (world && p->scenery)
            {
                sRegions[0].column = sSceneColumn;
                packed = SlotsPack();
            }
            /* Those columns hold nothing with the scenery straight on the eyes. */
            if (packed && p->straight) lent = true;
            else if (packed)
            {
                p->sceneryEye = true;
                p->textOnce = false;
                sSceneCached = false;
                lent = true;
            }
            else { StereoWhy(3); p->apart = false; }
        }
        if (p->apart && !world) SlotsPackCut();
        if (p->apart && world && sSlotTarget && SlotsPackedIn(sSlotTarget)) lent = true;

        /* No room beside the world (a double battle's four Pokemon, a move's
         * background as well): a surface of their own from the next frame
         * on (sSlotWorld), kept while such frames go on. */
        if (world && (!p->apart || lent))
        {
            sSlotWorld = true;
            sSlotWanted = true;
        }
    }
}

/* The way the frame goes is settled: what the scenery still owes for it. */
VIDEO_PIECE void StereoSettled(StereoPlan *p, const C3D_Tex *world)
{
    sFlatBefore = !p->apart && !world;
    /* Composed whole per eye below, scenery and all. */
    if (p->straight && !p->apart)
    {
        ++sStraightWhy[STRAIGHT_WHOLE];
        p->straight = false;
    }
    sStraightFrames += p->straight;
    if (p->whole && p->apart)
    {
        BattleSceneComposeKept();
    }
}

/*
 * Slots in the scene surface's free columns: cleared there by drawing,
 * which on hardware did not always land (RenderBattleWorld), where the
 * scenery's own composing clears the whole surface as a memory fill.
 * Composed again then, as every frame was before, whenever that fill did
 * not come this frame (sSceneFilled) - not only with the scene kept
 * whole: with the scenery kept apart and only its blending layer composed
 * again (BattleSceneComposeApart), the scene surface was kept as well.
 */
VIDEO_PIECE void StereoSlotsInScene(const StereoPlan *p, const C3D_Tex *world)
{
    if (p->apart && p->any && !world && SlotsPackedIn(sScene))
    {
        ++sSlotsInScene;
        if (!sSceneFilled)
        {
            ++sSlotsInSceneComposed;
            if (!sSceneKeptNow) ++sSlotsInSceneUnkept;
            sSceneCached = false;
            BattleSceneComposeKept();
            sSceneKeptNow = false;
        }
    }
}

/*
 * Over the world, the scenery kept with the scene surface left unfilled
 * (sSceneFillOwed): its columns past the scenery hold what earlier frames
 * left there, and a drawn clear of them is not enough on hardware (above).
 * A frame whose slots go into them after all, or into the scenery's
 * (sceneryEye), or that is composed whole per eye, pays the fill now and
 * composes the scenery again, as every frame did before: the world's
 * picture drawn again over the fill first, nothing having sampled it yet
 * (SceneFillPay). Otherwise only what that compose drew over the world's
 * picture is drawn (BattleSceneStrip). Where the slots went this frame
 * is the next one's guess (sSceneWorldLent).
 */
VIDEO_PIECE void StereoWorldScene(const StereoPlan *p)
{
    bool taken = p->sceneryEye || (p->apart && p->any && SlotsPackedIn(sScene));

    if (sSceneFillOwed && (taken || !p->apart))
    {
        if (taken) ++sWorldScene.slots;
        else ++sWorldScene.whole;
        sSceneWhy |= taken ? SCENE_WHY(SLOTS) : SCENE_WHY(PATH);
        SceneFillPay();
        BattleSceneComposeKept();
        sSceneKeptNow = false;
        /* The slots go over its columns (sceneryEye, above). */
        if (p->sceneryEye) sSceneCached = false;
    }
    else if (sSceneFillOwed)
    {
        BattleSceneStrip();
        ++sWorldScene.kept;
        ++sSceneReused;
    }
    /* What the compose it stands for draws over the world's picture. */
    else if (p->straight && sStraightStrip) BattleSceneStrip();
    sSceneFillOwed = false;
    sSceneWorldLent = taken;
}

/*
 * The 2D battle's eyes are put together on the screens' targets, each
 * cleared so far by drawing it, 192,000 pixels on the GPU's way to the
 * frame's last lists. As memory fills, as RenderEye clears its target,
 * they go in here: behind what is queued so far, ahead of everything the
 * eyes draw, while the GPU is not yet started on them, so with no cache
 * flush of their own; the split is the one GpuStartEarly would have made.
 * Nothing this frame drew into these targets or reads them before the
 * eyes below, and all the paths that follow draw both. With the text box
 * blending over the picture (textOnce off) the eyes are put together in
 * the logical surface, which keeps its drawn clear, and so does the voxel
 * battle, whose eyes are the world's picture. One split for the pair.
 */
VIDEO_PIECE void StereoEyesFill(StereoPlan *p, uint32_t clear, const C3D_Tex *world)
{
    p->eyesFilled = !world && p->textOnce;
#ifdef CTR_AUDIT_BITS
    /* nocache.txt bit 33554432: drawn into each eye, as before (A/B frames). */
    if (CtrDumpOff() & 33554432u) p->eyesFilled = false;
#endif
    if (p->eyesFilled)
    {
        ColourClear(sTop, clear);
        C3D_RenderTargetClear(sTopRight, C3D_CLEAR_COLOR, __builtin_bswap32(clear), 0);
        ++sEyesFilled;
    }
    else if (!world) ++sEyesDrawn;
}

/* The GPU started on what is queued, and how the frame went counted. */
VIDEO_PIECE void StereoPrepared(const StereoPlan *p)
{
    GpuStartEarly(CTR_EARLY_BATTLE);
    ++sStereoSplit[p->apart ? 0 : 1];
    sSceneryEyeFrames += p->sceneryEye;
    /* A straddle marks the frame (StereoWhy(2)); nothing else does. */
    if (sBarrierNow)
        ++sBarrierFrames[p->apart ? (p->sceneryEye ? 1 : 0) : ((sFrameFlat & CTR_FLAT_BG_FRONT) ? 3 : 2)];
    if (sObjWin.ready)
    {
        bool kept = p->apart && sObjWinSlotted && !(sSceneWhy & SCENE_WHY(COMPOSED));

        ++*(kept ? &sObjWinScene.kept : &sObjWinScene.composed);
        sObjWinScene.hidden += (unsigned)__builtin_popcount(SceneWindowHidden());
    }
    /* Composed whole below, the window with the rest. */
    if (!p->apart) sObjWinSlotted = false;
}

/*
 * The 2D battle that cannot be split: the whole scene, sprites and
 * all, composed once and put behind the screen at the scenery's depth
 * in each eye, the text box over it as below. Composed per eye, with
 * each sprite at its depth, it was the scene and the text box twice a
 * frame - 15-18 ms of drawing, 30 fps on an Old 3DS for the second a
 * stat change's animation lasts. The sprites lose their depth for as
 * long as it lasts; the scenery keeps its.
 */
VIDEO_PIECE void StereoFlat(StereoPlan *p)
{
    if (!p->whole) BattleSceneCompose(BATTLE_STEREO_MARGIN, 0);
    sGpuStage = CTR_STAGE_SCENE;
    p->any = false;
    p->front = 0;
    p->scenery = true;
    sHudFlatScene = true;
}

/* The logical surface's rows from `top` down, at their place on the eye. */
VIDEO_PIECE void BattleEyeRows(float top)
{
    const Tex3DS_SubTexture rows = {CTR_GAME_WIDTH, (u16)(CTR_GAME_HEIGHT - top), 0, 1 - top / 256.0f,
        CTR_GAME_WIDTH / 512.0f, 1 - CTR_GAME_HEIGHT / 256.0f};

    C2D_DrawImageAt((C2D_Image){&sSurface, &rows}, 0, top, 0, NULL, 1, 1);
}

/* Whole per eye, as RenderEye composes it, at the same depths. */
VIDEO_PIECE void StereoWholeEyes(float slider, C3D_Tex *world)
{
    PORT_PROF_BEGIN(bsWhole);

    for (int eye = 0; eye < 2; ++eye)
    {
        C3D_RenderTarget *target = eye ? sTopRight : sTop;

        sEyeParallax = eye ? -slider : slider;
        BlendForget();
        ColourClear(sLogical, 0);
        RenderBattleScene();
        SceneOn(sLogical);
        Blend(5, false, false);
        HudDrawCrisp(HUD_SPRITE_DEPTH);
        BattleBoxCompose();
        C3D_FrameSplit(0);
        BattleWorldEye(target, world);
        Blend(5, false, false);
        BattleEyeRows(0);
        C2D_Flush();
        C3D_FrameSplit(0);
    }
    sEyeParallax = 0;
    PORT_PROF_END(bsWhole, PORT_PROF_BS_WHOLE);
}

/* The sprites' slots both eyes take, in one command list. */
VIDEO_PIECE void StereoSlotsOnce(const StereoPlan *p)
{
    sSceneOn = NULL;
    PORT_PROF_BEGIN(bsSlots);
    if (p->any && !(Reg(0) & 128)) SlotsCompose();
    PORT_PROF_END(bsSlots, PORT_PROF_BS_SLOTS);
}

/* The text box and the edges for both eyes. */
VIDEO_PIECE void StereoText(const StereoPlan *p, const C3D_Tex *world)
{
    PORT_PROF_BEGIN(bsText);
    bool keep = p->textOnce && p->scenery && BattleEdgesKeep(!p->any);
    uint32_t textKey = p->textOnce && !world ? BattleTextKey() : 0;
    bool textKept = p->textOnce && BattleTextKeep(world == NULL, textKey);

    sEdgeStore.on = false;
    if (textKept)
    {
        /* The text's rows as the frame before left them; the edges' rows
         * cleared as the pass clears them, unless those are kept too. */
        if (!keep) BattleLogicalClear(0, BATTLE_EDGE_STORE_END);
    }
    else if (p->textOnce) BattleTextCompose(keep ? BATTLE_EDGE_STORE_END : 0);
    if (p->textOnce) BattleTextStore(textKept, world == NULL, textKey);
    else sTextStore.held = false;
    /* The edges for both eyes, after the text box's clear. */
    if (p->textOnce && p->scenery && !p->straight)
    {
        float origin = sLayerOrigin;

        sLayerOrigin = 0.0f;
        BattleEdgesStore(!p->any, keep);
        sLayerOrigin = origin;
    }
    else sEdgeStore.held = false;
    SplitFrame();
    sGpuStage = CTR_STAGE_LOGICAL;
    PORT_PROF_END(bsText, PORT_PROF_BS_TEXT);
}

/* The view widened, the scenery composed into the eye. */
VIDEO_PIECE void BattleEyeSceneryComposed(C3D_RenderTarget *picture)
{
    float offX = sOffX, origin = sLayerOrigin;
    int targetW = sTargetW;

    /* The view widened each side, the layers moved back by as
     * much: some layers stop at the view's edge (VIEW_LEFT,
     * VIEW_RIGHT) before they are moved by the eye's shift,
     * which left the screen's edge columns of the far side
     * uncovered, the world showing through. */
    StraightViewWiden(1);
    sLayerOrigin = BattleSceneryShift() - BATTLE_EDGE_MARGIN;
    EyeSceneryLines(picture, sScenePut.top, BATTLE_SCENE_BOTTOM);
    sOffX = offX;
    sTargetW = targetW;
    sLayerOrigin = origin;
}

/* The scenery on the eye, and the lines past the surface. */
VIDEO_PIECE void BattleEyeScenery(const StereoPlan *p, C3D_RenderTarget *picture)
{
    float origin = sLayerOrigin;

    /* The scenery alone, the sprites being the slots': unfiltered,
     * as the 2D field draws its layers; the slots keep the filter. */
    if (p->sceneryEye) BattleEyeSceneryComposed(picture);
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
    if (p->textOnce)
        BattleEdgesPlace(picture, BattleSceneryShift());
    else
    {
        /* Widened as the scenery above, moved back by as much. */
        sLayerOrigin = BattleSceneryShift() - BATTLE_EDGE_MARGIN;
        SceneOn(sLogical);
        BattleEdgesCompose(!p->any, BATTLE_EDGE_MARGIN, 0);
        sLayerOrigin = origin;
    }
}

/* The sprites' slots on the eye. */
VIDEO_PIECE void BattleEyeSlots(const StereoPlan *p, C3D_RenderTarget *picture)
{
    SlotsPlace(picture, false, false);
    if (p->front)
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

/* The eye's picture put together in the logical surface: the text box composed
 * over it, then the eye made ready for it. */
VIDEO_PIECE void BattleEyeBox(C3D_RenderTarget *target, C3D_Tex *world)
{
    SceneOn(sLogical);
    Blend(5, false, false);
    BattleBoxCompose();
    SplitFrame();
    if (world) BattleWorldEye(target, world);
    else DrawnClear(target, C2D_Color32(0, 0, 0, 255), 0, 0, CTR_GAME_WIDTH, CTR_GAME_HEIGHT);
}

/* The logical surface onto the eye, what of it there is to place. */
VIDEO_PIECE void BattleEyePlace(const StereoPlan *p, C3D_RenderTarget *target)
{
    BlendForget();
    SceneOn(target);
    C2D_ViewReset();
    Blend(5, false, false);
    /* Only the text box's lines when nothing of BG0 is above it. */
    if (p->textOnce && !sTextAbove) BattleEyeRows(BATTLE_BAND_TOP * sZoom + sOffY);
    /* Not the rows the edges are stored in (BattleEdgesStore). */
    else if (sEdgeStore.on) BattleEyeRows(BATTLE_EDGE_STORE_END);
    else BattleEyeRows(0);
    C2D_Flush();
}

/* One eye's picture. */
VIDEO_PIECE void BattleEyeDraw(const StereoPlan *p, int eye, uint32_t clear, float slider, C3D_Tex *world)
{
    C3D_RenderTarget *target = eye ? sTopRight : sTop;
    /* Where the eye's picture is put together: the screen, or with the
     * text box composed over it, the logical surface. */
    C3D_RenderTarget *picture = p->textOnce ? target : sLogical;

    sEyeParallax = eye ? -slider : slider;
    if (world && p->textOnce) BattleWorldEye(target, world);
    else if (!p->eyesFilled) DrawnClear(picture, world ? 0 : clear, 0, 0, CTR_GAME_WIDTH, CTR_GAME_HEIGHT);
    /* Filled above; what is placed next sets its state, as after that clear. */
    else BlendForget();
    if (p->straight) BattleSceneryStraightDraw(target);
    else if (p->scenery) BattleEyeScenery(p, picture);
    if (p->any && !(Reg(0) & 128)) BattleEyeSlots(p, picture);
    /* The info boxes over them at the screen's pixels: at their sprites'
     * depth, or the scenery's when the sprites are composed with it. */
    if (!(Reg(0) & 128))
    {
        SceneOn(picture);
        HudDrawCrisp(sHudFlatScene ? HUD_SCENERY_DEPTH : HUD_SPRITE_DEPTH);
    }
    if (!p->textOnce) BattleEyeBox(target, world);
    BattleEyePlace(p, target);
}

/*
 * Both eyes of a battle (see BATTLE_BACK_DEPTH). `world` is the voxel
 * battle's finished world, NULL for the 2D battle.
 */
static void RenderBattleStereo(uint32_t clear, float slider, C3D_Tex *world)
{
    StereoPlan p;

    StereoBegin(&p, world);
    PORT_PROF_BEGIN(bsPrep);
    StereoScene(&p, world);
    if (sObjWinSlotted) StereoObjWindowSlot(&p);
    StereoFrontLayers(&p);
    StereoRegions(&p, world);
    StereoSettled(&p, world);
    StereoSlotsInScene(&p, world);
    if (world) StereoWorldScene(&p);
    if (p.apart) sGpuStage = CTR_STAGE_SCENE;
    StereoEyesFill(&p, clear, world);
    StereoPrepared(&p);
    PORT_PROF_END(bsPrep, PORT_PROF_BS_PREP);

    if (!p.apart && !world) StereoFlat(&p);
    else if (!p.apart)
    {
        StereoWholeEyes(slider, world);
        return;
    }
    StereoSlotsOnce(&p);
    StereoText(&p, world);
    PORT_PROF_BEGIN(bsEyes);
    for (int eye = 0; eye < 2; ++eye) BattleEyeDraw(&p, eye, clear, slider, world);
    sEdgeStore.on = false;
    sEyeParallax = 0;
    sObjWinSlotted = false;
    PORT_PROF_END(bsEyes, PORT_PROF_BS_EYES);
}
