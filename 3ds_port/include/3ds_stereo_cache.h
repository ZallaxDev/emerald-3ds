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

#endif
