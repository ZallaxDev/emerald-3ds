/* The battlers drawn from Black 2 / White 2 pictures (3ds_bw_mons.h): their
 * textures, the OAM entries that show them and their drawing. */

/*
 * The battlers drawn from Black 2 / White 2 pictures (3ds_bw_mons.h).
 *
 * A battler whose picture the player has is drawn, wherever an OAM entry
 * shows its sprite's tiles (its 64x64 box; a move's copy of it too), from the
 * picture's current frame instead: the box's centre, its affine matrix or
 * flip, its blend, and the palette fade its sprite is under (BwTint) all
 * apply to the picture, at BwScale, standing on the game's own picture's
 * feet.
 *
 * One RGBA8 texture a battler, the picture's size rounded up to a power of
 * two (at most 64 KiB for 128x128), refilled when the frame changes - every
 * six frames or so - and given back after the battle with the files.
 */
/* A front picture at BW's own pixels, one to one with the GBA's (BW's 256x192
 * and the GBA's 240x160 are near enough the same density); a back one at
 * the height of the game's own back picture - the layout is made for it -
 * between one and two times, as BW itself shows them larger. */
#define BW_BACK_MIN 1.0f
#define BW_BACK_MAX 2.0f
#define BW_RETRY_FRAMES 120
static C3D_Tex sBwTex[CTR_BW_BATTLERS];
static unsigned sBwTexW[CTR_BW_BATTLERS], sBwTexH[CTR_BW_BATTLERS];
static const CtrBwPic *sBwPic[CTR_BW_BATTLERS];
static uint32_t sBwVersion[CTR_BW_BATTLERS], sBwTick[CTR_BW_BATTLERS];
static int sBwShownFrame[CTR_BW_BATTLERS];
static bool sBwReady[CTR_BW_BATTLERS], sBwFailLogged;
static const CtrBwBattler *sBw;
/* This frame's battler of each OAM entry, or -1 (BwFrame). */
static int8_t sBwOamBattler[128];
static uint32_t sBwLastBattleFrame, sBwFailFrame;
static uint32_t *sBwPixels;

static void BwRelease(void)
{
    for (unsigned k = 0; k < CTR_BW_BATTLERS; ++k)
    {
        if (sBwTex[k].data) C3D_TexDelete(&sBwTex[k]);
        memset(&sBwTex[k], 0, sizeof(sBwTex[k]));
        sBwTexW[k] = sBwTexH[k] = 0;
        CtrBwPic_Release(k);
        sBwPic[k] = NULL;
        sBwReady[k] = false;
        sBwVersion[k] = 0;
    }
    free(sBwPixels);
    sBwPixels = NULL;
    sBwFailFrame = 0;
}

static unsigned BwTexDim(unsigned n)
{
    unsigned p = 8;

    while (p < n) p <<= 1;
    return p;
}

/* Frame `frame` of battler k's picture into its texture, tile by tile as the
 * info boxes' (HudUpload). */
static bool BwUpload(unsigned k, unsigned frame)
{
    unsigned w, h, anchorX2, feetY, tw, th;
    C3D_Tex *tex = &sBwTex[k];
    uint32_t *data;

    CtrBwPic_Size(sBwPic[k], &w, &h, &anchorX2, &feetY);
    tw = BwTexDim(w);
    th = BwTexDim(h);
    if (tex->data && (sBwTexW[k] != tw || sBwTexH[k] != th))
    {
        C3D_TexDelete(tex);
        memset(tex, 0, sizeof(*tex));
    }
    if (!sBwPixels) sBwPixels = malloc(sizeof(uint32_t) * CTR_BW_MAX_DIM * CTR_BW_MAX_DIM);
    if (!tex->data)
    {
        if (sBwFailFrame && (int32_t)(sStats.frames - sBwFailFrame) < BW_RETRY_FRAMES) return false;
        if (!sBwPixels || !C3D_TexInit(tex, (u16)tw, (u16)th, GPU_RGBA8))
        {
            memset(tex, 0, sizeof(*tex));
            sBwFailFrame = sStats.frames | 1;
            if (!sBwFailLogged)
                CtrLog_Write(CTR_LOG_ERROR, "video: no memory for a BW battler (linear free=%lu); "
                             "the game's sprite is shown", (unsigned long)linearSpaceFree());
            sBwFailLogged = true;
            return false;
        }
        C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        C3D_TexSetFilter(tex, GPU_NEAREST, GPU_NEAREST);
        sBwTexW[k] = tw;
        sBwTexH[k] = th;
    }
    if (!sBwPixels) return false;
    CtrBwPic_Decode(sBwPic[k], frame, sBwPixels, w);
    data = tex->data;
    for (unsigned ty = 0; ty < th; ty += 8)
        for (unsigned tx = 0; tx < tw; tx += 8)
        {
            /* Tiles run left to right, then down (CtrVideo_Texel). */
            uint32_t *tile = data + ((ty / 8) * (tw / 8) + tx / 8) * 64;

            for (unsigned y = 0; y < 8; ++y)
                for (unsigned x = 0; x < 8; ++x)
                {
                    unsigned px = tx + x, py = ty + y;

                    tile[sMorton[y * 8 + x]] = px < w && py < h ? sBwPixels[py * w + px] : 0;
                }
        }
    C3D_TexFlush(tex);
    return true;
}

/* This frame's battlers: pictures read, animations moved on a frame, the
 * frame shown uploaded, and the OAM entries that show each one. */
static void BwFrame(void)
{
    bool live = false;

    memset(sBwOamBattler, -1, sizeof(sBwOamBattler));
    /* Read whatever the compositor is composing: the battle's intro and the
     * screens staged before its own VBlank callback is up show the battlers
     * too. Out of a battle, every battler is inactive. */
    sBw = CtrBwMons_Battlers();
    for (unsigned k = 0; sBw && k < CTR_BW_BATTLERS; ++k)
        if (sBw[k].active && (sBw[k].tile != 0xFFFF || sBw[k].copyBg)) live = true;
    if (!live)
    {
        sBw = NULL;
        if ((sBwTex[0].data || sBwTex[1].data || sBwTex[2].data || sBwTex[3].data || sBwPic[0] || sBwPic[1]
             || sBwPic[2] || sBwPic[3])
            && sStats.frames - sBwLastBattleFrame > 60)
            BwRelease();
        return;
    }
    sBwLastBattleFrame = sStats.frames;
    for (unsigned k = 0; k < CTR_BW_BATTLERS; ++k)
    {
        const CtrBwBattler *b = &sBw[k];
        unsigned frame;

        if (!b->active)
        {
            sBwReady[k] = false;
            continue;
        }
        if (sBwVersion[k] != b->version)
        {
            sBwPic[k] = NULL;
            sBwVersion[k] = b->version;
            sBwTick[k] = 0;
            sBwShownFrame[k] = -1;
            sBwReady[k] = false;
        }
        /* Read on the reader thread (3ds_bw_mons.c): the game's picture is
         * shown until it is in. */
        if (!sBwPic[k]) sBwPic[k] = CtrBwPic_Get(k, b->path);
        if (!sBwPic[k]) continue;
        if (!b->paused) ++sBwTick[k];
        frame = CtrBwPic_FrameAt(sBwPic[k], sBwTick[k]);
        if (!sBwReady[k] || (int)frame != sBwShownFrame[k])
        {
            sBwReady[k] = BwUpload(k, frame);
            sBwShownFrame[k] = sBwReady[k] ? (int)frame : -1;
        }
    }
    for (unsigned i = 0; i < 128; ++i)
    {
        unsigned attr0 = sMemory.oam[i * 4], attr1 = sMemory.oam[i * 4 + 1], tile = sMemory.oam[i * 4 + 2] & 1023;

        /* A battler's sprite is a 64x64 square. */
        if ((attr0 >> 14) != 0 || (attr1 >> 14) != 3) continue;
        for (unsigned k = 0; k < CTR_BW_BATTLERS; ++k)
            if (sBwReady[k] && sBw[k].active && sBw[k].tile == tile)
            {
                sBwOamBattler[i] = (int8_t)k;
                break;
            }
    }
}

/*
 * The tint a battler's picture is drawn with. Its sprite's palette is the
 * game's, faded or blended by the battle (a status flash, a move's colour,
 * the screen fading out): every colour taken some way towards one colour.
 * How far, and towards what, is read back from the palette shown against
 * the unfaded one - fitted over its fifteen colours - and the picture
 * taken the same way. A fade of every palette in even steps is the layer's
 * own tint already (sPaletteFade). Then the layer's own, as HudTint.
 */
static void BwTint(unsigned base, bool transformed, C2D_ImageTint *tint)
{
    const uint16_t *now = sMemory.palette + base, *ref = gPlttBufferUnfaded + base;
    float a = 0, colour[3] = {0, 0, 0};

    *tint = sTint;
    if (sPaletteFade <= 0 && memcmp(now, ref, 32))
    {
        float mu[3] = {0}, mf[3] = {0}, slopes = 0;
        unsigned used = 0;

        for (unsigned i = 1; i < 16; ++i)
            for (unsigned ch = 0; ch < 3; ++ch)
            {
                mu[ch] += ((ref[i] >> (ch * 5)) & 31) / 15.0f;
                mf[ch] += ((now[i] >> (ch * 5)) & 31) / 15.0f;
            }
        for (unsigned ch = 0; ch < 3; ++ch)
        {
            float cov = 0, var = 0;

            for (unsigned i = 1; i < 16; ++i)
            {
                float u = (float)((ref[i] >> (ch * 5)) & 31) - mu[ch], f = (float)((now[i] >> (ch * 5)) & 31) - mf[ch];

                cov += u * f;
                var += u * u;
            }
            if (var < 4.0f) continue;
            slopes += cov / var;
            ++used;
        }
        if (used)
        {
            a = 1.0f - slopes / used;
            if (a < 0) a = 0;
            if (a > 1) a = 1;
        }
        if (a > 0.02f)
            for (unsigned ch = 0; ch < 3; ++ch)
            {
                float t = (mf[ch] - (1.0f - a) * mu[ch]) / a;

                colour[ch] = (t < 0 ? 0 : t > 31 ? 31 : t) * (255.0f / 31.0f);
            }
        else
            a = 0;
    }
    /* Transform washes the sprite's palette 6/16 towards white for good
     * (HandleSpeciesGfxDataChange), the unfaded one too: that wash first. */
    if (transformed)
    {
        float w = 6.0f / 16.0f, both = 1 - (1 - w) * (1 - a);

        for (unsigned ch = 0; ch < 3; ++ch)
            colour[ch] = (255.0f * w * (1 - a) + colour[ch] * a) / both;
        a = both;
    }
    if (a <= 0) return;
    for (unsigned c = 0; c < 4; ++c)
    {
        float b = tint->corners[c].blend, both = 1 - (1 - a) * (1 - b);
        uint32_t layer = tint->corners[c].color, rgb[3];

        for (unsigned ch = 0; ch < 3; ++ch)
        {
            float own = (layer >> (ch * 8)) & 0xFF;

            rgb[ch] = (uint32_t)((colour[ch] * a * (1 - b) + own * b) / both + 0.5f);
        }
        tint->corners[c].color = C2D_Color32(rgb[0], rgb[1], rgb[2], layer >> 24);
        tint->corners[c].blend = both;
    }
}

/*
 * Battler k's picture where OAM entry `i` (at x, y, its box boxW x boxH)
 * shows its sprite: about the box's centre, through the entry's affine
 * matrix or flip, cut where a fainting opponent's sprite is erased.
 */
static float BwScale(unsigned k)
{
    const CtrBwBattler *b = &sBw[k];
    unsigned w, h, anchorX2, feetY, topY;
    float scale;

    if (!b->back) return 1.0f;
    CtrBwPic_Size(sBwPic[k], &w, &h, &anchorX2, &feetY);
    topY = CtrBwPic_TopY(sBwPic[k]);
    if (feetY <= topY || b->feetY <= b->topY) return BW_BACK_MIN;
    scale = (float)(b->feetY - b->topY + 1) / (float)(feetY - topY + 1);
    return scale < BW_BACK_MIN ? BW_BACK_MIN : scale > BW_BACK_MAX ? BW_BACK_MAX : scale;
}

/*
 * A move's copy of a battler on BG1 or BG2 (MoveBattlerSpriteToBG), drawn
 * from its picture while the battler's sprite is hidden: the game moves and
 * fades that copy instead of the sprite (Leech Life, Faint Attack, Memento).
 * The copy's 64x64 sits at the top-left of the layer's map, so its centre
 * is (32, 32) less the layer's scroll; it takes the layer's blend and the
 * fade of its BG palette. Drawn with the sprites of the layer's priority,
 * under them all, as the layer is under them. The line-by-line scroll some
 * moves wave the copy with is not followed: the picture moves whole.
 */
static void BwDrawCopies(unsigned priority, bool effects)
{
    unsigned display = Reg(0);

    if (!sBw) return;
    for (unsigned k = 0; k < CTR_BW_BATTLERS; ++k)
    {
        const CtrBwBattler *b = &sBw[k];
        unsigned n = b->copyBg, w, h, anchorX2, feetY;
        int x, y;
        float scale, left, top;
        C2D_ImageTint tint;

        if (!b->active || !n || n > 2 || !b->spriteHidden || !sBwReady[k] || !sBwPic[k]) continue;
        if (!(display & (0x100u << n)) || (Reg(8 + n * 2) & 3) != priority) continue;
        x = 32 - (int)(Reg(0x10 + n * 4) & 511);
        y = 32 - (int)(Reg(0x12 + n * 4) & 511);
        while (x < -256) x += 512;
        while (x >= 256) x -= 512;
        while (y < -64) y += 256;
        while (y >= 192) y -= 256;
        scale = BwScale(k);
        CtrBwPic_Size(sBwPic[k], &w, &h, &anchorX2, &feetY);
        left = x - 32 - (anchorX2 / 2.0f) * scale + b->centreX2 / 2.0f;
        top = y - 32 - (feetY + 1.0f) * scale + b->feetY + 1.0f;
        if (left + w * scale <= sClipX0 || left >= sClipX1 || top + h * scale <= sClipY0 || top >= sClipY1) continue;
        {
            const Tex3DS_SubTexture sub = {(u16)w, (u16)h, 0.0f, 1.0f, (float)w / sBwTexW[k],
                                           1.0f - (float)h / sBwTexH[k]};

            SpriteBatchFlush();
            Blend(n, effects, false);
            BwTint((b->copyPalette & 15) * 16, b->transformed, &tint);
            ViewBase();
            C2D_DrawImageAt((C2D_Image){&sBwTex[k], &sub}, left + CTR_VIEW_X + sLayerShift, top + CTR_VIEW_Y, 0,
                            &tint, scale, scale);
            C2D_Flush();
            ++sStats.sprites;
        }
    }
}

static void BwDrawInScene(unsigned k, unsigned i, int x, int y, unsigned boxW, unsigned boxH, bool affine,
                          bool effects, unsigned mode)
{
    const CtrBwBattler *b = &sBw[k];
    unsigned attr1 = sMemory.oam[i * 4 + 1], attr2 = sMemory.oam[i * 4 + 2];
    unsigned w, h, anchorX2, feetY;
    float left, top, cx, cy;
    bool clip = !affine && b->clipRows < 64;
    C2D_ImageTint tint;
    C3D_Mtx matrix;
    float scale = BwScale(k);

    CtrBwPic_Size(sBwPic[k], &w, &h, &anchorX2, &feetY);
    {
        const Tex3DS_SubTexture sub = {(u16)w, (u16)h, 0.0f, 1.0f, (float)w / sBwTexW[k],
                                       1.0f - (float)h / sBwTexH[k]};

        /* The picture's top-left from the box's centre, in GBA pixels: its
         * middle on the game's picture's middle, its feet on its feet. */
        left = -(anchorX2 / 2.0f) * scale + (b->centreX2 / 2.0f - 32.0f);
        top = -(feetY + 1.0f) * scale + (b->feetY + 1.0f - 32.0f);
        cx = x + CTR_VIEW_X + sLayerShift + boxW / 2.0f;
        cy = y + CTR_VIEW_Y + boxH / 2.0f;
        if (clip && y + b->clipRows <= sClipY0) return;
        SpriteBatchFlush();
        Blend(4, effects, mode == 1);
        BwTint(256 + ((attr2 >> 12) & 15) * 16, b->transformed, &tint);
        C2D_Flush();
        if (clip) Scissor(sClipX0, sClipY0, sClipX1, sClipY1 < y + b->clipRows ? sClipY1 : y + b->clipRows);
        Mtx_Identity(&matrix);
        if (affine)
        {
            unsigned index = ((attr1 >> 9) & 31) * 16;
            float pa = (int16_t)sMemory.oam[index + 3] / 256.0f;
            float pb = (int16_t)sMemory.oam[index + 7] / 256.0f;
            float pc = (int16_t)sMemory.oam[index + 11] / 256.0f;
            float pd = (int16_t)sMemory.oam[index + 15] / 256.0f;
            float det = pa * pd - pb * pc;

            if (fabsf(det) < 0.00001f) { if (clip) RestoreScissor(); return; }
            matrix.r[0] = FVec4_New(pd / det, -pb / det, 0, cx);
            matrix.r[1] = FVec4_New(-pc / det, pa / det, 0, cy);
        }
        else
        {
            matrix.r[0] = FVec4_New((attr1 & 0x1000) ? -1.0f : 1.0f, 0, 0, cx);
            matrix.r[1] = FVec4_New(0, (attr1 & 0x2000) ? -1.0f : 1.0f, 0, cy);
        }
        ViewAffine(&matrix);
        C2D_DrawImageAt((C2D_Image){&sBwTex[k], &sub}, left, top, 0, &tint, scale, scale);
        C2D_Flush();
        if (clip) RestoreScissor();
        ViewBase();
        ++sStats.sprites;
    }
}

