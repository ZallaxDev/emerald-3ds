#include <string.h>
#include "lr_test.h"
#include "lr_types.h"
#include "lr_goals.h"

static void base(TrainerJourneyCore *c) {
    memset(c, 0, sizeof(*c));
    c->mindRolled = 1;
    c->temperament = LR_TEMP_STEADY;
    c->homeRegion = LR_REGION_KANTO;
    c->faction = LR_FACTION_INDEPENDENT;
    for (int i = 0; i < 8; i++) c->universalFacets[i] = 50;
}

int main(void) {
    /* Hate tiers. */
    LR_CHECK_EQ_INT(LrHateTier(0), 0);
    LR_CHECK_EQ_INT(LrHateTier(39), 0);
    LR_CHECK_EQ_INT(LrHateTier(40), 1);
    LR_CHECK_EQ_INT(LrHateTier(69), 1);
    LR_CHECK_EQ_INT(LrHateTier(70), 2);
    LR_CHECK_EQ_INT(LrHateTier(100), 2);

    TrainerJourneyCore c;

    /* New goals push to the front; the third drops the oldest. */
    base(&c);
    LR_CHECK(LrSetGoal(&c, LR_GOAL_RISE, LR_GOAL_TARGET_NONE));
    LR_CHECK_EQ_INT(c.goal1_type, LR_GOAL_RISE);
    LR_CHECK_EQ_INT(c.goal2_type, LR_GOAL_NONE);
    LR_CHECK(LrSetGoal(&c, LR_GOAL_PROVE, LR_GOAL_TARGET_NONE));
    LR_CHECK_EQ_INT(c.goal1_type, LR_GOAL_PROVE);
    LR_CHECK_EQ_INT(c.goal2_type, LR_GOAL_RISE);
    LR_CHECK(LrSetGoal(&c, LR_GOAL_RICH, 5));
    LR_CHECK_EQ_INT(c.goal1_type, LR_GOAL_RICH);
    LR_CHECK_EQ_INT(c.goal1_target, 5);
    LR_CHECK_EQ_INT(c.goal2_type, LR_GOAL_PROVE);

    /* Re-setting a goal moves it to the front without duplicating. */
    base(&c);
    LrSetGoal(&c, LR_GOAL_RISE, LR_GOAL_TARGET_NONE);
    LrSetGoal(&c, LR_GOAL_PROVE, LR_GOAL_TARGET_NONE);
    LR_CHECK(LrSetGoal(&c, LR_GOAL_RISE, LR_GOAL_TARGET_NONE));
    LR_CHECK_EQ_INT(c.goal1_type, LR_GOAL_RISE);
    LR_CHECK_EQ_INT(c.goal2_type, LR_GOAL_PROVE);

    /* NONE is rejected and changes nothing. */
    base(&c);
    LrSetGoal(&c, LR_GOAL_RISE, LR_GOAL_TARGET_NONE);
    LR_CHECK(!LrSetGoal(&c, LR_GOAL_NONE, LR_GOAL_TARGET_NONE));
    LR_CHECK_EQ_INT(c.goal1_type, LR_GOAL_RISE);

    /* Avenge removes Settle. */
    base(&c);
    LrSetGoal(&c, LR_GOAL_SETTLE, LR_GOAL_TARGET_NONE);
    LR_CHECK(LrSetGoal(&c, LR_GOAL_AVENGE, LR_TRAINER_PLAYER));
    LR_CHECK_EQ_INT(c.goal1_type, LR_GOAL_AVENGE);
    LR_CHECK_EQ_INT(c.goal1_target, LR_TRAINER_PLAYER);
    LR_CHECK_EQ_INT(c.goal2_type, LR_GOAL_NONE);

    /* Settle removes Prove and Rise; Avenge survives. */
    base(&c);
    LrSetGoal(&c, LR_GOAL_PROVE, LR_GOAL_TARGET_NONE);
    LrSetGoal(&c, LR_GOAL_RISE, LR_GOAL_TARGET_NONE);
    LrSetGoal(&c, LR_GOAL_SETTLE, LR_GOAL_TARGET_NONE);
    LR_CHECK_EQ_INT(c.goal1_type, LR_GOAL_SETTLE);
    LR_CHECK_EQ_INT(c.goal2_type, LR_GOAL_NONE);

    base(&c);
    LrSetGoal(&c, LR_GOAL_AVENGE, LR_TRAINER_PLAYER);
    LrSetGoal(&c, LR_GOAL_SETTLE, LR_GOAL_TARGET_NONE);
    LR_CHECK_EQ_INT(c.goal1_type, LR_GOAL_SETTLE);
    LR_CHECK_EQ_INT(c.goal2_type, LR_GOAL_AVENGE);

    /* Goal queries. */
    base(&c);
    LrSetGoal(&c, LR_GOAL_AVENGE, LR_TRAINER_PLAYER);
    LR_CHECK(LrHasGoal(&c, LR_GOAL_AVENGE));
    LR_CHECK(LrHasGoalAgainstPlayer(&c, LR_GOAL_AVENGE));
    base(&c);
    LrSetGoal(&c, LR_GOAL_AVENGE, 7);
    LR_CHECK(LrHasGoal(&c, LR_GOAL_AVENGE));
    LR_CHECK(!LrHasGoalAgainstPlayer(&c, LR_GOAL_AVENGE));
    base(&c);
    LrSetGoal(&c, LR_GOAL_PROVE, LR_TRAINER_PLAYER);
    LR_CHECK(LrHasGoalAgainstPlayer(&c, LR_GOAL_PROVE));

    /* InitGoals priority. */
    LrTrainerTraits traits;
    memset(&traits, 0, sizeof(traits));

    base(&c);
    LR_CHECK_EQ_INT(LrInitGoals(&c, &traits), 0);
    LR_CHECK_EQ_INT(c.goal1_type, LR_GOAL_NONE);

    base(&c); c.universalFacets[LR_FACET_BRAVERY] = 34;
    LR_CHECK_EQ_INT(LrInitGoals(&c, &traits), 1);
    LR_CHECK_EQ_INT(c.goal1_type, LR_GOAL_SETTLE);

    base(&c); c.universalFacets[LR_FACET_BRAVERY] = 35;
    c.universalFacets[LR_FACET_AMBITION] = 70;
    LR_CHECK_EQ_INT(LrInitGoals(&c, &traits), 1);
    LR_CHECK_EQ_INT(c.goal1_type, LR_GOAL_RISE);

    base(&c); c.universalFacets[LR_FACET_AMBITION] = 69;
    LR_CHECK_EQ_INT(LrInitGoals(&c, &traits), 0);

    base(&c); c.universalFacets[LR_FACET_AMBITION] = 60;
    traits.traitsRolled = 1; traits.wealth = LR_WEALTH_RICH;
    LR_CHECK_EQ_INT(LrInitGoals(&c, &traits), 1);
    LR_CHECK_EQ_INT(c.goal1_type, LR_GOAL_RICH);

    base(&c); c.universalFacets[LR_FACET_AMBITION] = 59;
    LR_CHECK_EQ_INT(LrInitGoals(&c, &traits), 0);

    /* InitGoals clears existing goals first. */
    base(&c);
    LrSetGoal(&c, LR_GOAL_RISE, LR_GOAL_TARGET_NONE);
    LR_CHECK_EQ_INT(LrInitGoals(&c, &traits), 0);
    LR_CHECK_EQ_INT(c.goal1_type, LR_GOAL_NONE);
    LR_CHECK_EQ_INT(c.goal2_type, LR_GOAL_NONE);

    /* ShiftIntent. */
    base(&c);
    LrSetGoal(&c, LR_GOAL_SETTLE, LR_GOAL_TARGET_NONE);
    LR_CHECK_EQ_INT(LrShiftIntent(&c, LR_INTENT_HUNTS, 0), LR_INTENT_WONT_HUNT);
    LR_CHECK_EQ_INT(LrShiftIntent(&c, LR_INTENT_WONT_HUNT, 0), LR_INTENT_WONT_HUNT);

    base(&c);
    LrSetGoal(&c, LR_GOAL_AVENGE, LR_TRAINER_PLAYER);
    c.hate = 40;
    LR_CHECK_EQ_INT(LrShiftIntent(&c, LR_INTENT_WONT_HUNT, 59), LR_INTENT_HUNTS);
    LR_CHECK_EQ_INT(LrShiftIntent(&c, LR_INTENT_NONE, 59), LR_INTENT_NONE);
    LR_CHECK_EQ_INT(LrShiftIntent(&c, LR_INTENT_WONT_HUNT, 60), LR_INTENT_WONT_HUNT);
    c.hate = 39;
    LR_CHECK_EQ_INT(LrShiftIntent(&c, LR_INTENT_WONT_HUNT, 10), LR_INTENT_WONT_HUNT);

    base(&c);
    LrSetGoal(&c, LR_GOAL_PROVE, LR_TRAINER_PLAYER);
    c.hate = 70;
    LR_CHECK_EQ_INT(LrShiftIntent(&c, LR_INTENT_WONT_HUNT, 10), LR_INTENT_HUNTS);

    base(&c);
    LrSetGoal(&c, LR_GOAL_AVENGE, 7);   /* not against the player */
    c.hate = 90;
    LR_CHECK_EQ_INT(LrShiftIntent(&c, LR_INTENT_WONT_HUNT, 10), LR_INTENT_WONT_HUNT);

    /* HasGoal(NONE) is always false. */
    base(&c);
    LR_CHECK(!LrHasGoal(&c, LR_GOAL_NONE));
    LR_CHECK(!LrHasGoalAgainstPlayer(&c, LR_GOAL_NONE));
    LrSetGoal(&c, LR_GOAL_RISE, LR_GOAL_TARGET_NONE);
    LR_CHECK(!LrHasGoal(&c, LR_GOAL_NONE));

    /* Avenge + stress = 1800. */
    base(&c);
    LrSetGoal(&c, LR_GOAL_AVENGE, LR_TRAINER_PLAYER);
    c.stress = 70;
    LR_CHECK_EQ_INT(LrAmbushMult(&c), 1800);

    /* AmbushMultiplier is per-mille. */
    base(&c);
    LR_CHECK_EQ_INT(LrAmbushMult(&c), 1000);
    LrSetGoal(&c, LR_GOAL_AVENGE, LR_TRAINER_PLAYER);
    LR_CHECK_EQ_INT(LrAmbushMult(&c), 1500);
    LrSetGoal(&c, LR_GOAL_PROVE, LR_TRAINER_PLAYER);
    LR_CHECK_EQ_INT(LrAmbushMult(&c), 2100);
    c.stress = 70;
    LR_CHECK_EQ_INT(LrAmbushMult(&c), 2520);

    base(&c);
    LrSetGoal(&c, LR_GOAL_SETTLE, LR_GOAL_TARGET_NONE);
    LR_CHECK_EQ_INT(LrAmbushMult(&c), 500);
    c.stress = 70;
    LR_CHECK_EQ_INT(LrAmbushMult(&c), 600);

    base(&c);
    c.stress = 70;
    LR_CHECK_EQ_INT(LrAmbushMult(&c), 1200);

    return lr_test_summary("test_goals");
}
