#ifndef LR_ABSENCE_H
#define LR_ABSENCE_H
#include "lr_life_events.h"
typedef struct { uint32_t beats, appliedEvents, agedDays; } LrAbsenceStats;

/* Intrinsic preference only; summary selection additionally checks catalog eligibility. */
uint16_t LrCategoryWeight(const LrWorldState *world, uint16_t slot, uint8_t category);
void LrSummarizeAbsence(LrWorldState *world, const LrEventCatalog *catalog,
                        uint32_t fromMinute, uint32_t toMinute, LrAbsenceStats *out);
#endif
