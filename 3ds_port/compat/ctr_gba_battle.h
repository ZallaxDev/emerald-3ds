/*
 * Force-included into the translation units of the battle scene (full.mk,
 * CTR_GBA_BATTLE_SRCS): the battle engine, its controllers, the healthboxes,
 * the move animations and the intro.
 *
 * Everything the battle draws is placed for a 240x160 screen: the BG0 pages of
 * the text box are 160 lines apart, the intro's scanline table is split at line
 * 80, sprites enter from x=-240 and x=+240. So these units keep the GBA
 * geometry (CTR_GBA_STAGE), and the compositor shows the picture 1:1 with the
 * text box on the bottom edge of the top screen and the scene carried out to
 * the rest of it (CtrVideo_SetBattle, docs/ARCHITECTURE.md). Their VBlank callbacks go
 * through the bridge so it knows when the battle is on screen; main.h declares
 * the renamed function, so the prototype still matches.
 */
#ifndef CTR_GBA_BATTLE_H
#define CTR_GBA_BATTLE_H

#define SetVBlankCallback CtrBattle_SetVBlankCallback

/* How the battle's sandstorm picture is shown (3ds_video.c). */
#define CTR_SANDSTORM_NONE 0
#define CTR_SANDSTORM_STILL 1
#define CTR_SANDSTORM_BLOWN 2
unsigned char CtrBattle_Sandstorm(void);

#endif
