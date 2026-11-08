/*
 * A block of memory claimed once and handed out in pieces.
 *
 * The chunk meshes and the building pages used to come straight from the
 * console's VRAM allocator, a few thousand variable-sized pieces over an hour
 * of play, freed in whatever order the caches evicted them. VRAM ended up in
 * crumbs: 1.5 MiB free and no 512 KiB piece anywhere, so a city's building
 * page could not load at all. Each cache now claims its whole budget as one
 * block at start-up and cuts it here, where the fragmentation stays inside the
 * cache that caused it, and where the cache can cure it by evicting.
 *
 * First fit over an address-ordered list of live pieces; freeing merges by
 * construction, because a gap is simply the space between two neighbours.
 * Pieces may be taken from the low end or the high end: large and small ones
 * kept at opposite ends do not strand each other. Nothing here touches the
 * memory itself, so it is tested on the host.
 */
#ifndef CTR_VOXEL_ARENA_H
#define CTR_VOXEL_ARENA_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    uint32_t offset, size;
} VoxelArenaPiece;

typedef struct
{
    uint8_t *base;
    uint32_t size, align;
    VoxelArenaPiece *pieces; /* live, by address */
    unsigned count, capacity;
    uint32_t used;
} VoxelArena;

/* `pieces` holds `capacity` entries: at most that many pieces live at once.
 * `align` is a power of two; every piece starts and ends on it. */
void VoxelArena_Init(VoxelArena *arena, void *base, uint32_t size, uint32_t align,
                     VoxelArenaPiece *pieces, unsigned capacity);
/* NULL if no gap is large enough (or the piece table is full). */
void *VoxelArena_Alloc(VoxelArena *arena, uint32_t size, bool fromTop);
void VoxelArena_Free(VoxelArena *arena, void *block);
/* The largest piece that would fit right now. */
uint32_t VoxelArena_LargestGap(const VoxelArena *arena);
/* The place of `block` among the live pieces (by address), or -1. */
int VoxelArena_PieceOf(const VoxelArena *arena, const void *block);
/* The largest piece that would fit once the pieces flagged in `freed` (one
 * entry per live piece, by address) were freed. Nothing is freed. */
uint32_t VoxelArena_LargestGapFreeing(const VoxelArena *arena, const bool *freed);
/* Whether a piece of `bytes`, rounded as VoxelArena_Alloc rounds it, would fit
 * there. */
bool VoxelArena_FitsFreeing(const VoxelArena *arena, const bool *freed, uint32_t bytes);

/*
 * Room made by sliding pieces along the arena, when there is enough free in
 * all but no gap of it large enough. Only the bookkeeping, like the rest:
 * the caller copies what a piece holds to its new place.
 */
enum
{
    VOXEL_ARENA_STAYS,  /* where it is */
    VOXEL_ARENA_FREED,  /* counted as free: the caller frees it before a slide passes over it */
    VOXEL_ARENA_SLIDES, /* may slide, never past another piece */
};
/* The most sliding pieces a run between two that stay may hold for a plan. */
#define VOXEL_ARENA_PLAN_PIECES 16u

typedef struct
{
    int piece;          /* the live piece to slide first */
    uint32_t offset;    /* where to */
    uint32_t cost;      /* the bytes the whole way copies and frees, and a price a slide */
} VoxelArenaSlide;

typedef struct
{
    uint32_t from, to, size; /* offsets in the arena */
} VoxelArenaCopy;

/* The piece at `block` moved to `offset` (aligned) between its neighbours,
 * which stay: anywhere over its own place and the gaps beside it. Its new
 * address, or NULL (nothing changed) if it does not fit there. */
void *VoxelArena_Move(VoxelArena *arena, void *block, uint32_t offset);
/* The copies a slide over `distance` takes for `held` bytes, each no longer
 * than the distance or `copyMax` (VoxelArena_SlideSteps). */
unsigned VoxelArena_SlideCopies(uint32_t held, uint32_t distance, uint32_t copyMax);
/* Those copies for the first `held` bytes of a piece slid from `from` to
 * `to`, in the order they must run, each finished before the next starts.
 * Their number; none are written if that is more than `max`. */
unsigned VoxelArena_SlideSteps(uint32_t from, uint32_t to, uint32_t held, uint32_t copyMax,
                               VoxelArenaCopy *steps, unsigned max);
/*
 * A way to a gap of `size`, for the pieces as `kind` has them (one entry per
 * live piece, by address): those that SLIDE move along the arena, never past
 * one another or one that STAYS, and those FREED count as free. A piece
 * holding `held[i]` bytes may slide only in at most `copiesMax` copies. Of
 * every way found, the one that copies and frees the fewest bytes in the
 * fewest slides; its first slide in `first`. False if there is none, or if
 * no piece need slide (the gap is there once the FREED pieces are freed).
 * Taken one slide at a time, asked again after each, it comes to the gap.
 */
bool VoxelArena_SlidePlan(const VoxelArena *arena, const uint8_t *kind, const uint32_t *held,
                          uint32_t size, uint32_t copyMax, unsigned copiesMax, VoxelArenaSlide *first);

#endif
