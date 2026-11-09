#include "lr_absence.h"
#include "lr_daily.h"

uint16_t LrCategoryWeight(const LrWorldState *w, uint16_t slot, uint8_t category) {
    if (!w || w->trainerCount > LR_MAX_TRAINERS || slot >= w->trainerCount || !w->runtime[slot].active) return 0;
    const TrainerJourneyCore *c = &w->cores[slot];
    const LrTrainerRuntime *r = &w->runtime[slot];
    switch (category) {
    case LR_CATEGORY_TRAINING: return (uint16_t)(10 + c->universalFacets[LR_FACET_AMBITION] + c->universalFacets[LR_FACET_DISCIPLINE] + r->skill / 2);
    case LR_CATEGORY_PERSONAL: return (uint16_t)(10 + c->universalFacets[LR_FACET_SOCIAL] + c->universalFacets[LR_FACET_LOYALTY] / 2);
    case LR_CATEGORY_ACTIVITY: return (uint16_t)(10 + c->universalFacets[LR_FACET_AMBITION] / 2 + r->collector);
    case LR_CATEGORY_TEAM: return (uint16_t)(10 + r->collector + r->resources / 2);
    case LR_CATEGORY_LIFE: return (uint16_t)(10 + c->stress + (100 - r->resources) / 2);
    default: return 0;
    }
}

static uint8_t recover(uint8_t value, uint32_t days, unsigned rate) {
    uint32_t amount = days > 100 ? 100 : days * rate;
    return (uint8_t)(amount >= value ? 0 : value - amount);
}

void LrSummarizeAbsence(LrWorldState *w, const LrEventCatalog *catalog,
                        uint32_t from, uint32_t to, LrAbsenceStats *out) {
    if (!w || !out || w->trainerCount > LR_MAX_TRAINERS || from != w->nowMinute || to < from ||
        to > LR_MAX_MINUTE || !LrCatalogValidate(catalog) || catalog->version != w->contentVersion) return;
    LrAbsenceStats stats = {0};
    uint32_t duration = to - from;
    uint32_t beats = (duration + 1439) / 1440;
    if (beats > 7) beats = 7;
    uint32_t previous = from;
    for (uint32_t j = 1; j <= beats; j++) {
        uint32_t minute = from + (uint32_t)((uint64_t)duration * j / beats);
        uint32_t days = minute / 1440 - previous / 1440;
        uint32_t scale = (minute - previous + 1439) / 1440;
        if (scale > 30) scale = 30;
        w->nowMinute = minute;
        stats.agedDays += days;
        for (uint16_t i = 0; i < w->trainerCount; i++) LrAgeTrainer(w, i, days, minute);
        for (uint16_t i = 0; i < w->trainerCount; i++) {
            if (!w->runtime[i].active) continue;
            stats.beats++;
            for (unsigned n = 0; n < LR_NEED_COUNT; n++)
                w->cores[i].needs[n] = recover(w->cores[i].needs[n], days, 10);
            w->cores[i].stress = recover(w->cores[i].stress, days, 5);
            if (LrRandomNext(&w->rng) % 100 >= 60) continue;
            uint16_t weights[LR_CATEGORY_COUNT] = {0};
            for (unsigned n = 0; n < catalog->count; n++) {
                const LrLifeEvent *e = &catalog->events[n];
                if (e->odds && LrEventFits(w, i, e)) weights[e->category] = LrCategoryWeight(w, i, e->category);
            }
            unsigned total = 0;
            for (unsigned n = 0; n < LR_CATEGORY_COUNT; n++) total += weights[n];
            if (!total) continue;
            uint32_t roll = LrRandomNext(&w->rng) << 16;
            roll |= LrRandomNext(&w->rng);
            uint8_t category = (uint8_t)LrWeightedIndex(weights, LR_CATEGORY_COUNT, roll);
            const LrLifeEvent *e = LrPickLifeEvent(w, i, catalog, minute, category);
            if (e && LrApplyLifeEvent(w, i, e, minute, (uint8_t)scale)) stats.appliedEvents++;
        }
        previous = minute;
    }
    *out = stats;
}
