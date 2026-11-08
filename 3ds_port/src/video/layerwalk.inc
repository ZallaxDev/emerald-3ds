/*
 * Part of 3ds_video.c. Layer textures' walk: which cells changed
 * (LayerRenderCells, per cell), LayersRender, and the layers drawn out
 * (DrawLayerRect, DrawEdgeBand).
 */

static unsigned LayerBanks(unsigned bg);
#ifdef CTR_AUDIT_BITS
static unsigned CtrDumpOff(void);
#endif

/*
 * The palettes of a layer's cells as they were at token t. In a battle by the
 * banks its map names, and the tile banks' only with a cell drawn from one:
 * Surf turns its wave's colours every four frames, and every layer with a
 * texture would walk all its cells for them. Rows whose map changed are walked
 * anyway, so the map as it is now says what the others are made of. Every
 * writer of sBgPaletteStamp writes one of the stamps looked at here.
 * nocache.txt bit 268435456: any change, as before (A/B frames).
 */
static bool LayerPalettesHold(unsigned bg, bool watch, uint32_t t)
{
    unsigned banks;

    if (sBgPaletteStamp <= t) return true;
#ifdef CTR_AUDIT_BITS
    if (CtrDumpOff() & 268435456u) return false;
#endif
    if (!watch || sBgOtherStamp > t || (sBankColourStamp > t && sCellsBanked[bg])) return false;
    banks = LayerBanks(bg);
    for (unsigned b = 0; b < 16; ++b)
        if ((banks & (1u << b)) && sBgBankStamp[b] > t) return false;
    return true;
}

/*
 * Which colours of each bank moved after token t, for a layer whose bytes, map
 * and banks did not: false if something else may have, and all its cells are
 * walked. Every writer of sBgBankStamp stamps the colours it changed; a tile
 * bank's colours reach only cells drawn from a bank, and with one of those there
 * is no pass. nocache.txt bit 2097152: none, every change walks all the cells
 * (A/B frames).
 */
static bool LayerColoursMoved(unsigned bg, uint32_t t, uint16_t moved[16], unsigned *any)
{
    unsigned banks;

#ifdef CTR_AUDIT_BITS
    if (CtrDumpOff() & (268435456u | 2097152u)) return false;
#endif
    if (sBgOtherStamp > t || sCellsBanked[bg]) return false;
    banks = LayerBanks(bg);
    *any = 0;
    for (unsigned b = 0; b < 16; ++b)
    {
        unsigned colours = 0;

        if ((banks & (1u << b)) && sBgBankStamp[b] > t)
            for (unsigned index = 1; index < 16; ++index)
                if (sBgColourStamp[b][index] > t) colours |= 1u << index;
        moved[b] = (uint16_t)colours;
        *any |= colours;
    }
    return true;
}

/* Cells first to last, of a line of `count` that wraps, as bits. */
static uint64_t CellsMask(int first, int last, unsigned count)
{
    uint64_t mask = 0;

    if ((unsigned)(last - first) + 1 >= count) return ~(uint64_t)0;
    for (int i = first; i <= last; ++i)
        mask |= (uint64_t)1 << ((unsigned)i & (count - 1));
    return mask;
}

/*
 * The columns and rows of its cells this frame can show: all of them, but for a
 * large layer of 64 columns and 32 rows, the only kind nothing but a plan reads
 * and whose plans read the map where the scroll puts it (BattleMapPlan). There,
 * what its plans have asked for so far, at this frame's scroll, and a cell around
 * it: a plan's tiles start and end up to seven pixels past its clip. Asked for by
 * the plans and not worked out from the view, a window cannot fall short of a
 * target or a zoom: the few clips there are have all been seen within a frame or
 * two of the layer's first. Until then, no window. nocache.txt bit 4194304: no
 * window, the pass looks at every cell whose colours moved (A/B frames).
 */
static void LayerWindow(unsigned bg, bool plain, unsigned rows, unsigned columns, uint64_t *shownRows,
                        uint64_t *shownColumns)
{
    const LayerTexture *layer = &sLayers[bg];
    int scrollX = (int)(Reg(0x10 + bg * 4) & 511), scrollY = (int)(Reg(0x12 + bg * 4) & 511);

    *shownRows = *shownColumns = ~(uint64_t)0;
    if (!plain || rows != 32 || layer->readX0 >= layer->readX1 || BattleTerrainSplit(bg)) return;
#ifdef CTR_AUDIT_BITS
    if (CtrDumpOff() & 4194304u) return;
#endif
    *shownRows = CellsMask((scrollY + layer->readY0 - 8) >> 3, (scrollY + layer->readY1 + 7) >> 3, rows);
    *shownColumns = CellsMask((scrollX + layer->readX0 - 8) >> 3, (scrollX + layer->readX1 + 7) >> 3, columns);
}

/* Whether any of a row's cells in those columns is stale. */
static bool LayerRowStale(unsigned bg, unsigned row, unsigned columns, uint64_t shownColumns)
{
    const uint32_t *word = sCellStale[bg] + row * (columns / 32);

    return (word[0] & (uint32_t)shownColumns) || (columns == 64 && (word[1] & (uint32_t)(shownColumns >> 32)));
}

static bool LayerStaleShown(unsigned bg, unsigned rows, unsigned columns, uint64_t shownRows, uint64_t shownColumns)
{
    for (unsigned row = 0; row < rows; ++row)
        if (((shownRows >> row) & 1) && LayerRowStale(bg, row, columns, shownColumns)) return true;
    return false;
}

static bool LayerRenderCells(unsigned bg)
{
    static uint16_t dirty[LAYER_CELLS];
    static int16_t cellSlot[LAYER_CELLS];
    static uint16_t cellEntry[LAYER_CELLS];
    static uint32_t memoStamp[1024], stamp;
    static uint16_t memoEntry[1024];
    static int16_t memoSlot[1024];
    LayerTexture *layer = &sLayers[bg];
    unsigned control = Reg(8 + bg * 2), size = control >> 14;
    unsigned map = ((control >> 8) & 31) * 0x800;
    unsigned chars = ((control >> 2) & 3) * 0x4000;
    bool color256 = (control & 128) != 0;
    unsigned columns = (size & 1) ? 64 : 32, rows = (size & 2) ? 64 : 32;
    unsigned cells = rows * columns;
    bool watch = sBattle, plain = (sBattleLayerWanted & (1u << bg)) && (size & 1);
    unsigned watching = plain ? CELLS_PLAIN : watch ? CELLS_WATCHED : 0;
    bool full = !layer->valid || sCellControl[bg] != control || sCellsWatched[bg] != watching;
    unsigned count = 0, tileBytes = color256 ? 64 : 32, stale = 0;
    unsigned charBytes = Min(1024u * tileBytes, 0x10000 - chars);
    uint32_t walked = layer->walked;
    uint64_t shownRows, shownColumns;
    uint16_t moved[16] = {0};
    unsigned movedAny = 0, seen = 0, left = 0, back = 0;
    bool sameBytes, sameMap, sameTiles, pass = false;

    if (plain && !full) charBytes = Min(sCellsPlainTop[bg] * tileBytes, charBytes);
    sameBytes = !full && layer->walkedGeneration == sCacheGeneration && !VramChangedAfter(chars, charBytes, walked);
    sameMap = !VramChangedAfter(map, rows * columns * 2, walked) && sTilemapBankStamp <= walked;
    sameTiles = sameBytes && LayerPalettesHold(bg, watch, walked);

    /* Nothing it is made of changed since its cells were last walked: the
     * texture is what a walk would draw. */
    if (sameTiles && sameMap && !sCellsStale[bg]) return false;
    LayerWindow(bg, plain, rows, columns, &shownRows, &shownColumns);
    if (sameTiles && sameMap)
    {
        /* The same, but for cells left stale: those that came into view. */
        if (!LayerStaleShown(bg, rows, columns, shownRows, shownColumns)) return false;
        pass = true;
    }
    else if (sameBytes && sameMap && watch && !color256)
        pass = LayerColoursMoved(bg, walked, moved, &movedAny);
    /* A map that moved walks them all rather than row by row with stale cells. */
    if (!pass && sCellsStale[bg]) sameTiles = false;
    if (full && sCellsBanked[bg])
    {
        memset(sCellBanked[bg], 0, sizeof(sCellBanked[bg]));
        sCellsBanked[bg] = 0;
    }
    sCellsWatched[bg] = (uint8_t)watching;
    /* Every row is read below, or those read add to what was read. */
    if (!sameTiles && !pass)
    {
        sCellsPlainTop[bg] = 0;
        memset(sCellRowColours[bg], 0, sizeof(sCellRowColours[bg]));
        if (sCellsStale[bg]) memset(sCellStale[bg], 0, sizeof(sCellStale[bg]));
        sCellsStale[bg] = 0;
    }
    /* Recorded at this token: its VRAM is compared now (see SyncBlock). */
    VramSyncRange(chars, charBytes);
    VramSyncRange(map, rows * columns * 2);
    layer->walked = sStats.frames + 1;
    layer->walkedGeneration = sCacheGeneration;
    ++stamp;
    for (unsigned row = 0; row < rows; ++row)
    {
        unsigned rowBase = map + CtrVideo_TextMapOffset(0, row, size);
        uint32_t *rowStale = sCellStale[bg] + row * (columns / 32);
        bool rowShown = (shownRows >> row) & 1;

        /* A pass: a row none of whose colours moved, with no stale cell. */
        if (pass)
        {
            if (!(sCellRowColours[bg][row] & movedAny) && !LayerRowStale(bg, row, columns, ~(uint64_t)0)) continue;
        }
        /*
         * Same tiles, same palettes, same slots: a cell can only have changed
         * if its tilemap entry did, or its bank. The camera redraws a row or
         * a column of the map at a time, so most rows lie in VRAM that did
         * not change and are not walked at all (both screenblocks of a
         * 64-wide row).
         */
        else if (sameTiles && !VramChangedAfter(rowBase, 64, walked) && !TilemapBanksChangedAfter(rowBase, walked)
                 && (columns < 64 || (!VramChangedAfter(rowBase + 2048, 64, walked)
                                      && !TilemapBanksChangedAfter(rowBase + 2048, walked))))
            continue;

        for (unsigned column = 0; column < columns; ++column)
        {
            unsigned cell = row * columns + column;
            unsigned entryAddress = rowBase + (column & 31) * 2 + (column >> 5) * 2048;
            uint32_t staleBit = 1u << (column & 31);
            bool wasStale = pass && (rowStale[column >> 5] & staleBit);
            unsigned entry, index, bank;
            int slot;
            uint32_t sig;

            if (pass && !wasStale && !(sCellColours[bg][cell] & movedAny)) continue;
            entry = Read16(entryAddress);
            index = entry & 1023;
            bank = color256 ? 0 : TilemapBankAt(entryAddress, entry);
            if (pass)
            {
                /* Its own bank's colours; then out of view it stays as it is. */
                if (!wasStale && !(sCellColours[bg][cell] & moved[entry >> 12])) continue;
                if (!rowShown || !((shownColumns >> column) & 1))
                {
                    if (!wasStale)
                    {
                        rowStale[column >> 5] |= staleBit;
                        ++sCellsStale[bg];
                        ++left;
                    }
                    continue;
                }
                if (wasStale)
                {
                    rowStale[column >> 5] &= ~staleBit;
                    --sCellsStale[bg];
                    ++back;
                }
            }
            ++seen;
            if (plain)
            {
                stale += bank != 0;
                bank = 0;
                if (index >= sCellsPlainTop[bg]) sCellsPlainTop[bg] = index + 1;
            }
            if (watch && (bank || sCellsBanked[bg]))
            {
                uint32_t *word = &sCellBanked[bg][cell >> 5], mask = 1u << (cell & 31);

                if ((*word & mask) != (bank ? mask : 0u))
                {
                    *word ^= mask;
                    sCellsBanked[bg] += bank ? 1u : ~0u;
                }
            }
            if (bank)
                slot = BankedTileSlot(bank, entry, chars);
            else if (memoStamp[index] == stamp && memoEntry[index] == entry)
                slot = memoSlot[index];
            else
            {
                unsigned address = chars + index * (color256 ? 64 : 32);

                slot = address < 0x10000 ? GetTileSlot(address, entry >> 12, color256) : -1;
                memoStamp[index] = stamp;
                memoEntry[index] = (uint16_t)entry;
                memoSlot[index] = (int16_t)slot;
            }
            cellSlot[cell] = (int16_t)slot;
            cellEntry[cell] = (uint16_t)entry;
            if (watch)
            {
                sCellColours[bg][cell] = slot < 0 ? 0 : (uint16_t)sTiles[slot].colors[0];
                sCellRowColours[bg][row] |= sCellColours[bg][cell];
            }
            /* Nothing drawn is 0; anything drawn has the top bit set. */
            sig = slot < 0 ? 0 : (((sTiles[slot].serial << 2) | ((entry >> 10) & 3)) | 1u << 31);
            if (!full && sCellSig[bg][cell] == sig) continue;
            if ((sCellSig[bg][cell] != 0) != (sig != 0))
                sCellsShown[bg] += sig != 0 ? 1u : ~0u;
            sCellSig[bg][cell] = sig;
            dirty[count++] = (uint16_t)cell;
        }
    }
    if (stale && full)
    {
        static unsigned told;

        if (told < 32)
        {
            ++told;
            CtrLog_Write(CTR_LOG_VIDEO, "VIDEO: battle layer BG%u: %u cells match a tile bank the field left, "
                         "read without it", bg, stale);
        }
    }
    if (watch)
    {
        if (pass)
        {
            ++sLayerPasses.passes;
            sLayerPasses.passSeen += seen;
            sLayerPasses.passChanged += count;
            sLayerPasses.left += left;
            sLayerPasses.back += back;
        }
        else
        {
            ++sLayerPasses.walks;
            sLayerPasses.walkSeen += seen;
            sLayerPasses.walkChanged += count;
        }
    }
    if (count == 0) return false;
    layer->valid = true;
    sCellControl[bg] = control;

    {
        /* Mostly changed: the whole layer, which needs no clearing either.
         * Not after a walk that skipped rows or a pass: their cells were not
         * read. */
        bool whole = full || (!sameTiles && !pass && count > cells / 2);

        if (whole)
        {
            count = 0;
            for (unsigned cell = 0; cell < cells; ++cell)
                dirty[count++] = (uint16_t)cell;
        }
        if (FastCells(bg, dirty, count, cellSlot, cellEntry, columns))
            return true;
        full = whole;
    }
    BlendForget();
    sInLayerTexture = true;
    if (full)
    {
        C2D_TargetClear(layer->target, 0);
        SceneOn(layer->target);
        C2D_ViewReset();
        count = 0;
        for (unsigned cell = 0; cell < cells; ++cell)
            dirty[count++] = (uint16_t)cell;
    }
    else
    {
        /* Clear the changed cells to transparent first: a tile's transparent
         * pixels must not keep what was drawn there before. */
        SceneOn(layer->target);
        C2D_ViewReset();
        C2D_Flush();
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        for (unsigned i = 0; i < count; ++i)
            C2D_DrawRectSolid((dirty[i] & (columns - 1)) * 8, (dirty[i] >> __builtin_ctz(columns)) * 8, 0, 8, 8, 0);
        C2D_Flush();
        BlendForget();
    }
    Blend(bg, false, false);
    for (unsigned i = 0; i < count; ++i)
    {
        unsigned cell = dirty[i];

        if (cellSlot[cell] >= 0)
            DrawSlot(cellSlot[cell], (cell & (columns - 1)) * 8, (cell >> __builtin_ctz(columns)) * 8,
                     cellEntry[cell] & 1024, cellEntry[cell] & 2048);
    }
    C2D_Flush();
    /* DrawSlot drops quads past the geometry budget, and their cells already
     * hold their new signatures: draw the whole layer again next frame, or
     * they stay holes until their tiles change. */
    if (sStats.tiles >= DRAW_BUDGET)
        layer->valid = false;
    sInLayerTexture = false;
    BlendForget();
    return true;
}

/* A field background from its texture: one quad, wrapping as its tilemap
 * does, over the band the tilemap spans (DrawTextBg's limits). */
static bool DrawFieldBgTex(unsigned bg)
{
    LayerTexture *layer = &sLayers[bg];
    float width, height, sx, sy;
    int x0 = sClipX0 > 0 ? sClipX0 : 0, y0 = sClipY0 > 0 ? sClipY0 : 0, x1, y1;

    if (!sFieldLayers || !LayerDrawable(bg)) return false;
    if (sCellsShown[bg] == 0) return true;
    width = layer->tex.width;
    height = layer->tex.height;
    x1 = sClipX1 < (int)width ? sClipX1 : (int)width;
    /* The texture repeats as the tilemap wraps. */
    if (bg == 0 && sFieldBanner) x1 = sClipX1;
    y1 = sClipY1 < (int)height ? sClipY1 : (int)height;
    if (x0 >= x1 || y0 >= y1) return true;
    sx = Reg(0x10 + bg * 4) & 511;
    sy = Reg(0x12 + bg * 4) & 511;
    ViewBase();
    {
        const Tex3DS_SubTexture run = {(u16)(x1 - x0), (u16)(y1 - y0),
            (sx + x0) / width, 1.0f - (sy + y0) / height,
            (sx + x1) / width, 1.0f - (sy + y1) / height};

        ++sStats.tiles;
        C2D_DrawImageAt((C2D_Image){&layer->tex, &run}, x0 + CTR_VIEW_X + sLayerShift,
                        y0 + CTR_VIEW_Y, 0, &sTint, 1, 1);
    }
    return true;
}

/* Inside the frame, before the logical surface: the maps that changed. */
/* Whether it queued drawing for the GPU. */
static bool LayersRender(void)
{
    bool any = false;

    for (unsigned bg = 0; bg < 4; ++bg)
    {
        if (!sLayerReady[bg]) continue;
        if (LayerRenderCells(bg)) any = true;
        sLayerFresh |= 1u << bg;
    }
    /* Rendering to a texture and then sampling it needs a command split. */
    if (any) C3D_FrameSplit(0);
    return any;
}

static bool LayerDrawable(unsigned bg)
{
    return sLayerReady[bg] && sLayers[bg].valid;
}

static bool DrawBandBgTex(unsigned bg);

/*
 * One rectangle [x0, x1) x [y0, y1) of the screen, in GBA coordinates, cut
 * from a layer texture: the screen point (x0, y0) shows the picture point
 * (sx, sy), and each screen pixel steps (dx, dy) through the picture - 1 to
 * copy, -1 to mirror, a fraction to stretch. Clipped to the current clip
 * rectangle by moving its texture coordinates, so it needs no scissor. With
 * fade, its corners are tinted by EdgeFade.
 */
static void DrawLayerRect(unsigned bg, int x0, int x1, int y0, int y1,
                          float sx, float sy, float dx, float dy, bool fade, int top, int bottom)
{
    LayerTexture *layer = &sLayers[bg];
    int cx0 = x0 > sClipX0 ? x0 : sClipX0, cx1 = x1 < sClipX1 ? x1 : sClipX1;
    int cy0 = y0 > sClipY0 ? y0 : sClipY0, cy1 = y1 < sClipY1 ? y1 : sClipY1;
    float width = layer->tex.width, height = layer->tex.height;
    float ox = Reg(0x10 + bg * 4) & 511, oy = Reg(0x12 + bg * 4) & 511;
    float s0, s1, t0, t1;
    C2D_ImageTint tint = sTint;

    if (cx0 >= cx1 || cy0 >= cy1) return;
    s0 = sx + (cx0 - x0) * dx + ox;
    s1 = sx + (cx1 - x0) * dx + ox;
    t0 = sy + (cy0 - y0) * dy + oy;
    t1 = sy + (cy1 - y0) * dy + oy;
    if (fade)
    {
        bool white;
        float bright = LayerBrightness(bg, &white);

        FadeCorner(&tint, C2D_TopLeft, EdgeFade(cx0, cy0, top, bottom), bright, white);
        FadeCorner(&tint, C2D_TopRight, EdgeFade(cx1, cy0, top, bottom), bright, white);
        FadeCorner(&tint, C2D_BotLeft, EdgeFade(cx0, cy1, top, bottom), bright, white);
        FadeCorner(&tint, C2D_BotRight, EdgeFade(cx1, cy1, top, bottom), bright, white);
    }
    {
        /*
         * A subtexture whose top is below its bottom means "rotated" to
         * Tex3DS, so a mirror is drawn as the plain cut flipped by a negative
         * scale, the same way flipped tiles are. (Only unfaded cuts mirror.)
         */
        bool flipY = t1 < t0;
        float ta = flipY ? t1 : t0, tb = flipY ? t0 : t1;
        const Tex3DS_SubTexture cut = {(u16)(cx1 - cx0), (u16)(cy1 - cy0),
            s0 / width, 1.0f - ta / height, s1 / width, 1.0f - tb / height};

        ++sStats.tiles;
        C2D_DrawImageAt((C2D_Image){&layer->tex, &cut}, cx0 + CTR_VIEW_X + sLayerShift,
                        cy0 + CTR_VIEW_Y, 0, &tint, 1, flipY ? -1 : 1);
    }
}

/* One corner cell: the corner block stretched to it, faded, skipped if black. */
static void DrawCornerCell(unsigned bg, int x0, int x1, int y0, int y1, float sx, float sy,
                           int top, int bottom)
{
    if (y0 < VIEW_TOP) y0 = VIEW_TOP;
    if (y1 > VIEW_BOTTOM) y1 = VIEW_BOTTOM;
    if (y0 < top && y1 > top) y1 = top;
    if (y0 < bottom && y1 > bottom) y0 = bottom;
    if (x0 >= x1 || y0 >= y1) return;
    if (sUnderlaid && EdgeFade(x0, y0, top, bottom) >= 1.0f && EdgeFade(x1, y0, top, bottom) >= 1.0f
        && EdgeFade(x0, y1, top, bottom) >= 1.0f && EdgeFade(x1, y1, top, bottom) >= 1.0f)
        return;
    DrawLayerRect(bg, x0, x1, y0, y1, sx, sy, 8.0f / (x1 - x0), 8.0f / (y1 - y0), true, top, bottom);
}

/*
 * One 8-line band above or below a still picture, from its texture: the
 * picture's row src repeated, or, when that row is plain (EdgeRowPlain), a
 * column of its plain tile wherever the row holds something else.
 */
static void DrawEdgeBand(unsigned bg, int y0, int y1, int src, int top, int bottom)
{
    unsigned plain = EdgeRowPlain(bg, src);
    unsigned control = Reg(8 + bg * 2), size = control >> 14;
    unsigned map = ((control >> 8) & 31) * 0x800;
    unsigned scrollX = Reg(0x10 + bg * 4) & 511, scrollY = Reg(0x12 + bg * 4) & 511;
    unsigned rowBase = map + CtrVideo_TextMapOffset(0, (unsigned)(src + (int)scrollY) >> 3, size);
    int plainColumn = -1, run = 0;

    bool isPlain[30];

    if (plain == ~0u)
    {
        DrawLayerRect(bg, 0, 240, y0, y1, 0, src, 1, 1, true, top, bottom);
        return;
    }
    for (int c = 0; c < 30; ++c)
    {
        unsigned column = ((scrollX >> 3) + (unsigned)c) & ((size & 1) ? 63u : 31u);

        isPlain[c] = Read16(rowBase + (column & 31) * 2 + (column >> 5) * 2048) == plain;
        if (isPlain[c] && plainColumn < 0) plainColumn = c;
    }
    for (int c = 0; c <= 30; ++c)
    {
        if (c < 30 && isPlain[c]) continue;
        /* The plain columns up to here as one cut, then this one as plain. */
        if (c > run) DrawLayerRect(bg, run * 8, c * 8, y0, y1, run * 8, src, 1, 1, true, top, bottom);
        if (c < 30)
            DrawLayerRect(bg, c * 8, c * 8 + 8, y0, y1, plainColumn * 8, src, 1, 1, true, top, bottom);
        run = c + 1;
    }
}
