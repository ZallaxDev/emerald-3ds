#ifndef CTR_SPRITE_IDS_H
#define CTR_SPRITE_IDS_H
#include <stdint.h>

/*
 * Sprites' identities (phase F1 of the engine plan, point 3): a number that a
 * sprite keeps for as long as it lives and that no other sprite ever has, so
 * the renderer can follow an actor from frame to frame instead of guessing it
 * from OAM. Numbered by sprite.c when a sprite is made; CtrSprites_Track gives
 * one to a sprite that appeared by copying another. SDK-free.
 */
void CtrSprites_Track(void);
/* For the log, at exit. */
void CtrSprites_Report(void);

#endif
