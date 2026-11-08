/*
 * The OAM's draw list (3ds_obj_list.h).
 */
#include "3ds_obj_list.h"
#include <string.h>

const uint8_t gCtrObjDims[3][4][2] = {
    {{8, 8}, {16, 16}, {32, 32}, {64, 64}},
    {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
    {{8, 16}, {8, 32}, {16, 32}, {32, 64}},
};

/* The three attributes of every entry as the last pass wrote them; the
 * fourth is the affine matrices', copied after it (CopyMatricesToOamBuffer),
 * and nothing a mark depends on. */
static uint16_t sPublished[128][3];
static bool sEverPublished;
static CtrObjList sList;
static uint32_t sListed, sMismatched, sUnpublished, sOff;

void CtrObjList_Publish(const void *oamBuffer)
{
    const uint16_t *oam = (const uint16_t *)oamBuffer;

    for (unsigned i = 0; i < 128; ++i)
    {
        sPublished[i][0] = oam[i * 4];
        sPublished[i][1] = oam[i * 4 + 1];
        sPublished[i][2] = oam[i * 4 + 2];
    }
    sEverPublished = true;
}

static bool Marked(const uint32_t *set, unsigned i)
{
    return set && (set[i >> 5] >> (i & 31)) & 1;
}

static void Entry(CtrObjEntry *e, const uint16_t *obj, const CtrObjMarkSets *marks, unsigned i)
{
    unsigned attr0 = obj[0], attr1 = obj[1], attr2 = obj[2];
    unsigned shape = attr0 >> 14, mode = (attr0 >> 10) & 3;
    bool affine = (attr0 & 0x100) != 0, twice = affine && (attr0 & 0x200);
    bool shown = (attr0 & 0x300) != 0x200;
    int x = (int)(attr1 & 511), y = (int)(attr0 & 255), w = 8, h = 8;

    memset(e, 0, sizeof(*e));
    if (shape < 3)
    {
        w = gCtrObjDims[shape][attr1 >> 14][0] << twice;
        h = gCtrObjDims[shape][attr1 >> 14][1] << twice;
    }
    /* The GBA's own reading: a sprite comes back from the other side only
     * when it crosses the end of the coordinate range. */
    if (x + w > 512) x -= 512;
    if (y + h > 256) y -= 256;
    e->x = (int16_t)x;
    e->y = (int16_t)y;
    e->w = (uint8_t)w;
    e->h = (uint8_t)h;
    e->priority = (uint8_t)((attr2 >> 10) & 3);
    e->mode = (uint8_t)mode;
    e->shape = (uint8_t)shape;
    e->flags = (uint8_t)((shown ? CTR_OBJ_SHOWN : 0) | ((attr0 & 0x1000) ? CTR_OBJ_MOSAIC : 0)
                         | (shown && mode != 3 && shape != 3 ? CTR_OBJ_DRAWN : 0));
    e->marks = (uint8_t)((Marked(marks->fog, i) ? CTR_OBJ_MARK_FOG : 0)
                         | (Marked(marks->lattice, i) ? CTR_OBJ_MARK_LATTICE : 0)
                         | (Marked(marks->particle, i) ? CTR_OBJ_MARK_PARTICLE : 0));
}

/*
 * The marks each entry was built with: the sets whole, compared once a latch
 * - while they are the same, an entry whose attributes are those it was built
 * from is not built again (most of OAM, most frames), and one not shown is
 * only emptied. One pass over the 128 entries, the check that the shown OAM
 * is the published one included.
 */
static uint32_t sBuiltSets[3][4];
static uint16_t sBuilt[128][3];
static bool sBuiltAny;

static bool MarksSame(const CtrObjMarkSets *marks)
{
    static const uint32_t none[4];
    const uint32_t *sets[3] = {marks->fog, marks->lattice, marks->particle};
    bool same = sBuiltAny;

    for (unsigned s = 0; s < 3; ++s)
    {
        const uint32_t *words = sets[s] ? sets[s] : none;

        if (memcmp(words, sBuiltSets[s], sizeof(sBuiltSets[s])))
        {
            memcpy(sBuiltSets[s], words, sizeof(sBuiltSets[s]));
            same = false;
        }
    }
    return same;
}

const CtrObjList *CtrObjList_Latch(const uint16_t *oam, const CtrObjMarkSets *marks, bool off)
{
    bool marksSame;

    if (off) { ++sOff; return NULL; }
    if (!sEverPublished) { ++sUnpublished; return NULL; }
    marksSame = MarksSame(marks);
    sBuiltAny = true;
    memset(sList.count, 0, sizeof(sList.count));
    memset(sList.members, 0, sizeof(sList.members));
    memset(sList.gather, 0, sizeof(sList.gather));
    sList.furthest = -1;
    sList.semi = false;
    sList.window = false;
    /* 127 down to 0: each priority's entries in the order the GBA draws them. */
    for (int i = 127; i >= 0; --i)
    {
        const uint16_t *obj = oam + i * 4;
        CtrObjEntry *e = &sList.entry[i];

        /* The shown OAM must be the one the pass published, whose marks
         * these are. Not, the list is left: the next latch builds every
         * entry again (the marks were taken as this one's). */
        if (obj[0] != sPublished[i][0] || obj[1] != sPublished[i][1] || obj[2] != sPublished[i][2])
        {
            ++sMismatched;
            sBuiltAny = false;
            return NULL;
        }
        if ((obj[0] & 0x300) == 0x200)
        {
            /* Not shown: nothing reads it but the flags. */
            if (e->flags) memset(e, 0, sizeof(*e));
            sBuilt[i][0] = obj[0];
            continue;
        }
        if (!marksSame || obj[0] != sBuilt[i][0] || obj[1] != sBuilt[i][1] || obj[2] != sBuilt[i][2])
        {
            Entry(e, obj, marks, (unsigned)i);
            sBuilt[i][0] = obj[0];
            sBuilt[i][1] = obj[1];
            sBuilt[i][2] = obj[2];
        }
        if ((int)e->priority > sList.furthest) sList.furthest = (int8_t)e->priority;
        if (e->mode == 1) sList.semi = true;
        if (e->shape != 3 && e->mode < 2) sList.gather[i >> 5] |= 1u << (i & 31);
        if (e->shape != 3 && e->mode == 2)
        {
            if (!sList.window || e->x < sList.windowX0) sList.windowX0 = e->x;
            if (!sList.window || e->y < sList.windowY0) sList.windowY0 = e->y;
            if (!sList.window || e->x + e->w > sList.windowX1) sList.windowX1 = (int16_t)(e->x + e->w);
            if (!sList.window || e->y + e->h > sList.windowY1) sList.windowY1 = (int16_t)(e->y + e->h);
            sList.window = true;
        }
        if (e->flags & CTR_OBJ_DRAWN)
        {
            sList.members[e->priority][i >> 5] |= 1u << (i & 31);
            sList.order[e->priority][sList.count[e->priority]++] = (uint8_t)i;
        }
    }
    ++sListed;
    return &sList;
}

void CtrObjList_Counts(uint32_t *listed, uint32_t *mismatched, uint32_t *unpublished, uint32_t *off)
{
    *listed = sListed;
    *mismatched = sMismatched;
    *unpublished = sUnpublished;
    *off = sOff;
    sListed = sMismatched = sUnpublished = sOff = 0;
}
