#ifndef CTR_FIELD_ICON_H
#define CTR_FIELD_ICON_H

#include <stdint.h>

/* Game-side map icons: the existing two-frame party art, with shared palettes. */
uint8_t CtrFieldIcon_Create(uint16_t species, uint32_t personality, int16_t marker);
void CtrFieldIcon_Destroy(uint8_t spriteId, uint16_t species);

#endif
