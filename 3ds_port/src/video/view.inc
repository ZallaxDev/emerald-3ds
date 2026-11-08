/*
 * Part of 3ds_video.c. The view (sViewX/Y, sZoom, the target's size), the
 * scene surface's handles, the stage, centred and battle flags, the per-line
 * registers, and the OAM's marks (anchors, weather, fog, screen, machine,
 * Fly) and extended palettes. Most later units read this state.
 */

/*
 * The game lays itself out over the whole 400x240 viewport, so screen and
 * logical coordinates are the same. A background only covers the part of the
 * screen its tilemap actually spans; see DrawTextBg.
 *
 * A GBA stage (CtrVideo_SetStage) is the exception: its 240x160 picture sits
 * 1:1 at (CTR_STAGE_X, CTR_STAGE_Y) and the rest of the screen is filled around
 * it. sViewX/Y is where the GBA origin lands on the screen; everything else
 * stays in GBA coordinates, so the space around the picture is negative or
 * past 240x160.
 *
 * A centred GBA screen (CtrVideo_SetCentred) sits at the same place, but its
 * margins are not filled from the picture: only the layers that wrap on the
 * GBA (an affine map with wrap-around) and sprites reach into them, and text
 * layers stop at the edge of the 240x160 screen - except the backgrounds a
 * screen names in sCentredFills, which carry on to the edges.
 *
 * The battle scene (CtrVideo_SetBattle) sits centred across and on the bottom
 * edge, so its text box is the bottom of the top screen. The text box is
 * stretched to the whole width, and the scene above and beside it comes from
 * its own layers as the GBA would wrap them (DrawBattleBg).
 */
static int sViewX, sViewY;
/*
 * Magnification of the frame being composed: a screen point is
 * (gba + view) * zoom + offset. The battle scene is composed at
 * CTR_BATTLE_ZOOM and its text box at 1 (DrawBattleTextLayer).
 */
static float sZoom = 1.0f, sOffX, sOffY;
/* Size of the surface being composed: the screen, or the battle scene's. */
static int sTargetW = CTR_GAME_WIDTH, sTargetH = CTR_GAME_HEIGHT;
/* Height of the texture behind it: scissor rows count from its far edge. */
static int sSurfaceH = 256;
/*
 * The battle scene at 1.5 is not composed at 1.5: nearest sampling at a
 * fractional scale makes GBA pixels alternately one and two screen pixels
 * wide, which shimmers on hardware as anything moves. It is composed at 2,
 * every GBA pixel an exact 2x2 block, into a surface of its own, and that
 * surface is drawn at 0.75 with bilinear filtering: every pixel the same
 * size, edges a pixel soft ("sharp bilinear"). The text box is composed
 * after it at 1:1, crisp. See RenderBattleScene.
 */
#define SCENE_ZOOM 2.0f
#define SCENE_W 1024
#define SCENE_H 256
/*
 * The voxel battle's world picture lives in the scene surface's first
 * WORLD_COLUMNS columns (RenderBattleWorld): no VRAM surface of its own,
 * which on hardware the battle found no room for in one battle of two
 * (404 KiB free in blocks under 140 KiB: "3D voxel surface not made", the
 * whole battle flat). The scenery and the slots then start past it, after
 * WORLD_GAP cleared columns: the filter at a slot's left edge reads the
 * column before it.
 */
#define WORLD_COLUMNS 400
#define WORLD_GAP 8
static int sSceneColumn;
/*
 * The GBA line the battle scene is composed down to: the top of the text box
 * frame (CTR_BATTLE_BOX_TOP), which floats a few lines lower than the GBA's
 * picture ends; below that, round the box's panel, the layers are composed
 * at the screen's scale apart (BattleEdgesCompose).
 */
#define BATTLE_SCENE_BOX_BOTTOM (112 + (int)ceilf((CTR_BATTLE_BOX_TOP - (CTR_GAME_HEIGHT - 48)) / CTR_BATTLE_ZOOM))
/* Or the whole picture, with no text box to go behind (BattleSceneBottom). */
static int BattleSceneBottom(void);
#define BATTLE_SCENE_BOTTOM BattleSceneBottom()
static C3D_Tex sSceneTex;
static C3D_RenderTarget *sScene;
static bool sSceneFailed;
static uint32_t sSceneUsedFrame, sSceneFailFrame;
/* Layers (Layers mask bits) the current composition pass leaves out. */
static unsigned sLayerExclude;
/* Screen pixels per GBA pixel that the stereo displacement is measured in. */
static float sShiftZoom = 1.0f;
#define CTR_VIEW_X sViewX
#define CTR_VIEW_Y sViewY
/* Requested by the game bridge, and what the current frame is composed as. */
static bool sStageRequested, sStage;
/* A stage whose every layer fades into the margins (CtrVideo_SetStageFaded). */
static bool sStageFadedRequested, sStageFaded;
/* A stage magnified to fit the screen (CtrVideo_SetStageZoomed). */
static bool sStageZoomedRequested, sStageZoomed;
static bool sCentred;
static unsigned sCentredRequested, sCentredScreen;
static bool sBattleRequested, sBattle;
/*
 * A battle in front of the voxel world (RenderBattleWorld): this frame's
 * scenery is the world, and the layers it stands in for are left out of the
 * battle's own picture - BG3, its scenery, and while the intro slides that
 * in, the entry picture on BG1 and BG2.
 */
static bool sBattleWorld;
static unsigned sWorldLayers;
/* battle_bg.c: BG3 holds a move's background rather than the scenery.
 * battle_intro.c: the intro is sliding the scenery in. */
unsigned char CtrBattleBg_MoveBgShown(void);
unsigned char CtrBattleIntro_Sliding(void);
/* battle_bg.c: the scenery as loaded, a number each load changes. */
uint32_t CtrBattleBg_SceneryVersion(void);
/* Visible extent in GBA coordinates. */
#define VIEW_LEFT ((int)floorf(-sOffX / sZoom) - sViewX)
#define VIEW_TOP ((int)floorf(-sOffY / sZoom) - sViewY)
#define VIEW_RIGHT ((int)ceilf((sTargetW - sOffX) / sZoom) - sViewX)
#define VIEW_BOTTOM ((int)ceilf((sTargetH - sOffY) / sZoom) - sViewY)

/*
 * Scanline scroll (CtrVideo_SetLineScroll): the eight BG scroll registers,
 * one value per line, and which of them the frame drives line by line.
 */
#define LINE_MAX 240
static uint16_t sLineScroll[8][LINE_MAX];
/*
 * BLDALPHA line by line (CtrVideo_SetLineAlpha): the weights each GBA line
 * shows, on while they are not all the same, and while a band of equal lines
 * is composed (ComposeAlphaBands), its value in place of the register's.
 */
static uint16_t sLineAlpha[CTR_GBA_LINES];
static bool sLineAlphaOn, sLineAlphaBanded;
static int sLineAlphaBand = -1;
/* The table's version: one on whenever its weights, or whether it is on,
 * change (CtrVideo_SetLineAlpha). */
static uint32_t sLineAlphaVersion;
static unsigned sLineMask, sLineCount;

#define ATLAS_SIZE 1024
#define CACHE_COUNT 16384
#define HASH_COUNT 32768
/* Citro2D quads a frame. Layer cells, the bulk of them once, have their own
 * buffer (FastCells), which takes the linear memory this used to. */
#define MAX_DRAWS 12288
/* Past this the walks drop quads, and what was composed so is not kept. */
#define DRAW_BUDGET (MAX_DRAWS - 32)

typedef struct
{
    uint32_t key, checked, paletteVersion;
    uint8_t bytes[64];
    uint32_t colors[8];
    /* Bumped on every upload (new bytes or new palette), never reused. */
    uint32_t serial;
    bool valid, visible;
} Tile;
static uint32_t sTileSerial;

/*
 * Tiles are cached by palette: 0-15 the BG banks, 16-31 the OBJ banks, 32 and
 * 33 the BG and OBJ palettes as one 256-colour bank, and from PAL_EXT the OBJ
 * banks past the GBA's sixteen (CtrVideo_LatchExtPalettes), whose colours
 * follow the GBA's 512 here, and from PAL_TILE_BANK the field's banks of
 * other tilesets (CtrVideo_SetTileBank), seven palettes a bank, after those.
 */
#define PAL_BG256 32
#define PAL_OBJ256 33
#define PAL_EXT 34
#define PAL_TILE_BANK (PAL_EXT + CTR_EXT_PALETTES)
#define PAL_IDS (PAL_TILE_BANK + CTR_TILE_BANKS * CTR_TILE_BANK_PALETTES)
/* Where the banks' colours start in sPalette and sTexturePalette. */
#define PAL_TILE_BANK_COLOURS (512 + CTR_EXT_PALETTES * 16)
#define PAL_COLOURS (PAL_TILE_BANK_COLOURS + CTR_TILE_BANKS * CTR_TILE_BANK_PALETTES * 16)

static uint32_t sOamAnchored[4];
static int16_t sOamAnchor[128];

void CtrVideo_ClearOamAnchors(void)
{
    memset(sOamAnchored, 0, sizeof(sOamAnchored));
}

void CtrVideo_SetOamAnchor(unsigned first, unsigned end, int y)
{
    if (end > 128) end = 128;
    for (unsigned i = first; i < end; ++i)
    {
        sOamAnchored[i >> 5] |= 1u << (i & 31);
        sOamAnchor[i] = (int16_t)y;
    }
}

/* The OAM y (8 bits) moved by whole turns of 256 to lie nearest its sprite's
 * true y: an entry of a sprite is never 128 lines from where it is anchored. */
static int OamUnwrapY(unsigned index, int y)
{
    int anchor = sOamAnchor[index];

    while (y - anchor > 128) y -= 256;
    while (anchor - y > 128) y += 256;
    return y;
}

static uint32_t sVoxelWeatherOam[4];
static uint32_t sVoxelCloudOam[4];
/*
 * Which sprites the voxel view draws over itself: the weather, before its
 * text layer, and then the sprites placed on the screen rather than on the
 * map (sScreenOam), which on the GBA sit over the text layer: the mon shown
 * for a field move over its banner, Fly's bird and its rider. The map's own
 * sprites are cards in the 3D scene (voxel_entities.c).
 */
enum { VOXEL_OBJ_NONE, VOXEL_OBJ_WEATHER, VOXEL_OBJ_SCREEN, VOXEL_OBJ_FIELD };
static unsigned sVoxelObjPass = VOXEL_OBJ_NONE;
/* The swoop (voxel_swoop.c) has squashed the world's sprite cards away: the
 * game's own object sprites are laid over the world instead, where the 2D
 * field puts them (VOXEL_OBJ_FIELD). */
static bool sSwoopFieldObjs;

void CtrVideo_ClearVoxelWeatherOam(void)
{
    memset(sVoxelWeatherOam, 0, sizeof(sVoxelWeatherOam));
    memset(sVoxelCloudOam, 0, sizeof(sVoxelCloudOam));
}

void CtrVideo_MarkVoxelWeatherOam(unsigned first, unsigned end)
{
    if (end > 128) end = 128;
    for (unsigned i = first; i < end; ++i)
        sVoxelWeatherOam[i >> 5] |= 1u << (i & 31);
}

void CtrVideo_MarkVoxelCloudOam(unsigned first, unsigned end)
{
    if (end > 128) end = 128;
    for (unsigned i = first; i < end; ++i)
        sVoxelCloudOam[i >> 5] |= 1u << (i & 31);
}

/* OAM entries of sprites placed on the screen rather than on the map. */
static uint32_t sScreenOam[4];
static uint32_t sMachineOam[4];
static uint32_t sFlyOam[4];

void CtrVideo_ClearScreenOam(void)
{
    memset(sScreenOam, 0, sizeof(sScreenOam));
    memset(sMachineOam, 0, sizeof(sMachineOam));
    memset(sFlyOam, 0, sizeof(sFlyOam));
}

void CtrVideo_MarkScreenOam(unsigned first, unsigned end)
{
    if (end > 128) end = 128;
    for (unsigned i = first; i < end; ++i)
        sScreenOam[i >> 5] |= 1u << (i & 31);
}

/* Screen sprites that belong to the map's picture around the player. */
void CtrVideo_MarkMachineOam(unsigned first, unsigned end)
{
    if (end > 128) end = 128;
    for (unsigned i = first; i < end; ++i)
        sMachineOam[i >> 5] |= 1u << (i & 31);
}

/* Fly's bird and its rider, on that picture around the player too. */
void CtrVideo_MarkFlyOam(unsigned first, unsigned end)
{
    if (end > 128) end = 128;
    for (unsigned i = first; i < end; ++i)
        sFlyOam[i >> 5] |= 1u << (i & 31);
}

/* OAM entries of the fog's sprites (CtrVideo_MarkFogOam). */
static uint32_t sFogOam[4];

/* Weather carried over the whole view (CtrVideo_MarkWeatherOam). */
static uint32_t sLatticeOam[4], sParticleOam[4];

void CtrVideo_ClearFogOam(void)
{
    memset(sFogOam, 0, sizeof(sFogOam));
    memset(sLatticeOam, 0, sizeof(sLatticeOam));
    memset(sParticleOam, 0, sizeof(sParticleOam));
}

void CtrVideo_MarkWeatherOam(unsigned first, unsigned end, bool lattice)
{
    uint32_t *marks = lattice ? sLatticeOam : sParticleOam;

    if (end > 128) end = 128;
    for (unsigned i = first; i < end; ++i)
        marks[i >> 5] |= 1u << (i & 31);
}

void CtrVideo_MarkFogOam(unsigned first, unsigned end)
{
    if (end > 128) end = 128;
    for (unsigned i = first; i < end; ++i)
        sFogOam[i >> 5] |= 1u << (i & 31);
}

/*
 * The ext palettes (CtrVideo_LatchExtPalettes): their faded colours as the
 * game last handed them over, which are in use, and the one each OAM entry
 * is drawn with, plus one (0: attr2's own).
 */
static uint16_t sExtPltt[CTR_EXT_PALETTES * 16];
static unsigned sExtInUse;
static uint8_t sOamExt[128];
static bool sOamExtMarked;

void CtrVideo_LatchExtPalettes(const uint16_t *colors, unsigned inUse)
{
    sExtInUse = inUse & ((1u << CTR_EXT_PALETTES) - 1);
    if (sExtInUse)
        memcpy(sExtPltt, colors, sizeof(sExtPltt));
}

const uint16_t *CtrVideo_ExtPalette(unsigned index)
{
    return sExtPltt + (index % CTR_EXT_PALETTES) * 16;
}

void CtrVideo_ClearExtPaletteOam(void)
{
    if (!sOamExtMarked) return;
    memset(sOamExt, 0, sizeof(sOamExt));
    sOamExtMarked = false;
}

void CtrVideo_MarkExtPaletteOam(unsigned first, unsigned end, unsigned index)
{
    if (index >= CTR_EXT_PALETTES) return;
    if (end > 128) end = 128;
    for (unsigned i = first; i < end; ++i)
        sOamExt[i] = (uint8_t)(index + 1);
    sOamExtMarked = sOamExtMarked || first < end;
}

/* The palette bank OAM entry i is drawn with (GetTileSlot): attr2's, or the
 * ext palette it is marked with while that one is in use - a mark outlives a
 * scene that stops building OAM, the palettes do not. A 256-colour sprite has
 * the one bank. */
static unsigned ObjBank(unsigned i, unsigned attr2)
{
    unsigned ext = sOamExt[i];

    if (ext && (sExtInUse >> (ext - 1)) & 1)
        return PAL_EXT + ext - 1;
    return 16u + (attr2 >> 12);
}

/* A battle transition's sprite: on the screen, and not the weather's. */
static bool TransitionOam(unsigned i)
{
    bool screen = (sScreenOam[i >> 5] & (1u << (i & 31))) != 0;
    if (sVoxelWeatherOam[i >> 5] & (1u << (i & 31))) return false;
    return screen;
}

/* Which sprites a pass draws: all, only a transition's, or all but those. */
enum { OBJ_ALL, OBJ_TRANSITION, OBJ_FIELD };
static unsigned sObjFilter = OBJ_ALL;
