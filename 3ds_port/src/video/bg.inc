/*
 * Part of 3ds_video.c. Backgrounds from textures: stage and line backgrounds,
 * the battle's layers (MapPlan, DrawBattleBgTex, DrawBattleTerrainTex) and
 * DrawAffineBg.
 */

/*
 * A stage background from its texture, filled around the picture exactly as
 * DrawStageBg does it tile by tile: a layer that scrolls wraps across and is
 * mirrored above and below; a still one is carried out from its edges - the
 * 8-pixel band along each edge repeated outwards, the corners stretched from
 * the corner block - and faded to black.
 */
/*
 * A faded stage's margins (the cable car): the picture mirrored about its
 * edges and faded into black, as the intro's carried edges are. Carried, the
 * edge columns of its dirt, trees and pylons smeared into streaks; mirrored,
 * the margins hold the scene's own texture. Drawn in strips so the fade stays
 * a gradient: a mirrored cut is drawn flipped, which flips its tint too, and
 * across a strip that thin the difference does not show. Above the picture
 * only a still layer (the sky) is mirrored: the scrolling ones hold nothing
 * there, and a pylon's head would hang upside down.
 */
static bool MarginBlack(int x, int y)
{
    return sUnderlaid && EdgeFade(x, y, 0, 160) >= 1.0f;
}

static void DrawFadedStageTex(unsigned bg)
{
    DrawLayerRect(bg, 0, 240, 0, 160, 0, 0, 1, 1, false, 0, 160);
    for (int x = 0; x > VIEW_LEFT && !MarginBlack(x, 80); x -= 8)
        DrawLayerRect(bg, x - 8, x, 0, 160, (float)(8 - x), 0, -1, 1, true, 0, 160);
    for (int x = 240; x < VIEW_RIGHT && !MarginBlack(x, 80); x += 8)
        DrawLayerRect(bg, x, x + 8, 0, 160, (float)(480 - x), 0, -1, 1, true, 0, 160);
    for (int y = 160; y < VIEW_BOTTOM && !MarginBlack(120, y); y += 4)
    {
        float sy = (float)(320 - y);

        DrawLayerRect(bg, 0, 240, y, y + 4, 0, sy, 1, -1, true, 0, 160);
        for (int x = 0; x > VIEW_LEFT && !MarginBlack(x, y); x -= 16)
            DrawLayerRect(bg, x - 16, x, y, y + 4, (float)(16 - x), sy, -1, -1, true, 0, 160);
        for (int x = 240; x < VIEW_RIGHT && !MarginBlack(x, y); x += 16)
            DrawLayerRect(bg, x, x + 16, y, y + 4, (float)(480 - x), sy, -1, -1, true, 0, 160);
    }
    if (sScrolls[bg]) return;
    for (int y = 0; y > VIEW_TOP && !MarginBlack(120, y); y -= 4)
    {
        float sy = (float)(4 - y);

        DrawLayerRect(bg, 0, 240, y - 4, y, 0, sy, 1, -1, true, 0, 160);
        for (int x = 0; x > VIEW_LEFT && !MarginBlack(x, y); x -= 16)
            DrawLayerRect(bg, x - 16, x, y - 4, y, (float)(16 - x), sy, -1, -1, true, 0, 160);
        for (int x = 240; x < VIEW_RIGHT && !MarginBlack(x, y); x += 16)
            DrawLayerRect(bg, x, x + 16, y - 4, y, (float)(480 - x), sy, -1, -1, true, 0, 160);
    }
}

static bool DrawStageBgTex(unsigned bg)
{
    int top, bottom;
    bool wraps;

    if (!sStage || !LayerDrawable(bg)) return false;
    if (sStageFaded)
    {
        ViewBase();
        DrawFadedStageTex(bg);
        return true;
    }
    PictureRows(bg, &top, &bottom);
    wraps = sScrolls[bg];
    if (!wraps)
    {
        MeasureStill(bg);
        /* A still panorama wraps too when the scene around it scrolls. */
        if (sPanorama[bg])
            for (unsigned other = 0; other < 4; ++other)
                if (other != bg && sScrolls[other] && (Reg(0) & (0x100u << other))) wraps = true;
    }
    ViewBase();
    if (wraps)
    {
        DrawLayerRect(bg, VIEW_LEFT, VIEW_RIGHT, top, bottom, VIEW_LEFT, top, 1, 1, false, top, bottom);
        DrawLayerRect(bg, VIEW_LEFT, VIEW_RIGHT, VIEW_TOP, top, VIEW_LEFT, 2 * top - VIEW_TOP, 1, -1,
                      false, top, bottom);
        DrawLayerRect(bg, VIEW_LEFT, VIEW_RIGHT, bottom, VIEW_BOTTOM, VIEW_LEFT, bottom, 1, -1,
                      false, top, bottom);
        return true;
    }
    DrawLayerRect(bg, 0, 240, top, bottom, 0, top, 1, 1, false, top, bottom);
    if (DrawLeavesMargins(bg)) return true;
    for (int x = 0; x > VIEW_LEFT; x -= 8)
        DrawLayerRect(bg, x - 8, x, top, bottom, 0, top, 1, 1, true, top, bottom);
    for (int x = 240; x < VIEW_RIGHT; x += 8)
        DrawLayerRect(bg, x, x + 8, top, bottom, 232, top, 1, 1, true, top, bottom);
    for (int y = top; y > VIEW_TOP; y -= 8)
        DrawEdgeBand(bg, y - 8, y, top, top, bottom);
    for (int y = bottom; y < VIEW_BOTTOM; y += 8)
        DrawEdgeBand(bg, y, y + 8, bottom - 8, top, bottom);
    /*
     * The corners: the 8x8 corner block of the picture, in 16-pixel cells
     * each with its own fade, so the gradient bends round the corner instead
     * of being smeared across one quad. Cells already black are left to
     * StageUnderlay when it drew.
     */
    for (int y = top; y > VIEW_TOP; y -= 16)
        for (int x = 0; x > VIEW_LEFT; x -= 16)
        {
            DrawCornerCell(bg, x - 16, x, y - 16, y, 0, top, top, bottom);
            DrawCornerCell(bg, 240 - x, 256 - x, y - 16, y, 232, top, top, bottom);
        }
    for (int y = bottom; y < VIEW_BOTTOM; y += 16)
        for (int x = 0; x > VIEW_LEFT; x -= 16)
        {
            DrawCornerCell(bg, x - 16, x, y, y + 16, 0, bottom - 8, top, bottom);
            DrawCornerCell(bg, 240 - x, 256 - x, y, y + 16, 232, bottom - 8, top, bottom);
        }
    return true;
}

static bool DrawLineBg(unsigned bg)
{
    unsigned baseX = Reg(0x10 + bg * 4) & 511, baseY = Reg(0x12 + bg * 4) & 511;
    /* A still stage fades its margins (DrawEdgeRegion); so do these layers,
     * and every layer of a faded one (the cable car). */
    bool fade = sStage && (!sScrolls[bg] || sStageFaded);
    int top = 0, bottom = 160;
    int clipX0 = sClipX0, clipX1 = sClipX1, clipY0 = sClipY0, clipY1 = sClipY1;
    /* The battle terrain repeats its top tile row above the picture, as
     * DrawBattleBg does; this is the line where that starts. */
    bool clampTop = sBattle && bg == 3;
    C3D_Tex *tex;
    float width, height;
    BattlePictureView view;
    bool picture;

    if (!(LineBackgrounds() & (1u << bg)) || !LayerDrawable(bg)) return false;
    tex = &sLayers[bg].tex;
    width = tex->width;
    height = tex->height;
    if (fade) PictureRows(bg, &top, &bottom);
    /* A move's picture, magnified to the screen (BattlePicture). */
    picture = BattlePictureBegin(bg, &view);
    if (picture)
    {
        clipX0 = sClipX0;
        clipX1 = sClipX1;
        clipY0 = sClipY0;
        clipY1 = sClipY1;
        clampTop = false;
    }
    else if (sBattle && !(Reg(8 + bg * 2) & 0x4000))
    {
        /* A 32-column layer only wraps into the sides when it is made to. */
        MeasureStill(bg);
        if (!sPanorama[bg])
        {
            if (clipX0 < 0) clipX0 = 0;
            if (clipX1 > 240) clipX1 = 240;
            if (clipY0 < 0) clipY0 = 0;
            if (clipY1 > 160) clipY1 = 160;
            clampTop = false;
        }
    }
    ViewBase();
    for (int y = clipY0; y < clipY1;)
    {
        /* The split battle terrain (BattleTerrainLine): line y shows line sy. */
        bool split = BattleTerrainSplit(bg);
        int sy = split ? BattleTerrainLine(y) : y;
        unsigned x = LineValue(bg * 2, sy, baseX), v = LineValue(bg * 2 + 1, sy, baseY);
        int end = y + 1;
        /* The texture row line y shows. */
        int ty = (int)v + sy;
        int rowTop = -(int)(v & 7);

        if (clampTop && sy < rowTop)
        {
            /* The tile row under line 0, once per eight lines above it. */
            int phase = ((sy - rowTop) % 8 + 8) % 8;

            ty = (int)(v - (v & 7)) + phase;
            end = y + 8 - phase;
            if (end > clipY1) end = clipY1;
        }
        else while (end < clipY1 && (!split || BattleTerrainLine(end) - end == sy - y)
               && LineValue(bg * 2, split ? BattleTerrainLine(end) : end, baseX) == x
               && LineValue(bg * 2 + 1, split ? BattleTerrainLine(end) : end, baseY) == v)
        {
            /* The fade is linear between these lines, so a run stops at them. */
            if (fade && (end == top || end == bottom || end == top - (int)FADE_DOWN
                         || end == bottom + (int)FADE_DOWN))
                break;
            ++end;
        }
        /* Left margin, picture, right margin: each piece gets its own fade. */
        for (unsigned piece = 0; piece < 3; ++piece)
        {
            int x0 = fade ? (piece == 0 ? clipX0 : piece == 1 ? 0 : 240) : clipX0;
            int x1 = fade ? (piece == 0 ? 0 : piece == 1 ? 240 : clipX1) : clipX1;

            if (x0 < clipX0) x0 = clipX0;
            if (x1 > clipX1) x1 = clipX1;
            if (x0 >= x1) { if (!fade) break; continue; }
            {
                /* The texture repeats, so coordinates past its edges wrap. */
                const Tex3DS_SubTexture run = {(u16)(x1 - x0), (u16)(end - y),
                    ((int)x + x0) / width, 1.0f - ty / height,
                    ((int)x + x1) / width, 1.0f - (ty + end - y) / height};
                C2D_ImageTint tint = sTint;

                if (fade)
                {
                    bool white;
                    float bright = LayerBrightness(bg, &white);

                    FadeCorner(&tint, C2D_TopLeft, EdgeFade(x0, y, top, bottom), bright, white);
                    FadeCorner(&tint, C2D_TopRight, EdgeFade(x1, y, top, bottom), bright, white);
                    FadeCorner(&tint, C2D_BotLeft, EdgeFade(x0, end, top, bottom), bright, white);
                    FadeCorner(&tint, C2D_BotRight, EdgeFade(x1, end, top, bottom), bright, white);
                }
                ++sStats.tiles;
                C2D_DrawImageAt((C2D_Image){tex, &run}, x0 + CTR_VIEW_X + sLayerShift,
                                y + CTR_VIEW_Y, 0, &tint, 1, 1);
            }
            if (!fade) break;
        }
        y = end;
    }
    if (picture) BattlePictureEnd(&view);
    return true;
}

/*
 * What of a map texture a background shows, as rectangles: the source of the
 * native battle renderer to come. A plan is made from the registers and the
 * cells alone, for a clip of the GBA's picture: it names no target and no
 * view. Drawing it (MapPlanDraw) is where those come in, so the same runs can
 * land on the scene surface at 2x, on a slot, on the edges at the screen's
 * scale, or straight on an eye.
 *
 * Each run is a rectangle of whole tiles: destination in GBA coordinates and
 * the texel of the texture at its top-left corner, which repeats as the
 * tilemap wraps. Whole tiles as the walk draws them, past the clip too, so
 * what the rest of a tile lands on is what it was (BattleSceneComposeMono).
 * A run for each tile row above the picture, and one for the rest.
 */
#define MAP_RUNS_MAX 34
typedef struct
{
    int x0, y0, x1, y1;
    unsigned sx, sy;
} MapRun;

typedef struct
{
    MapRun run[MAP_RUNS_MAX];
    unsigned count;
    /* Kept to this rectangle by a scissor, as a walk that stays in the
     * picture is (DrawBattleCut): the runs overhang it. */
    bool cut;
    int cutX0, cutY0, cutX1, cutY1;
} MapPlan;

/* The clip to show, and the columns what depends on columns is judged over:
 * one whole compose's when the pass is a part of it (sClipWhole). */
typedef struct
{
    int x0, y0, x1, y1, judgeX0, judgeX1;
} MapClip;

/* Whether a cell of the block, wrapped as the map wraps, draws something:
 * what a tile walk finds with EntrySlot, as of this frame's look at them. */
static bool MapCellsShown(unsigned bg, unsigned column, unsigned count, unsigned row, unsigned rowCount)
{
    unsigned size = Reg(8 + bg * 2) >> 14;
    unsigned columns = (size & 1) ? 64 : 32, rows = (size & 2) ? 64 : 32;

    if (count > columns) count = columns;
    if (rowCount > rows) rowCount = rows;
    for (unsigned r = 0; r < rowCount; ++r)
    {
        const uint32_t *line = sCellSig[bg] + ((row + r) & (rows - 1)) * columns;

        for (unsigned i = 0; i < count; ++i)
            if (line[(column + i) & (columns - 1)]) return true;
    }
    return false;
}

/* A run, if any cell of it draws: nothing drawn, nothing counted (sBgDrawn).
 * False when the plan is full. */
static bool MapPlanAdd(MapPlan *plan, unsigned bg, int x0, int y0, int x1, int y1, unsigned sx, unsigned sy)
{
    if (x0 >= x1 || y0 >= y1
        || !MapCellsShown(bg, sx / 8, (unsigned)(x1 - x0) / 8, sy / 8, (unsigned)(y1 - y0) / 8))
        return true;
    if (plan->count == MAP_RUNS_MAX) return false;
    plan->run[plan->count++] = (MapRun){x0, y0, x1, y1, sx, sy};
    return true;
}

/*
 * The rules of DrawBattleBg, over the cells. With wraps, a layer that wraps
 * across the sides (a 64-column map, a panorama): every row where the map
 * wraps it, but those wholly above the picture, which carry on only if the art
 * reaches the top edge, and from where it does. Without, the part of the
 * picture the clip holds, as DrawBattleCut walks it (DrawTextSpanAt): the last
 * tile there is the one whose place before the scroll is by the clip's edge,
 * so on a clip that ends off the tile grid one more that shows is not drawn,
 * and neither is it here. BG3 is the terrain's: its art always carries on above
 * the picture, from its first row where a row has none, and each band of its
 * lines (DrawBattleBgSplit) has a plan of its own, at that band's line shift.
 * False if it does not fit in a plan.
 */
static bool BattleMapPlan(unsigned bg, const MapClip *clip, bool wraps, MapPlan *plan)
{
    unsigned size = Reg(8 + bg * 2) >> 14;
    unsigned scrollX = Reg(0x10 + bg * 4) & 511, scrollY = (Reg(0x12 + bg * 4) + sBattleBgLineShift) & 511;
    unsigned columns = (size & 1) ? 64 : 32, rows = (size & 2) ? 64 : 32;
    int ox = (int)(scrollX & 7), oy = (int)(scrollY & 7);
    int x0 = clip->x0, x1 = clip->x1, y0 = clip->y0, y1 = clip->y1;
    int left, right, top, bottom, py;
    unsigned sx;

    plan->count = 0;
    plan->cut = !wraps;
    if (!wraps)
    {
        if (x0 < 0) x0 = 0;
        if (x1 > 240) x1 = 240;
        if (y0 < 0) y0 = 0;
        if (y1 > 160) y1 = 160;
    }
    plan->cutX0 = x0;
    plan->cutY0 = y0;
    plan->cutX1 = x1;
    plan->cutY1 = y1;
    if (x0 >= x1 || y0 >= y1) return true;
    left = ((x0 + ox) & ~7) - ox;
    top = ((y0 + oy) & ~7) - oy;
    if (wraps)
    {
        right = left + ((x1 - left + 7) & ~7);
        bottom = top + ((y1 - top + 7) & ~7);
    }
    else
    {
        right = x1 / 8 * 8 - ox;
        bottom = y1 / 8 * 8 - oy;
        right += right < x1 ? 8 : 0;
        bottom += bottom < y1 ? 8 : 0;
    }
    sx = (unsigned)(left + (int)scrollX) & (columns * 8 - 1);
    py = top;
    if (wraps && py + 8 <= 0)
    {
        int judged = ((clip->judgeX0 + ox) & ~7) - ox;
        unsigned firstColumn = (unsigned)(judged + (int)scrollX) >> 3;
        unsigned count = (unsigned)(clip->judgeX1 - judged + 7) / 8 + 1;
        bool above = bg == 3 || MapCellsShown(bg, firstColumn, count, (scrollY >> 3) & (rows - 1), 1);

        for (; py + 8 <= 0 && py < bottom; py += 8)
        {
            unsigned row = ((unsigned)(py + (int)scrollY) >> 3) & (rows - 1);

            if (!above) continue;
            if (!MapCellsShown(bg, firstColumn, count, row, 1))
            {
                if (rows == 64 && MapCellsShown(bg, firstColumn, count, row ^ 32, 1)) row ^= 32;
                else if (bg == 3) row = (scrollY >> 3) & (rows - 1);
                else continue;
            }
            if (!MapPlanAdd(plan, bg, left, py, right, py + 8, sx, row * 8)) return false;
        }
    }
    return MapPlanAdd(plan, bg, left, py, right, bottom, sx, (unsigned)(py + (int)scrollY) & (rows * 8 - 1));
}

/*
 * Whether a plan can be drawn from the texture: false if one of its cells is
 * stale, and the layer is walked for this draw (never expected, and counted: the
 * window stood short). What it asked for widens the next frames' window
 * (LayerWindow), by its clip: runs with nothing drawn are not in the plan, and
 * they can have something drawn by the time they scroll on.
 */
static bool MapPlanFresh(unsigned bg, const MapPlan *plan)
{
    LayerTexture *layer = &sLayers[bg];
    unsigned size = Reg(8 + bg * 2) >> 14;
    unsigned columns = (size & 1) ? 64 : 32, rows = (size & 2) ? 64 : 32;

    if (plan->cutX0 < plan->cutX1 && plan->cutY0 < plan->cutY1)
    {
        if (layer->readX0 >= layer->readX1)
        {
            layer->readX0 = (int16_t)plan->cutX0;
            layer->readX1 = (int16_t)plan->cutX1;
            layer->readY0 = (int16_t)plan->cutY0;
            layer->readY1 = (int16_t)plan->cutY1;
        }
        if (plan->cutX0 < layer->readX0) layer->readX0 = (int16_t)plan->cutX0;
        if (plan->cutX1 > layer->readX1) layer->readX1 = (int16_t)plan->cutX1;
        if (plan->cutY0 < layer->readY0) layer->readY0 = (int16_t)plan->cutY0;
        if (plan->cutY1 > layer->readY1) layer->readY1 = (int16_t)plan->cutY1;
    }
    if (!sCellsStale[bg]) return true;
    for (unsigned i = 0; i < plan->count; ++i)
    {
        const MapRun *run = &plan->run[i];
        int firstColumn = (int)run->sx >> 3, firstRow = (int)run->sy >> 3;
        uint64_t mask = CellsMask(firstColumn, ((int)run->sx + run->x1 - run->x0 - 1) >> 3, columns);
        unsigned count = (unsigned)(((int)run->sy + run->y1 - run->y0 - 1) >> 3) - (unsigned)firstRow + 1;

        for (unsigned r = 0; r < count && r < rows; ++r)
            if (LayerRowStale(bg, ((unsigned)firstRow + r) & (rows - 1), columns, mask))
            {
                ++sLayerPasses.missed;
                return false;
            }
    }
    return true;
}

/* A plan on the target and at the view of the moment: the blend, the tint and
 * the depth are the caller's, as they are for a tile walk. */
static void MapPlanDraw(C3D_Tex *tex, const MapPlan *plan)
{
    float width = tex->width, height = tex->height;

    if (plan->count == 0) return;
    if (plan->cut)
    {
        C2D_Flush();
        Scissor(plan->cutX0, plan->cutY0, plan->cutX1, plan->cutY1);
    }
    ViewBase();
    for (unsigned i = 0; i < plan->count; ++i)
    {
        const MapRun *run = &plan->run[i];
        unsigned w = (unsigned)(run->x1 - run->x0), h = (unsigned)(run->y1 - run->y0);
        /* The texture repeats, so coordinates past its edges wrap. */
        const Tex3DS_SubTexture cut = {(u16)w, (u16)h, run->sx / width, 1.0f - run->sy / height,
                                       (run->sx + w) / width, 1.0f - (run->sy + h) / height};

        if (++sStats.tiles >= DRAW_BUDGET)
        {
            Error(2, "GPU geometry budget exceeded");
            break;
        }
        if (!C2D_DrawImageAt((C2D_Image){tex, &cut}, run->x0 + CTR_VIEW_X + sLayerShift, run->y0 + CTR_VIEW_Y, 0,
                             &sTint, 1, 1))
            Error(3, "Citro2D geometry submission failed");
    }
    if (plan->cut)
    {
        C2D_Flush();
        RestoreScissor();
    }
}

/* Drawn from its map texture this frame, if it is not a picture: wanted, its
 * cells looked at this frame and whole, and with no tile bank among them. */
static bool BattleLayerReady(unsigned bg)
{
    return (sBattleLayerWanted & (1u << bg)) && (sLayerFresh & (1u << bg)) && LayerDrawable(bg)
        && sCellsBanked[bg] == 0;
}

/* Whether a battle layer wraps: a 64-column map, or one the frame measures
 * as a panorama. The measure falls here for the walk and for the eyes alike. */
static bool LayerWrapsMeasured(unsigned bg)
{
    if (Reg(8 + bg * 2) & 0x4000) return true;
    MeasureStill(bg);
    return sPanorama[bg];
}

/*
 * A large background of the 2D battle from its texture, in place of DrawBattleBg
 * and under the blend of the moment (Layers), so in the blend passes of a slot
 * and in the bands of BLDALPHA too; false to walk it, as it was. A move's
 * picture stays walked: it is sampled at its magnified scale (BattlePicture),
 * not on the scene's quarter texels. Measured and decided as the walk does: the
 * picture first, then whether it wraps (the frame's measure falls here as it
 * falls there), then a layer only measured.
 */
static bool DrawBattleBgTex(unsigned bg)
{
    MapPlan plan;
    bool wraps;

    /* The terrain goes by its bands (DrawBattleTerrainTex). */
    if (!BattleLayerReady(bg) || BattlePicture(bg) || BattleTerrainSplit(bg)) return false;
    wraps = LayerWrapsMeasured(bg);
    if (!(sLayerMeasureOnly & (1u << bg)))
    {
        const MapClip clip = {sClipX0, sClipY0, sClipX1, sClipY1, sClipWhole.on ? sClipWhole.x0 : sClipX0,
                              sClipWhole.on ? sClipWhole.x1 : sClipX1};

        if (!BattleMapPlan(bg, &clip, wraps, &plan) || !MapPlanFresh(bg, &plan)) return false;
        MapPlanDraw(&sLayers[bg].tex, &plan);
    }
    sBattleLayerDrawn |= 1u << bg;
    sBattleLayerUsed[bg] = sStats.frames;
    return true;
}

/*
 * A band of the split terrain's lines from its texture, in place of the walk of
 * DrawBattleBg, which comes here with the band's scissor and line shift set and
 * all that goes before the walk decided as ever: the picture, whether it wraps,
 * a layer only measured. False to walk the band, as it was.
 */
static bool DrawBattleTerrainTex(unsigned bg, int x0, int y0, int x1, int y1)
{
    const MapClip clip = {x0, y0, x1, y1, sClipWhole.on ? sClipWhole.x0 : x0, sClipWhole.on ? sClipWhole.x1 : x1};
    MapPlan plan;

    if (!BattleTerrainSplit(bg) || !BattleLayerReady(bg) || !BattleMapPlan(bg, &clip, true, &plan)
        || !MapPlanFresh(bg, &plan))
        return false;
    MapPlanDraw(&sLayers[bg].tex, &plan);
    sBattleLayerDrawn |= 1u << bg;
    sBattleLayerUsed[bg] = sStats.frames;
    return true;
}

/*
 * A composition kept this frame (sBgDrawn is what it holds): the large layers it
 * shows from their textures were used, though nothing drew them. Without this a
 * layer standing still lost its texture's hold, and the key its term: composed
 * by walking, then from the texture again, every BATTLE_LAYER_HOLD_FRAMES. The
 * key says the composition is one from the texture (SCENE_PART_DRAWABLE); a
 * layer that changes, empties or stops being shown stops coming here.
 */
static void BattleLayersKept(void)
{
    for (unsigned bg = 1; bg < 4; ++bg)
        if (sBgDrawn[bg] && BattleLayerReady(bg))
        {
            sBattleLayerKept |= 1u << bg;
            sBattleLayerUsed[bg] = sStats.frames;
        }
}

/* A walk of a layer of the battle, for what asks for its texture. */
static void BattleLayerWalked(unsigned bg, unsigned quads)
{
    if (!sBattle || bg == 0 || (bg == 3 && !BattleTerrainSplit(bg))) return;
    if (quads > sBattleWalk[bg]) sBattleWalk[bg] = quads;
    if (quads >= BATTLE_LAYER_QUADS && BattlePicture(bg)) sBattleLayerPicture |= 1u << bg;
}

static void DrawAffineBg(unsigned bg)
{
    /*
     * The Berry Blender's bowl turns on an affine layer that wraps round: in
     * the margins, where centred screens carry their affine maps on (below),
     * copies of the bowl went round the screen's edges. Kept to the picture.
     */
    if (sCentred && sCentredScreen == CTR_CENTRED_BLENDER
     && (sClipX0 < 0 || sClipY0 < 0 || sClipX1 > 240 || sClipY1 > 160))
    {
        int clipX0 = sClipX0, clipY0 = sClipY0, clipX1 = sClipX1, clipY1 = sClipY1;
        bool scissored = sScissored;

        if (sClipX0 < 0) sClipX0 = 0;
        if (sClipY0 < 0) sClipY0 = 0;
        if (sClipX1 > 240) sClipX1 = 240;
        if (sClipY1 > 160) sClipY1 = 160;
        if (sClipX0 < sClipX1 && sClipY0 < sClipY1)
        {
            C2D_Flush();
            sScissored = true;
            Scissor(sClipX0, sClipY0, sClipX1, sClipY1);
            DrawAffineBg(bg);
            C2D_Flush();
        }
        sClipX0 = clipX0;
        sClipY0 = clipY0;
        sClipX1 = clipX1;
        sClipY1 = clipY1;
        sScissored = scissored;
        RestoreScissor();
        return;
    }
    unsigned control = Reg(8 + bg * 2), base = bg == 2 ? 0x20 : 0x30;
    float a = (int16_t)Reg(base) / 256.0f, b = (int16_t)Reg(base + 2) / 256.0f;
    float c = (int16_t)Reg(base + 4) / 256.0f, d = (int16_t)Reg(base + 6) / 256.0f;
    float rx = CtrVideo_AffineReference(Reg(base + 8) | (Reg(base + 10) << 16)) / 256.0f;
    float ry = CtrVideo_AffineReference(Reg(base + 12) | (Reg(base + 14) << 16)) / 256.0f;
    float det = a * d - b * c;
    if (fabsf(det) < 0.00001f) { Error(5, "singular affine BG matrix"); return; }
    C3D_Mtx matrix;
    Mtx_Identity(&matrix);
    matrix.r[0] = FVec4_New(d / det, -b / det, 0, (b * ry - d * rx) / det + CTR_VIEW_X + sLayerShift);
    matrix.r[1] = FVec4_New(-c / det, a / det, 0, (c * rx - a * ry) / det + CTR_VIEW_Y);
    ViewAffine(&matrix);
    /* The box of the map under the clip rectangle's four corners. */
    float minX = INFINITY, maxX = -INFINITY, minY = INFINITY, maxY = -INFINITY;
    for (unsigned corner = 0; corner < 4; ++corner)
    {
        float x = (corner & 1) ? sClipX1 : sClipX0;
        float y = (corner & 2) ? sClipY1 : sClipY0;
        float sx = rx + a * x + b * y, sy = ry + c * x + d * y;
        minX = fminf(minX, sx); maxX = fmaxf(maxX, sx);
        minY = fminf(minY, sy); maxY = fmaxf(maxY, sy);
    }
    int firstX = (int)floorf(minX / 8), lastX = (int)floorf(maxX / 8);
    int firstY = (int)floorf(minY / 8), lastY = (int)floorf(maxY / 8);
    unsigned tiles = 16u << (control >> 14), mask = tiles - 1;
    unsigned map = ((control >> 8) & 31) * 0x800, chars = ((control >> 2) & 3) * 0x4000;
    /*
     * A centred GBA screen fills its margins with its affine map repeated,
     * wrap-around or not: without it the space above and left of the picture
     * would be empty while the map's own sea shows below and to the right.
     * Not the roulette's wheel: repeated, copies of it turned round the
     * margins beside the table.
     */
    bool wrap = (control & 0x2000) || (sCentred && sCentredScreen != CTR_CENTRED_ROULETTE);
    /*
     * Under minification the screen maps to far more of the map than exists.
     * Without wrap-around nothing outside the map is drawn anyway, so clamping
     * the scan to it bounds the work by the map instead of by how small the
     * layer was scaled: the title logo shrinks enough to blow any fixed budget
     * and would disappear for those frames.
     */
    if (!wrap)
    {
        if (firstX < 0) firstX = 0;
        if (firstY < 0) firstY = 0;
        if (lastX >= (int)tiles) lastX = (int)tiles - 1;
        if (lastY >= (int)tiles) lastY = (int)tiles - 1;
    }
    if (lastX < firstX || lastY < firstY) return;
    if ((lastX - firstX + 1) * (lastY - firstY + 1) > 16384)
    { Error(6, "affine BG minification exceeds baseline budget"); return; }
    for (int y = firstY; y <= lastY; ++y)
        for (int x = firstX; x <= lastX; ++x)
        {
            if (!wrap && ((unsigned)x >= tiles || (unsigned)y >= tiles)) continue;
            unsigned entry = map + ((unsigned)y & mask) * tiles + ((unsigned)x & mask);
            if (entry >= 0x10000) { Error(7, "affine map outside BG VRAM"); continue; }
            DrawTile(chars + sMemory.vram[entry] * 64, 0, true, x * 8, y * 8, false, false);
        }
    ViewBase();
}
