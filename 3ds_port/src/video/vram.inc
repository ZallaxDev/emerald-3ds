/*
 * Part of 3ds_video.c. The memory the game writes and what the compositor
 * made of it: sMemory, sStats, the atlas and surfaces, the tile cache, the
 * palettes and VRAM's shadow with its stamps. Offers the announced writes
 * (CtrVideo_NotifyWrite), SyncBlock and VramChangedAfter.
 */

static CtrVideoMemory sMemory;

const uint8_t *CtrVideo_GetBgVram(void) { return sMemory.vram; }

void CtrVideo_NotifyTilesetAnimWrite(const void *dest, unsigned bytes)
{
    CtrVideo_NotifyWrite(dest, bytes);
    uintptr_t base = (uintptr_t)sMemory.vram;
    uintptr_t address = (uintptr_t)dest;
    if (base == 0 || address < base || address - base >= 0x8000 || bytes == 0)
        return;
    unsigned offset = (unsigned)(address - base);
    if (bytes > 0x8000 - offset) bytes = 0x8000 - offset;
    CtrVoxel_NotifyTilesetAnimWrite(offset / 32, (offset % 32 + bytes + 31) / 32);
}
static CtrVideoStats sStats;
static C3D_Tex sAtlas, sSurface;
/* The voxel bloom's quarter-size target (VoxelBloomPrepare); absent, no bloom. */
static C3D_Tex sBloomTex;
static C3D_RenderTarget *sBloom;
static C3D_RenderTarget *sLogical, *sTop, *sTopRight;
static bool sStereo;
static Tile sTiles[CACHE_COUNT];
static uint16_t sHash[HASH_COUNT];
static uint16_t sPalette[PAL_COLOURS];
static uint16_t sTexturePalette[PAL_COLOURS];
static uint8_t sMorton[64];
/* A 4bpp tile's two pixels of a byte, (x, y) and (x + 1, y) for an even x,
 * are neighbouring texels in the tiled order: written as one word. */
static uint8_t sMortonPair[32];
static bool sMortonPairs;
static C2D_ImageTint sTint;
static uint32_t sPaletteVersion[PAL_IDS];
static uint32_t sPaletteChanges[PAL_IDS][8];
static unsigned sUsed;

/*
 * What changed in the logical VRAM, a kilobyte at a time. The game writes VRAM
 * in many ways (copies, DMA, decompression, plain pointers), so instead of
 * hooking them all a block is compared with a copy of what it held when it
 * was last looked at: against walking every cell of every layer and comparing
 * every tile drawn with its bytes, frame after frame, when nothing changed.
 * sVramStamp[b] is the frame token (sStats.frames + 1, as Tile.checked) at
 * which block b was last seen changed; something checked at token t still
 * holds while the stamps of its blocks are <= t.
 *
 * A block is compared the first time it is asked about in a frame (SyncBlock),
 * not every block every frame: all 96 KiB were 1.5 ms a frame on an Old 3DS,
 * in every mode, and the voxel field asks about its overlay alone. Whatever
 * records a token t for some VRAM syncs its blocks at t, so a block changed
 * and changed back while nobody looked is no change to anyone.
 */
#define VRAM_BYTES 0x18000
#define VRAM_BLOCK 1024
static uint32_t sVramShadow[VRAM_BYTES / 4];
static uint32_t sVramStamp[VRAM_BYTES / VRAM_BLOCK];
/* The frame token at which each block was last compared. */
static uint32_t sVramSynced[VRAM_BYTES / VRAM_BLOCK];
/* Blocks written through an announced copy since they were last compared
 * (CtrVideo_NotifyWrite), and what changed without one, for the log. */
static uint32_t sVramAnnounced[(VRAM_BYTES / VRAM_BLOCK + 31) / 32];
static uint32_t sUnannouncedBlocks, sUnannouncedLogged;
/* Of those, OBJ blocks (64-95): what the battle's kept slots and the voxel
 * field's cards would show late if trusted (CtrVideo_ObjVramSyncChanged). */
static uint32_t sUnannouncedObjBlocks;
/*
 * The blocks whose announcements are trusted this frame (VramTrustFrame),
 * and those that lost the trust: a block found changed with no announcement
 * while it was trusted is compared again whenever asked about, until the
 * trust of its kind ends (the battle over, the field left).
 */
#define VRAM_BLOCK_WORDS ((VRAM_BYTES / VRAM_BLOCK + 31) / 32)
static uint32_t sVramTrust[VRAM_BLOCK_WORDS], sVramDistrust[VRAM_BLOCK_WORDS];
static uint32_t sTrustLost;
/* The token at which a background palette last changed. */
static uint32_t sBgPaletteStamp;
/* The same per 16-colour bank of the BG palette, and for the renderer's tile
 * banks, whose palettes are their own: for the battle's kept scenery, which
 * a change to a bank none of its layers names leaves as it is (ScenePalettesHold).
 * sBgOtherStamp: a tile bank registered. sBankColourStamp: a registered bank's
 * colours followed the map's (UpdateTileBankPalettes), which only what is
 * drawn from a bank shows (BankColoursHold). */
static uint32_t sBgBankStamp[16], sBgOtherStamp, sBankColourStamp;
/* The same per colour of a bank, for a layer's cells (LayerColoursMoved). */
static uint32_t sBgColourStamp[16][16];
/* Bumped when the tile cache is emptied: every slot changes meaning. */
static uint32_t sCacheGeneration;

/* Eight words at a time, one branch for all eight: memcmp's byte loop cost
 * a millisecond a frame here. */
static bool BlockDiffers(const uint32_t *a, const uint32_t *b)
{
    for (unsigned i = 0; i < VRAM_BLOCK / 4; i += 8)
        if ((a[i] ^ b[i]) | (a[i + 1] ^ b[i + 1]) | (a[i + 2] ^ b[i + 2]) | (a[i + 3] ^ b[i + 3])
            | (a[i + 4] ^ b[i + 4]) | (a[i + 5] ^ b[i + 5]) | (a[i + 6] ^ b[i + 6]) | (a[i + 7] ^ b[i + 7]))
            return true;
    return false;
}

/* A writer the contract does not cover yet: its screen and block. */
static __attribute__((noinline, cold)) void UnannouncedBlock(unsigned b)
{
    ++sUnannouncedBlocks;
    if (b >= 0x10000 / VRAM_BLOCK) ++sUnannouncedObjBlocks;
    if (sUnannouncedLogged < 200)
    {
        ++sUnannouncedLogged;
        CtrLog_Write(CTR_LOG_VIDEO, "CONTRACT unannounced vram block=%u (%05x) cb2=%08lx frame=%lu total=%lu",
                     b, b * VRAM_BLOCK, (unsigned long)CtrProf_Scene(), (unsigned long)sStats.frames,
                     (unsigned long)sUnannouncedBlocks);
    }
}

/*
 * The comparison every block goes through, kept as lean as before the
 * announcements: the log out of line and the announcement read after the
 * comparison. With both inside, the function saved three more registers
 * and lost the paired loads of its loop - 0.13 ms a frame of layer work on
 * an Old 3DS (F2e run).
 */
/* A trusted block found changed without an announcement: the trust goes,
 * for it alone, and the miss is counted (VramTrustFrame's log). */
static __attribute__((noinline, cold)) void TrustLost(unsigned b)
{
    sVramTrust[b / 32] &= ~(1u << (b % 32));
    sVramDistrust[b / 32] |= 1u << (b % 32);
    ++sTrustLost;
}

static void SyncBlockSlow(unsigned b, uint32_t now)
{
    const uint32_t *block = (const uint32_t *)sMemory.vram + b * (VRAM_BLOCK / 4);
    uint32_t *shadow = sVramShadow + b * (VRAM_BLOCK / 4);

    sVramSynced[b] = now;
    if (BlockDiffers(block, shadow))
    {
        memcpy(shadow, block, VRAM_BLOCK);
        sVramStamp[b] = now;
        if (!(sVramAnnounced[b / 32] & (1u << (b % 32))))
        {
            UnannouncedBlock(b);
            if (sVramTrust[b / 32] & (1u << (b % 32)))
                TrustLost(b);
        }
    }
    sVramAnnounced[b / 32] &= ~(1u << (b % 32));
}

void CtrVideo_NotifyWrite(const void *dest, unsigned bytes)
{
    uintptr_t base = (uintptr_t)sMemory.vram, address = (uintptr_t)dest;

    if (base == 0 || bytes == 0 || address < base || address - base >= VRAM_BYTES)
        return;
    {
        unsigned first = (unsigned)(address - base) / VRAM_BLOCK;
        unsigned end = (unsigned)(address - base) + bytes;
        unsigned last = ((end < VRAM_BYTES ? end : VRAM_BYTES) - 1) / VRAM_BLOCK;

        for (unsigned b = first; b <= last; ++b)
            sVramAnnounced[b / 32] |= 1u << (b % 32);
    }
}

/* Block b's stamp, compared this frame if it was not yet. */
/*
 * Where the announcements are trusted (VramTrustFrame): a block nothing wrote
 * through an announced copy is taken as unchanged, without comparing it. On
 * the field, 2D (where the comparing was a third of the layer work) or
 * voxel, all of VRAM, and not while one of the field effects that store into
 * VRAM directly is up. In a battle, the OBJ blocks (64-95): the battle's
 * sprites, its info boxes and their text come by the BIOS copies, DMA and
 * decompressions the port announces (3ds_prof_wrap.c) -
 * HpTextIntoHealthboxObject and the healthbox's other text are CpuCopy32, a
 * transformed mon's picture DmaCopy32 - and the console's log of 0b3b36d91
 * had no unannounced block in 31,562 frames, battles of both kinds among
 * them (the comparing logs one with or without the trust). Its backgrounds
 * stay compared: the battle's animations write BG VRAM in more ways.
 */
#ifndef CTR_VRAM_TRUST
#define CTR_VRAM_TRUST 1
#endif
enum { VRAM_TRUST_NONE, VRAM_TRUST_ALL, VRAM_TRUST_OBJ };

static inline uint32_t SyncBlock(unsigned b)
{
    uint32_t now = sStats.frames + 1;

    if (sVramSynced[b] != now)
    {
        if (((sVramTrust[b / 32] & ~sVramAnnounced[b / 32]) >> (b % 32)) & 1)
            sVramSynced[b] = now;
        else
            SyncBlockSlow(b, now);
    }
    return sVramStamp[b];
}

/*
 * Once a frame, before VRAM is read, with the region trusted this frame
 * (VRAM_TRUST_*). Trusted, two blocks a frame of the region are still
 * compared: a write that came by no announced way is logged (CONTRACT),
 * takes that block's trust away (TrustLost) and is shown, late but shown -
 * round the field's 96 blocks in 48 frames, the battle's 32 OBJ blocks in
 * 16. Every block that becomes trusted is compared first, for what changed
 * while the announcements were not trusted. A block that lost the trust
 * stays without it until the trust of its kind ends: the battle's through
 * its menus (the bag, the party) until it is over, the field's when it is
 * left.
 */
#define VRAM_SWEEP_BLOCKS 2u

/* The token whose VramTrustFrame has run: from then to the end of that
 * frame's present nothing writes VRAM, so a block compared then holds for
 * the token (CtrVideo_ObjVramSyncChanged). */
static uint32_t sVramTrustToken;

static void VramTrustFrame(unsigned region)
{
    static unsigned sSweep, sRegion;
    static uint16_t sTrustFrames[3];
    uint32_t now = sStats.frames + 1;
    unsigned blocks = VRAM_BYTES / VRAM_BLOCK;
    unsigned first = region == VRAM_TRUST_OBJ ? 0x10000 / VRAM_BLOCK : 0;

    if (region > VRAM_TRUST_OBJ) region = VRAM_TRUST_NONE;
    sVramTrustToken = now;
    /* Through a battle (its menus too, where nothing is trusted) a block
     * keeps the trust it lost; past it, or leaving the field, it gets it
     * back - compared first, as every block that becomes trusted. */
    if (region != sRegion && !sBattleRequested)
        memset(sVramDistrust, 0, sizeof(sVramDistrust));
    sRegion = region;
    ++sTrustFrames[region];
    if (sStats.frames % 600 == 599)
    {
        if (sTrustFrames[VRAM_TRUST_ALL] || sTrustFrames[VRAM_TRUST_OBJ] || sTrustLost)
            CtrLog_Write(CTR_LOG_VIDEO, "VRAM trust in 600 frames: field %u, battle OBJ blocks %u, none %u; "
                         "blocks that lost it %lu, unannounced in all %lu (OBJ blocks %lu)",
                         (unsigned)sTrustFrames[VRAM_TRUST_ALL], (unsigned)sTrustFrames[VRAM_TRUST_OBJ],
                         (unsigned)sTrustFrames[VRAM_TRUST_NONE], (unsigned long)sTrustLost,
                         (unsigned long)sUnannouncedBlocks, (unsigned long)sUnannouncedObjBlocks);
        memset(sTrustFrames, 0, sizeof(sTrustFrames));
    }
    for (unsigned b = 0; b < blocks; ++b)
    {
        bool trusted = region != VRAM_TRUST_NONE && b >= first && !(sVramDistrust[b / 32] & (1u << (b % 32)));
        bool was = (sVramTrust[b / 32] >> (b % 32)) & 1;

        if (trusted && !was)
        {
            /* Compared before it is trusted: a change found then is the
             * untrusted frames', and costs no trust. */
            if (sVramSynced[b] != now)
                SyncBlockSlow(b, now);
            sVramTrust[b / 32] |= 1u << (b % 32);
        }
        else if (!trusted && was)
            sVramTrust[b / 32] &= ~(1u << (b % 32));
    }
    if (region == VRAM_TRUST_NONE)
        return;
    if (sSweep < first)
        sSweep = first;
    for (unsigned i = 0; i < VRAM_SWEEP_BLOCKS; ++i)
    {
        if (sVramSynced[sSweep] != now)
            SyncBlockSlow(sSweep, now);
        sSweep = sSweep + 1 < blocks ? sSweep + 1 : first;
    }
}

static void VramSyncRange(unsigned address, unsigned bytes)
{
    if (bytes == 0 || address >= VRAM_BYTES) return;
    if (bytes > VRAM_BYTES - address) bytes = VRAM_BYTES - address;
    for (unsigned b = address / VRAM_BLOCK; b <= (address + bytes - 1) / VRAM_BLOCK; ++b)
        SyncBlock(b);
}

/* Whether VRAM [address, address + bytes) changed after token t. */
static bool VramChangedAfter(unsigned address, unsigned bytes, uint32_t t)
{
    unsigned last;

    if (bytes == 0) return false;
    if (address >= VRAM_BYTES) return true;
    if (bytes > VRAM_BYTES - address) bytes = VRAM_BYTES - address;
    last = (address + bytes - 1) / VRAM_BLOCK;
    for (unsigned b = address / VRAM_BLOCK; b <= last; ++b)
        if (SyncBlock(b) > t) return true;
    return false;
}

/*
 * For the voxel field's cards (3ds_video.h): pure reads, never a comparison -
 * SyncBlock writes the shadow and the stamps and clears the announcements. A
 * block is taken as changed after `token` while a write announced to it is
 * pending (not compared yet), while its stamp is past the token, and while
 * it is not trusted and was not compared this frame (a change could be there
 * unseen). Anywhere in the frame; exact once VramTrustFrame has run (Present),
 * which compares what becomes trusted.
 */
uint32_t CtrVideo_VramToken(void)
{
    return sStats.frames + 1;
}

bool CtrVideo_ObjVramSyncChanged(unsigned offset, unsigned bytes, uint32_t token, bool *changed)
{
    unsigned first, last;
    uint32_t now = sStats.frames + 1;
    bool any = false;

    *changed = true;
    if (sVramTrustToken != now || bytes == 0 || offset >= 0x8000) return false;
    if (bytes > 0x8000 - offset) bytes = 0x8000 - offset;
    first = (0x10000 + offset) / VRAM_BLOCK;
    last = (0x10000 + offset + bytes - 1) / VRAM_BLOCK;
    /* Every block, not up to the first changed one: the caller records this
     * token for the whole range. */
    for (unsigned b = first; b <= last; ++b)
        if (SyncBlock(b) > token) any = true;
    *changed = any;
    return true;
}
