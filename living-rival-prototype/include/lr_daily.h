#ifndef LR_DAILY_H
#define LR_DAILY_H
#include "lr_life_events.h"

void LrAgeTrainer(LrWorldState *world, uint16_t slot, uint32_t elapsedDays, uint32_t minute);
void LrSyncGoals(LrWorldState *world, uint16_t slot, uint32_t minute);
bool LrSetTrackedGoal(LrWorldState *world, uint16_t slot, uint8_t type, uint16_t target, uint32_t minute);
bool LrRecordGoalProgress(LrWorldState *world, uint16_t slot, uint8_t type, uint16_t target, uint32_t minute);
void LrDailyTick(LrWorldState *world, const LrEventCatalog *catalog, uint32_t minute);
#endif
