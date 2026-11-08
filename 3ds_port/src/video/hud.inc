/*
 * Part of 3ds_video.c. The battle's info boxes at the screen's resolution
 * (3ds_battle_hud.h): their texture and windows, HudFrame, HudDrawInScene and
 * HudDrawCrisp.
 */

/*
 * The battle's info boxes and text box at the screen's resolution
 * (3ds_battle_hud.h).
 *
 * Each info box battle_interface.c makes has a picture drawn for the top
 * screen; its sprites - the two halves and the bar - are left out wherever
 * the GBA would draw them, and the picture drawn instead where the left half
 * is. Normally that is after the scene, at the screen's own pixels
 * (HudDrawCrisp): nothing the battle draws goes over its info boxes, which
 * moves raise to the front while they play (UpdateOamPriorityInAllHealthboxes).
 * When something does - a background or a sprite in front of a box, a window
 * shaping the sprites - or the scene is composed per eye for the 3D slider,
 * the picture is drawn into the scene in the left half's place instead
 * (HudDrawInScene), magnified with it.
 *
 * A picture is two RGBA8 textures, 128 and 32 texels wide (40 KiB of linear
 * memory, where one 256 wide would take 64), made for the boxes of a battle
 * and given back after it.
 */
#define HUD_TEX_H 64
static const unsigned sHudPartW[2] = {128, 32};
static C3D_Tex sHudTex[CTR_HUD_BOXES][2];
static uint32_t sHudVersion[CTR_HUD_BOXES];
static bool sHudReady[CTR_HUD_BOXES];
static const CtrHudBox *sHud;
static bool sHudCrisp, sHudFailLogged;
static uint32_t sHudLastBattleFrame;
/* When making a picture last failed (0: not since the last battle). */
static uint32_t sHudFailFrame;
#define HUD_RETRY_FRAMES 120
/* Whether a background in front of the boxes drew something last frame. */
static bool sHudCovered;
/* This frame's box of each OAM entry (HudBoxOfTile), or -1: read once in
 * HudFrame, for every pass that asks (the 3D slider's slots compose the
 * sprites once a group). */
static int8_t sHudOamBox[128];

static void HudRelease(void)
{
    for (unsigned k = 0; k < CTR_HUD_BOXES; ++k)
    {
        for (unsigned part = 0; part < 2; ++part)
        {
            if (sHudTex[k][part].data) C3D_TexDelete(&sHudTex[k][part]);
            memset(&sHudTex[k][part], 0, sizeof(sHudTex[k][part]));
        }
        sHudReady[k] = false;
        sHudVersion[k] = 0;
    }
    sHudFailFrame = 0;
}

/*
 * The picture's rows that changed (the box's changedTop/Bottom; all of them
 * into a texture just made) into its two textures, a tile row of 8 at a time:
 * a bar going down is two or three tile rows. Tile by tile, each texel's
 * place from the Morton table - CtrVideo_Texel for every texel divided by the
 * texture's width each time, and the whole picture went up every frame of a
 * bar's slide: 1.6 ms a box in Azahar.
 */
static bool HudUpload(unsigned k, const CtrHudBox *box)
{
    unsigned top = box->changedTop & ~7u, bottom = (box->changedBottom + 7u) & ~7u;

    if (!sHudTex[k][0].data || !sHudTex[k][1].data || bottom > HUD_TEX_H || top >= bottom)
    {
        top = 0;
        bottom = HUD_TEX_H;
    }
    for (unsigned part = 0, x0 = 0; part < 2; x0 += sHudPartW[part], ++part)
    {
        C3D_Tex *tex = &sHudTex[k][part];
        uint32_t *data;

        if (!tex->data)
        {
            /* Not every frame: tried again a couple of seconds on. */
            if (sHudFailFrame && (int32_t)(sStats.frames - sHudFailFrame) < HUD_RETRY_FRAMES) return false;
            if (!C3D_TexInit(tex, sHudPartW[part], HUD_TEX_H, GPU_RGBA8))
            {
                memset(tex, 0, sizeof(*tex));
                /* Without its other part the box is not drawn: give back the
                 * 32 KiB of the first rather than refill it every frame. */
                if (part && sHudTex[k][0].data)
                {
                    C3D_TexDelete(&sHudTex[k][0]);
                    memset(&sHudTex[k][0], 0, sizeof(sHudTex[k][0]));
                }
                sHudFailFrame = sStats.frames | 1;
                if (!sHudFailLogged)
                    CtrLog_Write(CTR_LOG_ERROR, "video: no memory for an HD info box (linear free=%lu); "
                                 "the GBA's box is shown", (unsigned long)linearSpaceFree());
                sHudFailLogged = true;
                return false;
            }
            C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        }
        data = tex->data;
        for (unsigned ty = top; ty < bottom; ty += 8)
            for (unsigned tx = 0; tx < sHudPartW[part]; tx += 8)
            {
                /* Tiles run left to right, then down (CtrVideo_Texel). */
                uint32_t *tile = data + ((ty / 8) * (sHudPartW[part] / 8) + tx / 8) * 64;

                for (unsigned y = 0; y < 8; ++y)
                {
                    const uint32_t *row = box->pixels + (ty + y) * CTR_HUD_W + x0 + tx;
                    bool inside = ty + y < CTR_HUD_H;

                    for (unsigned x = 0; x < 8; ++x)
                        tile[sMorton[y * 8 + x]] = inside && x0 + tx + x < CTR_HUD_W ? row[x] : 0;
                }
            }
        TexFilled(tex);
    }
    sHudVersion[k] = box->version;
    CtrBattleHud_Uploaded(k);
    return true;
}

/*
 * Blend's state for a picture with soft edges: its colour over what is
 * there, and its coverage added to the surface's (Blend writes the source's
 * alone), so the edges and the shadows of the boxes and the text box do not
 * leave the surface see-through - through to the black under it - when it is
 * put on the screen. Not when the layer blends by the GBA's alpha effect,
 * which keeps Blend's own. BlendForget after drawing.
 */
static void HudBlend(unsigned layer, bool effects)
{
    unsigned control = Reg(0x50);

    Blend(layer, effects, false);
    if (effects && ((control >> 6) & 3) == 1 && (control & (1u << layer)) && ((control >> 8) & 63)) return;
    C2D_Flush();
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
}

/*
 * The tint a box's picture is drawn with: the palette fade its sprites are
 * under (the picture is unfaded), then the layer's own (Blend's, a brightness
 * effect). Two mixes towards a colour are one: strength 1 - (1-a)(1-b), the
 * colour the two weighted by what each adds.
 */
static void HudTint(unsigned k, C2D_ImageTint *tint)
{
    float a = sHud[k].fade;

    *tint = sTint;
    if (a <= 0) return;
    for (unsigned c = 0; c < 4; ++c)
    {
        float b = tint->corners[c].blend, both = 1 - (1 - a) * (1 - b);
        uint32_t layer = tint->corners[c].color, rgb[3];

        for (unsigned ch = 0; ch < 3; ++ch)
        {
            float box = (sHud[k].fadeColour >> (24 - ch * 8)) & 0xFF, own = (layer >> (ch * 8)) & 0xFF;

            rgb[ch] = (uint32_t)((box * a * (1 - b) + own * b) / both + 0.5f);
        }
        tint->corners[c].color = C2D_Color32(rgb[0], rgb[1], rgb[2], layer >> 24);
        tint->corners[c].blend = both;
    }
}

/* A box's picture at (x, y), `scale` times the screen's size. */
static void HudDrawPicture(unsigned k, float x, float y, float scale, bool smooth)
{
    C2D_ImageTint tint;

    HudTint(k, &tint);
    for (unsigned part = 0, x0 = 0; part < 2; x0 += sHudPartW[part], ++part)
    {
        const Tex3DS_SubTexture all = {(u16)sHudPartW[part], HUD_TEX_H, 0, 1, 1, 0};

        C3D_TexSetFilter(&sHudTex[k][part], smooth ? GPU_LINEAR : GPU_NEAREST, smooth ? GPU_LINEAR : GPU_NEAREST);
        C2D_DrawImageAt((C2D_Image){&sHudTex[k][part], &all}, x + x0 * scale, y, 0, &tint, scale, scale);
    }
}

/* The box whose sprites use OAM tile `tile`, drawn from its picture; or -1. */
static int HudBoxOfTile(unsigned tile)
{
    if (!sHud) return -1;
    for (unsigned k = 0; k < CTR_HUD_BOXES; ++k)
    {
        const CtrHudBox *box = &sHud[k];

        if (!sHudReady[k]) continue;
        if ((tile >= box->boxTile && tile < (unsigned)box->boxTile + box->boxTiles)
            || (box->barTiles && tile >= box->barTile && tile < (unsigned)box->barTile + box->barTiles))
            return (int)k;
    }
    return -1;
}

/* Whether OAM entry `i` is a box's left half, where its picture is drawn. */
static int HudLeftHalf(unsigned i)
{
    int k = sHudOamBox[i];

    return k >= 0 && (sMemory.oam[i * 4 + 2] & 1023) == sHud[k].boxTile ? k : -1;
}

/* A visible OAM entry's place in GBA coordinates, wrapped as the battle reads
 * it: a sprite drawn, or with `window` one in the OBJ window's mode. */
static bool OamPlace(unsigned i, bool window, unsigned *priority, int *x, int *y, int *w, int *h)
{
    const uint16_t *obj = sMemory.oam + i * 4;
    unsigned attr0 = obj[0], attr1 = obj[1], attr2 = obj[2], shape = attr0 >> 14;
    bool affine = (attr0 & 0x100) != 0;

    if ((!affine && (attr0 & 0x200)) || shape == 3 || (((attr0 >> 10) & 3) == 2) != window
        || ((attr0 >> 10) & 3) == 3) return false;
    *w = gCtrObjDims[shape][attr1 >> 14][0] * (affine && (attr0 & 0x200) ? 2 : 1);
    *h = gCtrObjDims[shape][attr1 >> 14][1] * (affine && (attr0 & 0x200) ? 2 : 1);
    *x = attr1 & 511;
    *y = attr0 & 255;
    if (*x + *w > 512) *x -= 512;
    if (*y + *h > 256) *y -= 256;
    /* Below the picture: where the GBA parks its unused entries (y 160),
     * tile 0 - an info box's first tile, as often as not. */
    if (*y >= 160 || *y + *h <= 0) return false;
    *priority = (attr2 >> 10) & 3;
    return true;
}

static bool HudEntry(unsigned i, unsigned *priority, int *x, int *y, int *w, int *h)
{
    return OamPlace(i, false, priority, x, y, w, h);
}

/*
 * Whether the boxes can be drawn after the scene: no window shapes the
 * sprites, no background in front of them drew anything, and no sprite in
 * front of a box overlaps its picture.
 */
/*
 * Whether the windows show a box's picture, (x, y) w x h in GBA pixels, as it
 * is: every part of the screen it covers shows sprites, as the compositor
 * reads the windows (WindowPartition). The battle keeps WIN0 and the OBJ
 * window on throughout (battle_bg.c); while its intro opens the scene through
 * WIN0 (BattleCurtain) a box inside the opening is as it would be anywhere.
 * A box the windows cut, or a sprite in window mode shaping the sprites, is
 * drawn into the scene with them.
 */
static unsigned WindowPartition(int top, int bottom, int rects[25][4], unsigned masks[25]);

static bool HudWindowsShow(int x, int y, int w, int h)
{
    unsigned control = Reg(0), count;
    int rects[25][4];
    unsigned masks[25];

    if (!(control & 0xE000)) return true;
    /* A sprite in window mode shapes the sprites only where it is (a stat
     * change's pattern over its battler): one apart from the box, or whose
     * window shows sprites too, leaves the box as it is. */
    if ((control & 0x8000) && !((Reg(0x4A) >> 8) & 0x10))
        for (unsigned i = 0; i < 128; ++i)
        {
            unsigned p;
            int ox, oy, ow, oh;

            if (OamPlace(i, true, &p, &ox, &oy, &ow, &oh) && ox < x + w && ox + ow > x && oy < y + h && oy + oh > y)
                return false;
        }
    if (y < 0) { h += y; y = 0; }
    if (y + h > 160) h = 160 - y;
    if (h <= 0) return true;
    count = WindowPartition(y, y + h, rects, masks);
    for (unsigned r = 0; r < count; ++r)
        if (rects[r][0] < x + w && rects[r][2] > x && !(masks[r] & 0x10)) return false;
    return true;
}

static bool HudCrispPossible(void)
{
    if (sHudCovered) return false;
    for (unsigned i = 0; i < 128; ++i)
    {
        unsigned p;
        int x, y, w, h, k = HudLeftHalf(i);

        if (k < 0 || !HudEntry(i, &p, &x, &y, &w, &h))
            continue;
        /* The picture's extent in GBA pixels. */
        w = (int)ceilf(CTR_HUD_W / CTR_BATTLE_ZOOM);
        h = (int)ceilf(CTR_HUD_H / CTR_BATTLE_ZOOM);
        if (!HudWindowsShow(x, y, w, h)) return false;
        for (unsigned j = 0; j < 128; ++j)
        {
            unsigned q;
            int ox, oy, ow, oh;

            if (j == i || sHudOamBox[j] >= 0) continue;
            if (!HudEntry(j, &q, &ox, &oy, &ow, &oh)) continue;
            if (q > p || (q == p && j > i)) continue;
            if (ox < x + w && ox + ow > x && oy < y + h && oy + oh > y) return false;
        }
    }
    return true;
}

static unsigned BattleBoxLayers(void);

/* After a scene: whether a background in front of the boxes drew in it. */
static void HudCoverUpdate(void)
{
    unsigned front = 4;

    sHudCovered = false;
    if (!sHud) return;
    for (unsigned i = 0; i < 128; ++i)
    {
        unsigned p;
        int x, y, w, h, k = HudLeftHalf(i);

        if (k >= 0 && HudEntry(i, &p, &x, &y, &w, &h)
            && p < front)
            front = p;
    }
    for (unsigned bg = 1; bg < 4; ++bg)
        if (sBgDrawn[bg] && (Reg(0) & (0x100u << bg)) && (Reg(8 + bg * 2) & 3) < front
            && !(BattleBoxLayers() & (1u << bg)))
            sHudCovered = true;
}

/* This frame's boxes, their pictures uploaded; given back once out of battle. */
static void HudFrame(void)
{
    sHud = NULL;
    memset(sHudOamBox, -1, sizeof(sHudOamBox));
    if (!sBattle)
    {
        if (sHudTex[0][0].data || sHudTex[1][0].data || sHudTex[2][0].data || sHudTex[3][0].data)
            if (sStats.frames - sHudLastBattleFrame > 60) HudRelease();
        return;
    }
    sHudLastBattleFrame = sStats.frames;
    sHud = CtrBattleHud_Boxes();
    if (!sHud) return;
    for (unsigned k = 0; k < CTR_HUD_BOXES; ++k)
    {
        if (!sHud[k].used) { sHudReady[k] = false; continue; }
        if (sHudReady[k] && sHudVersion[k] == sHud[k].version) continue;
        sHudReady[k] = HudUpload(k, &sHud[k]);
    }
    for (unsigned i = 0; i < 128; ++i)
        sHudOamBox[i] = (int8_t)HudBoxOfTile(sMemory.oam[i * 4 + 2] & 1023);
}

/*
 * The part of a box's picture over one of its halves, in the current view
 * (the scene's): each half draws what is over its own rectangle, so a box
 * whose halves the 3D slider's sprite groups put apart is still whole.
 */
static void HudDrawInScene(unsigned k, unsigned tile, int x, int y, int w, int h, bool effects)
{
    /* The right half is 64 pixels right of the left one (SpriteCB_HealthBoxOther). */
    int left = tile == sHud[k].boxTile ? x : x - 64;

    SpriteBatchFlush();
    HudBlend(4, effects);
    ViewBase();
    C2D_Flush();
    /* With each sprite at its depth (BattleSpriteShift), the half is moved too. */
    Scissor((int)floorf(x + sLayerShift), y, (int)ceilf(x + w + sLayerShift), y + h);
    HudDrawPicture(k, left + CTR_VIEW_X + sLayerShift, y + CTR_VIEW_Y, 1.0f / CTR_BATTLE_ZOOM, true);
    C2D_Flush();
    RestoreScissor();
    BlendForget();
    ++sStats.sprites;
}

/* Every box's picture over the scene at the screen's own pixels, the GBA's
 * order kept: the lower OAM entry in front. */
enum { HUD_FLAT, HUD_SPRITE_DEPTH, HUD_SCENERY_DEPTH };

static void HudDrawCrisp(unsigned depth)
{
    bool any = false;

    if (!sHud || !sHudCrisp) return;
    C2D_Flush();
    C2D_ViewReset();
    for (int i = 127; i >= 0; --i)
    {
        unsigned p;
        int x, y, w, h, k = HudLeftHalf((unsigned)i);
        float shift = 0;

        if (k < 0 || !HudEntry((unsigned)i, &p, &x, &y, &w, &h))
            continue;
        /* With the 3D slider up, this eye's: where the box's sprites would
         * be (BattleSpriteShift, as the slots place them), or the scenery's
         * when the scene is put behind the screen whole. Whole pixels. */
        if (depth == HUD_SPRITE_DEPTH) shift = BattleSpriteShift(y + h / 2);
        else if (depth == HUD_SCENERY_DEPTH) shift = BattleSceneryShift();
        if (!any) HudBlend(4, true);
        any = true;
        HudDrawPicture((unsigned)k, roundf((x + CTR_VIEW_X) * sZoom + sOffX + shift),
                       roundf((y + CTR_VIEW_Y) * sZoom + sOffY), 1, false);
        ++sStats.sprites;
    }
    C2D_Flush();
    if (any) BlendForget();
    ViewBase();
}
