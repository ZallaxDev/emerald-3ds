#ifndef LR_MIND_H
#define LR_MIND_H

#include <stdint.h>
#include "lr_types.h"

void LrRollMind(TrainerJourneyCore *core, TrainerJourneyExtended *ext,
                const LrTrainerTraits *traits, uint32_t seed);
int32_t LrComputeBaseline(const TrainerJourneyCore *core, uint16_t globalFacetIndex);
int32_t LrGetFacet(const TrainerJourneyCore *core, const TrainerJourneyExtended *ext,
                   uint16_t globalFacetIndex);
void LrDeriveValues(TrainerJourneyCore *core, const TrainerJourneyExtended *ext);

#endif
