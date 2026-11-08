/*
 * Part of 3ds_video.c. The frame composed: the field's light, Layers, the
 * windows (WindowEdge, WindowPartition), the OBJ window, Compose and
 * ComposeLines.
 */

/*
 * The circle of light of a dark cave (CtrVideo_SetFieldLight). The game opens
 * window 0 round the player line by line and leaves only the text layer and
 * the backdrop outside it; here the field keeps window 0 whole and the dark is
 * drawn over the field under the text layer instead. The 2D field draws the
 * GBA's own circle, row for row; the voxel world a soft one (VoxelFlashLight).
 */
static struct
{
    bool on;
    int x, y, radius;
    /* For the voxel light: the radius it shows, eased towards the game's,
     * the glow of Flash being used, and a frame count for the flicker. */
    float shown, burst;
    uint32_t ticks;
} sFieldLight;
/* The backdrop shown this frame, faded: what lies outside the window. */
static uint32_t sBackdropShown = 0xFF000000u;
/* The field is the voxel world this frame: its dark is VoxelFlashLight's. */
static bool sFieldVoxel;
/*
 * The swoop between the two renderers of the field (voxel_swoop.c). Which
 * renderer drew the last field frame - the voxel world or the 2D layers -
 * is kept across the frames that draw neither (a map warming up is black),
 * and a change of it starts the swoop: the option toggled, a map with or
 * without voxel data entered. Frames off the field (a battle, a menu, the
 * title) forget it, so coming back from those is the cut it always was.
 */
enum { FIELD_MODE_NONE, FIELD_MODE_PIXEL, FIELD_MODE_VOXEL };
/* How long the 2D field may wait for the voxel world's chunks (1.5 s). */
#define VOXEL_SWOOP_HOLD_FRAMES 90u
/* The most game frames one shown picture may step the swoop by. */
#define VOXEL_SWOOP_STEPS_MAX 4u
static unsigned sFieldMode;
static VoxelSwoop sSwoop;

void CtrVideo_SetFieldLight(bool on, int x, int y, int radius)
{
    if (on && (!sFieldLight.on || sFieldLight.radius < 0))
        sFieldLight.shown = (float)radius;
    else if (on && radius > sFieldLight.radius && sFieldLight.shown < radius - 6.0f)
        sFieldLight.burst = 1.0f;
    sFieldLight.on = on;
    if (!on) { sFieldLight.burst = 0.0f; return; }
    sFieldLight.x = x;
    sFieldLight.y = y;
    sFieldLight.radius = radius;
    ++sFieldLight.ticks;
    /* The game grows the circle a pixel every other frame; eased, it opens
     * smoothly instead, and catches up within a few frames once it stops. */
    sFieldLight.shown += ((float)radius - sFieldLight.shown) * 0.25f;
    if (radius > sFieldLight.shown - 0.5f && radius < sFieldLight.shown + 0.5f) sFieldLight.shown = (float)radius;
    sFieldLight.burst *= 0.955f;
}

static bool FieldLightShown(void)
{
    return sFieldLight.on && !sTransitionCompose && !sStage && !sBattle && !sCentred;
}

/*
 * The 2D dark: the GBA's own circle, made by the midpoint walk the game uses
 * (SetFlashScanlineEffectWindowBoundaries), so every row opens exactly as
 * wide as it did there - the edges [x - half, x + half) - and outside it the
 * backdrop. Rows of the same width are one rectangle each side.
 */
static void FieldDarkness(void)
{
    static int16_t half[256];
    static int cached = -1;
    int r = sFieldLight.radius, cx = sFieldLight.x, cy = sFieldLight.y;
    int left = VIEW_LEFT, right = VIEW_RIGHT, top = VIEW_TOP, bottom = VIEW_BOTTOM;
    float ox = CTR_VIEW_X + sLayerShift, oy = CTR_VIEW_Y;
    uint32_t colour = sBackdropShown;

    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (cached != r)
    {
        int x = r, d = r, y = 0;

        for (int i = 0; i <= r; ++i) half[i] = -1;
        while (x >= y)
        {
            half[y] = (int16_t)x;
            half[x] = (int16_t)y;
            d -= (y * 2) - 1;
            y++;
            if (d < 0)
            {
                d += 2 * (x - 1);
                x--;
            }
        }
        cached = r;
    }
    ViewBase();
    Blend(5, false, false);
    if (cy - r > top)
        C2D_DrawRectSolid(ox + left, oy + top, 0, right - left, cy - r - top, colour);
    if (cy + r + 1 < bottom)
        C2D_DrawRectSolid(ox + left, oy + cy + r + 1, 0, right - left, bottom - cy - r - 1, colour);
    for (int dy = -r, next; dy <= r; dy = next)
    {
        int h = half[dy < 0 ? -dy : dy], y0 = cy + dy, y1;

        for (next = dy + 1; next <= r && half[next < 0 ? -next : next] == h; ++next) {}
        y1 = cy + next;
        if (y1 <= top || y0 >= bottom) continue;
        if (h < 0)
        {
            C2D_DrawRectSolid(ox + left, oy + y0, 0, right - left, y1 - y0, colour);
            continue;
        }
        if (cx - h > left)
            C2D_DrawRectSolid(ox + left, oy + y0, 0, cx - h - left, y1 - y0, colour);
        if (cx + h < right)
            C2D_DrawRectSolid(ox + cx + h, oy + y0, 0, right - cx - h, y1 - y0, colour);
    }
}

/*
 * A layer straight onto an eye (sLayerOnEye) that is the lesser part of an
 * alpha blend, on these lines: the scene surface would keep the transparent
 * alpha under it (Blend, underWorld) and the world show as it is, so it is not
 * drawn.
 */
static bool LayerLeftToWorld(unsigned bg, bool effects)
{
    unsigned control = Reg(0x50), weights = Reg(0x52);

    return sLayerOnEye && effects && ((control >> 6) & 3) == 1 && (control & (1u << bg)) && ((control >> 8) & 63)
        && Min(weights & 31, 16) < Min((weights >> 8) & 31, 16);
}

/*
 * A transition's picture leaves its first and last columns for the screen's
 * margins (TransitionPicture), which stretch them: what its BG0 holds there
 * reaches the edge, as the Poké Balls' black trail must. A sprite in them was
 * stretched too, a ball coming in as a striped block, so its sprites stop short
 * of those columns and go on in the margins by themselves
 * (TransitionSpritesInMargins).
 */
#define TRANSITION_OBJ_X0 1
#define TRANSITION_OBJ_X1 239

static void TransitionObjInset(void)
{
    SpriteBatchFlush();
    C2D_Flush();
    Scissor(sClipX0 > TRANSITION_OBJ_X0 ? sClipX0 : TRANSITION_OBJ_X0, sClipY0,
            sClipX1 < TRANSITION_OBJ_X1 ? sClipX1 : TRANSITION_OBJ_X1, sClipY1);
}

static void Layers(unsigned mask)
{
    unsigned display = Reg(0), mode = display & 7;
    mask &= ~sLayerExclude;
    if (mode > 2) { Error(10, "bitmap display modes are not part of baseline"); return; }
    for (int priority = 3; priority >= 0; --priority)
    {
        /* One depth plane at a time while composing them separately. */
        if (!(sPriorityMask & (SLOT_OBJ(priority) | SLOT_BG(priority)))) continue;
        /* Priority is the depth scale: 3 stays at the screen plane. */
        sLayerShift = (sLayerOrigin + sParallax * (3 - priority)) / sShiftZoom;
        /* The dark of a cave over the field, under its text. */
        if (priority == 0 && (mask & 1) && (sPriorityMask & SLOT_BG(0)) && !sFieldVoxel && FieldLightShown())
            FieldDarkness();
        for (int bg = 3; bg >= 0 && (sPriorityMask & SLOT_BG(priority)); --bg)
        {
            if (!(mask & (1u << bg)) || !(display & (0x100u << bg)) || (Reg(8 + bg * 2) & 3) != (unsigned)priority) continue;
            if ((mode == 1 && bg == 3) || (mode == 2 && bg < 2)) continue;
            /* Only a mosaic that shows: the field sets the bit on every BG
             * (InitOverworldBgs) and leaves REG_MOSAIC at 0. */
            if ((Reg(8 + bg * 2) & 0x40) && (Reg(0x4c) & 0xff))
            {
                Error(11, "BG mosaic not supported");
                sQualityMosaic = true;
            }
            if (LayerLeftToWorld((unsigned)bg, mask & 32)) continue;
            int clipY0 = sClipY0, clipY1 = sClipY1, viewY = sViewY, drop = StageLayerDrop(bg);

            sViewY += drop;
            sClipY0 -= drop;
            sClipY1 -= drop;
            Blend(bg, mask & 32, false);
            /* Which stage a slow frame is in: the layer walk, the sprites or
             * the GPU. Guessing that from fps alone costs a hardware run. */
            uint64_t start = svcGetSystemTick();
            float shift = sLayerShift;
            uint32_t tilesBefore = sStats.tiles;
            if (sFieldUi && bg == 0 && !sFieldBanner) sLayerShift += CTR_FIELD_UI_SHIFT / sShiftZoom;
            if ((mode == 1 && bg == 2) || mode == 2) DrawAffineBg(bg);
            else if (sBattle && (bg == 0 || (BattleBoxLayers() & (1u << bg)))) DrawBattleTextLayer(bg);
            else if (!DrawLineBg(bg) && !DrawFieldBgTex(bg) && !DrawStageBgTex(bg) && !DrawBandBgTex(bg)
                     && !DrawBattleBgTex(bg))
            {
                DrawTextBg(bg);
                BattleLayerWalked(bg, sStats.tiles - tilesBefore);
            }
            sBgDrawn[bg] += sStats.tiles - tilesBefore;
            sLayerShift = shift;
            sViewY = viewY;
            sClipY0 = clipY0;
            sClipY1 = clipY1;
            sBgTicks += svcGetSystemTick() - start;
        }
        if ((mask & 16) && (display & 0x1000) && (sPriorityMask & SLOT_OBJ(priority)))
        {
            uint64_t start = svcGetSystemTick();
            if (sTransitionCompose) TransitionObjInset();
            DrawObjects(priority, mask & 32);
            if (sTransitionCompose)
            {
                C2D_Flush();
                RestoreScissor();
            }
            sObjTicks += svcGetSystemTick() - start;
        }
    }
}

/*
 * A window edge is an 8-bit field, so 255 is the largest value the game can
 * write. On a 240x160 screen that already meant "past the edge"; on this
 * viewport it has to keep meaning the edge of the screen, or everything beyond
 * x=255 falls outside every window. The overworld sets WIN0H=0x00FF with
 * WININ showing all layers and WINOUT showing only the empty text layer, which
 * is exactly the black band on the right of the map.
 *
 * A window whose first edge is also saturated is empty on hardware (WIN1H is
 * set to 0xFFFF for that purpose) and stays empty here.
 *
 * A wide unit's WIN_RANGE says what each edge passes 255 by (`over`, left in
 * bits 8-15, right in 0-7; gba/io_reg.h): such an edge is exact, 384 being
 * 384 and not the edge of the screen.
 */
static int WindowEdge(unsigned limits, unsigned over, bool last, unsigned extent)
{
    unsigned first = (limits >> 8) + (over >> 8), end = (limits & 255) + (over & 255);

    if (last)
        return (end == 255 && first < 255) ? (int)extent : (int)Min(end, extent);
    return (int)Min(first, extent);
}

/* What window w's edges pass 255 by, while WINxH still holds the value
 * SetGpuReg wrote with it (gpu_regs.c; the offsets are gba/io_reg.h's). */
static unsigned WindowOver(unsigned w, unsigned across)
{
    if (Reg(0x5a + 2 * w) != across) return 0;
    return Reg(0x56 + 2 * w);
}

/*
 * One axis of a window, in GBA coordinates. A stage keeps the GBA's own
 * reading of the registers (an end past the screen, or before the start, means
 * the screen edge), and a window that reaches an edge of the GBA screen reaches
 * the edge of the stage: that is what the game meant by it, and the letterbox
 * of the intro would otherwise leave its margins outside every window.
 */
/*
 * The battle intro opens its scene through window 0: a band across the whole
 * screen, with nothing shown outside it, growing from the middle line to the
 * full height. That is a curtain over the whole picture, so on the battle
 * screen it keeps the picture's proportions like the stage's letterbox does;
 * any other battle window (a spotlight, a move's band around a battler) marks
 * a place in the scene and stays 1:1.
 */
static bool BattleCurtain(void)
{
    unsigned across = Reg(0x40);

    return sBattle && sZoom == 1.0f && (Reg(0) & 0x2000) && !(Reg(0x4a) & 63)
        && (across >> 8) == 0 && (across & 255) >= 240;
}

static void WindowSpan(unsigned limits, unsigned over, bool vertical, int *first, int *last)
{
    bool gba = sStage || sBattle || sTransitionCompose;
    unsigned extent = gba ? (vertical ? 160u : 240u)
                          : (vertical ? (unsigned)CTR_GAME_HEIGHT : (unsigned)CTR_GAME_WIDTH);

    if (gba) over = 0;
    *first = WindowEdge(limits, over, false, extent);
    *last = WindowEdge(limits, over, true, extent);
    /*
     * A left edge past the right one wraps round the line on the GBA: that is
     * how a window starts left of the screen. The label of the starter on the
     * left (starter_choose.c, its column 0 less 4 pixels) is WIN0H 252..108,
     * which on a centred screen is -4..108, just into the margin.
     */
    if (sCentred && !vertical && *first > *last) *first -= 256;
    if (!gba) return;
    if ((limits & 255) > extent || (limits >> 8) > (limits & 255)) *last = (int)extent;
    /* A transition is composed as the GBA picture, margins added after; a
     * magnified stage is the GBA picture alone (RenderEye), its windows - the
     * cutscenes' black bars, the descent's ray of light - the GBA's own. */
    if (*first >= *last || sTransitionCompose || sStageZoomed) return;
    if (vertical && sBattle && !BattleCurtain())
    {
        if (*first == 0) *first = VIEW_TOP;
        if (*last >= (int)extent) *last = VIEW_BOTTOM;
        return;
    }
    if (vertical)
    {
        /*
         * Vertically the stage keeps the picture's proportions: the intro's
         * cinematic bars close in to 32 lines of 160, and on a 240-line screen
         * that has to be 48 lines of 240, or the scene between them is a thin
         * strip in the middle of a black screen.
         */
        *first = VIEW_TOP + *first * (VIEW_BOTTOM - VIEW_TOP) / (int)extent;
        *last = VIEW_TOP + *last * (VIEW_BOTTOM - VIEW_TOP) / (int)extent;
        return;
    }
    if (*first == 0) *first = VIEW_LEFT;
    if (*last >= (int)extent) *last = VIEW_RIGHT;
}

/*
 * The window edges an HBlank DMA writes line by line (CtrVideo_SetLineWindow):
 * WIN0H, or WIN0H and WIN1H, for each of the picture's 160 lines. The PokéNav
 * lights its chosen option with one, and its condition graph is a polygon
 * drawn as two of them. The Battle Dome's tourney tree moves both from its
 * HBlank interrupt, and what they show (WININ) with them. Only those use them
 * here (LineWindows): the frame is composed in bands of lines whose windows
 * are the same, each band partitioned as a frame with fixed windows is.
 */
static struct
{
    unsigned windows;
    bool inside;
    uint16_t across[2][160];
    uint16_t control[160];
} sLineWindows;

/* The band being composed: its lines, and the line-driven windows' edges there. */
static struct
{
    bool on;
    int top, bottom;
    uint16_t across[2];
} sLineBand;

/*
 * A text background in a band of line windows (Compose): the picture's part
 * of the cell from the layer texture, and the margins as a centred screen
 * fills them. A band is a line or two tall, so walking its tiles would cost
 * far more than the quad.
 */
static bool DrawBandBgTex(unsigned bg)
{
    const CentredFill *fill = CentredFillOf(sCentredScreen);

    if (!sLineBand.on || !LayerDrawable(bg)) return false;
    ViewBase();
    DrawLayerRect(bg, 0, 240, 0, 160, 0, 0, 1, 1, false, 0, 0);
    if (CentredLayers(fill) & (1u << bg)) DrawCentredMargins(bg, fill, -64, 64);
    return true;
}

void CtrVideo_SetLineWindow(const uint16_t *values, unsigned lines, bool both)
{
    sLineWindows.windows = values ? (both ? 3u : 1u) : 0u;
    if (!values) return;
    for (unsigned y = 0; y < 160; ++y)
    {
        sLineWindows.across[0][y] = y < lines ? values[both ? y * 2 : y] : 0;
        sLineWindows.across[1][y] = y < lines && both ? values[y * 2 + 1] : 0;
    }
}

void CtrVideo_SetLineWindowInside(const uint16_t *values)
{
    sLineWindows.inside = values != NULL;
    if (values) memcpy(sLineWindows.control, values, sizeof(sLineWindows.control));
}

static bool LineWindows(void)
{
    return sLineWindows.windows && (Reg(0) & 0x6000)
        && (sCentredScreen == CTR_CENTRED_POKENAV || (sLineWindows.inside && sCentredScreen != CTR_CENTRED_NONE));
}

/* Window w's rectangle in this band, empty when x0 >= x1 or y0 >= y1. */
static void WindowRect(unsigned w, int *x0, int *x1, int *y0, int *y1)
{
    unsigned across = Reg(0x40 + 2 * w), over = WindowOver(w, across);

    if (sLineBand.on && (sLineWindows.windows & (1u << w))) across = sLineBand.across[w], over = 0;
    /* The cave's circle is drawn as such (FieldDarkness): window 0 stays whole. */
    if (w == 0 && FieldLightShown()) across = 0x00FF, over = 0;
    WindowSpan(across, over, false, x0, x1);
    WindowSpan(Reg(0x44 + 2 * w), 0, true, y0, y1);
    if (sLineBand.on)
    {
        if (*y0 < sLineBand.top) *y0 = sLineBand.top;
        if (*y1 > sLineBand.bottom) *y1 = sLineBand.bottom;
    }
}

static bool Inside(int px, int py, unsigned w)
{
    int x0, x1, y0, y1;

    WindowRect(w, &x0, &x1, &y0, &y1);
    /* Same reading of an empty window as the partition uses. */
    return x0 < x1 && y0 < y1 && px >= x0 && px < x1 && py >= y0 && py < y1;
}

/*
 * The rectangles lines [top, bottom) split into by the windows, each with the
 * layer mask it shows: WIN0 > WIN1 > outside. At most 5 x 5 of them.
 */
#define WINDOW_RECTS 25
static unsigned WindowPartition(int top, int bottom, int rects[WINDOW_RECTS][4],
                                unsigned masks[WINDOW_RECTS])
{
    unsigned display = Reg(0), count = 0;
    /* Window rectangles are GBA coordinates; the partition covers the whole
     * viewport so the margins keep the "outside" mask. */
    bool gba = sTransitionCompose;
    int xs[6] = {gba ? 0 : VIEW_LEFT, gba ? 240 : VIEW_RIGHT};
    int ys[6] = {top, bottom};
    unsigned nx = 2, ny = 2;
    for (unsigned w = 0; w < 2; ++w)
        if (display & (0x2000u << w))
        {
            int x0, x1, y0, y1;

            WindowRect(w, &x0, &x1, &y0, &y1);
            if (y0 < top) y0 = top;
            if (y1 > bottom) y1 = bottom;
            if (sTransitionCompose)
            {
                if (x0 < xs[0]) x0 = xs[0];
                if (x1 > xs[1]) x1 = xs[1];
            }

            /*
             * A window left enabled with both edges past the screen is how the
             * game turns one off: the overworld runs with WIN1H = 0xFFFF all
             * the time. It covers nothing, so its edges must not cut the
             * frame. Cutting there split every overworld frame in two at
             * x=255 - the end of the GBA's addressable width, which is exactly
             * where the seam showed - and walked every layer twice to do it.
             */
            if (x0 >= x1 || y0 >= y1) continue;
            xs[nx++] = x0;
            xs[nx++] = x1;
            ys[ny++] = y0;
            ys[ny++] = y1;
        }
    for (unsigned i = 0; i < nx; ++i) for (unsigned j = i + 1; j < nx; ++j)
        if (xs[j] < xs[i]) { int t=xs[j]; xs[j]=xs[i]; xs[i]=t; }
    for (unsigned i = 0; i < ny; ++i) for (unsigned j = i + 1; j < ny; ++j)
        if (ys[j] < ys[i]) { int t=ys[j]; ys[j]=ys[i]; ys[i]=t; }
    for (unsigned y = 0; y + 1 < ny; ++y) for (unsigned x = 0; x + 1 < nx; ++x)
    {
        if (xs[x] == xs[x+1] || ys[y] == ys[y+1]) continue;
        /* Outside every window: WINOUT once any window is on, the OBJ
         * window too (a stat change after the bag has only that one). */
        unsigned mask = (display & 0xE000) ? Reg(0x4a) & 63 : 63;
        for (int w = 1; w >= 0; --w)
            if ((display & (0x2000u << w)) && Inside(xs[x], ys[y], (unsigned)w))
                mask = (Reg(0x48) >> (8*w)) & 63;
        rects[count][0] = xs[x];
        rects[count][1] = ys[y];
        rects[count][2] = xs[x+1];
        rects[count][3] = ys[y+1];
        masks[count++] = mask;
    }
    return count;
}

/* Layers a transition's band composes through its windows (TransitionCompose). */
static unsigned sTransitionLayers;
/* Its sprites this frame, with no window cutting them (TransitionSpritesInMargins). */
static bool sTransitionObjShown;

/*
 * The OBJ window (DISPCNT bit 15): where a sprite in window mode (OAM mode 2)
 * has an opaque pixel, and neither window 0 nor 1 is, the layers shown are
 * WINOUT's upper byte instead of its lower. The battle keeps it on throughout
 * with no such sprite, so it changes nothing, until a stat change
 * (StatsChangeAnimation_Step1, src/battle_anim_utility_funcs.c): an invisible
 * copy of the Pokemon in window mode, and BG1 - the rising or falling
 * pattern - shown only through it, blended over the Pokemon.
 *
 * Per pixel is not what the window rectangles (WindowPartition) can cut, so
 * the layers the window adds are composed once a frame, at 1:1, into a small
 * surface of their own over the window's sprites as a mask (ObjWindowCompose),
 * and that surface is drawn where the outside of the windows is, after that
 * rectangle's own layers (ObjWindowDraw). It exists, and the frame pays for
 * it, only while a window sprite is on screen: a stat change, about a second.
 * The layers drawn over the mask go on top of that rectangle - the stat
 * pattern at priority 0 is; a window taking layers away is not reproduced.
 */
#define OBJWIN_SIZE 128
/* The link battle's VS letters, two 64x64 sprites at double size moving
 * apart, are a window wider than that (they were cut off): 256 KB of VRAM
 * then, for the second they last. */
#define OBJWIN_SIZE_LARGE 256
static C3D_Tex sObjWinTex;
static C3D_RenderTarget *sObjWinTarget;
static bool sObjWinWanted, sObjWinFailed;
static uint32_t sObjWinUsedFrame, sObjWinFailFrame;
/* The side the window's box needed last frame, and when a surface that
 * large could not be made. */
static int sObjWinNeed;
static uint32_t sObjWinLargeFailFrame;
static bool sObjWinLargeFailed;
/* This frame's: its box in GBA pixels and the layers it adds. */
static struct { bool ready; int x0, y0, x1, y1; unsigned layers; } sObjWin;
/*
 * The 3D battle's: the window's layers left out of the scenery, to be drawn
 * in the slot of the sprite group under them instead (SlotsCompose). A stat
 * change's pattern is on its Pokemon and blends with it; in the scenery it
 * was a background in front of the sprites that blends, which can only be
 * composed whole - per eye over the voxel world, 15 ms of drawing for the
 * second it lasts, or flat in the 2D battle, the Pokemon losing its depth.
 */
static bool sObjWinSlotted;
/* Drawing it over an eye's picture, at its group's displacement (ObjWindowEye). */
static bool sObjWinEye;
static float sObjWinEyeShift;

/* Outside the frame, like the battle's other surfaces (SlotsPrepare). */
static void ObjWindowRelease(void)
{
    if (sObjWinTarget) { ClearWhenMadeForget(sObjWinTarget); C3D_RenderTargetDelete(sObjWinTarget); }
    if (sObjWinTex.data) C3D_TexDelete(&sObjWinTex);
    sObjWinTarget = NULL;
    memset(&sObjWinTex, 0, sizeof(sObjWinTex));
}

static void ObjWindowPrepare(void)
{
    bool wanted = sObjWinWanted;
    bool large = sObjWinNeed > OBJWIN_SIZE
              && !(sObjWinLargeFailed && sStats.frames - sObjWinLargeFailFrame < LAYER_RETRY_FRAMES);

    sObjWinWanted = false;
    if (!wanted) sObjWinNeed = 0;
    if (wanted) sObjWinUsedFrame = sStats.frames;
    else if (sObjWinTarget && (sTransitionRequested || sVramAsked
                               || sStats.frames - sObjWinUsedFrame > LAYER_IDLE_FRAMES))
        ObjWindowRelease();
    /* Too small for the window now on screen: made again, larger. */
    if (wanted && sObjWinTarget && large && sObjWinTex.width < OBJWIN_SIZE_LARGE)
        ObjWindowRelease();
    if (!wanted || sObjWinTarget || (sObjWinFailed && sStats.frames - sObjWinFailFrame < LAYER_RETRY_FRAMES))
        return;
    /* RGBA8: the mask's count needs more than one bit of alpha. 64 KB of
     * VRAM; a Pokemon's 64x64 fits the half size too. */
    for (unsigned size = large ? OBJWIN_SIZE_LARGE : OBJWIN_SIZE; size >= OBJWIN_SIZE / 2 && !sObjWinTarget;
         size /= 2)
    {
        /* The battle's kept scenery gives way first, before the window is
         * made smaller for it (SlotsYield). */
        for (unsigned round = 0; round < 2 && !sObjWinTarget; ++round)
        {
            if (round == 1 && !SlotsYield()) break;
            if (C3D_TexInitVRAM(&sObjWinTex, size, size, GPU_RGBA8)
                && (sObjWinTarget = C3D_RenderTargetCreateFromTex(&sObjWinTex, GPU_TEXFACE_2D, 0, -1)))
            {
                ClearWhenMade(sObjWinTarget);
                break;
            }
            ObjWindowRelease();
        }
    }
    sObjWinFailed = sObjWinTarget == NULL;
    if (large && (!sObjWinTarget || sObjWinTex.width < OBJWIN_SIZE_LARGE))
    {
        sObjWinLargeFailed = true;
        sObjWinLargeFailFrame = sStats.frames;
    }
    else if (large)
        sObjWinLargeFailed = false;
    if (sObjWinTarget)
    {
        /* Drawn at the scene's whole-number zoom: every texel a block. */
        C3D_TexSetFilter(&sObjWinTex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&sObjWinTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    }
    else
        sObjWinFailFrame = sStats.frames;
    CtrLog_Write(sObjWinTarget ? CTR_LOG_VIDEO : CTR_LOG_ERROR, "VIDEO: OBJ window surface %s, %u texels (VRAM free=%lu)",
                 sObjWinTarget ? "made" : "not made", sObjWinTarget ? (unsigned)sObjWinTex.width : 0u,
                 (unsigned long)vramSpaceFree());
}

/* The box of the window's sprites, in GBA pixels; false if there is none. */
static bool ObjWindowBounds(int *x0, int *y0, int *x1, int *y1)
{
    bool any = false;

    /* The list has it from the same reading (3ds_obj_list.c). */
    if (sObjList)
    {
        if (!sObjList->window) return false;
        *x0 = sObjList->windowX0;
        *y0 = sObjList->windowY0;
        *x1 = sObjList->windowX1;
        *y1 = sObjList->windowY1;
        return true;
    }
    for (unsigned i = 0; i < 128; ++i)
    {
        unsigned attr0 = sMemory.oam[i * 4], attr1 = sMemory.oam[i * 4 + 1];
        unsigned shape = attr0 >> 14;
        bool affine = (attr0 & 0x100) != 0, twice = affine && (attr0 & 0x200);
        int x, y, w, h;

        if (((attr0 >> 10) & 3) != 2 || (!affine && (attr0 & 0x200)) || shape == 3) continue;
        w = gCtrObjDims[shape][attr1 >> 14][0] << twice;
        h = gCtrObjDims[shape][attr1 >> 14][1] << twice;
        /* The GBA's reading, as DrawObjects has it for these screens. */
        x = (int)(attr1 & 511);
        y = (int)(attr0 & 255);
        if (x + w > 512) x -= 512;
        if (y + h > 256) y -= 256;
        if (!any || x < *x0) *x0 = x;
        if (!any || y < *y0) *y0 = y;
        if (!any || x + w > *x1) *x1 = x + w;
        if (!any || y + h > *y1) *y1 = y + h;
        any = true;
    }
    return any;
}

/*
 * In the frame, before anything is composed (CtrVideo_Present): the layers
 * the OBJ window adds, over its sprites as a mask, into its surface. Only on
 * the screens that keep the GBA's geometry, where the window is used.
 */
static void ObjWindowCompose(void)
{
    unsigned display = Reg(0), out = Reg(0x4a) & 63, in = (Reg(0x4a) >> 8) & 63;
    /* Not the battle's text box: it is composed apart (DrawBattleTextLayer). */
    unsigned layers = in & ~out & (display >> 8) & (sBattle ? 14u : 15u);
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0, size;

    sObjWin.ready = false;
    if (!sBattle && !sStage) return;
    if ((display & 0x9000) != 0x9000 || (display & 7) > 2) return;
    /* A window that adds no layer but changes the effects, the sprites or
     * hides one (Encore's and Flatter's spotlight: BLDY off inside it) is
     * not drawn: counted, for the frames that lose it. */
    if (!layers)
    {
        if (((in ^ out) & 63) && ObjWindowBounds(&x0, &y0, &x1, &y1)) sQualityObjWin = true;
        return;
    }
    if (!ObjWindowBounds(&x0, &y0, &x1, &y1)) return;
    /* Only what can be on screen: the VS letters end sliding off it as they
     * grow, a box far wider than any surface. */
    if (x0 < VIEW_LEFT) x0 = VIEW_LEFT;
    if (y0 < VIEW_TOP) y0 = VIEW_TOP;
    if (x1 > VIEW_RIGHT) x1 = VIEW_RIGHT;
    if (y1 > VIEW_BOTTOM) y1 = VIEW_BOTTOM;
    if (x0 >= x1 || y0 >= y1) return;
    if (out & 31 & ~in)
    {
        Error(12, "OBJ window hiding layers not supported");
        sQualityObjWin = true;
    }
    sObjWinWanted = true;
    sObjWinNeed = x1 - x0 > y1 - y0 ? x1 - x0 : y1 - y0;
    if (!sObjWinTarget) return;
    size = (int)sObjWinTex.width;
    if (x1 - x0 > size || y1 - y0 > size)
    {
        Error(13, "OBJ window larger than its surface");
        if (x1 - x0 > size) x1 = x0 + size;
        if (y1 - y0 > size) y1 = y0 + size;
    }

    float zoom = sZoom, offX = sOffX, offY = sOffY, origin = sLayerOrigin, parallax = sParallax;
    float shift = sLayerShift, fade = sPaletteFade;
    int viewX = sViewX, viewY = sViewY, targetW = sTargetW, targetH = sTargetH, surfaceH = sSurfaceH;
    unsigned exclude = sLayerExclude, priorities = sPriorityMask;
    bool depth = sSpriteDepth, objMask = sObjMaskOn;

    /* GBA (x0, y0) at the surface's corner, at 1:1, flat. */
    sZoom = 1.0f;
    sOffX = sOffY = 0.0f;
    sViewX = -x0;
    sViewY = -y0;
    sTargetW = x1 - x0;
    sTargetH = y1 - y0;
    sSurfaceH = size;
    sLayerOrigin = sParallax = sLayerShift = 0.0f;
    sPaletteFade = 0.0f;
    sLayerExclude = 0;
    sPriorityMask = SLOTS_ALL;
    sSpriteDepth = sObjMaskOn = false;
    ClipToView();

    ColourClear(sObjWinTarget, 0);
    BlendForget();
    SceneOn(sObjWinTarget);
    C2D_ViewReset();
    sObjWinPass = OBJWIN_MASK;
    for (int priority = 3; priority >= 0; --priority)
        DrawObjects((unsigned)priority, false);
    C2D_Flush();
    BlendForget();
    sObjWinPass = OBJWIN_LAYERS;
    /* Without the effect bit: the blend is the surface's (Blend). */
    Layers(layers);
    C2D_Flush();
    sObjWinPass = OBJWIN_NONE;
    BlendForget();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    /* Rendering to a texture and then sampling it needs a command split. */
    C3D_FrameSplit(0);

    sZoom = zoom;
    sOffX = offX;
    sOffY = offY;
    sViewX = viewX;
    sViewY = viewY;
    sTargetW = targetW;
    sTargetH = targetH;
    sSurfaceH = surfaceH;
    sLayerOrigin = origin;
    sParallax = parallax;
    sLayerShift = shift;
    sPaletteFade = fade;
    sLayerExclude = exclude;
    sPriorityMask = priorities;
    sSpriteDepth = depth;
    sObjMaskOn = objMask;
    ClipToView();

    sObjWin.ready = true;
    sObjWin.x0 = x0;
    sObjWin.y0 = y0;
    sObjWin.x1 = x1;
    sObjWin.y1 = y1;
    sObjWin.layers = layers;
}

/*
 * The OBJ window's surface over [x0, x1) x [y0, y1), a rectangle of the
 * outside of windows 0 and 1, already scissored: drawn as its frontmost layer
 * would be, with that layer's blend and depth.
 */
static void ObjWindowDraw(int x0, int y0, int x1, int y1)
{
    unsigned layers = sObjWin.layers & (sObjWinEye ? ~0u : ~sLayerExclude), bg = 0, priority = 4;
    float shift = sLayerShift;

    if (sObjWinSlotted && !sObjWinEye) return;

    if (x0 < sObjWin.x0) x0 = sObjWin.x0;
    if (y0 < sObjWin.y0) y0 = sObjWin.y0;
    if (x1 > sObjWin.x1) x1 = sObjWin.x1;
    if (y1 > sObjWin.y1) y1 = sObjWin.y1;
    if (x0 >= x1 || y0 >= y1) return;
    for (unsigned b = 0; b < 4; ++b)
        if ((layers & (1u << b)) && (Reg(8 + b * 2) & 3) < priority)
        {
            priority = Reg(8 + b * 2) & 3;
            bg = b;
        }
    if (priority > 3 || !(sPriorityMask & SLOT_BG(priority))) return;

    int w = sObjWin.x1 - sObjWin.x0, h = sObjWin.y1 - sObjWin.y0;
    float size = (float)sObjWinTex.width;
    const Tex3DS_SubTexture cut = {(u16)w, (u16)h, 0.0f, 1.0f, w / size, 1.0f - h / size};

    Blend(bg, ((Reg(0x4a) >> 8) & 32) != 0, false);
    /* Only where the window's mask and a layer's pixel both are. */
    C3D_AlphaTest(true, GPU_GREATER, 0xc0);
    ViewBase();
    /* Over an eye, displaced like the group the window is over (ObjWindowEye). */
    sLayerShift = sObjWinEye ? sObjWinEyeShift : (sLayerOrigin + sParallax * (3 - (int)priority)) / sShiftZoom;
    C2D_DrawImageAt((C2D_Image){&sObjWinTex, &cut}, sObjWin.x0 + CTR_VIEW_X + sLayerShift,
                    sObjWin.y0 + CTR_VIEW_Y, 0, &sTint, 1, 1);
    C2D_Flush();
    BlendForget();
    sLayerShift = shift;
    /* A background in front of the sprites (RenderBattleStereo). */
    if (!sObjWinEye) sBgDrawn[bg] += 1;
}

/*
 * The OBJ window's layers over an eye's picture, after the sprites' groups:
 * at the displacement of the group its box is in, blended with what is
 * there as on the GBA, and at the screen's zoom with the window's own
 * nearest filtering - inside the group's slot they came out smoothed, the
 * slot being composed at twice the size and drawn down bilinearly.
 */
static void ObjWindowEye(C3D_RenderTarget *picture, int centre)
{
    unsigned priorities = sPriorityMask, exclude = sLayerExclude;

    SceneOn(picture);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    sPriorityMask = SLOTS_ALL;
    sLayerExclude = 0;
    sObjWinEye = true;
    sObjWinEyeShift = BattleSpriteShift(centre) / sZoom;
    ObjWindowDraw(sObjWin.x0, sObjWin.y0, sObjWin.x1, sObjWin.y1);
    sObjWinEye = false;
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    sPriorityMask = priorities;
    sLayerExclude = exclude;
}

static void ComposeBand(int top, int bottom)
{
    int rects[WINDOW_RECTS][4];
    unsigned masks[WINDOW_RECTS];
    unsigned count = WindowPartition(top, bottom, rects, masks);
    /* What the pass composes into: the view, or less (RenderBands' box). */
    int clipX0 = sClipX0, clipY0 = sClipY0, clipX1 = sClipX1, clipY1 = sClipY1;
    int wholeX0 = sClipWhole.x0, wholeX1 = sClipWhole.x1;

    /* Each rectangle has a uniform mask. GPU scissor clips transformed primitives. */
    for (unsigned i = 0; i < count; ++i)
    {
        unsigned mask = sTransitionCompose ? masks[i] & sTransitionLayers : masks[i];
        int x0 = rects[i][0] > clipX0 ? rects[i][0] : clipX0, x1 = rects[i][2] < clipX1 ? rects[i][2] : clipX1;
        int y0 = rects[i][1] > clipY0 ? rects[i][1] : clipY0, y1 = rects[i][3] < clipY1 ? rects[i][3] : clipY1;

        if (sTransitionCompose && !(mask & 17u)) continue;
        if (x0 >= x1 || y0 >= y1) continue;
        C2D_Flush();
        Scissor(x0, y0, x1, y1);
        sScissored = true;
        sClipX0 = x0; sClipX1 = x1;
        sClipY0 = y0; sClipY1 = y1;
        /* The rectangle as one compose clips it, for a pass cut from it. */
        sClipWhole.x0 = rects[i][0] > wholeX0 ? rects[i][0] : wholeX0;
        sClipWhole.x1 = rects[i][2] < wholeX1 ? rects[i][2] : wholeX1;
        Layers(mask);
        /* The OBJ window opens only where windows 0 and 1 are not. */
        if (sObjWin.ready && !sTransitionCompose
            && !((Reg(0) & 0x2000) && Inside(x0, y0, 0)) && !((Reg(0) & 0x4000) && Inside(x0, y0, 1)))
            ObjWindowDraw(x0, y0, x1, y1);
    }
    sClipX0 = clipX0; sClipY0 = clipY0;
    sClipX1 = clipX1; sClipY1 = clipY1;
    sClipWhole.x0 = wholeX0;
    sClipWhole.x1 = wholeX1;
}

static void ComposeLines(void);

/*
 * The pass in bands of the lines whose BLDALPHA is the same (sLineAlpha), each
 * with its own weights: Surf's and Muddy Water's wave is a band of EVA 1-13
 * over lines where it is EVA 0, growing a line a frame, and drawn with line
 * 0's weights over the whole screen it showed at once, or not at all for 48
 * frames, where the GBA reveals it line by line. Lines above the picture
 * have its first line's weights, those below its last's. Only for a pass
 * whose layers those weights reach - a first target of the alpha blend, or
 * the sprites, which blend with them when semi-transparent.
 */
static bool ComposeAlphaBands(void)
{
    unsigned control = Reg(0x50), drawn = 63u & ~sLayerExclude;
    int clipX0 = sClipX0, clipY0 = sClipY0, clipX1 = sClipX1, clipY1 = sClipY1;
    bool scissored = sScissored, targets;
    static uint32_t looked = ~0u;
    static bool seeThrough;

    /* The battle's alone (contests and the field keep line 0's weights). */
    if (!sLineAlphaOn || sLineAlphaBand >= 0 || sRegLine || !sBattle) return false;
    /* A semi-transparent sprite blends with the weights wherever it is. */
    if ((drawn & 16) && looked != sStats.frames)
    {
        looked = sStats.frames;
        seeThrough = sObjList && sObjList->semi;
        for (unsigned i = 0; i < 128 && !seeThrough && !sObjList; ++i)
        {
            unsigned attr0 = sMemory.oam[i * 4];

            seeThrough = (attr0 & 0x300) != 0x200 && ((attr0 >> 10) & 3) == 1;
        }
    }
    targets = (drawn & 32) && ((control >> 6) & 3) == 1 && (control & drawn & 31);
    if (!targets && !((drawn & 16) && seeThrough)) return false;
    for (int y = clipY0, next; y < clipY1; y = next)
    {
        unsigned value = sLineAlpha[y < 0 ? 0 : y >= CTR_GBA_LINES ? CTR_GBA_LINES - 1 : y];

        for (next = y + 1; next < clipY1; ++next)
            if (sLineAlpha[next < 0 ? 0 : next >= CTR_GBA_LINES ? CTR_GBA_LINES - 1 : next] != value) break;
        sClipX0 = clipX0;
        sClipX1 = clipX1;
        sClipY0 = y;
        sClipY1 = next;
        C2D_Flush();
        Scissor(clipX0, y, clipX1, next);
        sScissored = true;
        /* Blend keeps its state while its key holds: not the weights. */
        BlendForget();
        sLineAlphaBand = (int)value;
        ComposeLines();
        sLineAlphaBand = -1;
        BlendForget();
    }
    C2D_Flush();
    sClipX0 = clipX0;
    sClipY0 = clipY0;
    sClipX1 = clipX1;
    sClipY1 = clipY1;
    sScissored = scissored;
    RestoreScissor();
    sLineAlphaBanded = true;
    return true;
}

static void Compose(void)
{
    if (!ComposeAlphaBands()) ComposeLines();
}

static void ComposeLines(void)
{
    unsigned display = Reg(0);
    int top = VIEW_TOP, bottom = VIEW_BOTTOM;
    int first = top > 0 ? top : 0, end = bottom < 160 ? bottom : 160;

    /* No window at all. With only the OBJ window on - a stat change after
     * the bag or party menu, which leave windows 0 and 1 off - outside it is
     * WINOUT's: composed as if there were no windows, the stat pattern
     * covered the whole screen. */
    if (!(display & 0xE000)) { Layers(63); return; }
    if (!LineWindows())
    {
        /* Windows line by line off the PokeNav: the first line's for all,
         * counted once a frame when the lines differ (sQuality). */
        static uint32_t looked = ~0u;

        if (sLineWindows.windows && (display & 0x6000) && looked != sStats.frames)
        {
            looked = sStats.frames;
            for (unsigned y = 1; y < 160 && !sQualityLineDropped; ++y)
                if (sLineWindows.across[0][y] != sLineWindows.across[0][0]
                    || sLineWindows.across[1][y] != sLineWindows.across[1][0])
                    sQualityLineDropped = true;
        }
        ComposeBand(top, bottom);
    }
    else
    {
        /* Above and below the picture no line opens a window. */
        sLineBand.on = true;
        sLineBand.across[0] = sLineBand.across[1] = 0;
        sLineBand.top = top;
        sLineBand.bottom = bottom < 0 ? bottom : 0;
        if (top < 0) ComposeBand(top, sLineBand.bottom);
        for (int y = first, next; y < end; y = next)
        {
            sLineBand.across[0] = sLineWindows.across[0][y];
            sLineBand.across[1] = sLineWindows.across[1][y];
            for (next = y + 1; next < end; ++next)
                if (sLineWindows.across[0][next] != sLineBand.across[0]
                 || sLineWindows.across[1][next] != sLineBand.across[1]
                 || (sLineWindows.inside && sLineWindows.control[next] != sLineWindows.control[y])) break;
            sLineBand.top = y;
            sLineBand.bottom = next;
            sBandInside = sLineWindows.inside ? sLineWindows.control[y] : -1;
            ComposeBand(y, next);
            sBandInside = -1;
        }
        sLineBand.across[0] = sLineBand.across[1] = 0;
        sLineBand.top = top > 160 ? top : 160;
        sLineBand.bottom = bottom;
        if (bottom > 160) ComposeBand(sLineBand.top, bottom);
        sLineBand.on = false;
    }
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    sScissored = false;
    ClipToView();
}
