/* Host-only regression tests: the stereo adapter must never intercept
 * weather, masked-out sprites or the flat (slider-down) compositor.
 * All fixtures are synthetic; no ROM or artwork is required. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "3ds_stereo_gate.h"

static unsigned failures;
#define CHECK(c) do { if (!(c)) { ++failures; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static bool Eligible(unsigned oam, unsigned count, float eye, const uint32_t mask[4],
                     bool fog, bool lattice, bool particle)
{
    return CtrStereoMon_DrawEligible(oam, count, eye, mask, fog, lattice, particle);
}

int main(void)
{
    uint32_t mask[4] = {0, 0, 0, 0};
    CHECK(!Eligible(0, 1, 1.0f, NULL, false, false, false));
    CHECK(!Eligible(128, 1, 1.0f, mask, false, false, false));
    CHECK(!Eligible(65535, 1, 1.0f, mask, false, false, false));
    for (unsigned oam = 0; oam < 128; ++oam)
    {
        mask[oam >> 5] |= 1u << (oam & 31);
        CHECK(Eligible(oam, 1, 1.0f, mask, false, false, false));
        CHECK(Eligible(oam, 4, -1.0f, mask, false, false, false));
        CHECK(!Eligible(oam, 0, 1.0f, mask, false, false, false));
        CHECK(!Eligible(oam, 1, 0.0f, mask, false, false, false));
        CHECK(!Eligible(oam, 1, 1.0f, mask, true, false, false));
        CHECK(!Eligible(oam, 1, 1.0f, mask, false, true, false));
        CHECK(!Eligible(oam, 1, 1.0f, mask, false, false, true));
        CHECK(!Eligible(oam, 1, -1.0f, mask, true, true, true));
        for (unsigned other = 0; other < 128; ++other)
            if (other != oam)
                CHECK(!Eligible(other, 1, 1.0f, mask, false, false, false));
        mask[oam >> 5] &= ~(1u << (oam & 31));
        CHECK(!Eligible(oam, 1, 1.0f, mask, false, false, false));
    }
    printf("stereo_draw_gate_test: %s (%u failures)\n", failures ? "FAILED" : "PASS", failures);
    return failures ? 1 : 0;
}
