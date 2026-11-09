#include "lr_memory.h"
#include "lr_facet_tables.h"
#include "lr_goals.h"
#include "lr_mind.h"

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int memory_count(const TrainerJourneyExtended *ext) {
    return ext->memoryCount > LR_MAX_MEMORIES ? LR_MAX_MEMORIES : (int)ext->memoryCount;
}

static int region_count(const TrainerJourneyCore *core) {
    return core->regionFacetCount > LR_MAX_REGION_FACETS ? LR_MAX_REGION_FACETS : (int)core->regionFacetCount;
}

static int faction_count(const TrainerJourneyCore *core) {
    return core->factionFacetCount > LR_MAX_FACTION_FACETS ? LR_MAX_FACTION_FACETS : (int)core->factionFacetCount;
}

static void set_facet(TrainerJourneyCore *core, TrainerJourneyExtended *ext, int idx, int value) {
    value = clamp_int(value, 0, 100);
    if (idx < LR_UNIVERSAL_FACET_COUNT) {
        core->universalFacets[idx] = (int8_t)value;
        return;
    }
    idx -= LR_UNIVERSAL_FACET_COUNT;
    if (idx < region_count(core)) {
        ext->regionFacets[idx] = (uint8_t)value;
        return;
    }
    idx -= region_count(core);
    if (idx < faction_count(core)) {
        ext->factionFacets[idx] = (uint8_t)value;
    }
}

static void add_universal(TrainerJourneyCore *core, int facet, int delta) {
    int v = clamp_int(core->universalFacets[facet] + delta, 0, 100);
    core->universalFacets[facet] = (int8_t)v;
}

static void add_facet(TrainerJourneyCore *core, TrainerJourneyExtended *ext, int idx, int delta) {
    set_facet(core, ext, idx, LrGetFacet(core, ext, (uint16_t)idx) + delta);
}

static int pride_index(const TrainerJourneyCore *core) {
    const LrRegionProfile *rp = LrGetRegionProfile(core->homeRegion);
    if (rp->prideFacet >= 0 && (int)rp->prideFacet < region_count(core))
        return LR_UNIVERSAL_FACET_COUNT + (int)rp->prideFacet;
    return LR_FACET_AMBITION;
}

static int32_t strength_at(const TrainerJourneyExtended *ext, int i) {
    return LrMemoryStrength(ext->memoryIntensities[i], ext->memoryLongTerm[i] != 0);
}

static void swap_memories(TrainerJourneyExtended *ext, int a, int b) {
    uint8_t e = ext->memoryEmotions[a]; ext->memoryEmotions[a] = ext->memoryEmotions[b]; ext->memoryEmotions[b] = e;
    uint8_t i = ext->memoryIntensities[a]; ext->memoryIntensities[a] = ext->memoryIntensities[b]; ext->memoryIntensities[b] = i;
    uint8_t c = ext->memoryCauses[a]; ext->memoryCauses[a] = ext->memoryCauses[b]; ext->memoryCauses[b] = c;
    uint16_t t = ext->memoryTimestamps[a]; ext->memoryTimestamps[a] = ext->memoryTimestamps[b]; ext->memoryTimestamps[b] = t;
    uint8_t l = ext->memoryLongTerm[a]; ext->memoryLongTerm[a] = ext->memoryLongTerm[b]; ext->memoryLongTerm[b] = l;
    uint8_t f = ext->memoryTargetFacets[a]; ext->memoryTargetFacets[a] = ext->memoryTargetFacets[b]; ext->memoryTargetFacets[b] = f;
}

static void sort_memories(TrainerJourneyExtended *ext) {
    const int count = memory_count(ext);
    for (int i = 1; i < count; i++) {
        int j = i;
        while (j > 0 && strength_at(ext, j) > strength_at(ext, j - 1)) {
            swap_memories(ext, j, j - 1);
            j--;
        }
    }
}

static int find_memory(const TrainerJourneyExtended *ext, uint8_t emotion, uint8_t cause) {
    const int count = memory_count(ext);
    for (int i = 0; i < count; i++) {
        if (ext->memoryEmotions[i] == emotion && ext->memoryCauses[i] == cause && !ext->memoryLongTerm[i])
            return i;
    }
    return -1;
}

bool LrIsBadEmotion(uint8_t emotion) {
    return emotion == LR_MEMORY_ANGER || emotion == LR_MEMORY_FEAR ||
           emotion == LR_MEMORY_SHAME || emotion == LR_MEMORY_GRIEF;
}

int32_t LrMemoryStrength(uint8_t intensity, bool longTerm) {
    int32_t v = intensity;
    if (longTerm) v += 50;
    return v;
}

void LrDrift(TrainerJourneyCore *core, TrainerJourneyExtended *ext,
             uint8_t emotion, uint8_t cause, uint8_t targetFacet) {
    const int32_t baseline = LrComputeBaseline(core, targetFacet);
    const int32_t current = LrGetFacet(core, ext, targetFacet);

    switch (emotion) {
    case LR_MEMORY_ANGER:
        set_facet(core, ext, targetFacet, (int)(current + clamp_int((int)(baseline + 25 - current), 0, 5)));
        add_universal(core, LR_FACET_CRUELTY, 2);
        break;
    case LR_MEMORY_FEAR:
        add_universal(core, LR_FACET_BRAVERY, -6);
        break;
    case LR_MEMORY_SHAME: {
        int pride = pride_index(core);
        if (LrGetFacet(core, ext, (uint16_t)pride) >= 55) {
            add_facet(core, ext, pride, 3);
            int32_t prideBaseline = LrComputeBaseline(core, (uint16_t)pride);
            int32_t target = LrGetFacet(core, ext, targetFacet);
            set_facet(core, ext, targetFacet,
                      (int)(target + clamp_int((int)(prideBaseline + 25 - target), 0, 4)));
        } else {
            add_universal(core, LR_FACET_BRAVERY, -4);
            add_facet(core, ext, pride, -3);
        }
        break;
    }
    case LR_MEMORY_GRIEF:
        set_facet(core, ext, targetFacet, (int)(current + clamp_int((int)(baseline + 25 - current), 0, 3)));
        add_universal(core, LR_FACET_SOCIAL, -5);
        add_universal(core, LR_FACET_LOYALTY, 2);
        break;
    case LR_MEMORY_JOY:
        add_universal(core, LR_FACET_SOCIAL, 4);
        set_facet(core, ext, targetFacet, LrGetFacet(core, ext, targetFacet) - 3);
        break;
    case LR_MEMORY_PRIDE: {
        int pride = pride_index(core);
        add_facet(core, ext, pride, 3);
        add_universal(core, LR_FACET_BRAVERY, 3);
        break;
    }
    default:
        break;
    }

    if (cause == LR_CAUSE_PLAYER) {
        if (emotion == LR_MEMORY_ANGER || emotion == LR_MEMORY_SHAME || emotion == LR_MEMORY_GRIEF) {
            if (core->universalFacets[LR_FACET_ANGER] >= 50 &&
                !LrHasGoal(core, LR_GOAL_AVENGE) &&
                core->faction != LR_FACTION_ELITE_FOUR_PATH) {
                LrSetGoal(core, LR_GOAL_AVENGE, LR_TRAINER_PLAYER);
            }
        } else if (emotion == LR_MEMORY_FEAR) {
            if (core->universalFacets[LR_FACET_BRAVERY] < 35 &&
                !LrHasGoal(core, LR_GOAL_SETTLE)) {
                LrSetGoal(core, LR_GOAL_SETTLE, LR_GOAL_TARGET_NONE);
            }
        }
    }

    LrDeriveValues(core, ext);
}

bool LrRemember(TrainerJourneyCore *core, TrainerJourneyExtended *ext,
                uint8_t emotion, uint8_t intensity, uint8_t cause,
                uint16_t nowDay, uint8_t targetFacet) {
    if (emotion >= LR_MEMORY_EMOTION_COUNT) return false;
    if (cause > LR_CAUSE_OTHER_TRAINER) return false;
    if (intensity > 100) intensity = 100;
    if (ext->memoryCount > LR_MAX_MEMORIES) ext->memoryCount = (uint8_t)LR_MAX_MEMORIES;

    if (LrIsBadEmotion(emotion))
        core->stress = (uint8_t)clamp_int((int)core->stress + (int)intensity / 8, 0, 100);
    else
        core->stress = (uint8_t)clamp_int((int)core->stress - (int)intensity / 10, 0, 100);

    int idx = find_memory(ext, emotion, cause);
    bool became = false;

    if (idx >= 0) {
        int v = clamp_int((int)ext->memoryIntensities[idx] + (int)intensity / 2 + 5, 0, 100);
        ext->memoryIntensities[idx] = (uint8_t)v;
        ext->memoryTimestamps[idx] = nowDay;
        ext->memoryTargetFacets[idx] = targetFacet;

        if (!ext->memoryLongTerm[idx] && v >= 65) {
            ext->memoryLongTerm[idx] = 1;
            became = true;
            LrDrift(core, ext, emotion, cause, targetFacet);
        }
    } else {
        int32_t strength = LrMemoryStrength(intensity, false);

        if (ext->memoryCount < LR_MAX_MEMORIES) {
            idx = ext->memoryCount++;
        } else {
            sort_memories(ext);
            int weakest = LR_MAX_MEMORIES - 1;
            if (strength <= strength_at(ext, weakest)) return false;
            idx = weakest;
        }

        ext->memoryEmotions[idx] = emotion;
        ext->memoryIntensities[idx] = intensity;
        ext->memoryCauses[idx] = cause;
        ext->memoryTimestamps[idx] = nowDay;
        ext->memoryLongTerm[idx] = 0;
        ext->memoryTargetFacets[idx] = targetFacet;

        if (intensity >= 65) {
            ext->memoryLongTerm[idx] = 1;
            became = true;
            LrDrift(core, ext, emotion, cause, targetFacet);
        }
    }

    sort_memories(ext);
    return became;
}
