/*
 * Part of 3ds_video.c. Tiles as quads: DrawTile, the view helpers (ViewBase,
 * ViewAffine), TileWalk's batches, text spans, empty walks and the scissor.
 */

static void DrawTile(unsigned address, unsigned bank, bool color256,
                     float x, float y, bool flipX, bool flipY)
{
    int slot = GetTileSlot(address, bank, color256);
    if (slot >= 0) DrawSlot(slot, x, y, flipX, flipY);
}

/*
 * Walks the tiles of a text background that cover [left, right) x [top, bottom)
 * of the screen. With a mirror axis (given doubled, -1 for none) each tile is
 * drawn reflected about it instead, flipped, so the region it lands on becomes
 * the mirror image of the one walked.
 */
/* Extra horizontal displacement of what DrawTextSpanAt draws, for a band that
 * is moved as a whole (the battle text box, DrawBattleText). */
static int sSpanShiftX, sSpanShiftY;

/* The base transform of everything composed: the current zoom. */
static void ViewBase(void)
{
    C2D_ViewReset();
    if (sZoom != 1.0f)
    {
        C2D_ViewTranslate(sOffX, sOffY);
        C2D_ViewScale(sZoom, sZoom);
    }
}

/* An affine layer's or sprite's own matrix, on top of the zoom. */
static void ViewAffine(C3D_Mtx *matrix)
{
    for (unsigned r = 0; r < 2; ++r)
        matrix->r[r] = FVec4_Scale(matrix->r[r], sZoom);
    matrix->r[0].w += sOffX;
    matrix->r[1].w += sOffY;
    C2D_ViewRestore(matrix);
}

/* The cells' and sprites' own vertex buffer (FastInit). */
typedef struct { int16_t x, y; float s, t; } FastVertex;
static FastVertex *sFastVertices;
static uint16_t *sFastIndices;
static unsigned sFastUsed;
static bool sFastReady;
static bool SpriteBatchRoom(unsigned tiles);
static void SpriteBatchFlush(void);
static void FastQuad(FastVertex *v, int x, int y, int slot, bool flipX, bool flipY);
static unsigned sSpriteCount;

/* The same tint at the four corners: what a batch can carry. */
static bool TintUniform(const C2D_ImageTint *tint)
{
    for (unsigned c = 1; c < 4; ++c)
        if (tint->corners[c].color != tint->corners[0].color || tint->corners[c].blend != tint->corners[0].blend)
            return false;
    return true;
}

/*
 * A walk's tiles in one draw, as sprites are (SpriteBatchFlush): the battle
 * scene's layers were ~1000 Citro2D quads a frame. The quads are in the
 * coordinates DrawSlot takes - Citro2D's view (ViewBase: a composition's zoom
 * and offset) is a shader uniform the GPU applies to both - so whole pixels
 * there batch, at any zoom. The tiles of one walk never overlap, so the few
 * left to Citro2D may be drawn in any order with them. TileWalkBegin says
 * whether this walk can batch at all; TileWalkSlot draws one tile;
 * TileWalkEnd sends them.
 */
static bool TileWalkBegin(void)
{
    return sFastReady && TintUniform(&sTint);
}

static void TileWalkSlot(bool batch, int slot, float x, float y, bool flipX, bool flipY)
{
    if (batch && (float)(int)x == x && (float)(int)y == y && SpriteBatchRoom(1))
    {
        if (++sStats.tiles >= DRAW_BUDGET)
        {
            Error(2, "GPU geometry budget exceeded");
            return;
        }
        FastQuad(sFastVertices + sFastUsed * 4, (int)x, (int)y, slot, flipX, flipY);
        ++sFastUsed;
        ++sSpriteCount;
        return;
    }
    DrawSlot(slot, x, y, flipX, flipY);
}

static void TileWalkEnd(bool batch)
{
    if (batch)
        SpriteBatchFlush();
}

static bool VramChangedAfter(unsigned address, unsigned bytes, uint32_t t);

/*
 * A walk that drew nothing - the field's text layer, most of the time - is
 * not walked again while nothing it read can have changed: the same layer,
 * scroll, rectangle and placement, and no change since to its tilemap, its
 * tiles or the background palettes. 1500 entries a frame for nothing was a
 * fifth of the voxel overlay.
 */
typedef struct
{
    bool valid;
    unsigned control, scrollX, scrollY;
    int left, right, top, bottom, mirrorX2, mirrorY2, spanX, spanY;
    float shift, zoom, offX, offY;
    uint32_t token;
} EmptyWalk;
static EmptyWalk sEmptyWalk[4];

static bool EmptyWalkHolds(const EmptyWalk *w, const EmptyWalk *now)
{
    unsigned control = now->control, size = control >> 14;
    unsigned map = ((control >> 8) & 31) * 0x800, chars = ((control >> 2) & 3) * 0x4000;
    unsigned mapBytes = 0x800 << ((size & 1) + ((size >> 1) & 1));
    unsigned charBytes = (control & 128) ? 0x10000 : 0x8000;

    if (!w->valid || w->control != now->control || w->scrollX != now->scrollX || w->scrollY != now->scrollY
        || w->left != now->left || w->right != now->right || w->top != now->top || w->bottom != now->bottom
        || w->mirrorX2 != now->mirrorX2 || w->mirrorY2 != now->mirrorY2 || w->spanX != now->spanX
        || w->spanY != now->spanY || w->shift != now->shift || w->zoom != now->zoom || w->offX != now->offX
        || w->offY != now->offY)
        return false;
    if (chars + charBytes > 0x10000) charBytes = 0x10000 - chars;
    return sBgPaletteStamp <= w->token && sTilemapBankStamp <= w->token
        && !VramChangedAfter(map, mapBytes, w->token) && !VramChangedAfter(chars, charBytes, w->token);
}

static void DrawTextSpanAt(unsigned bg, int left, int right, int top, int bottom,
                           int mirrorX2, int mirrorY2)
{
    unsigned control = Reg(8 + bg * 2), size = control >> 14;
    unsigned map = ((control >> 8) & 31) * 0x800;
    unsigned chars = ((control >> 2) & 3) * 0x4000;
    bool color256 = (control & 128) != 0;
    unsigned scrollX = Reg(0x10 + bg * 4) & 511, scrollY = Reg(0x12 + bg * 4) & 511;

    /*
     * A tilemap entry repeats across the layer: the field overlays are mostly
     * one blank entry, and a viewport of 400x240 walks 1500 of them per layer
     * per frame. This memo turns a repeat into one comparison instead of a
     * hash probe and a revalidation, which is what a 60fps overworld needs on
     * an Old 3DS. The stamp makes it valid for this call only, so every entry
     * is still revalidated once per layer per frame.
     */
    static uint32_t sMemoStamp[1024];
    static uint16_t sMemoEntry[1024];
    static int16_t sMemoSlot[1024];
    static uint32_t sStamp;
    uint32_t stamp = ++sStamp;
    bool batch;
    uint32_t tilesBefore = sStats.tiles;
    EmptyWalk key = {true, control, scrollX, scrollY, left, right, top, bottom, mirrorX2, mirrorY2,
                     sSpanShiftX, sSpanShiftY, sLayerShift, sZoom, sOffX, sOffY,
                     /* Conservative: a block changed later this frame is a change next frame. */
                     sStats.frames};
    /* Not in a battle: its text box is walked in several cuts a frame, and a
     * cut never meets the last one's key. */
    bool cached = bg < 4 && !sBattle;
    bool empty = cached && EmptyWalkHolds(&sEmptyWalk[bg], &key);

    if (empty)
        return;
    /* The key below records this frame's token for the tilemap, so its
     * blocks are compared now (the contract, vram.inc): Read16 does not, and a
     * map changed and changed back unseen would keep a stale "empty". */
    if (cached)
        VramSyncRange(map, 0x800u << ((size & 1) + ((size >> 1) & 1)));
    batch = TileWalkBegin();

    ViewBase();
    for (int y = top / 8 - 1; y * 8 <= bottom; ++y)
    {
        int py = y * 8 - (int)(scrollY & 7);
        unsigned row = (unsigned)(y + (int)(scrollY / 8));
        unsigned rowBase;

        if (py + 8 <= top || py >= bottom) continue;
        rowBase = map + CtrVideo_TextMapOffset(0, row, size);
        for (int x = left / 8 - 1; x * 8 <= right; ++x)
        {
            int px = x * 8 - (int)(scrollX & 7);
            unsigned column = (unsigned)(x + (int)(scrollX / 8)) & ((size & 1) ? 63u : 31u);
            unsigned entry, index;
            int slot;

            /* Inside the GBA area the wrap-around is kept exactly as on
             * hardware; only the margins are denied to a narrow background. */
            if (px + 8 <= left || px >= right) continue;
            /* Columns past 31 live in the next screenblock, 1024 entries on. */
            unsigned entryAddress = rowBase + (column & 31) * 2 + (column >> 5) * 2048;
            unsigned bank;

            entry = Read16(entryAddress);
            index = entry & 1023;
            bank = color256 ? 0 : TilemapBankAt(entryAddress, entry);
            if (bank)
            {
                slot = BankedTileSlot(bank, entry, chars);
            }
            else if (sMemoStamp[index] == stamp && sMemoEntry[index] == entry)
            {
                slot = sMemoSlot[index];
            }
            else
            {
                unsigned address = chars + index * (color256 ? 64 : 32);
                if (address >= 0x10000) { Error(4, "BG character address exceeds 64 KiB"); continue; }
                slot = GetTileSlot(address, entry >> 12, color256);
                sMemoStamp[index] = stamp;
                sMemoEntry[index] = (uint16_t)entry;
                sMemoSlot[index] = (int16_t)slot;
            }
            if (slot < 0) continue;
            {
                bool flipX = (entry & 1024) != 0, flipY = (entry & 2048) != 0;
                int dx = px, dy = py;

                if (mirrorX2 >= 0) { dx = mirrorX2 - px - 8; flipX = !flipX; }
                if (mirrorY2 >= 0) { dy = mirrorY2 - py - 8; flipY = !flipY; }
                TileWalkSlot(batch, slot, dx + sSpanShiftX + CTR_VIEW_X + sLayerShift,
                             dy + sSpanShiftY + CTR_VIEW_Y, flipX, flipY);
            }
        }
    }
    TileWalkEnd(batch);
    if (cached)
    {
        key.valid = sStats.tiles == tilesBefore;
        sEmptyWalk[bg] = key;
    }
}

static void DrawTextSpan(unsigned bg, int left, int right, int top, int bottom)
{
    DrawTextSpanAt(bg, left, right, top, bottom, -1, -1);
}

/*
 * The scissor the window partition set for the rectangle being composed, so a
 * pass that narrows it can put it back.
 */
static bool sScissored;

static void Scissor(int x0, int y0, int x1, int y1)
{
    float sx0 = (x0 + CTR_VIEW_X) * sZoom + sOffX, sx1 = (x1 + CTR_VIEW_X) * sZoom + sOffX;
    float sy0 = (y0 + CTR_VIEW_Y) * sZoom + sOffY, sy1 = (y1 + CTR_VIEW_Y) * sZoom + sOffY;

    if (sLayerOnEye)
    {
        /* A screen target is tilted: its x runs up the screen and its y from
         * the screen's right edge. The view here is the widened one, the
         * layers sLayerOrigin across of it (BattleSceneryStraightDraw). */
        sx0 += sLayerOrigin;
        sx1 += sLayerOrigin;
        if (sx0 < 0) sx0 = 0;
        if (sy0 < 0) sy0 = 0;
        if (sx1 > CTR_GAME_WIDTH) sx1 = CTR_GAME_WIDTH;
        if (sy1 > CTR_GAME_HEIGHT) sy1 = CTR_GAME_HEIGHT;
        C3D_SetScissor(GPU_SCISSOR_NORMAL, (unsigned)(CTR_GAME_HEIGHT - roundf(sy1)),
                       (unsigned)(CTR_GAME_WIDTH - roundf(sx1)), (unsigned)(CTR_GAME_HEIGHT - roundf(sy0)),
                       (unsigned)(CTR_GAME_WIDTH - roundf(sx0)));
        return;
    }
    if (sx0 < 0) sx0 = 0;
    if (sy0 < 0) sy0 = 0;
    C3D_SetScissor(GPU_SCISSOR_NORMAL, (unsigned)roundf(sx0), (unsigned)(sSurfaceH - roundf(sy1)),
                   (unsigned)roundf(sx1), (unsigned)(sSurfaceH - roundf(sy0)));
}

static void RestoreScissor(void)
{
    if (sScissored) Scissor(sClipX0, sClipY0, sClipX1, sClipY1);
    else C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
}
