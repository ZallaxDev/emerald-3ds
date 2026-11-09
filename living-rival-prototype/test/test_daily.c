#include "lr_test.h"
#include "lr_fixtures.h"
#include "lr_daily.h"
#include "lr_needs.h"

static LrWorldState world, before;

static void test_memory_aging(void) {
    LR_CHECK(test_world_init(&world, 1, 23, 1440));
    TrainerJourneyExtended *e = &world.exts[0];
    e->memoryCount = 2;
    e->memoryIntensities[0] = 10; e->memoryIntensities[1] = 31;
    e->memoryLongTerm[1] = 1; e->memoryEmotions[1] = LR_MEMORY_GRIEF;
    e->memoryCauses[1] = LR_CAUSE_OTHER_TRAINER; e->memoryTargetFacets[1] = LR_FACET_SOCIAL;
    e->memoryTimestamps[1] = 1;
    LrAgeTrainer(&world, 0, 1, 1440);
    LR_CHECK_EQ_INT(e->memoryCount, 1);
    LR_CHECK_EQ_INT(e->memoryIntensities[0], 30);
    LR_CHECK_EQ_INT(e->memoryLongTerm[0], 1);
    LR_CHECK_EQ_INT(e->memoryEmotions[0], LR_MEMORY_GRIEF);
    LR_CHECK_EQ_INT(e->memoryCauses[0], LR_CAUSE_OTHER_TRAINER);
    LR_CHECK_EQ_INT(e->memoryTargetFacets[0], LR_FACET_SOCIAL);
    LR_CHECK_EQ_INT(e->memoryTimestamps[0], 1);
    LR_CHECK_EQ_INT(e->memoryIntensities[1], 0);
    LR_CHECK_EQ_INT(e->memoryTargetFacets[1], 0);
    LrAgeTrainer(&world, 0, 65535, 1440);
    LR_CHECK_EQ_INT(e->memoryIntensities[0], 30);
}

static void test_goal_identity(void) {
    LR_CHECK(test_world_init(&world, 1, 23, 100));
    LR_CHECK(LrSetTrackedGoal(&world, 0, LR_GOAL_RICH, LR_GOAL_TARGET_NONE, 100));
    world.nowMinute = 200;
    LR_CHECK(LrSetTrackedGoal(&world, 0, LR_GOAL_AVENGE, LR_TRAINER_PLAYER, 200));
    world.nowMinute = 300;
    LR_CHECK(LrSetTrackedGoal(&world, 0, LR_GOAL_RICH, LR_GOAL_TARGET_NONE, 300));
    LR_CHECK_EQ_INT(world.runtime[0].goalProgressMinute[0], 100);
    LR_CHECK_EQ_INT(world.runtime[0].goalProgressMinute[1], 200);
    LR_CHECK(LrRecordGoalProgress(&world, 0, LR_GOAL_RICH, LR_GOAL_TARGET_NONE, 300));
    LR_CHECK_EQ_INT(world.runtime[0].goalProgressMinute[0], 300);
    LR_CHECK_EQ_INT(world.runtime[0].goalProgressMinute[1], 200);
    before = world;
    LR_CHECK(!LrRecordGoalProgress(&world, 0, LR_GOAL_RICH, LR_GOAL_TARGET_NONE, 301));
    LR_CHECK(memcmp(&world, &before, sizeof(world)) == 0);
}

static void test_expiry(void) {
    LR_CHECK(test_world_init(&world, 1, 23, 0));
    LR_CHECK(LrSetTrackedGoal(&world, 0, LR_GOAL_RICH, LR_GOAL_TARGET_NONE, 0));
    world.nowMinute = 10;
    LR_CHECK(LrSetTrackedGoal(&world, 0, LR_GOAL_PROTECT, 2000, 10));
    world.nowMinute = 43200; LrAgeTrainer(&world, 0, 30, 43200);
    LR_CHECK_EQ_INT(world.cores[0].goal2_type, LR_GOAL_RICH);
    world.nowMinute = 43201; LrAgeTrainer(&world, 0, 0, 43201);
    LR_CHECK_EQ_INT(world.cores[0].goal1_type, LR_GOAL_PROTECT);
    LR_CHECK_EQ_INT(world.cores[0].goal2_type, LR_GOAL_NONE);
    LR_CHECK_EQ_INT(world.cores[0].goal2_target, LR_GOAL_TARGET_NONE);
    LR_CHECK_EQ_INT(world.runtime[0].goalProgressMinute[0], 10);
}

static void test_daily_order(void) {
    LR_CHECK(test_world_init(&world, 1, 23, 1440));
    world.cores[0].fear = 5; world.cores[0].notoriety = 5;
    world.exts[0].memoryCount = 1; world.exts[0].memoryIntensities[0] = 10;
    LrLifeEvent event = {.id=1,.titleId=1,.odds=20,.pool=255,.effectCount=2};
    event.effects[0] = (LrEventEffect){LR_FX_FEAR,0,10};
    event.effects[1] = (LrEventEffect){LR_FX_GOAL,LR_GOAL_RICH,0};
    LrEventCatalog catalog = {&event,1,1};
    LrDailyTick(&world, &catalog, 1440);
    LR_CHECK_EQ_INT(world.cores[0].notoriety, 3);
    LR_CHECK_EQ_INT(world.cores[0].fear, 14);
    LR_CHECK_EQ_INT(world.exts[0].memoryCount, 1);
    LR_CHECK_EQ_INT(world.exts[0].memoryEmotions[0], LR_MEMORY_FEAR);
    LR_CHECK_EQ_INT(world.runtime[0].goalProgressMinute[0], 1440);
    LR_CHECK(LrWorldValidate(&world));
    world.exts[0].memoryCount = 0;
    world.cores[0].stress = 0;
    LrHourlyTick(&world, 1500);
    LR_CHECK_EQ_INT(world.cores[0].stress, 0);
}

static void test_event_progress(void) {
    LR_CHECK(test_world_init(&world, 1, 23, 0));
    LR_CHECK(LrSetTrackedGoal(&world, 0, LR_GOAL_RISE, LR_GOAL_TARGET_NONE, 0));
    LR_CHECK(LrSetTrackedGoal(&world, 0, LR_GOAL_RICH, LR_GOAL_TARGET_NONE, 0));
    world.nowMinute = 100;
    LrLifeEvent event = {.id=1,.titleId=1,.odds=20,.pool=255,.effectCount=1};
    event.effects[0] = (LrEventEffect){LR_FX_BADGE,0,1};
    LR_CHECK(LrApplyLifeEvent(&world, 0, &event, 100, 1));
    LR_CHECK_EQ_INT(world.runtime[0].goalProgressMinute[0], 0);
    LR_CHECK_EQ_INT(world.runtime[0].goalProgressMinute[1], 100);
    world.nowMinute = 200; event.effects[0] = (LrEventEffect){LR_FX_RESOURCE,0,5};
    LR_CHECK(LrApplyLifeEvent(&world, 0, &event, 200, 1));
    LR_CHECK_EQ_INT(world.runtime[0].goalProgressMinute[0], 200);
    LR_CHECK_EQ_INT(world.runtime[0].goalProgressMinute[1], 100);
}

int main(void) {
    test_memory_aging(); test_goal_identity(); test_expiry(); test_daily_order(); test_event_progress();
    return lr_test_summary("test_daily");
}
