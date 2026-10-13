#ifndef CTR_FOLLOWER_H
#define CTR_FOLLOWER_H

#include <stdbool.h>
#include <stdint.h>
struct Sprite;

void CtrFollower_Update(void);
/* The voxel renderer must place this at its own feet, never attach it to a nearby NPC. */
bool CtrFollower_IsSprite(const struct Sprite *sprite);
/* Interpolated map pixels (including MAP_OFFSET), independent of the sprite's transparent border. */
bool CtrFollower_GetPosition(const struct Sprite *sprite, int32_t *x, int32_t *y, int *footPad);

#endif
