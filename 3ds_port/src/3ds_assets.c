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
#include "3ds_pak.h"
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
/* The payload table's lock, each payload's state (PAYLOAD_*, see the payload
 * cache below) and the condition a reader signals once a payload it held is
 * published, failed or given back. */
static CtrLock sLock;
static CtrCond sPublished;
static u8 *sState;
enum
{
    PAYLOAD_EMPTY,      /* not in memory, nobody reading it */
    PAYLOAD_DEMAND,     /* reserved: read for a request, on whichever thread asked */
    PAYLOAD_WARM,       /* reserved: read by the warm-up worker */
    PAYLOAD_PREFETCH,   /* reserved: read by the voxel stream's prefetch */
    PAYLOAD_READY,      /* in memory: sPayloads[].data */
    PAYLOAD_FAILED,     /* a read of it failed: the workers leave it, a request tries once more */
    PAYLOAD_GIVEN_UP,   /* that try failed too: NULL at once from then on */
};
/* Port_TransformAsset keeps state of its own (3ds_icons_ptbr.c): one call at
 * a time, from any thread, and never under sLock. A zeroed LightLock is an
 * unlocked one, so it needs no start-up of its own. */
static CtrLock sTransformLock;

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
    sState = calloc(sEntryCount, 1);
    if (sPayloads == NULL || sState == NULL)
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
    CtrCond_Init(&sPublished);
    sStats.entries = sEntryCount;
    sStats.indexBytes = indexBytes + ptrBytes + sPathBlobSize
                      + sEntryCount * ((u32)sizeof(*sPayloads) + 1);
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
 * in reads each, far more on a console's SD card. All the payloads together
 * are about 4 MiB, so a worker reads every one of them in the background
 * from boot (WarmWorker), the pack's in its own order and in runs. A payload
 * the game asks for before the worker got to it is read on the spot.
 *
 * Each payload has a state (sState, PAYLOAD_*). Whoever reads one reserves
 * it under sLock (DEMAND, WARM or PREFETCH), reads it with the lock given
 * up, and under the lock again publishes it, marks it failed or gives it
 * back, then wakes whoever waits on sPublished. Nobody reads the card under
 * sLock, and nobody reads a payload another reader holds: they wait for it
 * (WAIT_BOUND_MS at most). The voxel world's prefetch publishes from its own
 * worker for that reason: the game may be waiting for that payload on the
 * very thread that would otherwise adopt it.
 *
 * A read that fails is not tried again by the workers; a request tries it
 * once more, and after that gets NULL at once. Each reader that gives a
 * payload up says so in one log line.
 */

/* A payload whole into a buffer of its own, or NULL and why in `result`.
 * From the pack, one read of exactly its bytes, checked against its CRC. */
static u8 *ReadPayload(u32 idx, CtrDataRead *result)
{
    u32 size = sEntries[idx].size;
    u8 *buffer = malloc(size != 0 ? size : 1);

    *result = buffer != NULL ? CtrData_ReadFile(GetAssetPathByIndex(idx), buffer, size) : CTR_DATA_READ_NOMEM;
    if (*result == CTR_DATA_READ_OK)
        return buffer;
    free(buffer);
    return NULL;
}

/* Graphics with English words, redrawn for a translated build: every
 * payload passes here once, on whichever thread read it, before it is
 * published. */
static void Transform(u32 idx, u8 *buffer)
{
    CtrLock_Lock(&sTransformLock);
    Port_TransformAsset(GetAssetPathByIndex(idx), buffer, sEntries[idx].size);
    CtrLock_Unlock(&sTransformLock);
}

/* Under sLock: `buffer`, transformed already, is the payload from now on. */
static void Publish(u32 idx, u8 *buffer)
{
    sPayloads[idx].data = buffer;
    sPayloads[idx].size = sEntries[idx].size;
    sPayloads[idx].stamp = sStamp;
    sState[idx] = PAYLOAD_READY;
    sStats.bytes += sEntries[idx].size;
    ++sStats.loads;
    if (sStats.bytes > sStats.peakBytes)
        sStats.peakBytes = sStats.bytes;
}

/*
 * A reserved payload's read over: `data` (transformed) published, or the
 * failure recorded, and whoever waits told. `owner` is the reservation it
 * was read under. A failure only marks a payload still reserved so: a
 * request that stopped waiting for this reader may be reading it as well
 * (WAIT_BOUND_MS). `last`: this failure gives the payload up. Returns the
 * payload as the table holds it - this read's, or one another reader
 * published first (then `data` is freed) - or NULL. A request's NULL counts
 * as an error, as it always has.
 */
static u8 *Finish(u32 idx, u8 owner, u8 *data, bool last)
{
    u8 *table;

    CtrLock_Lock(&sLock);
    if (data != NULL && sPayloads[idx].data == NULL)
        Publish(idx, data);
    else if (data == NULL)
    {
        ++sStats.failures;
        if (sState[idx] == owner)
            sState[idx] = last ? PAYLOAD_GIVEN_UP : PAYLOAD_FAILED;
    }
    table = sPayloads[idx].data;
    if (table != NULL)
        sPayloads[idx].stamp = sStamp;
    else if (owner == PAYLOAD_DEMAND)
        ++sStats.errors;
    CtrLock_Unlock(&sLock);
    CtrCond_Broadcast(&sPublished);
    if (data != NULL && data != table)
        free(data);
    return table;
}

/* A reservation given back unread (no memory to read it into, or a prefetch
 * that never ran): the payload is anyone's again. */
static void Release(u32 idx, u8 owner)
{
    bool released;

    CtrLock_Lock(&sLock);
    released = sState[idx] == owner;
    if (released)
        sState[idx] = PAYLOAD_EMPTY;
    if (owner == PAYLOAD_DEMAND)
        ++sStats.errors;
    CtrLock_Unlock(&sLock);
    if (released)
        CtrCond_Broadcast(&sPublished);
}

/* Failure lines in all, at most: a damaged pack must not flood the log. */
#define FAIL_LINES 64u
static u32 sFailLines;

/* One line for a reader that gave a payload up; outside sLock. */
static void LogFailure(u32 idx, CtrDataRead result, const char *reader, bool last)
{
    if (__atomic_add_fetch(&sFailLines, 1, __ATOMIC_RELAXED) > FAIL_LINES)
        return;
    CtrLog_Write(CTR_LOG_ERROR, "asset %s: %s (%s)%s", GetAssetPathByIndex(idx), CtrData_ReadName(result), reader,
                 last ? ", given up" : "; a request will try it once more");
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

static bool Reserved(u8 state)
{
    return state == PAYLOAD_DEMAND || state == PAYLOAD_WARM || state == PAYLOAD_PREFETCH;
}

static const char *ReaderName(u8 state)
{
    return state == PAYLOAD_WARM ? "warm-up" : state == PAYLOAD_PREFETCH ? "voxel prefetch" : "request";
}

/*
 * How long a request waits for another reader before it reads the payload
 * itself as well. Far beyond one read of any of them - a warm-up run
 * (WARM_RUN), a voxel page slice and then the payload, the log's drain on
 * the same card in the way (230 ms seen on an Old 3DS) - and short enough
 * that a reservation left behind by mistake costs one stall, not the game.
 */
#define WAIT_BOUND_MS 500u

static u8 *GetPayload(u32 idx)
{
    static u32 sDemandLogged, sStuckLogged;
    /* The wait for the lock and for a payload someone else is reading, apart
     * from the read itself (the frame profiler's iowait and io). */
    uint32_t waitStart = Port_ProfTick();
    uint64_t deadline = 0;
    u64 start;
    u8 *data, *table, stuck = PAYLOAD_EMPTY;
    bool waited = false, retry;
    CtrDataRead result;

    CtrLock_Lock(&sLock);
    while (Reserved(sState[idx]))
    {
        uint64_t now = CtrPlatform_Milliseconds();

        if (!waited)
            deadline = now + WAIT_BOUND_MS;
        waited = true;
        if (now >= deadline)
        {
            stuck = sState[idx];
            break;
        }
        CtrCond_WaitTimeout(&sPublished, &sLock, (unsigned)(deadline - now) * 1000u);
    }
    Port_ProfAdd(PORT_PROF_IO_WAIT, waitStart);
    data = sPayloads[idx].data;
    if (data != NULL)
    {
        ++sStats.hits;
        if (waited)
            ++sStats.waits;
        sPayloads[idx].stamp = sStamp;
        CtrLock_Unlock(&sLock);
        return data;
    }
    if (sState[idx] == PAYLOAD_GIVEN_UP)
    {
        ++sStats.errors;
        CtrLock_Unlock(&sLock);
        return NULL;
    }
    /* Not read yet, failed once, or held past the bound by another reader
     * (whose copy, if it comes, is then let go): read here. */
    retry = sState[idx] == PAYLOAD_FAILED;
    sState[idx] = PAYLOAD_DEMAND;
    ++sStats.misses;
    HintNeighbours(GetAssetPathByIndex(idx));
    CtrLock_Unlock(&sLock);
    if (stuck != PAYLOAD_EMPTY && __atomic_add_fetch(&sStuckLogged, 1, __ATOMIC_RELAXED) <= 8)
        CtrLog_Write(CTR_LOG_ERROR, "asset %s: its %s reader held it over %u ms; read here as well",
                     GetAssetPathByIndex(idx), ReaderName(stuck), WAIT_BOUND_MS);

    start = CtrPlatform_Ticks();
    data = ReadPayload(idx, &result);
    /* The one more try a payload gets, here at once if nobody failed it yet. */
    if (data == NULL && result != CTR_DATA_READ_NOMEM && !retry)
        data = ReadPayload(idx, &result);
    if (data != NULL)
        Transform(idx, data);
    if (result == CTR_DATA_READ_NOMEM)
    {
        Release(idx, PAYLOAD_DEMAND);
        table = NULL;
    }
    else
        table = Finish(idx, PAYLOAD_DEMAND, data, true);
    Port_ProfAdd(PORT_PROF_IO, (uint32_t)start);
    if (table == NULL)
        LogFailure(idx, result, "on demand", result != CTR_DATA_READ_NOMEM);
    /* The game waited for this one: worth knowing which, on hardware.
     * Capped, so a run of them cannot flood the log. */
    else if (__atomic_add_fetch(&sDemandLogged, 1, __ATOMIC_RELAXED) <= 96)
        CtrLog_Write(CTR_LOG_FS, "asset read on demand: %s (%lu B, %.2f ms)", GetAssetPathByIndex(idx),
                     (unsigned long)sEntries[idx].size, CtrPlatform_TickMs(CtrPlatform_Ticks() - start));
    return table;
}

/* What the worker may fill the cache up to: never enough to make the
 * collector evict what the game is using. */
#define WARM_BUDGET_SHARE 2u
/*
 * One pack read covers neighbours up to WARM_GAP apart, WARM_RUN long at
 * most. The pack is read positionally straight into the run buffer, so a
 * run costs one request of the FS service and its bytes. Every payload of a
 * run is reserved until the run is read, so a request for any of them waits
 * for the whole read, and the run's length is that wait's bound: at the few
 * MiB/s an SD card gives through the FS service (an estimate, not measured
 * on this port), 64 KiB is about a frame where 256 KiB was four. With the
 * pack's payloads in path order (pak.py) a run of 64 KiB still holds some
 * ninety of the average 700-byte ones. A gap of 4 KiB costs less to read
 * through than another request.
 */
#define WARM_GAP (4u * 1024u)
#define WARM_RUN (64u * 1024u)

typedef struct
{
    u64 offset;
    u32 size, idx, crc;
    u16 rank;
    u8 claimed;     /* reserved by this worker for the read under way */
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

/* Reserves the payloads of list[first, end) that nobody has, reads or failed
 * yet, and marks them `claimed`. */
static unsigned WarmClaim(WarmEntry *list, unsigned first, unsigned end)
{
    unsigned claimed = 0;

    CtrLock_Lock(&sLock);
    for (unsigned i = first; i < end; ++i)
    {
        list[i].claimed = sState[list[i].idx] == PAYLOAD_EMPTY;
        if (list[i].claimed)
        {
            sState[list[i].idx] = PAYLOAD_WARM;
            ++claimed;
        }
    }
    CtrLock_Unlock(&sLock);
    return claimed;
}

/* One payload into the cache on a read of its own, unless it is taken. */
static void WarmOne(WarmEntry *entry)
{
    CtrDataRead result;
    u8 *data;

    if (WarmClaim(entry, 0, 1) == 0)
        return;
    data = ReadPayload(entry->idx, &result);
    if (data != NULL)
        Transform(entry->idx, data);
    if (result == CTR_DATA_READ_NOMEM)
        Release(entry->idx, PAYLOAD_WARM);
    else
        Finish(entry->idx, PAYLOAD_WARM, data, false);
    if (data == NULL && result != CTR_DATA_READ_NOMEM)
        LogFailure(entry->idx, result, "warm-up", false);
}

/*
 * The pack: neighbours list[first, end) - at most WARM_RUN of them, or one
 * payload alone, longer - reserved and read in one go, each checked against
 * its CRC and published as soon as it is copied out (a request may be
 * waiting for any one of them).
 */
static void WarmRun(WarmEntry *list, unsigned first, unsigned end, u8 *run)
{
    u64 start = list[first].offset;
    u64 bytes = list[end - 1].offset + list[end - 1].size - start;
    unsigned lost = 0;
    bool ok;

    if (bytes > WARM_RUN)
    {
        WarmOne(&list[first]);
        return;
    }
    if (WarmClaim(list, first, end) == 0)
        return;
    ok = CtrData_ReadRange(start, run, (u32)bytes);
    for (unsigned i = first; i < end; ++i)
    {
        WarmEntry *entry = &list[i];
        const u8 *bytesAt = run + (entry->offset - start);
        CtrDataRead result = CTR_DATA_READ_IO;
        u8 *copy = NULL;

        if (!entry->claimed)
            continue;
        if (ok && CtrPak_Crc32(0, bytesAt, entry->size) != entry->crc)
            result = CTR_DATA_READ_CRC;
        else if (ok && (copy = malloc(entry->size != 0 ? entry->size : 1)) == NULL)
            result = CTR_DATA_READ_NOMEM;
        else if (ok)
        {
            memcpy(copy, bytesAt, entry->size);
            Transform(entry->idx, copy);
            result = CTR_DATA_READ_OK;
        }
        if (result == CTR_DATA_READ_NOMEM)
            Release(entry->idx, PAYLOAD_WARM);
        else
            Finish(entry->idx, PAYLOAD_WARM, copy, false);
        if (result == CTR_DATA_READ_CRC)
            LogFailure(entry->idx, result, "warm-up", false);
        else if (result == CTR_DATA_READ_IO)
            ++lost;
    }
    /* One line for the run, not one for each of its payloads. */
    if (lost != 0 && __atomic_add_fetch(&sFailLines, 1, __ATOMIC_RELAXED) <= FAIL_LINES)
        CtrLog_Write(CTR_LOG_ERROR, "assets: a pack run of %u payloads (%lu B at %llu) failed to read; "
                     "a request will try each once more", lost, (unsigned long)bytes, (unsigned long long)start);
}

/* Where the run that starts at list[first] ends, list[first, end) being in
 * pack order: neighbours of one rank up to WARM_GAP apart, WARM_RUN long. */
static unsigned RunEnd(const WarmEntry *list, unsigned first, unsigned end)
{
    u64 start = list[first].offset, stop = start + list[first].size;
    unsigned next = first + 1;

    while (next < end && list[next].rank == list[first].rank
           && list[next].offset >= stop && list[next].offset - stop <= WARM_GAP
           && list[next].offset + list[next].size - start <= WARM_RUN)
    {
        stop = list[next].offset + list[next].size;
        ++next;
    }
    return next;
}

/* list[first, end) in runs (`run`), or a file at a time without one. */
static void WarmSpan(WarmEntry *list, unsigned first, unsigned end, u8 *run)
{
    while (first < end && WarmRoom())
    {
        unsigned stop = run != NULL ? RunEnd(list, first, end) : first + 1;

        if (run != NULL)
            WarmRun(list, first, stop, run);
        else
            WarmOne(&list[first]);
        first = stop;
    }
}

/*
 * The neighbours of what a request last had to read (HintNeighbours), before
 * the rest: one prefix of paths, so in the pack (written in path order) one
 * stretch, read in runs; a file at a time without `run`.
 */
static void WarmHinted(WarmEntry *list, unsigned count, u8 *run)
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
    for (unsigned i = 0; i < count && WarmRoom();)
    {
        unsigned end = i;

        while (end < count && strncmp(GetAssetPathByIndex(list[end].idx), hint, length) == 0)
            ++end;
        if (end == i)
            ++i;
        else
        {
            WarmSpan(list, i, end, run);
            i = end;
        }
    }
}

/* Everything, the first ranks first: in runs from the pack, else a file at
 * a time; the hinted neighbours between two reads. */
static void WarmAll(WarmEntry *list, unsigned count, u8 *run)
{
    unsigned first = 0;

    qsort(list, count, sizeof(*list), CompareWarmEntries);
    while (first < count && WarmRoom())
    {
        unsigned end = run != NULL ? RunEnd(list, first, count) : first + 1;

        WarmHinted(list, count, run);
        WarmSpan(list, first, end, run);
        first = end;
    }
}

static void WarmWorker(void *arg)
{
    WarmEntry *list = arg;
    unsigned count = 0;
    u8 *run = NULL;
    u64 begin = CtrPlatform_Ticks();

    /* Where the pack holds each payload. One it lacks, or holds with another
     * size, is left to the requests, which say why it does not read. */
    for (u32 idx = 0; idx < sEntryCount; ++idx)
    {
        WarmEntry *entry = &list[count];
        u32 size = 0;

        if (CtrData_Locate(GetAssetPathByIndex(idx), &entry->offset, &size, &entry->crc)
            && size == sEntries[idx].size)
        {
            entry->size = size;
            entry->idx = idx;
            entry->rank = (u16)WarmRank(GetAssetPathByIndex(idx));
            entry->claimed = 0;
            ++count;
        }
    }
    if (count != 0 && (run = malloc(WARM_RUN)) != NULL)
        WarmAll(list, count, run);
    else
    {
        /* RomFS or loose files (or no room for a run): a file at a time. */
        for (u32 idx = 0; idx < sEntryCount; ++idx)
            list[idx] = (WarmEntry){0, sEntries[idx].size, idx, 0, (u16)WarmRank(GetAssetPathByIndex(idx)), 0};
        count = 0;
        WarmAll(list, sEntryCount, NULL);
    }
    free(run);
    free(list);
    CtrLog_Write(CTR_LOG_FS, "assets warmed%s: %lu KiB of %lu payloads in %.0f ms (%s), "
                 "%lu requests waited for a reader, %lu reads failed",
                 sWarmStop ? " until exit" : "", (unsigned long)(sStats.bytes >> 10),
                 (unsigned long)sStats.loads, CtrPlatform_TickMs(CtrPlatform_Ticks() - begin),
                 count ? "pack runs" : "file by file", (unsigned long)sStats.waits,
                 (unsigned long)sStats.failures);
    __atomic_store_n(&sWarmRunning, false, __ATOMIC_RELEASE);
}

/* At most this long for the worker to finish the read it is in at exit:
 * a run or a file, some tens of milliseconds. */
#define WARM_JOIN_MS 1000u
static CtrThread sWarmThread;
static bool sWarmStarted;

/* Exiting with the warm-up under way, its reads failed in the log as the
 * RomFS went away under them: it stops first, a read at most later, and is
 * joined before the file systems close. */
void CtrAssets_StopWarmup(void)
{
    sWarmStop = true;
    if (sWarmThread == NULL)
        return;
    if (CtrPlatform_JoinThread(sWarmThread, WARM_JOIN_MS))
        sWarmThread = NULL;
    else
        CtrLog_Write(CTR_LOG_ERROR, "assets: the warm-up worker was still reading after %u ms; "
                     "exiting under it", WARM_JOIN_MS);
}

bool CtrAssets_WarmingUp(void)
{
    return __atomic_load_n(&sWarmRunning, __ATOMIC_ACQUIRE);
}

void CtrAssets_StartWarmup(void)
{
    WarmEntry *list;

    if (!LoadAssetMap() || sWarmStarted)
        return;
    sWarmStarted = true;
    list = malloc(sEntryCount * sizeof(*list));
    sWarmRunning = true;
    /*
     * On the system core, beside the game rather than in the gaps it leaves:
     * the worker waits on the FS service far more than it computes. From
     * the RomFS that wait is the service's per file - some 9.6 ms each on an
     * Old 3DS, a minute for all of them, wherever the worker runs - and the
     * field asks for its tiles in the frame meanwhile; from the pack it
     * reads runs (WarmRun).
     */
    if (list == NULL || (sWarmThread = CtrPlatform_StartJoinable(WarmWorker, list, 16 * 1024, 1)) == NULL)
    {
        free(list);
        sWarmRunning = false;
        CtrLog_Write(CTR_LOG_ERROR, "assets: no warm-up worker; payloads load on demand");
    }
}

const struct CtrAssetStats *CtrAssets_GetStats(void)
{
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
        sState[i] = PAYLOAD_EMPTY;
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
    CtrAssets_TransformAsset(plain, data, size);
}

void CtrAssets_TransformAsset(const char *path, void *data, unsigned size)
{
    CtrLock_Lock(&sTransformLock);
    Port_TransformAsset(path, data, size);
    CtrLock_Unlock(&sTransformLock);
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
    /* Only one nobody has, reads or failed: reserved for the stream's
     * worker from here, so nobody else reads it meanwhile. */
    CtrLock_Lock(&sLock);
    if (sState[idx] != PAYLOAD_EMPTY)
    {
        CtrLock_Unlock(&sLock);
        return false;
    }
    sState[idx] = PAYLOAD_PREFETCH;
    CtrLock_Unlock(&sLock);
    out->index = idx;
    out->size = sEntries[idx].size;
    out->path = GetAssetPathByIndex((u32)idx);
    return true;
}

void *CtrAssets_PrefetchRead(const CtrAssetPrefetch *request)
{
    s32 idx = request->index;
    CtrDataRead result;
    u8 *data, *table;

    if (!sLoaded || idx < 0 || (u32)idx >= sEntryCount)
        return NULL;
    data = ReadPayload((u32)idx, &result);
    if (data != NULL)
        Transform((u32)idx, data);
    if (result == CTR_DATA_READ_NOMEM)
    {
        Release((u32)idx, PAYLOAD_PREFETCH);
        return NULL;
    }
    /* Published here, on the stream's worker: a request on the game's
     * thread may be waiting for it already. */
    table = Finish((u32)idx, PAYLOAD_PREFETCH, data, false);
    if (data == NULL)
        LogFailure((u32)idx, result, "voxel prefetch", false);
    return table;
}

void CtrAssets_PrefetchAdopt(const CtrAssetPrefetch *request, void *payload)
{
    s32 idx = request->index;

    /* The cache's own, published by PrefetchRead (or NULL): never the
     * caller's to free. */
    (void)payload;
    if (!sLoaded || idx < 0 || (u32)idx >= sEntryCount)
        return;
    /* A reservation whose read never ran (the stream stopped first). */
    Release((u32)idx, PAYLOAD_PREFETCH);
}

void Port_AssetPreload(void)
{
    if (!LoadAssetMap())
        CtrAssets_Fatal("asset index could not be loaded from RomFS");
}
