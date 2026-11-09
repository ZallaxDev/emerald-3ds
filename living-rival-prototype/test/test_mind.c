#include <string.h>
#include "lr_test.h"
#include "lr_types.h"
#include "lr_mind.h"

static void make_core(TrainerJourneyCore *c, TrainerJourneyExtended *e,
                      uint8_t temp, uint8_t region, uint8_t faction) {
    memset(c, 0, sizeof(*c));
    memset(e, 0, sizeof(*e));
    c->trainerId = 1;
    c->temperament = temp;
    c->archetype = LR_ARCH_GRINDER;
    c->faction = faction;
    c->homeRegion = region;
    c->personalSeed = 4242;
    c->mindRolled = 1;   /* hand-built cores are queryable; never-rolled is memset directly */
}

int main(void) {
    TrainerJourneyCore a, b;
    TrainerJourneyExtended ea, eb;

    /* Determinism. */
    make_core(&a, &ea, LR_TEMP_HOTHEAD, LR_REGION_HOENN, LR_FACTION_GYM_CIRCUIT);
    make_core(&b, &eb, LR_TEMP_HOTHEAD, LR_REGION_HOENN, LR_FACTION_GYM_CIRCUIT);
    LrRollMind(&a, &ea, 0, 777);
    LrRollMind(&b, &eb, 0, 777);
    LR_CHECK(memcmp(&a, &b, sizeof(a)) == 0);
    LR_CHECK(memcmp(&ea, &eb, sizeof(ea)) == 0);
    LR_CHECK_EQ_INT(a.mindRolled, 1);

    /* A different seed changes at least one facet. */
    make_core(&b, &eb, LR_TEMP_HOTHEAD, LR_REGION_HOENN, LR_FACTION_GYM_CIRCUIT);
    LrRollMind(&b, &eb, 0, 778);
    LR_CHECK(memcmp(a.universalFacets, b.universalFacets, sizeof(a.universalFacets)) != 0);

    /* Ranges and structural invariants over many seeds/temperaments. */
    for (int t = 0; t < LR_TEMPERAMENT_COUNT; t++) {
        for (uint32_t s = 0; s < 200; s++) {
            make_core(&a, &ea, (uint8_t)t, LR_REGION_SINNOH, LR_FACTION_TEAM_MAGMA);
            LrRollMind(&a, &ea, 0, s);
            LR_CHECK_EQ_INT(a.regionFacetCount, 3);
            LR_CHECK_EQ_INT(a.factionFacetCount, 1);
            for (int i = 0; i < LR_UNIVERSAL_FACET_COUNT; i++)
                LR_CHECK(a.universalFacets[i] >= 0 && a.universalFacets[i] <= 100);
            for (int i = 0; i < a.regionFacetCount; i++)
                LR_CHECK(ea.regionFacets[i] >= 0 && ea.regionFacets[i] <= 100);
            for (int i = 0; i < a.factionFacetCount; i++)
                LR_CHECK(ea.factionFacets[i] >= 0 && ea.factionFacets[i] <= 100);
            for (int i = 0; i < LR_VALUE_COUNT; i++)
                LR_CHECK(a.values[i] >= -3 && a.values[i] <= 3);
            for (int i = 0; i < LR_NEED_COUNT; i++)
                LR_CHECK(a.needs[i] >= 10 && a.needs[i] <= 44);
            LR_CHECK(a.stress >= 5 && a.stress <= 24);
            LR_CHECK_EQ_INT(a.goal1_type, LR_GOAL_NONE);
            LR_CHECK_EQ_INT(a.goal2_type, LR_GOAL_NONE);
        }
    }

    /* Baseline matches the tables exactly. */
    make_core(&a, &ea, LR_TEMP_HOTHEAD, LR_REGION_KANTO, LR_FACTION_INDEPENDENT);
    LR_CHECK_EQ_INT(LrComputeBaseline(&a, LR_FACET_ANGER), 80);
    LR_CHECK_EQ_INT(LrComputeBaseline(&a, LR_FACET_BRAVERY), 60);
    make_core(&a, &ea, LR_TEMP_COWARD, LR_REGION_KANTO, LR_FACTION_INDEPENDENT);
    LR_CHECK_EQ_INT(LrComputeBaseline(&a, LR_FACET_BRAVERY), 15);
    LR_CHECK_EQ_INT(LrComputeBaseline(&a, LR_FACET_HONESTY), 45);
    make_core(&a, &ea, LR_TEMP_DRIFTER, LR_REGION_KANTO, LR_FACTION_INDEPENDENT);
    LR_CHECK_EQ_INT(LrComputeBaseline(&a, LR_FACET_AMBITION), 25);
    LR_CHECK_EQ_INT(LrComputeBaseline(&a, LR_FACET_LOYALTY), 35);
    make_core(&a, &ea, LR_TEMP_LOYAL, LR_REGION_KANTO, LR_FACTION_INDEPENDENT);
    LR_CHECK_EQ_INT(LrComputeBaseline(&a, LR_FACET_LOYALTY), 85);

    /* STEADY with no traits/rank: every universal facet within +-10 of 50. */
    make_core(&a, &ea, LR_TEMP_STEADY, LR_REGION_HOENN, LR_FACTION_INDEPENDENT);
    LrRollMind(&a, &ea, 0, 31);
    for (int i = 0; i < LR_UNIVERSAL_FACET_COUNT; i++)
        LR_CHECK(a.universalFacets[i] >= 40 && a.universalFacets[i] <= 60);

    /* Clamp at the top: Hothead Anger 80 + aggression 100 (+25) is pinned to 100. */
    LrTrainerTraits traits;
    memset(&traits, 0, sizeof(traits));
    traits.aggression = 100;
    traits.traitsRolled = 1;
    make_core(&a, &ea, LR_TEMP_HOTHEAD, LR_REGION_KANTO, LR_FACTION_INDEPENDENT);
    LrRollMind(&a, &ea, &traits, 5);
    LR_CHECK_EQ_INT(a.universalFacets[LR_FACET_ANGER], 100);

    /* Signed trait math: aggression below 50 must lower Anger, not wrap. */
    traits.aggression = 10;
    make_core(&a, &ea, LR_TEMP_STEADY, LR_REGION_KANTO, LR_FACTION_INDEPENDENT);
    LrRollMind(&a, &ea, &traits, 5);
    LR_CHECK(a.universalFacets[LR_FACET_ANGER] <= 40);  /* 50 - 20 + jitter */

    /* rank adds rank*3 to Ambition. */
    memset(&traits, 0, sizeof(traits));
    traits.traitsRolled = 1;
    traits.rank = 3;
    make_core(&a, &ea, LR_TEMP_STEADY, LR_REGION_KANTO, LR_FACTION_INDEPENDENT);
    LrRollMind(&a, &ea, &traits, 5);
    LR_CHECK(a.universalFacets[LR_FACET_AMBITION] >= 49);  /* 50 + 9 - 10 */

    /* Global facet mapping, including a short profile (Hoenn = 3 region facets). */
    make_core(&a, &ea, LR_TEMP_STEADY, LR_REGION_HOENN, LR_FACTION_GYM_CIRCUIT);
    LrRollMind(&a, &ea, 0, 3);
    LR_CHECK_EQ_INT(LrGetFacet(&a, &ea, 0), a.universalFacets[0]);
    LR_CHECK_EQ_INT(LrGetFacet(&a, &ea, 7), a.universalFacets[7]);
    LR_CHECK_EQ_INT(LrGetFacet(&a, &ea, 8), ea.regionFacets[0]);
    LR_CHECK_EQ_INT(LrGetFacet(&a, &ea, 10), ea.regionFacets[2]);
    LR_CHECK_EQ_INT(LrGetFacet(&a, &ea, 11), ea.factionFacets[0]);
    LR_CHECK_EQ_INT(LrGetFacet(&a, &ea, 12), 50);   /* past the profile */
    LR_CHECK_EQ_INT(LrComputeBaseline(&a, 12), 50);

    /* Never-rolled trainer: neutral, no uninitialized reads. */
    memset(&a, 0, sizeof(a));
    memset(&ea, 0, sizeof(ea));
    LR_CHECK_EQ_INT(LrGetFacet(&a, &ea, 0), 50);
    LR_CHECK_EQ_INT(LrGetFacet(&a, &ea, 8), 50);
    LR_CHECK_EQ_INT(LrComputeBaseline(&a, 8), 50);
    LR_CHECK_EQ_INT(LrComputeBaseline(&a, 0), 50);

    /* Values formulas are exact on hand-built facets. */
    memset(&a, 0, sizeof(a));
    memset(&ea, 0, sizeof(ea));
    a.universalFacets[LR_FACET_HONESTY] = 100;
    a.universalFacets[LR_FACET_DISCIPLINE] = 100;
    LrDeriveValues(&a, &ea);
    LR_CHECK_EQ_INT(a.values[LR_VALUE_HONOUR], 3);
    a.universalFacets[LR_FACET_HONESTY] = 0;
    a.universalFacets[LR_FACET_DISCIPLINE] = 0;
    LrDeriveValues(&a, &ea);
    LR_CHECK_EQ_INT(a.values[LR_VALUE_HONOUR], -3);

    a.universalFacets[LR_FACET_SOCIAL] = 50;
    a.homeRegion = LR_REGION_KANTO;   /* no family facet -> Social alone */
    LrDeriveValues(&a, &ea);
    LR_CHECK_EQ_INT(a.values[LR_VALUE_FAMILY], 0);
    a.universalFacets[LR_FACET_SOCIAL] = 0;
    LrDeriveValues(&a, &ea);
    LR_CHECK_EQ_INT(a.values[LR_VALUE_FAMILY], -3);

    /* Spirit comes from the region facet when present; 0 when absent. */
    a.homeRegion = LR_REGION_HOENN;   /* spirit = facet 2 */
    a.regionFacetCount = 3;
    ea.regionFacets[2] = 100;
    LrDeriveValues(&a, &ea);
    LR_CHECK_EQ_INT(a.values[LR_VALUE_SPIRIT], 3);
    ea.regionFacets[2] = 0;
    LrDeriveValues(&a, &ea);
    LR_CHECK_EQ_INT(a.values[LR_VALUE_SPIRIT], -3);
    a.homeRegion = LR_REGION_KANTO;   /* no spirit facet */
    a.regionFacetCount = 3;
    LrDeriveValues(&a, &ea);
    LR_CHECK_EQ_INT(a.values[LR_VALUE_SPIRIT], 0);

    return lr_test_summary("test_mind");
}
