/*
 * Part of 3ds_video.c. Centred screens: what fills each one's margins
 * (sCentredFills), DrawCentredBg, and DrawTextBg, the plain walk of a text
 * background.
 */

/*
 * How a centred screen fills the top screen around its 240x160 picture.
 *
 * - layers: the backgrounds carried out to the edges, each margin tile
 *   repeating the tilemap's edge tile in its row, column or corner - moved
 *   inwards by inset (left, top, right, bottom, in tiles) past a border, so
 *   what repeats is the pattern inside it. The edge is the tilemap's, not the
 *   screen's: when the speech slides its background (the platform moving
 *   aside for the player), the margins keep showing the empty side of it
 *   rather than the platform that slid to the edge. across keeps a fill to
 *   the sides. Those screens' backgrounds are a
 *   plain field or a pattern of one tile, so it carries on as if the screen
 *   were wider: the clock's teal, the starter's meadow, the naming screen's
 *   stripes and title bar, the sky of the professor's speech.
 * - skyRows: the speech's sky, the first rows of its background, drawn from
 *   the top of the screen rather than the top of the picture, so it starts at
 *   the top-left corner and carries on to the right as the rest does.
 * - band: the text box of the professor's speech (BG0 from bandRow down)
 *   drawn moved by (bandX, bandY), to where the overworld shows its text box
 *   (menu.c, sStandardTextBox_WindowTemplates, and CTR_FIELD_UI_SHIFT). It
 *   shares its callback and its BG0 with the title menu, whose windows go
 *   down to the bottom of the screen; the speech is told apart by the
 *   character base its BG0 uses (main_menu.c, sBirchBgTemplate).
 */
typedef struct
{
    uint8_t layers;
    int8_t inset[4];
    /* Only beside the picture, nothing above or below it. */
    bool across;
    /* Instead of layers: the visible background drawn behind the others. */
    bool backmost;
    /* The layers' margins read on through the tilemap, as the GBA wraps it,
     * instead of repeating the picture's edge row and column. */
    bool wrap;
    /* Instead: one tile of the layer, at tileColumn, tileRow, over all the
     * margins - a backdrop of stripes the picture's edges only cut. */
    bool tile;
    uint8_t tileColumn, tileRow;
    /* Or that tilemap entry itself, whatever the tilemap holds now. */
    bool entry;
    uint16_t tileEntry;
    uint8_t skyRows, bandRow, bandChars;
    int8_t bandX, bandY;
} CentredFill;

static const CentredFill sCentredFills[CTR_CENTRED_SCREENS] =
{
    [CTR_CENTRED_MAIN_MENU] = {.layers = 1u << 1, .across = true, .skyRows = 4, .bandRow = 14, .bandChars = 3,
                               .bandX = -4, .bandY = 32},
    /* Inside the yellow border; the title bar is its top row. */
    [CTR_CENTRED_NAMING] = {.layers = 1u << 3, .inset = {2, 0, 2, 1}},
    [CTR_CENTRED_CLOCK] = {.layers = 1u << 3},
    /* The meadow round the bag: one plain green tile at every edge of it. */
    [CTR_CENTRED_STARTER] = {.layers = 1u << 2},
    /* Whatever background is at the back of the PokéNav screen on show
     * (CentredLayers): the dots, the Hoenn sea, the ribbons' wood. */
    [CTR_CENTRED_POKENAV] = {.backmost = true},
    /* The PC's scrolling pattern, which the GBA wraps round its 32x32 map. */
    [CTR_CENTRED_STORAGE] = {.layers = 1u << 3, .wrap = true},
    [CTR_CENTRED_SUMMARY] = {0},
    [CTR_CENTRED_BAG] = {0},
    [CTR_CENTRED_BAG_WHOLE] = {0},
    [CTR_CENTRED_SHOP] = {0},
    /* The Pokédex: a tile its screen on show names (CentredFillOf), or else
     * whatever is at the back of it. */
    [CTR_CENTRED_POKEDEX] = {.backmost = true},
    [CTR_CENTRED_POKEDEX_CAUGHT] = {.layers = 1u << 3, .tile = true, .tileColumn = 0, .tileRow = 2},
    [CTR_CENTRED_PARTY] = {0},
    [CTR_CENTRED_PARTY_WHOLE] = {0},
    /* The trade menu's two layers of stripes, which repeat across. */
    [CTR_CENTRED_TRADE_MENU] = {.layers = (1u << 2) | (1u << 3), .wrap = true},
    /* The trade itself on its black. */
    [CTR_CENTRED_TRADE] = {0},
};

int CtrPokenavList_Bg(void);
/* pokedex.c: the tile of a layer the Pokédex's screen on show carries out to
 * the edges, as bg << 16 | column << 8 | row, or with 1 << 20 as
 * bg << 16 | the tilemap entry; -1 when it names none. */
int CtrPokedex_Backdrop(void);

/* How a centred screen's margins are filled. The Pokédex's depend on which of
 * its screens is up: its stripes, the search's green, the area map's sea. */
static const CentredFill *CentredFillOf(unsigned screen)
{
    static CentredFill dex;
    int backdrop;

    if (screen != CTR_CENTRED_POKEDEX || (backdrop = CtrPokedex_Backdrop()) < 0) return &sCentredFills[screen];
    dex = (CentredFill){.layers = (uint8_t)(1u << ((backdrop >> 16) & 3)), .tile = true,
                        .tileColumn = (uint8_t)(backdrop >> 8), .tileRow = (uint8_t)backdrop,
                        .entry = (backdrop & (1 << 20)) != 0, .tileEntry = (uint16_t)backdrop};
    return &dex;
}

/* The backgrounds a centred screen carries out to its edges. */
static unsigned CentredLayers(const CentredFill *fill)
{
    unsigned display = Reg(0), best = 4, priority = 0;
    int list;

    if (!fill->backmost) return fill->layers;
    if ((display & 7) > 2) return 0;
    /* A PokéNav list scrolls behind the screen's frame: the frame is what
     * goes on past the picture, not the list's next entries. */
    list = CtrPokenavList_Bg();
    for (unsigned bg = 0; bg < 4; ++bg)
        if ((display & (0x100u << bg)) && (Reg(8 + bg * 2) & 3) >= priority && (int)bg != list
         && !((display & 7) == 1 && bg == 3) && !((display & 7) == 2 && bg < 2))
        {
            priority = Reg(8 + bg * 2) & 3;
            best = bg;
        }
    return best < 4 ? 1u << best : 0;
}

static void DrawCentredSpan(unsigned bg, int left, int right, int top, int bottom)
{
    if (left < sClipX0 - sSpanShiftX) left = sClipX0 - sSpanShiftX;
    if (right > sClipX1 - sSpanShiftX) right = sClipX1 - sSpanShiftX;
    if (top < sClipY0 - sSpanShiftY) top = sClipY0 - sSpanShiftY;
    if (bottom > sClipY1 - sSpanShiftY) bottom = sClipY1 - sSpanShiftY;
    if (left < right && top < bottom) DrawTextSpan(bg, left, right, top, bottom);
}

static int FloorDiv8(int value)
{
    return value >= 0 ? value / 8 : -((7 - value) / 8);
}

/* The margins of the picture's rows from, up to to (beside it: across). */
static void DrawCentredMargins(unsigned bg, const CentredFill *fill, int from, int to)
{
    unsigned control = Reg(8 + bg * 2), size = control >> 14;
    unsigned map = ((control >> 8) & 31) * 0x800;
    unsigned chars = ((control >> 2) & 3) * 0x4000;
    bool color256 = (control & 128) != 0;
    unsigned scrollX = Reg(0x10 + bg * 4) & 511, scrollY = Reg(0x12 + bg * 4) & 511;
    unsigned mask = (size & 1) ? 63u : 31u, rows = (size & 2) ? 63u : 31u;
    int ox = (int)(scrollX & 7), oy = (int)(scrollY & 7);
    bool batch;

    ViewBase();
    batch = TileWalkBegin();
    if (from < FloorDiv8(sClipY0 - sSpanShiftY + oy)) from = FloorDiv8(sClipY0 - sSpanShiftY + oy);
    for (int w = from; w < to && w * 8 - oy + sSpanShiftY < sClipY1; ++w)
    {
        bool beside = w >= 0 && w < 20;
        unsigned row = fill->tile ? fill->tileRow
                     : fill->wrap ? (unsigned)(w + (int)(scrollY >> 3)) & rows
                     : w < 0 ? (unsigned)fill->inset[1] : w >= 20 ? 19u - (unsigned)fill->inset[3]
                     : (unsigned)(w + (int)(scrollY >> 3));
        unsigned rowBase;

        if (!beside && fill->across) continue;
        rowBase = map + CtrVideo_TextMapOffset(0, row, size);
        for (int v = FloorDiv8(sClipX0 + ox); v * 8 - ox < sClipX1; ++v)
        {
            bool inside = v >= 0 && v < 30;
            unsigned column, entry, address;
            int slot;

            if (inside && beside) continue;
            column = fill->tile ? fill->tileColumn
                   : fill->wrap ? ((unsigned)v + (scrollX >> 3)) & mask
                   : v < 0 ? (unsigned)fill->inset[0] : v >= 30 ? 29u - (unsigned)fill->inset[2]
                   : ((unsigned)v + (scrollX >> 3)) & mask;
            entry = fill->entry ? fill->tileEntry : Read16(rowBase + (column & 31) * 2 + (column >> 5) * 2048);
            address = chars + (entry & 1023) * (color256 ? 64 : 32);
            if (address >= 0x10000) continue;
            slot = GetTileSlot(address, entry >> 12, color256);
            if (slot >= 0)
                TileWalkSlot(batch, slot, v * 8 - ox + CTR_VIEW_X + sLayerShift,
                             w * 8 - oy + sSpanShiftY + CTR_VIEW_Y, entry & 1024, entry & 2048);
        }
    }
    TileWalkEnd(batch);
}

static void DrawLayerRect(unsigned bg, int x0, int x1, int y0, int y1,
                          float sx, float sy, float dx, float dy, bool fade, int top, int bottom);

/* The PokéNav keeps to its tiles: no layer textures for it. */
static bool CentredTexture(unsigned bg)
{
    return sCentredScreen != CTR_CENTRED_POKENAV && LayerDrawable(bg);
}

/*
 * Lines [top, bottom) of a centred screen's picture: from the layer's texture
 * when it has one (LayersPrepare), one quad where the tiles are 30 per line.
 * Walked tile by tile, the PC's four layers were ~3000 quads a frame and 30
 * fps on an Old 3DS; the bag's three, 1800 and a frame dropped every second.
 */
static void DrawCentredPicture(unsigned bg, int top, int bottom)
{
    if (!CentredTexture(bg) || sSpanShiftX || sSpanShiftY)
    {
        DrawCentredSpan(bg, 0, 240, top, bottom);
        return;
    }
    ViewBase();
    DrawLayerRect(bg, 0, 240, top, bottom, 0, (float)top, 1, 1, false, 0, 0);
}

/*
 * The margins of a layer that reads on through its tilemap (fill->wrap): the
 * texture repeats as the tilemap wraps, so they are the four bands around the
 * picture cut from it at their own place.
 */
static bool DrawWrapMarginsTex(unsigned bg, const CentredFill *fill)
{
    if (!fill->wrap || fill->across || !CentredTexture(bg)) return false;
    ViewBase();
    DrawLayerRect(bg, VIEW_LEFT, VIEW_RIGHT, VIEW_TOP, 0, VIEW_LEFT, VIEW_TOP, 1, 1, false, 0, 0);
    DrawLayerRect(bg, VIEW_LEFT, VIEW_RIGHT, 160, VIEW_BOTTOM, VIEW_LEFT, 160, 1, 1, false, 0, 0);
    DrawLayerRect(bg, VIEW_LEFT, 0, 0, 160, VIEW_LEFT, 0, 1, 1, false, 0, 0);
    DrawLayerRect(bg, 240, VIEW_RIGHT, 0, 160, 240, 0, 1, 1, false, 0, 0);
    return true;
}

/* True when the centred screen draws this layer itself. */
static bool DrawCentredBg(unsigned bg)
{
    const CentredFill *fill = CentredFillOf(sCentredScreen);

    bool speech = fill->bandRow && ((Reg(8) >> 2) & 3) == fill->bandChars;

    if (CentredLayers(fill) & (1u << bg))
    {
        int sky = speech ? fill->skyRows : 0;

        if (sky)
        {
            sSpanShiftY = (int)(Reg(0x12 + bg * 4) & 511) - CTR_VIEW_Y;
            DrawCentredSpan(bg, 0, 240, 0, sky * 8);
            DrawCentredMargins(bg, fill, 0, sky);
            sSpanShiftY = 0;
        }
        DrawCentredPicture(bg, sky * 8, 160);
        if (sky || !DrawWrapMarginsTex(bg, fill))
            DrawCentredMargins(bg, fill, sky ? sky : -64, 64);
        return true;
    }
    if (bg == 0 && speech)
    {
        DrawCentredSpan(0, 0, 240, 0, fill->bandRow * 8);
        sSpanShiftX = fill->bandX;
        sSpanShiftY = fill->bandY;
        DrawCentredSpan(0, 0, 240, fill->bandRow * 8, 160);
        sSpanShiftX = sSpanShiftY = 0;
        return true;
    }
    return false;
}

static void DrawTextBg(unsigned bg)
{
    unsigned control = Reg(8 + bg * 2), size = control >> 14;
    /*
     * A background covers as many pixels as its tilemap spans: 256 for the
     * 32-column size the fixed screens use, 512 for the 64-column one the port
     * widened for the field camera. Inside that band the GBA wrap-around
     * applies exactly, so scrolling behaves as on hardware; past it there is
     * nothing to show on a 400px screen, and the line stays backdrop rather
     * than repeating the same image.
     */
    int right = (int)Min((size & 1) ? 512 : 256, CTR_GAME_WIDTH);
    int bottom = (int)Min((size & 2) ? 512 : 256, CTR_GAME_HEIGHT);
    int left = 0, top = 0;

    if (sStage)
    {
        DrawStageBg(bg);
        return;
    }
    if (sBattle)
    {
        if (bg == 0) DrawBattleText(bg);
        else DrawBattleBgSplit(bg);
        return;
    }
    if (sCentred)
    {
        if (DrawCentredBg(bg)) return;
        if (CentredTexture(bg))
        {
            DrawCentredPicture(bg, 0, 160);
            return;
        }
        if (right > 240) right = 240;
        if (bottom > 160) bottom = 160;
    }
    if (bg == 0 && sFieldBanner) right = CTR_GAME_WIDTH;
    if (right > sClipX1) right = sClipX1;
    if (bottom > sClipY1) bottom = sClipY1;
    if (left < sClipX0) left = sClipX0;
    if (top < sClipY0) top = sClipY0;
    if (right < left) right = left;
    if (bottom < top) bottom = top;
    DrawTextSpan(bg, left, right, top, bottom);
}
