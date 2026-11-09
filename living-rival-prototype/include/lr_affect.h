#ifndef LR_AFFECT_H
#define LR_AFFECT_H

#include <stdint.h>
#include "lr_types.h"

/* All multipliers are per-mille (1000 == 1.0). LrAddHate/LrAddFear apply the
 * chain to positive deltas internally: call them with the raw base value and
 * do not pre-multiply. LrCalculate* is for previews only. */
int32_t LrCalculateHateDelta(int32_t base, const TrainerJourneyCore *core, const LrTrainerTraits *traits);
int32_t LrCalculateFearDelta(int32_t base, const TrainerJourneyCore *core, const LrTrainerTraits *traits);
void LrAddHate(TrainerJourneyCore *core, TrainerJourneyExtended *ext,
               const LrTrainerTraits *traits, int32_t delta, uint16_t nowDay);
void LrAddFear(TrainerJourneyCore *core, TrainerJourneyExtended *ext,
               const LrTrainerTraits *traits, int32_t delta, uint16_t nowDay);
void LrAddNotoriety(TrainerJourneyCore *core, int32_t delta);
int32_t LrEffectiveFear(const TrainerJourneyCore *core);

#endif
