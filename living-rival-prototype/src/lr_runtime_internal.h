#ifndef LR_RUNTIME_INTERNAL_H
#define LR_RUNTIME_INTERNAL_H
#include "lr_runtime.h"

bool LrTrainerStateValidate(const TrainerJourneyCore *core, const TrainerJourneyExtended *ext,
                            const LrTrainerTraits *traits, const LrTrainerRuntime *runtime,
                            uint32_t minute);
bool LrEventStateValidate(const LrWorldEvent *event, uint32_t minute);

#endif
