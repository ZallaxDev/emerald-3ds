#ifndef CTR_FIELD_ICON_PALETTE_H
#define CTR_FIELD_ICON_PALETTE_H

#include <stdint.h>

/* Recolour a 16-colour table in RAM; icon pixels and ROM assets stay unchanged.
 * The same nearest-colour approximation is used for every species. */
void CtrFieldIcon_RecolorPalette(const uint16_t icon[16],
                                 const uint16_t normal[16], const uint16_t shiny[16],
                                 uint16_t result[16]);

#endif
