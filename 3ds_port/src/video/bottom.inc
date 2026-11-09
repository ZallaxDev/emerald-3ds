/*
 * Part of 3ds_video.c. The bottom screen's surface: its strips, the native
 * menus and the transfer; the storage's windows; and RenderEye, the frame to
 * one eye.
 */

/*
 * The bottom screen's surface, the whole 320x240 screen. The PokéNav uses
 * the 240x240 area left of the button column; the PC's boxes all of it. It
 * is not linked to the screen, since a linked target is copied over the
 * whole of it and would cover the column the bottom screen draws itself;
 * after the frame the PokéNav's part is copied into the first 240 columns of
 * the framebuffer, which are exactly that area (the framebuffer runs column
 * by column from the left edge), and the boxes' into all of it. Made when
 * one of them opens, given back a few seconds after it closes - or at once
 * when the voxel world asks for VRAM (CtrVideo_RequestPlaneRelease): kept
 * after a bag opened from the voxel field, its 150 KiB in the middle of VRAM
 * left no 256 KiB for an atlas, and the world waited seconds for its tiles.
 */
static bool sPlaneReleaseAsked;
#define BOTTOM_SIZE 240
#define BOTTOM_WIDTH 320
static C3D_RenderTarget *sBottom;
static uint32_t sBottomUsed, sBottomFailFrame;
static bool sBottomFailed;

/*
 * The canvas round the window, over the window's columns: the header's 40
 * rows above it and the dock's 40 below, drawn into the bottom surface as a
 * texture, so the window's columns go to the framebuffer whole, one display
 * transfer, with nothing of the picture outside the window. (A copy that
 * skipped those rows, a texture copy with gaps, came out shifted 40 rows in
 * Azahar and is gone.) 3ds_bottom_ui.c names its canvas when it
 * puts it on the screen (CtrVideo_BottomStrips); the texture is filled from it
 * before the bottom is drawn, the same frame. Rows 0-39 are the header's,
 * 64-103 the dock's.
 */
#define STRIP_TEX_W 256
#define STRIP_TEX_H 128
#define STRIP_DOCK_ROW 64
static C3D_Tex sStripTex;
static const u16 *sStripCanvas;
static bool sStripDirty, sStripReady, sStripFailed;

void CtrVideo_BottomStrips(const uint16_t *canvas)
{
    sStripCanvas = canvas;
    sStripDirty = true;
}

static void StripRelease(void)
{
    if (sStripTex.data) C3D_TexDelete(&sStripTex);
    memset(&sStripTex, 0, sizeof(sStripTex));
    sStripReady = sStripFailed = false;
}

/* The canvas is the framebuffer's layout: column x, row y at x * 240 + 239 - y. */
static void StripUpload(void)
{
    u16 *data;

    if (!sStripCanvas || sStripFailed) return;
    if (!sStripTex.data)
    {
        if (!C3D_TexInit(&sStripTex, STRIP_TEX_W, STRIP_TEX_H, GPU_RGB565))
        {
            memset(&sStripTex, 0, sizeof(sStripTex));
            sStripFailed = true;
            CtrLog_Write(CTR_LOG_ERROR, "video: no memory for the bottom window's surroundings (linear free=%lu); "
                         "the window's columns go whole, the header and the dock over them black",
                         (unsigned long)linearSpaceFree());
            return;
        }
        C3D_TexSetFilter(&sStripTex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&sStripTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        sStripDirty = true;
    }
    if (!sStripDirty) return;
    data = sStripTex.data;
    for (int strip = 0; strip < 2; ++strip)
    {
        int screenY = strip ? CTR_BOTTOM_WIN_Y + 160 : 0, texY = strip ? STRIP_DOCK_ROW : 0;

        for (int ty = 0; ty < CTR_BOTTOM_WIN_Y; ty += 8)
            for (int tx = 0; tx < 240; tx += 8)
            {
                u16 *tile = data + (((texY + ty) / 8) * (STRIP_TEX_W / 8) + tx / 8) * 64;

                for (int x = 0; x < 8; ++x)
                {
                    const u16 *column = sStripCanvas + (CTR_BOTTOM_WIN_X + tx + x) * BOTTOM_SIZE
                                      + (BOTTOM_SIZE - 1 - (screenY + ty));

                    for (int y = 0; y < 8; ++y)
                        tile[sMorton[y * 8 + x]] = column[-y];
                }
            }
    }
    TexFilled(&sStripTex);
    sStripDirty = false;
    sStripReady = true;
}

static void BottomRelease(void)
{
    if (sBottom) { ClearWhenMadeForget(sBottom); C3D_RenderTargetDelete(sBottom); }
    sBottom = NULL;
    StripRelease();
}

/* Outside the frame, where deleting a render target may wait for the GPU. */
static bool BottomReady(bool wanted)
{
    if (!wanted)
    {
        if (sBottom && (sPlaneReleaseAsked || sStats.frames - sBottomUsed > LAYER_IDLE_FRAMES))
        {
            BottomRelease();
            CtrLog_Write(CTR_LOG_VIDEO, "bottom screen surface released (VRAM free=%lu)",
                         (unsigned long)vramSpaceFree());
        }
        return false;
    }
    sBottomUsed = sStats.frames;
    if (sBottom) return true;
    if (sBottomFailed && sStats.frames - sBottomFailFrame < LAYER_RETRY_FRAMES) return false;
    sBottom = C3D_RenderTargetCreate(BOTTOM_SIZE, BOTTOM_WIDTH, GPU_RB_RGB565, -1);
    /* The battle's kept scenery before any atlas (SlotsYield). */
    if (!sBottom && SlotsYield())
        sBottom = C3D_RenderTargetCreate(BOTTOM_SIZE, BOTTOM_WIDTH, GPU_RB_RGB565, -1);
    if (!sBottom && CtrVoxel_ReleaseIdleVramFor(BOTTOM_SIZE * BOTTOM_WIDTH * 2u, true) > 0)
        sBottom = C3D_RenderTargetCreate(BOTTOM_SIZE, BOTTOM_WIDTH, GPU_RB_RGB565, -1);
    ClearWhenMade(sBottom);
    if (!sBottom)
    {
        if (!sBottomFailed)
            CtrLog_Write(CTR_LOG_ERROR, "VIDEO: no VRAM for the bottom screen surface (free=%lu)",
                         (unsigned long)vramSpaceFree());
        sBottomFailed = true;
        sBottomFailFrame = sStats.frames;
        return false;
    }
    sBottomFailed = false;
    CtrLog_Write(CTR_LOG_VIDEO, "bottom screen surface ready (VRAM free=%lu)", (unsigned long)vramSpaceFree());
    return true;
}

static bool sBottomInUse;
/* 3ds_bottom_ui.c draws the game's screen itself, from its state: its
 * picture is not composed at all (CtrVideo_SetBottomNative). */
static bool sBottomNative;

bool CtrVideo_BottomInUse(void) { return sBottomInUse; }

/*
 * The battle's sandstorm picture (AnimTask_LoadSandstormBackground): none
 * over a voxel world that draws the sandstorm itself (CtrVoxel_DrawsSand) -
 * the route's own sand already blows through the battle, and BG1 over the
 * world, in front of the sprites, made the 3D slider compose the scene
 * whole per eye (bs.whole 10 ms, 27 fps, port.log 2026-10-28); its sprites
 * still fly. Otherwise held still on an Old 3DS: scrolled, BG1 changes every
 * frame, so the scene surface and the lines past it are composed afresh
 * each one, ~30-40 fps with the 3D slider up; still, the scene is kept
 * between frames (SceneCacheKey) but for those its blend steps on.
 */
unsigned char CtrBattle_Sandstorm(void)
{
    if (sBattleWorld && CtrVoxel_DrawsSand()) return CTR_SANDSTORM_NONE;
    return sOldModel ? CTR_SANDSTORM_STILL : CTR_SANDSTORM_BLOWN;
}
void CtrVideo_SetBottomNative(bool native) { sBottomNative = native; }

/* Without the classic menus the bottom screen draws the Pokédex, the bag and
 * the party menu opened from the field itself: their pictures are never
 * composed, not even the frames they open or close in while it has no view
 * of them (the old screen flashed there). */
static bool sNativeMenus;

void CtrVideo_SetNativeMenus(bool on) { sNativeMenus = on; }

static bool BottomNative(unsigned screen)
{
    return sBottomNative
        || (sNativeMenus && (screen == CTR_CENTRED_POKEDEX || screen == CTR_CENTRED_BAG
                             || screen == CTR_CENTRED_PARTY));
}

/* The screens drawn over the whole bottom screen, rather than in its window. */
static bool BottomWhole(unsigned screen)
{
    return screen == CTR_CENTRED_STORAGE || screen == CTR_CENTRED_SUMMARY || screen == CTR_CENTRED_BAG_WHOLE
        || screen == CTR_CENTRED_PARTY_WHOLE || screen == CTR_CENTRED_SHOP || screen == CTR_CENTRED_TRADE_MENU
        || screen == CTR_CENTRED_TRADE || screen == CTR_CENTRED_POKEDEX_CAUGHT;
}

/* The screens drawn on the bottom screen at all. */
static bool BottomScreen(unsigned screen)
{
    return screen == CTR_CENTRED_POKENAV || screen == CTR_CENTRED_BAG || screen == CTR_CENTRED_POKEDEX
        || screen == CTR_CENTRED_PARTY || screen == CTR_CENTRED_CARD || BottomWhole(screen);
}

bool CtrVideo_BottomWhole(void) { return sBottomInUse && BottomWhole(sCentredScreen); }

/*
 * The composed picture into the bottom screen's framebuffer, inside the
 * frame: there citro3d queues the transfer after the drawing (a split) instead
 * of waiting for it. Called after C3D_FrameEnd it waited for the GPU to finish
 * the whole frame and then for the copy, every frame a bottom screen is up:
 * the PC, the bag and the Pokédex spent ~5 ms of CPU a frame on an Old 3DS
 * beyond their layers and sprites, and dropped frames. The framebuffer is
 * single-buffered (3ds_log.c).
 *
 * A game screen sits in the bottom screen's window, the GBA picture 1:1 at
 * (CTR_BOTTOM_WIN_X, CTR_BOTTOM_WIN_Y); 3ds_bottom_ui.c's canvas owns the
 * rest (the header, the dock, the window's frame). One with the whole screen
 * (BottomWhole) is the picture at the same place and its margins round it,
 * all of it transferred. A display transfer copies whole framebuffer lines,
 * which are screen columns: the window's 240 columns go whole, with the
 * header and the dock drawn over them (StripUpload). Without that texture
 * they are black over the window's columns, and that is logged once.
 */

static void BottomTransfer(void)
{
    u16 *fb = (u16 *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
    u16 *in;
    const u32 flags = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0)
                    | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565)
                    | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);

    if (!sBottom || !fb || gfxGetScreenFormat(GFX_BOTTOM) != GSP_RGB565_OES) return;
    /* A screen with the whole screen (the boxes, a summary, the bag from a
     * battle): all of it, margins included. */
    if (BottomWhole(sCentredScreen))
    {
        C3D_SyncDisplayTransfer((u32 *)sBottom->frameBuf.colorBuf, GX_BUFFER_DIM(BOTTOM_SIZE, BOTTOM_WIDTH),
                                (u32 *)fb, GX_BUFFER_DIM(BOTTOM_SIZE, BOTTOM_WIDTH), flags);
        return;
    }
    /* The target is BOTTOM_WIDTH lines of BOTTOM_SIZE (8x8 tiles): the
     * window's first column is line CTR_BOTTOM_WIN_X, a whole tile row. */
    in = (u16 *)sBottom->frameBuf.colorBuf + CTR_BOTTOM_WIN_X * BOTTOM_SIZE;
    /* The header and the dock drawn over the columns (StripUpload): whole. */
    C3D_SyncDisplayTransfer((u32 *)in, GX_BUFFER_DIM(BOTTOM_SIZE, 240),
                            (u32 *)(fb + CTR_BOTTOM_WIN_X * BOTTOM_SIZE), GX_BUFFER_DIM(BOTTOM_SIZE, 240), flags);
}

/*
 * A game screen on the bottom screen (the PC's boxes, a summary, the bag, the
 * party menu, the Pokédex, the PokéNav): the GBA picture 1:1, cut at its
 * edges, where the GBA screen ends and parks what it hides. In the window
 * only the picture reaches the screen (BottomTransfer), everything round it
 * is 3ds_bottom_ui.c's; with the whole screen its margins are the layers'
 * own past the picture (CentredFill), the sprites kept to the picture.
 */
static void StorageComposePart(int left, int right, int top, int bottom, unsigned exclude)
{
    ClipToView();
    sClipX0 = left;
    sClipX1 = right;
    sClipY0 = top;
    sClipY1 = bottom;
    sLayerExclude = exclude;
    C2D_Flush();
    sScissored = true;
    RestoreScissor();
    Compose();
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    sScissored = false;
    sLayerExclude = 0;
}

static void WindowCompose(void)
{
    if (BottomWhole(sCentredScreen))
    {
        unsigned back = CentredLayers(CentredFillOf(sCentredScreen)) ? CentredLayers(CentredFillOf(sCentredScreen))
                                                                       : 1u << 3;

        StorageComposePart(-CTR_BOTTOM_WIN_X, 240 + CTR_BOTTOM_WIN_X, -CTR_BOTTOM_WIN_Y, 160 + CTR_BOTTOM_WIN_Y,
                           63u & ~back);
    }
    StorageComposePart(0, 240, 0, 160, 0);
    ClipToView();
}

int CtrVideo_BottomPictureY(int y)
{
    y -= CTR_BOTTOM_WIN_Y;
    return y >= 0 && y < 160 ? y : -1;
}

static void SurfaceFilter(GPU_TEXTURE_FILTER_PARAM filter)
{
    /* Batched draws sample with the state at the flush: flush first, and
     * rebind so the new filter reaches the GPU. */
    C2D_Flush();
    C3D_TexSetFilter(&sSurface, filter, filter);
    C3D_TexBind(0, &sSurface);
}

static void RenderEye(C3D_RenderTarget *target, uint32_t clear, float parallax)
{
    const Tex3DS_SubTexture logical = {CTR_GAME_WIDTH, CTR_GAME_HEIGHT, 0, 1,
        CTR_GAME_WIDTH / 512.0f, 1 - CTR_GAME_HEIGHT / 256.0f};
    bool scene = sBattle && sScene;

    sParallax = parallax;
    sLayerShift = 0;
    BlendForget();
    ColourClear(sLogical, clear);
    sIndicatorApartHere = scene;
    if (scene) RenderBattleScene();
    /* The 2D battle's scene, the bulk of its GPU work, goes to the GPU while
     * the CPU composes the text box and the info boxes over it. */
    if (scene && target != sBottom)
    {
        sGpuStage = CTR_STAGE_SCENE;
        GpuStartEarly(CTR_EARLY_SCENE);
    }
    SceneOn(sLogical);
    Blend(5, false, false);
    StageUnderlay();
    if (target != sBottom) CornerUnderlay();
    if (scene) BattleIndicatorCompose();
    sIndicatorApartHere = false;
    if (scene) HudDrawCrisp(HUD_SPRITE_DEPTH);
    /* After a scene composed on its own, only the text box is left. */
    if (scene) sLayerExclude = 63 & ~(1u | 32u | BattleBoxLayers());
    {
        PORT_PROF_BEGIN(bmText);
        if (!(Reg(0) & 128))
        {
            if (target != sBottom) Compose();
            else WindowCompose();
        }
        if (scene) PORT_PROF_END(bmText, PORT_PROF_BM_TEXT);
    }
    sLayerExclude = 0;
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    /*
     * The logical surface composed: the GPU on it while the CPU records its
     * blit and, per eye, composes it again for the other eye - which only
     * appends to the queue: a tile the first eye drew was checked this frame
     * and keeps its slot as it is, and one only the second eye shows (its
     * parallax brings it in) is decoded into a slot nothing queued reads
     * (GetTileSlot). Not
     * for the bottom screen, whose transfer into its own framebuffer the
     * CPU-drawn canvas round it shares (BottomTransfer); its route keeps the
     * start at FrameEnd. The split either way.
     */
    {
        bool started = sGpuEarly;

        if (!started) sGpuStage = CTR_STAGE_LOGICAL;
        if (target != sBottom) GpuStartEarly(CTR_EARLY_EYE);
        if (started || !sGpuEarly) C3D_FrameSplit(0);
        sGpuStage = CTR_STAGE_LOGICAL;
    }

    BlendForget();
    ColourClear(target, C2D_Color32(0, 0, 0, 255));
    SceneOn(target);
    if (target == sBottom)
    {
        /* The middle 320 columns of the logical frame, the picture in the
         * window's place (BottomTransfer keeps only that). Tilted as a screen
         * target is. */
        int width = BOTTOM_WIDTH;
        const Tex3DS_SubTexture middle = {(u16)width, BOTTOM_SIZE,
            (CTR_GAME_WIDTH - width) / 2 / 512.0f, 1,
            (CTR_GAME_WIDTH + width) / 2 / 512.0f, 1 - BOTTOM_SIZE / 256.0f};

        /* The framebuffer's own measures, as for a screen target. */
        C2D_SceneSize(BOTTOM_SIZE, BOTTOM_WIDTH, true);
        C2D_ViewReset();
        Blend(5, false, false);
        C2D_DrawImageAt((C2D_Image){&sSurface, &middle}, 0, 0, 0, NULL, 1, 1);
        /* In the window: the header and the dock over its columns, so they
         * go whole to the screen (BottomTransfer). */
        if (!BottomWhole(sCentredScreen))
        {
            StripUpload();
            if (sStripReady)
            {
                const float w = 240.0f / STRIP_TEX_W, h = (float)CTR_BOTTOM_WIN_Y / STRIP_TEX_H,
                            dock = (float)STRIP_DOCK_ROW / STRIP_TEX_H;
                const Tex3DS_SubTexture head = {240, CTR_BOTTOM_WIN_Y, 0, 1, w, 1 - h};
                const Tex3DS_SubTexture foot = {240, CTR_BOTTOM_WIN_Y, 0, 1 - dock, w, 1 - dock - h};

                C2D_DrawImageAt((C2D_Image){&sStripTex, &head}, CTR_BOTTOM_WIN_X, 0, 0, NULL, 1, 1);
                C2D_DrawImageAt((C2D_Image){&sStripTex, &foot}, CTR_BOTTOM_WIN_X, CTR_BOTTOM_WIN_Y + 160, 0, NULL, 1, 1);
            }
        }
    }
    else if (sStageZoomed)
    {
        /* The GBA picture alone, out of the margins composed round it,
         * magnified and centred, smooth as no whole number of pixels fits.
         * A scene shown through window 0 alone with a bar at its top and its
         * bottom (the charge) fills the width, the bars' outer lines cut off;
         * one whose window is open at an edge (the chase, its top bar left
         * where the art cuts Rayquaza's tail off: rayquaza_scene.c,
         * DUO_LOWER) is the window's lines alone, covering the screen, a few
         * columns at its sides cut off; one without is shown whole, fitted
         * to the height. */
        int y0 = 0, y1 = 160;
        float scale = (float)CTR_GAME_HEIGHT / 160.0f;

        if ((Reg(0) & 0xE000) == 0x2000 && !(Reg(0x4a) & 31))
        {
            unsigned v = Reg(0x44);
            int top = (int)(v >> 8), bottom = (int)(v & 255);

            if (bottom > 160 || top > bottom) bottom = 160;
            scale = (float)CTR_GAME_WIDTH / 240.0f;
            if ((top == 0) != (bottom == 160) && bottom - top >= 16)
            {
                y0 = top;
                y1 = bottom;
                scale = fmaxf(scale, (float)CTR_GAME_HEIGHT / (float)(y1 - y0));
            }
        }
        {
            const Tex3DS_SubTexture picture = {240, (u16)(y1 - y0),
                CTR_STAGE_X / 512.0f, 1 - (CTR_STAGE_Y + y0) / 256.0f,
                (CTR_STAGE_X + 240) / 512.0f, 1 - (CTR_STAGE_Y + y1) / 256.0f};

            C2D_ViewReset();
            Blend(5, false, false);
            SurfaceFilter(GPU_LINEAR);
            C2D_DrawImageAt((C2D_Image){&sSurface, &picture}, (CTR_GAME_WIDTH - 240 * scale) / 2,
                            (CTR_GAME_HEIGHT - (y1 - y0) * scale) / 2, 0, NULL, scale, scale);
            SurfaceFilter(GPU_NEAREST);
        }
    }
    else
    {
        C2D_ViewReset();
        Blend(5, false, false);
        C2D_DrawImageAt((C2D_Image){&sSurface, &logical}, 0, 0, 0, NULL, 1, 1);
    }
    C2D_Flush();
    C3D_FrameSplit(0);
}
