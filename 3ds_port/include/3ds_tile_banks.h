#ifndef CTR_TILE_BANKS_H
#define CTR_TILE_BANKS_H

/*
 * The field's other tilesets, in 2D.
 *
 * The GBA holds one primary and one secondary tileset in VRAM, the current
 * map's, and draws whatever it shows of the maps joined to it with them. Its
 * 240-pixel screen sees about seven metatiles past a crossing, which the maps
 * were drawn for; the 3DS view sees much further, and where a neighbour has
 * another secondary tileset (Petalburg's next to Route 104's Rustboro set)
 * its ground came out in the current set's art, and the map just left turned
 * into roofs and fences the moment the player crossed.
 *
 * So a neighbour with another secondary tileset (the primary is shared by
 * every outdoor map) takes a bank: its secondary tiles and its seven
 * palettes (6-12), kept by the renderer beside the GBA's VRAM, and the cells
 * of the field's tilemap drawn from that map carry the bank's number
 * (field_camera.c), handed over with the tilemap (bg.c, LoadBgVram). The
 * renderer reads an entry of a banked cell as the neighbour's: a tile index
 * past the primary's from the bank, a palette 6-12 the bank's.
 *
 * The bank's palettes are its tileset's own colours taken through whatever
 * the shown palettes are taken through: a uniform fade the renderer applies
 * itself (from the unfaded colours, as for the current map), anything else -
 * the weather's colour maps, a blend that is not a fade - channel by channel,
 * as the current map's palettes were changed (CtrTileBank_BuildLut). Tile
 * animations of the bank's own tileset stand still; the primary's (water,
 * flowers) are shared and move.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CTR_TILE_BANKS 2
#define CTR_TILE_BANK_FIRST_PALETTE 6
#define CTR_TILE_BANK_PALETTES 7
/* Tile indices from here on are the secondary tileset's. */
#define CTR_TILE_BANK_FIRST_TILE 512
#define CTR_TILE_BANK_TILES 512
#define CTR_TILE_BANK_BYTES (CTR_TILE_BANK_TILES * 32)

/*
 * A channel's map from unfaded to shown value (0-31), learnt from colours
 * whose unfaded and shown values are both known. A value no colour has is
 * placed between the nearest known ones (linearly), or past the last at its
 * offset; with none known the channel is left as it is.
 */
static inline void CtrTileBank_BuildLut(const uint16_t *unfaded, const uint16_t *shown, unsigned count,
                                        uint8_t lut[3][32])
{
    for (unsigned c = 0; c < 3; ++c)
    {
        int known[32];
        int any = 0;

        for (unsigned v = 0; v < 32; ++v)
            known[v] = -1;
        for (unsigned i = 0; i < count; ++i)
        {
            unsigned u = (unfaded[i] >> (c * 5)) & 31, s = (shown[i] >> (c * 5)) & 31;

            if (known[u] < 0)
            {
                known[u] = (int)s;
                any = 1;
            }
        }
        for (int v = 0; v < 32; ++v)
        {
            int lo = v, hi = v, value;

            if (!any)
            {
                lut[c][v] = (uint8_t)v;
                continue;
            }
            if (known[v] >= 0)
            {
                lut[c][v] = (uint8_t)known[v];
                continue;
            }
            while (lo >= 0 && known[lo] < 0) --lo;
            while (hi < 32 && known[hi] < 0) ++hi;
            if (lo >= 0 && hi < 32)
                value = known[lo] + ((known[hi] - known[lo]) * (v - lo) + (hi - lo) / 2) / (hi - lo);
            else if (lo >= 0)
                value = known[lo] + (v - lo);
            else
                value = known[hi] - (hi - v);
            lut[c][v] = (uint8_t)(value < 0 ? 0 : value > 31 ? 31 : value);
        }
    }
}

static inline uint16_t CtrTileBank_ApplyLut(const uint8_t lut[3][32], uint16_t colour)
{
    return (uint16_t)(lut[0][colour & 31] | lut[1][(colour >> 5) & 31] << 5 | lut[2][(colour >> 10) & 31] << 10);
}

/*
 * Where a banked entry's tile and palette come from: *tile is the bank's
 * tile (0-511) and *fromBank set, or the entry's own index; *palette is the
 * bank palette (0-6) and *bankPalette set, or the entry's own bank.
 */
static inline void CtrTileBank_Resolve(unsigned entry, unsigned *tile, bool *fromBank,
                                       unsigned *palette, bool *bankPalette)
{
    unsigned index = entry & 1023, pal = entry >> 12;

    *fromBank = index >= CTR_TILE_BANK_FIRST_TILE;
    *tile = *fromBank ? index - CTR_TILE_BANK_FIRST_TILE : index;
    *bankPalette = pal >= CTR_TILE_BANK_FIRST_PALETTE
                && pal < CTR_TILE_BANK_FIRST_PALETTE + CTR_TILE_BANK_PALETTES;
    *palette = *bankPalette ? pal - CTR_TILE_BANK_FIRST_PALETTE : pal;
}

/* The slot of the renderer's table of banked tilemaps (count slots, each a
 * buffer and its row of banks; a free slot's buffer NULL) that a tilemap and
 * its banks take: the one either is already in, else the first free one, else
 * count. A row of banks keeps its slot whatever buffer it comes with: a warp
 * resets the heap without unregistering, and the next buffers need not land
 * where the last ones were (Route 111's Mirage Tower allocates first), which
 * would leave every slot to a freed address. */
static inline unsigned CtrTileBank_SourceSlot(const void *const *buffers, const uint8_t *const *banks,
                                              unsigned count, const void *buffer, const uint8_t *rowBanks)
{
    unsigned free = count;

    for (unsigned i = 0; i < count; ++i)
    {
        if (buffers[i] == buffer || banks[i] == rowBanks) return i;
        if (buffers[i] == NULL && free == count) free = i;
    }
    return free;
}

#endif
