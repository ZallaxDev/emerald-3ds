#ifndef CTR_FIELD_ICON_H
#define CTR_FIELD_ICON_H

#include <stdint.h>
#include <stdbool.h>

/* Game-side map icons: the existing two-frame party art, with shared palettes. */
uint8_t CtrFieldIcon_Create(uint16_t species, uint32_t personality, int16_t marker, bool shiny);
void CtrFieldIcon_Destroy(uint8_t spriteId, uint16_t species);
/* Pixel movement this frame: scale the bob and retain facing on vertical/idle frames. */
void CtrFieldIcon_SetMotion(uint8_t spriteId, int16_t dx, int16_t dy);
/* Transparent rows below the standing frame, measured before rendering. */
int CtrFieldIcon_FootPad(uint8_t spriteId);

#endif
