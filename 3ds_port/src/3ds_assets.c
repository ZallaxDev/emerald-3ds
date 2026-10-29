/*
 * ARM11 asset resolver.
 *
 * The game keeps the INCBIN stub addresses the linker produced, and every
 * access is remapped to an external payload, supporting exact stubs,
 * `base + offset` members of a concatenated group and interior pointers of a
 * containing asset.
 *
 * The indices are about 0.5 MiB, so they stay resident and every lookup is a
 * binary search in RAM with no I/O at all. Payloads keep a bounded,
 * reclaimable cache.
 */

/* libc first: global.h defines min/max/abs as macros and would break the
 * newlib declarations if it were included before them. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port_platform.h"
#include "port_log.h"
#include "port_prof.h"
#include "3ds_assets.h"
#include "3ds_data.h"
#include "3ds_platform.h"

u32 GetDecompressedDataSize(const u32 *ptr);

#define ASSET_INDEX_PATH "romfs:/assets/asset_index.bin"
#define ASSET_PTR_INDEX_PATH "romfs:/assets/asset_ptr_index.bin"
#define ASSET_MAP_PATH "romfs:/assets/asset_map.txt"

struct AssetMapEntry
{
    u32 addr;
    u32 size;
    u32 pathOffset;
};

struct AssetPtrMapEntry
{
    u32 ptr;
    u32 base;
    u32 offset;
};

/* One payload per asset index; `stamp` is a frame number, never a byte count. */
struct AssetPayload
{
    u8 *data;
    u32 size;
    u32 stamp;
};

static struct AssetMapEntry *sEntries;
static struct AssetPtrMapEntry *sPtrEntries;
static struct AssetPayload *sPayloads;
/*
 * Every stub address and every grouped pointer the index knows lies in
 * [sLowest, sHighest]. The game's copies and decompressions all ask whether
 * their source is an asset (CpuSet, LZ77...), and nearly all of them come
 * from RAM: one comparison answers those instead of three binary searches.
 */
static u32 sLowest = 0xFFFFFFFFu, sHighest;
static char *sPathBlob;
static u32 sPathBlobSize;
static u32 sEntryCount;
static u32 sPtrEntryCount;
static bool sLoaded;
static bool sTried;

static struct CtrAssetStats sStats;
static u32 sBudget = CTR_ASSET_CACHE_BUDGET;
static u32 sStamp;
/* The payload table's lock, and the payloads the warm-up worker has claimed
 * and is reading (see the payload cache below). */
static CtrLock sLock;
static volatile u8 *sWarming;

static void *ReadWholeFile(const char *path, u32 *outSize)
{
    FILE *file = fopen(path, "rb");
    long size;
    void *buffer;

    if (file == NULL)
        return NULL;
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) <= 0
     || fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        return NULL;
    }
    buffer = malloc((size_t)size + 1);
    if (buffer == NULL)
    {
        fclose(file);
        return NULL;
    }
    if (fread(buffer, 1, (size_t)size, file) != (size_t)size)
    {
        fclose(file);
        free(buffer);
        return NULL;
    }
    fclose(file);
    ((u8 *)buffer)[size] = 0;
    *outSize = (u32)size;
    return buffer;
}

static bool LoadAssetMap(void)
{
    u32 indexBytes = 0, ptrBytes = 0;
    void *index;
    void *ptrIndex;
    u32 i;

    if (sTried)
        return sLoaded;
    sTried = true;

    index = ReadWholeFile(ASSET_INDEX_PATH, &indexBytes);
    if (index == NULL || indexBytes % sizeof(struct AssetMapEntry) != 0 || indexBytes == 0)
    {
        free(index);
        PORT_LOG("[FS] asset index missing or malformed (%lu bytes)\n", (unsigned long)indexBytes);
        sTried = false;
        return false;
    }
    sPathBlob = ReadWholeFile(ASSET_MAP_PATH, &sPathBlobSize);
    if (sPathBlob == NULL)
    {
        free(index);
        PORT_LOG("[FS] asset path table missing\n");
        sTried = false;
        return false;
    }
    /* asset_map.txt is a line-oriented table; each path has to become its own
     * C string or a lookup returns that path plus the rest of the file. */
    for (i = 0; i < sPathBlobSize; i++)
    {
        if (sPathBlob[i] == '\n' || sPathBlob[i] == '\r')
            sPathBlob[i] = '\0';
    }

    sEntries = index;
    sEntryCount = indexBytes / sizeof(struct AssetMapEntry);

    /* The pointer map is optional: a build with no grouped INCBIN has none. */
    ptrIndex = ReadWholeFile(ASSET_PTR_INDEX_PATH, &ptrBytes);
    if (ptrIndex != NULL && ptrBytes % sizeof(struct AssetPtrMapEntry) == 0)
    {
        sPtrEntries = ptrIndex;
        sPtrEntryCount = ptrBytes / sizeof(struct AssetPtrMapEntry);
    }
    else
    {
        free(ptrIndex);
    }

    sPayloads = calloc(sEntryCount, sizeof(*sPayloads));
    if (sPayloads == NULL)
    {
        PORT_LOG("[FS] asset payload table allocation failed\n");
        sTried = false;
        return false;
    }

    /* A malformed path offset would otherwise be read as arbitrary memory. */
    for (i = 0; i < sEntryCount; i++)
    {
        if (sEntries[i].pathOffset >= sPathBlobSize)
        {
            PORT_LOG("[FS] asset %lu has out-of-range path offset\n", (unsigned long)i);
            sTried = false;
            return false;
        }
        if (i != 0 && sEntries[i].addr < sEntries[i - 1].addr)
        {
            PORT_LOG("[FS] asset index is not sorted at row %lu\n", (unsigned long)i);
            sTried = false;
            return false;
        }
    }

    for (i = 0; i < sEntryCount; i++)
    {
        if (sEntries[i].addr < sLowest) sLowest = sEntries[i].addr;
        if (sEntries[i].addr > sHighest) sHighest = sEntries[i].addr;
    }
    for (i = 0; i < sPtrEntryCount; i++)
    {
        if (sPtrEntries[i].ptr < sLowest) sLowest = sPtrEntries[i].ptr;
        if (sPtrEntries[i].ptr > sHighest) sHighest = sPtrEntries[i].ptr;
    }

    CtrLock_Init(&sLock);
    sStats.entries = sEntryCount;
    sStats.pointerEntries = sPtrEntryCount;
    sStats.indexBytes = indexBytes + ptrBytes + sPathBlobSize
                      + sEntryCount * (u32)sizeof(*sPayloads);
    sLoaded = true;
    PORT_LOG("[FS] assets: %lu entries, %lu ptr rows, %lu KiB resident index\n",
            (unsigned long)sEntryCount, (unsigned long)sPtrEntryCount,
            (unsigned long)(sStats.indexBytes >> 10));
    return true;
}

static const struct AssetMapEntry *GetAssetEntry(u32 idx)
{
    return &sEntries[idx];
}

static const char *GetAssetPathByIndex(u32 idx)
{
    return sPathBlob + sEntries[idx].pathOffset;
}

static s32 FindAssetIndexByAddr(u32 addr)
{
    s32 lo = 0;
    s32 hi = (s32)sEntryCount - 1;

    while (lo <= hi)
    {
        s32 mid = lo + ((hi - lo) >> 1);
        u32 midAddr = sEntries[mid].addr;

        if (midAddr == addr)
            return mid;
        if (midAddr < addr)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

static bool RemapPointerViaPtrMap(u32 ptr, u32 *outBase, u32 *outOffset)
{
    s32 lo = 0;
    s32 hi = (s32)sPtrEntryCount - 1;
    s32 first = -1;
    bool found = false;
    u32 bestOffset = 0xFFFFFFFF;

    if (sPtrEntryCount == 0)
        return false;

    while (lo <= hi)
    {
        s32 mid = lo + ((hi - lo) >> 1);

        if (sPtrEntries[mid].ptr >= ptr)
        {
            if (sPtrEntries[mid].ptr == ptr)
                first = mid;
            hi = mid - 1;
        }
        else
        {
            lo = mid + 1;
        }
    }
    if (first < 0)
        return false;

    /* Several groups can start at the same address; keep the tightest match. */
    for (; first < (s32)sPtrEntryCount && sPtrEntries[first].ptr == ptr; first++)
    {
        u32 base = sPtrEntries[first].base;
        u32 offset = sPtrEntries[first].offset;
        s32 assetIdx = FindAssetIndexByAddr(base);

        if (assetIdx < 0 || offset >= GetAssetEntry(assetIdx)->size)
            continue;
        if (!found || offset < bestOffset)
        {
            found = true;
            bestOffset = offset;
            *outBase = base;
            *outOffset = offset;
        }
    }
    return found;
}

static s32 FindAssetIndexForPointer(u32 ptr, u32 *outOffset)
{
    u32 base, extraOffset;
    s32 idx;

    if (ptr < sLowest || ptr > sHighest)
        return -1;
    idx = FindAssetIndexByAddr(ptr);

    if (idx >= 0)
    {
        *outOffset = 0;
        return idx;
    }
    if (RemapPointerViaPtrMap(ptr, &base, &extraOffset))
    {
        idx = FindAssetIndexByAddr(base);
        if (idx >= 0)
        {
            *outOffset = extraOffset;
            return idx;
        }
    }
    return -1;
}

static s32 FindInteriorAssetIndexForSizedPointer(u32 ptr, u32 size, u32 *outOffset)
{
    s32 lo = 0;
    s32 hi = (s32)sEntryCount - 1;
    s32 candidate;

    if (sEntryCount == 0)
        return -1;

    while (lo <= hi)
    {
        s32 mid = lo + ((hi - lo) >> 1);

        if (sEntries[mid].addr <= ptr)
            lo = mid + 1;
        else
            hi = mid - 1;
    }

    for (candidate = hi; candidate >= 0; candidate--)
    {
        u32 base = sEntries[candidate].addr;
        u32 assetSize = sEntries[candidate].size;
        u32 offset;

        if (base > ptr)
            continue;
        if (assetSize == 0 || ptr >= base + assetSize)
            continue;
        offset = ptr - base;
        if (offset == 0)
            continue;
        if (size != 0 && offset + size > assetSize)
            continue;
        *outOffset = offset;
        return candidate;
    }
    return -1;
}

static s32 FindAssetIndexForSizedPointer(u32 ptr, u32 size, u32 *outOffset)
{
    s32 idx;

    if (ptr < sLowest || ptr > sHighest)
        return -1;
    idx = FindAssetIndexByAddr(ptr);

    if (idx >= 0)
    {
        if (size != 0 && size > GetAssetEntry(idx)->size)
            return -1;
        *outOffset = 0;
        return idx;
    }
    idx = FindAssetIndexForPointer(ptr, outOffset);
    if (idx < 0)
        return -1;
    if (size != 0 && *outOffset + size > GetAssetEntry(idx)->size)
        return -1;
    return idx;
}

/* ── Payload cache ──────────────────────────────────────────────────────── */

/*
 * The game asks for a payload the moment it needs it, in the middle of a
 * frame: the scene loads of the intro and the title spent 11 ms of a frame
 * in reads each, far more on a console's SD card, where each read also
 * refills a 64 KiB buffer. All the payloads together are about 4 MiB, so a
 * worker reads every one of them in the background from boot (WarmWorker),
 * the pack's in its own order and in large runs. A payload the game asks for
 * before the worker got to it is read on the spot as before, and one the
 * worker is reading is waited for.
 *
 * sLock guards the payload table and the statistics. The game thread holds
 * it while it reads a payload itself; the worker reads without it and only
 * takes it to publish.
 */

static u8 *ReadPayload(u32 idx)
{
    const char *path = GetAssetPathByIndex(idx);
    u32 expected = sEntries[idx].size;
    FILE *file;
    u8 *buffer;

    file = CtrData_Open(path);
    if (file == NULL)
    {
        PORT_LOG("[ERROR] asset open failed: %s\n", path);
        return NULL;
    }
    buffer = malloc(expected != 0 ? expected : 1);
    if (buffer == NULL)
    {
        fclose(file);
        PORT_LOG("[ERROR] asset alloc failed: %s (%lu bytes)\n", path, (unsigned long)expected);
        return NULL;
    }
    if (fread(buffer, 1, expected, file) != expected)
    {
        fclose(file);
        free(buffer);
        PORT_LOG("[ERROR] asset short read: %s\n", path);
        return NULL;
    }
    fclose(file);
    return buffer;
}

/* Under sLock. */
static void Publish(u32 idx, u8 *buffer)
{
    /* Graphics with English words, redrawn for a translated build. Here,
     * where the demand read, the warm-up worker and the prefetch all pass. */
    Port_TransformAsset(GetAssetPathByIndex(idx), buffer, sEntries[idx].size);
    sPayloads[idx].data = buffer;
    sPayloads[idx].size = sEntries[idx].size;
    sPayloads[idx].stamp = sStamp;
    sStats.bytes += sEntries[idx].size;
    ++sStats.loads;
    if (sStats.bytes > sStats.peakBytes)
        sStats.peakBytes = sStats.bytes;
}

/*
 * A payload the game had to wait for names its neighbours: the rest of a
 * tileset's folder (its animation frames are asked for one by one over the
 * next seconds, 3-19 ms each on an Old 3DS while the warm-up still runs), or
 * the rest of a source file's generated tables. The warm-up worker reads
 * those next (WarmHinted). Under sLock.
 */
static char sWarmHint[96];
static bool sWarmHintPending;

static void HintNeighbours(const char *path)
{
    static const char generated[] = "generated/assets/";
    size_t length = 0;

    if (strncmp(path, generated, sizeof(generated) - 1) == 0)
    {
        const char *group = strstr(path + sizeof(generated) - 1, "__");

        if (group != NULL)
            length = (size_t)(group - path) + 2;
    }
    else if (strncmp(path, "data/tilesets/", 14) == 0)
    {
        /* data/tilesets/<primary|secondary>/<name>/ */
        const char *p = path + 14;

        p = strchr(p, '/');
        if (p != NULL)
            p = strchr(p + 1, '/');
        if (p != NULL)
            length = (size_t)(p - path) + 1;
    }
    if (length == 0 || length >= sizeof(sWarmHint))
        return;
    if (sWarmHintPending && strncmp(sWarmHint, path, length) == 0 && sWarmHint[length] == '\0')
        return;
    memcpy(sWarmHint, path, length);
    sWarmHint[length] = '\0';
    sWarmHintPending = true;
}

static u8 *GetPayload(u32 idx)
{
    u8 *data;

    CtrLock_Lock(&sLock);
    while (sPayloads[idx].data == NULL && sWarming != NULL && sWarming[idx])
    {
        CtrLock_Unlock(&sLock);
        CtrPlatform_SleepUs(200);
        CtrLock_Lock(&sLock);
    }
    data = sPayloads[idx].data;
    if (data == NULL)
    {
        static unsigned sDemandLogged;
        u64 start = CtrPlatform_Ticks();

        ++sStats.misses;
        if (sWarming != NULL)
            HintNeighbours(GetAssetPathByIndex(idx));
        data = ReadPayload(idx);
        if (data != NULL)
            Publish(idx, data);
        else
            ++sStats.errors;
        Port_ProfAdd(PORT_PROF_IO, (uint32_t)start);
        /* The game waited for this one: worth knowing which, on hardware.
         * Capped, so a run of them cannot flood the log. */
        if (sDemandLogged < 96)
        {
            ++sDemandLogged;
            CtrLog_Write(CTR_LOG_FS, "asset read on demand: %s (%lu B, %.2f ms)", GetAssetPathByIndex(idx),
                         (unsigned long)sEntries[idx].size, CtrPlatform_TickMs(CtrPlatform_Ticks() - start));
        }
    }
    else
    {
        ++sStats.hits;
    }
    if (data != NULL)
        sPayloads[idx].stamp = sStamp;
    CtrLock_Unlock(&sLock);
    return data;
}

/* What the worker may fill the cache up to: never enough to make the
 * collector evict what the game is using. */
#define WARM_BUDGET_SHARE 2u
/* One pack read covers neighbours up to this far apart, up to this much. */
#define WARM_GAP (4u * 1024u)
#define WARM_RUN (256u * 1024u)

typedef struct
{
    u64 offset;
    u32 size, idx, rank;
} WarmEntry;

/*
 * What the game shows first is read first: the boot runs straight into the
 * copyright screen, the intro and the title, and then the menus. The rest
 * follows in the pack's (or the index's) order.
 */
static const char *const sWarmFirst[] = {
    "graphics/intro/", "graphics/title_screen/", "graphics/rayquaza_scene/",
    "graphics/pokemon/rayquaza/", "graphics/pokemon/groudon/", "graphics/pokemon/kyogre/",
    /* The field the save continues on: the primary tilesets (100 KiB, with
     * the water and flower animations every map shows) and the map name
     * pop-up were read in the frame, a dozen of them, in the first minute. */
    "data/tilesets/primary/", "generated/assets/build_root_src_map_name_popup__",
    /* The follower's ball, as it first comes out there: its two files were
     * 38 ms of that frame. */
    "graphics/balls/",
    "graphics/battle_anims/", "graphics/battle_interface/", "graphics/battle_environment/",
    "graphics/battle_transitions/", "graphics/fonts/", "graphics/text_window/", "graphics/interface/",
    "graphics/birch_speech/", "graphics/misc/",
};
#define WARM_RANKS (sizeof(sWarmFirst) / sizeof(sWarmFirst[0]))

static u32 WarmRank(const char *path)
{
    for (u32 i = 0; i < WARM_RANKS; ++i)
        if (strncmp(path, sWarmFirst[i], strlen(sWarmFirst[i])) == 0)
            return i;
    return WARM_RANKS;
}

static int CompareWarmEntries(const void *a, const void *b)
{
    const WarmEntry *x = a, *y = b;

    if (x->rank != y->rank)
        return x->rank < y->rank ? -1 : 1;
    if (x->offset != y->offset)
        return x->offset < y->offset ? -1 : 1;
    return x->idx < y->idx ? -1 : x->idx > y->idx;
}

/* Set at exit (CtrAssets_StopWarmup): the worker stops before the file
 * system it reads from is closed. */
static volatile bool sWarmStop, sWarmRunning;

static bool WarmRoom(void)
{
    return !sWarmStop && sStats.bytes < sBudget / WARM_BUDGET_SHARE;
}

/* Claims the payloads [first, end) of the list that nobody holds yet. */
static unsigned WarmClaim(const WarmEntry *list, unsigned first, unsigned end)
{
    unsigned claimed = 0;

    CtrLock_Lock(&sLock);
    for (unsigned i = first; i < end; ++i)
        if (sPayloads[list[i].idx].data == NULL)
        {
            sWarming[list[i].idx] = 1;
            ++claimed;
        }
    CtrLock_Unlock(&sLock);
    return claimed;
}

static void WarmRelease(const WarmEntry *list, unsigned first, unsigned end)
{
    CtrLock_Lock(&sLock);
    for (unsigned i = first; i < end; ++i)
        sWarming[list[i].idx] = 0;
    CtrLock_Unlock(&sLock);
}

/* One payload into the cache, unless someone holds it or has it already. */
static void WarmOne(const WarmEntry *entry)
{
    u32 idx = entry->idx;
    u8 *data;

    if (WarmClaim(entry, 0, 1) == 0)
        return;
    data = ReadPayload(idx);
    CtrLock_Lock(&sLock);
    if (data != NULL && sPayloads[idx].data == NULL)
        Publish(idx, data), data = NULL;
    sWarming[idx] = 0;
    CtrLock_Unlock(&sLock);
    free(data);
}

/* The neighbours of what the game last waited for (HintNeighbours), first. */
static void WarmHinted(const WarmEntry *list, unsigned count)
{
    char hint[sizeof(sWarmHint)];
    size_t length;

    CtrLock_Lock(&sLock);
    if (!sWarmHintPending)
    {
        CtrLock_Unlock(&sLock);
        return;
    }
    memcpy(hint, sWarmHint, sizeof(hint));
    sWarmHintPending = false;
    CtrLock_Unlock(&sLock);
    length = strlen(hint);
    for (unsigned i = 0; i < count && WarmRoom(); ++i)
        if (sPayloads[list[i].idx].data == NULL
         && strncmp(GetAssetPathByIndex(list[i].idx), hint, length) == 0)
            WarmOne(&list[i]);
}

/* The pack: runs of neighbouring payloads in one read each. */
static void WarmPack(WarmEntry *list, unsigned count, u8 *run)
{
    unsigned first = 0;

    qsort(list, count, sizeof(*list), CompareWarmEntries);
    while (first < count && WarmRoom())
    {
        unsigned end = first + 1;
        u64 start = list[first].offset, stop = start + list[first].size;

        while (end < count && list[end].rank == list[first].rank
               && list[end].offset >= stop && list[end].offset - stop <= WARM_GAP
               && list[end].offset + list[end].size - start <= WARM_RUN)
        {
            stop = list[end].offset + list[end].size;
            ++end;
        }
        if (WarmClaim(list, first, end) != 0)
        {
            bool ok = CtrData_ReadRange(start, run, (u32)(stop - start));

            for (unsigned i = first; ok && i < end; ++i)
            {
                u32 idx = list[i].idx;
                u8 *copy;

                if (!sWarming[idx] || (copy = malloc(list[i].size ? list[i].size : 1)) == NULL)
                    continue;
                memcpy(copy, run + (list[i].offset - start), list[i].size);
                CtrLock_Lock(&sLock);
                if (sPayloads[idx].data == NULL)
                    Publish(idx, copy), copy = NULL;
                sWarming[idx] = 0;
                CtrLock_Unlock(&sLock);
                free(copy);
            }
            WarmRelease(list, first, end);
        }
        first = end;
    }
}

static void WarmWorker(void *arg)
{
    WarmEntry *list = arg;
    unsigned count = 0;
    u8 *run = NULL;
    u64 begin = CtrPlatform_Ticks();

    for (u32 idx = 0; idx < sEntryCount; ++idx)
        if (CtrData_Locate(GetAssetPathByIndex(idx), &list[count].offset, &list[count].size)
            && list[count].size == sEntries[idx].size)
        {
            list[count].idx = idx;
            list[count++].rank = WarmRank(GetAssetPathByIndex(idx));
        }
    if (count == sEntryCount && (run = malloc(WARM_RUN)) != NULL)
        WarmPack(list, count, run);
    else
    {
        /* RomFS or loose files: one file at a time. */
        for (u32 idx = 0; idx < sEntryCount; ++idx)
            list[idx] = (WarmEntry){0, sEntries[idx].size, idx, WarmRank(GetAssetPathByIndex(idx))};
        qsort(list, sEntryCount, sizeof(*list), CompareWarmEntries);
        count = 0;
        for (u32 i = 0; i < sEntryCount && WarmRoom(); ++i)
        {
            WarmHinted(list, sEntryCount);
            WarmOne(&list[i]);
        }
    }
    free(run);
    free(list);
    CtrLog_Write(CTR_LOG_FS, "assets warmed%s: %lu KiB of %lu payloads in %.0f ms (%s)",
                 sWarmStop ? " until exit" : "", (unsigned long)(sStats.bytes >> 10),
                 (unsigned long)sStats.loads, CtrPlatform_TickMs(CtrPlatform_Ticks() - begin),
                 count ? "pack runs" : "file by file");
    sWarmRunning = false;
}

/* Exiting with the warm-up under way, its reads failed in the log as the
 * RomFS went away under them: it stops first, a payload at most later. */
void CtrAssets_StopWarmup(void)
{
    sWarmStop = true;
    for (unsigned waited = 0; sWarmRunning && waited < 200; ++waited)
        CtrPlatform_SleepUs(5000);
}

void CtrAssets_StartWarmup(void)
{
    WarmEntry *list;

    if (!LoadAssetMap() || sWarming != NULL)
        return;
    sWarming = calloc(sEntryCount, 1);
    list = malloc(sEntryCount * sizeof(*list));
    sWarmRunning = true;
    if (sWarming == NULL || list == NULL
        /*
         * On the system core: on the application core, below the game, the
         * worker only ran while the game waited for the display, and read
         * the RomFS a file at a time over a whole minute (58 s on an Old
         * 3DS), the field asking for its tiles in the frame meanwhile. It
         * waits on the FS service far more than it computes.
         */
        || !CtrPlatform_StartThread(WarmWorker, list, 16 * 1024, 1))
    {
        free(list);
        sWarmRunning = false;
        CtrLog_Write(CTR_LOG_ERROR, "assets: no warm-up worker; payloads load on demand");
    }
}

const struct CtrAssetStats *CtrAssets_GetStats(void)
{
    sStats.budget = sBudget;
    return &sStats;
}

/*
 * Safe point only. A payload published to the game this frame must never be
 * freed underneath it, so the collector skips anything touched recently and
 * only runs between main-loop callbacks.
 */
void CtrAssets_Collect(void)
{
    u32 i;

    ++sStamp;
    if (!sLoaded || sStats.bytes <= sBudget)
        return;

    CtrLock_Lock(&sLock);
    for (i = 0; i < sEntryCount && sStats.bytes > sBudget; i++)
    {
        u32 age = sStamp - sPayloads[i].stamp;

        if (sPayloads[i].data == NULL || age < CTR_ASSET_CACHE_GRACE_FRAMES)
            continue;
        sStats.bytes -= sPayloads[i].size;
        free(sPayloads[i].data);
        sPayloads[i].data = NULL;
        sPayloads[i].size = 0;
        ++sStats.evictions;
    }
    CtrLock_Unlock(&sLock);
}

/* ── Public bridge API ──────────────────────────────────────────────────── */

bool Port_IsAssetStub(const void *ptr)
{
    u32 offset;

    return LoadAssetMap() && FindAssetIndexForPointer((u32)ptr, &offset) >= 0;
}

const char *Port_GetAssetPath(const void *ptr)
{
    u32 offset = 0;
    s32 idx;

    if (!LoadAssetMap())
        return NULL;
    idx = FindAssetIndexForPointer((u32)ptr, &offset);
    return idx >= 0 ? GetAssetPathByIndex(idx) : NULL;
}

u32 Port_GetAssetSize(const void *ptr)
{
    u32 offset;
    s32 idx;

    if (!LoadAssetMap())
        return 0;
    idx = FindAssetIndexForPointer((u32)ptr, &offset);
    if (idx < 0 || offset >= GetAssetEntry(idx)->size)
        return 0;
    return GetAssetEntry(idx)->size - offset;
}

u32 Port_GetAssetSizeExact(const void *base)
{
    s32 idx;

    if (base == NULL || !LoadAssetMap())
        return 0;
    idx = FindAssetIndexByAddr((u32)base);
    return idx >= 0 ? GetAssetEntry(idx)->size : 0;
}

u32 Port_GetAssetSizeSized(const void *ptr, u32 size)
{
    u32 offset;
    s32 idx;

    if (!LoadAssetMap())
        return 0;
    idx = FindAssetIndexForSizedPointer((u32)ptr, size, &offset);
    if (idx < 0 || offset >= GetAssetEntry(idx)->size)
        return 0;
    return GetAssetEntry(idx)->size - offset;
}

u32 Port_GetDecompressedAssetSize(const void *ptr)
{
    u32 offset;
    s32 idx;
    const u8 *data;

    if (!LoadAssetMap())
        return 0;
    idx = FindAssetIndexForPointer((u32)ptr, &offset);
    if (idx < 0)
        return GetDecompressedDataSize(ptr);
    data = GetPayload((u32)idx);
    if (data == NULL || offset + 4 > sPayloads[idx].size)
        return 0;
    data += offset;
    return (data[3] << 16) | (data[2] << 8) | data[1];
}

bool Port_LoadAssetToBuffer(const char *path, void *dest, u32 maxSize)
{
    FILE *file = CtrData_Open(path);
    long size;
    bool ok;

    if (file == NULL)
        return false;
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) <= 0
     || fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        return false;
    }
    if (maxSize != 0 && (u32)size > maxSize)
        size = (long)maxSize;
    ok = fread(dest, 1, (size_t)size, file) == (size_t)size;
    fclose(file);
    return ok;
}

bool Port_LoadAssetPointerToBuffer(const void *ptr, void *dest, u32 maxSize)
{
    u32 offset, size;
    s32 idx;
    const u8 *data;

    if (!LoadAssetMap())
        return false;
    idx = FindAssetIndexForPointer((u32)ptr, &offset);
    if (idx < 0)
        return false;
    data = GetPayload((u32)idx);
    if (data == NULL || offset >= sPayloads[idx].size)
        return false;
    size = sPayloads[idx].size - offset;
    if (maxSize != 0 && size > maxSize)
        size = maxSize;
    memcpy(dest, data + offset, size);
    return true;
}

bool Port_LoadAssetPointerToBufferSized(const void *ptr, void *dest, u32 size)
{
    u32 offset;
    s32 idx;
    const u8 *data;

    if (size == 0 || !LoadAssetMap())
        return false;
    idx = FindAssetIndexForSizedPointer((u32)ptr, size, &offset);
    if (idx < 0)
        return false;
    data = GetPayload((u32)idx);
    if (data == NULL || offset + size > sPayloads[idx].size)
        return false;
    memcpy(dest, data + offset, size);
    return true;
}

bool Port_LoadAssetRangeToBuffer(const void *base, u32 offset, void *dest, u32 size)
{
    s32 idx;
    const u8 *data;

    if (!LoadAssetMap())
        return false;
    idx = FindAssetIndexByAddr((u32)base);
    if (idx < 0 || offset > GetAssetEntry(idx)->size
     || size > GetAssetEntry(idx)->size - offset)
        return false;
    data = GetPayload((u32)idx);
    if (data == NULL || offset + size > sPayloads[idx].size)
        return false;
    memcpy(dest, data + offset, size);
    return true;
}

/* Caller owns the returned block; it is not part of the reclaimable cache. */
bool Port_LoadAssetPointerToHeap(const void *ptr, u8 **outBuf, u32 *outSize)
{
    u32 offset, size;
    s32 idx;
    const u8 *data;
    u8 *copy;

    if (outBuf == NULL || outSize == NULL)
        return false;
    *outBuf = NULL;
    *outSize = 0;
    if (!LoadAssetMap())
        return false;
    idx = FindAssetIndexForPointer((u32)ptr, &offset);
    if (idx < 0)
        return false;
    data = GetPayload((u32)idx);
    if (data == NULL || offset >= sPayloads[idx].size)
        return false;
    size = sPayloads[idx].size - offset;
    copy = malloc(size);
    if (copy == NULL)
        return false;
    memcpy(copy, data + offset, size);
    *outBuf = copy;
    *outSize = size;
    return true;
}

static const void *ResolveLoaded(s32 idx, u32 offset, const void *fallback)
{
    const u8 *data = GetPayload((u32)idx);

    if (data == NULL)
    {
        /*
         * A recognised stub is not graphic data. Handing it back would let the
         * caller read adjacent constants as tiles and hide the I/O failure, so
         * this aborts instead.
         */
        PORT_LOG("[ERROR] unresolved asset %ld (%s)\n", (long)idx, GetAssetPathByIndex((u32)idx));
        CtrAssets_Fatal(GetAssetPathByIndex((u32)idx));
        return fallback;
    }
    return data + offset;
}

/*
 * A graphic the game has just decompressed (LZ77UnCompWram/Vram, bios.c),
 * named by the asset its compressed data came from without the ".lz": the
 * Portuguese build redraws the English words in it (3ds_icons_ptbr.c).
 */
void Port_TransformDecompressed(const void *source, void *data, unsigned size)
{
    char plain[128];
    const char *path;
    size_t n;
    u32 offset = 0;
    s32 idx = FindAssetIndexForPointer((u32)source, &offset);

    if (idx < 0 || offset != 0 || data == NULL)
        return;
    path = GetAssetPathByIndex((u32)idx);
    n = strlen(path);
    if (n <= 3 || n - 3 >= sizeof(plain) || strcmp(path + n - 3, ".lz") != 0)
        return;
    memcpy(plain, path, n - 3);
    plain[n - 3] = '\0';
    Port_TransformAsset(plain, data, size);
}

const void *Port_ResolveAssetPointer(const void *ptr)
{
    u32 offset;
    s32 idx;
    const void *map = Port_ResolveMapAssetPointer(ptr);

    if (map != ptr)
        return map;
    if (!LoadAssetMap())
        return ptr;
    idx = FindAssetIndexForPointer((u32)ptr, &offset);
    if (idx < 0)
        return ptr;
    return ResolveLoaded(idx, offset, ptr);
}

const void *Port_ResolveAssetPointerSized(const void *ptr, u32 size)
{
    u32 offset;
    s32 idx;

    if (!LoadAssetMap())
        return ptr;
    idx = FindAssetIndexForSizedPointer((u32)ptr, size, &offset);
    if (idx < 0)
        return ptr;
    return ResolveLoaded(idx, offset, ptr);
}

const void *Port_ResolveAssetPointerInContainingAsset(const void *ptr, u32 size)
{
    u32 offset;
    s32 idx;

    if (!LoadAssetMap())
        return ptr;
    idx = FindInteriorAssetIndexForSizedPointer((u32)ptr, size, &offset);
    if (idx < 0)
        return Port_ResolveAssetPointerSized(ptr, size);
    return ResolveLoaded(idx, offset, ptr);
}

u32 Port_GetSpriteFrameSize(const void *base, u32 declaredSize)
{
    s32 idx;

    /* sizeof(INCBIN) describes the stub, not the image, in an external build. */
    if (declaredSize >= 32 || base == NULL || !LoadAssetMap())
        return declaredSize;
    idx = FindAssetIndexByAddr((u32)base);
    return idx >= 0 ? GetAssetEntry(idx)->size : declaredSize;
}

const void *Port_ResolveSpriteFramePointer(const void *base, u32 size, u32 offset)
{
    s32 idx;
    u32 assetSize;

    if (base == NULL || size == 0 || !LoadAssetMap())
        return NULL;
    idx = FindAssetIndexByAddr((u32)base);
    if (idx < 0)
        return (const u8 *)base + offset;
    assetSize = GetAssetEntry(idx)->size;
    if (offset >= assetSize || size > assetSize - offset)
        return NULL;
    return ResolveLoaded(idx, offset, NULL);
}

const void *Port_PeekSpriteFramePointer(const void *base, u32 size, u32 offset)
{
    s32 idx;
    u32 assetSize;

    if (base == NULL || size == 0 || !LoadAssetMap())
        return NULL;
    idx = FindAssetIndexByAddr((u32)base);
    if (idx < 0)
        return (const u8 *)base + offset;
    assetSize = GetAssetEntry(idx)->size;
    if (offset >= assetSize || size > assetSize - offset || sPayloads[idx].data == NULL)
        return NULL;
    return GetPayload((u32)idx) + offset;
}

bool CtrAssets_PrefetchFind(const void *ptr, CtrAssetPrefetch *out)
{
    u32 offset;
    s32 idx;

    if (ptr == NULL || Port_ResolveMapAssetPointer(ptr) != ptr || !LoadAssetMap())
        return false;
    idx = FindAssetIndexForPointer((u32)ptr, &offset);
    if (idx < 0)
        return false;
    /* In memory, or the warm-up worker is already reading it. */
    CtrLock_Lock(&sLock);
    if (sPayloads[idx].data != NULL || (sWarming != NULL && sWarming[idx]))
    {
        CtrLock_Unlock(&sLock);
        return false;
    }
    CtrLock_Unlock(&sLock);
    out->index = idx;
    out->size = sEntries[idx].size;
    out->path = GetAssetPathByIndex((u32)idx);
    return true;
}

void *CtrAssets_PrefetchRead(const CtrAssetPrefetch *request)
{
    FILE *file = CtrData_Open(request->path);
    u8 *buffer;

    if (file == NULL)
        return NULL;
    buffer = malloc(request->size != 0 ? request->size : 1);
    if (buffer != NULL && fread(buffer, 1, request->size, file) != request->size)
    {
        free(buffer);
        buffer = NULL;
    }
    fclose(file);
    return buffer;
}

void CtrAssets_PrefetchAdopt(const CtrAssetPrefetch *request, void *payload)
{
    s32 idx = request->index;

    if (payload == NULL)
        return;
    if (idx < 0 || (u32)idx >= sEntryCount || request->size != sEntries[idx].size)
    {
        free(payload);
        return;
    }
    /* Under the lock: the warm-up worker publishes too (Publish). */
    CtrLock_Lock(&sLock);
    if (sPayloads[idx].data != NULL)
    {
        CtrLock_Unlock(&sLock);
        free(payload);
        return;
    }
    Publish((u32)idx, payload);
    CtrLock_Unlock(&sLock);
}

void Port_AssetPreload(void)
{
    if (!LoadAssetMap())
        CtrAssets_Fatal("asset index could not be loaded from RomFS");
}
