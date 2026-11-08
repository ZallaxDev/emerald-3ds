/*
 * Part of 3ds_video.c. The Game Corner's backdrops (3ds_corner_art.h): their
 * textures and CornerUnderlay, the felt and the wall behind roulette and
 * slots.
 */

/*
 * The Game Corner's backdrops (3ds_corner_art.h): two small textures filled
 * once when the roulette or the slot machine opens, drawn repeated as the
 * backdrop - a dozen quads, faded with it (sTint, set by Blend(5)). Freed a
 * while after the screen closes, outside the frame (CornerArtRelease).
 */
static C3D_Tex sCornerTex[2];
static unsigned sCornerScreen;
static uint16_t sCornerTable;
static uint32_t sCornerUsed;
static bool sCornerFailed;

static void CornerArtRelease(void)
{
    for (unsigned i = 0; i < 2; ++i)
        if (sCornerTex[i].data) C3D_TexDelete(&sCornerTex[i]);
    memset(sCornerTex, 0, sizeof(sCornerTex));
    sCornerScreen = CTR_CENTRED_NONE;
}

/* The textures for the screen on show; false without memory for them. */
static bool CornerArtReady(unsigned screen)
{
    /* Green at either table, as the tables stand in the Game Corner: the
     * green the game gives its other table (roulette.c, bgColors[1]). */
    uint16_t table = screen == CTR_CENTRED_ROULETTE ? 10 | 19 << 5 | 6 << 10 : 0;

    sCornerUsed = sStats.frames;
    if (!sCornerTex[0].data)
    {
        if (sCornerFailed) return false;
        if (!C3D_TexInit(&sCornerTex[0], CTR_CORNER_PATTERN_DIM, CTR_CORNER_PATTERN_DIM, GPU_RGB565)
            || !C3D_TexInit(&sCornerTex[1], CTR_CORNER_TRIM_DIM, CTR_CORNER_TRIM_DIM, GPU_RGB565))
        {
            CornerArtRelease();
            sCornerFailed = true;
            CtrLog_Write(CTR_LOG_ERROR, "VIDEO: no linear memory for the Game Corner's backdrop "
                         "(linear free=%lu); plain backdrop", (unsigned long)linearSpaceFree());
            return false;
        }
        for (unsigned i = 0; i < 2; ++i)
        {
            C3D_TexSetFilter(&sCornerTex[i], GPU_NEAREST, GPU_NEAREST);
            C3D_TexSetWrap(&sCornerTex[i], GPU_REPEAT, GPU_REPEAT);
        }
        sCornerScreen = CTR_CENTRED_NONE;
    }
    if (sCornerScreen == screen && sCornerTable == table) return true;
    if (screen == CTR_CENTRED_ROULETTE)
        CtrCornerArt_Felt(sCornerTex[0].data, sCornerTex[1].data, table);
    else
        CtrCornerArt_Wall(sCornerTex[0].data, sCornerTex[1].data);
    TexFilled(&sCornerTex[0]);
    TexFilled(&sCornerTex[1]);
    sCornerScreen = screen;
    sCornerTable = table;
    return true;
}

/* Screen [x0, x1) x [y0, y1) from texture t, texel (s, u) at its top left,
 * repeated. */
static void CornerQuad(unsigned t, int x0, int y0, int x1, int y1, int s, int u)
{
    const C3D_Tex *tex = &sCornerTex[t];
    const float w = tex->width, h = tex->height;
    const Tex3DS_SubTexture cut = {(u16)(x1 - x0), (u16)(y1 - y0), s / w, 1.0f - u / h,
                                   (s + x1 - x0) / w, 1.0f - (u + y1 - y0) / h};

    if (x0 >= x1 || y0 >= y1) return;
    ++sStats.tiles;
    C2D_DrawImageAt((C2D_Image){(C3D_Tex *)tex, &cut}, x0, y0, 0, &sTint, 1, 1);
}

/* Screen [x0, x1) x [y0, y1) in the colour of texel (texel, texel) of the
 * trim texture. */
static void CornerSolid(int x0, int y0, int x1, int y1, unsigned texel)
{
    const float c = (texel + 0.5f) / CTR_CORNER_TRIM_DIM;
    const Tex3DS_SubTexture cut = {(u16)(x1 - x0), (u16)(y1 - y0), c, 1.0f - c, c, 1.0f - c};

    if (x0 >= x1 || y0 >= y1) return;
    ++sStats.tiles;
    C2D_DrawImageAt((C2D_Image){&sCornerTex[1], &cut}, x0, y0, 0, &sTint, 1, 1);
}

/* The four sides of a ring: rows y0 and y1 - 1 across, columns x0 and
 * x1 - 1 between them, d wide. */
static void CornerRing(int x0, int y0, int x1, int y1, int d, unsigned texel)
{
    CornerSolid(x0, y0, x1, y0 + d, texel);
    CornerSolid(x0, y1 - d, x1, y1, texel);
    CornerSolid(x0, y0 + d, x0 + d, y1 - d, texel);
    CornerSolid(x1 - d, y0 + d, x1, y1 - d, texel);
}

/*
 * The roulette's felt: green, with its Poké Balls, over the whole screen -
 * the picture's backdrop too - inside a dark band and a gold cord round the
 * screen's edge.
 */
static void DrawRouletteFelt(void)
{
    const int w = CTR_GAME_WIDTH, h = CTR_GAME_HEIGHT;

    CornerQuad(0, 8, 8, w - 8, h - 8, 8, 8);
    CornerRing(0, 0, w, h, 6, CTR_CORNER_FELT_BAND);
    /* The cord, dashed along (x + y) as row 0 and column 0 hold it. */
    CornerQuad(1, 6, 6, w - 6, 7, 12, 0);
    CornerQuad(1, 6, h - 7, w - 6, h - 6, 6 + h - 7, 0);
    CornerQuad(1, 6, 7, 7, h - 7, 0, 13);
    CornerQuad(1, w - 7, 7, w - 6, h - 7, 0, w - 7 + 7);
    CornerRing(7, 7, w - 7, h - 7, 1, CTR_CORNER_FELT_LINE);
}

/*
 * The slot machine in the casino's wall: the wall in the margins only (the
 * machine is the picture), and round the picture a gold frame, 13 pixels
 * wide, its red band lit with bulbs 16 apart - which is what makes the
 * corners' bulbs meet, and the wall's coins (8 + 16k across, 16k down) fall
 * clear of the frame's edges.
 */
static void DrawSlotsWall(void)
{
    const int w = CTR_GAME_WIDTH, h = CTR_GAME_HEIGHT, px = CTR_VIEW_X, py = CTR_VIEW_Y;
    const int x0 = px - 13, y0 = py - 13, x1 = px + 240 + 13, y1 = py + 160 + 13;
    const int half = CTR_CORNER_BULB_PERIOD / 2;

    CornerQuad(0, 0, 0, w, y0, 8, 0);
    CornerQuad(0, 0, y1, w, h, 8, y1);
    CornerQuad(0, 0, y0, x0, y1, 8, y0);
    CornerQuad(0, x1, y0, w, y1, x1 + 8, y0);
    CornerRing(x0, y0, x1, y1, 1, CTR_CORNER_FRAME_DARK);
    CornerRing(x0 + 1, y0 + 1, x1 - 1, y1 - 1, 1, CTR_CORNER_FRAME_GOLD);
    CornerRing(x0 + 8, y0 + 8, x1 - 8, y1 - 8, 1, CTR_CORNER_FRAME_GOLD);
    CornerRing(x0 + 9, y0 + 9, x1 - 9, y1 - 9, 1, CTR_CORNER_FRAME_DARK);
    CornerRing(x0 + 10, y0 + 10, x1 - 10, y1 - 10, 3, CTR_CORNER_FRAME_SHADOW);
    /* The band: lit bulbs first along the top and down the sides, dark
     * first along the bottom, so they alternate the whole way round. */
    CornerQuad(1, x0 + 2, y0 + 2, x1 - 2, y0 + 8, 0, 0);
    CornerQuad(1, x0 + 2, y1 - 8, x1 - 2, y1 - 2, half, 0);
    CornerQuad(1, x0 + 2, y0 + 8, x0 + 8, y1 - 8, 0, 6);
    CornerQuad(1, x1 - 8, y0 + 8, x1 - 2, y1 - 8, 0, 6);
}

/* Behind a Game Corner screen, instead of its backdrop or black margins. */
static void CornerUnderlay(void)
{
    if (!sCentred || (sCentredScreen != CTR_CENTRED_ROULETTE && sCentredScreen != CTR_CENTRED_SLOTS)
        || (Reg(0) & 128) || !CornerArtReady(sCentredScreen))
        return;
    ViewBase();
    if (sCentredScreen == CTR_CENTRED_ROULETTE) DrawRouletteFelt();
    else DrawSlotsWall();
}
