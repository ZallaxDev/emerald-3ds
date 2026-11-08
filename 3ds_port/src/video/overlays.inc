/*
 * Part of 3ds_video.c. What goes over the finished picture: the FPS, the HOME
 * notice, the poison veil, the speed badge, the swoop's light; and the held
 * top screen.
 */

#ifndef CTR_SHOW_FPS
#define CTR_SHOW_FPS 1
#endif
#if CTR_SHOW_FPS
/*
 * The FPS counter (SHOW_FPS=1), shown when the options turn it on
 * (CtrSettings_ShowFps): a 3x5 pixel font, each pixel a 1x1 solid
 * rectangle, over a translucent box in the top-left corner. Drawn last, over
 * whatever the frame composed, and at zero parallax in both eyes.
 */
#define FPS_PIXEL 1.0f
#define FPS_ADVANCE (4 * FPS_PIXEL)

static const char *const sFpsGlyphs[] =
{
    "111101101101111", "010110010010111", "111001111100111", "111001111001111",
    "101101111001001", "111100111001111", "111100111101111", "111001001001001",
    "111101111101111", "111101111001111",
    "111100111100100", /* F */
    "111101111100100", /* P */
    "001001010100100", /* / */
};
#endif

/* Game frames a second, hidden ones included (CtrPlatform_FrameCount): the
 * game's speed, beside the pictures a second the counter always showed.
 * With real-time pacing (3ds_platform.c) the two differ on a slow frame. */
static float sGameFps;
static uint64_t sGameFpsMark;

/*
 * The field's poison step (fldeff_misc.c, Task_FieldPoisonEffect) is a mosaic
 * that swells to 10 pixels and back, and the mosaic is not composed here: the
 * step had a sound and nothing to see. It is a violet veil over the view
 * instead, as deep as the mosaic is coarse - on whatever drew the field, the
 * 2D layers or the voxel world, which reads no register at all.
 */
static unsigned PoisonVeil(void)
{
    unsigned size = Reg(0x4c) & 15;

    return size > 10 ? 150 : size * 15;
}

/*
 * The HOME button refused in a local wireless session (3ds_platform.c): the
 * system draws nothing, so the game shows it was pressed - a house crossed
 * out, in the top screen's top right corner, for a second and a half.
 */
static uint32_t sNoHomeUntil;

void CtrVideo_ShowNoHome(void)
{
    sNoHomeUntil = sStats.frames + 90;
}

static void DrawNoHome(C3D_RenderTarget *target)
{
    const float x = CTR_GAME_WIDTH - 34, y = 6, w = 28;
    const u32 white = C2D_Color32(255, 255, 255, 255), red = C2D_Color32(230, 40, 40, 255);

    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    BlendForget();
    SceneOn(target);
    C2D_ViewReset();
    Blend(5, false, false);
    C2D_DrawCircleSolid(x + w / 2, y + w / 2, 0, w / 2, C2D_Color32(0, 0, 0, 170));
    /* The house: its roof and its walls, a door cut out of them. */
    C2D_DrawTriangle(x + 6, y + 14, white, x + 14, y + 6, white, x + 22, y + 14, white, 0);
    C2D_DrawRectSolid(x + 8, y + 14, 0, 12, 8, white);
    C2D_DrawRectSolid(x + 12, y + 17, 0, 4, 5, C2D_Color32(0, 0, 0, 255));
    /* Crossed out. */
    C2D_DrawLine(x + 6, y + 6, red, x + 22, y + 22, red, 3, 0);
    C2D_Flush();
}

/*
 * The swoop's light over a finished picture of the top screen, the same in
 * both eyes (on the glass): the flat flash and a radial bloom from the middle
 * as it dies away. Four triangles and a rectangle at most, nothing allocated.
 */
static void DrawSwoopLight(C3D_RenderTarget *target, const VoxelSwoopFrame *fx)
{
    const float w = CTR_GAME_WIDTH, h = CTR_GAME_HEIGHT, cx = w * 0.5f, cy = h * 0.5f;
    uint32_t rgb = fx->colour & 0x00FFFFFFu;

    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    BlendForget();
    SceneOn(target);
    C2D_ViewReset();
    Blend(5, false, false);
    if (fx->bloom > 0.005f)
    {
        /* A diamond, bright in the middle and gone at the screen's edges. */
        uint32_t core = rgb | ((uint32_t)(fx->bloom * 0.9f * 255.0f) << 24), edge = rgb;
        float rx = w * 0.75f, ry = h * 0.9f;

        C2D_DrawTriangle(cx, cy, core, cx - rx, cy, edge, cx, cy - ry, edge, 0);
        C2D_DrawTriangle(cx, cy, core, cx, cy - ry, edge, cx + rx, cy, edge, 0);
        C2D_DrawTriangle(cx, cy, core, cx + rx, cy, edge, cx, cy + ry, edge, 0);
        C2D_DrawTriangle(cx, cy, core, cx, cy + ry, edge, cx - rx, cy, edge, 0);
    }
    if (fx->flash > 0.005f)
        C2D_DrawRectSolid(0, 0, 0, w, h, rgb | ((uint32_t)(fminf(fx->flash, 1.0f) * 255.0f) << 24));
    C2D_Flush();
}

static void DrawPoisonVeil(C3D_RenderTarget *target, unsigned alpha)
{
    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    BlendForget();
    SceneOn(target);
    C2D_ViewReset();
    Blend(5, false, false);
    C2D_DrawRectSolid(0, 0, 0, CTR_GAME_WIDTH, CTR_GAME_HEIGHT, C2D_Color32(150, 60, 200, alpha));
}

#if CTR_SHOW_FPS
static void DrawFps(C3D_RenderTarget *target)
{
    unsigned fps = (unsigned)(sStats.fps + 0.5f), game = (unsigned)(sGameFps + 0.5f);
    unsigned glyphs[12], count = 0;
    char digits[8];
    int length = snprintf(digits, sizeof(digits), "%u/%u", fps > 999 ? 999 : fps, game > 999 ? 999 : game);

    glyphs[count++] = 10;   /* F */
    glyphs[count++] = 11;   /* P */
    glyphs[count++] = 5;    /* S, which is a 5 */
    glyphs[count++] = ~0u;  /* space */
    for (int i = 0; i < length; ++i)
        glyphs[count++] = digits[i] == '/' ? 12u : (unsigned)(digits[i] - '0');

    /* Whatever path composed the frame, the counter draws with the 2D
     * program and no depth test, as RenderVoxel's UI pass does. */
    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    BlendForget();
    SceneOn(target);
    C2D_ViewReset();
    Blend(5, false, false);
    C2D_DrawRectSolid(2, 2, 0, count * FPS_ADVANCE + FPS_PIXEL * 3, 5 * FPS_PIXEL + FPS_PIXEL * 4,
                      C2D_Color32(0, 0, 0, 160));
    for (unsigned i = 0; i < count; ++i)
    {
        if (glyphs[i] == ~0u) continue;
        const char *bits = sFpsGlyphs[glyphs[i]];
        float x = 2 + FPS_PIXEL * 2 + i * FPS_ADVANCE, y = 2 + FPS_PIXEL * 2;
        for (unsigned p = 0; p < 15; ++p)
            if (bits[p] == '1')
                C2D_DrawRectSolid(x + (p % 3) * FPS_PIXEL, y + (p / 3) * FPS_PIXEL, 0,
                                  FPS_PIXEL, FPS_PIXEL, C2D_Color32(255, 255, 255, 255));
    }
    C2D_Flush();
}
#endif

/*
 * The battle speed badge (CtrPlatform_BattleSpeedBadge): ">> X2", ">> X1.5"
 * in the top right corner, in the FPS counter's 3x5 pixel font at 3x, over a
 * translucent box. Drawn last, like the counter, at zero parallax.
 */
#define BADGE_PIXEL 3.0f
static const char *const sBadgeGlyphs[] =
{
    "111101101101111", "010110010010111", "111001111100111", "111001111001111",
    "101101111001001", "111100111001111", "111100111101111", "111001001001001",
    "111101111101111", "111101111001111",
    "101101010101101", /* X */
    "000000000000010", /* . */
};

static void DrawSpeedBadge(C3D_RenderTarget *target, int halves)
{
    char text[16];
    unsigned glyphs[6], count = 0;
    float width = 0.0f, x, y;
    const float arrows = 2 * (3 * BADGE_PIXEL + BADGE_PIXEL), pad = 2 * BADGE_PIXEL;
    const u32 white = C2D_Color32(255, 255, 255, 255);

    halves = halves < 2 ? 2 : halves > 8 ? 8 : halves;
    snprintf(text, sizeof(text), "X%d%s", halves / 2, halves & 1 ? ".5" : "");
    for (const char *c = text; *c && count < 6; ++c)
        glyphs[count++] = *c == 'X' ? 10u : *c == '.' ? 11u : (unsigned)(*c - '0');
    for (unsigned i = 0; i < count; ++i)
        width += (glyphs[i] == 11 ? 2 : 4) * BADGE_PIXEL;
    x = CTR_GAME_WIDTH - 4 - (pad + arrows + width + pad);
    y = 4;

    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    BlendForget();
    SceneOn(target);
    C2D_ViewReset();
    Blend(5, false, false);
    C2D_DrawRectSolid(x, y, 0, pad + arrows + width + pad, 5 * BADGE_PIXEL + 2 * pad, C2D_Color32(0, 0, 0, 160));
    x += pad;
    y += pad;
    /* Two arrowheads: fast forward. */
    for (int i = 0; i < 2; ++i)
    {
        float ax = x + i * 4 * BADGE_PIXEL;

        C2D_DrawTriangle(ax, y, white, ax, y + 5 * BADGE_PIXEL, white, ax + 3 * BADGE_PIXEL, y + 2.5f * BADGE_PIXEL, white, 0);
    }
    x += arrows;
    for (unsigned i = 0; i < count; ++i)
    {
        const char *bits = sBadgeGlyphs[glyphs[i]];

        for (unsigned p = 0; p < 15; ++p)
            if (bits[p] == '1')
                C2D_DrawRectSolid(x + (p % 3) * BADGE_PIXEL, y + (p / 3) * BADGE_PIXEL, 0, BADGE_PIXEL, BADGE_PIXEL, white);
        x += (glyphs[i] == 11 ? 2 : 4) * BADGE_PIXEL;
    }
    C2D_Flush();
}

/*
 * The bottom screen runs some of the game's menus without showing them (the
 * party menu, the bag): while it does, the top screen keeps the last frame it
 * presented, the world as it was when the menu opened. Nothing is rendered;
 * the frame is only paced to the display.
 */
static bool sHoldTop;

/*
 * The Pokédex draws its device over the held frame, and a screen chosen from
 * it opens with the top still held (CtrBottom_LeaveTo): the world's frame is
 * kept aside as the device comes up and put back as it goes, the top still
 * held. Each eye's colour buffer, copied whole, in linear memory meanwhile.
 */
static struct
{
    void *eye[2];
    bool kept, dexShown, failed;
} sHeldTop;

static void HeldTopForget(void)
{
    for (int i = 0; i < 2; ++i)
    {
        if (sHeldTop.eye[i]) linearFree(sHeldTop.eye[i]);
        sHeldTop.eye[i] = NULL;
    }
    sHeldTop.kept = sHeldTop.dexShown = false;
}

static u32 TargetBytes(const C3D_RenderTarget *target)
{
    const C3D_FrameBuf *fb = &target->frameBuf;

    return (u32)fb->width * fb->height * (fb->colorFmt == GPU_RB_RGBA8 ? 4 : fb->colorFmt == GPU_RB_RGB8 ? 3 : 2);
}

/* Outside a frame, the last one having left the top screen alone (held). */
static void HeldTopKeep(void)
{
    C3D_RenderTarget *eyes[2] = { sTop, sStereo ? sTopRight : NULL };

    if (!sTop) return;
    for (int i = 0; i < 2; ++i)
    {
        u32 size;

        if (!eyes[i]) continue;
        size = TargetBytes(eyes[i]);
        sHeldTop.eye[i] = linearAlloc(size);
        if (!sHeldTop.eye[i])
        {
            if (!sHeldTop.failed)
                CtrLog_Write(CTR_LOG_ERROR, "video: no linear memory to keep the held top screen (free=%lu); "
                             "the Pokédex's device stays on it", (unsigned long)linearSpaceFree());
            sHeldTop.failed = true;
            HeldTopForget();
            return;
        }
        C3D_SyncTextureCopy((u32 *)eyes[i]->frameBuf.colorBuf, 0, (u32 *)sHeldTop.eye[i], 0, size,
                            GX_TRANSFER_RAW_COPY(1));
    }
    sHeldTop.kept = true;
}

/* In a frame: the kept frame back in the top's buffers, which go out at its end. */
static void HeldTopPutBack(void)
{
    C3D_RenderTarget *eyes[2] = { sTop, sTopRight };

    for (int i = 0; i < 2; ++i)
        if (sHeldTop.eye[i] && eyes[i])
        {
            C3D_SyncTextureCopy((u32 *)sHeldTop.eye[i], 0, (u32 *)eyes[i]->frameBuf.colorBuf, 0,
                                TargetBytes(eyes[i]), GX_TRANSFER_RAW_COPY(1));
            C3D_FrameDrawOn(eyes[i]);
        }
    bool stereo = sHeldTop.eye[1] != NULL && sTopRight;
    if (stereo != sStereo) { gfxSet3D(stereo); sStereo = stereo; }
    CtrLog_Write(CTR_LOG_VIDEO, "top screen: the held frame back from under the Pokédex");
    HeldTopForget();
}

void CtrVideo_HoldTop(bool hold)
{
    if (hold != sHoldTop)
        CtrLog_Write(CTR_LOG_VIDEO, "top screen %s at frame %lu", hold ? "held" : "released",
                     (unsigned long)sStats.frames);
    sHoldTop = hold;
    if (!hold) HeldTopForget();
}

bool CtrVideo_TopHeld(void) { return sHoldTop; }
