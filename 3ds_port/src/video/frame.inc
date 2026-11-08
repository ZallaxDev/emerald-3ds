/*
 * Part of 3ds_video.c. The clip rectangle, the stereo state, the quality
 * counts, the transition's line registers, and what every later unit calls:
 * Reg, Read16, Min, Error, ClipToView and BlendForget.
 */

/*
 * Current clip rectangle. With windows enabled the frame is composed as a
 * partition of rectangles and every layer is visited once per rectangle, so
 * geometry outside the current one must not be submitted at all: the scissor
 * would discard it after paying for it, which is what pushed a windowed
 * overworld frame to three times the draw calls it needs.
 */
static int sClipX0, sClipY0, sClipX1 = CTR_GAME_WIDTH, sClipY1 = CTR_GAME_HEIGHT;

static void ClipToView(void)
{
    sClipX0 = VIEW_LEFT;
    sClipY0 = VIEW_TOP;
    sClipX1 = VIEW_RIGHT;
    sClipY1 = VIEW_BOTTOM;
}

/* Per-frame time in the layer walk and in the sprites, reported with fps. */
static uint64_t sBgTicks, sObjTicks;

/*
 * Stereoscopic depth. The GBA already orders everything by priority, 0 nearest
 * and 3 furthest, so that is the depth scale: priority 3 stays at the screen
 * plane and the rest come forward, which puts interface windows in front of
 * the map and the sprites between the two.
 *
 * The frame is composed once per eye with each layer displaced by whole
 * pixels; a fractional displacement would resample the very pixel grid this
 * renderer exists to preserve. At the widest slider setting the nearest layer
 * separates by CTR_STEREO_PIXELS in each eye.
 */
#define CTR_STEREO_PIXELS 1.0f

/*
 * Stereoscopy of the voxel world (RenderVoxel): the world drawn once per eye
 * from its own camera (CtrVoxel_Draw, eye 0 and 1). The second pass costs
 * the GPU the world's fill again, which an Old 3DS does not have in a frame
 * (an earlier attempt ran at 40-45 fps there), so OPTIONS' 3D WORLD on AUTO
 * leaves the world flat on one, slider or not, and takes the slider on a New
 * 3DS (sVoxelStereo, set at start-up; -DCTR_VOXEL_STEREO=1 or =0 forces
 * AUTO either way). ON and OFF decide outright. 3D DEPTH scales the slider.
 */
static bool sVoxelStereo;
/* An Old 3DS (CtrVideo_Init): CtrBattle_Sandstorm. */
static bool sOldModel;

static bool VoxelStereoWanted(void)
{
    int mode = CtrSettings_GetInt("voxel_stereo", 0);

    return mode == 1 || (mode == 0 && sVoxelStereo);
}

/* The slider as the voxel world takes it: scaled by OPTIONS' 3D DEPTH. */
static float VoxelStereoSlider(float slider)
{
    static const float depths[] = {0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f};
    int depth = CtrSettings_GetInt("voxel_depth", 3);

    if (depth < 0 || depth >= (int)(sizeof(depths) / sizeof(depths[0]))) depth = 3;
    return slider * depths[depth];
}

/* Displacement per depth unit for the eye being composed, and the resulting
 * displacement of the layer being drawn, both in whole screen pixels. */
static float sParallax, sLayerShift;
/* The 3D battle (RenderBattleStereo): sprites at their own depths, and the
 * tiles each background drew in the last composition. */
static bool sSpriteDepth;
static unsigned sBgDrawn[4];
static unsigned sStereoSplit[2];
/* Why a 3D battle frame was composed whole per eye: a sprite blending, one in
 * mosaic, a background in front of a sprite, no room for the slots. */
static unsigned sStereoWhy[4];
/* The same, for this frame alone (CTR_FLAT_*, the frame profiler's record). */
static uint8_t sFrameFlat;
/* Diagnostic: which kind of blend (BlendWhy). */
static unsigned sBlendWhy[6];
/*
 * The 2D battle's slots with the slider up, in 600 frames (RenderBattleStereo):
 * frames with slots in the scene surface's free columns, those of them with
 * the scene composed again for it (the surface not filled this frame), and of
 * those the ones whose scene was not kept whole either - the scenery kept
 * apart with its blending layer composed again - which were left to the
 * drawn clear before. Then the frames whose slots were packed again cut to
 * what their sprites draw (SlotsPackCut), the frames it was tried in, and how
 * many slots were cut in the first.
 */
static unsigned sSlotsInScene, sSlotsInSceneComposed, sSlotsInSceneUnkept;
static unsigned sSlotsCutFrames, sSlotsCutTried, sSlotsCut;
/*
 * What frames left out or drew another way (CtrVideo_TakeQualityCounts), and
 * this frame's own marks of it, counted once a frame at its end whatever the
 * passes that met them (QualityFrameEnd).
 */
static CtrQualityCounts sQuality, sQualityTaken, sQualityLogged;
static bool sQualityMosaic, sQualityObjWin, sQualityLineDropped;

/* The counts since `since` was last taken, into `out` (all are uint32_t). */
static void QualityDelta(CtrQualityCounts *since, CtrQualityCounts *out)
{
    const uint32_t *now = (const uint32_t *)&sQuality;
    uint32_t *then = (uint32_t *)since, *delta = (uint32_t *)out;

    for (unsigned i = 0; i < sizeof(CtrQualityCounts) / sizeof(uint32_t); ++i)
    {
        delta[i] = now[i] - then[i];
        then[i] = now[i];
    }
}

void CtrVideo_TakeQualityCounts(CtrQualityCounts *out)
{
    QualityDelta(&sQualityTaken, out);
}

void CtrVideo_NoteLineDropped(void)
{
    sQualityLineDropped = true;
}

/* Once a frame, after it is drawn: its marks into the counts. */
static void QualityFrameEnd(void)
{
    sQuality.mosaicIgnored += sQualityMosaic;
    sQuality.objWinIgnored += sQualityObjWin;
    sQuality.lineDropped += sQualityLineDropped;
    sQualityMosaic = sQualityObjWin = sQualityLineDropped = false;
    if (sLineAlphaOn && sLineAlphaBanded) ++sQuality.lineAlphaBanded;
    else if (sLineAlphaOn) ++sQuality.lineAlphaDropped;
    sLineAlphaBanded = false;
}

static void StereoWhy(unsigned why)
{
    ++sStereoWhy[why];
    sFrameFlat |= 1u << why;
}
static float BattleSpriteShift(int centre);
static void SceneOn(C3D_RenderTarget *target);
static float BattleSceneryShift(void);
static bool sObjMaskOn;
/* The OBJ window's own composition (ObjWindowCompose): its sprites as a
 * mask, then the layers it shows over that mask. */
enum { OBJWIN_NONE, OBJWIN_MASK, OBJWIN_LAYERS };
static unsigned sObjWinPass;
/* The battle's BG0 drew something above its text box (DrawBattleText). */
static bool sTextAbove;
static uint32_t sObjMask[4];
/*
 * The Wireless Adapter's status indicator: link_rfu puts it in OAM entry 125
 * itself, at the GBA screen's top right. In the 2D battle (RenderEye) it is
 * left out of the scene and drawn over the finished picture
 * (BattleIndicatorCompose): the lines above the scene's surface are filled
 * from its top rows, repeated, and its top rows came out again there as a
 * stray line. The 3D battle's surface has no sprites in it.
 */
#define WIRELESS_INDICATOR_OAM 125
static bool sObjIndicatorPass, sIndicatorApartHere;
static bool WirelessIndicatorApart(void);
/* Fixed horizontal placement of every layer, on top of the depth parallax.
 * Zero for the 2D compositor, which reproduces the GBA frame as it is. */
static float sLayerOrigin;
/* A layer being drawn straight onto an eye, over the voxel world
 * (BattleSceneryStraightDraw): Scissor and Blend see a screen target. */
static bool sLayerOnEye;
/* The voxel overlay's text and prompts in this eye, in whole pixels: in
 * front of the nearest world (ComposeVoxelOverlay). Zero everywhere else. */
static int sOverlayEyeShift;

/*
 * The text layer is a 32-column tilemap, so it only addresses the left 256px
 * of the 400px viewport, and widening it does not fit in background VRAM (see
 * the note on sStandardTextBox_WindowTemplates in src/menu.c). That is a
 * limit on where the *game* can put a window, not on where this compositor can
 * draw one: on the field BG0 carries nothing but windows, so it is drawn moved
 * to where it belongs. The standard text box spans x=16..232 within the band,
 * so its centre reaches the middle of the viewport at +76. The voxel overlay
 * does the same (ComposeVoxelOverlay); sFieldUi asks for it in the 2D field.
 */
#define CTR_FIELD_UI_SHIFT 76.0f
static bool sFieldUi;
/*
 * The banner of a field move (src/field_effect.c, the mon shown for Surf, Cut,
 * Fly...) is BG0 too, but a pattern of streaks meant to wrap across the whole
 * screen as on the GBA, not a window: while it is up BG0 is drawn over the
 * full width and not moved to the text band.
 */
static bool sFieldBanner;

void CtrVideo_SetFieldBanner(bool banner)
{
    sFieldBanner = banner;
}
/* The 2D field this frame: its text backgrounds are drawn from layer
 * textures kept up to date cell by cell (LayerRenderCells). */
static bool sFieldLayers;

/*
 * Composing the whole frame twice costs twice the CPU, which is a frame an Old
 * 3DS does not have. Instead each depth plane is composed once into its own
 * surface and the two eyes are then four textured quads each: the layer walk,
 * which is where the time goes, runs once per frame however many eyes there
 * are.
 *
 * A layer that blends with what is underneath it needs that underneath in its
 * own surface: its plane is given the picture behind it, colour only, before
 * its layers are drawn (RenderBands). The last plane holds every remaining
 * priority.
 */
/*
 * Three planes rather than one per priority: each one costs a surface to clear
 * and a textured quad per eye, and the overworld has no frame to spare for a
 * fourth. The two furthest priorities share the last plane, which still leaves
 * the interface, the near layer and the background at three depths.
 */
#define CTR_PRIORITIES 4
#define CTR_BANDS 3
static C3D_Tex sBandTex[CTR_BANDS];
static C3D_RenderTarget *sBand[CTR_BANDS];
/* How many of them exist: two when the third gave its VRAM to a layer. */
static unsigned sBandCount;
static bool sBandsFailed;
/* A failed allocation is tried again this many frames later, not never: the
 * VRAM it needs may have been in use by the overworld only for a while. */
#define CTR_BANDS_RETRY_FRAMES 300u
static uint32_t sBandsRetryFrame;
/* Whether each plane drew anything; an empty one is not worth a quad. */
static bool sBandUsed[CTR_BANDS];
/* Depth planes the last frame used, 0 when it was composed per eye. */
static unsigned sPlanes;
/*
 * What the current composition pass may draw, by depth slot: bit 2p is the
 * sprites of priority p, bit 2p+1 its backgrounds - front to back in that
 * order, as the GBA stacks them.
 */
#define SLOTS_ALL 255u
#define SLOT_OBJ(p) (1u << ((p) * 2))
#define SLOT_BG(p) (2u << ((p) * 2))
static unsigned sPriorityMask = SLOTS_ALL;

/*
 * Last blend configuration submitted. Reset whenever something else may have
 * changed the GPU state, which is once per eye pass.
 */
static unsigned sBlendKey = ~0u;
static void BlendForget(void) { sBlendKey = ~0u; }

/*
 * The 2D compositor draws everything at depth 0, in submission order, so a
 * depth test can only ever pass. Citro2D still enables one (GEQUAL, writing
 * depth): every pixel of every layer read and wrote the 16-bit depth of the
 * logical surface and of the screens - a third more memory traffic per pixel
 * for nothing, and the 3D slider's planes multiply the pixels. Off, the
 * picture is the same.
 */
static void Flat2D(void)
{
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
}

/* C2D_TargetClear without the depth buffer, which the 2D frame never reads. */
static void ColourClear(C3D_RenderTarget *target, uint32_t color)
{
    C2D_Flush();
    C3D_FrameSplit(0);
    C3D_RenderTargetClear(target, C3D_CLEAR_COLOR, __builtin_bswap32(color), 0);
}

/*
 * A battle transition's frame, line by line (CtrVideo_SetLineRegisters): the
 * registers each of its 160 lines shows, and while a band of those lines is
 * composed, that band's values in place of the registers (Reg).
 */
static bool sTransitionRequested, sTransition;
/* Composing the transition's own picture: GBA geometry (RenderTransition). */
static bool sTransitionCompose;
static const uint16_t (*sLineRegs)[CTR_LINE_REGS];
static const uint16_t *sRegLine;

static unsigned Reg(unsigned offset)
{
    if (sCentredScreen == CTR_CENTRED_POKEDEX_CAUGHT && offset >= 0x18 && offset <= 0x1e)
        return 0;
    if (sRegLine && offset - CTR_LINE_REG_FIRST < CTR_LINE_REGS * 2)
        return sRegLine[(offset - CTR_LINE_REG_FIRST) / 2];
    if (offset == 0x52 && sLineAlphaBand >= 0)
        return (unsigned)sLineAlphaBand;
    return sMemory.regs[offset / 2];
}
static unsigned Min(unsigned a, unsigned b) { return a < b ? a : b; }

static void Error(unsigned bit, const char *reason)
{
    if (!(sReported & (1u << bit)))
    {
        sReported |= 1u << bit;
        ++sStats.errors;
        CtrLog_Write(CTR_LOG_ERROR, "VIDEO: %s", reason);
    }
}

static unsigned Read16(unsigned offset)
{
    if (offset >= 0x10000)
    {
        Error(0, "BG tilemap outside BG VRAM");
        return 0;
    }
    return sMemory.vram[offset] | (sMemory.vram[offset + 1] << 8);
}
