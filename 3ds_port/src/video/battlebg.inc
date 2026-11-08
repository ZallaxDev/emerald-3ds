/*
 * Part of 3ds_video.c. The battle's backgrounds: the terrain split between
 * the sides, a move's one-picture background (BattlePicture) and
 * DrawBattleBg.
 */

/*
 * The battle terrain split at CTR_BATTLE_SIDE_SPLIT (ctr_battle_layout.h):
 * the opponent's half above it drawn CTR_BATTLE_ENEMY_RISE lines higher, the
 * player's half below it CTR_BATTLE_PLAYER_DROP lower, as the game moves the
 * two sides' sprites. Only while BG3 holds the terrain, not a move's
 * background, which is drawn for the whole screen.
 */
static bool BattleTerrainSplit(unsigned bg)
{
    return sBattle && bg == 3 && !CtrBattleBg_MoveBgShown();
}

/*
 * The terrain line shown on line y of the split terrain. Between the two
 * halves the lines are filled from the four just below the split - open
 * ground on every terrain, the opponent's platform ending above it and the
 * player's starting well below - repeated: the terrain's stripes repeat
 * every four lines, so they carry on the half below without a seam.
 */
static int BattleTerrainLine(int y)
{
    const int top = CTR_BATTLE_SIDE_SPLIT - CTR_BATTLE_ENEMY_RISE;
    const int bottom = CTR_BATTLE_SIDE_SPLIT + CTR_BATTLE_PLAYER_DROP;

    if (y < top) return y + CTR_BATTLE_ENEMY_RISE;
    if (y < bottom) return CTR_BATTLE_SIDE_SPLIT + ((y - bottom) & 3);
    /* The terrain's map is empty from line 112, where the GBA's text box
     * began: the scene goes on below it with its last four lines. */
    if (y - CTR_BATTLE_PLAYER_DROP >= BATTLE_BAND_TOP)
        return BATTLE_BAND_TOP - 4 + ((y - CTR_BATTLE_PLAYER_DROP - BATTLE_BAND_TOP) & 3);
    return y - CTR_BATTLE_PLAYER_DROP;
}

/*
 * A move's background that is one picture for the GBA's screen - Psychic,
 * Thunder, Ice, Dark, Solar Beam, the impacts: art over the 240x160 picture
 * and nothing in the rows the GBA never shows - has nothing of its own to
 * carry into the margins the battle has round that picture: the GBA's wrap
 * left bands above it and at its sides, or its top row repeated. Such a layer
 * is drawn magnified instead, uniformly, to cover the whole top screen with a
 * few pixels to spare for the 3D slider's displacement: a GBA point p at
 * f + k (p - f). The fixed point f is the lowest that still brings the
 * picture's top edge over the screen's, so the battlers' part of the art
 * moves as little as it can. Backgrounds made to wrap (Sandstorm, the
 * water, the fog) keep the wrap, which is art; a battler copied into a
 * background covers a corner of the picture, not all of it, and keeps its
 * place; the terrain is never one.
 */
#define BATTLE_PICTURE_SPARE 4.0f
#define BATTLE_PICTURE_SCALE ((CTR_GAME_WIDTH / CTR_BATTLE_ZOOM + 2 * BATTLE_PICTURE_SPARE) / 240.0f)
/* GBA line on the screen's top edge: (120, 112) is on screen (200, 192). */
#define BATTLE_PICTURE_SCREEN_TOP (112.0f - (CTR_GAME_HEIGHT - 48) / CTR_BATTLE_ZOOM)
#define BATTLE_PICTURE_FIX_Y ((BATTLE_PICTURE_SCREEN_TOP - BATTLE_PICTURE_SPARE) / (1.0f - BATTLE_PICTURE_SCALE))

static bool BattleLayerReady(unsigned bg);
static const uint32_t *LayerCellSigs(unsigned bg);

static bool BattlePicture(unsigned bg)
{
    static uint32_t frame[4];
    static bool picture[4];
    unsigned control, size, map, chars, scrollX, scrollY, rows;
    unsigned shown = 0, hidden = 0, hiddenTiles = 0, lastEntry = ~0u;
    bool color256, lastFull = false;
    const uint32_t *cells;

    if (!sBattle || bg == 0 || bg > 3 || (bg == 3 && !CtrBattleBg_MoveBgShown())) return false;
    /* Once a frame: the frame's number, one on so that none is 0. */
    if (frame[bg] == sStats.frames + 1) return picture[bg];
    frame[bg] = sStats.frames + 1;
    control = Reg(8 + bg * 2);
    size = control >> 14;
    map = ((control >> 8) & 31) * 0x800;
    chars = ((control >> 2) & 3) * 0x4000;
    color256 = (control & 128) != 0;
    scrollX = Reg(0x10 + bg * 4) & 511;
    scrollY = Reg(0x12 + bg * 4) & 511;
    rows = (size & 2) ? 64 : 32;
    /* With its cells looked at this frame, they say what the tiles would. */
    cells = BattleLayerReady(bg) ? LayerCellSigs(bg) : NULL;
    /* The picture's 30x20 tiles, and below it the rows the GBA never shows. */
    for (unsigned r = 0; r < rows; ++r)
        for (unsigned c = 0; c < 30; ++c)
        {
            if (cells)
            {
                unsigned columns = (size & 1) ? 64 : 32;

                lastFull = cells[(((scrollY >> 3) + r) & (rows - 1)) * columns
                                 + (((scrollX >> 3) + c) & (columns - 1))] != 0;
            }
            else
            {
                unsigned entry = MapEntry(map, size, (scrollX >> 3) + c, (scrollY >> 3) + r);

                if (entry != lastEntry)
                {
                    lastEntry = entry;
                    lastFull = EntrySlot(chars, entry, color256) >= 0;
                }
            }
            if (r < 20) shown += lastFull;
            else
            {
                hidden += lastFull;
                ++hiddenTiles;
            }
        }
    picture[bg] = shown * 5 >= 600 * 4 && hidden * 10 <= hiddenTiles;
    return picture[bg];
}

typedef struct
{
    float zoom, offX, offY;
    int clipX0, clipY0, clipX1, clipY1;
    bool scissored;
} BattlePictureView;

/*
 * The magnified view of a picture layer (BattlePicture), or false to draw it
 * as it is. The clip becomes the part of the picture that lands in it, and
 * the scissor keeps what is drawn to where it was; BattlePictureEnd after.
 */
static bool BattlePictureBegin(unsigned bg, BattlePictureView *saved)
{
    const float k = BATTLE_PICTURE_SCALE, fx = 120.0f, fy = BATTLE_PICTURE_FIX_Y;

    if (!BattlePicture(bg)) return false;
    *saved = (BattlePictureView){sZoom, sOffX, sOffY, sClipX0, sClipY0, sClipX1, sClipY1, sScissored};
    C2D_Flush();
    Scissor(sClipX0, sClipY0, sClipX1, sClipY1);
    sScissored = true;
    sClipX0 = (int)floorf(fx + (saved->clipX0 - fx) / k);
    sClipX1 = (int)ceilf(fx + (saved->clipX1 - fx) / k);
    sClipY0 = (int)floorf(fy + (saved->clipY0 - fy) / k);
    sClipY1 = (int)ceilf(fy + (saved->clipY1 - fy) / k);
    if (sClipX0 < 0) sClipX0 = 0;
    if (sClipY0 < 0) sClipY0 = 0;
    if (sClipX1 > 240) sClipX1 = 240;
    if (sClipY1 > 160) sClipY1 = 160;
    /* A point drawn at p + view lands at off + zoom (k (p + view) + (1 - k) (f + view)). */
    sOffX += sZoom * (1.0f - k) * (fx + CTR_VIEW_X);
    sOffY += sZoom * (1.0f - k) * (fy + CTR_VIEW_Y);
    sZoom *= k;
    return true;
}

static void BattlePictureEnd(const BattlePictureView *saved)
{
    C2D_Flush();
    sZoom = saved->zoom;
    sOffX = saved->offX;
    sOffY = saved->offY;
    sClipX0 = saved->clipX0;
    sClipY0 = saved->clipY0;
    sClipX1 = saved->clipX1;
    sClipY1 = saved->clipY1;
    sScissored = saved->scissored;
    ViewBase();
    RestoreScissor();
}

/* Lines the tile walk below shows moved by: the line it draws at y is y + this. */
static int sBattleBgLineShift;
/*
 * A pass that composes a part of what one compose would, cut at a column (the
 * mono battle's kept scenery and the strip past it, BattleSceneComposeMono):
 * the columns that one compose's clip spans in the rectangle being drawn
 * (ComposeBand). What a layer decides from its clip's columns - which rows
 * above the picture carry on, and from where (DrawBattleBg) - is decided over
 * these, so each part comes out as that compose's own; only the walk is cut.
 */
static struct { bool on; int x0, x1; } sClipWhole;

/*
 * Layers of the battle's edges whose maps name only clear tiles
 * (MonoSceneBlank): the compose still goes through them, so that what it
 * decides from the layers it draws (ComposeAlphaBands) and the measures it
 * takes of them (MeasureStill, its 8-frame turn) are the same, but nothing of
 * them is walked. Set only round BattleEdgesCompose.
 */
static unsigned sLayerMeasureOnly;
static bool DrawBattleTerrainTex(unsigned bg, int x0, int y0, int x1, int y1);

static void DrawBattleBg(unsigned bg)
{
    unsigned control = Reg(8 + bg * 2), size = control >> 14;
    unsigned map = ((control >> 8) & 31) * 0x800;
    unsigned chars = ((control >> 2) & 3) * 0x4000;
    bool color256 = (control & 128) != 0;
    unsigned scrollX = Reg(0x10 + bg * 4) & 511, scrollY = (Reg(0x12 + bg * 4) + sBattleBgLineShift) & 511;
    unsigned columns = (size & 1) ? 64 : 32, rows = (size & 2) ? 64 : 32;
    int ox = (int)(scrollX & 7), oy = (int)(scrollY & 7);
    int x0 = sClipX0, x1 = sClipX1, y0 = sClipY0, y1 = sClipY1;
    unsigned firstColumn, count;
    unsigned lastEntry = ~0u;
    int lastSlot = -1;
    bool wraps = columns == 64;
    BattlePictureView picture;

    if (BattlePictureBegin(bg, &picture))
    {
        if (sClipX0 < sClipX1 && sClipY0 < sClipY1 && !(sLayerMeasureOnly & (1u << bg)))
            DrawTextSpanAt(bg, sClipX0, sClipX1, sClipY0, sClipY1, -1, -1);
        BattlePictureEnd(&picture);
        return;
    }
    if (!wraps)
    {
        MeasureStill(bg);
        wraps = sPanorama[bg];
    }
    /* A blank layer of the edges (BattleEdgesCompose): measured as the
     * compose measures it, nothing walked - it draws nothing. */
    if (sLayerMeasureOnly & (1u << bg)) return;
    if (!wraps)
    {
        DrawBattleCut(bg, 0, 240, 0, 160, 0);
        return;
    }
    if (!ClipCell(&x0, &x1, &y0, &y1)) return;
    if (DrawBattleTerrainTex(bg, x0, y0, x1, y1)) return;
    {
        /* The columns the rows are judged over: one compose's (sClipWhole). */
        int from = sClipWhole.on ? sClipWhole.x0 : x0, to = sClipWhole.on ? sClipWhole.x1 : x1;

        firstColumn = (unsigned)(((from + ox) & ~7) - ox + (int)scrollX) >> 3;
        count = (unsigned)(to - (((from + ox) & ~7) - ox) + 7) / 8 + 1;
    }
    /*
     * Above the picture a layer only carries on if its art reaches the top
     * edge. The entry grass of the intro is a band that slides down out of
     * the picture: its map wraps it round to the top, which the GBA never
     * shows and which here repeated the grass along the top of the screen.
     */
    bool above = bg == 3 || BattleRowShown(map, size, chars, color256,
                                           (scrollY >> 3) & (rows - 1), firstColumn, count);
    bool batch;

    ViewBase();
    batch = TileWalkBegin();
    for (int py = ((y0 + oy) & ~7) - oy; py < y1; py += 8)
    {
        int row = (int)(((unsigned)(py + (int)scrollY) >> 3) & (rows - 1));

        if (py + 8 <= 0 && !above) continue;
        /* A row wholly above the picture: where its art comes from. */
        if (py + 8 <= 0 && !BattleRowShown(map, size, chars, color256, row, firstColumn, count))
        {
            if (rows == 64 && BattleRowShown(map, size, chars, color256, row ^ 32, firstColumn, count))
                row ^= 32;
            else if (bg == 3)
                row = (int)((scrollY >> 3) & (rows - 1));
            else
                continue;
        }
        for (int px = ((x0 + ox) & ~7) - ox; px < x1; px += 8)
        {
            unsigned column = ((unsigned)(px + (int)scrollX) >> 3) & (columns - 1);
            unsigned entry = MapEntry(map, size, column, (unsigned)row);

            if (entry != lastEntry)
            {
                lastEntry = entry;
                lastSlot = EntrySlot(chars, entry, color256);
            }
            if (lastSlot >= 0)
                TileWalkSlot(batch, lastSlot, px + CTR_VIEW_X + sLayerShift, py + CTR_VIEW_Y,
                             entry & 1024, entry & 2048);
        }
    }
    TileWalkEnd(batch);
}

/* Lines [y0, y1) of a battle background, each showing the line `shift` below it. */
static void DrawBattleBgBand(unsigned bg, int y0, int y1, int shift)
{
    int x0 = sClipX0, x1 = sClipX1, clipY0 = sClipY0, clipY1 = sClipY1;

    if (!ClipCell(&x0, &x1, &y0, &y1)) return;
    C2D_Flush();
    Scissor(x0, y0, x1, y1);
    sClipY0 = y0;
    sClipY1 = y1;
    sBattleBgLineShift = shift;
    DrawBattleBg(bg);
    sBattleBgLineShift = 0;
    sClipY0 = clipY0;
    sClipY1 = clipY1;
    C2D_Flush();
    RestoreScissor();
}

/* A battle background, the terrain split between the two sides (BattleTerrainLine). */
static void DrawBattleBgSplit(unsigned bg)
{
    int y0 = sClipY0, y1 = sClipY1;

    if (!BattleTerrainSplit(bg))
    {
        DrawBattleBg(bg);
        return;
    }
    /* In runs of lines drawn with the same shift: each half, and four lines
     * at a time between them and below the terrain's end. */
    for (int y = y0; y < y1;)
    {
        int shift = BattleTerrainLine(y) - y, end = y + 1;

        while (end < y1 && BattleTerrainLine(end) - end == shift) ++end;
        DrawBattleBgBand(bg, y, end, shift);
        y = end;
    }
}
