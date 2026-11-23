/* SDK-free regression: depth-pack ID changes must invalidate cached art. */
#include <stdio.h>
#include <stdint.h>
#include "3ds_stereo_cache.h"

static unsigned failures;
#define CHECK(c) do { if (!(c)) { ++failures; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void)
{
    int mon = 1, trainer = 2;
    CHECK(CtrStereoBattle_SameSource(&mon, 25, &mon, 25));
    CHECK(!CtrStereoBattle_SameSource(&mon, 25, &mon, 150));
    CHECK(!CtrStereoBattle_SameSource(&mon, 25, &trainer, 25));
    CHECK(!CtrStereoBattle_SameSource(&mon, 25, &trainer, 150));
    CHECK(CtrStereoBattle_SameSource(&mon, 0xc001, &mon, 0xc001));
    CHECK(!CtrStereoBattle_SameSource(&mon, 0xc001, &mon, 0xc002));
    CHECK(CtrStereoBattle_SameSource(NULL, 0, NULL, 0));

    /* Regression: the 0x4000 trainer-kind bit used to collide with the
     * "trainer planes loaded" bit, leaving a ready sprite flat forever. */
    uint64_t pending = CtrStereoBattle_SilhouetteKey(25, 0x4007, false, false);
    CHECK(pending != CtrStereoBattle_SilhouetteKey(25, 0x4007, false, true));
    CHECK(pending != CtrStereoBattle_SilhouetteKey(25, 0x4007, true, false));
    CHECK(CtrStereoBattle_SilhouetteKey(25, 0x4007, false, true)
          != CtrStereoBattle_SilhouetteKey(25, 0x4007, true, false));
    CHECK(CtrStereoBattle_SilhouetteKey(25, 0x4007, true, true)
          != CtrStereoBattle_SilhouetteKey(25, 0x4008, true, true));
    CHECK(CtrStereoBattle_SilhouetteKey(0x8000, 0x4000, false, false)
          != CtrStereoBattle_SilhouetteKey(0x8001, 0x4000, false, false));
    /* The ready bit is an atomic release/acquire handoff. */
    bool ready = false;
    CHECK(!CtrStereoBattle_FlagAcquire(&ready));
    CtrStereoBattle_FlagRelease(&ready, true);
    CHECK(CtrStereoBattle_FlagAcquire(&ready));
    CtrStereoBattle_FlagRelease(&ready, false);
    CHECK(!CtrStereoBattle_FlagAcquire(&ready));
    printf(failures ? "stereo_cache_test: %u FAILED\n" : "stereo_cache_test: PASS\n", failures);
    return failures != 0;
}
