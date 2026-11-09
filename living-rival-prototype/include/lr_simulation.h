#ifndef LR_SIMULATION_H
#define LR_SIMULATION_H
#include "lr_absence.h"
typedef struct {
    uint32_t fromMinute, toMinute, summaryEndMinute, hourlyTicks, dailyTicks, summaryBeats, appliedSummaryEvents;
} LrAdvanceStats;

bool LrSimulateTo(LrWorldState *world, const LrEventCatalog *catalog, uint32_t targetMinute, LrAdvanceStats *out);
bool LrResumeOffline(LrWorldState *world, const LrEventCatalog *catalog, uint64_t rtcMinute, LrAdvanceStats *out);
bool LrAnchorOfflineClock(LrWorldState *world, uint64_t rtcMinute);
#endif
