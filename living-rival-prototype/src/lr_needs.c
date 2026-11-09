#include "lr_needs.h"
#include "lr_memory.h"

static uint8_t add100(uint8_t value, unsigned amount) {
    unsigned result = value + amount;
    return (uint8_t)(result > 100 ? 100 : result);
}

void LrTickNeeds(TrainerJourneyCore *c, const TrainerJourneyExtended *e, uint32_t minute) {
    if (!c || !e) return;
    c->needs[LR_NEED_DRINK] = add100(c->needs[LR_NEED_DRINK], 5);
    if (minute % 360 == 0) c->needs[LR_NEED_COMPANY] = add100(c->needs[LR_NEED_COMPANY], 5);
    if (minute % 720 == 0) c->needs[LR_NEED_FAITH] = add100(c->needs[LR_NEED_FAITH], 10);
    if (minute % 180 == 0) c->needs[LR_NEED_MONEY] = add100(c->needs[LR_NEED_MONEY], 8);
    unsigned violence = c->faction == LR_FACTION_TEAM_MAGMA || c->faction == LR_FACTION_TEAM_AQUA ? 3 : 1;
    c->needs[LR_NEED_VIOLENCE] = add100(c->needs[LR_NEED_VIOLENCE], violence);
    unsigned stress = 0;
    for (unsigned i = 0; i < LR_NEED_COUNT; i++) if (c->needs[i] >= 70) stress += 2;
    unsigned count = e->memoryCount > LR_MAX_MEMORIES ? LR_MAX_MEMORIES : e->memoryCount;
    for (unsigned i = 0; i < count; i++)
        if (e->memoryLongTerm[i] && LrIsBadEmotion(e->memoryEmotions[i])) stress++;
    c->stress = add100(c->stress, stress);
}

static bool comrade(const LrWorldState *w, uint16_t actor, uint16_t other) {
    return actor != other && w->runtime[other].active &&
           w->cores[actor].faction == w->cores[other].faction &&
           w->cores[actor].currentRegion == w->cores[other].currentRegion;
}

bool LrTrySnap(LrWorldState *w, uint16_t slot, uint32_t minute) {
    if (!w || w->trainerCount > LR_MAX_TRAINERS || slot >= w->trainerCount ||
        !w->runtime[slot].active || w->cores[slot].stress < 85 || minute > LR_MAX_MINUTE ||
        w->cores[slot].faction == LR_FACTION_INDEPENDENT ||
        w->chronicle.nextId == 0 || w->chronicle.nextId == UINT32_MAX ||
        w->chronicle.count > LR_CHRONICLE_CAPACITY || w->chronicle.head >= LR_CHRONICLE_CAPACITY) return false;
    uint16_t count = 0;
    for (uint16_t i = 0; i < w->trainerCount; i++) if (comrade(w, slot, i)) count++;
    if (!count) return false;
    LrRng rng = w->rng;
    if (LrRandomNext(&rng) % 100 >= 2) { w->rng = rng; return false; }
    uint32_t choice = LrRandomNext(&rng) % count;
    uint16_t victim = 0;
    for (uint16_t i = 0; i < w->trainerCount; i++) if (comrade(w, slot, i)) {
        if (choice == 0) { victim = i; break; }
        choice--;
    }
    LrWorldEvent event = {0};
    event.minute = minute; event.kind = LR_EVENT_CLASH;
    event.region = w->cores[slot].currentRegion;
    for (unsigned i = 0; i < 4; i++) { event.people[i] = LR_GOAL_TARGET_NONE; event.factions[i] = 255; }
    event.people[0] = w->cores[slot].trainerId; event.people[1] = w->cores[victim].trainerId;
    event.factions[0] = w->cores[slot].faction;
    event.outcomes[0] = 1; event.outcomes[1] = 2;
    if (!LrChronicleAppend(&w->chronicle, &event, 0)) return false;
    w->rng = rng;
    w->runtime[slot].hostileTo = event.people[1];
    w->cores[slot].stress = (uint8_t)(w->cores[slot].stress - 20);
    return true;
}

void LrHourlyTick(LrWorldState *w, uint32_t minute) {
    if (!w || w->trainerCount > LR_MAX_TRAINERS) return;
    for (uint16_t i = 0; i < w->trainerCount; i++) if (w->runtime[i].active) {
        LrTickNeeds(&w->cores[i], &w->exts[i], minute);
        LrTrySnap(w, i, minute);
    }
}
