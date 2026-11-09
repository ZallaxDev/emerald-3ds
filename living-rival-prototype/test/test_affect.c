#include <string.h>
#include "lr_test.h"
#include "lr_types.h"
#include "lr_affect.h"
#include "lr_mind.h"
#include "lr_memory.h"

/* Put every universal facet exactly on its temperament baseline, so only the
 * multiplier under test is active. */
static void base(TrainerJourneyCore *c, TrainerJourneyExtended *e, uint8_t temp) {
    memset(c, 0, sizeof(*c));
    memset(e, 0, sizeof(*e));
    c->mindRolled = 1;
    c->temperament = temp;
    c->archetype = LR_ARCH_GRINDER;
    c->homeRegion = LR_REGION_KANTO;
    c->faction = LR_FACTION_INDEPENDENT;
    for (int i = 0; i < LR_UNIVERSAL_FACET_COUNT; i++)
        c->universalFacets[i] = (int8_t)LrComputeBaseline(c, (uint16_t)i);
}

int main(void) {
    TrainerJourneyCore c;
    TrainerJourneyExtended e;
    LrTrainerTraits t;

    /* Temperament multipliers in isolation. */
    base(&c, &e, LR_TEMP_HOTHEAD);
    LR_CHECK_EQ_INT(LrCalculateHateDelta(40, &c, 0), 52);   /* 1.30 */
    base(&c, &e, LR_TEMP_COWARD);
    LR_CHECK_EQ_INT(LrCalculateFearDelta(40, &c, 0), 72);   /* 2.00 clamped to 1.80 */
    base(&c, &e, LR_TEMP_STEADY);
    LR_CHECK_EQ_INT(LrCalculateHateDelta(40, &c, 0), 40);
    LR_CHECK_EQ_INT(LrCalculateFearDelta(40, &c, 0), 40);

    /* Trait gains. */
    memset(&t, 0, sizeof(t));
    t.traitsRolled = 1;
    t.aggression = 100;
    base(&c, &e, LR_TEMP_STEADY);
    LR_CHECK_EQ_INT(LrCalculateHateDelta(40, &c, &t), 48);   /* 1.20 */
    LR_CHECK_EQ_INT(LrCalculateFearDelta(40, &c, &t), 36);   /* 0.90 */
    t.aggression = 50;
    t.disposition = LR_DISPOSITION_FRIENDLY;
    LR_CHECK_EQ_INT(LrCalculateHateDelta(40, &c, &t), 36);   /* 0.90 */
    t.disposition = LR_DISPOSITION_SURLY;
    LR_CHECK_EQ_INT(LrCalculateHateDelta(40, &c, &t), 44);   /* 1.10 */

    /* Mind multiplier: +/-20% hate, +/-30% fear, clamped. */
    base(&c, &e, LR_TEMP_STEADY);
    c.universalFacets[LR_FACET_ANGER] = 70;
    LR_CHECK_EQ_INT(LrCalculateHateDelta(40, &c, 0), 48);    /* 1.20 */
    c.universalFacets[LR_FACET_ANGER] = 100;
    LR_CHECK_EQ_INT(LrCalculateHateDelta(40, &c, 0), 48);    /* clamped at +20 */
    c.universalFacets[LR_FACET_ANGER] = 30;
    LR_CHECK_EQ_INT(LrCalculateHateDelta(40, &c, 0), 32);    /* 0.80 */

    base(&c, &e, LR_TEMP_STEADY);
    c.universalFacets[LR_FACET_BRAVERY] = 80;
    LR_CHECK_EQ_INT(LrCalculateFearDelta(40, &c, 0), 28);    /* 0.70 */
    c.universalFacets[LR_FACET_BRAVERY] = 20;
    LR_CHECK_EQ_INT(LrCalculateFearDelta(40, &c, 0), 52);    /* 1.30 */

    /* Combined multiplier clamps to [0.5, 1.8]. */
    t.aggression = 100;
    t.disposition = LR_DISPOSITION_NEUTRAL;
    base(&c, &e, LR_TEMP_HOTHEAD);
    c.universalFacets[LR_FACET_ANGER] = 100;   /* Hothead baseline 80 + 20 */
    LR_CHECK_EQ_INT(LrCalculateHateDelta(40, &c, &t), 72);   /* 1.872 -> 1.8 */

    t.aggression = 0;
    t.disposition = LR_DISPOSITION_FRIENDLY;
    base(&c, &e, LR_TEMP_COWARD);
    c.universalFacets[LR_FACET_ANGER] = 20;   /* Coward baseline 40 - 20 */
    LR_CHECK_EQ_INT(LrCalculateHateDelta(40, &c, &t), 20);   /* 0.336 -> 0.5 */

    /* Diminishing returns above hate 80. */
    base(&c, &e, LR_TEMP_STEADY);
    c.hate = 79;
    LR_CHECK_EQ_INT(LrCalculateHateDelta(40, &c, 0), 40);
    c.hate = 80;
    LR_CHECK_EQ_INT(LrCalculateHateDelta(40, &c, 0), 13);
    c.hate = 100;
    LR_CHECK_EQ_INT(LrCalculateHateDelta(40, &c, 0), 7);
    LR_CHECK_EQ_INT(LrCalculateHateDelta(1, &c, 0), 1);      /* never below 1 */

    /* Negative deltas bypass multiplication. */
    base(&c, &e, LR_TEMP_HOTHEAD);
    LR_CHECK_EQ_INT(LrCalculateHateDelta(-15, &c, 0), -15);

    /* AddHate applies, clamps, and creates a memory for significant deltas. */
    base(&c, &e, LR_TEMP_STEADY);
    LrAddHate(&c, &e, 0, 40, 123);
    LR_CHECK_EQ_INT(c.hate, 40);
    LR_CHECK_EQ_INT(e.memoryCount, 1);
    LR_CHECK_EQ_INT(e.memoryEmotions[0], LR_MEMORY_ANGER);
    LR_CHECK_EQ_INT(e.memoryIntensities[0], 40);
    LR_CHECK_EQ_INT(e.memoryCauses[0], LR_CAUSE_PLAYER);
    LR_CHECK_EQ_INT(e.memoryTimestamps[0], 123);
    LR_CHECK_EQ_INT(e.memoryTargetFacets[0], LR_FACET_ANGER);
    LR_CHECK_EQ_INT(c.stress, 5);

    base(&c, &e, LR_TEMP_STEADY);
    LrAddHate(&c, &e, 0, 9, 1);
    LR_CHECK_EQ_INT(c.hate, 9);
    LR_CHECK_EQ_INT(e.memoryCount, 0);

    /* Exactly 10 creates a memory. */
    base(&c, &e, LR_TEMP_STEADY);
    LrAddHate(&c, &e, 0, 10, 1);
    LR_CHECK_EQ_INT(e.memoryCount, 1);

    /* A multiplied positive delta feeds the memory intensity. */
    base(&c, &e, LR_TEMP_HOTHEAD);
    LrAddHate(&c, &e, 0, 40, 1);
    LR_CHECK_EQ_INT(c.hate, 52);
    LR_CHECK_EQ_INT(e.memoryIntensities[0], 52);

    base(&c, &e, LR_TEMP_STEADY);
    c.hate = 79;
    LrAddHate(&c, &e, 0, 30, 1);
    LR_CHECK_EQ_INT(c.hate, 100);
    LR_CHECK_EQ_INT(e.memoryCount, 1);
    LR_CHECK_EQ_INT(e.memoryIntensities[0], 30);   /* pre-clamp delta */

    base(&c, &e, LR_TEMP_STEADY);
    c.hate = 10;
    LrAddHate(&c, &e, 0, -20, 1);
    LR_CHECK_EQ_INT(c.hate, 0);
    LR_CHECK_EQ_INT(e.memoryCount, 1);
    LR_CHECK_EQ_INT(e.memoryEmotions[0], LR_MEMORY_JOY);

    /* AddFear has the same shape, with Fear/Bravery. */
    base(&c, &e, LR_TEMP_COWARD);
    LrAddFear(&c, &e, 0, 40, 7);
    LR_CHECK_EQ_INT(c.fear, 72);   /* 2.00 clamped to 1.80 */
    LR_CHECK_EQ_INT(e.memoryCount, 1);
    LR_CHECK_EQ_INT(e.memoryEmotions[0], LR_MEMORY_FEAR);
    LR_CHECK_EQ_INT(e.memoryTargetFacets[0], LR_FACET_BRAVERY);

    base(&c, &e, LR_TEMP_STEADY);
    c.fear = 10;
    LrAddFear(&c, &e, 0, -20, 1);
    LR_CHECK_EQ_INT(c.fear, 0);
    LR_CHECK_EQ_INT(e.memoryCount, 1);
    LR_CHECK_EQ_INT(e.memoryEmotions[0], LR_MEMORY_JOY);

    /* Notoriety and effective fear. */
    base(&c, &e, LR_TEMP_STEADY);
    c.notoriety = 50;
    LrAddNotoriety(&c, 60);
    LR_CHECK_EQ_INT(c.notoriety, 100);
    LrAddNotoriety(&c, -110);
    LR_CHECK_EQ_INT(c.notoriety, 0);

    c.fear = 40;
    c.notoriety = 20;
    LR_CHECK_EQ_INT(LrEffectiveFear(&c), 45);
    c.notoriety = 100;
    LR_CHECK_EQ_INT(LrEffectiveFear(&c), 65);

    return lr_test_summary("test_affect");
}
