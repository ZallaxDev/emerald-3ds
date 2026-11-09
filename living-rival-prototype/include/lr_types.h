#ifndef LR_TYPES_H
#define LR_TYPES_H

#include <stdint.h>
#include "lr_enums.h"

#define LR_MAX_MEMORIES 12
#define LR_HISTORY_SIZE 60
#define LR_NO_TRAINER 0xFFu
#define LR_GOAL_TARGET_NONE 0xFFFFu

#if defined(__GNUC__) || defined(__clang__)
#define LR_PACKED __attribute__((packed))
#else
#error "Define LR_PACKED for this compiler; see SAVE_SERIALIZATION_DESIGN.md"
#endif

typedef struct LR_PACKED {
    uint16_t trainerId;
    uint8_t  temperament, archetype, faction, homeRegion;
    uint8_t  journeyPhase, badgesEarned, currentRegion;
    uint32_t milestoneFlags;
    uint8_t  teamTemplateId, teamVersion;
    uint16_t speciesBitmaskLo, speciesBitmaskHi;
    uint8_t  preferredTypes[3];
    uint8_t  hate, fear, notoriety;
    int8_t   universalFacets[LR_UNIVERSAL_FACET_COUNT];
    uint8_t  regionFacetCount;
    uint8_t  regionFacetIds[LR_MAX_REGION_FACETS];
    uint8_t  factionFacetCount;
    uint8_t  factionFacetIds[LR_MAX_FACTION_FACETS];
    int8_t   values[LR_VALUE_COUNT];
    uint8_t  needs[LR_NEED_COUNT];
    uint8_t  stress;
    uint8_t  goal1_type;
    uint16_t goal1_target;
    uint8_t  goal2_type;
    uint16_t goal2_target;
    uint8_t  mentorId;
    uint8_t  studentIds[2];
    uint8_t  crewId;
    uint16_t nextMilestoneDay;
    uint8_t  activityLevel, visibilityFlags;
    uint8_t  recentRumorId, fameLevel;
    uint32_t personalSeed;
    uint8_t  lifeRules, mindRolled;
    uint16_t historyIndex;
    uint32_t facetFlags;
} TrainerJourneyCore;

typedef struct LR_PACKED {
    uint8_t  regionFacets[LR_MAX_REGION_FACETS];
    uint8_t  factionFacets[LR_MAX_FACTION_FACETS];
    uint8_t  lifeFx[16];
    uint8_t  memoryCount;
    uint8_t  memoryEmotions[LR_MAX_MEMORIES];
    uint8_t  memoryIntensities[LR_MAX_MEMORIES];
    uint8_t  memoryCauses[LR_MAX_MEMORIES];
    uint16_t memoryTimestamps[LR_MAX_MEMORIES];
    uint8_t  memoryLongTerm[LR_MAX_MEMORIES];
    uint8_t  historyTextIds[LR_HISTORY_SIZE];
    uint8_t  historyTimestamps[LR_HISTORY_SIZE];
} TrainerJourneyExtended;

typedef struct {
    uint8_t aggression;      /* 0..100 */
    uint8_t disposition;     /* LrDisposition */
    uint8_t devotion;        /* 0..100 */
    uint8_t smarts;          /* LrSmarts */
    uint8_t wealth;          /* LrWealth */
    uint8_t rank;            /* 0..8 */
    uint8_t traitsRolled;    /* 0/1 */
    uint16_t pastEvents;     /* LR_PAST_* bits */
} LrTrainerTraits;

#define LR_STATIC_ASSERT(cond, name) typedef char lr_static_assert_##name[(cond) ? 1 : -1]
LR_STATIC_ASSERT(sizeof(TrainerJourneyCore) == 87, core_size);
LR_STATIC_ASSERT(sizeof(TrainerJourneyExtended) == 221, extended_size);

#endif
