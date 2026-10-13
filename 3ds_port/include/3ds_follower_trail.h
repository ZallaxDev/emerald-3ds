#ifndef CTR_FOLLOWER_TRAIL_H
#define CTR_FOLLOWER_TRAIL_H

#include <stdbool.h>
#include <stdint.h>

/* One tile behind, measured along the walked path rather than in frames. */
#define CTR_FOLLOWER_DISTANCE 16
#define CTR_FOLLOWER_POINTS (CTR_FOLLOWER_DISTANCE + 1)

typedef struct { int32_t x, y; } CtrFollowerPoint;
typedef struct
{
    CtrFollowerPoint points[CTR_FOLLOWER_POINTS];
    CtrFollowerPoint last;
    unsigned next, count;
} CtrFollowerTrail;

void CtrFollowerTrail_Reset(CtrFollowerTrail *trail, CtrFollowerPoint player);
/* Seed a straight one-tile trail from a validated adjacent tile to the player. */
void CtrFollowerTrail_Seed(CtrFollowerTrail *trail, CtrFollowerPoint player,
                           int tailX, int tailY);
/* Discontinuities reset the trail; a fresh trail waits for one walked tile. */
bool CtrFollowerTrail_Update(CtrFollowerTrail *trail, CtrFollowerPoint player,
                             CtrFollowerPoint *follower);

#endif
