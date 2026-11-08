/*
 * Part of 3ds_video.c. The battle's sprites of a frame gathered: groups, kept
 * gathers, BattleSprites and LooseDraw.
 */

/*
 * The groups are kept (sGatherKept): a gather's result is a function of the
 * entries it takes - each one's index, mode, priority, whether it is an info
 * box's, and its box as the GBA reads it - and of the clip, the blend, the
 * barrier and what it is asked for. All of that is compared whole, entry by
 * entry, not hashed; while it is the same, so are the groups, the loose
 * sprites and the furthest priority, copied back, and the blend the frame
 * is flat for is counted again (StereoWhy, BlendWhy). A frame whose sprites
 * move gathers them again. One kept result for each of the three ways
 * BattleSprites asks (with the boxes, without, with a barrier), and none past
 * GATHER_KEPT_SLOTS groups. Audit builds: nocache.txt bit 8192 turns it off
 * with the kept slots.
 */
#define GATHER_KEPT_SLOTS 48
#define GATHER_GLOBALS 9
typedef struct
{
    bool held, any, apart;
    int8_t why;
    unsigned slotCount, looseCount, furthest;
    uint32_t global[GATHER_GLOBALS];
    uint32_t input[128 * 2];
    BattleSlot slots[GATHER_KEPT_SLOTS];
    __typeof__(sLoose[0]) loose[LOOSE_MAX];
} GatherKept;
static GatherKept sGatherKept[3];
/* Gathers this window kept, made again, and made with too many groups to keep. */
static unsigned sGatherHits, sGatherMade, sGatherBig;
/*
 * Pairs of groups found apart since either last changed (BattleSpritesGather):
 * whether two groups merge depends on them alone and the barrier, and a merge
 * changes only the group it grows and the one moved into the gap, so a pair
 * found apart is not looked at again until one of them changes. The groups
 * merged are the same, in the same order, as walking every pair from the
 * start again after each merge did - the first pair that merges, every time -
 * without its n^3 comparisons.
 */
static uint32_t sGroupsApart[128][4];

static void GroupsApartForget(unsigned k, unsigned count)
{
    memset(sGroupsApart[k], 0, sizeof(sGroupsApart[k]));
    for (unsigned a = 0; a < count && a < k; ++a)
        sGroupsApart[a][k >> 5] &= ~(1u << (k & 31));
}

/* nocache.txt bit 8192: the battle's groups and slots composed every frame. */
static bool KeptSlotsOff(void)
{
#ifdef CTR_AUDIT_BITS
    return (CtrDumpOff() & 8192) != 0;
#else
    return false;
#endif
}

/*
 * `barrier`, when not NULL, is the nearest and furthest priority of the
 * backgrounds in front of sprites (BattleFrontRange): no two groups merge
 * into one on both sides of them (BattleSprites).
 */
static bool BattleSpritesGather(unsigned *furthest, bool *apart, bool boxes, const unsigned *barrier)
{
    unsigned display = Reg(0), control = Reg(0x50);
    bool alpha = ((control >> 6) & 3) == 1 && (control & 16);
    int reach = (int)ceilf(BATTLE_SPRITE_DEPTH / sShiftZoom) + 1;
    int top = (int)lroundf((sScenePut.oy - sOffY) / sZoom), bottom = BATTLE_SCENE_BOTTOM;
    int left = VIEW_LEFT - reach, right = VIEW_RIGHT + reach;
    const CtrObjList *list = sObjList;
    GatherKept *kept = &sGatherKept[barrier ? 2 : boxes ? 0 : 1];
    uint32_t global[GATHER_GLOBALS], input[128 * 2];
    unsigned inputs = 0;
    int why = -1;
    bool keep = !KeptSlotsOff(), drop = sHud && sHudCrisp;

    sSlotCount = 0;
    sLooseCount = 0;
    *furthest = 0;
    /* The OBJ window is on through every battle (DISPCNT 0xbf40); its own
     * sprites (mode 2) are not drawn by this renderer anyway (DrawObjects).
     * Windows 0 and 1 cut the slots as they cut the screen (SlotsCompose). */
    *apart = true;
    if (!(display & 0x1000)) return false;
#ifdef CTR_AUDIT_BITS
    if (CtrDumpOff() & 2) drop = false;
#endif
    for (unsigned i = 0; i < 128; ++i)
    {
        unsigned mode, priority;
        int x, y, w, h, full, height;
        BattleSlot *slot;

        if (list)
        {
            const CtrObjEntry *e = &list->entry[i];

            /* The entries the list says the groups take, by the same test. */
            if (!(list->gather[i >> 5] & (1u << (i & 31)))) continue;
            mode = e->mode;
            priority = e->priority;
            x = e->x;
            y = e->y;
            w = e->w;
            h = height = e->h;
        }
        else
        {
            unsigned attr0 = sMemory.oam[i * 4], attr1 = sMemory.oam[i * 4 + 1], attr2 = sMemory.oam[i * 4 + 2];
            unsigned shape = attr0 >> 14;
            bool affine = (attr0 & 0x100) != 0, twice = affine && (attr0 & 0x200);

            mode = (attr0 >> 10) & 3;
            if ((!affine && (attr0 & 0x200)) || shape == 3 || mode >= 2) continue;
            priority = (attr2 >> 10) & 3;
            w = gCtrObjDims[shape][attr1 >> 14][0] << twice;
            h = height = gCtrObjDims[shape][attr1 >> 14][1] << twice;
            x = (int)(attr1 & 511);
            y = (int)(attr0 & 255);
            if (x + w > 512) x -= 512;
            if (y + h > 256) y -= 256;
        }
        if (!boxes && sHudOamBox[i] >= 0) continue;
        input[inputs * 2] = i | mode << 7 | priority << 9 | (unsigned)(sHudOamBox[i] >= 0) << 11
                          | (unsigned)w << 12 | (unsigned)h << 20;
        input[inputs * 2 + 1] = (uint16_t)x | (uint32_t)(uint16_t)y << 16;
        ++inputs;
        full = y;
        /* Under the text box and above the scene nothing of it shows. */
        if (x < left) { w -= left - x; x = left; }
        if (y < top) { h -= top - y; y = top; }
        if (x + w > right) w = right - x;
        if (y + h > bottom) h = bottom - y;
        if (w <= 0 || h <= 0) continue;
        if (mode == 1 && !alpha && sLooseCount < LOOSE_MAX)
        {
            sLoose[sLooseCount].index = (uint8_t)i;
            sLoose[sLooseCount].priority = (uint8_t)priority;
            sLoose[sLooseCount].x = (int16_t)x;
            sLoose[sLooseCount].y = (int16_t)y;
            sLoose[sLooseCount].w = (int16_t)w;
            sLoose[sLooseCount].h = (int16_t)h;
            sLoose[sLooseCount].centre = (int16_t)(full + height / 2);
            ++sLooseCount;
            if (priority > *furthest) *furthest = priority;
            continue;
        }
        if (mode == 1 || alpha)
        {
            if (*apart) { why = alpha ? 1 : 0; StereoWhy(0); BlendWhy((unsigned)why); }
            *apart = false;
        }
        /* Mosaic is not drawn (DrawObjects), composed whole or apart: kept
         * apart, a sprite in it keeps its depth. */
        if (priority > *furthest) *furthest = priority;
        slot = &sSlots[sSlotCount++];
        memset(slot->members, 0, sizeof(slot->members));
        slot->members[i >> 5] = 1u << (i & 31);
        slot->x = (int16_t)x;
        slot->y = (int16_t)y;
        slot->w = (int16_t)w;
        slot->h = (int16_t)h;
        slot->nearest = slot->furthest = (uint8_t)priority;
        slot->front = false;
        slot->patch = 0;
        slot->objWin = false;
        slot->blendBg = 0;
        slot->first = (uint8_t)i;
        /* The depth of the whole sprite, not of the part on show. */
        slot->centre = (int16_t)(full + height / 2);
    }
    global[0] = (unsigned)alpha | (unsigned)drop << 1 | (unsigned)boxes << 2 | (barrier ? 8u : 0u);
    global[1] = (uint32_t)reach;
    global[2] = (uint32_t)top;
    global[3] = (uint32_t)bottom;
    global[4] = (uint32_t)left;
    global[5] = (uint32_t)right;
    global[6] = barrier ? barrier[0] : 0;
    global[7] = barrier ? barrier[1] : 0;
    global[8] = inputs;
    if (keep && kept->held && !memcmp(kept->global, global, sizeof(global))
        && !memcmp(kept->input, input, inputs * 2 * sizeof(input[0])))
    {
        /* The same entries as the kept gather's: its groups, and the blend
         * that made the frame flat counted again, as the gather counted it. */
        if (kept->why >= 0) { StereoWhy(0); BlendWhy((unsigned)kept->why); }
        sSlotCount = kept->slotCount;
        memcpy(sSlots, kept->slots, sSlotCount * sizeof(sSlots[0]));
        sLooseCount = kept->looseCount;
        memcpy(sLoose, kept->loose, sLooseCount * sizeof(sLoose[0]));
        *furthest = kept->furthest;
        *apart = kept->apart;
        ++sGatherHits;
        return kept->any;
    }
    if (keep) ++sGatherMade;
    /* Groups: merged while any two touch, each at the depth of its middle;
     * the first pair that does every time, as walking every pair from the
     * start after each merge (sGroupsApart). */
    for (unsigned k = 0; k < sSlotCount; ++k)
        memset(sGroupsApart[k], 0, sizeof(sGroupsApart[k]));
    for (bool merged = true; merged; )
    {
        merged = false;
        for (unsigned a = 0; a < sSlotCount && !merged; ++a)
            for (unsigned b = a + 1; b < sSlotCount && !merged; ++b)
            {
                BattleSlot *p = &sSlots[a], *q = &sSlots[b];
                int x0, y0, x1, y1;

                if (sGroupsApart[a][b >> 5] & (1u << (b & 31))) continue;
                sGroupsApart[a][b >> 5] |= 1u << (b & 31);
                if (p->x > q->x + q->w || q->x > p->x + p->w || p->y > q->y + q->h || q->y > p->y + p->h)
                    continue;
                x0 = p->x < q->x ? p->x : q->x;
                y0 = p->y < q->y ? p->y : q->y;
                x1 = p->x + p->w > q->x + q->w ? p->x + p->w : q->x + q->w;
                y1 = p->y + p->h > q->y + q->h ? p->y + p->h : q->y + q->h;
                if (x1 - x0 > SLOT_GROUP_MAX || y1 - y0 > SLOT_GROUP_MAX) continue;
                if (fabsf(BattleSpriteNear(p->centre) - BattleSpriteNear(q->centre)) > SLOT_GROUP_DEPTH)
                    continue;
                /* By each entry's priority as the OAM holds it, a subsprite's
                 * own where a sprite has them (src/sprite.c writes it into
                 * every entry): Sandstorm's flying sand is 1 over its sprite's
                 * 2, in front of the sand's BG1 with the Pokemon behind it.
                 * Only an entry alone between two such backgrounds of other
                 * priorities still straddles them, for the fallback. */
                if (barrier)
                {
                    unsigned nearest = p->nearest < q->nearest ? p->nearest : q->nearest;
                    unsigned back = p->furthest > q->furthest ? p->furthest : q->furthest;

                    if (back > barrier[0] && nearest <= barrier[1]) continue;
                }
                if (q->first < p->first) p->first = q->first;
                for (unsigned m = 0; m < 4; ++m) p->members[m] |= q->members[m];
                if (q->nearest < p->nearest) p->nearest = q->nearest;
                if (q->furthest > p->furthest) p->furthest = q->furthest;
                p->centre = (int16_t)((p->centre * p->w * p->h + q->centre * q->w * q->h)
                                      / (p->w * p->h + q->w * q->h));
                p->x = (int16_t)x0;
                p->y = (int16_t)y0;
                p->w = (int16_t)(x1 - x0);
                p->h = (int16_t)(y1 - y0);
                sSlots[b] = sSlots[--sSlotCount];
                /* What a and b hold now is new to every pair. */
                GroupsApartForget(a, sSlotCount);
                GroupsApartForget(b, sSlotCount);
                merged = true;
            }
    }
    /*
     * The info boxes drawn from their pictures (HudDrawCrisp): their sprites
     * draw nothing (DrawObjects), and a group of them alone is an empty slot
     * - in a double battle four of them 128 wide, which with the four
     * Pokemon did not fit the slots' own surface. Groups with any other
     * sprite in them stay as they are, at the depth they have. The furthest
     * priority is then that of what is left.
     */
    if (drop)
    {
        bool dropped = false;
        uint32_t others[4] = {0, 0, 0, 0};

        for (unsigned i = 0; i < 128; ++i)
            if (sHudOamBox[i] < 0) others[i >> 5] |= 1u << (i & 31);
        for (unsigned k = 0; k < sSlotCount; )
        {
            const uint32_t *members = sSlots[k].members;

            if (!((members[0] & others[0]) | (members[1] & others[1]) | (members[2] & others[2])
                  | (members[3] & others[3])))
            {
                sSlots[k] = sSlots[--sSlotCount];
                dropped = true;
            }
            else ++k;
        }
        if (dropped)
        {
            *furthest = 0;
            for (unsigned k = 0; k < sSlotCount; ++k)
                if (sSlots[k].furthest > *furthest) *furthest = sSlots[k].furthest;
            for (unsigned k = 0; k < sLooseCount; ++k)
                if (sLoose[k].priority > *furthest) *furthest = sLoose[k].priority;
        }
    }
    /* Back to front among themselves, as the GBA layers them: the furthest
     * priority first, then the higher OAM entries (SlotsPlace interleaves
     * them with the groups). */
    for (unsigned i = 1; i < sLooseCount; ++i)
        for (unsigned j = i; j > 0; --j)
        {
            unsigned a = sLoose[j - 1].priority << 8 | sLoose[j - 1].index, b = sLoose[j].priority << 8 | sLoose[j].index;

            if (a >= b) break;
            {
                __typeof__(sLoose[0]) t = sLoose[j];

                sLoose[j] = sLoose[j - 1];
                sLoose[j - 1] = t;
            }
        }
    kept->held = keep && sSlotCount <= GATHER_KEPT_SLOTS;
    if (keep && !kept->held) ++sGatherBig;
    if (kept->held)
    {
        memcpy(kept->global, global, sizeof(global));
        memcpy(kept->input, input, inputs * 2 * sizeof(input[0]));
        kept->slotCount = sSlotCount;
        memcpy(kept->slots, sSlots, sSlotCount * sizeof(sSlots[0]));
        kept->looseCount = sLooseCount;
        memcpy(kept->loose, sLoose, sLooseCount * sizeof(sLoose[0]));
        kept->furthest = *furthest;
        kept->apart = *apart;
        kept->why = (int8_t)why;
        kept->any = sSlotCount > 0 || sLooseCount > 0;
    }
    return sSlotCount > 0 || sLooseCount > 0;
}

/* The backgrounds drawn in front of a sprite (before the furthest one, as
 * RenderBattleStereo finds them): their nearest and furthest priority in
 * range[0] and range[1], or false with none. */
static bool BattleFrontRange(unsigned furthest, unsigned range[2])
{
    bool front = false;

    range[0] = 3;
    range[1] = 0;
    for (unsigned bg = 1; bg < 4; ++bg)
        if (sBgDrawn[bg] && (Reg(8 + bg * 2) & 3) < furthest)
        {
            unsigned priority = Reg(8 + bg * 2) & 3;

            front = true;
            if (priority < range[0]) range[0] = priority;
            if (priority > range[1]) range[1] = priority;
        }
    return front;
}

/*
 * A group on both sides of a background in front of sprites cannot be put
 * apart, and the frame is composed whole, every Pokemon flat. With the info
 * boxes drawn from their pictures, their sprites draw nothing, but they
 * still joined a Pokemon's group: the opponent with its box (priority 0 over
 * its 2), which Surf's wave (BG1, priority 1) comes between - all of the
 * move flat. For such a frame alone the groups are gathered again without
 * those sprites; any other frame keeps the groups, and depths, it had.
 */
static bool BattleSlotsStraddle(unsigned furthest)
{
    unsigned range[2];

    if (!BattleFrontRange(furthest, range)) return false;
    for (unsigned k = 0; k < sSlotCount; ++k)
        if (sSlots[k].furthest > range[0] && sSlots[k].nearest <= range[1]) return true;
    return false;
}

/*
 * The priority barrier, over the voxel world. Sandstorm's flying sand flies
 * in front of its BG1 sand (priority 1, as BG1's) and touches the Pokemon
 * behind it (2): merged into one group with them, every frame it did was
 * composed whole per eye - 186 of a test double battle's 6000, inside the
 * 158 frames the sand lasts each turn, 14.4 ms of drawing in Azahar against
 * 3.1, the mode's slowest hundredth - while the ~96 frames of each turn where
 * it touched none kept their depths. For such a frame alone the groups are
 * gathered once more, none merged across the backgrounds in front
 * (BattleSpritesGather): each keeps its own depth, the sand in front of BG1
 * as the GBA layers it, the Pokemon behind with BG1 blended in their slots,
 * as in the frames between. What still straddles (an entry alone between two
 * such backgrounds) or finds no room keeps the fallbacks. Not in the 2D
 * battle, where such frames are composed once, flat, for less than apart: a
 * decision of its own. Audit builds: nocache.txt bit 256 turns it off.
 */
static bool sBarrierNow;
/* Frames the barrier gathered again, by what they then were: apart, apart
 * with the scenery composed into each eye, whole for another reason, whole
 * because a group still straddled. */
static unsigned sBarrierFrames[4];
/* Frames whose scenery was composed into each eye (RenderBattleStereo). */
static unsigned sSceneryEyeFrames;

static bool BattleSprites(unsigned *furthest, bool *apart, bool world)
{
    bool any = BattleSpritesGather(furthest, apart, true, NULL);
    bool boxes = true;
    unsigned range[2];

    bool regroup = true, barrier = world;

#ifdef CTR_AUDIT_BITS
    regroup = !(CtrDumpOff() & 4);
    if (CtrDumpOff() & 256) barrier = false;
#endif
    sBarrierNow = false;
    if (regroup && any && *apart && sHud && sHudCrisp && BattleSlotsStraddle(*furthest))
    {
        any = BattleSpritesGather(furthest, apart, false, NULL);
        boxes = false;
    }
    /* With the sprites the gather just took (`boxes`): the same entries,
     * loose sprites and blends, only the groups change; RenderBattleStereo's
     * front and straddle loops judge them as any others. */
    if (barrier && any && *apart && BattleSlotsStraddle(*furthest))
    {
        BattleFrontRange(*furthest, range);
        any = BattleSpritesGather(furthest, apart, boxes, range);
        sBarrierNow = true;
    }
    return any;
}

/* Loose sprite `l` over an eye's picture, at its depth (sLoose). */
static void LooseDraw(C3D_RenderTarget *picture, unsigned l)
{
    float offX = sOffX;
    unsigned exclude = sLayerExclude;
    uint32_t mask[4];

    memcpy(mask, sObjMask, sizeof(mask));
    BlendForget();
    SceneOn(picture);
    ClipToView();
    ViewBase();
    sLayerExclude = 63u & ~16u;
    sObjMaskOn = true;
    memset(sObjMask, 0, sizeof(sObjMask));
    sObjMask[sLoose[l].index >> 5] = 1u << (sLoose[l].index & 31);
    sOffX = offX + BattleSpriteShift(sLoose[l].centre);
    Compose();
    C2D_Flush();
    sObjMaskOn = false;
    memcpy(sObjMask, mask, sizeof(mask));
    sLayerExclude = exclude;
    sOffX = offX;
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    ClipToView();
    BlendForget();
}
