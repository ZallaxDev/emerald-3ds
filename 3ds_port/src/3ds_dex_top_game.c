/*
 * The Pokédex on the top screen, game side (3ds_dex_top.h): which Pokémon
 * the device's window shows, and its front picture and battle terrain from
 * the game's own data.
 *
 * The Pokémon is the one on the info page (and its area, cry and size
 * pages), else the one under the list's cursor once it has been seen
 * (pokedex.c, patch 0058). While the list scrolls, or for a moment after the
 * Pokédex names none (a page turning), the window keeps what it shows rather
 * than blinking and decoding every Pokémon scrolled past.
 */
#include <string.h>

#include "global.h"
#include "main.h"
#include "data.h"
#include "pokemon.h"
#include "graphics.h"
#include "overworld.h"
#include "wild_encounter.h"
#include "constants/battle.h"
#include "constants/characters.h"
#include "constants/map_groups.h"
#include "constants/map_types.h"
#include "constants/species.h"
#include "constants/wild_encounter.h"

#include "port_platform.h"
#include "3ds_video.h"
#include "3ds_dex_top.h"

/* pokedex.c */
bool8 CtrPokedex_IsOpen(void);
u16 CtrPokedex_InfoDexNum(void);
bool8 CtrPokedex_ListScrolling(void);

/* Frames the window keeps its Pokémon after the Pokédex names none while it
 * is still open: a page turning, the cursor on one not yet seen. */
#define HOLD_FRAMES 60
#define FRAME_BYTES (CTR_DEX_TOP_DIM * CTR_DEX_TOP_DIM / 2)

static bool8 sOpen;
static u16 sShown;
static u8 sHold;
/* The terrain whose backdrop the native side holds whole, or -1: only nine,
 * so a new Pokémon on the same one sends nothing again. */
static s16 sBackdropTerrain = -1;

/* LZ77 data of a game symbol (an asset stub, resolved from the game data)
 * into a buffer of `capacity` bytes; the size comes from the stream's own
 * header. As 3ds_bottom_ui.c's Unlz, without the heap: the game's is nearly
 * full by the time the field is up. */
static u32 Unlz(const void *src, void *dst, u32 capacity)
{
    const u8 *res = src ? Port_ResolveAssetPointer(src) : NULL;
    u32 header, size;

    if (!res)
        return 0;
    header = res[0] | (res[1] << 8) | (res[2] << 16) | ((u32)res[3] << 24);
    size = header >> 8;
    if ((header & 0xFF) != 0x10 || size == 0 || size > capacity)
        return 0;
    LZ77UnCompWram((const u32 *)res, dst);
    return size;
}

/* A species' front picture as 64x64 RGBA5551 frames (rows top to bottom,
 * 0 = transparent): the first two of its frames (Castform has four). */
static unsigned DecodePicture(u16 species, u16 *out)
{
    static u8 tiles[4 * FRAME_BYTES];
    static u16 pal[16];
    unsigned frames;
    u32 size = Unlz(gMonFrontPicTable[species].data, tiles, sizeof(tiles));

    if (size < FRAME_BYTES || Unlz(gMonPaletteTable[species].data, pal, sizeof(pal)) != sizeof(pal))
        return 0;
    frames = size >= 2 * FRAME_BYTES ? 2 : 1;
    for (unsigned f = 0; f < frames; ++f)
        for (unsigned y = 0; y < CTR_DEX_TOP_DIM; ++y)
            for (unsigned x = 0; x < CTR_DEX_TOP_DIM; ++x)
            {
                unsigned tile = (y / 8) * 8 + x / 8;
                u8 v = (tiles[f * FRAME_BYTES + tile * 32 + (y & 7) * 4 + (x & 7) / 2] >> ((x & 1) * 4)) & 15;

                out[(f * CTR_DEX_TOP_DIM + y) * CTR_DEX_TOP_DIM + x] = v ? CtrVideo_RGBA5551(pal[v]) : 0;
            }
    return frames;
}

/* A species' name in ASCII capitals, from the game's own character set
 * (the window's font has letters, digits, '.' and '-'). */
static void AsciiName(u16 species, char *out, unsigned size)
{
    const u8 *s = gSpeciesNames[species];
    unsigned n = 0;

    while (*s != EOS && n + 1 < size)
    {
        u8 c = *s++;

        if (c >= CHAR_A && c < CHAR_A + 26) out[n++] = 'A' + (c - CHAR_A);
        else if (c >= CHAR_a && c < CHAR_a + 26) out[n++] = 'A' + (c - CHAR_a);
        else if (c >= CHAR_0 && c < CHAR_0 + 10) out[n++] = '0' + (c - CHAR_0);
        else if (c == CHAR_PERIOD) out[n++] = '.';
        else if (c == CHAR_HYPHEN) out[n++] = '-';
        else if (c == CHAR_SPACE) out[n++] = ' ';
    }
    out[n] = 0;
}

/*
 * The backdrop: the battle terrain the species is met in. Its first place in
 * the wild encounter tables decides - tall grass (a cave's if the map is
 * underground), water (the sea's on an ocean route, else a pond's), rock
 * smash a mountain's - and a species never met wild takes one by its type.
 * The terrain's tiles, tilemap and palettes (the game's own, as the battle
 * loads them into palettes 2..4) are decoded into 256x128 pixels, row by
 * row, for the native side.
 */
struct Terrain
{
    const u32 *tiles, *tilemap, *palette;
};

static const struct Terrain sTerrains[] =
{
    [BATTLE_ENVIRONMENT_GRASS] = {gBattleEnvironmentTiles_TallGrass, gBattleEnvironmentTilemap_TallGrass, gBattleEnvironmentPalette_TallGrass},
    [BATTLE_ENVIRONMENT_LONG_GRASS] = {gBattleEnvironmentTiles_LongGrass, gBattleEnvironmentTilemap_LongGrass, gBattleEnvironmentPalette_LongGrass},
    [BATTLE_ENVIRONMENT_SAND] = {gBattleEnvironmentTiles_Sand, gBattleEnvironmentTilemap_Sand, gBattleEnvironmentPalette_Sand},
    [BATTLE_ENVIRONMENT_UNDERWATER] = {gBattleEnvironmentTiles_Underwater, gBattleEnvironmentTilemap_Underwater, gBattleEnvironmentPalette_Underwater},
    [BATTLE_ENVIRONMENT_WATER] = {gBattleEnvironmentTiles_Water, gBattleEnvironmentTilemap_Water, gBattleEnvironmentPalette_Water},
    [BATTLE_ENVIRONMENT_POND] = {gBattleEnvironmentTiles_PondWater, gBattleEnvironmentTilemap_PondWater, gBattleEnvironmentPalette_PondWater},
    [BATTLE_ENVIRONMENT_MOUNTAIN] = {gBattleEnvironmentTiles_Rock, gBattleEnvironmentTilemap_Rock, gBattleEnvironmentPalette_Rock},
    [BATTLE_ENVIRONMENT_CAVE] = {gBattleEnvironmentTiles_Cave, gBattleEnvironmentTilemap_Cave, gBattleEnvironmentPalette_Cave},
    [BATTLE_ENVIRONMENT_BUILDING] = {gBattleEnvironmentTiles_Building, gBattleEnvironmentTilemap_Building, gBattleEnvironmentPalette_Building},
};

static bool8 InTable(const struct WildPokemonInfo *info, unsigned count, u16 species)
{
    if (info == NULL) return FALSE;
    for (unsigned i = 0; i < count; ++i)
        if (info->wildPokemon[i].species == species) return TRUE;
    return FALSE;
}

static u8 TerrainOf(u16 species)
{
    for (unsigned i = 0; gWildMonHeaders[i].mapGroup != MAP_GROUP(MAP_UNDEFINED); ++i)
    {
        const struct WildPokemonHeader *h = &gWildMonHeaders[i];
        const struct MapHeader *map = Overworld_GetMapHeaderByGroupAndId(h->mapGroup, h->mapNum);
        u8 type = map ? map->mapType : MAP_TYPE_ROUTE;

        if (InTable(h->landMonsInfo, LAND_WILD_COUNT, species))
            return type == MAP_TYPE_UNDERGROUND ? BATTLE_ENVIRONMENT_CAVE : BATTLE_ENVIRONMENT_GRASS;
        if (InTable(h->waterMonsInfo, WATER_WILD_COUNT, species) || InTable(h->fishingMonsInfo, FISH_WILD_COUNT, species))
            return type == MAP_TYPE_OCEAN_ROUTE ? BATTLE_ENVIRONMENT_WATER
                 : type == MAP_TYPE_UNDERWATER ? BATTLE_ENVIRONMENT_UNDERWATER : BATTLE_ENVIRONMENT_POND;
        if (InTable(h->rockSmashMonsInfo, ROCK_WILD_COUNT, species))
            return BATTLE_ENVIRONMENT_MOUNTAIN;
    }
    switch (gSpeciesInfo[species].types[0])
    {
    case TYPE_WATER: return BATTLE_ENVIRONMENT_WATER;
    case TYPE_ROCK: case TYPE_GROUND: case TYPE_STEEL: case TYPE_DARK: return BATTLE_ENVIRONMENT_CAVE;
    case TYPE_FIRE: case TYPE_DRAGON: return BATTLE_ENVIRONMENT_MOUNTAIN;
    case TYPE_PSYCHIC: case TYPE_GHOST: return BATTLE_ENVIRONMENT_BUILDING;
    default: return BATTLE_ENVIRONMENT_GRASS;
    }
}

static void Backdrop(u16 species)
{
    static u8 tiles[8192];
    /* The battle background is 512 pixels wide: two screen blocks side by
     * side, the left one first; its left 256 columns are the picture. */
    static u16 map[2 * 32 * 32];
    static u16 pal[3 * 16];
    static u16 row[CTR_DEX_TOP_BACKDROP_W];
    u8 terrain = TerrainOf(species);
    const struct Terrain *t = &sTerrains[terrain];

    if (terrain == sBackdropTerrain && CtrDexTop_BackdropComplete())
        return;
    sBackdropTerrain = -1;
    CtrDexTop_BackdropBegin();
    if (Unlz(t->tiles, tiles, sizeof(tiles)) == 0 || Unlz(t->tilemap, map, sizeof(map)) == 0
        || Unlz(t->palette, pal, sizeof(pal)) < sizeof(pal))
        return;
    for (unsigned y = 0; y < CTR_DEX_TOP_BACKDROP_H; ++y)
    {
        for (unsigned x = 0; x < CTR_DEX_TOP_BACKDROP_W; ++x)
        {
            u16 entry = map[(y / 8) * 32 + x / 8];
            unsigned tile = entry & 0x3ff, palette = entry >> 12;
            unsigned tx = (entry & 0x400) ? 7 - (x & 7) : (x & 7), ty = (entry & 0x800) ? 7 - (y & 7) : (y & 7);
            u8 v = tile < 256 ? (tiles[tile * 32 + ty * 4 + tx / 2] >> ((tx & 1) * 4)) & 15 : 0;
            u16 colour = palette >= 2 && palette < 5 ? pal[(palette - 2) * 16 + v] : pal[0];

            row[x] = CtrVideo_RGBA5551(colour);
        }
        CtrDexTop_BackdropRow(y, row);
    }
    sBackdropTerrain = terrain;
}

static void Show(u16 national)
{
    static u16 picture[CTR_DEX_TOP_FRAMES][CTR_DEX_TOP_DIM * CTR_DEX_TOP_DIM];
    char name[CTR_DEX_TOP_NAME];
    u16 species = national ? NationalPokedexNumToSpecies(national) : SPECIES_NONE;
    unsigned frames = species != SPECIES_NONE ? DecodePicture(species, picture[0]) : 0;

    if (frames == 0)
    {
        CtrDexTop_Show(0, NULL, 0, NULL);
        return;
    }
    AsciiName(species, name, sizeof(name));
    CtrDexTop_Show(national, picture[0], frames, name);
    Backdrop(species);
}

void CtrDexTopGame_Frame(void)
{
    u16 national;

    if (!CtrPokedex_IsOpen())
    {
        if (sOpen) CtrDexTop_Hide();
        sOpen = FALSE;
        sShown = sHold = 0;
        sBackdropTerrain = -1;
        return;
    }
    national = CtrPokedex_InfoDexNum();
    if (sOpen && sShown != 0)
    {
        /* Scrolling: what is shown waits for the list to settle. A page
         * turning: it stays a moment rather than blinking off. */
        if (CtrPokedex_ListScrolling())
            national = sShown;
        else if (national == 0 && sHold < HOLD_FRAMES)
        {
            national = sShown;
            ++sHold;
        }
        else if (national != 0)
            sHold = 0;
    }
    if (!sOpen || national != sShown)
    {
        sOpen = TRUE;
        sShown = national;
        Show(national);
    }
}
