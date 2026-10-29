#ifndef CTR_NUZLOCKE_UI_H
#define CTR_NUZLOCKE_UI_H

/*
 * The Nuzlocke tracker's cells of OPTIONS (src/3ds_nuzlocke_extras.inc, whose
 * lines go into gCtrExtras): their callbacks and texts, defined in
 * src/3ds_nuzlocke_game.c. Texts returned are in the game's own characters.
 * SDK-free.
 */
#include <stdint.h>

/* settings.txt key of the tracker's ON/OFF cell; OFF by default. */
#define CTR_NUZLOCKE_KEY "nuzlocke"

/* One text each, for the action cells. */
extern const char *const gCtrNuzlockeOpenText[1];
extern const char *const gCtrNuzlockeBackText[1];
extern const char *const gCtrNuzlockeRecordText[1];
extern const char *const gCtrNuzlockeInfoText[1];

void CtrNuzlockeUi_Open(void);
void CtrNuzlockeUi_Back(void);
/* After the ON/OFF cell changed. */
void CtrNuzlockeUi_Toggled(void);
/* Information cells: a tap only plays a sound. */
void CtrNuzlockeUi_Info(void);

const uint8_t *CtrNuzlockeUi_AreaText(void);
const uint8_t *CtrNuzlockeUi_RecordedText(void);
void CtrNuzlockeUi_ChoiceStep(int direction);
const uint8_t *CtrNuzlockeUi_ChoiceText(void);
/* Writes the chosen status for the current area. */
void CtrNuzlockeUi_Record(void);
void CtrNuzlockeUi_SlotStep(int direction);
const uint8_t *CtrNuzlockeUi_SlotText(void);
/* Marks (or clears) the chosen party Pokemon as fallen: a second tap confirms. */
void CtrNuzlockeUi_Fallen(void);
const uint8_t *CtrNuzlockeUi_FallenText(void);
/* Marks every fainted party Pokemon as fallen: a second tap confirms. */
void CtrNuzlockeUi_MarkFainted(void);
const uint8_t *CtrNuzlockeUi_MarkFaintedText(void);
const uint8_t *CtrNuzlockeUi_CapText(void);
const uint8_t *CtrNuzlockeUi_TotalsText(void);
const uint8_t *CtrNuzlockeUi_LinkText(void);

/* For the bottom screen's NUZLOCKE section, which shows them as buttons. */
/* Whether the tracker is on. */
int CtrNuzlockeUi_Enabled(void);
/* The current area's status (0 unused, 1 caught, 2 missed), -1 with none. */
int CtrNuzlockeUi_AreaStatus(void);
/* Records that status for the current area (CtrNuzlockeUi_Record). */
void CtrNuzlockeUi_RecordAs(int status);
/* The chosen party slot, and choosing one (an empty slot is not chosen). */
int CtrNuzlockeUi_Slot(void);
void CtrNuzlockeUi_SetSlot(int slot);
/* Whether the party Pokemon in a slot is marked fallen. */
int CtrNuzlockeUi_SlotFallen(int slot);

#endif
