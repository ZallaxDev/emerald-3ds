#include <string.h>
#include "lr_test.h"
#include "lr_types.h"
#include "lr_memory.h"
#include "lr_goals.h"
#include "lr_facet_tables.h"

static void base(TrainerJourneyCore *c, TrainerJourneyExtended *e, uint8_t home) {
    memset(c, 0, sizeof(*c));
    memset(e, 0, sizeof(*e));
    c->mindRolled = 1;
    c->temperament = LR_TEMP_STEADY;
    c->archetype = LR_ARCH_GRINDER;
    c->homeRegion = home;
    c->faction = LR_FACTION_INDEPENDENT;
    c->regionFacetCount = LrRegionFacetCount(home);
    c->factionFacetCount = LrFactionFacetCount(LR_FACTION_INDEPENDENT);
    for (int i = 0; i < LR_UNIVERSAL_FACET_COUNT; i++) c->universalFacets[i] = 50;
}

int main(void) {
    TrainerJourneyCore c;
    TrainerJourneyExtended e;

    /* Emotion classification and strength. */
    LR_CHECK(LrIsBadEmotion(LR_MEMORY_ANGER));
    LR_CHECK(LrIsBadEmotion(LR_MEMORY_FEAR));
    LR_CHECK(LrIsBadEmotion(LR_MEMORY_SHAME));
    LR_CHECK(LrIsBadEmotion(LR_MEMORY_GRIEF));
    LR_CHECK(!LrIsBadEmotion(LR_MEMORY_JOY));
    LR_CHECK(!LrIsBadEmotion(LR_MEMORY_PRIDE));
    LR_CHECK_EQ_INT(LrMemoryStrength(50, false), 50);
    LR_CHECK_EQ_INT(LrMemoryStrength(50, true), 100);

    /* Add, stress, and field storage. */
    base(&c, &e, LR_REGION_KANTO);
    LR_CHECK(!LrRemember(&c, &e, LR_MEMORY_ANGER, 30, LR_CAUSE_PLAYER, 10, LR_FACET_ANGER));
    LR_CHECK_EQ_INT(e.memoryCount, 1);
    LR_CHECK_EQ_INT(e.memoryEmotions[0], LR_MEMORY_ANGER);
    LR_CHECK_EQ_INT(e.memoryIntensities[0], 30);
    LR_CHECK_EQ_INT(e.memoryCauses[0], LR_CAUSE_PLAYER);
    LR_CHECK_EQ_INT(e.memoryTimestamps[0], 10);
    LR_CHECK_EQ_INT(e.memoryLongTerm[0], 0);
    LR_CHECK_EQ_INT(e.memoryTargetFacets[0], LR_FACET_ANGER);
    LR_CHECK_EQ_INT(c.stress, 3);   /* 30/8 */

    /* Reinforcement, not replacement. */
    LR_CHECK(!LrRemember(&c, &e, LR_MEMORY_ANGER, 20, LR_CAUSE_PLAYER, 20, LR_FACET_BRAVERY));
    LR_CHECK_EQ_INT(e.memoryCount, 1);
    LR_CHECK_EQ_INT(e.memoryIntensities[0], 45);   /* 30 + 20/2 + 5 */
    LR_CHECK_EQ_INT(e.memoryTimestamps[0], 20);
    LR_CHECK_EQ_INT(e.memoryTargetFacets[0], LR_FACET_BRAVERY);
    LR_CHECK_EQ_INT(c.stress, 5);   /* 3 + 20/8 */

    /* Good memories reduce stress. */
    base(&c, &e, LR_REGION_KANTO);
    c.stress = 50;
    LrRemember(&c, &e, LR_MEMORY_JOY, 30, LR_CAUSE_WORLD, 1, LR_FACET_SOCIAL);
    LR_CHECK_EQ_INT(c.stress, 47);  /* 50 - 3 */

    /* Threshold is exactly 65. */
    base(&c, &e, LR_REGION_KANTO);
    LR_CHECK(!LrRemember(&c, &e, LR_MEMORY_ANGER, 64, LR_CAUSE_WORLD, 1, LR_FACET_ANGER));
    LR_CHECK_EQ_INT(e.memoryLongTerm[0], 0);
    LR_CHECK(LrRemember(&c, &e, LR_MEMORY_ANGER, 65, LR_CAUSE_WORLD, 2, LR_FACET_ANGER));
    LR_CHECK_EQ_INT(e.memoryLongTerm[0], 1);

    /* A reinforced memory drifts once, when it crosses 65. */
    base(&c, &e, LR_REGION_KANTO);
    LrRemember(&c, &e, LR_MEMORY_ANGER, 60, LR_CAUSE_WORLD, 1, LR_FACET_ANGER);
    LR_CHECK(LrRemember(&c, &e, LR_MEMORY_ANGER, 2, LR_CAUSE_WORLD, 2, LR_FACET_ANGER));
    LR_CHECK_EQ_INT(e.memoryIntensities[0], 66);
    int angerAfterFirstDrift = c.universalFacets[LR_FACET_ANGER];
    LrRemember(&c, &e, LR_MEMORY_ANGER, 2, LR_CAUSE_WORLD, 3, LR_FACET_ANGER);
    LR_CHECK_EQ_INT(e.memoryCount, 2);   /* long-term memory was not reinforced */
    LR_CHECK_EQ_INT(c.universalFacets[LR_FACET_ANGER], angerAfterFirstDrift);

    /* Drift: Anger moves the target at most +5 and adds Cruelty. */
    base(&c, &e, LR_REGION_KANTO);
    c.universalFacets[LR_FACET_ANGER] = 40;
    LrDrift(&c, &e, LR_MEMORY_ANGER, LR_CAUSE_WORLD, LR_FACET_ANGER);
    LR_CHECK_EQ_INT(c.universalFacets[LR_FACET_ANGER], 45);
    LR_CHECK_EQ_INT(c.universalFacets[LR_FACET_CRUELTY], 52);

    /* Drift: Fear lowers Bravery. */
    base(&c, &e, LR_REGION_KANTO);
    LrDrift(&c, &e, LR_MEMORY_FEAR, LR_CAUSE_WORLD, LR_FACET_BRAVERY);
    LR_CHECK_EQ_INT(c.universalFacets[LR_FACET_BRAVERY], 44);

    /* Drift: Shame below 55 pride lowers Bravery and pride. */
    base(&c, &e, LR_REGION_KANTO);
    e.regionFacets[0] = 50;   /* Kanto pride = Honor */
    LrDrift(&c, &e, LR_MEMORY_SHAME, LR_CAUSE_WORLD, LR_FACET_ANGER);
    LR_CHECK_EQ_INT(c.universalFacets[LR_FACET_BRAVERY], 46);
    LR_CHECK_EQ_INT(e.regionFacets[0], 47);

    /* Drift: Shame at 55+ pride raises pride and pushes Anger. */
    base(&c, &e, LR_REGION_KANTO);
    e.regionFacets[0] = 60;
    c.universalFacets[LR_FACET_ANGER] = 40;
    LrDrift(&c, &e, LR_MEMORY_SHAME, LR_CAUSE_WORLD, LR_FACET_ANGER);
    LR_CHECK_EQ_INT(e.regionFacets[0], 63);
    LR_CHECK_EQ_INT(c.universalFacets[LR_FACET_ANGER], 44);   /* clamp(50+25-40, 0, 4) */

    /* Drift: Grief, Joy, Pride. */
    base(&c, &e, LR_REGION_KANTO);
    c.universalFacets[LR_FACET_ANGER] = 40;
    LrDrift(&c, &e, LR_MEMORY_GRIEF, LR_CAUSE_WORLD, LR_FACET_ANGER);
    LR_CHECK_EQ_INT(c.universalFacets[LR_FACET_ANGER], 43);
    LR_CHECK_EQ_INT(c.universalFacets[LR_FACET_SOCIAL], 45);
    LR_CHECK_EQ_INT(c.universalFacets[LR_FACET_LOYALTY], 52);

    base(&c, &e, LR_REGION_KANTO);
    c.universalFacets[LR_FACET_ANGER] = 40;
    LrDrift(&c, &e, LR_MEMORY_JOY, LR_CAUSE_WORLD, LR_FACET_ANGER);
    LR_CHECK_EQ_INT(c.universalFacets[LR_FACET_SOCIAL], 54);
    LR_CHECK_EQ_INT(c.universalFacets[LR_FACET_ANGER], 37);

    base(&c, &e, LR_REGION_KANTO);
    e.regionFacets[0] = 60;
    LrDrift(&c, &e, LR_MEMORY_PRIDE, LR_CAUSE_WORLD, LR_FACET_ANGER);
    LR_CHECK_EQ_INT(e.regionFacets[0], 63);
    LR_CHECK_EQ_INT(c.universalFacets[LR_FACET_BRAVERY], 53);

    /* Goal install: player-caused anger at >= 50 anger installs Avenge. */
    base(&c, &e, LR_REGION_KANTO);
    c.universalFacets[LR_FACET_ANGER] = 50;
    LrDrift(&c, &e, LR_MEMORY_ANGER, LR_CAUSE_PLAYER, LR_FACET_ANGER);
    LR_CHECK(LrHasGoal(&c, LR_GOAL_AVENGE));
    LR_CHECK(LrHasGoalAgainstPlayer(&c, LR_GOAL_AVENGE));

    base(&c, &e, LR_REGION_KANTO);
    c.universalFacets[LR_FACET_ANGER] = 40;   /* drift makes it 45, below 50 */
    LrDrift(&c, &e, LR_MEMORY_ANGER, LR_CAUSE_PLAYER, LR_FACET_ANGER);
    LR_CHECK(!LrHasGoal(&c, LR_GOAL_AVENGE));

    base(&c, &e, LR_REGION_KANTO);
    c.universalFacets[LR_FACET_ANGER] = 90;
    c.faction = LR_FACTION_ELITE_FOUR_PATH;
    LrDrift(&c, &e, LR_MEMORY_ANGER, LR_CAUSE_PLAYER, LR_FACET_ANGER);
    LR_CHECK(!LrHasGoal(&c, LR_GOAL_AVENGE));

    /* Goal install: player-caused fear below 35 bravery installs Settle. */
    base(&c, &e, LR_REGION_KANTO);
    c.universalFacets[LR_FACET_BRAVERY] = 34;
    LrDrift(&c, &e, LR_MEMORY_FEAR, LR_CAUSE_PLAYER, LR_FACET_BRAVERY);
    LR_CHECK(LrHasGoal(&c, LR_GOAL_SETTLE));

    base(&c, &e, LR_REGION_KANTO);
    c.universalFacets[LR_FACET_BRAVERY] = 40;   /* drift makes it 34, still below 35 -> installs */
    LrDrift(&c, &e, LR_MEMORY_FEAR, LR_CAUSE_PLAYER, LR_FACET_BRAVERY);
    LR_CHECK(LrHasGoal(&c, LR_GOAL_SETTLE));

    base(&c, &e, LR_REGION_KANTO);
    c.universalFacets[LR_FACET_BRAVERY] = 41;   /* drift makes it 35 -> no install */
    LrDrift(&c, &e, LR_MEMORY_FEAR, LR_CAUSE_PLAYER, LR_FACET_BRAVERY);
    LR_CHECK(!LrHasGoal(&c, LR_GOAL_SETTLE));

    /* Trim: at 12, a weaker memory is dropped; a stronger one replaces the weakest. */
    base(&c, &e, LR_REGION_KANTO);
    for (int i = 0; i < LR_MAX_MEMORIES; i++) {
        uint8_t emotion = (uint8_t)(i % LR_MEMORY_EMOTION_COUNT);
        uint8_t cause = (uint8_t)(i / LR_MEMORY_EMOTION_COUNT);
        LrRemember(&c, &e, emotion, (uint8_t)(20 + i * 3), cause, (uint16_t)i, LR_FACET_ANGER);
    }
    LR_CHECK_EQ_INT(e.memoryCount, LR_MAX_MEMORIES);
    LR_CHECK(!LrRemember(&c, &e, LR_MEMORY_PRIDE, 5, LR_CAUSE_OTHER_TRAINER, 99, LR_FACET_ANGER));
    LR_CHECK_EQ_INT(e.memoryCount, LR_MAX_MEMORIES);
    LR_CHECK(LrRemember(&c, &e, LR_MEMORY_PRIDE, 66, LR_CAUSE_OTHER_TRAINER, 100, LR_FACET_ANGER));
    LR_CHECK_EQ_INT(e.memoryCount, LR_MAX_MEMORIES);
    int has20 = 0, has66 = 0;
    for (int i = 0; i < e.memoryCount; i++) {
        if (e.memoryIntensities[i] == 20) has20 = 1;
        if (e.memoryIntensities[i] == 66 && e.memoryEmotions[i] == LR_MEMORY_PRIDE) has66 = 1;
    }
    LR_CHECK(!has20);
    LR_CHECK(has66);

    return lr_test_summary("test_memory");
}
