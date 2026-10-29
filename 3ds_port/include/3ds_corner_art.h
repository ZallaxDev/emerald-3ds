#ifndef CTR_CORNER_ART_H
#define CTR_CORNER_ART_H

/*
 * The Game Corner's own backdrops, drawn by 3ds_video.c behind its two
 * centred screens instead of a flat colour or black margins: the roulette's
 * felt, the Game Corner tables' green, with Poké Balls woven in it and a gold cord
 * round the screen, and the slot machine set in the casino's wall, coins on
 * green diamonds, inside a gold frame of bulbs.
 *
 * Each is two small textures the GPU repeats (a 64x64 pattern, a 32x32 one
 * for the trimmings and the plain colours), filled once when the screen
 * opens: drawing them is a handful of quads a frame and nothing is redrawn.
 * Both are RGB565, texels in the PICA's tiled order (CtrVideo_Texel).
 */
#include <stdint.h>

#define CTR_CORNER_PATTERN_DIM 64
#define CTR_CORNER_TRIM_DIM 32

/*
 * The trim texture of the roulette: row 0 and column 0 hold the cord (gold
 * and dark gold dashes, (x + y) % 4), repeated along a line; the plain
 * colours are single texels.
 */
#define CTR_CORNER_FELT_BAND 3   /* the dark band round the screen: (3, 3) */
#define CTR_CORNER_FELT_LINE 5   /* the shade inside the cord: (5, 5) */

/*
 * The trim texture of the slot machine's frame: rows 0-5 its red band with
 * the bulbs along it (lit at columns 1-4, dark at 17-20), columns 0-5 the
 * same down it; the frame's colours are single texels.
 */
#define CTR_CORNER_BULB_PERIOD 32
#define CTR_CORNER_FRAME_DARK 10 /* (10, 10) */
#define CTR_CORNER_FRAME_GOLD 12 /* (12, 12) */
#define CTR_CORNER_FRAME_SHADOW 14 /* (14, 14): the recess round the machine */

/* The roulette's felt and cord in the colour table (GBA BGR555). */
void CtrCornerArt_Felt(uint16_t *pattern, uint16_t *trim, uint16_t table);
/* The casino wall and the slot machine's frame. */
void CtrCornerArt_Wall(uint16_t *pattern, uint16_t *trim);

#endif
