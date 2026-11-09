#include <stddef.h>
#include "lr_life_events.h"
#include "lr_affect.h"
#include "lr_goals.h"

static bool valid_event(const LrLifeEvent *e) {
    if (!e || !e->id || !e->titleId || e->category >= LR_CATEGORY_COUNT ||
        (e->pool != 255 && e->pool >= LR_FACTION_COUNT) || e->kind >= LR_EVENT_KIND_COUNT ||
        e->conditionCount > 4 || e->effectCount > 4) return false;
    for (unsigned i = 0; i < e->conditionCount; i++) {
        const LrEventCondition *c = &e->conditions[i];
        int32_t max = c->field == LR_COND_BADGES ? 8 : c->field == LR_COND_PHASE ? LR_PHASE_RETIRED : 100;
        if (c->field >= LR_COND_COUNT || c->op > LR_OP_EQ || c->value < 0 || c->value > max) return false;
    }
    for (unsigned i = 0; i < e->effectCount; i++) {
        const LrEventEffect *f = &e->effects[i];
        if (f->kind >= LR_FX_COUNT || f->amount < -100 || f->amount > 100) return false;
        if (f->kind == LR_FX_NEED) { if (f->index >= LR_NEED_COUNT) return false; }
        else if (f->kind == LR_FX_GOAL) {
            if (!f->index || f->index > LR_GOAL_PROTECT || f->amount) return false;
        } else if (f->kind == LR_FX_REGION) {
            if (f->index >= LR_REGION_COUNT || f->amount) return false;
        } else if (f->kind == LR_FX_PHASE) {
            if (f->index > LR_PHASE_RETIRED || f->amount) return false;
        } else if (f->index) return false;
        if ((f->kind == LR_FX_TRAINING || f->kind == LR_FX_CAPTURE_INTENT ||
             f->kind == LR_FX_EVOLVE_INTENT || f->kind == LR_FX_REORGANIZE_INTENT) && f->amount < 0) return false;
    }
    return true;
}

bool LrCatalogValidate(const LrEventCatalog *catalog) {
    if (!catalog || catalog->count > LR_MAX_LIFE_EVENTS || (catalog->count && !catalog->events)) return false;
    for (unsigned i = 0; i < catalog->count; i++) {
        if (!valid_event(&catalog->events[i])) return false;
        for (unsigned j = 0; j < i; j++) if (catalog->events[j].id == catalog->events[i].id) return false;
    }
    return true;
}

static int32_t field_value(const LrWorldState *w, uint16_t slot, uint8_t field) {
    const TrainerJourneyCore *c = &w->cores[slot];
    const LrTrainerRuntime *r = &w->runtime[slot];
    switch (field) {
    case LR_COND_BADGES: return c->badgesEarned;
    case LR_COND_PHASE: return c->journeyPhase;
    case LR_COND_AMBITION: return c->universalFacets[LR_FACET_AMBITION];
    case LR_COND_SOCIAL: return c->universalFacets[LR_FACET_SOCIAL];
    case LR_COND_RESOURCES: return r->resources;
    case LR_COND_SKILL: return r->skill;
    case LR_COND_COLLECTOR: return r->collector;
    default: return c->stress;
    }
}

bool LrEventFits(const LrWorldState *w, uint16_t slot, const LrLifeEvent *e) {
    if (!w || w->trainerCount > LR_MAX_TRAINERS || slot >= w->trainerCount ||
        !w->runtime[slot].active || !valid_event(e) ||
        (e->pool != 255 && e->pool != w->cores[slot].faction)) return false;
    for (unsigned i = 0; i < e->conditionCount; i++) {
        const LrEventCondition *c = &e->conditions[i];
        int32_t value = field_value(w, slot, c->field);
        if ((c->op == LR_OP_GE && value < c->value) || (c->op == LR_OP_LE && value > c->value) ||
            (c->op == LR_OP_EQ && value != c->value)) return false;
    }
    return true;
}

uint16_t LrWeightedIndex(const uint16_t *weights, uint16_t count, uint32_t roll) {
    if (!weights || count > LR_MAX_LIFE_EVENTS) return count;
    uint32_t total = 0;
    for (uint16_t i = 0; i < count; i++) total += weights[i];
    if (!total) return count;
    roll %= total;
    for (uint16_t i = 0; i < count; i++) {
        if (roll < weights[i]) return i;
        roll -= weights[i];
    }
    return count;
}

static bool suppressed(const LrTrainerRuntime *r, const LrLifeEvent *e, uint32_t minute) {
    unsigned count = r->historyCount > LR_HISTORY_SIZE ? LR_HISTORY_SIZE : r->historyCount;
    for (unsigned i = 0; i < count; i++)
        if (r->historyEventIds[i] == e->id && minute >= r->historyMinutes[i] &&
            minute - r->historyMinutes[i] < 4320) return true;
    if (r->historyHead >= LR_HISTORY_SIZE) return true;
    for (unsigned n = 0; n < count && n < 30; n++) {
        unsigned i = (r->historyHead + LR_HISTORY_SIZE - 1 - n) % LR_HISTORY_SIZE;
        if (e->phraseId && r->historyPhraseIds[i] == e->phraseId) return true;
    }
    return false;
}

const LrLifeEvent *LrPickLifeEvent(LrWorldState *w, uint16_t slot,
                                  const LrEventCatalog *catalog, uint32_t minute, uint8_t category) {
    if (!w || !LrCatalogValidate(catalog) || catalog->version != w->contentVersion ||
        slot >= w->trainerCount || w->trainerCount > LR_MAX_TRAINERS || minute != w->nowMinute ||
        (category != LR_CATEGORY_ANY && category >= LR_CATEGORY_COUNT)) return NULL;
    uint16_t weights[LR_MAX_LIFE_EVENTS] = {0};
    uint32_t total = 0;
    for (unsigned pass = 0; pass < 2; pass++) {
        for (unsigned i = 0; i < catalog->count; i++) {
            const LrLifeEvent *e = &catalog->events[i];
            weights[i] = (e->category == category || category == LR_CATEGORY_ANY) &&
                         LrEventFits(w, slot, e) && (pass || !suppressed(&w->runtime[slot], e, minute)) ? e->odds : 0;
            total += weights[i];
        }
        if (total) break;
    }
    if (!total) return NULL;
    uint32_t roll = LrRandomNext(&w->rng) << 16;
    roll |= LrRandomNext(&w->rng);
    return &catalog->events[LrWeightedIndex(weights, catalog->count, roll)];
}

static int32_t clamp(int32_t value, int32_t max) {
    return value < 0 ? 0 : value > max ? max : value;
}

static void sync_goals(const TrainerJourneyCore *c, LrTrainerRuntime *r, uint32_t minute) {
    uint8_t types[2] = {c->goal1_type, c->goal2_type};
    uint16_t targets[2] = {c->goal1_target, c->goal2_target};
    uint32_t times[2] = {minute, minute};
    for (unsigned i = 0; i < 2; i++) for (unsigned j = 0; j < 2; j++)
        if (types[i] == r->trackedGoalTypes[j] && targets[i] == r->trackedGoalTargets[j])
            times[i] = r->goalProgressMinute[j];
    for (unsigned i = 0; i < 2; i++) {
        r->trackedGoalTypes[i] = types[i]; r->trackedGoalTargets[i] = targets[i];
        r->goalProgressMinute[i] = times[i];
    }
}

bool LrApplyLifeEvent(LrWorldState *w, uint16_t slot, const LrLifeEvent *e,
                      uint32_t minute, uint8_t scaleDays) {
    if (!w || !scaleDays || scaleDays > 30 || minute != w->nowMinute || minute > LR_MAX_MINUTE ||
        !LrEventFits(w, slot, e) || w->runtime[slot].historyHead >= LR_HISTORY_SIZE ||
        w->runtime[slot].historyCount > LR_HISTORY_SIZE) return false;
    TrainerJourneyCore core = w->cores[slot];
    TrainerJourneyExtended ext = w->exts[slot];
    LrTrainerRuntime runtime = w->runtime[slot];
    for (unsigned i = 0; i < e->effectCount; i++) {
        const LrEventEffect *f = &e->effects[i];
        int32_t amount = f->amount;
        switch (f->kind) {
        case LR_FX_NEED: core.needs[f->index] = (uint8_t)clamp(core.needs[f->index] + amount, 100); break;
        case LR_FX_RESOURCE: runtime.resources = (uint8_t)clamp(runtime.resources + amount * (amount > 0 ? scaleDays : 1), 100); break;
        case LR_FX_TRAINING: runtime.trainingPoints = (uint32_t)clamp((int32_t)runtime.trainingPoints + amount * scaleDays, 1000000); break;
        case LR_FX_CAPTURE_INTENT: runtime.capturesPending = (uint16_t)clamp(runtime.capturesPending + amount, 65535); break;
        case LR_FX_EVOLVE_INTENT: runtime.evolutionsPending = (uint16_t)clamp(runtime.evolutionsPending + amount, 65535); break;
        case LR_FX_REORGANIZE_INTENT: runtime.reorganizationsPending = (uint16_t)clamp(runtime.reorganizationsPending + amount, 65535); break;
        case LR_FX_HATE: LrAddHateCaused(&core, &ext, &w->traits[slot], amount, (uint16_t)(minute / 1440), LR_CAUSE_WORLD); break;
        case LR_FX_FEAR: LrAddFearCaused(&core, &ext, &w->traits[slot], amount, (uint16_t)(minute / 1440), LR_CAUSE_WORLD); break;
        case LR_FX_GOAL: LrSetGoal(&core, f->index, LR_GOAL_TARGET_NONE); break;
        case LR_FX_REGION: core.currentRegion = f->index; break;
        case LR_FX_STRESS: core.stress = (uint8_t)clamp(core.stress + amount, 100); break;
        case LR_FX_BADGE: core.badgesEarned = (uint8_t)clamp(core.badgesEarned + amount, 8); break;
        case LR_FX_PHASE: core.journeyPhase = f->index; if (f->index == LR_PHASE_RETIRED) runtime.active = 0; break;
        }
    }
    sync_goals(&core, &runtime, minute);
    LrWorldEvent event = {0};
    event.minute = minute; event.contentId = e->id; event.titleId = e->titleId;
    event.kind = e->kind; event.region = core.currentRegion; event.place = e->place;
    for (unsigned i = 0; i < 4; i++) { event.people[i] = LR_GOAL_TARGET_NONE; event.factions[i] = 255; }
    event.people[0] = core.trainerId; event.factions[0] = core.faction;
    if (!LrChronicleAppend(&w->chronicle, &event, NULL)) return false;
    unsigned head = runtime.historyHead;
    runtime.historyEventIds[head] = e->id; runtime.historyPhraseIds[head] = e->phraseId;
    runtime.historyMinutes[head] = minute;
    runtime.historyHead = (uint8_t)((head + 1) % LR_HISTORY_SIZE);
    if (runtime.historyCount < LR_HISTORY_SIZE) runtime.historyCount++;
    w->cores[slot] = core; w->exts[slot] = ext; w->runtime[slot] = runtime;
    return true;
}
