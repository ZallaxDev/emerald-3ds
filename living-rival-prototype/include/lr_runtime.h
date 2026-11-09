#ifndef LR_RUNTIME_H
#define LR_RUNTIME_H

#include "lr_chronicle.h"
#include "lr_rng.h"

#define LR_MAX_TRAINERS 80
#define LR_WORLD_SCHEMA 1

typedef struct {
    uint8_t active, resources, skill, collector;
    uint32_t trainingPoints;
    uint16_t capturesPending, evolutionsPending, reorganizationsPending, hostileTo;
    uint8_t trackedGoalTypes[2];
    uint16_t trackedGoalTargets[2];
    uint32_t goalProgressMinute[2];
    uint16_t historyEventIds[LR_HISTORY_SIZE], historyPhraseIds[LR_HISTORY_SIZE];
    uint32_t historyMinutes[LR_HISTORY_SIZE];
    uint8_t historyCount, historyHead;
} LrTrainerRuntime;

typedef struct {
    uint16_t schemaVersion, trainerCount;
    uint32_t contentVersion, nowMinute;
    uint64_t rtcHighWater;
    uint8_t rtcAnchored;
    LrRng rng;
    TrainerJourneyCore cores[LR_MAX_TRAINERS];
    TrainerJourneyExtended exts[LR_MAX_TRAINERS];
    LrTrainerTraits traits[LR_MAX_TRAINERS];
    LrTrainerRuntime runtime[LR_MAX_TRAINERS];
    LrChronicle chronicle;
} LrWorldState;

bool LrWorldInit(LrWorldState *out, const TrainerJourneyCore *cores,
                 const TrainerJourneyExtended *exts, const LrTrainerTraits *traits,
                 uint16_t count, uint32_t seed, uint32_t minute, uint32_t contentVersion);
bool LrWorldValidate(const LrWorldState *world);
int32_t LrFindTrainer(const LrWorldState *world, uint16_t trainerId);

#endif
