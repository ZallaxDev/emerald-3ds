#ifndef LR_NEEDS_H
#define LR_NEEDS_H
#include "lr_runtime.h"

/* Scheduling primitives: caller supplies a validated world and elapsed boundary. */
void LrTickNeeds(TrainerJourneyCore *core, const TrainerJourneyExtended *ext, uint32_t minute);
bool LrTrySnap(LrWorldState *world, uint16_t slot, uint32_t minute);
void LrHourlyTick(LrWorldState *world, uint32_t minute);
#endif
