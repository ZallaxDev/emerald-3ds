#include "3ds_follower_trail.h"

void CtrFollowerTrail_Reset(CtrFollowerTrail *trail, CtrFollowerPoint player)
{
    trail->points[0] = trail->last = player;
    trail->next = 1;
    trail->count = 1;
}

void CtrFollowerTrail_Seed(CtrFollowerTrail *trail, CtrFollowerPoint player,
                           int tailX, int tailY)
{
    if (!((tailX == 0 && (tailY == -1 || tailY == 1))
       || (tailY == 0 && (tailX == -1 || tailX == 1))))
    {
        CtrFollowerTrail_Reset(trail, player);
        return;
    }
    for (unsigned i = 0; i < CTR_FOLLOWER_POINTS; ++i)
    {
        int distance = CTR_FOLLOWER_DISTANCE - (int)i;
        trail->points[i] = (CtrFollowerPoint){player.x + tailX * distance,
                                             player.y + tailY * distance};
    }
    trail->last = player;
    trail->next = 0;
    trail->count = CTR_FOLLOWER_POINTS;
}

bool CtrFollowerTrail_Update(CtrFollowerTrail *trail, CtrFollowerPoint player,
                             CtrFollowerPoint *follower)
{
    int32_t dx = player.x - trail->last.x, dy = player.y - trail->last.y;
    if (!trail->count || (dx && dy) || dx > 16 || dx < -16 || dy > 16 || dy < -16)
    {
        CtrFollowerTrail_Reset(trail, player);
        return false;
    }
    int32_t distance = dx ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
    for (int32_t i = 0; i < distance; ++i)
    {
        trail->last.x += dx > 0 ? 1 : dx < 0 ? -1 : 0;
        trail->last.y += dy > 0 ? 1 : dy < 0 ? -1 : 0;
        trail->points[trail->next] = trail->last;
        trail->next = (trail->next + 1) % CTR_FOLLOWER_POINTS;
        if (trail->count < CTR_FOLLOWER_POINTS)
            ++trail->count;
    }
    if (trail->count < CTR_FOLLOWER_POINTS)
        return false;
    *follower = trail->points[trail->next];
    return true;
}
