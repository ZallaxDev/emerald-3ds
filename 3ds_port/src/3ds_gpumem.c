/*
 * GPU memory accounting (include/3ds_gpumem.h). The linker routes libctru's
 * VRAM and linear allocators through here (--wrap, Makefile), from the port
 * and from Citro3D/Citro2D alike, so what each scene holds, its peak and every
 * failed allocation are in the log with the code that made them - the budget
 * of phase F2, and the "largest block" a free total cannot tell.
 *
 * A failure is logged at once; the rest only when asked (CtrGpuMem_Report:
 * at a change of scene, from the frame profiler). Allocations are made from
 * more than one thread (the voxel streams), so the table has a lock.
 */
#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>

#include "3ds_gpumem.h"
#include "3ds_log.h"

void *__real_vramAlloc(size_t size);
void *__real_vramMemAlign(size_t size, size_t alignment);
void *__real_vramAllocAt(size_t size, vramAllocPos pos);
void *__real_vramMemAlignAt(size_t size, size_t alignment, vramAllocPos pos);
void *__real_vramRealloc(void *mem, size_t size);
void __real_vramFree(void *mem);
void *__real_linearAlloc(size_t size);
void *__real_linearMemAlign(size_t size, size_t alignment);
void *__real_linearRealloc(void *mem, size_t size);
void __real_linearFree(void *mem);
bool __real_C3D_TexInitWithParams(C3D_Tex *tex, C3D_TexCube *cube, C3D_TexInitParams p);
C3D_RenderTarget *__real_C3D_RenderTargetCreate(int width, int height, GPU_COLORBUF colorFmt,
                                                C3D_DEPTHTYPE depthFmt);
C3D_RenderTarget *__real_C3D_RenderTargetCreateFromTex(C3D_Tex *tex, GPU_TEXFACE face, int level,
                                                       C3D_DEPTHTYPE depthFmt);

enum { POOL_VRAM, POOL_LINEAR };

#define BLOCKS 4096     /* live allocations, open addressing */
#define SITES 96
#define FAIL_LINES 200

typedef struct
{
    void *ptr;
    uint32_t size;
    uint16_t site;
    uint8_t pool;
} Block;

typedef struct
{
    uint32_t address, live, peak, count;
    uint8_t pool, changed;
} Site;

static Block sBlocks[BLOCKS];
static Site sSites[SITES];
static unsigned sSiteCount;
static CtrGpuMemStats sStats;
static LightLock sLock = 1;
static unsigned sFailLines;
/* The code that called Citro3D, for the allocations Citro3D makes for it. */
static __thread uint32_t tOuter;

static CtrGpuPool *Pool(unsigned pool)
{
    return pool == POOL_VRAM ? &sStats.vram : &sStats.linear;
}

static unsigned SiteIndex(uint32_t address, unsigned pool)
{
    for (unsigned i = 0; i < sSiteCount; ++i)
        if (sSites[i].address == address && sSites[i].pool == pool)
            return i;
    if (sSiteCount == SITES)
        return SITES - 1;   /* the last one takes the rest */
    sSites[sSiteCount].address = address;
    sSites[sSiteCount].pool = pool;
    return sSiteCount++;
}

static unsigned Slot(const void *ptr)
{
    return ((uintptr_t)ptr >> 7) * 2654435761u % BLOCKS;
}

static void Added(void *ptr, size_t size, unsigned pool, uint32_t caller)
{
    if (ptr == NULL)
        return;
    LightLock_Lock(&sLock);
    unsigned i = Slot(ptr);

    for (unsigned n = 0; n < BLOCKS; ++n, i = (i + 1) % BLOCKS)
    {
        if (sBlocks[i].ptr == ptr)
            break;  /* already counted (a wrapped allocator calling another) */
        if (sBlocks[i].ptr == NULL)
        {
            unsigned site = SiteIndex(tOuter ? tOuter : caller, pool);
            CtrGpuPool *total = Pool(pool);

            sBlocks[i] = (Block){ptr, (uint32_t)size, (uint16_t)site, (uint8_t)pool};
            sSites[site].live += size;
            sSites[site].count++;
            sSites[site].changed = 1;
            if (sSites[site].live > sSites[site].peak) sSites[site].peak = sSites[site].live;
            total->live += size;
            total->count++;
            if (total->live > total->peak) total->peak = total->live;
            LightLock_Unlock(&sLock);
            return;
        }
    }
    if (sBlocks[i].ptr != ptr)
        ++sStats.untracked;
    LightLock_Unlock(&sLock);
}

static void Removed(void *ptr)
{
    if (ptr == NULL)
        return;
    LightLock_Lock(&sLock);
    unsigned i = Slot(ptr);

    for (unsigned n = 0; n < BLOCKS && sBlocks[i].ptr != NULL; ++n, i = (i + 1) % BLOCKS)
    {
        if (sBlocks[i].ptr != ptr)
            continue;
        Block block = sBlocks[i];
        Site *site = &sSites[block.site];

        site->live -= block.size;
        site->count--;
        site->changed = 1;
        Pool(block.pool)->live -= block.size;
        Pool(block.pool)->count--;
        /* Backward-shift deletion keeps every chain unbroken. */
        for (unsigned j = (i + 1) % BLOCKS; sBlocks[j].ptr != NULL; j = (j + 1) % BLOCKS)
        {
            unsigned home = Slot(sBlocks[j].ptr);

            if ((j > i && (home <= i || home > j)) || (j < i && home <= i && home > j))
            {
                sBlocks[i] = sBlocks[j];
                i = j;
            }
        }
        sBlocks[i].ptr = NULL;
        break;
    }
    LightLock_Unlock(&sLock);
}

static void MapVram(void);

static void Failed(size_t size, unsigned pool, uint32_t caller)
{
    /* A request retried every frame is one finding: logged the first time,
     * then with its count every 64 tries. */
    static uint32_t sLastSite, sLastSize, sRepeats;
    uint32_t site = tOuter ? tOuter : caller;

    ++Pool(pool)->failures;
    if (site == sLastSite && size == sLastSize && ++sRepeats % 64 != 0)
        return;
    if (site != sLastSite || size != sLastSize)
        sRepeats = 0;
    sLastSite = site;
    sLastSize = size;
    if (sFailLines >= FAIL_LINES)
        return;
    ++sFailLines;
    CtrLog_Write(CTR_LOG_ERROR, "GPUMEM fail %s size=%lu site=%08lx tries=%lu free=%lu largest=%lu live=%lu",
                 pool == POOL_VRAM ? "vram" : "linear", (unsigned long)size,
                 (unsigned long)site, (unsigned long)sRepeats + 1,
                 (unsigned long)(pool == POOL_VRAM ? vramSpaceFree() : linearSpaceFree()),
                 (unsigned long)(pool == POOL_VRAM ? CtrGpuMem_LargestVram() : 0),
                 (unsigned long)Pool(pool)->live);
    /* Where the free VRAM lies between what is held: the first few times. */
    if (pool == POOL_VRAM && sFailLines <= 8)
        MapVram();
}

/* The VRAM blocks held, in address order: "start+KiB@site", and the gaps
 * between them, "_KiB". What a "largest block" short of the free total
 * cannot say: which allocations cut the free VRAM into pieces. */
static void MapVram(void)
{
    enum { MAP_MAX = 64 };
    Block held[MAP_MAX];
    unsigned n = 0;
    uint32_t base = OS_VRAM_VADDR, end = OS_VRAM_VADDR + OS_VRAM_SIZE, at = base;
    char line[300];
    int used = 0;

    LightLock_Lock(&sLock);
    for (unsigned i = 0; i < BLOCKS && n < MAP_MAX; ++i)
        if (sBlocks[i].ptr != NULL && sBlocks[i].pool == POOL_VRAM)
            held[n++] = sBlocks[i];
    for (unsigned i = 1; i < n; ++i)
        for (unsigned j = i; j > 0 && (uintptr_t)held[j].ptr < (uintptr_t)held[j - 1].ptr; --j)
        {
            Block swap = held[j];

            held[j] = held[j - 1];
            held[j - 1] = swap;
        }
    for (unsigned i = 0; i <= n; ++i)
    {
        uint32_t start = i < n ? (uint32_t)(uintptr_t)held[i].ptr : end;

        if (start > at + 0x1000)
            used += snprintf(line + used, sizeof(line) - used, " _%lu", (unsigned long)((start - at) >> 10));
        if (i < n)
        {
            used += snprintf(line + used, sizeof(line) - used, " %lu+%lu@%08lx",
                             (unsigned long)((start - base) >> 10), (unsigned long)(held[i].size >> 10),
                             (unsigned long)sSites[held[i].site].address);
            at = start + held[i].size;
        }
        if (used > (int)sizeof(line) - 40 || i == n)
        {
            CtrLog_Write(CTR_LOG_VIDEO, "GPUMEM vram map%s", line);
            used = 0;
        }
    }
    LightLock_Unlock(&sLock);
}

#define CALLER ((uint32_t)(uintptr_t)__builtin_return_address(0))

static void *Track(void *ptr, size_t size, unsigned pool, uint32_t caller)
{
    if (ptr != NULL) Added(ptr, size, pool, caller);
    else if (size != 0) Failed(size, pool, caller);
    return ptr;
}

void *__wrap_vramAlloc(size_t size) { return Track(__real_vramAlloc(size), size, POOL_VRAM, CALLER); }
void *__wrap_vramMemAlign(size_t size, size_t alignment)
{
    return Track(__real_vramMemAlign(size, alignment), size, POOL_VRAM, CALLER);
}
void *__wrap_vramAllocAt(size_t size, vramAllocPos pos)
{
    return Track(__real_vramAllocAt(size, pos), size, POOL_VRAM, CALLER);
}
void *__wrap_vramMemAlignAt(size_t size, size_t alignment, vramAllocPos pos)
{
    return Track(__real_vramMemAlignAt(size, alignment, pos), size, POOL_VRAM, CALLER);
}
void *__wrap_vramRealloc(void *mem, size_t size)
{
    void *moved = __real_vramRealloc(mem, size);

    if (moved != NULL) { Removed(mem); Added(moved, size, POOL_VRAM, CALLER); }
    return moved;
}
void __wrap_vramFree(void *mem) { Removed(mem); __real_vramFree(mem); }

void *__wrap_linearAlloc(size_t size) { return Track(__real_linearAlloc(size), size, POOL_LINEAR, CALLER); }
void *__wrap_linearMemAlign(size_t size, size_t alignment)
{
    return Track(__real_linearMemAlign(size, alignment), size, POOL_LINEAR, CALLER);
}
void *__wrap_linearRealloc(void *mem, size_t size)
{
    void *moved = __real_linearRealloc(mem, size);

    if (moved != NULL) { Removed(mem); Added(moved, size, POOL_LINEAR, CALLER); }
    return moved;
}
void __wrap_linearFree(void *mem) { Removed(mem); __real_linearFree(mem); }

/* Citro3D's own allocations, charged to its caller. */
bool __wrap_C3D_TexInitWithParams(C3D_Tex *tex, C3D_TexCube *cube, C3D_TexInitParams p)
{
    uint32_t outer = tOuter;
    bool ok;

    if (!outer) tOuter = CALLER;
    ok = __real_C3D_TexInitWithParams(tex, cube, p);
    tOuter = outer;
    return ok;
}

C3D_RenderTarget *__wrap_C3D_RenderTargetCreateFromTex(C3D_Tex *tex, GPU_TEXFACE face, int level,
                                                       C3D_DEPTHTYPE depthFmt)
{
    uint32_t outer = tOuter;
    C3D_RenderTarget *target;

    if (!outer) tOuter = CALLER;
    target = __real_C3D_RenderTargetCreateFromTex(tex, face, level, depthFmt);
    tOuter = outer;
    return target;
}

C3D_RenderTarget *__wrap_C3D_RenderTargetCreate(int width, int height, GPU_COLORBUF colorFmt,
                                                C3D_DEPTHTYPE depthFmt)
{
    uint32_t outer = tOuter;
    C3D_RenderTarget *target;

    if (!outer) tOuter = CALLER;
    target = __real_C3D_RenderTargetCreate(width, height, colorFmt, depthFmt);
    tOuter = outer;
    return target;
}

/* VRAM is only allocated by the game thread, which is the one asking. */
uint32_t CtrGpuMem_LargestVram(void)
{
    uint32_t low = 0, high = vramSpaceFree() & ~0x7fu;

    while (high - low > 0x1000)
    {
        uint32_t mid = (low + (high - low) / 2) & ~0x7fu;
        void *probe = __real_vramMemAlign(mid, 0x80);

        if (probe) { __real_vramFree(probe); low = mid; }
        else high = mid;
    }
    return low;
}

void CtrGpuMem_Report(const char *why)
{
    if (!CtrLog_Enabled())
        return;
    CtrLog_Write(CTR_LOG_VIDEO, "GPUMEM %s: vram live=%luK peak=%luK n=%lu free=%luK largest=%luK fails=%lu"
                 " | linear live=%luK peak=%luK n=%lu free=%luK fails=%lu untracked=%lu",
                 why, (unsigned long)(sStats.vram.live >> 10), (unsigned long)(sStats.vram.peak >> 10),
                 (unsigned long)sStats.vram.count, (unsigned long)(vramSpaceFree() >> 10),
                 (unsigned long)(CtrGpuMem_LargestVram() >> 10), (unsigned long)sStats.vram.failures,
                 (unsigned long)(sStats.linear.live >> 10), (unsigned long)(sStats.linear.peak >> 10),
                 (unsigned long)sStats.linear.count, (unsigned long)(linearSpaceFree() >> 10),
                 (unsigned long)sStats.linear.failures, (unsigned long)sStats.untracked);
    /* Several sites to a line: "pool:site=live/peak xcount". */
    char line[360];
    int used = 0;

    LightLock_Lock(&sLock);
    for (unsigned i = 0; i < sSiteCount; ++i)
    {
        Site *site = &sSites[i];

        if (!site->changed)
            continue;
        site->changed = 0;
        used += snprintf(line + used, sizeof(line) - used, " %c:%08lx=%luK/%luK x%lu",
                         site->pool == POOL_VRAM ? 'v' : 'l', (unsigned long)site->address,
                         (unsigned long)(site->live >> 10), (unsigned long)(site->peak >> 10),
                         (unsigned long)site->count);
        if (used > (int)sizeof(line) - 48)
        {
            CtrLog_Write(CTR_LOG_VIDEO, "GPUMEM sites%s", line);
            used = 0;
        }
    }
    LightLock_Unlock(&sLock);
    if (used > 0)
        CtrLog_Write(CTR_LOG_VIDEO, "GPUMEM sites%s", line);
}
