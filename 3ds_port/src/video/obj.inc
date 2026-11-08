/*
 * Part of 3ds_video.c. Sprites: where they go (PlaceScreenObject), the fog's
 * lattice, their batches (SpriteBatchFlush) and DrawObjects.
 */

/*
 * Where a sprite placed on the screen, not on the map, goes in the field view
 * (400x240, the 240x160 picture's own layout on it). Those of a window - the
 * item shown by the PC, the mon shown for a move - sit where their window
 * layer does: BG0 is drawn moved by CTR_FIELD_UI_SHIFT (the field banner's
 * streaks excepted), and they follow it. The healing machine's balls and
 * monitors were placed by hand on the picture around the player (the player's
 * tile centre at GBA (120, 80)), so they go where the map's machine is: in
 * the 2D field the picture's offset on the view, in the voxel one the
 * machine's tile seen by the camera. Fly's bird and its rider fly round the
 * player on that picture: they are moved with it, whole, in the voxel field
 * to where the camera sees the player (the GBA's (120, 80), as the circle of
 * a dark cave) - not projected, which would bend the bird's arc.
 */
static void PlaceScreenObject(int i, unsigned boxW, unsigned boxH, bool voxel, int *x, int *y)
{
    if (sFlyOam[i >> 5] & (1u << (i & 31)))
    {
        float px, py, scaleX, scaleY;

        if (voxel && CtrVoxel_PlayerLightSpot(&px, &py, &scaleX, &scaleY))
        {
            *x += (int)lroundf(px) - 120;
            *y += (int)lroundf(py) - 80;
            return;
        }
        *x += CTR_STAGE_X;
        *y += CTR_STAGE_Y - 8;
        return;
    }
    if (sMachineOam[i >> 5] & (1u << (i & 31)))
    {
        if (voxel)
        {
            float sx, sy;

            if (CtrVoxel_ProjectPictureTile((*x + boxW / 2.0f - 120.0f) / 16.0f,
                                            (*y + boxH / 2.0f - 80.0f) / 16.0f, &sx, &sy))
            {
                *x = (int)(sx - boxW / 2.0f);
                *y = (int)(sy - boxH / 2.0f);
            }
            return;
        }
        if (!voxel)
        {
            /* The 2D field shows the picture 8 lines above the stage's
             * centre: the player's tile, GBA (112, 72), is at (192, 104). */
            *x += CTR_STAGE_X;
            *y += CTR_STAGE_Y - 8;
        }
        return;
    }
    /* With the text band, and in the voxel overlay at its depth too; the
     * machine's sprites above are at the world's own. */
    if (!sFieldBanner) *x += (int)CTR_FIELD_UI_SHIFT + (voxel ? sOverlayEyeShift : 0);
}

/*
 * The fog's picture repeated over the whole clip area, on the lattice the
 * sprite at (x, y) sits on. The game lays twenty sprites for a 240x160
 * screen and lets OAM wrap them round its edges; on this view that left the
 * right of the screen bare, and the 8-bit y of a row past the bottom read as
 * a row further down instead of the one at the top. The lattice does not
 * care which sprite it starts from, so any wrap of x or y is harmless here.
 * The 64 tiles are looked up once and drawn at every place, batched as the
 * sprites are (TileWalkSlot): the sandstorm's and the ash's lattice is some
 * two thousand tiles over the wide view, and one Citro2D quad each was most
 * of an Old 3DS frame.
 */
static void DrawFogLattice(unsigned attr2, unsigned bank, unsigned width, unsigned height, bool color256,
                           int x, int y, bool flipX, bool flipY)
{
    int slots[8 * 8];
    unsigned columns = width / 8, rows = height / 8;
    int x0 = x % (int)width, y0 = y % (int)height;
    bool batch;

    if (columns * rows > sizeof(slots) / sizeof(slots[0])) return;
    for (unsigned ty = 0; ty < rows; ++ty)
        for (unsigned tx = 0; tx < columns; ++tx)
        {
            unsigned sx = flipX ? columns - 1 - tx : tx;
            unsigned sy = flipY ? rows - 1 - ty : ty;
            unsigned tile = CtrVideo_ObjTile(attr2 & 1023, sx, sy, width, color256, Reg(0) & 0x40);
            slots[ty * columns + tx] = GetTileSlot(0x10000 + tile * 32, bank, color256);
        }
    if (x0 > sClipX0) x0 -= (int)width * ((x0 - sClipX0 + (int)width - 1) / (int)width);
    if (y0 > sClipY0) y0 -= (int)height * ((y0 - sClipY0 + (int)height - 1) / (int)height);
    batch = TileWalkBegin();
    for (int py = y0; py < sClipY1; py += (int)height)
        for (int px = x0; px < sClipX1; px += (int)width)
        {
            if (px + (int)width <= sClipX0 || py + (int)height <= sClipY0) continue;
            for (unsigned ty = 0; ty < rows; ++ty)
            {
                int top = py + (int)ty * 8;

                /* Only the tiles in the clip: a band of a window partition
                 * is a strip of the view. */
                if (top >= sClipY1 || top + 8 <= sClipY0) continue;
                for (unsigned tx = 0; tx < columns; ++tx)
                {
                    int slot = slots[ty * columns + tx], left = px + (int)tx * 8;

                    if (slot >= 0 && left < sClipX1 && left + 8 > sClipX0)
                        TileWalkSlot(batch, slot, left + CTR_VIEW_X + sLayerShift, top + CTR_VIEW_Y, flipX, flipY);
                }
            }
        }
    TileWalkEnd(batch);
}

/*
 * Sprite tiles without Citro2D's per-quad work, as layer cells are drawn
 * (FastCells): a sprite was up to 64 C2D_DrawImageAt calls, ~4.6 us of CPU
 * each on an Old 3DS. The tiles of consecutive sprites that share a blend
 * state go into the cells' vertex buffer and out in one indexed draw, on
 * Citro2D's own program and texture combiner - set up for the atlas and the
 * current tint by an empty Citro2D draw - with the tint, which is the same at
 * every corner, as fixed attributes: a Citro2D vertex carries its tint's blend
 * in its second point coordinate and its colour as four bytes. Only plain
 * sprites at 1:1 and whole pixels take this path; the rest are drawn as before.
 */
static unsigned sSpriteFirst;
static bool sSpritePending;

static void SpriteBatchFlush(void)
{
    C3D_AttrInfo savedAttr, *attr;
    C3D_BufInfo savedBuf, *buf;
    Tex3DS_SubTexture none = {0, 0, 0.0f, 1.0f, 0.0f, 1.0f};
    uint32_t color = sTint.corners[0].color;

    if (!sSpritePending)
        return;
    sSpritePending = false;
    if (sSpriteCount == 0)
        return;
    /* The batch's vertices are in the base view. An affine sprite drawn just
     * before it opened (the bag) leaves its own matrix set, which carried the
     * whole batch - the bag's item icon - off by that sprite's place. */
    ViewBase();
    C2D_DrawImageAt((C2D_Image){&sAtlas, &none}, 0, 0, 0, &sTint, 1, 1);
    C2D_Flush();
    attr = C3D_GetAttrInfo();
    savedAttr = *attr;
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_SHORT, 2);
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);
    AttrInfo_AddFixed(attr, 2);
    AttrInfo_AddFixed(attr, 3);
    C3D_FixedAttribSet(2, 0.0f, sTint.corners[0].blend, 0.0f, 0.0f);
    C3D_FixedAttribSet(3, (float)(color & 255), (float)((color >> 8) & 255),
                       (float)((color >> 16) & 255), (float)(color >> 24));
    buf = C3D_GetBufInfo();
    savedBuf = *buf;
    BufInfo_Init(buf);
    BufInfo_Add(buf, sFastVertices, sizeof(FastVertex), 2, 0x10);
    FastDraw(sSpriteFirst, sSpriteCount);
    C3D_SetAttrInfo(&savedAttr);
    C3D_SetBufInfo(&savedBuf);
    sSpriteCount = 0;
}

/* Room for a sprite of `tiles` tiles in the batch, opening one if needed. */
static bool SpriteBatchRoom(unsigned tiles)
{
    if (!sFastReady || sFastUsed + tiles > FAST_QUADS)
        return false;
    if (!sSpritePending)
    {
        sSpritePending = true;
        sSpriteFirst = sFastUsed;
        sSpriteCount = 0;
    }
    return true;
}

/*
 * One sprite, already placed: its tiles batched when they can be, drawn one by
 * one otherwise (an affine sprite, a fractional position, a full batch).
 */
static void DrawObjectAt(int x, int y, unsigned attr1, unsigned attr2, unsigned bank, unsigned width,
                         unsigned height, unsigned boxW, unsigned boxH, bool affine, bool color256, bool effects,
                         unsigned mode)
{
    if (x >= sClipX1 || x + (int)boxW <= sClipX0
     || y >= sClipY1 || y + (int)boxH <= sClipY0) return;
    ++sStats.sprites;
    {
        /* A new blend state ends the batch: Blend changes the GPU state. */
        static unsigned batchKey = ~0u;
        unsigned key = (effects ? 1u : 0u) | (mode == 1 ? 2u : 0u) | (sDayNightObject ? 4u : 0u);
        float shiftX = x + CTR_VIEW_X + sLayerShift;
        int whole = (int)shiftX;

        if (sSpritePending && key != batchKey) SpriteBatchFlush();
        /* At any zoom: the view is the GPU's, for these quads as for
         * Citro2D's; the battle scene composes its sprites at 2x. */
        if (!affine && (float)whole == shiftX
            && SpriteBatchRoom((width / 8) * (height / 8)))
        {
            bool flipX = (attr1 & 0x1000) != 0, flipY = (attr1 & 0x2000) != 0;

            /* A batch just opened: its blend state and tint. */
            if (sSpriteCount == 0) Blend(4, effects, mode == 1);
            batchKey = key;
            sSpritePending = true;
            for (unsigned ty = 0; ty < height / 8; ++ty)
                for (unsigned tx = 0; tx < width / 8; ++tx)
                {
                    unsigned sx = flipX ? width / 8 - 1 - tx : tx;
                    unsigned sy = flipY ? height / 8 - 1 - ty : ty;
                    unsigned tile = CtrVideo_ObjTile(attr2 & 1023, sx, sy, width, color256, Reg(0) & 0x40);
                    int slot = GetTileSlot(0x10000 + tile * 32, bank, color256);

                    if (slot < 0) continue;
                    ++sStats.tiles;
                    FastQuad(sFastVertices + sFastUsed * 4, whole + (int)tx * 8, y + CTR_VIEW_Y + (int)ty * 8,
                             slot, flipX, flipY);
                    ++sFastUsed;
                    ++sSpriteCount;
                }
            return;
        }
        SpriteBatchFlush();
        batchKey = ~0u;
    }
    Blend(4, effects, mode == 1);
    ViewBase();
    if (affine)
    {
        unsigned index = ((attr1 >> 9) & 31) * 16;
        float a = (int16_t)sMemory.oam[index + 3] / 256.0f;
        float b = (int16_t)sMemory.oam[index + 7] / 256.0f;
        float c = (int16_t)sMemory.oam[index + 11] / 256.0f;
        float d = (int16_t)sMemory.oam[index + 15] / 256.0f;
        float det = a * d - b * c;
        if (fabsf(det) < 0.00001f) { Error(9, "singular OBJ affine matrix"); return; }
        C3D_Mtx matrix;
        Mtx_Identity(&matrix);
        matrix.r[0] = FVec4_New(d / det, -b / det, 0, x + CTR_VIEW_X + sLayerShift + boxW / 2.0f - (d * width - b * height) / (2 * det));
        matrix.r[1] = FVec4_New(-c / det, a / det, 0, y + CTR_VIEW_Y + boxH / 2.0f - (a * height - c * width) / (2 * det));
        ViewAffine(&matrix);
    }
    bool flipX = !affine && (attr1 & 0x1000), flipY = !affine && (attr1 & 0x2000);
    sTileInset = affine ? 1.0f / (16.0f * ATLAS_SIZE) : 0.0f;
    for (unsigned ty = 0; ty < height / 8; ++ty)
        for (unsigned tx = 0; tx < width / 8; ++tx)
        {
            unsigned sx = flipX ? width / 8 - 1 - tx : tx;
            unsigned sy = flipY ? height / 8 - 1 - ty : ty;
            unsigned tile = CtrVideo_ObjTile(attr2 & 1023, sx, sy, width, color256, Reg(0) & 0x40);
            DrawTile(0x10000 + tile * 32, bank, color256,
                     (affine ? 0 : x + CTR_VIEW_X + sLayerShift) + (int)tx * 8,
                     (affine ? 0 : y + CTR_VIEW_Y) + (int)ty * 8, flipX, flipY);
        }
    sTileInset = 0.0f;
}

/*
 * This frame's OAM draw list (3ds_obj_list.h), latched once a presented frame
 * before anything reads OAM; NULL, the 128 entries are walked as before.
 */
static const CtrObjList *sObjList;

/* One OAM entry of DrawObjects' walk. */
static void DrawObjectEntry(int i, unsigned priority, bool effects, bool *fogDrawn, bool *latticeDrawn)
{
    bool fog = (sFogOam[i >> 5] & (1u << (i & 31))) != 0;
    bool lattice = (sLatticeOam[i >> 5] & (1u << (i & 31))) != 0;
    bool particle = (sParticleOam[i >> 5] & (1u << (i & 31))) != 0;

    /* The fog is one picture: drawn once, over the whole view. So are
     * the ash and the sandstorm's sand. */
    if ((fog && *fogDrawn) || (lattice && *latticeDrawn)) return;
    /* The voxel world draws a fog of its own, in the scene. */
    if (fog && sVoxelObjPass != VOXEL_OBJ_NONE && CtrVoxel_DrawsFog()) return;
    /* ... and its own rain, sand and ash (DrawWeather): the sand's
     * swirls too, which are particles. */
    if ((lattice || particle) && sVoxelObjPass != VOXEL_OBJ_NONE && CtrVoxel_DrawsWeather()) return;
    /* The clouds lie on the voxel world's water (voxel_entities.c). */
    if (sVoxelObjPass != VOXEL_OBJ_NONE && (sVoxelCloudOam[i >> 5] & (1u << (i & 31))))
        return;
    if (sVoxelObjPass != VOXEL_OBJ_NONE)
    {
        bool weather = (sVoxelWeatherOam[i >> 5] & (1u << (i & 31))) != 0;
        bool screen = (sScreenOam[i >> 5] & (1u << (i & 31))) != 0;

        if (sVoxelObjPass == VOXEL_OBJ_WEATHER ? !weather
            : sVoxelObjPass == VOXEL_OBJ_FIELD ? (weather || screen) : (weather || !screen))
            return;
    }
    if (sObjFilter != OBJ_ALL && TransitionOam((unsigned)i) != (sObjFilter == OBJ_TRANSITION))
        return;
    if (sObjMaskOn && !(sObjMask[i >> 5] & (1u << (i & 31)))) return;
    if (i == WIRELESS_INDICATOR_OAM && !sObjIndicatorPass && WirelessIndicatorApart()) return;
    const uint16_t *obj = sMemory.oam + i * 4;
    unsigned attr0 = obj[0], attr1 = obj[1], attr2 = obj[2];
    bool affine = (attr0 & 0x100) != 0, color256 = (attr0 & 0x2000) != 0;
    if ((!affine && (attr0 & 0x200)) || ((attr2 >> 10) & 3) != priority) return;
    unsigned mode = (attr0 >> 10) & 3, shape = attr0 >> 14;
    /* A window's sprites are its shape, drawn only into its mask. */
    if ((mode == 2) != (sObjWinPass == OBJWIN_MASK)) return;
    if (mode == 3 || shape == 3) return;
    /* Only a mosaic that shows: the game sets the bit with a size of 0. */
    if ((attr0 & 0x1000) && (Reg(0x4c) & 0xff00))
    {
        Error(8, "OBJ mosaic not supported");
        sQualityMosaic = true;
    }
    unsigned width = gCtrObjDims[shape][attr1 >> 14][0], height = gCtrObjDims[shape][attr1 >> 14][1];
    unsigned boxW = affine && (attr0 & 0x200) ? width * 2 : width;
    unsigned boxH = affine && (attr0 & 0x200) ? height * 2 : height;
    int x = attr1 & 511, y = attr0 & 255;
    if (sStage || sCentred || sBattle || sTransitionCompose)
    {
        /*
         * The GBA's own reading: a sprite only comes back from the other
         * side when it crosses the end of the coordinate range. Unused
         * entries sit at y=240 x=464, which the wide view would otherwise
         * turn into a column of blank tiles at (-48, -16), in its margin.
         */
        if (x + (int)boxW > 512) x -= 512;
        if (y + (int)boxH > 256) y -= 256;
    }
    else
    {
        if (x >= VIEW_RIGHT) x -= 512;
        if (sOamAnchored[i >> 5] & (1u << (i & 31))) y = OamUnwrapY((unsigned)i, y);
        else if (y >= VIEW_BOTTOM) y -= 256;
        if (sVoxelObjPass == VOXEL_OBJ_WEATHER)
        {
            x += CTR_STAGE_X;
            y += CTR_STAGE_Y;
        }
        else if (sVoxelObjPass == VOXEL_OBJ_SCREEN)
            PlaceScreenObject(i, boxW, boxH, true, &x, &y);
        if (sFieldUi && (sScreenOam[i >> 5] & (1u << (i & 31))))
            PlaceScreenObject(i, boxW, boxH, false, &x, &y);
    }
    /* A battle in 3D: each sprite at its own depth (BattleSpriteShift). */
    if (sSpriteDepth) sLayerShift = BattleSpriteShift(y + (int)boxH / 2) / sShiftZoom;
    /* An info box drawn from its picture: its sprites are not drawn. */
    if (sHud && sBattle && sObjWinPass == OBJWIN_NONE)
    {
        int box = sHudOamBox[i];

        if (box >= 0)
        {
            unsigned tile = attr2 & 1023;

            /* The halves, not the bar: it lies over them. */
            /* Only one in sight: the GBA's unused entries are tile 0 too. */
            if (!sHudCrisp && tile >= sHud[box].boxTile && tile < (unsigned)sHud[box].boxTile + sHud[box].boxTiles
                && x < sClipX1 && x + (int)boxW > sClipX0 && y < sClipY1 && y + (int)boxH > sClipY0)
                HudDrawInScene((unsigned)box, tile, x, y, (int)boxW, (int)boxH, effects);
            return;
        }
    }
    /* Below the PokeNav's picture is its background, not the space the
     * GBA parks its unused sprites in. */
    if (sCentredScreen == CTR_CENTRED_POKENAV && y >= 160) return;
    bool nightObject = !sStage && !sCentred && !sBattle
                    && CtrDayNight_Get()->daylight != 1.0f
                    && (sScreenOam[i >> 5] & (1u << (i & 31))) == 0;

    if (sDayNightObject != nightObject)
        SpriteBatchFlush();
    sDayNightObject = nightObject;
    /*
     * Any one of the fog's sprites gives the lattice its phase, on the
     * screen or not: as a map is entered the game can have all twenty
     * of them outside it until the camera first moves.
     */
    if ((fog || lattice) && !affine)
    {
        ++sStats.sprites;
        SpriteBatchFlush();
        Blend(4, effects, mode == 1);
        ViewBase();
        DrawFogLattice(attr2, ObjBank((unsigned)i, attr2), width, height, color256, x, y,
                       (attr1 & 0x1000) != 0, (attr1 & 0x2000) != 0);
        if (fog) *fogDrawn = true;
        else *latticeDrawn = true;
        return;
    }
    /*
     * Rain, snow, the sand swirls and the bubbles fall over the GBA's
     * 240x160 screen and wrap round its edges: copies of the screen side
     * by side carry them over the whole view, without a seam.
     */
    if (particle && !affine)
    {
        int left = x, top = y;

        while (left + (int)boxW > sClipX0) left -= 240;
        while (top + (int)boxH > sClipY0) top -= 160;
        for (int cy = top; cy < sClipY1; cy += 160)
            for (int cx = left; cx < sClipX1; cx += 240)
                DrawObjectAt(cx, cy, attr1, attr2, ObjBank((unsigned)i, attr2), width, height, boxW, boxH,
                             false, color256, effects, mode);
        return;
    }
    DrawObjectAt(x, y, attr1, attr2, ObjBank((unsigned)i, attr2), width, height, boxW, boxH, affine, color256,
                 effects, mode);
}

/*
 * The sprites of one priority, back to front: OAM 127 down to 0, the GBA's
 * order, which is also what draws the fog and the lattice once each. With the
 * list, only the entries that priority draws, and with a mask (a slot's, a
 * loose sprite's) only the mask's among them; on the screens that keep the
 * GBA's reading of a box (a stage, a centred screen, a battle) an entry whose
 * box is outside the clip is left before its attributes are read - an entry
 * that would draw nothing there and change nothing, so not the fog's, the
 * weather's or a mosaic's (counted when it shows). The rest is the same walk.
 */
static void DrawObjects(unsigned priority, bool effects)
{
    bool fogDrawn = false, latticeDrawn = false;
    float layerShift = sLayerShift;
    const CtrObjList *list = sObjList;

    if (!list || priority > 3)
    {
        for (int i = 127; i >= 0; --i)
            DrawObjectEntry(i, priority, effects, &fogDrawn, &latticeDrawn);
    }
    else
    {
        bool reject = sStage || sCentred || sBattle;

        if (sObjMaskOn)
        {
            for (int w = 3; w >= 0; --w)
                for (uint32_t bits = list->members[priority][w] & sObjMask[w]; bits; )
                {
                    unsigned b = 31u - (unsigned)__builtin_clz(bits);
                    int i = w * 32 + (int)b;
                    const CtrObjEntry *e = &list->entry[i];

                    bits &= ~(1u << b);
                    if (reject && !(e->marks & (CTR_OBJ_MARK_FOG | CTR_OBJ_MARK_LATTICE | CTR_OBJ_MARK_PARTICLE))
                        && !(e->flags & CTR_OBJ_MOSAIC)
                        && (e->x >= sClipX1 || e->x + e->w <= sClipX0 || e->y >= sClipY1 || e->y + e->h <= sClipY0))
                        continue;
                    DrawObjectEntry(i, priority, effects, &fogDrawn, &latticeDrawn);
                }
        }
        else
        {
            for (unsigned k = 0; k < list->count[priority]; ++k)
            {
                int i = list->order[priority][k];
                const CtrObjEntry *e = &list->entry[i];

                if (reject && !(e->marks & (CTR_OBJ_MARK_FOG | CTR_OBJ_MARK_LATTICE | CTR_OBJ_MARK_PARTICLE))
                    && !(e->flags & CTR_OBJ_MOSAIC)
                    && (e->x >= sClipX1 || e->x + e->w <= sClipX0 || e->y >= sClipY1 || e->y + e->h <= sClipY0))
                    continue;
                DrawObjectEntry(i, priority, effects, &fogDrawn, &latticeDrawn);
            }
        }
    }
    SpriteBatchFlush();
    sLayerShift = layerShift;
    ViewBase();
}
