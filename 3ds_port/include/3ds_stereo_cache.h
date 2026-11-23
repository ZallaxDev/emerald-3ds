#ifndef CTR_STEREO_CACHE_H
#define CTR_STEREO_CACHE_H

/* SDK-free guard for a texture cached in a mutable battle slot.
 * The same Planes allocation is reused when the Pokémon/trainer ID changes,
 * so pointer identity alone is insufficient to reuse its old composed art. */
#include <stdbool.h>
#include <stdint.h>

static inline bool CtrStereoBattle_SameSource(const void *cachedPlanes, uint16_t cachedId,
                                              const void *shownPlanes, uint16_t shownId)
{
    return cachedPlanes == shownPlanes && cachedId == shownId;
}

/* IDs and readiness flags must not occupy the same bits. In particular,
 * trainer IDs have the 0x4000 kind bit set: that is not "trainer ready".
 * Two readiness bits below the IDs make an injective cache key. */
static inline uint64_t CtrStereoBattle_SilhouetteKey(uint16_t monId, uint16_t trainerId,
                                                      bool monReady, bool trainerReady)
{
    return ((uint64_t)monId << 18) | ((uint64_t)trainerId << 2)
        | (monReady ? 2u : 0u) | (trainerReady ? 1u : 0u);
}

/* The async reader writes frame bytes and count before publishing "ready";
 * consumers acquire the flag before reading those non-atomic payloads.
 * The flag itself must never be read or written outside these helpers. */
static inline bool CtrStereoBattle_FlagAcquire(const bool *flag)
{
    return __atomic_load_n(flag, __ATOMIC_ACQUIRE);
}

static inline void CtrStereoBattle_FlagRelease(bool *flag, bool value)
{
    __atomic_store_n(flag, value, __ATOMIC_RELEASE);
}

#endif
