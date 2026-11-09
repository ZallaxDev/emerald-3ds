#ifndef LR_MEMORY_H
#define LR_MEMORY_H

#include <stdbool.h>
#include <stdint.h>
#include "lr_types.h"

bool LrRemember(TrainerJourneyCore *core, TrainerJourneyExtended *ext,
                uint8_t emotion, uint8_t intensity, uint8_t cause,
                uint16_t nowDay, uint8_t targetFacet);
bool LrIsBadEmotion(uint8_t emotion);
int32_t LrMemoryStrength(uint8_t intensity, bool longTerm);
void LrDrift(TrainerJourneyCore *core, TrainerJourneyExtended *ext,
             uint8_t emotion, uint8_t cause, uint8_t targetFacet);

#endif
