/*
 * Part of 3ds_video.c. Palette fades and uploads, the atlas's slots
 * (GetTileSlot), DrawSlot and the blending state (Blend).
 */

/*
 * A palette fade as a tint. The game fades by rewriting every colour each
 * frame (BlendPalette, src/util.c), and every colour of a layer texture
 * changing is every cell of it redrawn: the intro's four 256x512 layers,
 * 8192 tiles decoded and drawn a frame, 30 ms on an Old 3DS for as long as a
 * fade lasts. When the backgrounds' palette is exactly the unfaded one faded
 * towards one colour - black, white, the grey a battle transition flashes
 * (BlendPalettes towards RGB(11, 11, 11)) - the tiles keep the unfaded
 * colours and the fade is
 * drawn as a tint on the layers (Blend, LayerBrightness), which costs
 * nothing. Only an exact match counts, so any other palette effect is drawn
 * as before. Sprites keep the faded colours: they are a few tiles.
 */
extern uint16_t gPlttBufferUnfaded[];
static float sPaletteFade;
/* The colour it fades towards, GBA BGR555. */
static uint16_t sPaletteFadeColor;
/* Set while a layer texture is drawn: it holds the unfaded colours. */
static bool sInLayerTexture;

static uint16_t FadeColor(unsigned color, unsigned to, unsigned y)
{
    int r = color & 31, g = (color >> 5) & 31, b = (color >> 10) & 31;
    int tr = to & 31, tg = (to >> 5) & 31, tb = (to >> 10) & 31;

    r += ((tr - r) * (int)y) >> 4;
    g += ((tg - g) * (int)y) >> 4;
    b += ((tb - b) * (int)y) >> 4;
    return (uint16_t)(r | g << 5 | b << 10);
}

/*
 * The value (0-31) one channel of every background colour is faded towards
 * by y, or -1. Tried value by value: a wrong one fails on the first colours,
 * so this is cheap unless it matches.
 */
static int FadeChannelTarget(const uint16_t *now, const uint16_t *unfaded, unsigned shift, unsigned y)
{
    for (int to = 0; to < 32; ++to)
    {
        unsigned i = 0;

        for (; i < 256; ++i)
        {
            int u = (unfaded[i] >> shift) & 31, f = (now[i] >> shift) & 31;
            if (u + (((to - u) * (int)y) >> 4) != f) break;
        }
        if (i == 256) return to;
    }
    return -1;
}

/* The coefficient (1-16) the background colours are faded by, or 0. */
static unsigned DetectPaletteFade(uint16_t *color)
{
    const uint16_t *now = sMemory.palette, *unfaded = gPlttBufferUnfaded;

    if (!memcmp(now, unfaded, 512)) return 0;
    for (unsigned w = 0; w < 2; ++w)
        for (unsigned y = 1; y <= 16; ++y)
        {
            unsigned i = 0;

            while (i < 256 && FadeColor(unfaded[i], w ? 0x7fff : 0, y) == (now[i] & 0x7fff)) ++i;
            if (i == 256)
            {
                *color = w ? 0x7fff : 0;
                return y;
            }
        }
    /* Any other colour, channel by channel: the transitions' grey flash. */
    for (unsigned y = 1; y <= 16; ++y)
    {
        int r = FadeChannelTarget(now, unfaded, 0, y), g, b;

        if (r < 0 || (g = FadeChannelTarget(now, unfaded, 5, y)) < 0
            || (b = FadeChannelTarget(now, unfaded, 10, y)) < 0)
            continue;
        *color = (uint16_t)(r | g << 5 | b << 10);
        return y;
    }
    return 0;
}

/*
 * The banks' palettes as the current map's are shown (3ds_tile_banks.h): in a
 * uniform fade, which the renderer applies itself to colours decoded from the
 * unfaded palettes, their own unfaded colours; otherwise their colours taken
 * channel by channel through what took the current map's palettes 0-12 from
 * unfaded to shown - nothing, most of the time.
 */
static void UpdateTileBankPalettes(bool fading)
{
    const uint16_t *shown = sMemory.palette, *unfaded = gPlttBufferUnfaded;
    unsigned count = (CTR_TILE_BANK_FIRST_PALETTE + CTR_TILE_BANK_PALETTES) * 16;
    bool same = fading || !memcmp(shown, unfaded, count * sizeof(uint16_t));
    uint8_t lut[3][32];

    if (!same)
        CtrTileBank_BuildLut(unfaded, shown, count, lut);
    for (unsigned b = 0; b < CTR_TILE_BANKS; ++b)
    {
        if (!(sTileBanksLive & (1u << b))) continue;
        for (unsigned p = 0; p < CTR_TILE_BANK_PALETTES; ++p)
        {
            unsigned id = PAL_TILE_BANK + b * CTR_TILE_BANK_PALETTES + p;
            unsigned base = PAL_TILE_BANK_COLOURS + (b * CTR_TILE_BANK_PALETTES + p) * 16;
            const uint16_t *source = sTileBankPalettes[b] + p * 16;
            bool changed = false;

            for (unsigned index = 0; index < 16; ++index)
            {
                uint16_t colour = same ? source[index] & 0x7fff : CtrTileBank_ApplyLut(lut, source[index]);

                if (sPalette[base + index] == colour) continue;
                if (!changed)
                    memset(sPaletteChanges[id], 0, sizeof(sPaletteChanges[id]));
                changed = true;
                sPalette[base + index] = colour;
                sTexturePalette[base + index] = CtrVideo_RGBA5551(colour);
                sPaletteChanges[id][0] |= 1u << index;
            }
            if (changed)
            {
                ++sPaletteVersion[id];
                sBgPaletteStamp = sStats.frames + 1;
                sBankColourStamp = sStats.frames + 1;
            }
        }
    }
}

/*
 * A background colour drawn as transparent: the roulette paints the table's
 * colour (BG palette 5, colour 1) round its wheel and its grid, so it went on
 * the backdrop the Game Corner's felt replaces (CornerUnderlay) as flat
 * patches. -1 for none.
 */
static int sKeyedEntry = -1;

static void UpdatePalette(void)
{
    bool bgChanged = false, objChanged = false;
    unsigned fade = DetectPaletteFade(&sPaletteFadeColor);
    const uint16_t *bgSource = fade ? gPlttBufferUnfaded : sMemory.palette;
    int keyed = sCentred && sCentredScreen == CTR_CENTRED_ROULETTE ? 5 * 16 + 1 : -1;

    sPaletteFade = fade / 16.0f;
    if (keyed != sKeyedEntry)
    {
        /* Converted again below, with or without its colour. */
        if (sKeyedEntry >= 0) sPalette[sKeyedEntry] = ~bgSource[sKeyedEntry];
        if (keyed >= 0) sPalette[keyed] = ~bgSource[keyed];
        sKeyedEntry = keyed;
    }
    for (unsigned bank = 0; bank < 32; ++bank)
    {
        const uint16_t *source = bank < 16 ? bgSource : sMemory.palette;

        if (memcmp(sPalette + bank * 16, source + bank * 16, 32))
        {
            unsigned group = bank < 16 ? PAL_BG256 : PAL_OBJ256;
            if (bank < 16 ? !bgChanged : !objChanged)
                memset(sPaletteChanges[group], 0, sizeof(sPaletteChanges[group]));
            memset(sPaletteChanges[bank], 0, sizeof(sPaletteChanges[bank]));
            for (unsigned index = 0; index < 16; ++index)
            {
                unsigned p = bank * 16 + index;
                if (sPalette[p] == source[p]) continue;
                sPalette[p] = source[p];
                sTexturePalette[p] = (int)p == sKeyedEntry ? 0 : CtrVideo_RGBA5551(sPalette[p]);
                sPaletteChanges[bank][0] |= 1u << index;
                if (bank < 16) sBgColourStamp[bank][index] = sStats.frames + 1;
                unsigned entry = p & 255;
                sPaletteChanges[group][entry / 32] |= 1u << (entry & 31);
            }
            ++sPaletteVersion[bank];
            if (bank < 16) sBgBankStamp[bank] = sStats.frames + 1;
            if (bank < 16) bgChanged = true; else objChanged = true;
        }
    }
    if (bgChanged)
    {
        ++sPaletteVersion[PAL_BG256];
        sBgPaletteStamp = sStats.frames + 1;
    }
    if (objChanged) ++sPaletteVersion[PAL_OBJ256];
    /* The ext banks in use: no 256-colour bank holds them. */
    for (unsigned live = sExtInUse, ext = 0; live; live >>= 1, ++ext)
    {
        unsigned bank = PAL_EXT + ext, base = 512 + ext * 16;
        const uint16_t *source = sExtPltt + ext * 16;

        if (!(live & 1) || !memcmp(sPalette + base, source, 32)) continue;
        memset(sPaletteChanges[bank], 0, sizeof(sPaletteChanges[bank]));
        for (unsigned index = 0; index < 16; ++index)
        {
            if (sPalette[base + index] == source[index]) continue;
            sPalette[base + index] = source[index];
            sTexturePalette[base + index] = CtrVideo_RGBA5551(source[index]);
            sPaletteChanges[bank][0] |= 1u << index;
        }
        ++sPaletteVersion[bank];
    }
    if (sTileBanksLive && (bgChanged || sTileBanksChanged))
        UpdateTileBankPalettes(fade != 0);
    sTileBanksChanged = false;
}

/*
 * Why tiles were decoded into the atlas (phase F1's gate: a still scene
 * decodes nothing, and nothing is decoded that comes out as it was): new to
 * the cache or evicted, its bytes changed, or its palette. Totals per segment
 * of the profile (CtrVideo_DecodeCounts), with the decodes whose texels came
 * out identical to the slot's: those change nothing downstream.
 */
static uint32_t sDecodeNew, sDecodeBytes, sDecodePalette, sDecodeSame;
static uint32_t sDecodeFrames, sCellFrames, sDecodeFramesTiles, sCellsTotal;

void CtrVideo_DecodeCounts(uint32_t counts[8])
{
    counts[0] = sDecodeNew;
    counts[1] = sDecodeBytes;
    counts[2] = sDecodePalette;
    counts[3] = sDecodeSame;
    counts[4] = sDecodeFrames;
    counts[5] = sCellFrames;
    counts[6] = sCellsTotal;
    counts[7] = sDecodeFramesTiles;
    sDecodeNew = sDecodeBytes = sDecodePalette = sDecodeSame = 0;
    sDecodeFrames = sCellFrames = sCellsTotal = sDecodeFramesTiles = 0;
}

static bool TilePaletteChanged(const Tile *tile, unsigned paletteId)
{
    uint32_t version = sPaletteVersion[paletteId];
    if (tile->paletteVersion == version) return false;
    /* The last change mask is sufficient only for the next version. Tiles
     * returning after multiple palette changes are conservatively rebuilt. */
    if (tile->paletteVersion + 1 != version) return true;
    bool color256 = paletteId == PAL_BG256 || paletteId == PAL_OBJ256;

    for (unsigned i = 0; i < (color256 ? 8u : 1u); ++i)
        if (tile->colors[i] & sPaletteChanges[paletteId][i]) return true;
    return false;
}

/*
 * Resolves a tile to its atlas slot, uploading it if its bytes or its palette
 * changed. Returns -1 for a tile that is entirely transparent, which draws
 * nothing. The caller builds the subtexture: a slot is a fixed square of the
 * atlas, so that is four multiplications and no shared state.
 */
static int GetTileSlot(unsigned address, unsigned bank, bool color256)
{
    bool fromBank = address >= TILE_BANK_BASE;

    if (fromBank ? color256 || address + 32 > TILE_BANK_BASE + CTR_TILE_BANKS * CTR_TILE_BANK_BYTES
                 : address + (color256 ? 64 : 32) > 0x18000)
    {
        Error(1, "tile outside logical VRAM");
        address = 0;
        fromBank = false;
    }
    /* A bank's tiles stand in for VRAM past its end (sTileBankTiles). */
    const uint8_t *source = fromBank
        ? sTileBankTiles[(address - TILE_BANK_BASE) / CTR_TILE_BANK_BYTES] + (address - TILE_BANK_BASE) % CTR_TILE_BANK_BYTES
        : sMemory.vram + address;
    unsigned paletteId = color256 ? PAL_BG256 + (bank >= 16) : bank;
    uint32_t key = (address / 32) * PAL_IDS + paletteId + 1;
    unsigned hash = (key * 2654435761u) & (HASH_COUNT - 1);
    while (sHash[hash] && sTiles[sHash[hash] - 1].key != key)
        hash = (hash + 1) & (HASH_COUNT - 1);
    if (!sHash[hash])
    {
        if (sUsed == CACHE_COUNT)
            CtrPlatform_Fatal("VIDEO tile cache exhausted in a single frame");
        sHash[hash] = ++sUsed;
        sTiles[sUsed - 1] = (Tile){.key = key};
    }
    unsigned slot = sHash[hash] - 1;
    Tile *tile = &sTiles[slot];
    if (!tile->valid || tile->checked != sStats.frames + 1)
    {
        unsigned bytes = color256 ? 64 : 32;
        /* Compared now whatever follows: the tile records this token, so its
         * block must be (see SyncBlock). */
        uint32_t stamp = fromBank ? sTileBankStamp[(address - TILE_BANK_BASE) / CTR_TILE_BANK_BYTES]
                                  : SyncBlock(address / VRAM_BLOCK);
        /* A tile's bytes sit in one block: unchanged since it was last
         * checked, they need no comparing. */
        bool fresh = !tile->valid, palette = !fresh && TilePaletteChanged(tile, paletteId);

        if (fresh || palette
            || (stamp > tile->checked
                && memcmp(tile->bytes, source, bytes)))
        {
            if (fresh) ++sDecodeNew;
            else if (palette) ++sDecodePalette;
            else ++sDecodeBytes;
            memcpy(tile->bytes, source, bytes);
            unsigned paletteBase = color256 ? (bank >= 16 ? 256 : 0)
                                 : bank >= PAL_TILE_BANK ? PAL_TILE_BANK_COLOURS + (bank - PAL_TILE_BANK) * 16
                                 : bank >= PAL_EXT ? 512 + (bank - PAL_EXT) * 16 : bank * 16;
            uint16_t *dest = (uint16_t *)sAtlas.data + slot * 64;
            uint16_t before[64];

            /* What the slot held: a decode that comes out the same - a
             * palette changed and changed back, the cache's conservative
             * rebuild - keeps the tile's serial, so no layer cell holding it
             * is drawn again. */
            if (!fresh)
                memcpy(before, dest, sizeof(before));
            memset(tile->colors, 0, sizeof(tile->colors));
            if (!color256 && sMortonPairs)
            {
                /*
                 * Hundreds of these a frame when a map's tiles change (a
                 * warp, a connection to another tileset): the 16 colours in
                 * hand with index 0 clear, a byte's two pixels as one word,
                 * and the colours used as one mask.
                 */
                const uint16_t *source = sTexturePalette + paletteBase;
                uint32_t lut[16], used = 0;
                uint32_t *dest32 = (uint32_t *)dest;

                lut[0] = 0;
                for (unsigned i = 1; i < 16; ++i)
                    lut[i] = source[i];
                for (unsigned pair = 0; pair < 32; ++pair)
                {
                    unsigned byte = tile->bytes[pair], lo = byte & 15, hi = byte >> 4;

                    dest32[sMortonPair[pair]] = lut[lo] | lut[hi] << 16;
                    used |= (1u << lo) | (1u << hi);
                }
                used &= ~1u;
                tile->colors[0] = used;
                tile->visible = used != 0;
            }
            else
            {
                tile->visible = false;
                for (unsigned pixel = 0; pixel < 64; ++pixel)
                {
                    unsigned index = color256 ? tile->bytes[pixel]
                        : (tile->bytes[pixel / 2] >> ((pixel & 1) * 4)) & 15;
                    dest[sMorton[pixel]] = index ? sTexturePalette[paletteBase + index] : 0;
                    if (index)
                    {
                        tile->visible = true;
                        tile->colors[index / 32] |= 1u << (index & 31);
                    }
                }
            }
            /* No flush per tile (a GSP call each, hundreds in a frame of
             * palette animation): the atlas reaches memory with the rest of
             * linear memory, flushed before the GPU is given each list -
             * at C3D_FrameEnd, or by the wrappers once it is started early.
             * Written after that start, the texture cache is dropped before
             * the next draw (TexelsWritten): the GPU may hold lines of this
             * slot's memory fetched for a neighbour earlier in the frame. */
            TexelsWritten();
            tile->valid = true;
            if (!fresh && memcmp(before, dest, sizeof(before)) == 0)
                ++sDecodeSame;
            else
            {
                tile->serial = ++sTileSerial;
                ++sStats.uploads;
            }
        }
        tile->paletteVersion = sPaletteVersion[paletteId];
        tile->checked = sStats.frames + 1;
    }
    return tile->visible ? (int)slot : -1;
}

/*
 * How far inside its atlas slot a tile is read, in texture units. An affine
 * sprite is drawn tile by tile with the same matrix, and a pixel centre falling
 * exactly on the line between two tiles (Psychic scaling its target, pa 244)
 * read the slot next door in the atlas, some other tile: a stripe across the
 * sprite. The GBA reads one bitmap there. 1/16 of a texel keeps the read in the
 * tile and moves nothing else.
 */
static float sTileInset;

static void DrawSlotTinted(int slot, float x, float y, bool flipX, bool flipY, const C2D_ImageTint *tint)
{
    unsigned tileX = ((unsigned)slot % (ATLAS_SIZE / 8)) * 8;
    unsigned tileY = ((unsigned)slot / (ATLAS_SIZE / 8)) * 8;
    Tex3DS_SubTexture sub = {8, 8, tileX / (float)ATLAS_SIZE,
        1.0f - tileY / (float)ATLAS_SIZE,
        (tileX + 8) / (float)ATLAS_SIZE, 1.0f - (tileY + 8) / (float)ATLAS_SIZE};

    sub.left += sTileInset;
    sub.right -= sTileInset;
    sub.top -= sTileInset;
    sub.bottom += sTileInset;
    if (++sStats.tiles >= DRAW_BUDGET)
    {
        Error(2, "GPU geometry budget exceeded");
        return;
    }
    if (!C2D_DrawImageAt((C2D_Image){&sAtlas, &sub}, x, y, 0, tint,
                         flipX ? -1 : 1, flipY ? -1 : 1))
        Error(3, "Citro2D geometry submission failed");
}

static void DrawSlot(int slot, float x, float y, bool flipX, bool flipY)
{
    DrawSlotTinted(slot, x, y, flipX, flipY, &sTint);
}

static bool sDayNightObject;
static bool sDayNightEnv;
/*
 * A background that blends, in front of a group of sprites, drawn into the
 * group's slot over its sprites (SlotsCompose): the GBA's EVA x layer + EVB x
 * sprite, in two passes of the layer alone. SLOT_BLEND_UNDER scales what the
 * slot holds under the layer's pixels by EVB; SLOT_BLEND_OVER adds EVA x the
 * layer where the slot holds a sprite (its alpha, one bit), nothing where it
 * holds nothing - the scenery under the slot has the layer blended already.
 */
enum { SLOT_BLEND_NONE, SLOT_BLEND_UNDER, SLOT_BLEND_OVER };
static unsigned sSlotBlendPass;

static void Blend(unsigned layer, bool effects, bool semiTransparent)
{
    /*
     * This flushes the batch and rewrites the blend state, so it costs a GPU
     * draw call. The registers it reads are fixed for the frame, and nothing
     * between two calls touches the blend state, so asking for the same
     * configuration again is a no-op: an overworld frame with forty sprites
     * asked for it forty times.
     */
    const CtrDayNightLight *light = CtrDayNight_Get();
    bool night = (light->rgb[0] != 1.0f || light->rgb[1] != 1.0f || light->rgb[2] != 1.0f)
              && !sInLayerTexture && !sStage && !sCentred && !sBattle && CtrGame_IsOverworld()
              && ((layer >= 1 && layer <= 3) || (layer == 4 && sDayNightObject));
    unsigned pass = sInLayerTexture ? SLOT_BLEND_NONE : sSlotBlendPass;
    unsigned key = layer | (effects << 8) | (semiTransparent << 9) | (night << 10) | (pass << 11);

    if (key == sBlendKey) return;
    sBlendKey = key;

    C2D_Flush();
    if (sDayNightEnv)
    {
        C3D_TexEnvInit(C3D_GetTexEnv(4));
        C3D_TexEnvInit(C3D_GetTexEnv(5));
        sDayNightEnv = false;
    }
    C3D_AlphaTest(true, GPU_GREATER, 0);
    if (pass != SLOT_BLEND_NONE)
    {
        unsigned control = Reg(0x50);
        bool blends = effects && ((control >> 6) & 3) == 1 && (control & (1u << layer));
        unsigned eva = Min(Reg(0x52) & 31, 16), evb = Min((Reg(0x52) >> 8) & 31, 16);

        C2D_PlainImageTint(&sTint, C2D_Color32(0, 0, 0, 255), pass == SLOT_BLEND_OVER ? 1.0f - eva / 16.0f : 0.0f);
        if (!blends)
        {
            /* Where the window turns effects off, the layer as it is. */
            if (pass == SLOT_BLEND_UNDER)
                C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ZERO, GPU_ONE);
            else
                C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ZERO, GPU_ONE, GPU_ZERO, GPU_ONE);
        }
        else if (pass == SLOT_BLEND_UNDER)
        {
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ZERO, GPU_CONSTANT_COLOR, GPU_ZERO, GPU_ONE);
            C3D_BlendingColor(C2D_Color32(evb * 255 / 16, evb * 255 / 16, evb * 255 / 16, 255));
        }
        else
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_DST_ALPHA, GPU_ONE, GPU_ZERO, GPU_ONE);
        return;
    }
    if (sObjWinPass != OBJWIN_NONE)
    {
        /* The OBJ window's surface (ObjWindowCompose) keeps a coverage
         * count in alpha: half from the window's sprites, written alone
         * and the same however many overlap, then doubled by whatever a
         * layer draws. Only both together reach the threshold the surface
         * is drawn with (ObjWindowDraw). Colours as they are, untinted. */
        C2D_PlainImageTint(&sTint, C2D_Color32(0, 0, 0, 255), 0.0f);
        if (sObjWinPass == OBJWIN_MASK)
        {
            C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALPHA);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ZERO, GPU_ONE, GPU_CONSTANT_ALPHA, GPU_ZERO);
            C3D_BlendingColor(C2D_Color32(0, 0, 0, 0x80));
        }
        else
        {
            C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_DST_ALPHA, GPU_ONE);
        }
        return;
    }
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA,
                  GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ZERO);
    unsigned control = Reg(0x50), effect = (control >> 6) & 3;
    /* The palette fade (UpdatePalette) is on a background whatever the
     * windows say; the brightness effect of BLDY replaces it below. */
    bool paletteTint = sPaletteFade > 0 && !sInLayerTexture && (layer < 4 || layer == 5);
    uint32_t fadeTo = CtrVideo_RGBA8(sPaletteFadeColor, true);

    C2D_PlainImageTint(&sTint, C2D_Color32(fadeTo >> 24, fadeTo >> 16, fadeTo >> 8, 255),
                       paletteTint ? sPaletteFade : 0);
    if (night)
    {
        C3D_TexEnv *env = C3D_GetTexEnv(4);

        C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_CONSTANT);
        C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
        C3D_TexEnvColor(env, C2D_Color32((unsigned)(light->rgb[0] * 255 + 0.5f),
                                       (unsigned)(light->rgb[1] * 255 + 0.5f),
                                       (unsigned)(light->rgb[2] * 255 + 0.5f), 255));
        float fade = paletteTint ? sPaletteFade : 0.0f;
        uint32_t color = C2D_Color32(fadeTo >> 24, fadeTo >> 16, fadeTo >> 8, (unsigned)(fade * 255));

        if (effects && (control & (1u << layer)) && effect >= 2)
        {
            unsigned channel = effect == 2 ? 255 : 0;

            fade = Min(Reg(0x54), 16) / 16.0f;
            color = C2D_Color32(channel, channel, channel, (unsigned)(fade * 255));
        }
        float fadeRgb[3] = {(color & 255u) / 255.0f, ((color >> 8) & 255u) / 255.0f,
                            ((color >> 16) & 255u) / 255.0f};
        CtrDayNightLight moonLight = *light;

        if (!paletteTint)
            moonLight.glow *= 1.0f - sPaletteFade;
        CtrDayNightBlend moon = CtrDayNight_Moonlight(&moonLight, fade, fadeRgb);

        color = C2D_Color32((unsigned)(moon.rgb[0] * 255.0f + 0.5f),
                           (unsigned)(moon.rgb[1] * 255.0f + 0.5f),
                           (unsigned)(moon.rgb[2] * 255.0f + 0.5f),
                           (unsigned)(moon.amount * 255.0f + 0.5f));
        env = C3D_GetTexEnv(5);
        C3D_TexEnvSrc(env, C3D_RGB, GPU_CONSTANT, GPU_PREVIOUS, GPU_CONSTANT);
        C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
        C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
        C3D_TexEnvColor(env, color);
        sDayNightEnv = true;
        C2D_PlainImageTint(&sTint, C2D_Color32(0, 0, 0, 255), 0);
    }
    /* A semi-transparent sprite blends even where the window turns colour
     * effects off: the overworld keeps WIN0 over the whole screen without
     * them, and its fog and clouds are still see-through on the GBA. */
    if (!effects && !semiTransparent) return;
    if ((effect == 1 && (control & (1u << layer))) || semiTransparent)
    {
        /* GPU implements independently clamped EVA/EVB (not 1-EVA).
         * Valid for the title's BG1 over BG0/backdrop; arbitrary interleaved
         * target-2 masks are a phase-6 per-pixel effect, reported below. */
        if ((control >> 8) & 63)
        {
            unsigned eva = Min(Reg(0x52) & 31, 16) * 255 / 16;
            unsigned evb = Min((Reg(0x52) >> 8) & 31, 16) * 255 / 16;
            /*
             * Over the voxel world the layers it stands in for are left out,
             * so where nothing else of the scene is under this one the
             * blend's second target is transparent black, and the scene
             * surface keeps one bit of alpha: Surf's wave at its start
             * (EVA 0-1, EVB 16) came out as a black wave over the world
             * where the GBA shows the terrain all but untouched. The alpha
             * rounded instead: while this layer is the lesser part of the
             * blend, what it is drawn over keeps its alpha - the world shows
             * through, as the terrain would; from half on, as before.
             */
            bool underWorld = sBattleWorld && eva < evb;

            /* Straight onto an eye the world is what it is drawn over, where
             * the scene surface it stands for has transparent black: from
             * half on that second target is nothing, and below half the
             * layer is not drawn at all (LayerLeftToWorld). */
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_CONSTANT_COLOR,
                          sLayerOnEye ? GPU_ZERO : GPU_CONSTANT_ALPHA, underWorld ? GPU_ZERO : GPU_ONE,
                          underWorld ? GPU_ONE : GPU_ZERO);
            C3D_BlendingColor(C2D_Color32(eva, eva, eva, evb));
        }
    }
    else if ((control & (1u << layer)) && effect >= 2)
    {
        unsigned c = effect == 2 ? 255 : 0;
        if (!night)
            C2D_PlainImageTint(&sTint, C2D_Color32(c, c, c, 255), Min(Reg(0x54), 16) / 16.0f);
    }
}
