#include "lr_affect.h"
#include "lr_memory.h"
#include "lr_mind.h"

static int32_t clamp32(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static uint8_t safe_temperament(uint8_t t) {
    return t < LR_TEMPERAMENT_COUNT ? t : (uint8_t)LR_TEMP_STEADY;
}

static int32_t temperament_hate_gain(uint8_t t) {
    static const int32_t table[LR_TEMPERAMENT_COUNT] = {
        1300, 600, 1150, 900, 850, 1000, 700, 1000
    };
    return table[safe_temperament(t)];
}

static int32_t temperament_fear_gain(uint8_t t) {
    static const int32_t table[LR_TEMPERAMENT_COUNT] = {
        500, 2000, 600, 800, 1000, 700, 1200, 1000
    };
    return table[safe_temperament(t)];
}

static int32_t trait_hate_gain(const LrTrainerTraits *traits) {
    if (!traits || !traits->traitsRolled) return 1000;
    int32_t m = 1000 + ((int32_t)traits->aggression - 50) * 4;
    if (traits->disposition == LR_DISPOSITION_FRIENDLY) m -= 100;
    if (traits->disposition == LR_DISPOSITION_SURLY) m += 100;
    return m;
}

static int32_t trait_fear_gain(const LrTrainerTraits *traits) {
    if (!traits || !traits->traitsRolled) return 1000;
    return 1000 - ((int32_t)traits->aggression - 50) * 2;
}

static int32_t mind_hate_gain(const TrainerJourneyCore *core) {
    int32_t baseline = LrComputeBaseline(core, LR_FACET_ANGER);
    int32_t delta = (int32_t)core->universalFacets[LR_FACET_ANGER] - baseline;
    delta = clamp32(delta, -20, 20);
    return 1000 + delta * 10;
}

static int32_t mind_fear_gain(const TrainerJourneyCore *core) {
    int32_t baseline = LrComputeBaseline(core, LR_FACET_BRAVERY);
    int32_t delta = (int32_t)core->universalFacets[LR_FACET_BRAVERY] - baseline;
    delta = clamp32(delta, -30, 30);
    return 1000 - delta * 10;
}

static int32_t round_div(int32_t num, int32_t den) {
    if (num >= 0) return (num + den / 2) / den;
    return -((-num + den / 2) / den);
}

int32_t LrCalculateHateDelta(int32_t base, const TrainerJourneyCore *core, const LrTrainerTraits *traits) {
    if (base <= 0) return base;

    int32_t mult = temperament_hate_gain(core->temperament) * trait_hate_gain(traits) / 1000;
    mult = mult * mind_hate_gain(core) / 1000;
    mult = clamp32(mult, 500, 1800);

    int32_t delta = round_div(base * mult, 1000);

    if (core->hate >= 80) {
        delta = (delta * (120 - (int32_t)core->hate) + 60) / 120;
        if (delta < 1) delta = 1;
    }
    return delta;
}

int32_t LrCalculateFearDelta(int32_t base, const TrainerJourneyCore *core, const LrTrainerTraits *traits) {
    if (base <= 0) return base;

    int32_t mult = temperament_fear_gain(core->temperament) * trait_fear_gain(traits) / 1000;
    mult = mult * mind_fear_gain(core) / 1000;
    mult = clamp32(mult, 500, 1800);

    return round_div(base * mult, 1000);
}

static void add_hate(TrainerJourneyCore *core, TrainerJourneyExtended *ext,
                     const LrTrainerTraits *traits, int32_t delta, uint16_t nowDay, uint8_t cause) {
    if (delta > 0) delta = LrCalculateHateDelta(delta, core, traits);

    int32_t next = clamp32((int32_t)core->hate + delta, 0, 100);
    core->hate = (uint8_t)next;

    int32_t magnitude = delta < 0 ? -delta : delta;
    if (magnitude >= 10) {
        uint8_t emotion = delta > 0 ? (uint8_t)LR_MEMORY_ANGER : (uint8_t)LR_MEMORY_JOY;
        uint8_t intensity = (uint8_t)(magnitude > 100 ? 100 : magnitude);
        LrRemember(core, ext, emotion, intensity, cause, nowDay, LR_FACET_ANGER);
    }
}

static void add_fear(TrainerJourneyCore *core, TrainerJourneyExtended *ext,
                     const LrTrainerTraits *traits, int32_t delta, uint16_t nowDay, uint8_t cause) {
    if (delta > 0) delta = LrCalculateFearDelta(delta, core, traits);

    int32_t next = clamp32((int32_t)core->fear + delta, 0, 100);
    core->fear = (uint8_t)next;

    int32_t magnitude = delta < 0 ? -delta : delta;
    if (magnitude >= 10) {
        uint8_t emotion = delta > 0 ? (uint8_t)LR_MEMORY_FEAR : (uint8_t)LR_MEMORY_JOY;
        uint8_t intensity = (uint8_t)(magnitude > 100 ? 100 : magnitude);
        LrRemember(core, ext, emotion, intensity, cause, nowDay, LR_FACET_BRAVERY);
    }
}

void LrAddHate(TrainerJourneyCore *core, TrainerJourneyExtended *ext,
               const LrTrainerTraits *traits, int32_t delta, uint16_t nowDay) {
    add_hate(core, ext, traits, delta, nowDay, LR_CAUSE_PLAYER);
}

void LrAddFear(TrainerJourneyCore *core, TrainerJourneyExtended *ext,
               const LrTrainerTraits *traits, int32_t delta, uint16_t nowDay) {
    add_fear(core, ext, traits, delta, nowDay, LR_CAUSE_PLAYER);
}

void LrAddHateCaused(TrainerJourneyCore *core, TrainerJourneyExtended *ext,
                     const LrTrainerTraits *traits, int32_t delta, uint16_t nowDay, uint8_t cause) {
    if (!core || !ext || delta < -100 || delta > 100 || cause > LR_CAUSE_OTHER_TRAINER) return;
    add_hate(core, ext, traits, delta, nowDay, cause);
}

void LrAddFearCaused(TrainerJourneyCore *core, TrainerJourneyExtended *ext,
                     const LrTrainerTraits *traits, int32_t delta, uint16_t nowDay, uint8_t cause) {
    if (!core || !ext || delta < -100 || delta > 100 || cause > LR_CAUSE_OTHER_TRAINER) return;
    add_fear(core, ext, traits, delta, nowDay, cause);
}

void LrAddNotoriety(TrainerJourneyCore *core, int32_t delta) {
    core->notoriety = (uint8_t)clamp32((int32_t)core->notoriety + delta, 0, 100);
}

int32_t LrEffectiveFear(const TrainerJourneyCore *core) {
    return (int32_t)core->fear + (int32_t)core->notoriety / 4;
}
