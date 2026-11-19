/*
 * A panel the bottom screen's code draws for the top screen (the Rental
 * Pokemon's): 400x240 RGB565 in the bottom canvas' layout (column by column,
 * each from the bottom row up), shown over the whole top screen in place of
 * the game's picture while the bottom screen keeps asking for it.
 *
 * The bottom screen draws straight into the panel's canvas
 * (CtrTopPanel_Canvas) and hands the picture over only when it is whole
 * and has changed (CtrTopPanel_Show), and says every frame that it is still
 * wanted (CtrTopPanel_Keep), with how far the game's own fade to black has
 * gone, which the panel follows; a few frames without that and the panel is
 * gone, its texture given back (CtrTopPanel_Idle). If the texture cannot be
 * had the panel is a flat colour: the game's picture never shows through.
 */
#ifndef CTR_TOP_PANEL_INCLUDED
#define CTR_TOP_PANEL_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#define CTR_TOP_PANEL_W 400
#define CTR_TOP_PANEL_H 240

/* The picture's buffer: drawn into, then CtrTopPanel_Show. What is shown is only taken from it inside the frame. */
uint16_t *CtrTopPanel_Canvas(void);
void CtrTopPanel_Show(void);
/* dark: 0..16, how far towards black the panel is (the game's fade); 16 is fully black. */
void CtrTopPanel_Keep(unsigned dark);

/* The video side, inside the frame. */
bool CtrTopPanel_Active(void);
void CtrTopPanel_Tick(void);
void CtrTopPanel_Upload(void);
void CtrTopPanel_Draw(void);
void CtrTopPanel_Idle(void);

#endif
