/*
 * Part of 3ds_video.c. A battle transition over the field: its line bands,
 * layers and picture, and RenderTransition.
 */

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
    sTransitionObjShown = (uniform & 16u) != 0;
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

/*
 * The sprites crossing the picture's sides go on over the screen's margins: a
 * ball rolls in from the screen's edge, not from the picture's. Only those
 * crossing: one wholly outside waits there for its turn, out of the GBA's sight
 * (the balls of the trail, parked beside the picture). And only sprites no
 * window cuts, on an undimmed picture: the margins have neither.
 */
static void TransitionSpritesInMargins(void)
{
    /* The screen target is tilted: its x runs up the screen, from the right. */
    static const struct { int column, x0, x1; } sides[] = {
        {TRANSITION_OBJ_X0, 0, (int)TRANSITION_X + 2},
        {TRANSITION_OBJ_X1, CTR_GAME_WIDTH - (int)TRANSITION_X - 2, CTR_GAME_WIDTH},
    };
    float zoom = sZoom, offX = sOffX, offY = sOffY;
    int viewX = sViewX, viewY = sViewY;
    bool fieldLayers = sFieldLayers, fieldUi = sFieldUi;

    if (!sTransitionObjShown || (sLineRegs[0][0] & 128) || TransitionBandAt(0, 0x11).bright > 0.0f) return;
    C2D_Flush();
    sFieldLayers = sFieldUi = false;
    sViewX = sViewY = 0;
    sZoom = TRANSITION_ZOOM;
    sOffX = TRANSITION_X;
    sOffY = 0.0f;
    sObjFilter = OBJ_TRANSITION;
    sRegLine = PictureRegs(0);
    BlendForget();
    ViewBase();
    for (unsigned side = 0; side < 2; ++side)
    {
        /* An empty clip at that column: only a sprite across it passes. */
        sClipX0 = sClipX1 = sides[side].column;
        sClipY0 = -64;
        sClipY1 = 224;
        C3D_SetScissor(GPU_SCISSOR_NORMAL, 0, (unsigned)(CTR_GAME_WIDTH - sides[side].x1), CTR_GAME_HEIGHT,
                       (unsigned)(CTR_GAME_WIDTH - sides[side].x0));
        for (int priority = 3; priority >= 0; --priority)
            DrawObjects((unsigned)priority, false);
        C2D_Flush();
    }
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    sScissored = false;
    sRegLine = NULL;
    sObjFilter = OBJ_ALL;
    sFieldLayers = fieldLayers;
    sFieldUi = fieldUi;
    sViewX = viewX;
    sViewY = viewY;
    sZoom = zoom;
    sOffX = offX;
    sOffY = offY;
    ClipToView();
    BlendForget();
    C2D_ViewReset();
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

            GpuStartEarly(CTR_EARLY_VOXEL);
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
        /* The 2D field to the GPU while the transition's own picture is
         * composed, as the voxel world's above: what that decodes goes into
         * slots nothing queued reads (GetTileSlot), and its lines come from
         * the bridge's table (sLineRegs), which only the CPU reads. */
        {
            bool started = sGpuEarly;

            GpuStartEarly(CTR_EARLY_TRANSITION);
            if (started || !sGpuEarly) C3D_FrameSplit(0);
        }
        BlendForget();
    }

    if (sScene) TransitionCompose();

    C2D_TargetClear(sTop, clear);
    SceneOn(sTop);
    C2D_ViewReset();
    Blend(5, false, false);
    TransitionField();
    if (sScene)
    {
        TransitionPicture();
        TransitionSpritesInMargins();
    }
    C2D_Flush();
    C3D_FrameSplit(0);
}
