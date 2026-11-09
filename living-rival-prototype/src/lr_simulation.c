#include "lr_simulation.h"
#include "lr_daily.h"
#include "lr_needs.h"

static bool valid_inputs(const LrWorldState *world, const LrEventCatalog *catalog, const LrAdvanceStats *out) {
    return out && LrWorldValidate(world) && LrCatalogValidate(catalog) && world->contentVersion == catalog->version;
}

bool LrSimulateTo(LrWorldState *w, const LrEventCatalog *catalog, uint32_t target, LrAdvanceStats *out) {
    if (!valid_inputs(w,catalog,out) || target < w->nowMinute || target > LR_MAX_MINUTE) return false;
    LrAdvanceStats stats = {0};
    stats.fromMinute = w->nowMinute; stats.toMinute = target; stats.summaryEndMinute = w->nowMinute;
    if (target - w->nowMinute > 2880) {
        stats.summaryEndMinute = target - 2880;
        LrAbsenceStats summary;
        LrSummarizeAbsence(w,catalog,w->nowMinute,stats.summaryEndMinute,&summary);
        stats.summaryBeats = summary.beats; stats.appliedSummaryEvents = summary.appliedEvents;
    }
    uint32_t boundary = w->nowMinute / 60 * 60 + 60;
    for (; boundary <= target; boundary += 60) {
        w->nowMinute = boundary;
        LrHourlyTick(w,boundary); stats.hourlyTicks++;
        if (boundary % 1440 == 0) { LrDailyTick(w,catalog,boundary); stats.dailyTicks++; }
    }
    w->nowMinute = target;
    *out = stats;
    return true;
}

bool LrAnchorOfflineClock(LrWorldState *w, uint64_t rtcMinute) {
    if (!LrWorldValidate(w)) return false;
    if (rtcMinute > w->rtcHighWater) w->rtcHighWater = rtcMinute;
    w->rtcAnchored = 1;
    return true;
}

bool LrResumeOffline(LrWorldState *w, const LrEventCatalog *catalog, uint64_t rtcMinute, LrAdvanceStats *out) {
    if (!valid_inputs(w,catalog,out)) return false;
    uint64_t elapsed = w->rtcAnchored && rtcMinute > w->rtcHighWater ? rtcMinute - w->rtcHighWater : 0;
    if (elapsed > LR_MAX_MINUTE - w->nowMinute) return false;
    if (!LrSimulateTo(w,catalog,w->nowMinute + (uint32_t)elapsed,out)) return false;
    if (rtcMinute > w->rtcHighWater) w->rtcHighWater = rtcMinute;
    w->rtcAnchored = 1;
    return true;
}
