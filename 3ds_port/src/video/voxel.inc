/*
 * Part of 3ds_video.c. The voxel world's picture: diorama, bloom, gloom,
 * Flash's light, the 2D field over it in a swoop, and RenderVoxel.
 */

/* Seals a command list. Before the GPU is started early it is submitted at
 * FrameEnd, whose linear-heap flush covers all producers, Citro2D included;
 * after, the GX wrappers flush before it is queued. */
static void GpuSplit(void)
{
    C3D_FrameSplit(0);
}

/*
 * HD-2D diorama: the tilt-shift of a miniature. The camera always looks north,
 * so the top of the screen is the distance and the bottom the nearest ground;
 * both are blurred, most at the screen's edge and not at all by the focus band
 * around the player. Each band is two copies of the world surface drawn over
 * it with bilinear filtering half a texel off either way, so each copy is the
 * average of four texels and the two together are centred on the pixel;
 * fading in from the focus band to the edge. A wider
 * blur than the four one-pixel taps it replaces, for about the same fill.
 * The surface is sampled NEAREST again afterwards: the picture itself stays
 * pixel-sharp.
 */
#define DIORAMA_TOP 100    /* rows blurred at the top */
#define DIORAMA_BOTTOM 56  /* ... and at the bottom */

static void DioramaTap(int y0, int rows, bool top, float dx, float dy, float alpha)
{
    /* One column short at each side: the surface past the picture is not
     * part of it. */
    const Tex3DS_SubTexture region = {CTR_GAME_WIDTH - 2, (u16)rows,
        (1.0f + dx) / 512.0f, 1.0f - (y0 + dy) / 256.0f,
        (1.0f + dx + CTR_GAME_WIDTH - 2) / 512.0f, 1.0f - (y0 + dy + rows) / 256.0f};
    C2D_ImageTint tint;

    C2D_AlphaImageTint(&tint, 0.0f);
    if (top)
        C2D_TopImageTint(&tint, C2D_Color32f(1.0f, 1.0f, 1.0f, alpha), 0.0f);
    else
        C2D_BottomImageTint(&tint, C2D_Color32f(1.0f, 1.0f, 1.0f, alpha), 0.0f);
    C2D_DrawImageAt((C2D_Image){&sSurface, &region}, 1, y0, 0, &tint, 1, 1);
}

/* The strength chosen in OPTIONS: 1 is the full blur, the taps' own alphas. */
static float DioramaStrength(void)
{
    return CtrSettings_VoxelBlurStrength() / 100.0f;
}

static void VoxelDiorama(void)
{
    /* The swoop takes the blur off with the grade (CtrVoxel_SwoopNeutral). */
    float strength = DioramaStrength() * (1.0f - CtrVoxel_SwoopNeutral());

    if (strength <= 0.005f) return;

    SurfaceFilter(GPU_LINEAR);
    /* Half a texel either way: two 2x2 averages on either side of each
     * pixel, together centred on it - a blur that does not shift the picture. */
    DioramaTap(0, DIORAMA_TOP, true, 0.5f, 0.5f, 0.67f * strength);
    DioramaTap(0, DIORAMA_TOP, true, -0.5f, -0.5f, 0.50f * strength);
    DioramaTap(CTR_GAME_HEIGHT - DIORAMA_BOTTOM, DIORAMA_BOTTOM, false, 0.5f, 0.5f, 0.60f * strength);
    DioramaTap(CTR_GAME_HEIGHT - DIORAMA_BOTTOM, DIORAMA_BOTTOM, false, -0.5f, -0.5f, 0.45f * strength);
    SurfaceFilter(GPU_NEAREST);
}

/*
 * Bloom: the brightest parts of the picture - sunlit sand, white walls, water
 * catching the light - glow softly into their surroundings. The world surface
 * is drawn at a quarter of its size into a small target, keeping only what is
 * brighter than BLOOM_THRESHOLD (texture environment 4, which citro2d leaves
 * free), then stretched back over the screen with bilinear filtering and
 * added. Two passes: 6000 pixels, then one additive screen.
 */
#define BLOOM_W (CTR_GAME_WIDTH / 4)
#define BLOOM_H (CTR_GAME_HEIGHT / 4)
#define BLOOM_THRESHOLD 0.85f

static void VoxelBloomPrepare(void)
{
    const Tex3DS_SubTexture logical = {CTR_GAME_WIDTH, CTR_GAME_HEIGHT, 0, 1,
        CTR_GAME_WIDTH / 512.0f, 1 - CTR_GAME_HEIGHT / 256.0f};
    unsigned t = (unsigned)(BLOOM_THRESHOLD * 255.0f + 0.5f);
    C3D_TexEnv *env;

    C2D_TargetClear(sBloom, C2D_Color32(0, 0, 0, 255));
    SceneOn(sBloom);
    C2D_ViewReset();
    Blend(5, false, false);
    SurfaceFilter(GPU_LINEAR);
    /* Replace, not blend: the surface's alpha is not the picture's. */
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    /* (colour - threshold) x 4: black below it. */
    env = C3D_GetTexEnv(4);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_SUBTRACT);
    C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_4);
    C3D_TexEnvColor(env, 0xFF000000u | t << 16 | t << 8 | t);
    C2D_DrawImageAt((C2D_Image){&sSurface, &logical}, 0, 0, 0, NULL, 0.25f, 0.25f);
    C2D_Flush();
    C3D_TexEnvInit(C3D_GetTexEnv(4));
    SurfaceFilter(GPU_NEAREST);
    BlendForget();
}

static void VoxelBloomCompose(float strength)
{
    const Tex3DS_SubTexture region = {BLOOM_W, BLOOM_H, 0, 1,
        BLOOM_W / 128.0f, 1 - BLOOM_H / 64.0f};
    C2D_ImageTint tint;

    C2D_Flush();
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_ZERO, GPU_ONE);
    C2D_AlphaImageTint(&tint, strength);
    C2D_DrawImageAt((C2D_Image){&sBloomTex, &region}, 0, 0, 0, &tint, 4.0f, 4.0f);
    C2D_Flush();
    BlendForget();
}

/*
 * The game's own UI and weather over the voxel world. BG0 carries text; only
 * OAM entries tagged by the sprite sorter as weather are composed. Drawing all
 * OBJ here would duplicate the player and every NPC over their billboards.
 */

/*
 * The dark of a cave: a soft ring of shadow closing in round the player, over
 * the world and under the game's text. One textured quad (CtrVoxel_Gloom).
 */
static void VoxelGloom(void)
{
    float x, y, size, amount;
    const C3D_Tex *tex = CtrVoxel_Gloom(&x, &y, &size, &amount);
    C2D_ImageTint tint;

    if (tex == NULL || amount <= 0.0f) return;
    const Tex3DS_SubTexture whole = {tex->width, tex->height, 0.0f, 1.0f, 1.0f, 0.0f};
    unsigned alpha = (unsigned)(amount * 255.0f + 0.5f);

    Blend(5, false, false);
    C2D_PlainImageTint(&tint, C2D_Color32(0, 0, 0, alpha > 255 ? 255 : alpha), 1.0f);
    C2D_DrawImageAt((C2D_Image){(C3D_Tex *)tex, &whole}, x - size * 0.5f, y - size * 0.5f, 0,
                    &tint, size / tex->width, size / tex->height);
    C2D_Flush();
    BlendForget();
}

/*
 * The circle of light of a dark cave in the voxel world: the same ground lit
 * as on the GBA - the game's radius, measured on the floor round the player,
 * so the camera's foreshortening makes it an ellipse - but with a soft edge
 * that warms to amber as it falls into the dark, a faint warm pool round the
 * player, a slow flicker, and a glow when Flash is used, while the circle
 * opens. Two textured quads and the dark round them (four rectangles): no
 * cost to speak of. Faded with the screen like everything the game fades.
 */
#define FLASH_LIGHT_DIM 64
#define FLASH_LIGHT_QUAD 1.25f  /* the quad's half size, in radii */
#define FLASH_LIGHT_INNER 0.82f /* where the edge starts to darken, in radii */
#define FLASH_LIGHT_OUTER 1.18f /* where it is dark, in radii */
#define FLASH_LIGHT_POOL 0.85f  /* how far the warm pool reaches, in radii */
static C3D_Tex sFlashDarkTex, sFlashPoolTex;
static bool sFlashTexTried, sFlashTexReady;

static void MakeFlashLight(void)
{
    uint32_t *dark;
    uint8_t *pool;
    const float inner = FLASH_LIGHT_INNER / FLASH_LIGHT_QUAD, outer = FLASH_LIGHT_OUTER / FLASH_LIGHT_QUAD;
    const float reach = FLASH_LIGHT_POOL / FLASH_LIGHT_QUAD;

    sFlashTexTried = true;
    if (!C3D_TexInit(&sFlashDarkTex, FLASH_LIGHT_DIM, FLASH_LIGHT_DIM, GPU_RGBA8))
        goto fail;
    if (!C3D_TexInit(&sFlashPoolTex, FLASH_LIGHT_DIM, FLASH_LIGHT_DIM, GPU_A8))
    {
        C3D_TexDelete(&sFlashDarkTex);
        goto fail;
    }
    dark = sFlashDarkTex.data;
    pool = sFlashPoolTex.data;
    for (unsigned y = 0; y < FLASH_LIGHT_DIM; ++y)
        for (unsigned x = 0; x < FLASH_LIGHT_DIM; ++x)
        {
            float dx = ((float)x + 0.5f) / (FLASH_LIGHT_DIM * 0.5f) - 1.0f;
            float dy = ((float)y + 0.5f) / (FLASH_LIGHT_DIM * 0.5f) - 1.0f;
            float d = sqrtf(dx * dx + dy * dy);
            float t = (d - inner) / (outer - inner), p = 1.0f - d / reach, warm;
            unsigned texel = CtrVideo_Texel(x, y, FLASH_LIGHT_DIM), a;

            t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
            t = t * t * (3.0f - 2.0f * t);
            /* Amber where the edge is half dark, black beyond: blended by
             * alpha, the rim gives about a fifth of amber at its middle. */
            warm = 0.72f * (1.0f - t);
            a = (unsigned)(t * 255.0f + 0.5f);
            dark[texel] = (uint32_t)(warm * 255.0f) << 24 | (uint32_t)(warm * 150.0f) << 16
                        | (uint32_t)(warm * 60.0f) << 8 | a;
            p = p < 0.0f ? 0.0f : p;
            pool[texel] = (uint8_t)(p * p * 255.0f + 0.5f);
        }
    C3D_TexSetFilter(&sFlashDarkTex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&sFlashDarkTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    TexFilled(&sFlashDarkTex);
    C3D_TexSetFilter(&sFlashPoolTex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&sFlashPoolTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    TexFilled(&sFlashPoolTex);
    sFlashTexReady = true;
    return;
fail:
    CtrLog_Write(CTR_LOG_VIDEO, "flash light textures not made (linear free=%lu)",
                 (unsigned long)linearSpaceFree());
}

static void VoxelFlashLight(void)
{
    const Tex3DS_SubTexture whole = {FLASH_LIGHT_DIM, FLASH_LIGHT_DIM, 0.0f, 1.0f, 1.0f, 0.0f};
    float x = CTR_GAME_WIDTH * 0.5f, y = CTR_GAME_HEIGHT * 0.5f, sx = 1.0f, sy = 1.0f;
    float t = (float)sFieldLight.ticks, radius, w, h, x0, y0, x1, y1, fade = sPaletteFade;
    uint32_t fadeTo = CtrVideo_RGBA8(sPaletteFadeColor, true);
    uint32_t dark = C2D_Color32((uint8_t)((fadeTo >> 24) * fade), (uint8_t)(((fadeTo >> 16) & 255) * fade),
                                (uint8_t)(((fadeTo >> 8) & 255) * fade), 255);
    C2D_ImageTint tint;

    if (!FieldLightShown()) return;
    if (!sFlashTexTried) MakeFlashLight();
    CtrVoxel_PlayerLightSpot(&x, &y, &sx, &sy);
    /* A flame's slow breathing: two slow waves, a pixel or two. */
    radius = sFieldLight.shown * (1.0f + 0.010f * sinf(t * 0.071f) + 0.006f * sinf(t * 0.173f + 1.3f));
    w = radius * FLASH_LIGHT_QUAD * sx;
    h = radius * FLASH_LIGHT_QUAD * sy;
    x0 = x - w; x1 = x + w;
    y0 = y - h; y1 = y + h;

    C2D_Flush();
    Blend(5, false, false);
    if (sFlashTexReady && radius > 0.5f)
    {
        float glow = (0.10f + 0.03f * sinf(t * 0.21f) + 0.55f * sFieldLight.burst) * (1.0f - fade);

        if (glow > 0.004f)
        {
            unsigned a = (unsigned)(glow * 255.0f + 0.5f);

            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_ZERO, GPU_ONE);
            C2D_PlainImageTint(&tint, C2D_Color32(255, 196, 128, a > 255 ? 255 : a), 1.0f);
            C2D_DrawImageAt((C2D_Image){&sFlashPoolTex, &whole}, x0, y0, 0, &tint,
                            2.0f * w / FLASH_LIGHT_DIM, 2.0f * h / FLASH_LIGHT_DIM);
            C2D_Flush();
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ZERO);
        }
        C2D_PlainImageTint(&tint, dark, fade);
        C2D_DrawImageAt((C2D_Image){&sFlashDarkTex, &whole}, x0, y0, 0, &tint,
                        2.0f * w / FLASH_LIGHT_DIM, 2.0f * h / FLASH_LIGHT_DIM);
    }
    else
        x0 = x1 = x, y0 = y1 = y;
    /* The dark round the quad. */
    if (y0 > 0.0f) C2D_DrawRectSolid(0, 0, 0, CTR_GAME_WIDTH, y0, dark);
    if (y1 < CTR_GAME_HEIGHT) C2D_DrawRectSolid(0, y1, 0, CTR_GAME_WIDTH, CTR_GAME_HEIGHT - y1, dark);
    if (x0 > 0.0f) C2D_DrawRectSolid(0, y0, 0, x0, y1 - y0, dark);
    if (x1 < CTR_GAME_WIDTH) C2D_DrawRectSolid(x1, y0, 0, CTR_GAME_WIDTH - x1, y1 - y0, dark);
    C2D_Flush();
    BlendForget();
}

static void ComposeVoxelOverlay(void)
{
    /* The text band and the prompts beside it are moved together, as the GBA
     * lays them out; a move's banner, which wraps over the whole width, and
     * the weather stay on the glass. */
    int eyeShift = sFieldBanner ? 0 : (int)CtrVoxel_OverlayParallax();

    sPriorityMask = SLOTS_ALL;
    sParallax = 0;
    sLayerOrigin = 0.0f;
    sLayerShift = 0.0f;
    sClipX0 = sClipY0 = 0;
    sClipX1 = CTR_GAME_WIDTH;
    sClipY1 = CTR_GAME_HEIGHT;
    /* A pass over all 128 sprites, four priorities each, costs even with
     * none of its sprites up (0.1 ms each in Azahar): only those with some. */
    bool weather = (sVoxelWeatherOam[0] | sVoxelWeatherOam[1] | sVoxelWeatherOam[2] | sVoxelWeatherOam[3]
                    | sFogOam[0] | sFogOam[1] | sFogOam[2] | sFogOam[3]) != 0;
    bool screen = ((sScreenOam[0] & ~sVoxelWeatherOam[0]) | (sScreenOam[1] & ~sVoxelWeatherOam[1])
                   | (sScreenOam[2] & ~sVoxelWeatherOam[2]) | (sScreenOam[3] & ~sVoxelWeatherOam[3])) != 0;

    if (sSwoopFieldObjs)
    {
        /* The object events as the 2D field draws them, under everything
         * else of the overlay (the swoop, with the cards squashed away). */
        sVoxelObjPass = VOXEL_OBJ_FIELD;
        Layers(1u << 4);
        BlendForget();
    }
    sVoxelObjPass = VOXEL_OBJ_WEATHER;
    /* With the blending: clouds and fog are semi-transparent sprites, and
     * drawn without it they hid the world under them. */
    if (weather) Layers(1u << 4 | 32u);
    BlendForget();
    /* Text windows and prompts must stay above the precipitation. */
    sLayerOrigin = (sFieldBanner ? 0.0f : CTR_FIELD_UI_SHIFT) + (float)eyeShift;
    Layers(1u << 0);
    sLayerOrigin = 0.0f;
    sOverlayEyeShift = eyeShift;
    sVoxelObjPass = VOXEL_OBJ_SCREEN;
    if (screen) Layers(1u << 4);
    sVoxelObjPass = VOXEL_OBJ_NONE;
    sOverlayEyeShift = 0;
}

/*
 * The world to one screen target: the logical surface cleared and the world
 * drawn into it from `eye` (-1: the middle camera; 0, 1: one eye of the
 * pair), then the picture to the target with its tilt-shift, glow and the
 * dark of a cave, and the game's text and weather over it. With the slider
 * up this runs once per eye, the logical surface and the bloom's target
 * reused in turn: the GPU takes the frame's commands in order, so the second
 * eye's clear comes after the first eye's picture has been copied out. What
 * is laid over the world is placed as that eye saw it (CtrVoxel_Draw sets
 * the eye the voxel module projects by); the weather is the same in both
 * eyes, on the glass, and the text in front of the nearest world
 * (ComposeVoxelOverlay).
 */
static void RenderVoxelTo(C3D_RenderTarget *target, int eye, uint32_t clear, bool first)
{
    const Tex3DS_SubTexture logical = {CTR_GAME_WIDTH, CTR_GAME_HEIGHT, 0, 1,
        CTR_GAME_WIDTH / 512.0f, 1 - CTR_GAME_HEIGHT / 256.0f};
    float bloom;

    /* C2D_TargetClear clears colour and depth, which the 3D pass needs. */
    C2D_TargetClear(sLogical, clear);
    CtrVoxel_Draw(sLogical, eye);
    sSceneOn = NULL; /* drawn on with C3D_FrameDrawOn, behind SceneOn's back */

    /* Finish the world before sampling it. UI is drawn on top after blur.
     * The GPU draws it from here while the rest is recorded (GpuStartEarly):
     * until FrameEnd it waited, and every field frame took CPU + GPU, its
     * 6-9 ms of world after the whole present (DROP ... gpu late). The
     * second eye's world is split off the same way, for the same reason. */
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    if (first)
    {
        sGpuStage = CTR_STAGE_WORLD;
        GpuStartEarly(CTR_EARLY_VOXEL);
    }
    else
    {
        GpuSplit();
        sGpuStage = CTR_STAGE_WORLD;
    }

    /* Back to the 2D compositor, which assumes its own program and no depth. */
    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    BlendForget();
    bloom = sBloom != NULL ? CtrVoxel_Bloom() : 0.0f;
    if (bloom > 0.005f)
        VoxelBloomPrepare();
    C2D_TargetClear(target, C2D_Color32(0, 0, 0, 255));
    SceneOn(target);
    C2D_ViewReset();
    Blend(5, false, false);
    C2D_DrawImageAt((C2D_Image){&sSurface, &logical}, 0, 0, 0, NULL, 1, 1);
    if (CtrSettings_VoxelBlur())
        VoxelDiorama();
    if (bloom > 0.005f)
        VoxelBloomCompose(bloom);
    VoxelGloom();
    VoxelFlashLight();
    if (!(Reg(0) & 128)) ComposeVoxelOverlay();
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    GpuSplit();
    sGpuStage = CTR_STAGE_POST;
}

/*
 * The voxel path of the single Citro3D frame opened by CtrVideo_Present.
 * Same shape as RenderEye: compose the logical surface, split, blit it to the
 * screen. What changes is who composes it. `stereo` is the 3D slider, 0 for
 * one picture; with it up the world is drawn to each eye from its own camera
 * (CtrVoxel_SetStereo was given the slider before the update).
 */
/*
 * The swoop's crossfade (voxel_swoop.c): the 2D field, whole, composed as the
 * battle transition composes it (RenderTransition), laid over the finished
 * voxel picture at `alpha`. The text layer is left out: the voxel overlay
 * has drawn it. Into the logical surface, which the voxel picture has left.
 */
static unsigned DepthPlanes(void);
static unsigned BandMask(unsigned band);
static float BandDepth(unsigned band, unsigned count);

/* The whole logical picture laid over the target, moved by a fraction of a
 * pixel with the linear filter on: one tap of a blur (see DioramaTap). */
static void SwoopBlurTap(float dx, float dy, float alpha)
{
    const Tex3DS_SubTexture region = {CTR_GAME_WIDTH - 2, CTR_GAME_HEIGHT - 2,
        (1.0f + dx) / 512.0f, 1.0f - (1.0f + dy) / 256.0f,
        (1.0f + dx + CTR_GAME_WIDTH - 2) / 512.0f, 1.0f - (1.0f + dy + CTR_GAME_HEIGHT - 2) / 256.0f};
    C2D_ImageTint tint;

    C2D_AlphaImageTint(&tint, alpha);
    C2D_DrawImageAt((C2D_Image){&sSurface, &region}, 1, 1, 0, &tint, 1, 1);
}

/* The logical picture softened over the target: four taps round each pixel
 * at `blur` (0..1) of their full strength. */
static void SwoopBlur(float blur)
{
    float wide = blur > 1.0f ? blur - 1.0f : 0.0f;

    if (blur > 1.0f) blur = 1.0f;
    SurfaceFilter(GPU_LINEAR);
    SwoopBlurTap(0.5f, 0.5f, 0.55f * blur);
    SwoopBlurTap(-0.5f, -0.5f, 0.45f * blur);
    SwoopBlurTap(1.5f, -0.5f, 0.35f * blur);
    SwoopBlurTap(-1.5f, 0.5f, 0.30f * blur);
    /* Past 1: wider taps on top, for a softer picture still. */
    if (wide > 0.0f)
    {
        SwoopBlurTap(2.5f, 1.5f, 0.35f * wide);
        SwoopBlurTap(-2.5f, -1.5f, 0.35f * wide);
        SwoopBlurTap(1.5f, -2.5f, 0.30f * wide);
        SwoopBlurTap(-1.5f, 2.5f, 0.30f * wide);
    }
    SurfaceFilter(GPU_NEAREST);
}

static void Field2DOver(float alpha, float blur, uint32_t clear, C3D_RenderTarget *target, float parallax)
{
    const Tex3DS_SubTexture logical = {CTR_GAME_WIDTH, CTR_GAME_HEIGHT, 0, 1,
        CTR_GAME_WIDTH / 512.0f, 1 - CTR_GAME_HEIGHT / 256.0f};
    C2D_ImageTint tint;

    /* The voxel picture, still in the logical surface, softened in place
     * on the target before the 2D field goes over it. */
    if (blur > 0.005f)
    {
        C2D_Prepare();
        C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
        BlendForget();
        SceneOn(target);
        C2D_ViewReset();
        Blend(5, false, false);
        SwoopBlur(blur);
        C2D_Flush();
        C3D_FrameSplit(0);
    }

    /* The eye's parallax, as the field's depth planes give its layers
     * (DepthPlanes, BlitBands: the planes it would cut, the furthest at the
     * screen and the rest forward by whole pixels, the backgrounds scrolled
     * together on one plane): composed flat, every layer jumped by it on
     * the frame the swoop began and the frame it ended. Without planes the
     * field is composed per priority (RenderEye), and so is this. */
    unsigned count = parallax != 0.0f && sBandsReady ? DepthPlanes() : 0;

    sParallax = count >= 2 ? 0.0f : parallax;
    sLayerShift = 0;
    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    BlendForget();
    ColourClear(sLogical, clear);
    SceneOn(sLogical);
    Blend(5, false, false);
    StageUnderlay();
    /* As RenderEye composes the 2D field, without its text layer. */
    sLayerExclude = 1;
    if (!(Reg(0) & 128))
    {
        if (count < 2)
            Compose();
        else
            for (int band = (int)count - 1; band >= 0; --band)
            {
                sPriorityMask = BandMask(band);
                sLayerOrigin = parallax * BandDepth(band, count);
                Compose();
            }
        sPriorityMask = SLOTS_ALL;
        sLayerOrigin = 0.0f;
    }
    sLayerExclude = 0;
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    C3D_FrameSplit(0);
    BlendForget();
    SceneOn(target);
    C2D_ViewReset();
    Blend(5, false, false);
    C2D_AlphaImageTint(&tint, alpha > 1.0f ? 1.0f : alpha);
    C2D_DrawImageAt((C2D_Image){&sSurface, &logical}, 0, 0, 0, &tint, 1, 1);
    /* ... and softened likewise, as far as it is there. */
    if (blur > 0.005f) SwoopBlur(blur * (alpha > 1.0f ? 1.0f : alpha));
    C2D_Flush();
    C3D_FrameSplit(0);
}

static void RenderVoxel(uint32_t clear, float stereo)
{
    /* The brightness effect (BLDY) on the field's backgrounds and sprites. */
    unsigned control = Reg(0x50), effect = (control >> 6) & 3;
    float bright = effect >= 2 ? Min(Reg(0x54) & 31, 16) / 16.0f : 0.0f;
    VoxelSwoopFrame swoop;

    VoxelSwoop_Frame(&sSwoop, &swoop);
    /* Arriving from the 2D field, the world's light rings down from white -
     * unless the game has an effect of its own on. */
    if (swoop.whiteout > 0.0f && bright <= 0.0f)
        CtrVoxel_SetBrightness(swoop.whiteout, swoop.whiteout, true);
    else
        CtrVoxel_SetBrightness((control & 0x0e) ? bright : 0.0f, (control & 0x10) ? bright : 0.0f,
                               effect == 2);
    if (stereo > 0.0f)
    {
        uint64_t eyesStart = svcGetSystemTick();

        RenderVoxelTo(sTop, 0, clear, true);
        RenderVoxelTo(sTopRight, 1, clear, false);
        sStats.stereoEyesMs = (svcGetSystemTick() - eyesStart) * 1000.0f / SYSCLOCK_ARM11;
    }
    else
        RenderVoxelTo(sTop, -1, clear, true);
}
