#ifndef CTR_BOTTOM_H
#define CTR_BOTTOM_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The bottom screen: a touch companion to the game on the top screen (party,
 * region map, bag and trainer card in the field; action and move buttons in
 * battle). See docs/ARCHITECTURE.md.
 *
 * It is drawn by the CPU into a 320x240 canvas laid out exactly like the
 * bottom framebuffer (RGB565, column-major, each column bottom-to-top), and
 * only when what it shows changes. There is no GPU pass, no texture and no
 * VRAM: on an Old 3DS the compositor and the voxel overworld need all of it.
 */
#define CTR_BOTTOM_WIDTH 320
#define CTR_BOTTOM_HEIGHT 240

/* Native side (libctru): copy columns [x0, x1) of the canvas to the screen. */
void CtrBottom_Blit(const uint16_t *canvas, int x0, int x1);
/* Same, restricted to rows [y0, y1) of those columns. */
void CtrBottom_BlitRect(const uint16_t *canvas, int x0, int y0, int x1, int y1);
/* A band of rows [y0, y1) across the whole screen, laid out as the screen's
 * columns of (y1 - y0) pixels each, bottom to top (the 2x battle's text box). */
void CtrBottom_BlitBand(const uint16_t *band, int y0, int y1);
/* Game side: whether the screen shows a battle menu of its own (actions,
 * moves, a target), which stands in for the GBA's text box. */
bool CtrBottom_BattleMenuShown(void);
/* What the last Blit or BlitRect spent copying to the screen and flushing the
 * data cache, in system ticks (0 if nothing was sent): the slow-redraw log. */
void CtrBottom_LastBlitTicks(uint64_t *copy, uint64_t *flush);
/* The whole canvas at level/4 of its brightness (0..4), for fades. */
void CtrBottom_BlitDim(const uint16_t *canvas, int level);

/* Game side. Init before AgbMain; Frame once per frame, after input is
 * scanned and before the game reads its keys. */
void CtrBottom_Init(void);
void CtrBottom_Frame(void);
/* The game's soft reset: forget the game it was in (back to the title's
 * screen, no menu session, plan or focus) and let the top screen go. */
void CtrBottom_SoftReset(void);
/* Keys the bottom screen presses on the player's behalf this frame. */
uint16_t CtrBottom_InjectedKeys(void);
/* The player's keys as the game should see them: none while the button
 * column has the X focus, or until the keys that left it are let go. */
uint16_t CtrBottom_FilterKeys(uint16_t held);

/*
 * The light green the bottom screen's sections sit on (its own screens, and
 * the party menu's and the bag's backgrounds, which the compositor replaces
 * with it): flat, a frame line one pixel in with a light line inside, and a
 * faint Poké Ball in the middle. Colours are BGR555.
 */
#define CTR_SECTION_RGB(r, g, b) ((uint16_t)((r) | ((g) << 5) | ((b) << 10)))
#define CTR_SECTION_BASE CTR_SECTION_RGB(23, 29, 24)
#define CTR_SECTION_LINE CTR_SECTION_RGB(19, 26, 21)
#define CTR_SECTION_LIGHT CTR_SECTION_RGB(30, 31, 30)
#define CTR_SECTION_BALL CTR_SECTION_RGB(21, 28, 23)
#define CTR_SECTION_BALL_TOP CTR_SECTION_RGB(22, 28, 23)
#define CTR_SECTION_BALL_RADIUS 64
/* The Poké Ball, 2R x 2R from the section's centre minus R: 0 nothing, 1
 * the line colour, 2 the bottom (lighter) half's. Ready after CtrBottom_Init. */
const uint8_t *CtrBottom_BallMask(void);

#endif
