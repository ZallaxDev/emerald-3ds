#ifndef CTR_FOLLOWER_H
#define CTR_FOLLOWER_H

#include <stdbool.h>
struct Sprite;

void CtrFollower_Update(void);
/* The voxel renderer must place this at its own feet, never attach it to a nearby NPC. */
bool CtrFollower_IsSprite(const struct Sprite *sprite);

#endif
