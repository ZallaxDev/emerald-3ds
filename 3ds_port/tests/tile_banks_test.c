/* The field's banks of other tilesets (3ds_tile_banks.h): how a bank's
 * colours follow the shown palettes, and how a banked entry is read. */
#include <assert.h>
#include <stdio.h>
#include "3ds_tile_banks.h"

#define RGB(r, g, b) ((uint16_t)((r) | (g) << 5 | (b) << 10))

/* The game's BlendPalette: each channel towards the target by coeff / 16. */
static uint16_t Blend(uint16_t colour, uint16_t target, unsigned coeff)
{
    uint16_t out = 0;

    for (unsigned c = 0; c < 3; ++c)
    {
        int from = (colour >> (c * 5)) & 31, to = (target >> (c * 5)) & 31;

        out |= (uint16_t)((from + ((to - from) * (int)coeff >> 4)) << (c * 5));
    }
    return out;
}

static void LutTests(void)
{
    uint16_t unfaded[112], shown[112];
    uint8_t lut[3][32];

    /* Every value of every channel seen once, in some colour. */
    for (unsigned i = 0; i < 112; ++i)
        unfaded[i] = RGB(i % 32, (i * 7) % 32, (i * 13) % 32);

    /* Shown as unfaded: every colour as it is. */
    CtrTileBank_BuildLut(unfaded, unfaded, 112, lut);
    for (unsigned v = 0; v < 32; ++v)
        assert(lut[0][v] == v && lut[1][v] == v && lut[2][v] == v);

    /* A blend towards a colour: any colour comes out as the game blends it. */
    for (unsigned i = 0; i < 112; ++i)
        shown[i] = Blend(unfaded[i], RGB(4, 12, 28), 9);
    CtrTileBank_BuildLut(unfaded, shown, 112, lut);
    for (unsigned r = 0; r < 32; r += 3)
        for (unsigned g = 0; g < 32; g += 5)
            for (unsigned b = 0; b < 32; b += 7)
                assert(CtrTileBank_ApplyLut(lut, RGB(r, g, b)) == Blend(RGB(r, g, b), RGB(4, 12, 28), 9));

    /* A value no colour has: between its nearest known ones, or past the
     * last at its offset. Only red values 4 and 20 seen, at 8 and 24. */
    {
        uint16_t u[2] = {RGB(4, 0, 0), RGB(20, 0, 0)}, s[2] = {RGB(8, 0, 0), RGB(24, 0, 0)};

        CtrTileBank_BuildLut(u, s, 2, lut);
        assert(lut[0][4] == 8 && lut[0][20] == 24 && lut[0][12] == 16);
        assert(lut[0][0] == 4 && lut[0][30] == 31);
        /* The other channels saw only 0 -> 0: their offset is none. */
        assert(lut[1][17] == 17 && lut[2][31] == 31);
    }

    /* Nothing seen: the colours as they are. */
    CtrTileBank_BuildLut(unfaded, shown, 0, lut);
    assert(CtrTileBank_ApplyLut(lut, RGB(3, 17, 29)) == RGB(3, 17, 29));
    /* The top bit is no colour. */
    CtrTileBank_BuildLut(unfaded, unfaded, 112, lut);
    assert(CtrTileBank_ApplyLut(lut, 0x8000 | RGB(1, 2, 3)) == RGB(1, 2, 3));
    puts("PASS tile banks: colours follow the shown palettes (none, a blend, unseen values, nothing seen)");
}

static void ResolveTests(void)
{
    unsigned tile, palette;
    bool fromBank, bankPalette;

    /* A secondary tile in a secondary palette: both the bank's. */
    CtrTileBank_Resolve(7u << 12 | 0xC00 | 600, &tile, &fromBank, &palette, &bankPalette);
    assert(fromBank && tile == 88 && bankPalette && palette == 1);
    /* A primary tile in a primary palette: the entry's own. */
    CtrTileBank_Resolve(3u << 12 | 10, &tile, &fromBank, &palette, &bankPalette);
    assert(!fromBank && tile == 10 && !bankPalette && palette == 3);
    /* A primary tile in a secondary palette, and the other way round. */
    CtrTileBank_Resolve(12u << 12 | 511, &tile, &fromBank, &palette, &bankPalette);
    assert(!fromBank && tile == 511 && bankPalette && palette == 6);
    CtrTileBank_Resolve(5u << 12 | 512, &tile, &fromBank, &palette, &bankPalette);
    assert(fromBank && tile == 0 && !bankPalette && palette == 5);
    /* The text and UI palettes past the secondary's stay the entry's. */
    CtrTileBank_Resolve(13u << 12 | 1023, &tile, &fromBank, &palette, &bankPalette);
    assert(fromBank && tile == 511 && !bankPalette && palette == 13);
    /* The field's empty and filler entries name nothing of a bank. */
    CtrTileBank_Resolve(0, &tile, &fromBank, &palette, &bankPalette);
    assert(!fromBank && !bankPalette);
    CtrTileBank_Resolve(0x3014, &tile, &fromBank, &palette, &bankPalette);
    assert(!fromBank && !bankPalette);
    puts("PASS tile banks: a banked entry's tile and palette, and the entries that keep their own");
}

/* The table of banked tilemaps through Route 111 with the Mirage Tower: in by
 * a warp the field's buffers land past the tower's (B), out by another warp
 * the heap is reset without unregistering and they are back where they were
 * (A). Each row of banks must find its slot again. */
static void SourceSlotTests(void)
{
    static uint8_t rows[3][4];
    static char a[3], b[3];
    const void *buffers[3] = {NULL, NULL, NULL};
    const uint8_t *banks[3] = {NULL, NULL, NULL};

#define REGISTER(set)                                                                       \
    for (unsigned bg = 0; bg < 3; ++bg)                                                     \
    {                                                                                       \
        unsigned slot = CtrTileBank_SourceSlot(buffers, banks, 3, &(set)[bg], rows[bg]);    \
        assert(slot < 3);                                                                   \
        buffers[slot] = &(set)[bg];                                                         \
        banks[slot] = rows[bg];                                                             \
    }
    REGISTER(a);
    REGISTER(b);
    REGISTER(a);
#undef REGISTER
    for (unsigned bg = 0; bg < 3; ++bg)
    {
        unsigned found = 3;

        for (unsigned i = 0; i < 3; ++i)
            if (buffers[i] == &a[bg]) found = i;
        assert(found < 3 && banks[found] == rows[bg]);
    }

    /* A tilemap of another screen with no row there yet: a free slot, else none. */
    buffers[1] = NULL;
    assert(CtrTileBank_SourceSlot(buffers, banks, 3, &b[0], (const uint8_t *)&b[1]) == 1);
    buffers[1] = &a[1];
    assert(CtrTileBank_SourceSlot(buffers, banks, 3, &b[0], (const uint8_t *)&b[1]) == 3);
    printf("PASS tile banks: each row of banks keeps its slot when the field's buffers move\n");
}

int main(void)
{
    LutTests();
    ResolveTests();
    SourceSlotTests();
    return 0;
}
