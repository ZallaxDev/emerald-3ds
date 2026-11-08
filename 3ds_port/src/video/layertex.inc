/*
 * Part of 3ds_video.c. Layer textures, a background composed once into a
 * texture of its own (sLayers): their memory, release and signatures, and the
 * quads of their cells (FastCells).
 */

/*
 * Layer textures. A text background is composed once, unscrolled, into a
 * texture of its own that repeats exactly like its tilemap wraps, and kept
 * there, only the cells whose tile, entry or palette changed drawn again
 * (LayerRenderCells). Drawing it is then a handful of quads cut from that texture
 * instead of one quad per 8x8 tile:
 *
 * - a background that scrolls per line (the waves of the intro and the title)
 *   is one quad per run of lines sharing a scroll;
 * - a stage background (the intro, the title) is its picture plus the bands
 *   that fill the screen around it, about 35 quads for a whole 400x240 layer.
 *
 * On an Old 3DS a tile quad costs ~4.6 us of CPU, and the intro's leaves are
 * four 400x240 layers: 4500 quads and 22 ms a frame walked tile by tile. The
 * leaves never change while they are on screen, so the tilemap walk now
 * happens once for the scene.
 *
 * 16-bit colour is enough: the GBA has 15 bits and a transparent one.
 */
#define LAYER_IDLE_FRAMES 180
#define LAYER_RETRY_FRAMES 300u
/* In a battle, a layer waits for an atlas the battle's view leaves out: one
 * is there a frame or two after it starts. */
#define LAYER_BATTLE_RETRY_FRAMES 30u
typedef struct
{
    C3D_Tex tex;
    C3D_RenderTarget *target;
    uint32_t usedFrame;
    /* The token and tile cache generation of the last walk of its cells. */
    uint32_t walked, walkedGeneration;
    /* What a battle's plans have asked of it, in the picture's coordinates
     * (MapPlanFresh), none while x0 >= x1: the window of LayerWindow. */
    int16_t readX0, readY0, readX1, readY1;
    bool valid;
} LayerTexture;
static LayerTexture sLayers[4];
/* Whether each background is drawn from its texture this frame. */
static bool sLayerReady[4];
static uint32_t sLayerFail[4];
/* The backgrounds whose cells were looked at this frame (LayersRender): a
 * texture of a frame that did not is what it was. */
static unsigned sLayerFresh;
/*
 * The 2D battle's large backgrounds, from their map textures (DrawBattleBgTex).
 * A move's layer that covers the screen and moves - Surf's wave, the sand,
 * Haze - has a new scroll every frame, so nothing kept of it holds, and its
 * tiles were walked at every pass that shows it: the scenery, the edges, a
 * slot's patch, each band of each blend pass. Some 600 quads a walk, and most
 * of an Old 3DS's frame in 3D. Its texture is kept cell by cell like the
 * field's (LayerRenderCells); scroll, blend and bands then only move a few
 * quads.
 *
 * The terrain is one of them, though it stands still. It is split in bands of
 * lines (BattleTerrainLine), below the scene four lines at a time, and the walk
 * of a band is a row of tiles: the edges alone walked it for some 470 quads,
 * 2.5 ms of an Old 3DS's frame, on every frame the scenery moves (a blending
 * layer stepping is enough: the edges have it in them, BattleEdgesKeep). From
 * its texture a band is a quad or two (DrawBattleTerrainTex). A move's
 * background on BG3 stays walked: only the terrain asks.
 *
 * sBattleLayerWanted is apart from LineBackgrounds(), which says something
 * else to its readers (MonoSceneBlank, MonoCutUnsure). What asks is the last
 * frame's largest walk of a text layer, BG1, BG2 or the terrain, no box of the
 * battle's, not blank and not a move's picture (BattleLayersAsked): a battler
 * copied into a background (monbg) stays below BATTLE_LAYER_QUADS, and moves no
 * VRAM with every move. A texture in use stays wanted while its layer is large
 * and was drawn from it, or shown by a composition kept from it
 * (BattleLayersKept), in the last BATTLE_LAYER_HOLD_FRAMES: a layer drawn only
 * when it steps, or not at all while it stands still, does not walk in between.
 *
 * The surface (BattleLayerMake): a block of free VRAM with room left, or else
 * a gap of the building pages' arena, as the mono keeper's (sLayerLent[bg]: its
 * memory goes back there, never to C3D_TexDelete). Without the atlases' way out
 * of LayersPrepare. In a voxel battle the gap alone: it holds no VRAM there, and
 * the gap is memory the world holds anyway, lent until the world wants it. The
 * terrain goes on VRAM alone, and never in a voxel battle, where BG3 is the
 * world's: the arena lends one gap at a time, and held by a terrain from the
 * battle's start it would leave a move's wave and the mono keeper without. A
 * wave short of a surface takes the terrain's VRAM as well (BattleLayerMake).
 * Whatever the frame asks of VRAM goes first (BattleLayersGiveWay). Not made, it
 * waits on its own stamp (sBattleLayerFail): sLayerFail is the line and field
 * layers', and a large layer short of a surface says nothing of what they find.
 */
#define BATTLE_LAYER_QUADS 128u
#define BATTLE_LAYER_HOLD_FRAMES 30
/* Left in one block by a layer placed in VRAM: the OBJ window's surface. */
#define BATTLE_LAYER_VRAM_LEFT (64u * 1024u)
enum
{
    BATTLE_LAYER_NO_SURFACE,
    BATTLE_LAYER_GAP_SMALL,
    BATTLE_LAYER_PICTURE,
    BATTLE_LAYER_NOT_READY,
    BATTLE_LAYER_WHYS
};
static unsigned sBattleLayerWanted;
/* The layers made for this or taken on from the line path, and the gaps. */
static unsigned sBattleLayerHeld;
static bool sLayerLent[4];
/* The last frame's: largest walk, layers shown as a picture, drawn from a
 * texture; and why a surface was not made. */
static unsigned sBattleWalk[4], sBattleLayerPicture, sBattleLayerDrawn, sBattleLayerKept;
static uint8_t sBattleLayerDenied[4];
static uint32_t sBattleLayerUsed[4], sBattleLayerFail[4];
/* By frame and layer, for the log: and the gaps given back, the surfaces asked
 * for, and how the pages' arena stood when one last was (in bytes). */
static struct
{
    unsigned drawn, walked, why[BATTLE_LAYER_WHYS], lent, kept, back, asks, terrain;
    uint32_t pagesUsed, pagesGap;
} sBattleLayerCounts;
/*
 * The layers that found no VRAM while the voxel world held it on its way out
 * (the option off, the world still up): the anticipated request. The world
 * is parked as soon as it is not drawn any more, and these are made in that
 * same preparation, instead of after LAYER_RETRY_FRAMES (VoxelVramManage).
 */
static unsigned sLayersWaitWorld;
static bool sLayerFailLogged;
/*
 * A stage whose layer textures found no VRAM because the depth planes hold it.
 * Its layers drawn tile by tile cost 30 ms a frame on an Old 3DS, so the
 * planes give way to them: first the nearest (sPlaneShrinkAsked), which is
 * what the intro needs, and only with two left all of them, the stage then
 * composed per eye - twice the work, 30 fps - until it is over.
 */
static bool sStageWithoutPlanes;
static bool sPlaneShrinkAsked;
/* The overworld's ask for VRAM (CtrVideo_RequestPlaneRelease), for the frame
 * being prepared: the battle's surfaces, the layer textures and the OBJ
 * window of a screen that is gone give theirs back too (LayersPrepare,
 * ScenePrepare, SlotsPrepare, ObjWindowPrepare). Held after a battle in 3D, or back from the Fly map or
 * a menu, they kept the field's atlas out, and the field was drawn in 2D
 * until they went idle (LAYER_IDLE_FRAMES, three seconds). */
static bool sVramAsked;
static bool sBandsReady;
void CtrVideo_RequestPlaneRelease(void);
static bool ReleaseIdleSurfaces(void);
static bool SlotsYield(void);
static bool SceneYield(void);

static unsigned LineValue(unsigned reg, int y, unsigned base)
{
    if (!(sLineMask & (1u << reg))) return base;
    /* A stage's picture has 160 lines; the lines above and below it carry
     * the wave on with the same period rather than stopping it flat. */
    if (sStage) y = ((y % 160) + 160) % 160;
    /* Around the battle scene, the lines next to it: the intro's two halves
     * slide in from the edges of the screen. */
    if (sBattle) y = y < 0 ? 0 : y > 159 ? 159 : y;
    if (y >= 0 && (unsigned)y < sLineCount)
        return sLineScroll[reg][y] & 511;
    return base;
}

/* The shown text backgrounds of the current display mode. */
static unsigned TextBackgrounds(void)
{
    unsigned display = Reg(0), mode = display & 7, found = 0;

    if (mode > 1) return 0;
    for (unsigned bg = 0; bg < 4; ++bg)
    {
        if (!(display & (0x100u << bg))) continue;
        if (mode == 1 && bg >= 2) continue;
        found |= 1u << bg;
    }
    return found;
}

/* Which text backgrounds this frame scrolls per line. */
static unsigned LineBackgrounds(void)
{
    unsigned found = 0, text = TextBackgrounds();

    if (sCentredScreen == CTR_CENTRED_POKEDEX_CAUGHT) return 0;
    for (unsigned bg = 0; bg < 4; ++bg)
        if ((text & (1u << bg)) && (sLineMask & (3u << (bg * 2)))) found |= 1u << bg;
    return found;
}

static void LayerRelease(unsigned bg)
{
    LayerTexture *layer = &sLayers[bg];

    /* The target first: deleting it waits for the GPU, which may still read
     * the texture. A gap of the arena goes back there, not to VRAM. */
    if (layer->target) { ClearWhenMadeForget(layer->target); C3D_RenderTargetDelete(layer->target); }
    if (sLayerLent[bg])
    {
        CtrVoxel_ReturnPagesGap(layer->tex.data);
        ++sBattleLayerCounts.back;
    }
    else if (layer->tex.data) C3D_TexDelete(&layer->tex);
    memset(layer, 0, sizeof(*layer));
    sLayerLent[bg] = false;
    sLayerReady[bg] = false;
    sBattleLayerHeld &= ~(1u << bg);
}

static void LayersRelease(void)
{
    for (unsigned bg = 0; bg < 4; ++bg) LayerRelease(bg);
}

/* The backgrounds the last LayersPrepare drew from a texture. */
static unsigned sLayersWanted;

/* The layer textures of a screen that is not up any more (the bag's, the
 * Fly map's), before they go idle: outside the frame, as LayerRelease. */
static bool ReleaseIdleLayers(void)
{
    bool any = false;

    for (unsigned bg = 0; bg < 4; ++bg)
        if (!(sLayersWanted & (1u << bg)) && sLayers[bg].tex.data)
        {
            LayerRelease(bg);
            any = true;
        }
    return any;
}

/*
 * Outside the frame, where creating or deleting a target cannot stall one in
 * flight: a texture for every background drawn from one this frame, as large
 * as its tilemap. One that cannot be placed falls back to the tile walk and
 * is retried after a while, logged once.
 */
static bool LineWindows(void);

/* A texture and render target for background bg at its tilemap's size. */
static bool LayerCreate(unsigned bg, unsigned width, unsigned height)
{
    LayerTexture *layer = &sLayers[bg];

    if (C3D_TexInitVRAM(&layer->tex, width, height, GPU_RGBA5551)
        && (layer->target = C3D_RenderTargetCreateFromTex(&layer->tex, GPU_TEXFACE_2D, 0, -1)))
    {
        /* Not cleared when made (ClearWhenMade): a new layer is not valid,
         * and its first draw covers every cell, the empty ones too. */
        return true;
    }
    LayerRelease(bg);
    return false;
}

/* The 2D battle's large layers, below the slots they depend on. */
static void BattleLayersCount(void);
static unsigned BattleLayersAsked(void);
static void BattleLayersGiveWay(unsigned lines);
static bool BattleLayerMake(unsigned bg, unsigned width, unsigned height);
static bool BattleLayersYield(unsigned lines);

static void LayersPrepare(void)
{
    /* A centred screen too (DrawCentredPicture), the PokéNav only when its
     * line windows need them. */
    bool centred = sCentred && sCentredScreen != CTR_CENTRED_POKENAV;
    unsigned lines = LineBackgrounds()
                   | (sStage || sFieldLayers || centred || LineWindows() ? TextBackgrounds() : 0);
    unsigned want;

    BattleLayersCount();
    sBattleLayerWanted = BattleLayersAsked() & ~lines;
    memset(sBattleWalk, 0, sizeof(sBattleWalk));
    sBattleLayerPicture = sBattleLayerDrawn = sBattleLayerKept = sLayerFresh = 0;
    BattleLayersGiveWay(lines);
    want = lines | sBattleLayerWanted;
    sLayersWanted = want;

    /* The wanted ones of the wrong size go first, so what they held is
     * free for any of this frame's new ones. */
    for (unsigned bg = 0; bg < 4; ++bg)
    {
        unsigned size = Reg(8 + bg * 2) >> 14;

        if ((want & (1u << bg)) && sLayers[bg].tex.data
            && (sLayers[bg].tex.width != ((size & 1) ? 512 : 256)
                || sLayers[bg].tex.height != ((size & 2) ? 512 : 256)))
            LayerRelease(bg);
    }
    for (unsigned bg = 0; bg < 4; ++bg)
    {
        LayerTexture *layer = &sLayers[bg];
        unsigned size = Reg(8 + bg * 2) >> 14;
        unsigned width = (size & 1) ? 512 : 256, height = (size & 2) ? 512 : 256;

        sLayerReady[bg] = false;
        if (!(want & (1u << bg)))
        {
            if (layer->tex.data && (sVramAsked || sStats.frames - layer->usedFrame > LAYER_IDLE_FRAMES))
                LayerRelease(bg);
            continue;
        }
        if (!layer->tex.data)
        {
            bool large = (sBattleLayerWanted & (1u << bg)) != 0;
            uint32_t failed = large ? sBattleLayerFail[bg] : sLayerFail[bg];

            if (failed && (int32_t)(sStats.frames - failed)
                          < (int32_t)(sBattle ? LAYER_BATTLE_RETRY_FRAMES : LAYER_RETRY_FRAMES)) continue;
            /* A large layer of the battle: walked meanwhile, as it was, and
             * no atlas or surface gives way for it. */
            if (large)
            {
                if (!BattleLayerMake(bg, width, height)) continue;
            }
            else if (!LayerCreate(bg, width, height))
            {
                /*
                 * VRAM is taken and given back by every screen (the bottom
                 * surface, the voxel atlases, the other layers), so a block
                 * this size may be missing with enough free in pieces. The
                 * layers kept for a screen that is not up, and the voxel
                 * world's idle memory, go before this one gives up.
                 */
                unsigned freed = 0;

                for (unsigned other = 0; other < 4; ++other)
                    if (!(want & (1u << other)) && sLayers[other].tex.data)
                    {
                        LayerRelease(other);
                        ++freed;
                    }
                /* The battle's scenery kept without the 3D slider is only a
                 * speed-up: it goes before any atlas, and a move's line
                 * layer (Dig, Dragon Dance) never waits for it. */
                if (SlotsYield()) ++freed;
                /* So does a large layer's: it can be walked. */
                if (BattleLayersYield(lines)) ++freed;
                /* For a battle's layer, not the atlases on screen: walked
                 * tile by tile meanwhile, it waits for the block an atlas the
                 * battle's view leaves out makes. Taken at the battle's start,
                 * they went from its scenery, one no longer fitted back and
                 * the battle showed its classic background (port.log
                 * 2026-10-22). */
                if (!sStage) freed += CtrVoxel_ReleaseIdleVramFor(width * height * 2u, !sBattle) > 0;
                /* Outside a stage, the 3D planes of a screen that is gone
                 * and an idle bottom surface too: back from a hidden menu
                 * they held VRAM for 180 frames, and the field walked its
                 * layers tile by tile (5 ms a frame) meanwhile. */
                /* On a stage, the battle's scene surface kept from start-up
                 * goes before its planes (SceneYield). */
                if ((!freed || !LayerCreate(bg, width, height))
                    && !(!sStage && ReleaseIdleSurfaces() && LayerCreate(bg, width, height))
                    && !(sStage && SceneYield() && LayerCreate(bg, width, height)))
                {
                    if (sStage && sBandsReady)
                    {
                        /* Tile by tile for this one frame; a plane goes, or with
                         * only two left, all of them (sPlaneShrinkAsked). */
                        if (sBandCount > 2)
                            sPlaneShrinkAsked = true;
                        else
                        {
                            sStageWithoutPlanes = true;
                            CtrVideo_RequestPlaneRelease();
                        }
                        continue;
                    }
                    if (!sLayerFailLogged)
                        CtrLog_Write(CTR_LOG_ERROR, "VIDEO: no VRAM for a %ux%u layer texture (free=%lu); "
                                     "tile walk", width, height, (unsigned long)vramSpaceFree());
                    sLayerFailLogged = true;
                    /* Only this layer waits; the others still try. With the
                     * voxel world switched off and still holding its VRAM,
                     * it waits for the world to let go rather than for the
                     * retry: the 2D field is where the game goes next. */
                    sLayerFail[bg] = sStats.frames | 1;
                    if (CtrVoxel_HoldsVram() && !CtrSettings_Voxel())
                        sLayersWaitWorld |= 1u << bg;
                    continue;
                }
            }
            sLayerFail[bg] = sBattleLayerFail[bg] = 0;
            C3D_TexSetFilter(&layer->tex, GPU_NEAREST, GPU_NEAREST);
            C3D_TexSetWrap(&layer->tex, GPU_REPEAT, GPU_REPEAT);
        }
        /*
         * What the line path left: the battle's intro slides the terrain by
         * lines, and the terrain then stays on that texture to the battle's
         * end, where it used to go idle. Taken on as a large layer's, it gives
         * way like one (BattleLayersGiveWay) and leaves what one leaves.
         */
        if ((sBattleLayerWanted & (1u << bg)) && !(sBattleLayerHeld & (1u << bg)))
        {
            if (CtrGpuMem_LargestVram() < BATTLE_LAYER_VRAM_LEFT)
            {
                LayerRelease(bg);
                sBattleLayerDenied[bg] = BATTLE_LAYER_NO_SURFACE;
                sBattleLayerFail[bg] = sStats.frames | 1u;
                continue;
            }
            sBattleLayerHeld |= 1u << bg;
        }
        layer->usedFrame = sStats.frames;
        sLayerReady[bg] = true;
    }
    /* A background wanted from its texture and walked tile by tile instead. */
    for (unsigned bg = 0; bg < 4; ++bg)
        if ((want & (1u << bg)) && !sLayerReady[bg])
            SurfaceDenied(CTR_DENIED_LAYER);
}

/*
 * Every layer texture - the field's, the stages', the line windows' - is kept
 * cell by cell.
 *
 * Walked tile by tile, the 2D field is three 400x240 layers, about 4700 quads
 * a frame: 20 ms of CPU on an Old 3DS, so it never made 60 fps. Composed into
 * textures it is a quad per layer, but the whole-layer hash would recompose
 * all three every time a tile animates (water, flowers), since they share
 * their tiles. So each cell keeps what was drawn in it - the upload serial of
 * its tile (which covers the tile's bytes and palette) and its flips - and
 * only the cells whose tile, tilemap entry or palette changed are cleared and
 * drawn again: a row or a column as the camera moves, the animated tiles when
 * they animate. A frame where most cells changed (a new map, a palette fade)
 * is recomposed whole.
 *
 * The stages had a hash of the whole layer instead, and the title screen
 * cycles the colour of Rayquaza's markings every fourth frame: the whole
 * 1024-tile layer was drawn again each time, a dropped frame on an Old 3DS
 * for a dozen tiles that changed.
 */
#define LAYER_CELLS 4096
static uint32_t sCellSig[4][LAYER_CELLS];
/* How many of those cells draw something: none, and the layer's quad covers
 * the screen with transparent texels (the field's text layer, most of the
 * time), so it is not drawn at all. */
static unsigned sCellsShown[4];
static unsigned sCellControl[4];
/*
 * The cells drawn from a tile bank (TilemapBankAt), and how many: a large layer
 * with one is walked. Only a field map's banks left over where a battle's map
 * names the same entry: a battle's maps come by LZDecompressVram and such, not
 * bg.c's copies, so the table outlives the field (CtrVideo_TilemapRequested).
 * Kept in a battle alone (a field map walks thousands of cells a frame), so a
 * texture that goes in or out of one starts again (sCellsWatched: 0 out,
 * CELLS_WATCHED in, CELLS_PLAIN as below).
 *
 * A large layer of 64 columns reads no bank at all, and is what its walk is:
 * that walk is DrawBattleBg's, which reads none. Not a layer of 32 columns,
 * which DrawTextSpanAt can cut and which does read them, nor a line layer, drawn
 * from its texture alone: those stay as they were, a large one with a bank in
 * it walked. sCellsPlainTop: one past the highest tile such a layer's map names,
 * where its characters end. The terrain's character base reaches every tilemap
 * of the battle, and each text box would walk its 2048 cells again.
 */
#define CELLS_WATCHED 1
#define CELLS_PLAIN 2
static uint32_t sCellBanked[4][LAYER_CELLS / 32];
static unsigned sCellsBanked[4], sCellsPlainTop[4];
static uint8_t sCellsWatched[4];
/*
 * What a change of colours reaches, in a battle (the pass of LayerRenderCells).
 *
 * Surf and Muddy Water turn seven colours of their wave every four frames. The
 * wave is a 512x256 layer: 2048 cells, some 1800 of them drawn, and 195 use a
 * colour that turns. Each turn walked all of them, 2.7 ms of an Old 3DS's CPU
 * before the GPU could start: in a voxel battle without 3D, a late picture every
 * four frames (107 repeated of 544 on the console). So a turn looks only at the
 * cells that can have changed.
 *
 * sCellColours: the colours of a cell's tile (sTiles' colors, 4bpp), from its
 * last walk, 0 for a cell that draws nothing; sCellRowColours: of a row's cells,
 * those and maybe more, so most rows of the map are not read at all. Nothing but
 * the tile's bytes says which colours it uses, and the pass runs only with the
 * bytes, the map and the banks as they were at the last walk.
 *
 * sCellStale: of those, the cells outside what the frame shows, left as they
 * were. A layer refreshes first what the frame shows (LayerWindow), and about a
 * third of the wave's turning cells are off the picture. A stale cell
 * is drawn when it comes into view, with the colours of that frame: its signature
 * still names the tile as it was drawn, so if that is what the tile is again
 * nothing is drawn. A walk of all the rows ends it. layer->walked then says "all
 * cells but the stale ones", which makes the bits part of the key: nothing reads
 * a stale cell (MapPlanFresh walks the tiles instead, and counts it), and any
 * frame that has no window - a line layer, the terrain, a layer that stopped being
 * large - looks at every stale cell first. They are not caught up over later
 * frames: the wave would have turned again by then, and each would pay for a
 * decode nobody sees. Their count stays small: at most what turns.
 */
static uint16_t sCellColours[4][LAYER_CELLS], sCellRowColours[4][64];
static uint32_t sCellStale[4][LAYER_CELLS / 32];
static unsigned sCellsStale[4];
/* By 600 frames (battle only): walks and passes, cells looked at and cells that
 * changed in each; stale cells left and drawn on entering the view; plans that
 * met a stale cell (never expected: then the window was short). */
static struct
{
    uint32_t walks, walkSeen, walkChanged, passes, passSeen, passChanged, left, back, missed;
} sLayerPasses;

static const uint32_t *LayerCellSigs(unsigned bg)
{
    return sCellSig[bg];
}

static bool LayerDrawable(unsigned bg);

/*
 * Layer cells without Citro2D's per-quad work.
 *
 * Drawn through C2D_DrawImageAt a cell cost ~4.6 us of CPU on an Old 3DS: a
 * scene that opens on four 256x512 layers drew 8192 of them, 37 ms, and every
 * new screen paid it for each of its layers. A cell is always one whole atlas
 * tile (or nothing) over one 8x8 square of the texture, so it needs no tint,
 * no blending and no transform: four vertices of two shorts and two floats,
 * written straight into a buffer, and one indexed draw per layer. The tile
 * replaces what the square held, transparent texels included, so a changed
 * cell needs no clearing first; an empty one is drawn in the constant 0.
 *
 * It runs on Citro2D's own program, set up for the layer by an empty Citro2D
 * draw, and only borrows the vertex layout, the buffer and the first texture
 * stage, which it puts back: rebinding Citro2D (C2D_Prepare) mid-frame left
 * its tinted draws unfaded for the rest of the frame.
 *
 * The buffers are refilled from the start each frame: the GPU has finished
 * the last frame's draws once C3D_FrameBegin returns, and C3D_FrameEnd flushes
 * the linear heap before the GPU reads any of it. A frame that would need more
 * than FAST_QUADS cells keeps the Citro2D path.
 */
#define FAST_QUADS 8192

static void FastInit(void)
{
    sFastVertices = linearAlloc(FAST_QUADS * 4 * sizeof(FastVertex));
    sFastIndices = linearAlloc(FAST_QUADS * 6 * sizeof(uint16_t));
    if (sFastVertices == NULL || sFastIndices == NULL)
    {
        linearFree(sFastVertices);
        linearFree(sFastIndices);
        sFastVertices = NULL;
        sFastIndices = NULL;
        CtrLog_Write(CTR_LOG_ERROR, "VIDEO: no linear memory for layer cells; through Citro2D");
        return;
    }
    for (unsigned q = 0; q < FAST_QUADS; ++q)
    {
        uint16_t *index = sFastIndices + q * 6, base = (uint16_t)(q * 4);

        index[0] = base;
        index[1] = base + 1;
        index[2] = base + 2;
        index[3] = base + 2;
        index[4] = base + 1;
        index[5] = base + 3;
    }
    sFastReady = true;
}

/* One cell's square: corners top-left, top-right, bottom-left, bottom-right,
 * with texcoords as Citro2D gives an atlas slot (DrawSlotTinted). */
static void FastQuad(FastVertex *v, int x, int y, int slot, bool flipX, bool flipY)
{
    unsigned tileX = ((unsigned)slot % (ATLAS_SIZE / 8)) * 8, tileY = ((unsigned)slot / (ATLAS_SIZE / 8)) * 8;
    float s0 = tileX / (float)ATLAS_SIZE, s1 = (tileX + 8) / (float)ATLAS_SIZE;
    float t0 = 1.0f - tileY / (float)ATLAS_SIZE, t1 = 1.0f - (tileY + 8) / (float)ATLAS_SIZE;

    if (flipX) { float swap = s0; s0 = s1; s1 = swap; }
    if (flipY) { float swap = t0; t0 = t1; t1 = swap; }
    v[0] = (FastVertex){(int16_t)x, (int16_t)y, s0, t0};
    v[1] = (FastVertex){(int16_t)(x + 8), (int16_t)y, s1, t0};
    v[2] = (FastVertex){(int16_t)x, (int16_t)(y + 8), s0, t1};
    v[3] = (FastVertex){(int16_t)(x + 8), (int16_t)(y + 8), s1, t1};
}

static void FastDraw(unsigned first, unsigned count)
{
    if (count)
        C3D_DrawElements(GPU_TRIANGLES, (int)count * 6, C3D_UNSIGNED_SHORT, sFastIndices + first * 6);
}

static bool FastCells(unsigned bg, const uint16_t *cells, unsigned count, const int16_t *cellSlot,
                      const uint16_t *cellEntry, unsigned columns)
{
    LayerTexture *layer = &sLayers[bg];
    unsigned tiles = 0, first = sFastUsed;
    C3D_AttrInfo savedAttr, *attr;
    C3D_BufInfo savedBuf, *buf;
    C3D_TexEnv savedEnv, *env;
    Tex3DS_SubTexture none = {0, 0, 0.0f, 1.0f, 0.0f, 1.0f};

    if (!sFastReady || count > FAST_QUADS - sFastUsed)
        return false;
    /* Tiles first, then the empty cells after them. */
    for (unsigned i = 0; i < count; ++i)
        if (cellSlot[cells[i]] >= 0) ++tiles;
    for (unsigned i = 0, t = 0, e = tiles; i < count; ++i)
    {
        unsigned cell = cells[i], entry = cellEntry[cell];
        /* columns is 32 or 64: no library division per cell (no divider on the ARM11) */
        int x = (int)(cell & (columns - 1)) * 8, y = (int)(cell >> __builtin_ctz(columns)) * 8;

        if (cellSlot[cell] >= 0)
            FastQuad(sFastVertices + (first + t++) * 4, x, y, cellSlot[cell], entry & 1024, entry & 2048);
        else
            FastQuad(sFastVertices + (first + e++) * 4, x, y, 0, false, false);
    }
    sFastUsed += count;

    /* Citro2D sets up the target, its projection and the atlas, with a quad
     * of no size, and sends it: its state is then the GPU's. */
    sInLayerTexture = true;
    BlendForget();
    SceneOn(layer->target);
    C2D_ViewReset();
    Blend(bg, false, false);
    C2D_DrawImageAt((C2D_Image){&sAtlas, &none}, 0, 0, 0, &sTint, 1, 1);
    C2D_Flush();

    attr = C3D_GetAttrInfo();
    savedAttr = *attr;
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_SHORT, 2);
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);
    AttrInfo_AddFixed(attr, 2);
    AttrInfo_AddFixed(attr, 3);
    C3D_FixedAttribSet(2, 0.0f, 0.0f, 0.0f, 0.0f);
    C3D_FixedAttribSet(3, 255.0f, 255.0f, 255.0f, 255.0f);
    buf = C3D_GetBufInfo();
    savedBuf = *buf;
    BufInfo_Init(buf);
    BufInfo_Add(buf, sFastVertices, sizeof(FastVertex), 2, 0x10);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);

    env = C3D_GetTexEnv(0);
    savedEnv = *env;
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    FastDraw(first, tiles);
    if (count > tiles)
    {
        env = C3D_GetTexEnv(0);
        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, 0, 0);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        C3D_TexEnvColor(env, 0);
        FastDraw(first + tiles, count - tiles);
    }

    /* Citro2D's layout, buffer and stage back, as it left them. */
    *C3D_GetTexEnv(0) = savedEnv;
    C3D_SetAttrInfo(&savedAttr);
    C3D_SetBufInfo(&savedBuf);
    sInLayerTexture = false;
    BlendForget();
    sStats.cells += count;
    return true;
}
