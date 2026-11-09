#ifndef LR_FIXTURES_H
#define LR_FIXTURES_H
#include <string.h>
#include "lr_runtime.h"
#include "lr_mind.h"

static inline bool test_world_init(LrWorldState *world, uint16_t count, uint32_t seed, uint32_t minute) {
    static TrainerJourneyCore cores[LR_MAX_TRAINERS];
    static TrainerJourneyExtended exts[LR_MAX_TRAINERS];
    static LrTrainerTraits traits[LR_MAX_TRAINERS];
    memset(cores, 0, sizeof(cores)); memset(exts, 0, sizeof(exts)); memset(traits, 0, sizeof(traits));
    if (count > LR_MAX_TRAINERS) return false;
    for (unsigned i = 0; i < count; i++) {
        cores[i].trainerId = (uint16_t)(1000 + i * 3);
        cores[i].temperament = LR_TEMP_STEADY;
        cores[i].faction = LR_FACTION_INDEPENDENT;
        cores[i].homeRegion = cores[i].currentRegion = LR_REGION_HOENN;
        traits[i].traitsRolled = 1; traits[i].aggression = traits[i].devotion = 50;
        traits[i].rank = 4; traits[i].wealth = LR_WEALTH_AVERAGE;
        LrRollMind(&cores[i], &exts[i], &traits[i], seed + i);
        for (unsigned f = 0; f < 8; f++) cores[i].universalFacets[f] = 50;
        memset(cores[i].needs, 10, 5); cores[i].stress = 5;
        cores[i].goal1_type = cores[i].goal2_type = LR_GOAL_NONE;
        cores[i].goal1_target = cores[i].goal2_target = LR_GOAL_TARGET_NONE;
        LrDeriveValues(&cores[i], &exts[i]);
    }
    return LrWorldInit(world, cores, exts, traits, count, seed, minute, 1);
}
#endif
