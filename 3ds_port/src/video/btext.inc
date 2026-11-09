/*
 * Part of 3ds_video.c. BattleWorldEye, and the battle's text pass with what
 * keeps it (BattleTextCompose, BattleTextKeep).
 */

/*
 * The voxel battle's world for one eye: its finished picture (`world`,
 * RenderBattleWorld) copied - not blended: the blur, glow and shadows left
 * its alpha below one, which blended came out as black shadows and a dark
 * band - BATTLE_BACK_DEPTH behind the screen in whole pixels, its edge
 * columns as they are under where the displaced edge leaves none.
 */
static void BattleWorldEye(C3D_RenderTarget *target, C3D_Tex *world)
{
    const Tex3DS_SubTexture whole = {CTR_GAME_WIDTH, CTR_GAME_HEIGHT, 0, 1,
        CTR_GAME_WIDTH / (float)world->width, 1 - CTR_GAME_HEIGHT / (float)world->height};
    /* Stretched across by the shift, so that both edges stay covered: the
     * strip the shift uncovered used to be filled with the picture's edge
     * columns, which stood out as a line down the screen's side whenever
     * the picture's edge held something dark (a map's end, a wall). A few
     * per cent across, nothing at the middle where the Pokémon stand. */
    float shift = roundf(-sEyeParallax * BATTLE_BACK_DEPTH), reach = fabsf(shift);
    float scaleX = (CTR_GAME_WIDTH + 2.0f * reach) / CTR_GAME_WIDTH;

    BlendForget();
    SceneOn(target);
    C2D_ViewReset();
    C2D_Flush();
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    /* Nearest: the picture stays pixel-sharp, a column doubled now and then. */
    if (world == &sSceneTex) SceneTexFilter(GPU_NEAREST);
    C2D_DrawImageAt((C2D_Image){world, &whole}, shift - reach, 0, 0, NULL, scaleX, 1);
    if (world == &sSceneTex) SceneTexFilter(GPU_LINEAR);
    C2D_Flush();
    BlendForget();
}

/*
 * Rows of the logical surface drawn transparent, as far as the 3D battle
 * samples them: the picture's 400x240 (RenderBattleStereo) and the edges'
 * BATTLE_EDGE_MARGIN each side (BattleEdgesPlace), 416x240 of its 512x256.
 * Past that a frame may leave what it likes: every other route fills the
 * whole surface before it reads it. nocache.txt bit 33554432: the whole
 * surface's width and height, as before (A/B frames).
 */
static void BattleLogicalClear(int top, int bottom)
{
#ifdef CTR_AUDIT_BITS
    if (CtrDumpOff() & 33554432u)
    {
        DrawnClear(sLogical, 0, 0, top, 512, (bottom == CTR_GAME_HEIGHT ? 256 : bottom) - top);
        return;
    }
#endif
    DrawnClear(sLogical, 0, 0, top, CTR_GAME_WIDTH + 2 * BATTLE_EDGE_MARGIN, bottom - top);
}

/* What the scene does not hold: the text box's layers, composed as they are. */
VIDEO_PIECE void BattleBoxCompose(void)
{
    sLayerExclude = 63 & ~(1u | 32u | BattleBoxLayers());
    if (!(Reg(0) & 128)) Compose();
    sLayerExclude = 0;
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
}

/* The text box (BG0) alone into the logical surface, cleared transparent. */
static void BattleTextCompose(int keepRows)
{
    sTextAbove = false;
    BattleLogicalClear(keepRows, CTR_GAME_HEIGHT);
    SceneOn(sLogical);
    Blend(5, false, false);
    BattleBoxCompose();
}

/*
 * The text box of the 2D battle with the 3D slider up, kept in the logical
 * surface's rows from CTR_BATTLE_Y down from one frame to the next while
 * nothing it is made of changes - most of a battle the message stands
 * still, and composing it was 0.6-1.0 ms a frame on an Old 3DS (bs.text,
 * port.log 2026-11-01). As the edges are kept above those rows
 * (BattleEdgesKeep): only while the frame before took this same path, which
 * draws nothing else there - the text pass is the only one into those rows
 * between two such frames (the eyes are put together on the screens), and
 * the voxel battle draws its world into the whole surface every frame, so it
 * never keeps it. Its key (BattleTextKey) is everything the pass reads:
 * - DISPCNT, BG0's registers and those of the layers boxed with it
 *   (BattleBoxLayers); the windows' masks for those layers and the effects,
 *   and their rectangles unless the masks are the same in every window;
 *   BLDCNT, BLDY, MOSAIC, and BLDALPHA (by line too) when those layers blend;
 * - the view and clip the pass composes at;
 * - the palette banks those layers' maps name and bank 0 (the panel's fade,
 *   BoxFadeTint), faded and unfaded both, and the fade drawn as a tint;
 * - the HD box: where it is (BoxOffset), whether it is hidden or sliding;
 * - the version of the HD box's pictures (sBoxTexVersion) and whether they
 *   exist, as the pass left them - it fills them itself;
 * and, compared by their stamps since the pass, the layers' maps and the
 * characters those name (one tile past them: the box's middle is the tile
 * after the one the map names, DrawBattleText), and the tile banks.
 * Anything else - the windows line by line, a scanline register, an OBJ
 * window with one of those layers among its own (BattleTextWindowed) -
 * composes it as before, counted. nocache.txt bit 2048 turns it off in audit
 * builds.
 */
enum { TEXT_WHOLE_KEY, TEXT_WHOLE_PICTURES, TEXT_WHOLE_STAMP, TEXT_WHOLE_VRAM, TEXT_WHOLE_FADE, TEXT_WHOLE_PATH,
       TEXT_WHOLE_COUNT };
/*
 * As the scenery's (SceneKeyWhy), the key's terms hashed by kind beside it, to
 * explain a key that came out different; the key is the whole hash. Counted
 * per 600 frames by part (a frame may count in several), and this frame's
 * reasons as bits for its record (CTR_TEXT_WHY_*).
 */
enum { TEXT_PART_REG, TEXT_PART_VIEW, TEXT_PART_PALETTE, TEXT_PART_BOX, TEXT_PARTS };
_Static_assert((int)TEXT_PART_REG == (int)CTR_TEXT_WHY_REG && (int)TEXT_PART_BOX == (int)CTR_TEXT_WHY_BOX,
               "the parts' bits");
#define TEXT_WHY(bit) (1u << CTR_TEXT_WHY_##bit)
typedef struct { uint32_t part[TEXT_PARTS]; } TextKeyParts;
static TextKeyParts sTextKeyLast;
static unsigned sTextAgain[TEXT_PARTS];
static uint16_t sTextWhy;
static struct
{
    bool held, above, shown;
    uint32_t key, token, frame, pictures;
    TextKeyParts parts;
    struct { unsigned map, mapBytes, start, bytes; } vram[2];
} sTextStore;
static unsigned sTextKept, sTextWhole[TEXT_WHOLE_COUNT];

/* The layers the text pass draws: BG0 and those boxed with it. */
static unsigned BattleTextLayers(void)
{
    return 1u | BattleBoxLayers();
}

/*
 * The OBJ window reaches the text pass only through ObjWindowDraw, over a
 * layer of the pass among those the window adds, the sprites shaping it. A
 * battle's window never adds BG0 (ObjWindowCompose); a layer boxed with it
 * could be. Off those the pass neither draws nor reads the window, and the
 * masks and effects it does read are in its key. nocache.txt bit 16777216:
 * any window, as before (A/B frames).
 */
static bool BattleTextWindowed(void)
{
    if (!sObjWin.ready) return false;
#ifdef CTR_AUDIT_BITS
    if (CtrDumpOff() & 16777216u) return true;
#endif
    return (sObjWin.layers & BattleTextLayers()) != 0;
}

/* The VRAM a text layer's picture is made of: its map, and the characters
 * from the lowest tile it names to one past the highest. */
static void BattleTextVram(unsigned bg, unsigned *map, unsigned *mapBytes, unsigned *start, unsigned *bytes)
{
    unsigned chars, charBytes, control = Reg(8 + bg * 2), lo = ~0u, hi = 0;
    unsigned tileBytes = (control & 0x80) ? 64 : 32;

    SceneLayerVram(bg, map, mapBytes, &chars, &charBytes);
    if (*map + *mapBytes > 0x10000) *mapBytes = 0x10000 - *map;
    for (unsigned i = 0; i + 1 < *mapBytes; i += 2)
    {
        unsigned tile = (sMemory.vram[*map + i] | sMemory.vram[*map + i + 1] << 8) & 0x3ff;

        if (tile < lo) lo = tile;
        if (tile > hi) hi = tile;
    }
    *start = *bytes = 0;
    if (lo > hi || chars + lo * tileBytes >= 0x10000) return;
    *start = chars + lo * tileBytes;
    *bytes = (hi - lo + 2) * tileBytes;
    if (*start + *bytes > 0x10000) *bytes = 0x10000 - *start;
}

static uint32_t BattleTextKey(void)
{
    unsigned display = Reg(0), layers = BattleTextLayers(), control = Reg(0x50), in = Reg(0x48), out = Reg(0x4a);
    unsigned bits = layers | 32u, banks = 1u;
    bool uniform = (!(display & 0x2000) || (in & bits) == (out & bits))
                && (!(display & 0x4000) || ((in >> 8) & bits) == (out & bits))
                && (!(display & 0x8000) || ((out >> 8) & bits) == (out & bits));
    uint32_t h = 0x811c9dc5u, *part = sTextKeyLast.part;
    union { float f; uint32_t u; } v;
/* A term into the key and into its kind's part (BattleTextKeep). */
#define TEXT_TERM(kind, x) (h = SceneCacheMix(h, (uint32_t)(x)), \
                            part[TEXT_PART_##kind] = SceneCacheMix(part[TEXT_PART_##kind], (uint32_t)(x)))
#define TEXT_FLOAT(kind, x) (v.f = (x), TEXT_TERM(kind, v.u))

    for (unsigned i = 0; i < TEXT_PARTS; ++i)
        part[i] = 0x811c9dc5u;
    TEXT_TERM(REG, display);
    TEXT_TERM(REG, layers);
    for (unsigned bg = 0; bg < 4; ++bg)
        if (layers & (1u << bg))
        {
            TEXT_TERM(REG, Reg(8 + bg * 2));
            TEXT_TERM(REG, Reg(0x10 + bg * 4));
            TEXT_TERM(REG, Reg(0x12 + bg * 4));
            banks |= LayerBanks(bg);
        }
    TEXT_TERM(REG, in & (bits | bits << 8));
    TEXT_TERM(REG, out & (bits | bits << 8));
    if (!uniform)
    {
        for (unsigned offset = 0x40; offset < 0x48; offset += 2) TEXT_TERM(REG, Reg(offset));
        for (unsigned offset = 0x56; offset < 0x5e; offset += 2) TEXT_TERM(REG, Reg(offset));
    }
    TEXT_TERM(REG, control);
    TEXT_TERM(REG, Reg(0x54));
    TEXT_TERM(REG, Reg(0x4c));
    if (((control >> 6) & 3) == 1 && (control & layers))
    {
        TEXT_TERM(REG, Reg(0x52));
        if (sLineAlphaOn) TEXT_TERM(REG, sLineAlphaVersion);
    }
    /* The view, the clip and the passes Blend keys by. */
    TEXT_FLOAT(VIEW, sZoom);
    TEXT_FLOAT(VIEW, sOffX);
    TEXT_FLOAT(VIEW, sOffY);
    TEXT_FLOAT(VIEW, sLayerOrigin);
    TEXT_FLOAT(VIEW, sParallax);
    TEXT_FLOAT(VIEW, sShiftZoom);
    TEXT_TERM(VIEW, sViewX);
    TEXT_TERM(VIEW, sViewY);
    TEXT_TERM(VIEW, sTargetW);
    TEXT_TERM(VIEW, sTargetH);
    TEXT_TERM(VIEW, sClipX0);
    TEXT_TERM(VIEW, sClipY0);
    TEXT_TERM(VIEW, sClipX1);
    TEXT_TERM(VIEW, sClipY1);
    TEXT_TERM(VIEW, sScissored | sFieldUi << 1 | sTransitionCompose << 2 | sInLayerTexture << 3);
    TEXT_TERM(VIEW, sPriorityMask);
    TEXT_TERM(VIEW, sObjWinPass | sSlotBlendPass << 8);
    /* The palettes, faded and unfaded, and the fade drawn as a tint. */
    TEXT_FLOAT(PALETTE, sPaletteFade);
    TEXT_TERM(PALETTE, sPaletteFadeColor);
    for (unsigned b = 0; b < 16; ++b)
        if (banks & (1u << b))
            for (unsigned i = 0; i < 16; i += 2)
            {
                TEXT_TERM(PALETTE, sMemory.palette[b * 16 + i] | (uint32_t)sMemory.palette[b * 16 + i + 1] << 16);
                TEXT_TERM(PALETTE, gPlttBufferUnfaded[b * 16 + i] | (uint32_t)gPlttBufferUnfaded[b * 16 + i + 1] << 16);
            }
    /* The HD box (its pictures: BattleTextPictures). */
    TEXT_FLOAT(BOX, BoxOffset());
    TEXT_TERM(BOX, sBoxHidden | (CtrBattleHud_Ready() << 1));
    TEXT_TERM(BOX, sBoxSlide);
#undef TEXT_TERM
#undef TEXT_FLOAT
    return h;
}

/* The HD box's pictures as they are: their version, and whether they exist. */
static uint32_t BattleTextPictures(void)
{
    return SceneCacheMix(sBoxTexVersion, sBoxTexFailed | ((sBoxTex.data != NULL) << 1)
                                         | ((sTextTex.data != NULL) << 2));
}

/* Whether the text the frame before left in the logical surface is this
 * frame's (see sTextStore), `key` being this frame's (BattleTextKey, before
 * the pass); counts the frames that compose it by why. */
static bool BattleTextKeep(bool flat, uint32_t key)
{
    unsigned layers = BattleTextLayers();
    bool fading, stamped;

#ifdef CTR_AUDIT_BITS
    if (CtrDumpOff() & 2048) return false;
#endif
    if (!flat || !sTextStore.held || sTextStore.frame + 1 != sStats.frames || BattleTextWindowed() || sRegLine
        || sLineWindows.windows)
    {
        ++sTextWhole[TEXT_WHOLE_PATH];
        sTextWhy |= TEXT_WHY(PATH);
        return false;
    }
    stamped = sBgOtherStamp > sTextStore.token || !BankColoursHold(layers, sTextStore.token);
    if (key != sTextStore.key || BattleTextPictures() != sTextStore.pictures || stamped
        || sTilemapBankStamp > sTextStore.token)
    {
        unsigned control = Reg(0x50), why = 0;

        fading = sPaletteFade > 0.0f || memcmp(sMemory.palette, gPlttBufferUnfaded, 32) != 0
              || (((control >> 6) & 3) >= 2 && (control & layers));
        if (key != sTextStore.key)
            for (unsigned i = 0; i < TEXT_PARTS; ++i)
                if (sTextKeyLast.part[i] != sTextStore.parts.part[i])
                {
                    why |= 1u << i;
                    ++sTextAgain[i];
                }
        if (BattleTextPictures() != sTextStore.pictures) why |= TEXT_WHY(PICTURES);
        if (stamped) why |= TEXT_WHY(BG_STAMP);
        if (sTilemapBankStamp > sTextStore.token) why |= TEXT_WHY(BANK_STAMP);
        sTextWhy |= why | (fading ? TEXT_WHY(FADE) : 0);
        /* One each: the first of them, in the order they are looked at. */
        ++sTextWhole[fading ? TEXT_WHOLE_FADE : key != sTextStore.key ? TEXT_WHOLE_KEY
                     : why & TEXT_WHY(PICTURES) ? TEXT_WHOLE_PICTURES : TEXT_WHOLE_STAMP];
        return false;
    }
    for (unsigned bg = 0, k = 0; bg < 4 && k < 2; ++bg)
        if (layers & (1u << bg))
        {
            if (VramChangedAfter(sTextStore.vram[k].map, sTextStore.vram[k].mapBytes, sTextStore.token)
                || VramChangedAfter(sTextStore.vram[k].start, sTextStore.vram[k].bytes, sTextStore.token))
            {
                ++sTextWhole[TEXT_WHOLE_VRAM];
                sTextWhy |= TEXT_WHY(VRAM);
                return false;
            }
            ++k;
        }
    return true;
}

/* After this frame's text pass (`kept`: none, the text kept): what the next
 * frame keeps it by, `key` the key taken before the pass; with `held` false,
 * nothing to keep. */
static void BattleTextStore(bool kept, bool held, uint32_t key)
{
    unsigned layers = BattleTextLayers();

    if (kept)
    {
        sTextAbove = sTextStore.above;
        if (sTextStore.shown) sBoxShown = sStats.frames;
        sTextStore.frame = sStats.frames;
        ++sTextKept;
        sTextWhy |= TEXT_WHY(KEPT);
        return;
    }
    sTextStore.held = false;
    if (!held || BattleTextWindowed() || sRegLine || sLineWindows.windows) return;
    /* What the pass was made of, compared now: a change and a change back
     * before the next look is still a change (see SceneCacheSync). */
    for (unsigned bg = 0, k = 0; bg < 4 && k < 2; ++bg)
        if (layers & (1u << bg))
        {
            BattleTextVram(bg, &sTextStore.vram[k].map, &sTextStore.vram[k].mapBytes, &sTextStore.vram[k].start,
                           &sTextStore.vram[k].bytes);
            VramSyncRange(sTextStore.vram[k].map, sTextStore.vram[k].mapBytes);
            VramSyncRange(sTextStore.vram[k].start, sTextStore.vram[k].bytes);
            ++k;
        }
    sTextStore.key = key;
    sTextStore.parts = sTextKeyLast;
    sTextStore.pictures = BattleTextPictures();
    sTextStore.token = sStats.frames + 1;
    sTextStore.frame = sStats.frames;
    sTextStore.above = sTextAbove;
    sTextStore.shown = sBoxShown == sStats.frames;
    sTextStore.held = true;
}
