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
/* Back from HOME or sleep (the APT hook): a touch held across it is let go
 * and the screen drawn again, on the next Frame. */
void CtrBottom_OnResume(void);
/* Keys the bottom screen presses on the player's behalf this frame. */
uint16_t CtrBottom_InjectedKeys(void);
/* The player's keys as the game should see them: none while the button
 * column has the X focus, or until the keys that left it are let go. */
uint16_t CtrBottom_FilterKeys(uint16_t held);
#ifdef CTR_TEST_SWOOP
void CtrBottom_TestSwoop(unsigned step);
#endif

#endif
