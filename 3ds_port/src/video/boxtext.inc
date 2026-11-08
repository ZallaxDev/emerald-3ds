/*
 * Part of 3ds_video.c. The battle's text box at the screen's resolution: its
 * panel, its window's text and slide, DrawBattleText.
 */

/*
 * The battle text box (3ds_battle_hud.h, gen_battle_hud.py): a panel floating
 * over the scene with margins at its sides, the game's message window over
 * it 1:1 - its text, the window's fill left out - moved a few pixels right
 * to keep its distance from the panel's edge. Only while BG0 shows the
 * message box where battle_bg.c puts it; anything else (the GBA's menus, in
 * a link battle) is drawn as the GBA has it.
 */
#define BOX_TEX_W 32       /* the panel's art: its left cap, and its middle column */
#define BOX_TEX_H 64
#define BOX_MIDDLE_X 28
#define TEXT_TEX_W 256     /* the message window's 208x32 */
#define TEXT_TEX_H 32
#define BOX_TEXT_SHIFT 4
#define BOX_SLIDE_FRAMES 32
#define BOX_HIDDEN 60.0f
/* The player's trainer under it meanwhile, in GBA pixels: below the screen. */
#define BOX_TRAINER_HIDDEN 90.0f
static C3D_Tex sBoxTex, sTextTex;
static bool sBoxTexFailed;
static int sBoxCapW, sBoxArtH, sBoxArtY;
static uint8_t sBoxWindow[26 * 4 * 32];
static uint16_t sBoxEntries[26 * 4], sBoxColours[16];
static bool sBoxText;     /* the message window shows something */
/* A battle over the voxel world: the box waits out of sight while the
 * camera flies in, and slides in when the first message comes. */
static bool sBoxHidden, sBoxDecided, sBoxBarsUp;
/* The frame the HD text box was last drawn (or kept out of sight) in. */
static uint32_t sBoxShown = ~0u;
/* The cinema bars this far out of their way: the box comes in with them. */
#define BOX_BARS_LEAVING 0.35f
static int sBoxSlide = -1;
static uint32_t sBoxBattleFrames;
/* The HD text box's pictures as filled: one on whenever the panel's or the
 * message window's texture is written (BoxTexture, BoxWindowSync, a clear) -
 * part of the kept text's key (BattleTextKey). */
static uint32_t sBoxTexVersion;

static bool BoxTexture(void)
{
    const uint32_t *cap, *middle;
    int cx, cy, cw, ch, mx, my, mw, mh;
    uint32_t *data;

    if (sBoxTex.data && sTextTex.data) return true;
    if (sBoxTexFailed || !CtrBattleHud_Ready()) return false;
    cap = CtrBattleHud_BoxArt(false, &cx, &cy, &cw, &ch);
    middle = CtrBattleHud_BoxArt(true, &mx, &my, &mw, &mh);
    if (!cap || !middle || cw > BOX_MIDDLE_X || ch > BOX_TEX_H || mh != ch)
    {
        sBoxTexFailed = true;
        return false;
    }
    if (!C3D_TexInit(&sBoxTex, BOX_TEX_W, BOX_TEX_H, GPU_RGBA8)
        || !C3D_TexInit(&sTextTex, TEXT_TEX_W, TEXT_TEX_H, GPU_RGBA4))
    {
        if (sBoxTex.data) C3D_TexDelete(&sBoxTex);
        memset(&sBoxTex, 0, sizeof(sBoxTex));
        memset(&sTextTex, 0, sizeof(sTextTex));
        sBoxTexFailed = true;
        CtrLog_Write(CTR_LOG_ERROR, "video: no memory for the HD text box (linear free=%lu); the GBA's is shown",
                     (unsigned long)linearSpaceFree());
        return false;
    }
    C3D_TexSetFilter(&sBoxTex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&sBoxTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexSetFilter(&sTextTex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&sTextTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    data = sBoxTex.data;
    memset(data, 0, BOX_TEX_W * BOX_TEX_H * 4);
    for (int y = 0; y < ch; ++y)
    {
        for (int x = 0; x < cw; ++x)
            data[CtrVideo_Texel(x, y, BOX_TEX_W)] = cap[y * cw + x];
        data[CtrVideo_Texel(BOX_MIDDLE_X, y, BOX_TEX_W)] = middle[y * mw];
    }
    TexFilled(&sBoxTex);
    memset(sTextTex.data, 0, TEXT_TEX_W * TEXT_TEX_H * 2);
    TexFilled(&sTextTex);
    ++sBoxTexVersion;
    sBoxCapW = cw;
    sBoxArtH = ch;
    sBoxArtY = cy;
    memset(sBoxEntries, 0xFF, sizeof(sBoxEntries));
    return true;
}

/* The message window's 26x4 cells into the texture, when they changed. */
static void BoxWindowSync(unsigned map, unsigned size, unsigned chars)
{
    bool changed = memcmp(sBoxColours, (const uint16_t *)sTexturePalette, sizeof(sBoxColours)) != 0;
    uint16_t *data = sTextTex.data;

    for (unsigned i = 0; i < 26 * 4 && !changed; ++i)
    {
        unsigned entry = MapEntry(map, size, 2 + i % 26, 15 + i / 26);
        unsigned address = chars + (entry & 1023) * 32;

        if (entry != sBoxEntries[i] || address + 32 > 0x10000
            || memcmp(sBoxWindow + i * 32, sMemory.vram + address, 32))
            changed = true;
    }
    if (!changed) return;
    memcpy(sBoxColours, (const uint16_t *)sTexturePalette, sizeof(sBoxColours));
    sBoxText = false;
    for (unsigned i = 0; i < 26 * 4; ++i)
    {
        unsigned entry = MapEntry(map, size, 2 + i % 26, 15 + i / 26);
        unsigned address = chars + (entry & 1023) * 32, bank = entry >> 12;
        unsigned cx = (i % 26) * 8, cy = (i / 26) * 8;

        sBoxEntries[i] = (uint16_t)entry;
        if (address + 32 > 0x10000) address = 0;
        memcpy(sBoxWindow + i * 32, sMemory.vram + address, 32);
        for (unsigned p = 0; p < 64; ++p)
        {
            unsigned px = p & 7, py = p >> 3;
            unsigned sx = (entry & 1024) ? 7 - px : px, sy = (entry & 2048) ? 7 - py : py;
            unsigned index = (sBoxWindow[i * 32 + sy * 4 + sx / 2] >> ((sx & 1) * 4)) & 15;
            uint16_t texel = 0;

            /* The window's fill (index 15) and nothing (0) are the panel. */
            if (index && index != 15)
            {
                uint16_t c = sTexturePalette[bank * 16 + index];

                texel = (uint16_t)(((c >> 12) & 15) << 12 | ((c >> 7) & 15) << 8 | ((c >> 2) & 15) << 4 | 15);
                sBoxText = true;
            }
            data[CtrVideo_Texel(cx + px, cy + py, TEXT_TEX_W)] = texel;
        }
    }
    TexFilled(&sTextTex);
    ++sBoxTexVersion;
}

/* How far below its place the text box is this frame (its slide in). */
static float BoxOffset(void)
{
    float t;

    if (sBoxHidden) return BOX_HIDDEN;
    if (sBoxSlide < 0) return 0;
    /* An ease out, slowing to rest without overshoot. */
    t = 1.0f - (float)sBoxSlide / BOX_SLIDE_FRAMES;
    return BOX_HIDDEN * t * t * t;
}

/* Once a frame: whether the box waits out of sight, and its slide. */
static void BoxFrame(void)
{
    static bool sliding;
    bool slidingNow = false;

    if (!sBattle)
    {
        sBoxHidden = sliding = false;
        /* The party or the bag over the battle: back from them, the box is
         * where it was, not slid in again as for a new battle. */
        if (!sBoxDecided || !CtrVoxel_InBattle())
            sBoxDecided = sBoxBarsUp = false;
        sBoxSlide = -1;
        sBoxBattleFrames = 0;
        /* The next battle's message window is read afresh, and the last
         * one's text cleared: the next opens on the empty box, which only
         * clears the texture when it held a window. */
        sBoxText = false;
        if (sBoxEntries[0] != 0xFFFF && sTextTex.data)
        {
            memset(sTextTex.data, 0, TEXT_TEX_W * TEXT_TEX_H * 2);
            TexFilled(&sTextTex);
            ++sBoxTexVersion;
        }
        memset(sBoxEntries, 0xFF, sizeof(sBoxEntries));
        return;
    }
    ++sBoxBattleFrames;
    if (sBoxSlide >= 0 && ++sBoxSlide > BOX_SLIDE_FRAMES) sBoxSlide = -1;
    /* Only a battle over the voxel world, decided as it begins: the world is
     * there from its first frames, the camera's flight a little later. */
    if (!sBoxDecided)
    {
        /* Through the fade in, until there is a picture: the world, or a
         * message already in a 2D battle. */
        if (sBattleWorld && !sBoxText) sBoxHidden = true;
        if (sBattleWorld || sBoxText) sBoxDecided = true;
    }
    if (sBoxHidden)
    {
        bool barsGone = false;

        slidingNow = CtrBattleIntro_Sliding() != 0;
        {
            CtrVoxelBattleIntro intro;

            CtrVoxel_BattleIntro(&intro);
            if (intro.bars > 0.9f) sBoxBarsUp = true;
            barsGone = sBoxBarsUp && intro.bars < BOX_BARS_LEAVING;
        }
        /* In as the camera lands: the cinema bars leaving, the intro's
         * scenery done sliding, the first message begun, or, whatever
         * happens, after ten seconds. */
        if (barsGone || sBoxText || (sliding && !slidingNow) || sBoxBattleFrames > 600)
        {
            sBoxHidden = false;
            sBoxSlide = 0;
        }
        sliding = slidingNow;
    }
    /*
     * The player's trainer goes with it: its picture ends where the box
     * covers it. Out of sight while the box is (all of it: the picture is
     * taller than the box), up with it as it comes, arriving together.
     */
    {
        float offset = BoxOffset();

        CtrBattleHud_LiftPlayerTrainer((int)lroundf(offset > 0 ? offset * (BOX_TRAINER_HIDDEN / BOX_HIDDEN)
                                                               : offset / CTR_BATTLE_ZOOM));
    }
}

/*
 * The panel's art under the fade the message box's palette (BG palette 0) is
 * under, as a tint: the colour it goes towards and how far. A fade of every
 * palette in even steps is already a tint (UpdatePalette, sPaletteFade); the
 * battle's end fades by its own steps (BeginFastPaletteFade), which only the
 * palette shows - the text, drawn in the palette's colours, follows it, and
 * so must the panel. Read from white (colour 1) and the colour furthest from
 * it, as the info boxes are (3ds_battle_hud.c FadeOf).
 */
static void BoxFadeTint(C2D_ImageTint *tint)
{
    const uint16_t *unfaded = gPlttBufferUnfaded, *now = sMemory.palette;
    unsigned a = unfaded[1] & 0x7fff, fa = now[1] & 0x7fff, b = a, fb = fa;
    int best = 0;
    float k = 0, target[3];

    *tint = sTint;
    if (sPaletteFade > 0 || !memcmp(unfaded, now, 32)) return;
    for (unsigned i = 2; i < 16; ++i)
    {
        unsigned c = unfaded[i] & 0x7fff;
        int d = 0;

        for (int ch = 0; ch < 15; ch += 5)
        {
            int e = (int)((a >> ch) & 31) - (int)((c >> ch) & 31);
            d += e < 0 ? -e : e;
        }
        if (d > best)
        {
            best = d;
            b = c;
            fb = now[i] & 0x7fff;
        }
    }
    if (best == 0) return;
    /* How far, from the channel the two differ most in. */
    {
        int most = 0;

        for (int ch = 0; ch < 15; ch += 5)
        {
            int ua = (a >> ch) & 31, ub = (b >> ch) & 31, d = ua > ub ? ua - ub : ub - ua;

            if (d > most)
            {
                most = d;
                k = 1.0f - (float)((int)((fa >> ch) & 31) - (int)((fb >> ch) & 31)) / (float)(ua - ub);
            }
        }
    }
    if (k < 0.02f) return;
    if (k > 1) k = 1;
    /* Towards what: white's channels solved for it. */
    for (int ch = 0; ch < 3; ++ch)
    {
        float u = (a >> (ch * 5)) & 31, f = (fa >> (ch * 5)) & 31, t = (f - u * (1 - k)) / k;

        target[ch] = t < 0 ? 0 : t > 31 ? 255 : t * 255 / 31;
    }
    /*
     * How far the panel itself is, by its own colour (the window's fill,
     * colour 15): a fade in even steps takes every colour the same part of
     * the way, the battle's end takes the same amount off each channel, which
     * brings the panel's mid tones down well before white.
     */
    {
        unsigned u = unfaded[15] & 0x7fff, f = now[15] & 0x7fff;
        float moved = 0, way = 0;

        for (int ch = 0; ch < 3; ++ch)
        {
            float uc = (u >> (ch * 5)) & 31, fc = (f >> (ch * 5)) & 31, tc = target[ch] * 31 / 255;

            moved += fabsf(fc - uc);
            way += fabsf(tc - uc);
        }
        if (way > 0.5f)
            k = moved / way > 1 ? 1 : moved / way;
    }
    C2D_PlainImageTint(tint, C2D_Color32((uint8_t)target[0], (uint8_t)target[1], (uint8_t)target[2], 255), k);
}

/* The text box in HD, if BG0 shows the message box; false to draw the GBA's. */
static bool DrawBattleTextHd(unsigned bg)
{
    unsigned control = Reg(8 + bg * 2), size = control >> 14;
    unsigned map = ((control >> 8) & 31) * 0x800;
    unsigned chars = ((control >> 2) & 3) * 0x4000;
    unsigned scrollX = Reg(0x10 + bg * 4) & 511, scrollY = Reg(0x12 + bg * 4) & 511;
    int down = scrollY >= 256 ? 512 - (int)scrollY : -(int)scrollY;
    float y0, shift = sLayerShift;
    bool empty;

    if (bg != 0 || scrollX != 0 || down < 0 || down > 64 || (control & 128)) return false;
    /* The message window put on the box; or not yet, through the intro: the
     * box's own frame (textbox_map.bin) round its plain fill. */
    if ((MapEntry(map, size, 2, 15) & 1023) == 0x90 && (MapEntry(map, size, 27, 18) & 1023) == 0x90 + 103)
        empty = false;
    else if ((MapEntry(map, size, 2, 15) & 1023) == 0x0a && (MapEntry(map, size, 27, 18) & 1023) == 0x0a
             && (MapEntry(map, size, 0, 14) & 1023) == 0x03 && (MapEntry(map, size, 29, 19) & 1023) == 0x11)
        empty = true;
    else
        return false;
    if (!BoxTexture()) return false;
    if (empty)
    {
        /* Nothing to show; the window is read afresh when it is put there. */
        if (sBoxEntries[0] != 0xFFFF)
        {
            memset(sBoxEntries, 0xFF, sizeof(sBoxEntries));
            memset(sTextTex.data, 0, TEXT_TEX_W * TEXT_TEX_H * 2);
            TexFilled(&sTextTex);
            ++sBoxTexVersion;
        }
        sBoxText = false;
    }
    else
        BoxWindowSync(map, size, chars);

    /* Above the text box: windows, where the game put them. */
    {
        uint32_t before = sStats.tiles;
        DrawBattleCut(bg, 0, 240, 0, BATTLE_BAND_TOP, 0);
        if (sStats.tiles != before) sTextAbove = true;
    }
    y0 = sBoxArtY + down + BoxOffset();
    sBoxShown = sStats.frames;
    if (y0 >= CTR_GAME_HEIGHT) return true;
    /* Layers set the blend for BG0: its effects bit is in the key. */
    HudBlend(bg, (sBlendKey >> 8) & 1);
    ViewBase();
    {
        C2D_ImageTint art;
        const float cap = (float)sBoxCapW, h = (float)sBoxArtH;
        const Tex3DS_SubTexture left = {(u16)sBoxCapW, (u16)sBoxArtH, 0, 1, cap / BOX_TEX_W, 1 - h / BOX_TEX_H};
        const Tex3DS_SubTexture right = {(u16)sBoxCapW, (u16)sBoxArtH, cap / BOX_TEX_W, 1, 0, 1 - h / BOX_TEX_H};
        const float mu = (BOX_MIDDLE_X + 0.5f) / BOX_TEX_W;
        const Tex3DS_SubTexture middle = {1, (u16)sBoxArtH, mu, 1, mu, 1 - h / BOX_TEX_H};
        const Tex3DS_SubTexture text = {208, 32, 0, 1, 208.0f / TEXT_TEX_W, 0};

        BoxFadeTint(&art);

        C2D_DrawImageAt((C2D_Image){&sBoxTex, &left}, shift, y0, 0, &art, 1, 1);
        C2D_DrawImageAt((C2D_Image){&sBoxTex, &middle}, cap + shift, y0, 0, &art, CTR_GAME_WIDTH - 2 * cap, 1);
        C2D_DrawImageAt((C2D_Image){&sBoxTex, &right}, CTR_GAME_WIDTH - cap + shift, y0, 0, &art, 1, 1);
        /* The message window: GBA (16, 120), on the screen where the band puts it. */
        C2D_DrawImageAt((C2D_Image){&sTextTex, &text}, 16 + BOX_TEXT_SHIFT + shift,
                        (CTR_GAME_HEIGHT - 48) + 8 + down + BoxOffset(), 0, &sTint, 1, 1);
        sStats.tiles += 4;
    }
    C2D_Flush();
    BlendForget();
    return true;
}

static void DrawBattleText(unsigned bg)
{
    unsigned control = Reg(8 + bg * 2), size = control >> 14;
    unsigned map = ((control >> 8) & 31) * 0x800;
    unsigned chars = ((control >> 2) & 3) * 0x4000;
    bool color256 = (control & 128) != 0;
    unsigned scrollX = Reg(0x10 + bg * 4) & 511, scrollY = Reg(0x12 + bg * 4) & 511;
    unsigned rows = (size & 2) ? 64 : 32;
    /* The column under the left edge of the picture: the left cap. */
    unsigned first = scrollX >> 3;
    int ox = (int)(scrollX & 7), oy = (int)(scrollY & 7);
    int fill0 = 240 - BATTLE_BAND_CAP + VIEW_LEFT, fill1 = VIEW_RIGHT - BATTLE_BAND_CAP;
    int x0 = fill0, x1 = fill1, y0 = BATTLE_BAND_TOP, y1 = 160;

    if (DrawBattleTextHd(bg)) return;
    /* Above the text box: windows, where the game put them. */
    uint32_t before = sStats.tiles;
    DrawBattleCut(bg, 0, 240, 0, BATTLE_BAND_TOP, 0);
    if (sStats.tiles != before) sTextAbove = true;
    /* The box from its left cap, on the left edge of the screen... */
    DrawBattleCut(bg, VIEW_LEFT, fill0, BATTLE_BAND_TOP, 160, VIEW_LEFT);
    /* ...its right cap on the right edge... */
    DrawBattleCut(bg, fill1, VIEW_RIGHT, BATTLE_BAND_TOP, 160, VIEW_RIGHT - 240);
    /*
     * ...and its middle in between. A row is a frame of caps and a middle:
     * the five-tile rows of the message box have two cap tiles and then the
     * middle (numbered one after the cap), the three-tile rows of the menu
     * boxes one cap and then the middle itself. Column 2 tells which, since
     * the message window starts there and never holds the middle tile.
     */
    if (!ClipCell(&x0, &x1, &y0, &y1)) return;
    C2D_Flush();
    Scissor(x0, y0, x1, y1);
    ViewBase();
    for (int py = ((y0 + oy) & ~7) - oy; py < y1; py += 8)
    {
        unsigned row = ((unsigned)(py + (int)scrollY) >> 3) & (rows - 1);
        unsigned cap = MapEntry(map, size, first, row), inner = MapEntry(map, size, first + 1, row);
        unsigned middle, next = MapEntry(map, size, first + 2, row);
        int slot;

        if (!cap && !inner) continue;
        middle = next == inner ? inner : (inner & ~1023u) | ((inner + 1) & 1023);
        slot = EntrySlot(chars, middle, color256);
        if (slot < 0) continue;
        for (int px = ((x0 + ox) & ~7) - ox; px < x1; px += 8)
            DrawSlot(slot, px + CTR_VIEW_X + sLayerShift, py + CTR_VIEW_Y, middle & 1024, middle & 2048);
    }
    C2D_Flush();
    RestoreScissor();
}

/*
 * The level-up box (battle_script_commands.c, Cmd_drawlvlupbox): BG1 put in
 * front of BG0 while it is up. Magnified with the scene, it went down past
 * the screen's edge and the text box, drawn after the scene, covered it; it
 * goes with the text box instead, at 1:1 over it, its right edge on the
 * screen's as the GBA's is on the box's (DrawBattleTextLayer).
 */
static unsigned BattleBoxLayers(void)
{
    if (!sBattle || !(Reg(0) & 0x200) || (Reg(0xa) & 3) >= (Reg(8) & 3))
        return 0;
    return 1u << 1;
}

/*
 * The text box is not magnified: it keeps its own pixels on the bottom 48
 * lines, under a scene composed at CTR_BATTLE_ZOOM. The rectangle being
 * composed is carried from the scene's coordinates to the box's through the
 * screen, drawn there, and put back. The level-up box the same way, whole.
 */
static void DrawBattleTextLayer(unsigned bg)
{
    int viewX = sViewX, viewY = sViewY;
    float zoom = sZoom, offX = sOffX, offY = sOffY;
    int clipX0 = sClipX0, clipY0 = sClipY0, clipX1 = sClipX1, clipY1 = sClipY1;

    float shift = sLayerShift;

    sZoom = 1.0f;
    sOffX = sOffY = 0.0f;
    sLayerShift = shift * sShiftZoom;
    sViewX = CTR_BATTLE_X;
    sViewY = CTR_BATTLE_Y;
    sClipX0 = (int)roundf((clipX0 + viewX) * zoom + offX) - sViewX;
    sClipX1 = (int)roundf((clipX1 + viewX) * zoom + offX) - sViewX;
    sClipY0 = (int)roundf((clipY0 + viewY) * zoom + offY) - sViewY;
    sClipY1 = (int)roundf((clipY1 + viewY) * zoom + offY) - sViewY;
    if (bg == 0)
        DrawBattleText(bg);
    else
        DrawBattleCut(bg, VIEW_RIGHT - 240, VIEW_RIGHT, 0, 160, VIEW_RIGHT - 240);
    C2D_Flush();
    sZoom = zoom;
    sOffX = offX;
    sOffY = offY;
    sLayerShift = shift;
    sViewX = viewX;
    sViewY = viewY;
    sClipX0 = clipX0;
    sClipY0 = clipY0;
    sClipX1 = clipX1;
    sClipY1 = clipY1;
    RestoreScissor();
    ViewBase();
}

/* Whether a tile row of a background has anything to show across the screen. */
static bool BattleRowShown(unsigned map, unsigned size, unsigned chars, bool color256,
                           unsigned row, unsigned column, unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
        if (EntrySlot(chars, MapEntry(map, size, column + i, row), color256) >= 0)
            return true;
    return false;
}
