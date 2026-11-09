#include "lr_daily.h"
#include "lr_goals.h"

static bool valid_slot(const LrWorldState *w, uint16_t slot, uint32_t minute) {
    return w && w->trainerCount <= LR_MAX_TRAINERS && slot < w->trainerCount && minute <= w->nowMinute;
}

void LrSyncGoals(LrWorldState *w, uint16_t slot, uint32_t minute) {
    if (!valid_slot(w, slot, minute)) return;
    const TrainerJourneyCore *c = &w->cores[slot];
    LrTrainerRuntime *r = &w->runtime[slot];
    uint8_t types[2] = {c->goal1_type,c->goal2_type};
    uint16_t targets[2] = {c->goal1_target,c->goal2_target};
    uint32_t times[2] = {minute,minute};
    for (unsigned i = 0; i < 2; i++) for (unsigned j = 0; j < 2; j++)
        if (types[i] == r->trackedGoalTypes[j] && targets[i] == r->trackedGoalTargets[j])
            times[i] = r->goalProgressMinute[j];
    for (unsigned i = 0; i < 2; i++) {
        r->trackedGoalTypes[i] = types[i]; r->trackedGoalTargets[i] = targets[i];
        r->goalProgressMinute[i] = times[i];
    }
}

bool LrSetTrackedGoal(LrWorldState *w, uint16_t slot, uint8_t type, uint16_t target, uint32_t minute) {
    if (!valid_slot(w, slot, minute) || type == LR_GOAL_NONE || type > LR_GOAL_PROTECT) return false;
    if (!LrSetGoal(&w->cores[slot], type, target)) return false;
    LrSyncGoals(w, slot, minute);
    return true;
}

bool LrRecordGoalProgress(LrWorldState *w, uint16_t slot, uint8_t type, uint16_t target, uint32_t minute) {
    if (!valid_slot(w, slot, minute) || type == LR_GOAL_NONE || type > LR_GOAL_PROTECT) return false;
    LrSyncGoals(w, slot, minute);
    LrTrainerRuntime *r = &w->runtime[slot];
    for (unsigned i = 0; i < 2; i++) if (r->trackedGoalTypes[i] == type && r->trackedGoalTargets[i] == target) {
        if (minute < r->goalProgressMinute[i]) return false;
        r->goalProgressMinute[i] = minute;
        return true;
    }
    return false;
}

static uint8_t subtract(uint8_t value, uint32_t amount) {
    return (uint8_t)(amount >= value ? 0 : value - amount);
}

void LrAgeTrainer(LrWorldState *w, uint16_t slot, uint32_t days, uint32_t minute) {
    if (!valid_slot(w, slot, minute)) return;
    TrainerJourneyCore *c = &w->cores[slot];
    TrainerJourneyExtended *e = &w->exts[slot];
    unsigned count = e->memoryCount > LR_MAX_MEMORIES ? LR_MAX_MEMORIES : e->memoryCount;
    unsigned kept = 0;
    for (unsigned i = 0; i < count; i++) {
        uint8_t intensity = e->memoryIntensities[i];
        if (days) {
            if (e->memoryLongTerm[i]) {
                intensity = subtract(intensity, days);
                if (intensity < 30) intensity = 30;
            } else intensity = subtract(intensity, days > 10 ? 100 : days * 10);
        }
        if (!intensity) continue;
        e->memoryIntensities[kept] = intensity;
        e->memoryEmotions[kept] = e->memoryEmotions[i];
        e->memoryCauses[kept] = e->memoryCauses[i];
        e->memoryLongTerm[kept] = e->memoryLongTerm[i];
        e->memoryTimestamps[kept] = e->memoryTimestamps[i];
        e->memoryTargetFacets[kept] = e->memoryTargetFacets[i];
        kept++;
    }
    e->memoryCount = (uint8_t)kept;
    for (unsigned i = kept; i < LR_MAX_MEMORIES; i++) {
        e->memoryIntensities[i] = e->memoryEmotions[i] = e->memoryCauses[i] = 0;
        e->memoryLongTerm[i] = e->memoryTargetFacets[i] = 0; e->memoryTimestamps[i] = 0;
    }
    LrSyncGoals(w, slot, minute);
    LrTrainerRuntime *r = &w->runtime[slot];
    uint8_t types[2] = {LR_GOAL_NONE,LR_GOAL_NONE};
    uint16_t targets[2] = {LR_GOAL_TARGET_NONE,LR_GOAL_TARGET_NONE};
    kept = 0;
    for (unsigned i = 0; i < 2; i++)
        if (r->trackedGoalTypes[i] && minute >= r->goalProgressMinute[i] && minute - r->goalProgressMinute[i] <= 43200) {
            types[kept] = r->trackedGoalTypes[i]; targets[kept++] = r->trackedGoalTargets[i];
        }
    c->goal1_type = types[0]; c->goal2_type = types[1];
    c->goal1_target = targets[0]; c->goal2_target = targets[1];
    LrSyncGoals(w, slot, minute);
    c->fear = subtract(c->fear, days);
    c->notoriety = subtract(c->notoriety, days > 50 ? 100 : days * 2);
}

void LrDailyTick(LrWorldState *w, const LrEventCatalog *catalog, uint32_t minute) {
    if (!w || w->trainerCount > LR_MAX_TRAINERS || minute != w->nowMinute || !LrCatalogValidate(catalog) ||
        catalog->version != w->contentVersion) return;
    for (uint16_t i = 0; i < w->trainerCount; i++) {
        LrAgeTrainer(w, i, 1, minute);
        if (!w->runtime[i].active || LrRandomNext(&w->rng) % 100 >= 60) continue;
        const LrLifeEvent *event = LrPickLifeEvent(w, i, catalog, minute, LR_CATEGORY_ANY);
        if (event) LrApplyLifeEvent(w, i, event, minute, 1);
    }
    /* Faction and world phases are reserved for Week 4; neither draws RNG yet. */
}
