#include <assert.h>
#include <stdio.h>
#include "3ds_follower_trail.h"

int main(void)
{
    CtrFollowerTrail trail = {0};
    CtrFollowerPoint player = {0, 0}, follower;
    assert(!CtrFollowerTrail_Update(&trail, player, &follower));
    for (int x = 1; x < 16; ++x)
        assert(!CtrFollowerTrail_Update(&trail, (CtrFollowerPoint){x, 0}, &follower));
    assert(CtrFollowerTrail_Update(&trail, (CtrFollowerPoint){16, 0}, &follower));
    assert(follower.x == 0 && follower.y == 0);
    /* A right-angle turn follows the walked corner, never cuts across the wall. */
    for (int y = 1; y <= 16; ++y)
    {
        assert(CtrFollowerTrail_Update(&trail, (CtrFollowerPoint){16, y}, &follower));
        assert(follower.x == y && follower.y == 0);
    }
    assert(CtrFollowerTrail_Update(&trail, (CtrFollowerPoint){16, 20}, &follower));
    assert(follower.x == 16 && follower.y == 4);
    /* Stopping does not drain the gap; variable speed keeps the same distance. */
    for (unsigned i = 0; i < 80; ++i)
    {
        assert(CtrFollowerTrail_Update(&trail, (CtrFollowerPoint){16, 20}, &follower));
        assert(follower.x == 16 && follower.y == 4);
    }
    assert(CtrFollowerTrail_Update(&trail, (CtrFollowerPoint){16, 28}, &follower));
    assert(follower.x == 16 && follower.y == 12);
    assert(CtrFollowerTrail_Update(&trail, (CtrFollowerPoint){16, 24}, &follower));
    assert(follower.x == 16 && follower.y == 16);
    /* Warps and map-origin shifts discard stale coordinates. */
    assert(!CtrFollowerTrail_Update(&trail, (CtrFollowerPoint){400, 400}, &follower));
    assert(!CtrFollowerTrail_Update(&trail, (CtrFollowerPoint){401, 401}, &follower));
    CtrFollowerTrail_Reset(&trail, (CtrFollowerPoint){0, 0});
    assert(CtrFollowerTrail_Update(&trail, (CtrFollowerPoint){-16, 0}, &follower));
    assert(follower.x == 0 && follower.y == 0);
    assert(CtrFollowerTrail_Update(&trail, (CtrFollowerPoint){-32, 0}, &follower));
    assert(follower.x == -16 && follower.y == 0);
    for (int direction = -1; direction <= 1; direction += 2)
    {
        CtrFollowerTrail_Seed(&trail, (CtrFollowerPoint){32, 48}, direction, 0);
        assert(CtrFollowerTrail_Update(&trail, (CtrFollowerPoint){32, 48}, &follower));
        assert(follower.x == 32 + direction * 16 && follower.y == 48);
        CtrFollowerTrail_Seed(&trail, (CtrFollowerPoint){32, 48}, 0, direction);
        assert(CtrFollowerTrail_Update(&trail, (CtrFollowerPoint){32, 48}, &follower));
        assert(follower.x == 32 && follower.y == 48 + direction * 16);
    }
    CtrFollowerTrail_Seed(&trail, (CtrFollowerPoint){32, 48}, 1, 1);
    assert(!CtrFollowerTrail_Update(&trail, (CtrFollowerPoint){32, 48}, &follower));
    puts("PASS follower trail: distance, corners, stop, run, reversal, warp, negative coordinates");
    return 0;
}
