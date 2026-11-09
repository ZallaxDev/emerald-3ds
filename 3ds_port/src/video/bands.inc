/*
 * Part of 3ds_video.c. Depth planes: their bands and surfaces, RenderBands
 * and BlitBands.
 */

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
 * using them, the bottom screen's surface when no screen wants it, and the
 * slot surface when only the battle's kept scenery holds it (SlotsYield).
 * Whether anything went.
 */
static bool ReleaseIdleSurfaces(void)
{
    /* The battle's kept scenery too, which logs its own line (SlotsYield). */
    bool any = false, keeper = SlotsYield();

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
    return any || keeper;
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
static unsigned BandMask(unsigned band)
{
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
        w = gCtrObjDims[shape][attr1 >> 14][0] << twice;
        h = gCtrObjDims[shape][attr1 >> 14][1] << twice;
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
        unsigned mask = BandMask(band);
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
        /*
         * The furthest plane, the one that holds most of the picture, to the
         * GPU while the CPU composes the nearer ones. Each plane begins with
         * its own split (PlaneClear), so this is the split after the first;
         * the planes are only sampled after the last (BlitBands). What the
         * nearer planes decode goes into slots no queued command reads
         * (GetTileSlot), and the per-line tables they read are the CPU's.
         */
        if (last) GpuStartEarly(CTR_EARLY_PLANE);
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
