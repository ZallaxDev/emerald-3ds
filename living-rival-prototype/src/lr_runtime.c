#include <stddef.h>
#include <string.h>
#include "lr_runtime_internal.h"

static bool valid_goal(uint8_t type, uint16_t target) {
    return type <= LR_GOAL_PROTECT && (type != LR_GOAL_NONE || target == LR_GOAL_TARGET_NONE);
}

bool LrTrainerStateValidate(const TrainerJourneyCore *c, const TrainerJourneyExtended *e,
                            const LrTrainerTraits *t, const LrTrainerRuntime *r, uint32_t minute) {
    if (!c || !e || !t || !r || minute > LR_MAX_MINUTE || c->trainerId >= LR_TRAINER_PLAYER ||
        c->temperament >= LR_TEMPERAMENT_COUNT || c->archetype >= LR_ARCHETYPE_COUNT ||
        c->faction >= LR_FACTION_COUNT || c->homeRegion >= LR_REGION_COUNT ||
        c->currentRegion >= LR_REGION_COUNT || c->journeyPhase > LR_PHASE_RETIRED ||
        c->badgesEarned > 8 || c->regionFacetCount > LR_MAX_REGION_FACETS ||
        c->factionFacetCount > LR_MAX_FACTION_FACETS || c->stress > 100 || c->hate > 100 ||
        c->fear > 100 || c->notoriety > 100 || c->mindRolled > 1 || e->memoryCount > LR_MAX_MEMORIES ||
        t->aggression > 100 || t->devotion > 100 || t->rank > 8 || t->traitsRolled > 1 ||
        t->disposition > LR_DISPOSITION_SURLY || t->smarts > LR_SMARTS_SHARP || t->wealth > LR_WEALTH_RICH ||
        r->active > 1 || r->resources > 100 || r->skill > 100 || r->collector > 100 ||
        r->trainingPoints > 1000000 || r->historyCount > LR_HISTORY_SIZE || r->historyHead >= LR_HISTORY_SIZE ||
        (r->historyCount < LR_HISTORY_SIZE && r->historyHead != r->historyCount) ||
        (r->hostileTo != LR_GOAL_TARGET_NONE && r->hostileTo >= LR_TRAINER_PLAYER) ||
        (r->active && c->journeyPhase == LR_PHASE_RETIRED)) return false;
    if (!valid_goal(c->goal1_type, c->goal1_target) || !valid_goal(c->goal2_type, c->goal2_target) ||
        (c->goal1_type && c->goal1_type == c->goal2_type) ||
        (c->goal1_type == LR_GOAL_NONE && c->goal2_type != LR_GOAL_NONE) ||
        r->trackedGoalTypes[0] != c->goal1_type || r->trackedGoalTypes[1] != c->goal2_type ||
        r->trackedGoalTargets[0] != c->goal1_target || r->trackedGoalTargets[1] != c->goal2_target) return false;
    for (unsigned i = 0; i < 2; i++) if (r->goalProgressMinute[i] > minute) return false;
    for (unsigned i = 0; i < LR_NEED_COUNT; i++) if (c->needs[i] > 100) return false;
    for (unsigned i = 0; i < LR_UNIVERSAL_FACET_COUNT; i++)
        if (c->universalFacets[i] < 0 || c->universalFacets[i] > 100) return false;
    for (unsigned i = 0; i < LR_VALUE_COUNT; i++) if (c->values[i] < -3 || c->values[i] > 3) return false;
    for (unsigned i = 0; i < LR_MAX_REGION_FACETS; i++) if (e->regionFacets[i] > 100) return false;
    for (unsigned i = 0; i < LR_MAX_FACTION_FACETS; i++) if (e->factionFacets[i] > 100) return false;
    for (unsigned i = 0; i < e->memoryCount; i++) {
        if (e->memoryEmotions[i] >= LR_MEMORY_EMOTION_COUNT || e->memoryIntensities[i] > 100 ||
            e->memoryCauses[i] > LR_CAUSE_OTHER_TRAINER || e->memoryLongTerm[i] > 1 ||
            e->memoryTimestamps[i] > minute / 1440 ||
            e->memoryTargetFacets[i] >= LR_UNIVERSAL_FACET_COUNT + c->regionFacetCount + c->factionFacetCount)
            return false;
    }
    for (unsigned i = 0; i < r->historyCount; i++) if (r->historyMinutes[i] > minute) return false;
    return true;
}

bool LrEventStateValidate(const LrWorldEvent *e, uint32_t minute) {
    if (!e || e->id == 0 || e->id == UINT32_MAX || e->minute > minute ||
        e->kind >= LR_EVENT_KIND_COUNT || e->region >= LR_REGION_COUNT || e->heard > 1) return false;
    for (unsigned i = 0; i < 4; i++)
        if (e->factions[i] != 255 && e->factions[i] >= LR_FACTION_COUNT) return false;
    return true;
}

bool LrWorldValidate(const LrWorldState *w) {
    if (!w || w->schemaVersion != LR_WORLD_SCHEMA || w->trainerCount > LR_MAX_TRAINERS ||
        w->nowMinute > LR_MAX_MINUTE || w->rtcAnchored > 1 ||
        (!w->rtcAnchored && w->rtcHighWater != 0)) return false;
    for (unsigned i = 0; i < w->trainerCount; i++) {
        if ((i && w->cores[i-1].trainerId >= w->cores[i].trainerId) ||
            !LrTrainerStateValidate(&w->cores[i], &w->exts[i], &w->traits[i], &w->runtime[i], w->nowMinute))
            return false;
    }
    const LrChronicle *log = &w->chronicle;
    if (log->count > LR_CHRONICLE_CAPACITY || log->head >= LR_CHRONICLE_CAPACITY ||
        log->nextId == 0 || log->nextId <= log->count ||
        (log->count < LR_CHRONICLE_CAPACITY && log->head != log->count)) return false;
    for (unsigned i = 0; i < log->count; i++) {
        unsigned pos = log->count == LR_CHRONICLE_CAPACITY ? (log->head + i) % LR_CHRONICLE_CAPACITY : i;
        const LrWorldEvent *e = &log->events[pos];
        if (!LrEventStateValidate(e, w->nowMinute) || e->id != log->nextId - log->count + i) return false;
    }
    return true;
}

static void defaults(LrTrainerRuntime *r, const TrainerJourneyCore *c, const LrTrainerTraits *t,
                     uint32_t minute) {
    memset(r, 0, sizeof(*r));
    r->active = c->journeyPhase != LR_PHASE_RETIRED;
    r->resources = t->traitsRolled ? (uint8_t)(20 + t->wealth * 30) : 50;
    unsigned skill = 20 + t->rank * 8;
    r->skill = (uint8_t)(skill > 100 ? 100 : skill);
    r->collector = c->archetype == LR_ARCH_EXPERIMENTER ? 80 : 30;
    r->hostileTo = LR_GOAL_TARGET_NONE;
    r->trackedGoalTypes[0] = c->goal1_type; r->trackedGoalTypes[1] = c->goal2_type;
    r->trackedGoalTargets[0] = c->goal1_target; r->trackedGoalTargets[1] = c->goal2_target;
    r->goalProgressMinute[0] = minute; r->goalProgressMinute[1] = minute;
}

bool LrWorldInit(LrWorldState *out, const TrainerJourneyCore *cores,
                 const TrainerJourneyExtended *exts, const LrTrainerTraits *traits,
                 uint16_t count, uint32_t seed, uint32_t minute, uint32_t contentVersion) {
    if (!out || count > LR_MAX_TRAINERS || minute > LR_MAX_MINUTE || (count && (!cores || !exts))) return false;
    const LrTrainerTraits empty = {0};
    LrTrainerRuntime runtime;
    for (unsigned i = 0; i < count; i++) {
        const LrTrainerTraits *t = traits ? &traits[i] : &empty;
        defaults(&runtime, &cores[i], t, minute);
        if ((i && cores[i-1].trainerId >= cores[i].trainerId) ||
            !LrTrainerStateValidate(&cores[i], &exts[i], t, &runtime, minute)) return false;
    }
    /* Input arrays must be separate storage from the destination world. */
    uintptr_t begin = (uintptr_t)out, end = begin + sizeof(*out);
    if (count && (((uintptr_t)cores < end && (uintptr_t)cores + count * sizeof(*cores) > begin) ||
                  ((uintptr_t)exts < end && (uintptr_t)exts + count * sizeof(*exts) > begin) ||
                  (traits && (uintptr_t)traits < end && (uintptr_t)traits + count * sizeof(*traits) > begin))) return false;
    memset(out, 0, sizeof(*out));
    out->schemaVersion = LR_WORLD_SCHEMA; out->trainerCount = count;
    out->contentVersion = contentVersion; out->nowMinute = minute;
    LrRngSeed(&out->rng, seed);
    LrChronicleInit(&out->chronicle);
    for (unsigned i = 0; i < count; i++) {
        out->cores[i] = cores[i]; out->exts[i] = exts[i];
        if (traits) out->traits[i] = traits[i];
        defaults(&out->runtime[i], &out->cores[i], &out->traits[i], minute);
    }
    return true;
}

int32_t LrFindTrainer(const LrWorldState *world, uint16_t trainerId) {
    if (!world || world->trainerCount > LR_MAX_TRAINERS) return -1;
    for (uint16_t i = 0; i < world->trainerCount; i++)
        if (world->cores[i].trainerId == trainerId) return i;
    return -1;
}
