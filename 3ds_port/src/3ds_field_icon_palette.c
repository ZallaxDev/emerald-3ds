#include "3ds_field_icon_palette.h"

static unsigned ColorDistance(uint16_t a, uint16_t b)
{
    int r = (a & 31) - (b & 31);
    int g = ((a >> 5) & 31) - ((b >> 5) & 31);
    int blue = ((a >> 10) & 31) - ((b >> 10) & 31);
    return r * r + g * g + blue * blue;
}

void CtrFieldIcon_RecolorPalette(const uint16_t icon[16],
                                 const uint16_t normal[16], const uint16_t shiny[16],
                                 uint16_t result[16])
{
    for (unsigned i = 0; i < 16; ++i)
        result[i] = icon[i];
    /* Combat and icon palettes use different indices and levels of detail.
     * Every species uses the same nearest-colour approximation. */
    for (unsigned i = 1; i < 16; ++i)
    {
        /* Shared palettes reserve these entries for white highlights and dark outlines.
         * Other grey entries can be a body's colour (e.g. rock Pokémon), so map them too. */
        if (i == 3 || i == 14 || i == 15)
            continue;
        unsigned nearest = 1;
        unsigned distance = ColorDistance(icon[i], normal[nearest]);
        for (unsigned j = 2; j < 16; ++j)
        {
            unsigned candidate = ColorDistance(icon[i], normal[j]);
            if (candidate < distance)
            {
                nearest = j;
                distance = candidate;
            }
        }
        if (normal[nearest] != shiny[nearest])
            result[i] = shiny[nearest];
    }
}
