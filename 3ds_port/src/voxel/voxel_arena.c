/* See voxel_arena.h. */
#include <string.h>

#include "voxel_arena.h"

void VoxelArena_Init(VoxelArena *arena, void *base, uint32_t size, uint32_t align,
                     VoxelArenaPiece *pieces, unsigned capacity)
{
    arena->base = base;
    arena->align = align != 0 ? align : 1;
    arena->size = size & ~(arena->align - 1);
    arena->pieces = pieces;
    arena->count = 0;
    arena->capacity = capacity;
    arena->used = 0;
}

/* Start of gap i: the end of piece i-1, or the arena's start. */
static uint32_t GapStart(const VoxelArena *arena, unsigned i)
{
    return i == 0 ? 0 : arena->pieces[i - 1].offset + arena->pieces[i - 1].size;
}

static uint32_t GapEnd(const VoxelArena *arena, unsigned i)
{
    return i == arena->count ? arena->size : arena->pieces[i].offset;
}

void *VoxelArena_Alloc(VoxelArena *arena, uint32_t size, bool fromTop)
{
    unsigned at = arena->count + 1;
    uint32_t offset = 0;

    if (arena->base == NULL || size == 0 || arena->count >= arena->capacity)
        return NULL;
    size = (size + arena->align - 1) & ~(arena->align - 1);
    if (size > arena->size)
        return NULL;
    /* Gaps 0..count, the lowest first - or the highest, from the top. */
    for (unsigned n = 0; n <= arena->count; ++n)
    {
        unsigned i = fromTop ? arena->count - n : n;
        uint32_t start = GapStart(arena, i), end = GapEnd(arena, i);

        if (end - start >= size)
        {
            at = i;
            offset = fromTop ? end - size : start;
            break;
        }
    }
    if (at > arena->count)
        return NULL;
    memmove(&arena->pieces[at + 1], &arena->pieces[at],
            (arena->count - at) * sizeof(arena->pieces[0]));
    arena->pieces[at].offset = offset;
    arena->pieces[at].size = size;
    ++arena->count;
    arena->used += size;
    return arena->base + offset;
}

void VoxelArena_Free(VoxelArena *arena, void *block)
{
    uint32_t offset;
    unsigned lo = 0, hi;

    if (block == NULL || arena->base == NULL)
        return;
    offset = (uint32_t)((uint8_t *)block - arena->base);
    hi = arena->count;
    while (lo < hi)
    {
        unsigned mid = (lo + hi) / 2;

        if (arena->pieces[mid].offset < offset)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo >= arena->count || arena->pieces[lo].offset != offset)
        return; /* not one of ours */
    arena->used -= arena->pieces[lo].size;
    memmove(&arena->pieces[lo], &arena->pieces[lo + 1],
            (arena->count - lo - 1) * sizeof(arena->pieces[0]));
    --arena->count;
}

int VoxelArena_PieceOf(const VoxelArena *arena, const void *block)
{
    uint32_t offset;
    unsigned lo = 0, hi;

    if (block == NULL || arena->base == NULL)
        return -1;
    offset = (uint32_t)((const uint8_t *)block - arena->base);
    hi = arena->count;
    while (lo < hi)
    {
        unsigned mid = (lo + hi) / 2;

        if (arena->pieces[mid].offset < offset)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < arena->count && arena->pieces[lo].offset == offset ? (int)lo : -1;
}

uint32_t VoxelArena_LargestGapFreeing(const VoxelArena *arena, const bool *freed)
{
    uint32_t largest = 0, start = 0;

    if (arena->base == NULL)
        return 0;
    /* A run ends at the next piece that stays; the last at the arena's end. */
    for (unsigned i = 0; i <= arena->count; ++i)
    {
        uint32_t end;

        if (i < arena->count && freed[i])
            continue;
        end = i == arena->count ? arena->size : arena->pieces[i].offset;
        if (end - start > largest)
            largest = end - start;
        if (i < arena->count)
            start = arena->pieces[i].offset + arena->pieces[i].size;
    }
    return largest;
}

bool VoxelArena_FitsFreeing(const VoxelArena *arena, const bool *freed, uint32_t bytes)
{
    return VoxelArena_LargestGapFreeing(arena, freed) >= ((bytes + arena->align - 1u) & ~(arena->align - 1u));
}

uint32_t VoxelArena_LargestGap(const VoxelArena *arena)
{
    uint32_t largest = 0;

    if (arena->base == NULL)
        return 0;
    for (unsigned i = 0; i <= arena->count; ++i)
    {
        uint32_t gap = GapEnd(arena, i) - GapStart(arena, i);

        if (gap > largest)
            largest = gap;
    }
    return largest;
}

void *VoxelArena_Move(VoxelArena *arena, void *block, uint32_t offset)
{
    int i = VoxelArena_PieceOf(arena, block);
    uint32_t size;

    if (i < 0 || offset % arena->align != 0)
        return NULL;
    size = arena->pieces[i].size;
    /* Between its neighbours, which stay: the order by address holds. */
    if (offset < GapStart(arena, (unsigned)i) || offset > GapEnd(arena, (unsigned)i + 1u) - size)
        return NULL;
    arena->pieces[i].offset = offset;
    return arena->base + offset;
}

unsigned VoxelArena_SlideCopies(uint32_t held, uint32_t distance, uint32_t copyMax)
{
    uint32_t step = distance < copyMax ? distance : copyMax;

    if (held == 0)
        return 0;
    if (step == 0)
        return ~0u;
    return (unsigned)((held - 1u) / step + 1u);
}

unsigned VoxelArena_SlideSteps(uint32_t from, uint32_t to, uint32_t held, uint32_t copyMax,
                               VoxelArenaCopy *steps, unsigned max)
{
    uint32_t step = to < from ? from - to : to - from;
    unsigned count = VoxelArena_SlideCopies(held, step, copyMax);

    if (count > max)
        return count;
    if (step > copyMax)
        step = copyMax;
    /* Down, the lowest bytes first; up, the highest: each copy only ever
     * writes over bytes an earlier one has already read, never over any it
     * or a later one still reads, as it is no longer than the distance. */
    for (unsigned k = 0; k < count; ++k)
    {
        uint32_t start = to < from ? k * step : (held - 1u) / step * step - k * step;
        uint32_t size = held - start < step ? held - start : step;

        steps[k].from = from + start;
        steps[k].to = to + start;
        steps[k].size = size;
    }
    return count;
}

/*
 * The search VoxelArena_SlidePlan makes, a run of the arena at a time: the
 * pieces between two that stay (or an end of the arena), the room [a, b)
 * they leave, and of those pieces the ones that may slide, by address. A
 * way costs the bytes it copies and frees, and a price for each slide (one
 * a frame: the fewer, the sooner the gap) and for each copy.
 */
#define VOXEL_ARENA_SLIDE_COST (64u * 1024u)
#define VOXEL_ARENA_COPY_COST (4u * 1024u)

typedef struct
{
    const VoxelArena *arena;
    const uint8_t *kind;
    const uint32_t *held;
    uint32_t size, copyMax;
    unsigned copiesMax;
    unsigned lo, hi;
    uint32_t a, b;
    unsigned slides[VOXEL_ARENA_PLAN_PIECES], count;
    bool found, there;
    VoxelArenaSlide best;
} SlideSearch;

/*
 * The gap [s, s + size) with the run's first `below` sliding pieces under it
 * and the others over it, each slid only as far as it must be. The pieces
 * under it go first from the lowest, those over it from the highest, so a
 * slide only ever passes over free room and pieces to be freed.
 */
static void SlideTry(SlideSearch *search, unsigned below, uint32_t s)
{
    const VoxelArenaPiece *pieces = search->arena->pieces;
    uint32_t to[VOXEL_ARENA_PLAN_PIECES], limit = s, cost = 0;
    int down = -1, up = -1, first;

    for (unsigned q = below; q-- > 0;)
    {
        const VoxelArenaPiece *p = &pieces[search->slides[q]];

        to[q] = p->offset + p->size <= limit ? p->offset : limit - p->size;
        limit = to[q];
    }
    limit = s + search->size;
    for (unsigned q = below; q < search->count; ++q)
    {
        const VoxelArenaPiece *p = &pieces[search->slides[q]];

        to[q] = p->offset >= limit ? p->offset : limit;
        limit = to[q] + p->size;
    }
    for (unsigned q = 0; q < search->count; ++q)
    {
        unsigned i = search->slides[q];
        const VoxelArenaPiece *p = &pieces[i];
        uint32_t low, high;
        unsigned copies;

        if (to[q] == p->offset)
            continue;
        low = to[q] < p->offset ? to[q] : p->offset + p->size;
        high = to[q] < p->offset ? p->offset : to[q] + p->size;
        copies = VoxelArena_SlideCopies(search->held[i], high - low, search->copyMax);
        if (copies > search->copiesMax)
            return;
        cost += search->held[i] + VOXEL_ARENA_SLIDE_COST + copies * VOXEL_ARENA_COPY_COST;
        /* what it passes over is freed before it goes */
        for (unsigned f = search->lo; f < search->hi; ++f)
            if (search->kind[f] == VOXEL_ARENA_FREED && pieces[f].offset < high
             && pieces[f].offset + pieces[f].size > low)
                cost += pieces[f].size;
        if (q < below && down < 0)
            down = (int)q;
        if (q >= below)
            up = (int)q;
    }
    first = down >= 0 ? down : up;
    if (first < 0)
        search->there = true; /* nothing need slide */
    if (first < 0 || (search->found && cost >= search->best.cost))
        return;
    search->found = true;
    search->best.piece = (int)search->slides[first];
    search->best.offset = to[first];
    search->best.cost = cost;
}

/*
 * Every split of the run's sliding pieces into those under the gap and those
 * over it, and for each every place of the gap where what slides changes:
 * against all of them under it or all over it, or where one of them just
 * stays, those between it and the gap packed against it. What a split costs
 * only changes there, so the cheapest way is among them - and once its first
 * slide is made it is still among them, cheaper by that slide.
 */
static void SlideRun(SlideSearch *search)
{
    const VoxelArenaPiece *pieces = search->arena->pieces;
    uint32_t total = 0, under = 0;

    for (unsigned q = 0; q < search->count; ++q)
        total += pieces[search->slides[q]].size;
    if (search->b - search->a < total || search->b - search->a - total < search->size)
        return;
    for (unsigned below = 0; below <= search->count; ++below)
    {
        uint32_t lowest = search->a + under;
        uint32_t highest = search->b - search->size - (total - under);
        uint32_t packed = 0;

        SlideTry(search, below, lowest);
        SlideTry(search, below, highest);
        for (unsigned q = below; q-- > 0;)
        {
            uint32_t s;

            packed += pieces[search->slides[q]].size;
            s = pieces[search->slides[q]].offset + packed;
            if (s > lowest && s < highest)
                SlideTry(search, below, s);
        }
        packed = 0;
        for (unsigned q = below; q < search->count; ++q)
        {
            uint32_t at = pieces[search->slides[q]].offset;

            if (at >= packed + search->size && at - packed - search->size > lowest
             && at - packed - search->size < highest)
                SlideTry(search, below, at - packed - search->size);
            packed += pieces[search->slides[q]].size;
        }
        if (below < search->count)
            under += pieces[search->slides[below]].size;
    }
}

bool VoxelArena_SlidePlan(const VoxelArena *arena, const uint8_t *kind, const uint32_t *held,
                          uint32_t size, uint32_t copyMax, unsigned copiesMax, VoxelArenaSlide *first)
{
    SlideSearch search;
    uint32_t a = 0;
    unsigned lo = 0;

    if (arena->base == NULL || size == 0 || size > arena->size || copyMax == 0)
        return false;
    memset(&search, 0, sizeof(search));
    search.arena = arena;
    search.kind = kind;
    search.held = held;
    search.size = (size + arena->align - 1) & ~(arena->align - 1);
    search.copyMax = copyMax;
    search.copiesMax = copiesMax;
    for (unsigned i = 0; i <= arena->count; ++i)
    {
        bool many = false;

        if (i < arena->count && kind[i] != VOXEL_ARENA_STAYS)
            continue;
        search.lo = lo;
        search.hi = i;
        search.a = a;
        search.b = i == arena->count ? arena->size : arena->pieces[i].offset;
        search.count = 0;
        for (unsigned p = lo; p < i; ++p)
            if (kind[p] == VOXEL_ARENA_SLIDES)
            {
                if (search.count == VOXEL_ARENA_PLAN_PIECES)
                    many = true;
                else
                    search.slides[search.count++] = p;
            }
        if (!many)
            SlideRun(&search);
        if (i < arena->count)
        {
            a = arena->pieces[i].offset + arena->pieces[i].size;
            lo = i + 1;
        }
    }
    if (search.there)
        return false;
    if (search.found && first != NULL)
        *first = search.best;
    return search.found;
}
