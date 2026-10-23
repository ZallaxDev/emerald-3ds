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

struct CtrAssetStats
{
    uint32_t entries, pointerEntries, indexBytes;
    uint32_t bytes, peakBytes, budget;
    uint32_t loads, hits, misses, evictions, errors;
};

void CtrAssets_Collect(void);
/* Reads every payload into the cache from a background worker (see 3ds_assets.c). */
void CtrAssets_StartWarmup(void);
/* Stops the warm-up before the file system closes (at exit). */
void CtrAssets_StopWarmup(void);
const struct CtrAssetStats *CtrAssets_GetStats(void);
void CtrAssets_Fatal(const char *detail) __attribute__((noreturn));

/*
 * Reading a payload ahead of need, off the render thread (the voxel page
 * stream's worker). Find, on the game's thread: false if `ptr` is no asset or
 * is in memory already. Read, on any thread: touches nothing shared, and
 * returns the payload or NULL. Adopt, on the game's thread again: installs it
 * unless the game loaded it meanwhile (then it is freed).
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
