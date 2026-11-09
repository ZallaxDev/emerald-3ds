#include "lr_mind.h"
#include "lr_facet_tables.h"
#include "lr_rng.h"

static int clamp100(int v) {
    if (v < 0) return 0;
    if (v > 100) return 100;
    return v;
}

static uint8_t safe_temperament(uint8_t t) {
    return t < LR_TEMPERAMENT_COUNT ? t : (uint8_t)LR_TEMP_STEADY;
}

static uint8_t safe_archetype(uint8_t a) {
    return a < LR_ARCHETYPE_COUNT ? a : (uint8_t)LR_ARCH_GRINDER;
}

static int safe_region_count(uint8_t count) {
    return count > LR_MAX_REGION_FACETS ? LR_MAX_REGION_FACETS : (int)count;
}

static int safe_faction_count(uint8_t count) {
    return count > LR_MAX_FACTION_FACETS ? LR_MAX_FACTION_FACETS : (int)count;
}

void LrRollMind(TrainerJourneyCore *core, TrainerJourneyExtended *ext,
                const LrTrainerTraits *traits, uint32_t seed) {
    LrRng rng;
    LrRngSeed(&rng, seed);

    const uint8_t temp = safe_temperament(core->temperament);
    const uint8_t arch = safe_archetype(core->archetype);

    const int8_t *u = LrUniversalTemperamentOffsets(temp);
    int v[LR_UNIVERSAL_FACET_COUNT];
    for (int i = 0; i < LR_UNIVERSAL_FACET_COUNT; i++) v[i] = 50 + u[i];

    const LrRegionProfile *rp = LrGetRegionProfile(core->homeRegion);
    core->regionFacetCount = rp->facetCount;
    int rv[LR_MAX_REGION_FACETS];
    for (int i = 0; i < LR_MAX_REGION_FACETS; i++) {
        if (i < (int)core->regionFacetCount) {
            rv[i] = 50 + rp->temperamentOffsets[temp][i];
            core->regionFacetIds[i] = (uint8_t)i;
        } else {
            rv[i] = 0;
            core->regionFacetIds[i] = 0xFF;
        }
    }

    const LrFactionProfile *fp = LrGetFactionProfile(core->faction);
    core->factionFacetCount = fp->facetCount;
    int fv[LR_MAX_FACTION_FACETS];
    for (int i = 0; i < LR_MAX_FACTION_FACETS; i++) {
        if (i < (int)core->factionFacetCount) {
            fv[i] = 50 + fp->archetypeOffsets[arch][i];
            core->factionFacetIds[i] = (uint8_t)i;
        } else {
            fv[i] = 0;
            core->factionFacetIds[i] = 0xFF;
        }
    }

    if (traits && traits->traitsRolled) {
        int agg = (int)traits->aggression;
        v[LR_FACET_ANGER] += (agg - 50) / 2;
        v[LR_FACET_CRUELTY] += (agg - 50) / 3;
        if (traits->disposition == LR_DISPOSITION_FRIENDLY) {
            v[LR_FACET_SOCIAL] += 15;
            v[LR_FACET_CRUELTY] -= 10;
        }
        if (traits->disposition == LR_DISPOSITION_SURLY) {
            v[LR_FACET_SOCIAL] -= 15;
            v[LR_FACET_ANGER] += 5;
        }
        if (traits->smarts == LR_SMARTS_SHARP) v[LR_FACET_AMBITION] += 8;
        if (traits->wealth == LR_WEALTH_RICH) v[LR_FACET_AMBITION] += 10;

        if (traits->pastEvents & LR_PAST_CRIME) {
            v[LR_FACET_HONESTY] -= 8;
            v[LR_FACET_CRUELTY] += 4;
        }
        if (traits->pastEvents & LR_PAST_WAR) {
            v[LR_FACET_BRAVERY] += 6;
            v[LR_FACET_CRUELTY] += 4;
        }
        if (traits->pastEvents & LR_PAST_LOSS) {
            v[LR_FACET_SOCIAL] -= 4;
            v[LR_FACET_ANGER] += 3;
        }
        if (traits->pastEvents & LR_PAST_RANK) v[LR_FACET_AMBITION] += 6;
        if (traits->pastEvents & LR_PAST_GANG) v[LR_FACET_LOYALTY] += 6;

        v[LR_FACET_AMBITION] += (int)traits->rank * 3;
    }

    for (int i = 0; i < LR_UNIVERSAL_FACET_COUNT; i++)
        v[i] = clamp100(v[i] + LrRandomRange(&rng, -10, 11));
    for (int i = 0; i < (int)core->regionFacetCount; i++)
        rv[i] = clamp100(rv[i] + LrRandomRange(&rng, -10, 11));
    for (int i = 0; i < (int)core->factionFacetCount; i++)
        fv[i] = clamp100(fv[i] + LrRandomRange(&rng, -10, 11));

    for (int i = 0; i < LR_UNIVERSAL_FACET_COUNT; i++)
        core->universalFacets[i] = (int8_t)v[i];
    for (int i = 0; i < LR_MAX_REGION_FACETS; i++)
        ext->regionFacets[i] = (uint8_t)rv[i];
    for (int i = 0; i < LR_MAX_FACTION_FACETS; i++)
        ext->factionFacets[i] = (uint8_t)fv[i];

    LrDeriveValues(core, ext);

    for (int i = 0; i < LR_NEED_COUNT; i++)
        core->needs[i] = (uint8_t)LrRandomRange(&rng, 10, 45);
    core->stress = (uint8_t)LrRandomRange(&rng, 5, 25);

    core->goal1_type = LR_GOAL_NONE;
    core->goal2_type = LR_GOAL_NONE;
    core->goal1_target = LR_GOAL_TARGET_NONE;
    core->goal2_target = LR_GOAL_TARGET_NONE;

    core->mentorId = LR_NO_TRAINER;
    core->studentIds[0] = LR_NO_TRAINER;
    core->studentIds[1] = LR_NO_TRAINER;

    core->mindRolled = 1;
}

int32_t LrComputeBaseline(const TrainerJourneyCore *core, uint16_t globalFacetIndex) {
    if (!core->mindRolled) return 50;

    const uint8_t temp = safe_temperament(core->temperament);
    const uint8_t arch = safe_archetype(core->archetype);
    const int regionCount = safe_region_count(core->regionFacetCount);
    const int factionCount = safe_faction_count(core->factionFacetCount);

    int idx = (int)globalFacetIndex;
    if (idx < LR_UNIVERSAL_FACET_COUNT)
        return 50 + LrUniversalTemperamentOffsets(temp)[idx];

    idx -= LR_UNIVERSAL_FACET_COUNT;
    if (idx < regionCount) {
        const LrRegionProfile *rp = LrGetRegionProfile(core->homeRegion);
        return 50 + rp->temperamentOffsets[temp][idx];
    }

    idx -= regionCount;
    if (idx < factionCount) {
        const LrFactionProfile *fp = LrGetFactionProfile(core->faction);
        return 50 + fp->archetypeOffsets[arch][idx];
    }

    return 50;
}

int32_t LrGetFacet(const TrainerJourneyCore *core, const TrainerJourneyExtended *ext,
                   uint16_t globalFacetIndex) {
    if (!core->mindRolled) return 50;

    const int regionCount = safe_region_count(core->regionFacetCount);
    const int factionCount = safe_faction_count(core->factionFacetCount);

    int idx = (int)globalFacetIndex;
    if (idx < LR_UNIVERSAL_FACET_COUNT) return core->universalFacets[idx];

    idx -= LR_UNIVERSAL_FACET_COUNT;
    if (idx < regionCount) return ext->regionFacets[idx];

    idx -= regionCount;
    if (idx < factionCount) return ext->factionFacets[idx];

    return 50;
}

void LrDeriveValues(TrainerJourneyCore *core, const TrainerJourneyExtended *ext) {
    const LrRegionProfile *rp = LrGetRegionProfile(core->homeRegion);
    const int regionCount = safe_region_count(core->regionFacetCount);
    const int factionCount = safe_faction_count(core->factionFacetCount);

    int social = core->universalFacets[LR_FACET_SOCIAL];
    int loyalty = core->universalFacets[LR_FACET_LOYALTY];
    int honesty = core->universalFacets[LR_FACET_HONESTY];
    int disc = core->universalFacets[LR_FACET_DISCIPLINE];
    int ambition = core->universalFacets[LR_FACET_AMBITION];

    int family = social;
    if (rp->familyFacet >= 0 && (int)rp->familyFacet < regionCount)
        family = (social + ext->regionFacets[rp->familyFacet]) / 2;
    core->values[LR_VALUE_FAMILY] = (int8_t)((family - 50) / 16);

    int faction = loyalty;
    if (factionCount > 0) {
        int sum = 0;
        for (int i = 0; i < factionCount; i++) sum += ext->factionFacets[i];
        faction = (loyalty + sum / factionCount) / 2;
    }
    core->values[LR_VALUE_FACTION] = (int8_t)((faction - 50) / 16);

    int money = ambition;
    if (rp->moneyFacet >= 0 && (int)rp->moneyFacet < regionCount)
        money = (ambition + ext->regionFacets[rp->moneyFacet]) / 2;
    core->values[LR_VALUE_MONEY] = (int8_t)((money - 50) / 16);

    core->values[LR_VALUE_HONOUR] = (int8_t)(((honesty + disc) / 2 - 50) / 16);

    int law = honesty;
    if (rp->lawFacet >= 0 && (int)rp->lawFacet < regionCount)
        law = (honesty + ext->regionFacets[rp->lawFacet]) / 2;
    core->values[LR_VALUE_LAW] = (int8_t)((law - 50) / 16);

    int spirit = 0;
    if (rp->spiritFacet >= 0 && (int)rp->spiritFacet < regionCount)
        spirit = (ext->regionFacets[rp->spiritFacet] - 50) / 16;
    core->values[LR_VALUE_SPIRIT] = (int8_t)spirit;

    for (int i = 0; i < LR_VALUE_COUNT; i++) {
        if (core->values[i] < -3) core->values[i] = -3;
        if (core->values[i] > 3) core->values[i] = 3;
    }
}
