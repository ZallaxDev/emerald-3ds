#ifndef LR_FACET_TABLES_H
#define LR_FACET_TABLES_H

#include <stdint.h>
#include "lr_enums.h"

typedef struct {
    const char *name;
    uint8_t facetCount;
    const char *facetNames[LR_MAX_REGION_FACETS];
    int8_t temperamentOffsets[LR_TEMPERAMENT_COUNT][LR_MAX_REGION_FACETS];
    int8_t familyFacet;   /* local index, or -1 */
    int8_t lawFacet;
    int8_t spiritFacet;
    int8_t moneyFacet;
    int8_t prideFacet;
} LrRegionProfile;

typedef struct {
    const char *name;
    uint8_t facetCount;
    const char *facetNames[LR_MAX_FACTION_FACETS];
    int8_t archetypeOffsets[LR_ARCHETYPE_COUNT][LR_MAX_FACTION_FACETS];
} LrFactionProfile;

const LrRegionProfile *LrGetRegionProfile(uint8_t region);
const LrFactionProfile *LrGetFactionProfile(uint8_t faction);
const int8_t *LrUniversalTemperamentOffsets(uint8_t temperament);
uint8_t LrRegionFacetCount(uint8_t region);
uint8_t LrFactionFacetCount(uint8_t faction);

#endif
