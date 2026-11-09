#include "lr_goals.h"

int32_t LrHateTier(uint8_t hate) {
    if (hate < 40) return 0;
    if (hate < 70) return 1;
    return 2;
}

int32_t LrInitGoals(TrainerJourneyCore *core, const LrTrainerTraits *traits) {
    core->goal1_type = LR_GOAL_NONE;
    core->goal1_target = LR_GOAL_TARGET_NONE;
    core->goal2_type = LR_GOAL_NONE;
    core->goal2_target = LR_GOAL_TARGET_NONE;

    if (!core->mindRolled) return 0;

    int32_t bravery = core->universalFacets[LR_FACET_BRAVERY];
    int32_t ambition = core->universalFacets[LR_FACET_AMBITION];

    if (bravery < 35) {
        LrSetGoal(core, LR_GOAL_SETTLE, LR_GOAL_TARGET_NONE);
        return 1;
    }
    if (ambition >= 70) {
        LrSetGoal(core, LR_GOAL_RISE, LR_GOAL_TARGET_NONE);
        return 1;
    }
    if (traits && traits->traitsRolled && traits->wealth == LR_WEALTH_RICH && ambition >= 60) {
        LrSetGoal(core, LR_GOAL_RICH, LR_GOAL_TARGET_NONE);
        return 1;
    }
    return 0;
}

bool LrSetGoal(TrainerJourneyCore *core, uint8_t type, uint16_t target) {
    if (type == LR_GOAL_NONE || type > LR_GOAL_PROTECT) return false;

    uint8_t oldType[2] = { core->goal1_type, core->goal2_type };
    uint16_t oldTarget[2] = { core->goal1_target, core->goal2_target };

    uint8_t keepType[2] = { LR_GOAL_NONE, LR_GOAL_NONE };
    uint16_t keepTarget[2] = { LR_GOAL_TARGET_NONE, LR_GOAL_TARGET_NONE };
    int kept = 0;

    for (int i = 0; i < 2; i++) {
        uint8_t t = oldType[i];
        if (t == LR_GOAL_NONE || t == type) continue;
        if (type == LR_GOAL_AVENGE && t == LR_GOAL_SETTLE) continue;
        if (type == LR_GOAL_SETTLE && (t == LR_GOAL_PROVE || t == LR_GOAL_RISE)) continue;
        if (kept < 2) {
            keepType[kept] = t;
            keepTarget[kept] = oldTarget[i];
            kept++;
        }
    }

    core->goal1_type = type;
    core->goal1_target = target;
    core->goal2_type = keepType[0];
    core->goal2_target = keepTarget[0];
    return true;
}

bool LrHasGoal(const TrainerJourneyCore *core, uint8_t type) {
    return core->goal1_type == type || core->goal2_type == type;
}

bool LrHasGoalAgainstPlayer(const TrainerJourneyCore *core, uint8_t type) {
    if (core->goal1_type == type && core->goal1_target == LR_TRAINER_PLAYER) return true;
    if (core->goal2_type == type && core->goal2_target == LR_TRAINER_PLAYER) return true;
    return false;
}

uint8_t LrShiftIntent(const TrainerJourneyCore *core, uint8_t intent, int32_t effectiveFear) {
    if (LrHasGoal(core, LR_GOAL_SETTLE) && intent == LR_INTENT_HUNTS)
        return (uint8_t)LR_INTENT_WONT_HUNT;

    if ((LrHasGoalAgainstPlayer(core, LR_GOAL_AVENGE) || LrHasGoalAgainstPlayer(core, LR_GOAL_PROVE)) &&
        intent != LR_INTENT_NONE && effectiveFear < 60 && LrHateTier(core->hate) != 0)
        return (uint8_t)LR_INTENT_HUNTS;

    return intent;
}

int32_t LrAmbushMult(const TrainerJourneyCore *core) {
    int32_t m = 1000;
    if (LrHasGoalAgainstPlayer(core, LR_GOAL_AVENGE)) m = m * 1500 / 1000;
    if (LrHasGoalAgainstPlayer(core, LR_GOAL_PROVE)) m = m * 1400 / 1000;
    if (LrHasGoal(core, LR_GOAL_SETTLE)) m = m * 500 / 1000;
    if (core->stress >= 70) m = m * 1200 / 1000;
    return m;
}
