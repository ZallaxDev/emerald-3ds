#ifndef CTR_STEREO_GATE_H
#define CTR_STEREO_GATE_H

/* SDK-free guard for swapping a *validated* OAM battler with depth planes.
 * Deliberately does not inspect sprite attributes: the existing renderer
 * performs priority, OBJ window, animation, mask and visibility checks first.
 * All weather and non-battler sprites continue down its original path. */
#include <stdbool.h>
#include <stdint.h>

static inline bool CtrStereoMon_DrawEligible(unsigned oam, unsigned matched,
                                            float eyeParallax, const uint32_t skip[4],
                                            bool fog, bool lattice, bool particle)
{
    return matched != 0 && eyeParallax != 0.0f && skip != NULL && oam < 128
        && !fog && !lattice && !particle
        && (skip[oam >> 5] & (1u << (oam & 31))) != 0;
}

#endif
