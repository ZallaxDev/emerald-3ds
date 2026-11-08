#ifndef CTR_OBJ_LIST_H
#define CTR_OBJ_LIST_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The OAM's draw list (engine reform, stage 2).
 *
 * The producer is the sprite engine: each pass of AddSpritesToOamBuffer
 * (src/sprite.c) marks its entries - the fog's, the lattice's, the particles'
 * - and publishes what it wrote (CtrObjList_Publish). The list itself is
 * taken from the OAM that is presented, not from the last pass:
 * pacing hides frames, and a screen can write OAM without a pass. Once a
 * presented frame, before anything reads OAM (CtrVideo_Present), the shown
 * OAM is compared with the published one; equal, the marks are the pass's
 * that wrote it, and the list holds, per entry, its box with the GBA's wrap
 * applied, priority, mode, flags and marks, the entries each priority draws
 * in the GBA's order (127 down to 0) and what the renderer otherwise walks all
 * 128 entries for. Not equal - or nothing published - the consumers walk all
 * 128 as before, and the frame is counted (fallback).
 */

/* An OBJ's size in pixels by shape (square, wide, tall) and size: w, h. */
extern const uint8_t gCtrObjDims[3][4][2];

/* An entry's marks, as the pass that wrote it set them (src/sprite.c). */
enum
{
    CTR_OBJ_MARK_FOG = 1u << 0,
    CTR_OBJ_MARK_LATTICE = 1u << 1,
    CTR_OBJ_MARK_PARTICLE = 1u << 2,
};

/* An entry's flags. */
enum
{
    CTR_OBJ_SHOWN = 1u << 0,     /* not disabled (attr0 bits 8-9 not 0b10) */
    CTR_OBJ_MOSAIC = 1u << 1,
    CTR_OBJ_DRAWN = 1u << 2,     /* in its priority's list: shown, mode and shape not 3 */
};

typedef struct
{
    int16_t x, y;        /* the box's corner: past 512 across or 256 down, from the other side */
    uint8_t w, h;        /* the box, twice the sprite for an affine one at double size */
    uint8_t priority, mode, shape, flags;
    uint8_t marks;
} CtrObjEntry;

typedef struct
{
    CtrObjEntry entry[128];
    /* The entries each priority draws, 127 down to 0, and the same as bits. */
    uint8_t count[4];
    uint8_t order[4][128];
    uint32_t members[4][4];
    /* Shown, shape not 3, mode 0 or 1: the battle's groups (BattleSpritesGather). */
    uint32_t gather[4];
    /* The furthest priority of a shown entry, -1 with none (MonoObjFurthest). */
    int8_t furthest;
    /* A shown entry is semi-transparent (ComposeAlphaBands). */
    bool semi;
    /* The box of the OBJ window's shown entries, shape not 3 (ObjWindowBounds). */
    bool window;
    int16_t windowX0, windowY0, windowX1, windowY1;
} CtrObjList;

/* What the renderer marked each entry with: bit sets of 128. */
typedef struct
{
    const uint32_t *fog, *lattice, *particle;
} CtrObjMarkSets;

/* The producer: the OAM buffer a pass of AddSpritesToOamBuffer wrote. */
void CtrObjList_Publish(const void *oamBuffer);
/*
 * The list of the OAM presented (`oam`, 128 entries of 4 halfwords), with the
 * marks the renderer holds; NULL when the shown OAM is not the one published,
 * or with `off` (an audit build's nocache.txt). Once a presented frame.
 */
const CtrObjList *CtrObjList_Latch(const uint16_t *oam, const CtrObjMarkSets *marks, bool off);
/* Frames latched since the last call: with the list, without (the shown OAM
 * not the one published, nothing published at all) and turned off. */
void CtrObjList_Counts(uint32_t *listed, uint32_t *mismatched, uint32_t *unpublished, uint32_t *off);

#endif
