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

/* The same OAM entry must never represent two different battler slots.
 * Ambiguous matches are not depth-rendered; both use the original flat OAM
 * path instead. Only values 0..127 are candidate visible OAM entries. */
static inline unsigned CtrStereoMon_DropAmbiguousOam(int matched[4], unsigned count)
{
    unsigned ambiguous = 0;

    if (matched == NULL || count > 4) return 0;
    for (unsigned i = 0; i < count; ++i)
        if (matched[i] >= 0 && matched[i] < 128)
            for (unsigned j = i + 1; j < count; ++j)
                if (matched[i] == matched[j])
                    ambiguous |= (1u << i) | (1u << j);
    for (unsigned i = 0; i < count; ++i)
        if (ambiguous & (1u << i)) matched[i] = -1;
    return ambiguous;
}

/* The GBA's OBJ VRAM is 32 KiB (1024 4bpp tiles). A 64x64 4bpp
 * battler occupies 64 consecutive tiles. The depth matcher compares its
 * whole 2048-byte frame, so a base above 960 could read past OBJ VRAM.
 * The original hardware sprite renderer retains ownership instead. */
static inline bool CtrStereoMon_TileBaseValid(unsigned tileNum)
{
    return tileNum <= (32768u - 2048u) / 32u;
}

#endif
