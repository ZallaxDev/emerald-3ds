/*
 * A panel the bottom screen's code draws for the top screen (the Rental
 * Pokemon's): 400x240 RGB565 in the bottom canvas' layout (column by column,
 * each from the bottom row up), shown over the whole top screen in place of
 * the game's picture while the bottom screen keeps asking for it.
 *
 * The bottom screen hands a new picture over only when it changes
 * (CtrTopPanel_Show) and says every frame that it is still wanted
 * (CtrTopPanel_Keep); a few frames without that and the panel is gone, its
 * texture given back (CtrTopPanel_Idle).
 */
#ifndef CTR_TOP_PANEL_INCLUDED
#define CTR_TOP_PANEL_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#define CTR_TOP_PANEL_W 400
#define CTR_TOP_PANEL_H 240

void CtrTopPanel_Show(const uint16_t *canvas);
void CtrTopPanel_Keep(void);

/* The video side, inside the frame. */
bool CtrTopPanel_Active(void);
void CtrTopPanel_Tick(void);
void CtrTopPanel_Upload(void);
void CtrTopPanel_Draw(void);
void CtrTopPanel_Idle(void);

#endif
