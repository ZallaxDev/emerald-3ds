/*
 * Part of 3ds_video.c. The slots packed: the skyline (SlotsPack), the kept
 * regions' keys, SlotsCompose and SlotsPlace.
 */

/*
 * Each region's skyline: runs of columns and how far down each is taken.
 * A slot goes where it lies highest, the leftmost of those; it covers the
 * runs under it at its bottom. One slot adds two runs at most.
 */
typedef struct
{
    int16_t x, w, y;
} SlotRun;
static SlotRun sRuns[2][2 * 128 + 2];
static unsigned sRunCount[2];

/* Where a tw x th slot lies highest in the region, or false. */
static bool SlotRunFind(unsigned region, int tw, int th, int *bestX, int *bestY)
{
    const SlotRun *runs = sRuns[region];
    unsigned count = sRunCount[region];
    int end = sRegions[region].end, height = sRegions[region].tex->height;
    bool found = false;

    for (unsigned i = 0; i < count; ++i)
    {
        int x = runs[i].x, y = 0;

        if (x + tw > end) break;
        for (unsigned j = i; j < count && runs[j].x < x + tw; ++j)
            if (runs[j].y > y) y = runs[j].y;
        if (y + th > height) continue;
        if (!found || y < *bestY)
        {
            *bestX = x;
            *bestY = y;
            found = true;
        }
    }
    return found;
}

/* The runs under [x, x + tw) now down to y. */
static void SlotRunCover(unsigned region, int x, int tw, int y)
{
    SlotRun *runs = sRuns[region], kept[2 * 128 + 2];
    unsigned count = sRunCount[region], n = 0;

    for (unsigned i = 0; i < count; ++i)
    {
        int x0 = runs[i].x, x1 = runs[i].x + runs[i].w;

        if (x1 <= x || x0 >= x + tw) { kept[n++] = runs[i]; continue; }
        if (x0 < x) kept[n++] = (SlotRun){(int16_t)x0, (int16_t)(x - x0), runs[i].y};
        if (x0 <= x) kept[n++] = (SlotRun){(int16_t)x, (int16_t)tw, (int16_t)y};
        if (x1 > x + tw) kept[n++] = (SlotRun){(int16_t)(x + tw), (int16_t)(x1 - x - tw), runs[i].y};
    }
    /* Next to each other at the same depth: one run. */
    count = 0;
    for (unsigned i = 0; i < n; ++i)
    {
        if (count && runs[count - 1].y == kept[i].y && runs[count - 1].x + runs[count - 1].w == kept[i].x)
            runs[count - 1].w = (int16_t)(runs[count - 1].w + kept[i].w);
        else
            runs[count++] = kept[i];
    }
    sRunCount[region] = count;
}

/*
 * The slots, tallest first, each into the first region with room for it,
 * as high up as it goes there (the skyline above). Shelves the width of the
 * region needed 652 columns for the second battle's opening - a 128-pixel
 * Pokémon, a 64-pixel one and an info box - where the world beside them
 * leaves 616: the box fits under the smaller Pokémon. Then ordered by
 * region, the order they are composed in (SlotsCompose).
 */
/* One attempt at placing the slots, in `order`. */
static bool SlotsPackIn(const uint8_t *order)
{
    for (unsigned r = 0; r < sRegionCount; ++r)
    {
        sRuns[r][0] = (SlotRun){(int16_t)sRegions[r].column, (int16_t)(sRegions[r].end - sRegions[r].column), 0};
        sRunCount[r] = sRegions[r].end > sRegions[r].column;
    }
    for (unsigned k = 0; k < sSlotCount; ++k)
    {
        BattleSlot *slot = &sSlots[order[k]];
        int tw = (slot->w + 2) * (int)SCENE_ZOOM, th = (slot->h + 2) * (int)SCENE_ZOOM;
        unsigned region = 0;
        int x = 0, y = 0;

        while (region < sRegionCount && !SlotRunFind(region, tw, th, &x, &y)) ++region;
        if (region == sRegionCount) return false;
        SlotRunCover(region, x, tw, y + th);
        slot->region = (uint8_t)region;
        slot->sx = (int16_t)x;
        slot->sy = (int16_t)y;
    }
    return true;
}

/*
 * The slots, tallest first, each into the first region with room for it,
 * as high up as it goes there (the skyline above). Shelves the width of the
 * region needed 652 columns for the second battle's opening - a 128-pixel
 * Pokémon, a 64-pixel one and an info box - where the world beside them
 * leaves 616: the box fits under the smaller Pokémon. Tallest first left
 * Surf over the voxel world without room - two Pokemon and two info boxes
 * 128 wide in the 512 columns of the slots' own surface, which hold them
 * with the boxes stacked - so the widest first is tried next, then the
 * largest. Then ordered by region, the order they are composed in
 * (SlotsCompose).
 */
static bool SlotsPack(void)
{
    uint8_t order[128];
    bool packed = false;
    unsigned all = sRegionCount;
    /* In the 2D battle, the slots' own surface alone first: in the scene
     * surface's free columns they need it composed again (RenderBattleStereo). */
    bool own = all > 1 && sRegions[0].target != sScene && sRegions[all - 1].target == sScene;

    for (unsigned attempt = 0; attempt < 6 && !packed; ++attempt)
    {
        unsigned key = attempt % 3;

        if (attempt < 3 && !own) continue;
        sRegionCount = attempt < 3 ? all - 1 : all;
        for (unsigned i = 0; i < sSlotCount; ++i)
        {
            unsigned j = i;
            int mine = key == 0 ? sSlots[i].h : key == 1 ? sSlots[i].w : sSlots[i].w * sSlots[i].h;

            while (j > 0)
            {
                const BattleSlot *before = &sSlots[order[j - 1]];
                int theirs = key == 0 ? before->h : key == 1 ? before->w : before->w * before->h;

                if (theirs >= mine) break;
                order[j] = order[j - 1];
                --j;
            }
            order[j] = (uint8_t)i;
        }
        packed = SlotsPackIn(order);
    }
    sRegionCount = all;
    if (!packed) return false;
    for (unsigned i = 1; i < sSlotCount; ++i)
    {
        BattleSlot slot = sSlots[i];
        unsigned j = i;

        while (j > 0 && sSlots[j - 1].region > slot.region)
        {
            sSlots[j] = sSlots[j - 1];
            --j;
        }
        sSlots[j] = slot;
    }
    /*
     * Back to front as the GBA layers sprites: the furthest priority first,
     * and within one the higher OAM entries, which lower ones cover. Groups
     * only overlap when one would have been too big; the patches of
     * backgrounds come in their own pass (SlotsPlace).
     */
    for (unsigned i = 0; i < sSlotCount; ++i)
    {
        unsigned j = i;
        const BattleSlot *slot = &sSlots[i];

        while (j > 0)
        {
            const BattleSlot *before = &sSlots[sPlaceOrder[j - 1]];

            if (before->furthest > slot->furthest
                || (before->furthest == slot->furthest && before->first >= slot->first))
                break;
            sPlaceOrder[j] = sPlaceOrder[j - 1];
            --j;
        }
        sPlaceOrder[j] = (uint8_t)i;
    }
    return true;
}

/*
 * What OAM entry `i` draws into its group's slot (SlotsCompose, DrawObjects),
 * in GBA pixels: its box, read as the battle reads it, and an affine sprite's
 * picture as far as its matrix takes it - its tiles are not cut at the box,
 * only at the slot's own clip and scissor. False for an info box's sprite,
 * which draws nothing with the boxes drawn from their pictures (sHudCrisp).
 */
static bool SlotDrawnBox(unsigned i, int *x0, int *y0, int *x1, int *y1)
{
    unsigned attr0 = sMemory.oam[i * 4], attr1 = sMemory.oam[i * 4 + 1];
    unsigned shape = attr0 >> 14, size = attr1 >> 14;
    bool affine = (attr0 & 0x100) != 0, twice = affine && (attr0 & 0x200);
    int width = gCtrObjDims[shape][size][0], height = gCtrObjDims[shape][size][1];
    int w = width << twice, h = height << twice, x = (int)(attr1 & 511), y = (int)(attr0 & 255);

    if (sHudOamBox[i] >= 0) return false;
    if (x + w > 512) x -= 512;
    if (y + h > 256) y -= 256;
    *x0 = x;
    *y0 = y;
    *x1 = x + w;
    *y1 = y + h;
    if (affine)
    {
        /* The picture round the box's middle, as DrawObjectAt's matrix puts
         * it; a singular one draws nothing and keeps the box. */
        unsigned index = ((attr1 >> 9) & 31) * 16;
        float a = (int16_t)sMemory.oam[index + 3] / 256.0f, b = (int16_t)sMemory.oam[index + 7] / 256.0f;
        float c = (int16_t)sMemory.oam[index + 11] / 256.0f, d = (int16_t)sMemory.oam[index + 15] / 256.0f;
        float det = fabsf(a * d - b * c);

        if (det >= 0.00001f)
        {
            float cx = x + w / 2.0f, cy = y + h / 2.0f;
            float ex = (fabsf(d) * width + fabsf(b) * height) / (2 * det);
            float ey = (fabsf(c) * width + fabsf(a) * height) / (2 * det);
            int ax0 = (int)floorf(cx - ex), ax1 = (int)ceilf(cx + ex);
            int ay0 = (int)floorf(cy - ey), ay1 = (int)ceilf(cy + ey);

            if (ax0 < *x0) *x0 = ax0;
            if (ay0 < *y0) *y0 = ay0;
            if (ax1 > *x1) *x1 = ax1;
            if (ay1 > *y1) *y1 = ay1;
        }
    }
    return true;
}

/*
 * A group's box cut to what its sprites draw (SlotDrawnBox): an info box's
 * sprites draw nothing in a Pokemon's group they joined, which was as wide or
 * as tall as they made it all the same. All the rest of the group stays - its
 * members, depth, priorities and first entry, and every decision taken by
 * them - and its sprites keep their texels in the slot, with fewer clear ones
 * round them, which add nothing where the slot is drawn (SlotsPlace,
 * premultiplied; its bilinear draw reaches into the clear border the cut slot
 * has as well). Everything drawn in the old slot, its border too, is in the
 * cut one: the box is the old one wherever a sprite reaches its edge. Not for
 * a patch of backgrounds or a group they blend into (SlotsCompose), whose box
 * the layer fills; nor under the OBJ window, whose group is found by its box;
 * nor with weather drawn over the whole view (DrawObjects).
 */
static bool SlotCut(BattleSlot *slot)
{
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool boxes = false, drawn = false;

    if (slot->patch || slot->blendBg || slot->objWin) return false;
    for (unsigned m = 0; m < 4; ++m)
        if (slot->members[m] & (sFogOam[m] | sLatticeOam[m] | sParticleOam[m])) return false;
    for (unsigned m = 0; m < 4; ++m)
        for (uint32_t bits = slot->members[m]; bits; bits &= bits - 1)
        {
            int bx0, by0, bx1, by1;

            if (!SlotDrawnBox(m * 32 + (unsigned)__builtin_ctz(bits), &bx0, &by0, &bx1, &by1))
            {
                boxes = true;
                continue;
            }
            if (!drawn || bx0 < x0) x0 = bx0;
            if (!drawn || by0 < y0) y0 = by0;
            if (!drawn || bx1 > x1) x1 = bx1;
            if (!drawn || by1 > y1) y1 = by1;
            drawn = true;
        }
    if (!boxes || !drawn) return false;
    if (x0 < slot->x) x0 = slot->x;
    if (y0 < slot->y) y0 = slot->y;
    if (x1 > slot->x + slot->w) x1 = slot->x + slot->w;
    if (y1 > slot->y + slot->h) y1 = slot->y + slot->h;
    if (x0 == slot->x && y0 == slot->y && x1 == slot->x + slot->w && y1 == slot->y + slot->h) return false;
    slot->x = (int16_t)x0;
    slot->y = (int16_t)y0;
    slot->w = (int16_t)(x1 - x0);
    slot->h = (int16_t)(y1 - y0);
    return true;
}

/*
 * The 2D battle's slots, once packed with some in the scene surface's free
 * columns - which then need the scene composed again (RenderBattleStereo):
 * about 3 ms of Azahar's frame, in two frames of three of a double battle with
 * the slider up (2026-11-01), the four Pokemon with the info boxes they joined
 * not fitting the slots' own surface - packed again with the groups cut to
 * what their sprites draw (SlotCut). Kept only when that leaves those columns
 * free; otherwise, and in a frame whose slots did not fit at all, as they were
 * packed. Only past every decision taken by the groups' boxes, and with the
 * sprites drawn in their slots at their own boxes (no layer origin).
 */
static void SlotsPackCut(void)
{
    static BattleSlot packed[sizeof(sSlots) / sizeof(sSlots[0])];
    static uint8_t order[sizeof(sSlots) / sizeof(sSlots[0])];
    unsigned count = sSlotCount, cut = 0;
    bool inScene = false;

#ifdef CTR_AUDIT_BITS
    if (CtrDumpOff() & 128) return;
#endif
    if (!sHud || !sHudCrisp || sRegionCount < 2 || sLayerOrigin != 0.0f) return;
    for (unsigned k = 0; k < count && !inScene; ++k)
        if (sRegions[sSlots[k].region].target == sScene) inScene = true;
    if (!inScene) return;
    memcpy(packed, sSlots, count * sizeof(sSlots[0]));
    memcpy(order, sPlaceOrder, count);
    for (unsigned k = 0; k < count; ++k)
        cut += SlotCut(&sSlots[k]);
    if (!cut) return;
    ++sSlotsCutTried;
    bool fits = SlotsPack();

    for (unsigned k = 0; k < count && fits; ++k)
        if (sRegions[sSlots[k].region].target == sScene) fits = false;
    if (fits)
    {
        ++sSlotsCutFrames;
        sSlotsCut += cut;
        return;
    }
    memcpy(sSlots, packed, count * sizeof(sSlots[0]));
    memcpy(sPlaceOrder, order, count);
}

/* A rectangle of a target set to one colour by drawing it, not clearing:
 * no command split, which with the GPU started early costs a cache flush. */
static void DrawnClear(C3D_RenderTarget *target, uint32_t color, float x, float y, float w, float h)
{
    BlendForget();
    SceneOn(target);
    C2D_ViewReset();
    C2D_Flush();
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C2D_DrawRectSolid(x, y, 0, w, h, color);
    C2D_Flush();
    BlendForget();
}

/*
 * The slots kept (SlotsCompose). A region of the slots' own surface whose
 * slots are, one by one, what the frame that composed them had - the same
 * members at the same places in their slots, with the same tiles, palettes,
 * blend, fade and windows, each slot at the same texels - is left as that
 * frame composed it: nothing cleared, nothing drawn. All or nothing per
 * region, which is cleared whole before its first slot; and only a region
 * whose clear was a memory fill (ColourClear), never one cleared by drawing:
 * drawn clears over a surface's columns did not always land on hardware
 * (RenderBattleWorld), and a kept slot would show such a miss for as long as
 * it was kept. The scene surface's columns are never kept: every frame whose
 * slots go there has filled that surface first (RenderBattleStereo).
 *
 * The key (SlotKey), compared whole, in five parts that name why a region
 * was composed again:
 *  - pack: the region (column, end, its texture's size), and each slot's
 *    texel corner and size;
 *  - moved: each slot's members and, for each member in order, attr0 and
 *    attr1 without the position, attr2, its place in the slot's box (the
 *    GBA's wrap applied), an affine one's matrix (gOamMatrices in OAM), an
 *    info box's whether it is one; DISPCNT's mode, OBJ and mapping bits; the
 *    zoom of the shifts, the layer origin, the parallax, the sprites' depth
 *    and the eye's parallax with it, the priority mask, the view's place, the
 *    pass (filter, voxel pass, transition, stage, centred, battle, the
 *    boxes' kind, the indicator pass, the field's UI); the slot's place on
 *    the GBA screen where anything depends on it (a depth, an origin, a
 *    parallax);
 *  - palette: each member's palette (its bank, ext palette or the 256
 *    colours, as the tile cache names it) and that palette's version
 *    (sPaletteVersion, bumped by UpdatePalette at every change);
 *  - blend: BLDCNT, BLDALPHA, BLDY, the palette fade and its colour, MOSAIC;
 *  - window: DISPCNT's window bits, WININ, WINOUT, and for each slot whether
 *    windows 0 and 1 are off, outside its box, all over it or across it -
 *    across it, with the window's rectangle and the slot's place.
 * Then the tiles: each member's OBJ VRAM, its blocks' stamps after the token
 * of the compose (VramChangedAfter), and the surface's own serial
 * (sSlotSurface): made, given back, cleared late or written by the battle's
 * kept scenery without the slider since.
 *
 * Not kept, composed as before and counted: a patch of backgrounds or a slot
 * they blend into (their layers' tiles, maps and scroll are not in the key),
 * the OBJ window (a stat change), an info box drawn into its slot from its
 * picture, the weather's sprites (their phase is the view's), a mosaic that
 * shows, BLDALPHA or registers line by line, a region of more than
 * SLOT_KEY_SLOTS slots or SLOT_KEY_MEMBERS members.
 */
enum { SLOT_KEY_PACK, SLOT_KEY_MOVED, SLOT_KEY_PALETTE, SLOT_KEY_BLEND, SLOT_KEY_WINDOW, SLOT_KEY_PARTS };
#define SLOT_KEY_SLOTS 32
#define SLOT_KEY_MEMBERS 64
static const uint16_t sSlotKeyAt[SLOT_KEY_PARTS + 1] = {0, 72, 632, 760, 768, 1000};
typedef struct
{
    uint16_t used[SLOT_KEY_PARTS];
    bool full;
    uint32_t word[1000];
} SlotKey;
/* Why a region was composed again although it could be kept: nothing kept
 * yet (or the surface changed), then each part that differed, and its tiles. */
enum { SLOTS_AGAIN_NEW, SLOTS_AGAIN_PACK, SLOTS_AGAIN_MOVED, SLOTS_AGAIN_PALETTE, SLOTS_AGAIN_BLEND,
       SLOTS_AGAIN_WINDOW, SLOTS_AGAIN_TILES, SLOTS_AGAIN_COUNT };
/* Why a region could not be kept. */
enum { SLOTS_UNKEPT_SCENE, SLOTS_UNKEPT_BG, SLOTS_UNKEPT_OBJWIN, SLOTS_UNKEPT_HUD, SLOTS_UNKEPT_WEATHER,
       SLOTS_UNKEPT_MOSAIC, SLOTS_UNKEPT_LINE, SLOTS_UNKEPT_SIZE, SLOTS_UNKEPT_COUNT };
static struct
{
    bool held;
    uint32_t surface, token;
    unsigned ranges;
    uint32_t range[SLOT_KEY_MEMBERS][2];
    SlotKey key;
} sSlotsKept;
static SlotKey sSlotKeyNow;
static uint32_t sSlotRangeNow[SLOT_KEY_MEMBERS][2];
static unsigned sSlotRangesNow;
static unsigned sSlotsRegionsKept, sSlotsRegionsComposed, sSlotsAgain[SLOTS_AGAIN_COUNT];
static unsigned sSlotsUnkept[SLOTS_UNKEPT_COUNT];
/* The same as bits of this frame, for its record. */
static uint8_t sSlotsAgainNow, sSlotsUnkeptNow;
_Static_assert(SLOTS_AGAIN_COUNT <= 8 && SLOTS_UNKEPT_COUNT <= 8, "a bit each in the record");

static void SlotKeyPut(SlotKey *key, unsigned part, uint32_t value)
{
    if (key->used[part] >= sSlotKeyAt[part + 1] - sSlotKeyAt[part]) { key->full = true; return; }
    key->word[sSlotKeyAt[part] + key->used[part]++] = value;
}

static uint32_t FloatBits(float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static bool SlotKeyPartSame(const SlotKey *a, const SlotKey *b, unsigned part)
{
    return a->used[part] == b->used[part]
        && !memcmp(a->word + sSlotKeyAt[part], b->word + sSlotKeyAt[part], a->used[part] * sizeof(a->word[0]));
}

/* Window w over the box [x0, x1) x [y0, y1): 0 off, 1 outside it (or
 * empty), 2 all over it, 3 across it, with its rectangle in `rect`. */
static unsigned SlotWindowClass(unsigned w, int x0, int y0, int x1, int y1, int rect[4])
{
    if (!(Reg(0) & (0x2000u << w))) return 0;
    WindowRect(w, &rect[0], &rect[2], &rect[1], &rect[3]);
    if (rect[0] >= rect[2] || rect[1] >= rect[3]) return 1;
    if (rect[2] <= x0 || rect[0] >= x1 || rect[3] <= y0 || rect[1] >= y1) return 1;
    if (rect[0] <= x0 && rect[2] >= x1 && rect[1] <= y0 && rect[3] >= y1) return 2;
    return 3;
}

/*
 * Region `r`'s slots [k0, k1) into sSlotKeyNow and their OBJ VRAM into
 * sSlotRangeNow; SLOTS_UNKEPT_COUNT when it can be kept, else why not.
 */
static unsigned SlotsRegionKey(unsigned r, unsigned k0, unsigned k1)
{
    const SlotRegion *region = &sRegions[r];
    SlotKey *key = &sSlotKeyNow;
    bool absolute = sSpriteDepth || sLayerOrigin != 0.0f || sParallax != 0.0f;
    unsigned members = 0;

    if (region->target != sSlotTarget) return SLOTS_UNKEPT_SCENE;
    if (sObjWin.ready || sObjWinSlotted) return SLOTS_UNKEPT_OBJWIN;
    if (sLineAlphaOn || sRegLine || LineWindows()) return SLOTS_UNKEPT_LINE;
    if (k1 - k0 > SLOT_KEY_SLOTS) return SLOTS_UNKEPT_SIZE;
    memset(key->used, 0, sizeof(key->used));
    key->full = false;
    sSlotRangesNow = 0;
    SlotKeyPut(key, SLOT_KEY_PACK, (uint32_t)region->column);
    SlotKeyPut(key, SLOT_KEY_PACK, (uint32_t)region->end);
    SlotKeyPut(key, SLOT_KEY_PACK, region->tex->width | (uint32_t)region->tex->height << 16);
    SlotKeyPut(key, SLOT_KEY_PACK, k1 - k0);
    SlotKeyPut(key, SLOT_KEY_MOVED, Reg(0) & 0x10C7u);
    SlotKeyPut(key, SLOT_KEY_MOVED, FloatBits(sShiftZoom));
    SlotKeyPut(key, SLOT_KEY_MOVED, FloatBits(sLayerOrigin));
    SlotKeyPut(key, SLOT_KEY_MOVED, FloatBits(sParallax));
    SlotKeyPut(key, SLOT_KEY_MOVED, FloatBits(sSpriteDepth ? sEyeParallax : 0.0f));
    SlotKeyPut(key, SLOT_KEY_MOVED, (unsigned)sSpriteDepth | sObjFilter << 1 | sVoxelObjPass << 3
               | (unsigned)sTransitionCompose << 6 | (unsigned)sStage << 7 | (unsigned)sCentred << 8
               | (unsigned)sBattle << 9 | (unsigned)sHudCrisp << 10 | (unsigned)(sHud != NULL) << 11
               | (unsigned)sInLayerTexture << 12 | (unsigned)sObjIndicatorPass << 13 | (unsigned)sFieldUi << 14
               | sObjWinPass << 15);
    SlotKeyPut(key, SLOT_KEY_MOVED, sPriorityMask);
    SlotKeyPut(key, SLOT_KEY_MOVED, (uint16_t)sViewX | (uint32_t)(uint16_t)sViewY << 16);
    SlotKeyPut(key, SLOT_KEY_BLEND, Reg(0x50) | Reg(0x52) << 16);
    SlotKeyPut(key, SLOT_KEY_BLEND, Reg(0x54) | Reg(0x4c) << 16);
    SlotKeyPut(key, SLOT_KEY_BLEND, FloatBits(sPaletteFade));
    SlotKeyPut(key, SLOT_KEY_BLEND, sPaletteFadeColor);
    SlotKeyPut(key, SLOT_KEY_WINDOW, Reg(0) & 0xE000u);
    SlotKeyPut(key, SLOT_KEY_WINDOW, Reg(0x48) | Reg(0x4a) << 16);
    for (unsigned k = k0; k < k1; ++k)
    {
        const BattleSlot *slot = &sSlots[k];
        unsigned classes = 0;

        if (slot->patch || slot->blendBg) return SLOTS_UNKEPT_BG;
        if (slot->objWin) return SLOTS_UNKEPT_OBJWIN;
        SlotKeyPut(key, SLOT_KEY_PACK, (uint16_t)slot->sx | (uint32_t)(uint16_t)slot->sy << 16);
        SlotKeyPut(key, SLOT_KEY_PACK, (uint16_t)slot->w | (uint32_t)(uint16_t)slot->h << 16);
        for (unsigned m = 0; m < 4; ++m)
            SlotKeyPut(key, SLOT_KEY_MOVED, slot->members[m]);
        SlotKeyPut(key, SLOT_KEY_MOVED, absolute ? (uint16_t)slot->x | (uint32_t)(uint16_t)slot->y << 16 : ~0u);
        for (unsigned w = 0; w < 2; ++w)
        {
            int rect[4];
            unsigned c = SlotWindowClass(w, slot->x, slot->y, slot->x + slot->w, slot->y + slot->h, rect);

            classes |= c << (w * 2);
            if (c == 3)
            {
                SlotKeyPut(key, SLOT_KEY_WINDOW, (uint16_t)rect[0] | (uint32_t)(uint16_t)rect[2] << 16);
                SlotKeyPut(key, SLOT_KEY_WINDOW, (uint16_t)rect[1] | (uint32_t)(uint16_t)rect[3] << 16);
                SlotKeyPut(key, SLOT_KEY_WINDOW, (uint16_t)slot->x | (uint32_t)(uint16_t)slot->y << 16);
            }
        }
        SlotKeyPut(key, SLOT_KEY_WINDOW, classes);
        for (unsigned m = 0; m < 4; ++m)
            for (uint32_t bits = slot->members[m]; bits; bits &= bits - 1)
            {
                unsigned i = m * 32 + (unsigned)__builtin_ctz(bits);
                const uint16_t *obj = sMemory.oam + i * 4;
                unsigned attr0 = obj[0], attr1 = obj[1], attr2 = obj[2], shape = attr0 >> 14;
                bool affine = (attr0 & 0x100) != 0, twice = affine && (attr0 & 0x200);
                bool color256 = (attr0 & 0x2000) != 0, box = sHud && sHudOamBox[i] >= 0;
                int width, height, x = (int)(attr1 & 511), y = (int)(attr0 & 255);

                if (++members > SLOT_KEY_MEMBERS) return SLOTS_UNKEPT_SIZE;
                /* An affine sprite's place is in its matrix's sums too: the
                 * slot's own place then, as absolutes are. */
                if (affine && !absolute)
                {
                    absolute = true;
                    SlotKeyPut(key, SLOT_KEY_MOVED, (uint16_t)slot->x | (uint32_t)(uint16_t)slot->y << 16);
                }
                if (box && !sHudCrisp) return SLOTS_UNKEPT_HUD;
                if ((sFogOam[m] | sLatticeOam[m] | sParticleOam[m]) & (1u << (i & 31))) return SLOTS_UNKEPT_WEATHER;
                if ((attr0 & 0x1000) && (Reg(0x4c) & 0xff00)) return SLOTS_UNKEPT_MOSAIC;
                if (shape == 3) shape = 0;
                width = gCtrObjDims[shape][attr1 >> 14][0];
                height = gCtrObjDims[shape][attr1 >> 14][1];
                /* The battle's reading of a box, as DrawObjects has it. */
                if (x + (width << twice) > 512) x -= 512;
                if (y + (height << twice) > 256) y -= 256;
                SlotKeyPut(key, SLOT_KEY_MOVED, i | (unsigned)box << 7
                           | (unsigned)(i == WIRELESS_INDICATOR_OAM && WirelessIndicatorApart()) << 8 | attr2 << 16);
                SlotKeyPut(key, SLOT_KEY_MOVED, (attr0 & ~0xFFu) | (attr1 & ~0x1FFu) << 16);
                SlotKeyPut(key, SLOT_KEY_MOVED, (uint16_t)(x - slot->x) | (uint32_t)(uint16_t)(y - slot->y) << 16);
                if (affine)
                {
                    const uint16_t *matrix = sMemory.oam + ((attr1 >> 9) & 31) * 16;

                    SlotKeyPut(key, SLOT_KEY_MOVED, matrix[3] | (uint32_t)matrix[7] << 16);
                    SlotKeyPut(key, SLOT_KEY_MOVED, matrix[11] | (uint32_t)matrix[15] << 16);
                }
                /* An info box's sprite draws nothing with the boxes at the
                 * screen's pixels: no palette, no tiles. */
                if (box) continue;
                {
                    unsigned id = color256 ? PAL_OBJ256 : ObjBank(i, attr2);
                    unsigned units = color256 ? 2 : 1, first, last;

                    SlotKeyPut(key, SLOT_KEY_PALETTE, id);
                    SlotKeyPut(key, SLOT_KEY_PALETTE, sPaletteVersion[id]);
                    /* Its tiles, as DrawObjectAt reads them (CtrVideo_ObjTile):
                     * from the first to the last, all of OBJ VRAM if they
                     * wrap round its end. */
                    first = CtrVideo_ObjTile(attr2 & 1023, 0, 0, (unsigned)width, color256, Reg(0) & 0x40);
                    last = CtrVideo_ObjTile(attr2 & 1023, (unsigned)width / 8 - 1, (unsigned)height / 8 - 1,
                                            (unsigned)width, color256, Reg(0) & 0x40) + units - 1;
                    if (last < first || last > 1023) { first = 0; last = 1023; }
                    sSlotRangeNow[sSlotRangesNow][0] = 0x10000 + first * 32;
                    sSlotRangeNow[sSlotRangesNow][1] = (last - first + 1) * 32;
                    ++sSlotRangesNow;
                }
            }
    }
    return key->full ? SLOTS_UNKEPT_SIZE : SLOTS_UNKEPT_COUNT;
}

/* Whether a kept member's tiles changed after the region was composed. */
static bool SlotsTilesChanged(void)
{
    for (unsigned r = 0; r < sSlotsKept.ranges; ++r)
        if (VramChangedAfter(sSlotsKept.range[r][0], sSlotsKept.range[r][1], sSlotsKept.token)) return true;
    return false;
}

/*
 * Region `r`, slots [k0, k1): true when it is kept as the frame before left
 * it; false when it is to be composed, `*filled` then whether to clear it by
 * a memory fill and keep what is composed (sSlotsKept, now this frame's).
 */
static bool SlotsRegionKept(unsigned r, unsigned k0, unsigned k1, bool *filled)
{
    unsigned why;
    bool same = true;

    *filled = false;
    if (KeptSlotsOff()) { sSlotsKept.held = false; return false; }
    why = SlotsRegionKey(r, k0, k1);
    if (why != SLOTS_UNKEPT_COUNT)
    {
        /* The scene surface's region is not the slot surface's: what is
         * kept there stays so. */
        if (sRegions[r].target == sSlotTarget) sSlotsKept.held = false;
        ++sSlotsUnkept[why];
        sSlotsUnkeptNow |= (uint8_t)(1u << why);
        return false;
    }
    if (!sSlotsKept.held || sSlotsKept.surface != sSlotSurface)
    {
        ++sSlotsAgain[SLOTS_AGAIN_NEW];
        sSlotsAgainNow |= 1u << SLOTS_AGAIN_NEW;
        same = false;
    }
    else
    {
        static const uint8_t reasons[SLOT_KEY_PARTS] = {SLOTS_AGAIN_PACK, SLOTS_AGAIN_MOVED, SLOTS_AGAIN_PALETTE,
                                                        SLOTS_AGAIN_BLEND, SLOTS_AGAIN_WINDOW};

        for (unsigned part = 0; part < SLOT_KEY_PARTS; ++part)
            if (!SlotKeyPartSame(&sSlotsKept.key, &sSlotKeyNow, part))
            {
                ++sSlotsAgain[reasons[part]];
                sSlotsAgainNow |= (uint8_t)(1u << reasons[part]);
                same = false;
            }
        /* The kept members' tiles: the same members when the key is. */
        if (same && SlotsTilesChanged())
        {
            ++sSlotsAgain[SLOTS_AGAIN_TILES];
            sSlotsAgainNow |= 1u << SLOTS_AGAIN_TILES;
            same = false;
        }
    }
    if (same)
    {
        ++sSlotsRegionsKept;
        return true;
    }
    ++sSlotsRegionsComposed;
    sSlotsKept.key = sSlotKeyNow;
    sSlotsKept.ranges = sSlotRangesNow;
    memcpy(sSlotsKept.range, sSlotRangeNow, sSlotRangesNow * sizeof(sSlotRangeNow[0]));
    sSlotsKept.token = sStats.frames + 1;
    /* The fill and what follows make the surface its slots' again. */
    sSlotsKept.surface = ++sSlotSurface;
    sSlotsKept.held = true;
    *filled = true;
    return false;
}

/* Every slot's sprite into it, undisplaced; a region kept as it was left
 * whole (SlotsRegionKept). */
static void SlotsCompose(void)
{
    float zoom = sZoom, offX = sOffX, offY = sOffY;

    sZoom = SCENE_ZOOM;
    sLayerExclude = 63 & ~(16u | 32u);
    for (unsigned k = 0; k < sSlotCount; ++k)
    {
        const BattleSlot *slot = &sSlots[k];
        const SlotRegion *region = &sRegions[slot->region];
        int tw = (slot->w + 2) * (int)SCENE_ZOOM, th = (slot->h + 2) * (int)SCENE_ZOOM;

        /* A region cleared before its first slot (SlotsPack orders them). */
        if (k == 0 || slot->region != sSlots[k - 1].region)
        {
            unsigned end = k + 1;
            bool filled;

            while (end < sSlotCount && sSlots[end].region == slot->region) ++end;
            if (SlotsRegionKept(slot->region, k, end, &filled))
            {
                k = end - 1;
                continue;
            }
            C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
            if (filled)
            {
                /* The whole surface, its region, as a memory fill queued
                 * behind what is recorded and before what follows. */
                BlendForget();
                ColourClear(region->target, 0);
                sSceneOn = NULL;
            }
            else
                DrawnClear(region->target, 0, region->column, 0, region->end - region->column,
                           region->tex->height);
            SceneOn(region->target);
            sTargetW = region->tex->width;
            sTargetH = region->tex->height;
            sSurfaceH = region->tex->height;
        }

        /* GBA (x - 1, y - 1) on the slot's corner. */
        sOffX = slot->sx - (slot->x - 1) * SCENE_ZOOM;
        sOffY = slot->sy - (slot->y - 1) * SCENE_ZOOM;
        BlendForget();
        Blend(5, false, false);
        sClipX0 = slot->x;
        sClipY0 = slot->y;
        sClipX1 = slot->x + slot->w;
        sClipY1 = slot->y + slot->h;
        C2D_Flush();
        C3D_SetScissor(GPU_SCISSOR_NORMAL, (unsigned)slot->sx, (unsigned)(sSurfaceH - slot->sy - th),
                       (unsigned)(slot->sx + tw), (unsigned)(sSurfaceH - slot->sy));
        memcpy(sObjMask, slot->members, sizeof(sObjMask));
        sObjMaskOn = !slot->patch;
        sLayerExclude = slot->patch ? 63u & ~(slot->patch | 32u) : 63u & ~(16u | 32u);
        /* Through windows 0 and 1, each part of the slot with what its
         * window shows (ComposeBand scissors inside the slot's box). */
        Compose();
        sObjMaskOn = false;
        C2D_Flush();
        if (slot->blendBg && !slot->patch)
        {
            /* The layers alone, with the windows' colour effects bit:
             * without it the layer was drawn as it is over the sprites -
             * the sandstorm's sand filling them (port.log 2026-10-28). */
            sLayerExclude = 63u & ~(slot->blendBg | 32u);
            for (unsigned pass = SLOT_BLEND_UNDER; pass <= SLOT_BLEND_OVER; ++pass)
            {
                /* The slot's box again: Compose leaves the clip at the whole
                 * target, and the layer went over the slot beside this one
                 * in the same surface - twice blended there, Surf's wave
                 * came out as a pale band over the other Pokemon. */
                sClipX0 = slot->x;
                sClipY0 = slot->y;
                sClipX1 = slot->x + slot->w;
                sClipY1 = slot->y + slot->h;
                C3D_SetScissor(GPU_SCISSOR_NORMAL, (unsigned)slot->sx, (unsigned)(sSurfaceH - slot->sy - th),
                               (unsigned)(slot->sx + tw), (unsigned)(sSurfaceH - slot->sy));
                sSlotBlendPass = pass;
                BlendForget();
                Compose();
                C2D_Flush();
            }
            sSlotBlendPass = SLOT_BLEND_NONE;
            BlendForget();
        }
    }
    /* Past the geometry budget the slot draws drop quads (DrawSlotTinted,
     * TileWalkSlot): a region composed so is not to be kept, or its holes
     * stay while its key does. Composed again next frame, as the layer
     * textures are (LayerRenderCells). */
    if (sStats.tiles >= DRAW_BUDGET)
        sSlotsKept.held = false;
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    sLayerExclude = 0;
    SceneViewLeave(zoom, offX, offY);
}

/* The slots on `target` for the eye being drawn, each at its group's depth:
 * those behind the backgrounds in front of sprites, or those before them. */
static void SlotsPlaceBegin(C3D_RenderTarget *target)
{
    BlendForget();
    SceneOn(target);
    C2D_ViewReset();
    Blend(5, false, false);
    C2D_Flush();
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
}

static void SlotsPlace(C3D_RenderTarget *target, bool front, bool patches)
{
    float scale = sZoom / SCENE_ZOOM;
    /* The loose sprites, among the groups behind the backgrounds in front. */
    bool loose = !front && !patches && sLooseCount;
    unsigned next = 0;

    BlendForget();
    SceneOn(target);
    C2D_ViewReset();
    Blend(5, false, false);
    /*
     * Premultiplied: a slot's clear texels are 0 in colour as in alpha, so
     * where the bilinear draw takes half a sprite's edge texel and half a
     * clear one, it brings half the colour at half the coverage. Weighed by
     * its alpha again, that half colour came out darkened, a dark outline
     * round every sprite that the 2D battle, filtering sprite and scenery
     * together, never had. The alpha is kept whole for the same reason.
     */
    C2D_Flush();
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
    for (unsigned k = 0; k < sSlotCount; ++k)
    {
        const BattleSlot *slot = &sSlots[sPlaceOrder[k]];
        int tw = (slot->w + 2) * (int)SCENE_ZOOM, th = (slot->h + 2) * (int)SCENE_ZOOM;
        C3D_Tex *tex = sRegions[slot->region].tex;

        if (slot->front != front || (slot->patch != 0) != patches) continue;
        /* Those behind the group's frontmost sprite before it. */
        if (loose && next < sLooseCount)
        {
            unsigned key = (unsigned)slot->nearest << 8 | slot->first;
            bool drew = false;

            while (next < sLooseCount && ((unsigned)sLoose[next].priority << 8 | sLoose[next].index) > key)
            {
                C2D_Flush();
                LooseDraw(target, next++);
                drew = true;
            }
            if (drew) SlotsPlaceBegin(target);
        }
        const Tex3DS_SubTexture cut = {(u16)tw, (u16)th, slot->sx / (float)tex->width,
            1 - slot->sy / (float)tex->height, (slot->sx + tw) / (float)tex->width,
            1 - (slot->sy + th) / (float)tex->height};

        C2D_DrawImageAt((C2D_Image){tex, &cut},
                        (slot->x - 1) * sZoom + sOffX
                            + (slot->patch ? -sEyeParallax * BATTLE_BACK_DEPTH : BattleSpriteShift(slot->centre)),
                        (slot->y - 1) * sZoom + sOffY, 0, NULL, scale, scale);
    }
    C2D_Flush();
    while (loose && next < sLooseCount)
        LooseDraw(target, next++);
    BlendForget();
}
