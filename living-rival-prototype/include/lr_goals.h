#ifndef LR_GOALS_H
#define LR_GOALS_H

#include <stdbool.h>
#include <stdint.h>
#include "lr_types.h"

int32_t LrHateTier(uint8_t hate);
int32_t LrInitGoals(TrainerJourneyCore *core, const LrTrainerTraits *traits);
bool LrSetGoal(TrainerJourneyCore *core, uint8_t type, uint16_t target);
bool LrHasGoal(const TrainerJourneyCore *core, uint8_t type);
bool LrHasGoalAgainstPlayer(const TrainerJourneyCore *core, uint8_t type);
uint8_t LrShiftIntent(const TrainerJourneyCore *core, uint8_t intent, int32_t effectiveFear);
int32_t LrAmbushMult(const TrainerJourneyCore *core);

#endif
