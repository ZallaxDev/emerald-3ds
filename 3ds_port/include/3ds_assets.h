#ifndef CTR_ASSETS_H
#define CTR_ASSETS_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Resident index, reclaimable payloads. 24 MiB is a deliberate ARM11 figure:
 * the whole referenced graphics set is smaller than the linear+application heap
 * available here, so the cache exists to bound growth, not to survive on 4 MiB.
 */
#define CTR_ASSET_CACHE_BUDGET (24u * 1024u * 1024u)
/* Never reclaim something the game may still be holding from a recent frame. */
#define CTR_ASSET_CACHE_GRACE_FRAMES 8u

/*
 * hits: requests served from memory, `waits` of them after waiting for
 * another reader (the warm-up, the voxel prefetch, a request on another
 * thread) instead of reading the payload again. misses: requests that read
 * it themselves. errors: requests that got nothing. failures: reads that
 * failed, by anyone (a request makes up to two).
 */
struct CtrAssetStats
{
    uint32_t entries, indexBytes;
    uint32_t bytes, peakBytes;
    uint32_t loads, hits, misses, evictions, errors;
    uint32_t waits, failures;
};

void CtrAssets_Collect(void);
/* Reads every payload into the cache from a background worker (see 3ds_assets.c). */
void CtrAssets_StartWarmup(void);
/* Stops the warm-up and joins its worker before the file systems close (at
 * exit; one log line if it is still reading after a second). */
void CtrAssets_StopWarmup(void);
/* Whether the warm-up worker is still reading (the frame profiler's flag). */
bool CtrAssets_WarmingUp(void);
const struct CtrAssetStats *CtrAssets_GetStats(void);
void CtrAssets_Fatal(const char *detail) __attribute__((noreturn));
/* Port_TransformAsset (3ds_icons_ptbr.c) for a graphic read outside the
 * cache: one call at a time with those of the cache's readers. */
void CtrAssets_TransformAsset(const char *path, void *data, unsigned size);

/*
 * Reading a payload ahead of need, off the render thread (the voxel page
 * stream's worker). Find, on the game's thread: false if `ptr` is no asset,
 * or is in memory, being read or failed already; true reserves it, so that
 * nobody else reads it - a request for it waits for this read. Read, on the
 * stream's worker: reads and publishes it itself (the game's thread may be
 * the one waiting), and returns the cache's copy - never the caller's to
 * free or keep - or NULL if it failed (then neither the warm-up nor a later
 * Find tries it again). Adopt, on the game's thread, for every request Find
 * gave, read or not: gives back a reservation whose read never ran.
 */
typedef struct
{
    int32_t index;
    uint32_t size;
    const char *path;
} CtrAssetPrefetch;

bool CtrAssets_PrefetchFind(const void *ptr, CtrAssetPrefetch *out);
void *CtrAssets_PrefetchRead(const CtrAssetPrefetch *request);
void CtrAssets_PrefetchAdopt(const CtrAssetPrefetch *request, void *payload);

/* Reserved regions filled from the game data, see 3ds_script_loader.c. */
bool CtrGameData_Init(void);
uint32_t CtrGameData_Bytes(void);
bool CtrScripts_Init(void);
uint32_t CtrScripts_Bytes(void);
bool CtrSongs_Init(void);
uint32_t CtrSongs_Bytes(void);

/* Map payloads: whole-file resident, see 3ds_map_loader.c. */
bool CtrMaps_Init(void);
uint32_t CtrMaps_Bytes(void);

#endif
