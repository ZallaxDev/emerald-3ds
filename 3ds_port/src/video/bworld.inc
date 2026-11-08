/*
 * Part of 3ds_video.c. The battle over the voxel world: curtain, shadows, the
 * intro's blur and bars, and RenderBattleWorld.
 */

/*
 * The 3D battle: the battle in front of the voxel world (CtrSettings_
 * VoxelBattle). The world is the battle's scenery - drawn where the GBA draws
 * BG3, from the stage the voxel module chose near the player - and the rest
 * of the battle is the game's own picture over it, composed as the 2D battle
 * is (RenderBattleScene and the text box) into the logical surface cleared
 * transparent, once the world has gone to the screen with its blur and
 * glow. Whatever the GBA does to its scenery the world takes: BG3's
 * brightness (a move darkening the field), its palette fading
 * (VoxelWorld_ScreenFade), its scroll (a move shaking it,
 * CtrVoxel_SetBattleFrame) and the windows that hide it (the intro's curtain).
 */

/* Where the windows hide BG3 - the intro's curtain opening from the middle -
 * the world is hidden too: the backdrop is there. */
static void BattleWorldCurtain(uint32_t backdrop)
{
    int rects[WINDOW_RECTS][4];
    unsigned masks[WINDOW_RECTS], count;

    if (!(Reg(0) & 0x6000))
        return;
    count = WindowPartition(VIEW_TOP, VIEW_BOTTOM, rects, masks);
    for (unsigned i = 0; i < count; ++i)
    {
        float x0, y0, x1, y1;

        if (masks[i] & 8)
            continue;
        x0 = (rects[i][0] + sViewX) * sZoom + sOffX;
        x1 = (rects[i][2] + sViewX) * sZoom + sOffX;
        y0 = (rects[i][1] + sViewY) * sZoom + sOffY;
        y1 = (rects[i][3] + sViewY) * sZoom + sOffY;
        C2D_DrawRectSolid(x0, y0, 0, x1 - x0, y1 - y0, backdrop);
    }
}

/*
 * The GBA's scenery has a base under each side, which the world has not: a
 * soft shadow on the ground under each battler and trainer instead, the dark
 * blue of the world's own shadows fading out from the middle. One small
 * texture (linear memory, made once), stretched to each shadow.
 */
#define BATTLE_SHADOW_W 64
#define BATTLE_SHADOW_H 16
#define BATTLE_SHADOW_ALPHA 0.50f
static C3D_Tex sBattleShadowTex;
static bool sBattleShadowFailed;

static bool BattleShadowTexture(void)
{
    uint32_t *texels;

    if (sBattleShadowTex.data || sBattleShadowFailed)
        return sBattleShadowTex.data != NULL;
    if (!C3D_TexInit(&sBattleShadowTex, BATTLE_SHADOW_W, BATTLE_SHADOW_H, GPU_RGBA8))
    {
        memset(&sBattleShadowTex, 0, sizeof(sBattleShadowTex));
        sBattleShadowFailed = true;
        return false;
    }
    C3D_TexSetFilter(&sBattleShadowTex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&sBattleShadowTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    texels = sBattleShadowTex.data;
    for (unsigned y = 0; y < BATTLE_SHADOW_H; ++y)
        for (unsigned x = 0; x < BATTLE_SHADOW_W; ++x)
        {
            float u = (x + 0.5f) / (BATTLE_SHADOW_W / 2) - 1.0f, v = (y + 0.5f) / (BATTLE_SHADOW_H / 2) - 1.0f;
            float d = 1.0f - (u * u + v * v), a = d > 0.0f ? BATTLE_SHADOW_ALPHA * d * sqrtf(d) : 0.0f;

            texels[CtrVideo_Texel(x, y, BATTLE_SHADOW_W)] = 8u << 24 | 20u << 16 | 40u << 8
                                                           | (uint32_t)(a * 255.0f + 0.5f);
        }
    TexFilled(&sBattleShadowTex);
    return true;
}

static void BattleWorldShadows(void)
{
    static const Tex3DS_SubTexture whole = {BATTLE_SHADOW_W, BATTLE_SHADOW_H, 0, 1, 1, 0};
    VoxelBattleShadow shadows[4];
    unsigned count = VoxelBattle_Shadows(shadows, 4);

    if (count == 0 || !BattleShadowTexture())
        return;
    for (unsigned i = 0; i < count; ++i)
    {
        float x = (shadows[i].x + sViewX) * sZoom + sOffX, y = (shadows[i].y + sViewY) * sZoom + sOffY;
        float rx = shadows[i].rx * sZoom, ry = shadows[i].ry * sZoom;

        C2D_DrawImageAt((C2D_Image){&sBattleShadowTex, &whole}, x - rx, y - ry, 0, NULL,
                        rx * 2.0f / BATTLE_SHADOW_W, ry * 2.0f / BATTLE_SHADOW_H);
    }
}

/* BG3's scroll from rest, GBA pixels, either way round its 256-pixel turn:
 * the scenery rests at 0 (or 256, the same picture). */
static float BattleScenerySway(unsigned reg)
{
    return (float)((int)((Reg(reg) + 128) & 255) - 128);
}

/*
 * The 3D battle's entrance (CtrVoxel_BattleIntro): while the camera flies in
 * from the field the world is drawn as a shot - a zoom blur and speed lines
 * as fast as it travels, a dark indigo vignette as high as it climbs, cinema
 * bars framing it, a white flash and a burst of glow where it lands. A few
 * quads and, while it travels, two copies of the world surface: about what
 * the diorama blur costs, for a second and a half, and nothing once landed.
 * All of it is the world's, under the battle's own picture.
 */
/* The world's lowest line on screen: the text box covers the rest. */
#define BATTLE_INTRO_FLOOR (CTR_BATTLE_Y + BATTLE_BAND_TOP)
#define BATTLE_INTRO_BAR 22.0f      /* each cinema bar, closed */
#define BATTLE_INTRO_BAR_EDGE 10.0f /* its soft edge */
#define BATTLE_INTRO_STREAKS 12u

/* One copy of the world magnified about the target (the screen's middle,
 * where the camera looks), see-through: a step of the zoom blur. */
static void BattleIntroZoomTap(float scale, float alpha)
{
    const Tex3DS_SubTexture region = {CTR_GAME_WIDTH - 2, CTR_GAME_HEIGHT,
        1.0f / 512.0f, 1.0f, (CTR_GAME_WIDTH - 1) / 512.0f, 1.0f - CTR_GAME_HEIGHT / 256.0f};
    const float cx = CTR_GAME_WIDTH * 0.5f, cy = CTR_GAME_HEIGHT * 0.5f;
    C2D_ImageTint tint;

    C2D_AlphaImageTint(&tint, alpha);
    C2D_DrawImageAt((C2D_Image){&sSurface, &region}, cx + (1.0f - cx) * scale, cy - cy * scale, 0,
                    &tint, scale, scale);
}

/* Before the bloom is added: the zoom blur, from the world surface itself. */
static void BattleIntroBlur(const CtrVoxelBattleIntro *intro)
{
    float s = intro->speed;

    if (s < 0.04f || (Reg(0) & 128))
        return;
    Blend(5, false, false);
    SurfaceFilter(GPU_LINEAR);
    BattleIntroZoomTap(1.0f + 0.035f * s, 0.50f * s);
    BattleIntroZoomTap(1.0f + 0.080f * s, 0.32f * s);
    SurfaceFilter(GPU_NEAREST);
}

/* Over the world, under the curtain: the speed lines and the vignette. */
static void BattleIntroAtmosphere(const CtrVoxelBattleIntro *intro)
{
    const float w = CTR_GAME_WIDTH, bottom = BATTLE_INTRO_FLOOR;
    float shade = fmaxf(0.60f * intro->flight, 0.30f * intro->bars);

    if (intro->speed > 0.04f)
    {
        /* Streaks racing right to left, none across the middle band where
         * the battlers stand; each its own height, length and pace. */
        for (unsigned i = 0; i < BATTLE_INTRO_STREAKS; ++i)
        {
            uint32_t h = (i + 1) * 2654435761u;
            float y = 6.0f + (float)((h >> 8) % (unsigned)(bottom - 12.0f));
            float length = 70.0f + (float)((h >> 16) % 110u);
            float pace = 26.0f + (float)((h >> 20) % 18u);
            float x = w - fmodf(intro->time * pace + (float)(h % 400u), w + length);
            float alpha = intro->speed * (0.35f + 0.35f * (float)((h >> 4) & 3) / 3.0f);
            uint32_t head = C2D_Color32(225, 236, 255, (uint8_t)(alpha * 255.0f));
            uint32_t tail = C2D_Color32(225, 236, 255, 0);

            if (fabsf(y - bottom * 0.55f) < 24.0f)
                y = y < bottom * 0.55f ? y - 24.0f : y + 24.0f;
            C2D_DrawRectangle(x, y, 0, length, (h & 16) ? 2.0f : 1.0f, head, tail, head, tail);
        }
    }
    if (shade > 0.01f)
    {
        uint32_t dark = C2D_Color32(8, 6, 34, (uint8_t)(fminf(shade, 1.0f) * 230.0f));
        uint32_t none = C2D_Color32(8, 6, 34, 0);
        float band = 64.0f, side = 96.0f;
        /* Down to the text box, which covered the rest; the HD one floats
         * with the scene showing round it, and is not there yet. */
        float low = sStats.frames - sBoxShown <= 1 || sBoxHidden ? CTR_GAME_HEIGHT : bottom;

        C2D_DrawRectangle(0, 0, 0, w, band, dark, dark, none, none);
        C2D_DrawRectangle(0, low - band, 0, w, band, none, none, dark, dark);
        C2D_DrawRectangle(0, 0, 0, side, low, dark, none, dark, none);
        C2D_DrawRectangle(w - side, 0, 0, side, low, none, dark, none, dark);
    }
}

/* Over everything of the world: the landing's flash. */
static void BattleIntroFlash(const CtrVoxelBattleIntro *intro)
{
    if (intro->impact > 0.01f)
        C2D_DrawRectSolid(0, 0, 0, CTR_GAME_WIDTH, CTR_GAME_HEIGHT,
                          C2D_Color32(255, 250, 235, (uint8_t)(fminf(intro->impact, 1.0f) * 170.0f)));
}

/*
 * The cinema bars, over everything on the top screen - the battlers and the
 * text box too: drawn last into each finished picture (one eye's, or both at
 * the screen's depth), the bottom one on the screen's own edge. Four quads.
 */
static void BattleIntroBars(const CtrVoxelBattleIntro *intro, bool stereo)
{
    const float w = CTR_GAME_WIDTH, h = CTR_GAME_HEIGHT;
    float bar, edge;
    uint32_t black = C2D_Color32(0, 0, 0, 255), none = C2D_Color32(0, 0, 0, 0);

    if (intro->bars <= 0.01f || (Reg(0) & 128))
        return;
    bar = BATTLE_INTRO_BAR * intro->bars;
    edge = BATTLE_INTRO_BAR_EDGE * intro->bars;
    /* The right eye was drawn last: no target switch before it. */
    for (int eye = stereo ? 1 : 0; eye >= 0; --eye)
    {
        if (stereo)
        {
            /* Whatever RenderBattleStereo began last, flushed and switched
             * properly (SceneOn), as any change of target must be. */
            sSceneOn = NULL;
            SceneOn(eye ? sTopRight : sTop);
        }
        C2D_ViewReset();
        BlendForget();
        Blend(5, false, false);
        C2D_DrawRectSolid(0, 0, 0, w, bar, black);
        C2D_DrawRectangle(0, bar, 0, w, edge, black, black, none, none);
        C2D_DrawRectSolid(0, h - bar, 0, w, bar, black);
        C2D_DrawRectangle(0, h - bar - edge, 0, w, edge, none, none, black, black);
        C2D_Flush();
    }
}

/* What the world's picture is finished with this frame (BattleWorldPicture),
 * for SceneFillPay to draw it again. */
static struct { CtrVoxelBattleIntro intro; float bloom; uint32_t clear; } sWorldPicture;

/*
 * The voxel world from the logical surface into `world` with its blur, glow,
 * shadows, the intro's effects and the curtain. `kept`: into the scene
 * surface left unfilled for the scenery beside it (RenderBattleWorld). The
 * copy is then drawn as its blend over the fill comes out - colour times
 * alpha, alpha as it is, nothing where alpha is 0 - without reading what
 * the columns held (the world of a frame before, whose alpha the blur and
 * glow left below one); and nothing is drawn past the world's columns: the
 * shadows, the speed lines and the zoom blur reach past the picture, where
 * the scenery's compose drew a clear over them, and the kept scenery is
 * there now. The columns between are no one's to sample while it is kept.
 */
static void BattleWorldPicture(C3D_RenderTarget *world, bool kept)
{
    const Tex3DS_SubTexture logical = {CTR_GAME_WIDTH, CTR_GAME_HEIGHT, 0, 1,
        CTR_GAME_WIDTH / 512.0f, 1 - CTR_GAME_HEIGHT / 256.0f};
    const CtrVoxelBattleIntro *intro = &sWorldPicture.intro;

    SceneOn(world);
    C2D_ViewReset();
    if (kept)
    {
        C2D_Flush();
        C3D_SetScissor(GPU_SCISSOR_NORMAL, 0, 0, (unsigned)sSceneColumn, SCENE_H);
    }
    Blend(5, false, false);
    if (kept)
    {
        C2D_Flush();
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ZERO, GPU_ONE, GPU_ZERO);
    }
    C2D_DrawImageAt((C2D_Image){&sSurface, &logical}, 0, 0, 0, NULL, 1, 1);
    if (kept)
    {
        /* The effects over it with Blend's own state, as below the fill. */
        C2D_Flush();
        BlendForget();
        Blend(5, false, false);
    }
    if (CtrSettings_VoxelBlur())
        VoxelDiorama();
    BattleIntroBlur(intro);
    if (sWorldPicture.bloom > 0.005f)
        VoxelBloomCompose(sWorldPicture.bloom);
    if (!(Reg(0) & 128))
    {
        /* Blended over the world, not added as the glow was. */
        Blend(5, false, false);
        BattleWorldShadows();
        BattleIntroAtmosphere(intro);
        BattleWorldCurtain(sWorldPicture.clear);
        BattleIntroFlash(intro);
    }
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
}

/*
 * The fill RenderBattleWorld left out for a kept scenery, paid: the scene
 * surface filled whole, and the world's picture drawn into it again as it
 * was after the fill there (the bloom's small target still holds this
 * frame's glow). Before anything sampled the surface - the eyes and the
 * slots come after the scenery's compose - so the frame comes out as one
 * that filled it in RenderBattleWorld; the scenery is composed again by the
 * caller. No split after the picture: what follows is drawn into the same
 * surface, and its compose seals the list (SceneComposeInto).
 */
static void SceneFillPay(void)
{
    sSceneFillOwed = false;
    C2D_Flush();
    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    BlendForget();
    ColourClear(sScene, 0);
    sSceneOn = NULL;
    sSceneCached = false;
    BattleWorldPicture(sScene, false);
}

/*
 * Whether the scenery composed beside the world the frame before is this
 * frame's too, as BattleSceneCompose will find it (the same key, VRAM and
 * measures): asked before the fill, which the world's picture is drawn
 * after. Only for a text-mode scenery the compose can be cut for without
 * doubt (BattleSceneStrip; MonoCutUnsure), with no OBJ window in it, and not
 * when the last frame's slots went into the surface's columns
 * (sSceneWorldLent). The window is taken as left out of the scenery, as
 * RenderBattleStereo first takes it; a frame that ends up composed whole is a
 * wrong guess like any other. A wrong guess is paid for exactly
 * (SceneFillPay). Never with nocache.txt bit 1, as every kept scene.
 */
static bool BattleWorldSceneHeld(void)
{
    unsigned layers = BattleSceneOut(16u);
    int left, top, width, height;

#ifdef CTR_AUDIT_BITS
    if (CtrDumpNoCache()) return false;
#endif
    /* Nothing kept to lose. */
    if (!sSceneCached || sSceneEmpty) return false;
    if (sSceneWorldLent)
    {
        ++sWorldScene.slots;
        sSceneWhy |= SCENE_WHY(SLOTS);
        return false;
    }
    if ((Reg(0) & 7) || (sObjWin.ready && !SceneWindowApart()) || MonoCutUnsure((Reg(0) >> 8) & 14u & ~layers))
    {
        ++sWorldScene.other;
        sSceneWhy |= SCENE_WHY(PATH);
        return false;
    }
    /* Looked at as BattleSceneCompose will. */
    layers |= SceneWindowHidden();
    BattleSceneGeometry(BATTLE_STEREO_MARGIN, &left, &top, &width, &height);
    if (SceneViewKey(left, top, width, height, layers) != sSceneCacheKey)
    {
        ++sWorldScene.key;
        SceneKeyParted(&sSceneCacheParts, &sSceneKeyLast);
        return false;
    }
    if (!SceneCacheHolds(layers))
    {
        ++sWorldScene.vram;
        SceneHeldWhy(true, sHoldWhy);
        return false;
    }
    if (!StillHolds(sSceneCacheStill))
    {
        ++sWorldScene.still;
        sSceneWhy |= SCENE_WHY(STILL);
        return false;
    }
    return true;
}

/*
 * With the 3D slider up (`slider` > 0, the scene surface there), the world
 * is the battle's scenery as the 2D one is (BATTLE_BACK_DEPTH): finished once
 * with its blur, glow, shadows and curtain into the scene surface, then
 * copied into each eye behind the screen, the battle's sprites in front of
 * it (RenderBattleStereo). The world is drawn once; the GPU starts on it
 * while the rest of the frame is recorded.
 */
static void RenderBattleWorld(uint32_t clear, float slider)
{
    const Tex3DS_SubTexture logical = {CTR_GAME_WIDTH, CTR_GAME_HEIGHT, 0, 1,
        CTR_GAME_WIDTH / 512.0f, 1 - CTR_GAME_HEIGHT / 256.0f};
    unsigned control = Reg(0x50), effect = (control >> 6) & 3;
    float bright = effect >= 2 ? Min(Reg(0x54) & 31, 16) / 16.0f : 0.0f;
    float bloom;
    bool stereo = slider > 0.0f && sScene;
    /* With the slider up the picture goes into the scene surface's first
     * columns, where RenderBattleStereo copies it behind each eye. */
    C3D_RenderTarget *world = stereo ? sScene : sTop;

    sSceneColumn = stereo ? WORLD_COLUMNS + WORLD_GAP : 0;
    CtrVoxelBattleIntro intro;

    /* The world is BG3: its brightness is BG3's. */
    PORT_PROF_BEGIN(bwWorld);
    CtrVoxel_SetBrightness((control & 0x08) ? bright : 0.0f, 0.0f, effect == 2);
    C2D_TargetClear(sLogical, clear);
    CtrVoxel_Draw(sLogical, -1);
    sSceneOn = NULL; /* drawn on with C3D_FrameDrawOn, behind SceneOn's back */
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    GpuSplit();
    sGpuStage = CTR_STAGE_WORLD;
    PORT_PROF_END(bwWorld, PORT_PROF_BW_WORLD);
    PORT_PROF_BEGIN(bwPost);

    /* The world to the screen, as in the field. */
    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    BlendForget();
    CtrVoxel_BattleIntro(&intro);
    bloom = sBloom != NULL ? CtrVoxel_Bloom() : 0.0f;
    /* The landing glows: a burst of the bloom dying away with its flash. */
    if (sBloom != NULL && intro.impact > 0.01f)
        bloom = fminf(bloom + 0.7f * intro.impact, 1.0f);
    if (bloom > 0.005f)
        VoxelBloomPrepare();
    sWorldPicture.intro = intro;
    sWorldPicture.bloom = bloom;
    sWorldPicture.clear = clear;
    sSceneFillOwed = false;
    if (stereo)
    {
        /*
         * The whole surface, as a memory fill: a drawn clear of the slots'
         * columns did not land on hardware (a slot composed over what was
         * there frames before came back as a lit copy of a Pokémon at a
         * battle's end), so nothing of a frame before is left for a slot to
         * sample. The fill drops the scenery beside the world too, composed
         * again then. Not while that scenery holds (BattleWorldSceneHeld):
         * the fill is owed instead, and paid if this frame's slots take the
         * surface's columns after all (RenderBattleStereo, SceneFillPay).
         * The OBJ window as that function first takes it, for the checks.
         */
        sObjWinSlotted = sObjWin.ready;
        sSceneFillOwed = BattleWorldSceneHeld();
        if (!sSceneFillOwed)
        {
            ColourClear(sScene, 0);
            sSceneOn = NULL;
            sSceneCached = false;
        }
        else
        {
            /* The split the fill made, kept: the glow just drawn into its
             * target is sampled by the picture (VoxelBloomCompose), and
             * rendering to a texture and then sampling it needs a command
             * split. Before GpuStartEarly: no cache flush of its own. */
            C2D_Flush();
            GpuSplit();
            sSceneOn = NULL;
        }
    }
    else
        C2D_TargetClear(world, C2D_Color32(0, 0, 0, 255));
    BattleWorldPicture(world, sSceneFillOwed);
    GpuSplit();
    sGpuStage = CTR_STAGE_POST;
    PORT_PROF_END(bwPost, PORT_PROF_BW_POST);

    GpuStartEarly(CTR_EARLY_BATTLE);
    PORT_PROF_BEGIN(bwScene);
    if (stereo)
    {
        RenderBattleStereo(clear, slider, &sSceneTex);
        BattleIntroBars(&intro, true);
        PORT_PROF_END(bwScene, PORT_PROF_BW_SCENE);
        return;
    }

    /* The battle's own picture over it: the logical surface again, cleared
     * transparent, composed as RenderEye composes the 2D battle. */
    sParallax = 0.0f;
    sLayerShift = 0.0f;
    BlendForget();
    C2D_TargetClear(sLogical, 0);
    if (sScene) RenderBattleScene();
    SceneOn(sLogical);
    Blend(5, false, false);
    if (sScene) HudDrawCrisp(HUD_FLAT);
    /* After a scene composed on its own only the text box is left; without
     * its surface, everything but what the world stands in for. */
    sLayerExclude = sScene ? 63 & ~(1u | 32u | BattleBoxLayers()) : sWorldLayers;
    {
        PORT_PROF_BEGIN(bmText);
        if (!(Reg(0) & 128)) Compose();
        PORT_PROF_END(bmText, PORT_PROF_BM_TEXT);
    }
    sLayerExclude = 0;
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    GpuSplit();
    sGpuStage = CTR_STAGE_LOGICAL;

    BlendForget();
    SceneOn(sTop);
    C2D_ViewReset();
    Blend(5, false, false);
    C2D_DrawImageAt((C2D_Image){&sSurface, &logical}, 0, 0, 0, NULL, 1, 1);
    C2D_Flush();
    BattleIntroBars(&intro, false);
    PORT_PROF_END(bwScene, PORT_PROF_BW_SCENE);
}
