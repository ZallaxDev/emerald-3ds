/*
 * Part of 3ds_video.c. The field's banks of other tilesets
 * (3ds_tile_banks.h): tiles, palettes and the tilemap entries that use them.
 * Offers TilemapBankAt and BankedTileSlot, called per cell. Ends with the
 * libraries' flags and the FPS counters.
 */

/*
 * The field's banks of other tilesets (3ds_tile_banks.h): their tiles sit in
 * logical addresses past the GBA's VRAM, TILE_BANK_BASE on, CTR_TILE_BANK_BYTES
 * a bank, so the tile cache keys them as it keys any tile. A bank's tiles are
 * whole or not there: sTileBankStamp is the token at which they last changed.
 */
#define TILE_BANK_BASE VRAM_BYTES
static uint8_t sTileBankTiles[CTR_TILE_BANKS][CTR_TILE_BANK_BYTES];
static uint16_t sTileBankPalettes[CTR_TILE_BANKS][CTR_TILE_BANK_PALETTES * 16];
static uint32_t sTileBankStamp[CTR_TILE_BANKS];
static unsigned sTileBanksLive;
static bool sTileBanksChanged;

/*
 * The banks of the tilemap entries in VRAM TILEMAP_BANK_BASE on (the field's
 * screenblocks are 25-30), and the entry each was handed over with: a bank
 * holds only while VRAM holds that entry, so another screen writing there
 * without bg.c's copies never draws from a bank. sTilemapBankRow stamps a
 * row of 32 entries whose banks changed, which its VRAM need not have done
 * (a map left behind drawn again, the same entries now from a bank).
 */
#define TILEMAP_BANK_BASE 0x8000u
#define TILEMAP_BANK_ENTRIES ((0x10000u - TILEMAP_BANK_BASE) / 2)
static uint8_t sTilemapBank[TILEMAP_BANK_ENTRIES];
static uint16_t sTilemapBankEntry[TILEMAP_BANK_ENTRIES];
static uint32_t sTilemapBankRow[TILEMAP_BANK_ENTRIES / 32];
static uint32_t sTilemapBankStamp;
static unsigned sTilemapBanked;

#define TILEMAP_BANK_SOURCES 3
static struct
{
    const void *buffer;
    const uint8_t *banks;
    unsigned entries;
} sTilemapBankSources[TILEMAP_BANK_SOURCES];

void CtrVideo_SetTileBank(unsigned bank, const void *tiles, unsigned bytes, const uint16_t *palettes)
{
    if (bank >= CTR_TILE_BANKS) return;
    if (tiles == NULL)
    {
        sTileBanksLive &= ~(1u << bank);
        return;
    }
    if (bytes > CTR_TILE_BANK_BYTES) bytes = CTR_TILE_BANK_BYTES;
    memset(sTileBankTiles[bank], 0, sizeof(sTileBankTiles[bank]));
    memcpy(sTileBankTiles[bank], tiles, bytes);
    if (palettes != NULL)
        memcpy(sTileBankPalettes[bank], palettes, sizeof(sTileBankPalettes[bank]));
    else
        memset(sTileBankPalettes[bank], 0, sizeof(sTileBankPalettes[bank]));
    sTileBankStamp[bank] = sStats.frames + 1;
    sTileBanksLive |= 1u << bank;
    sTileBanksChanged = true;
    /* Every layer walks its cells again, as for new palettes. */
    sBgPaletteStamp = sStats.frames + 1;
    sBgOtherStamp = sStats.frames + 1;
}

void CtrVideo_RegisterTilemapBanks(const void *buffer, const uint8_t *banks, unsigned entries)
{
    const void *buffers[TILEMAP_BANK_SOURCES];
    const uint8_t *rows[TILEMAP_BANK_SOURCES];
    unsigned i;
    static bool sFullLogged;

    for (i = 0; i < TILEMAP_BANK_SOURCES; ++i)
    {
        buffers[i] = sTilemapBankSources[i].buffer;
        rows[i] = sTilemapBankSources[i].banks;
    }
    i = CtrTileBank_SourceSlot(buffers, rows, TILEMAP_BANK_SOURCES, buffer, banks);
    if (i == TILEMAP_BANK_SOURCES)
    {
        if (!sFullLogged) CtrLog_Write(CTR_LOG_VIDEO, "2D tile banks: no slot left for a tilemap");
        sFullLogged = true;
        return;
    }
    sTilemapBankSources[i].buffer = buffer;
    sTilemapBankSources[i].banks = banks;
    sTilemapBankSources[i].entries = entries;
}

void CtrVideo_UnregisterTilemapBanks(const void *buffer)
{
    for (unsigned i = 0; i < TILEMAP_BANK_SOURCES; ++i)
        if (sTilemapBankSources[i].buffer == buffer)
            sTilemapBankSources[i].buffer = NULL;
}

void CtrVideo_TilemapRequested(unsigned vramOffset, const void *src, unsigned bytes)
{
    const uint8_t *banks = NULL;
    const uint16_t *entries = src;
    unsigned first, count, available = 0;

    if (vramOffset >= 0x10000 || vramOffset + bytes <= TILEMAP_BANK_BASE) return;
    for (unsigned i = 0; i < TILEMAP_BANK_SOURCES; ++i)
        if (sTilemapBankSources[i].buffer == src && src != NULL)
        {
            banks = sTilemapBankSources[i].banks;
            available = sTilemapBankSources[i].entries;
            break;
        }
    /* Nothing banked there and nothing to bank: nothing to do. */
    if (banks == NULL && sTilemapBanked == 0) return;
    if (banks != NULL && sTilemapBanked == 0 && vramOffset >= TILEMAP_BANK_BASE)
    {
        /* The field's three layers come here at every camera step, most of
         * them with no neighbour banked: their rows read a word at a time
         * instead of every entry walked against the table. */
        unsigned n = bytes / 2 < available ? bytes / 2 : available, i = 0;
        uint32_t any = 0, word;

        for (; i + 4 <= n; i += 4)
        {
            memcpy(&word, banks + i, 4);
            any |= word;
        }
        for (; i < n; ++i)
            any |= banks[i];
        if (any == 0) return;
    }
    if (bytes > 0x10000 - vramOffset) bytes = 0x10000 - vramOffset;
    first = vramOffset >= TILEMAP_BANK_BASE ? (vramOffset - TILEMAP_BANK_BASE) / 2 : 0;
    count = (vramOffset + bytes - TILEMAP_BANK_BASE) / 2 - first;
    if (vramOffset < TILEMAP_BANK_BASE)
    {
        unsigned skipped = (TILEMAP_BANK_BASE - vramOffset) / 2;

        entries += skipped;
        if (banks != NULL) banks += skipped;
        available = available > skipped ? available - skipped : 0;
    }
    for (unsigned i = 0; i < count; ++i)
    {
        unsigned at = first + i;
        uint8_t bank = banks != NULL && i < available ? banks[i] : 0;
        uint16_t entry = bank ? entries[i] : 0;

        if (bank > CTR_TILE_BANKS) bank = 0;
        if (sTilemapBank[at] == bank && sTilemapBankEntry[at] == entry) continue;
        if ((sTilemapBank[at] != 0) != (bank != 0))
            sTilemapBanked += bank ? 1u : ~0u;
        sTilemapBank[at] = bank;
        sTilemapBankEntry[at] = entry;
        sTilemapBankRow[at / 32] = sStats.frames + 1;
        sTilemapBankStamp = sStats.frames + 1;
    }
}

/* Whether the banks of a row of the tilemap (its VRAM address) changed after t. */
static bool TilemapBanksChangedAfter(unsigned rowAddress, uint32_t t)
{
    if (rowAddress < TILEMAP_BANK_BASE || rowAddress >= 0x10000) return false;
    return sTilemapBankRow[(rowAddress - TILEMAP_BANK_BASE) / 64] > t;
}

/* The bank (1 + bank) of the entry at VRAM entryAddress, read as `entry`: 0 for none. */
static unsigned TilemapBankAt(unsigned entryAddress, unsigned entry)
{
    unsigned at, bank;

    if (sTilemapBanked == 0 || entryAddress < TILEMAP_BANK_BASE || entryAddress >= 0x10000) return 0;
    at = (entryAddress - TILEMAP_BANK_BASE) / 2;
    bank = sTilemapBank[at];
    if (bank == 0 || sTilemapBankEntry[at] != entry || !(sTileBanksLive & (1u << (bank - 1)))) return 0;
    return bank;
}

static int GetTileSlot(unsigned address, unsigned bank, bool color256);

/* The slot of a 4bpp entry of bank `bank` (1 + bank), its tiles from chars. */
static int BankedTileSlot(unsigned bank, unsigned entry, unsigned chars)
{
    unsigned tile, palette, address;
    bool fromBank, bankPalette;

    CtrTileBank_Resolve(entry, &tile, &fromBank, &palette, &bankPalette);
    --bank;
    address = fromBank ? TILE_BANK_BASE + bank * CTR_TILE_BANK_BYTES + tile * 32 : chars + tile * 32;
    if (!fromBank && address >= 0x10000) return -1;
    return GetTileSlot(address, bankPalette ? PAL_TILE_BANK + bank * CTR_TILE_BANK_PALETTES + palette : palette,
                       false);
}
static bool sC3d, sC2d;
static uint64_t sFpsStart;
static unsigned sFpsFrames;
static uint32_t sReported;
