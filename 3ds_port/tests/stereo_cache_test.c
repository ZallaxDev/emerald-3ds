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
    printf(failures ? "stereo_cache_test: %u FAILED\n" : "stereo_cache_test: PASS\n", failures);
    return failures != 0;
}
