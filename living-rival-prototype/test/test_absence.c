#include "lr_test.h"
#include "lr_fixtures.h"
#include "lr_absence.h"
#include "lr_daily.h"

static LrWorldState world, other;
static LrLifeEvent training = {.id=1,.titleId=1,.odds=20,.pool=255,.effectCount=1,
    .effects={{LR_FX_TRAINING,0,10}}};

static void test_summary_bounds(void) {
    LR_CHECK(test_world_init(&world, 80, 23, 0)); other = world;
    LrEventCatalog catalog = {&training,1,1};
    LrAbsenceStats a, b;
    LrSummarizeAbsence(&world, &catalog, 0, 180u * 1440u, &a);
    LrSummarizeAbsence(&other, &catalog, 0, 180u * 1440u, &b);
    LR_CHECK_EQ_INT(a.beats, 560);
    LR_CHECK(a.appliedEvents <= 560);
    LR_CHECK_EQ_INT(a.agedDays, 180);
    LR_CHECK(memcmp(&world, &other, sizeof(world)) == 0);
    LR_CHECK(LrWorldValidate(&world));
    for (unsigned i = 0; i < 80; i++) LR_CHECK(world.runtime[i].trainingPoints <= 2100);
}

static void test_duration(void) {
    const uint32_t days[] = {1,6,180};
    const unsigned beats[] = {1,6,7};
    LrEventCatalog catalog = {&training,1,1}; LrAbsenceStats stats;
    for (unsigned i = 0; i < 3; i++) {
        LR_CHECK(test_world_init(&world, 1, 23, 0));
        LrSummarizeAbsence(&world, &catalog, 0, days[i] * 1440, &stats);
        LR_CHECK_EQ_INT(stats.beats, beats[i]);
    }
    LR_CHECK(test_world_init(&world, 1, 23, 0)); world.nowMinute = 1440;
    LR_CHECK(LrApplyLifeEvent(&world,0,&training,1440,1));
    LR_CHECK_EQ_INT(world.runtime[0].trainingPoints, 10);
    world.runtime[0].trainingPoints = 0;
    LR_CHECK(LrApplyLifeEvent(&world,0,&training,1440,30));
    LR_CHECK_EQ_INT(world.runtime[0].trainingPoints, 300);
}

static void test_personalized_weights(void) {
    LR_CHECK(test_world_init(&world, 2, 23, 0));
    world.cores[0].universalFacets[LR_FACET_AMBITION] = 90;
    world.cores[0].universalFacets[LR_FACET_DISCIPLINE] = 90;
    world.runtime[0].skill = 80; world.runtime[0].collector = 0;
    world.runtime[1].collector = 80;
    LR_CHECK_EQ_INT(LrCategoryWeight(&world,0,LR_CATEGORY_TRAINING), 230);
    LR_CHECK(LrCategoryWeight(&world,1,LR_CATEGORY_ACTIVITY) > LrCategoryWeight(&world,0,LR_CATEGORY_ACTIVITY));
}

static void test_impossible_categories(void) {
    LR_CHECK(test_world_init(&world, 1, 23, 0)); world.runtime[0].resources = 0;
    LrEventCatalog full = LrPrototypeEventCatalog();
    LrEventCatalog catalog = {&full.events[6],3,1};
    LrRng expected = world.rng;
    for (unsigned i = 0; i < 7; i++) LrRandomNext(&expected);
    LrAbsenceStats stats;
    LrSummarizeAbsence(&world,&catalog,0,180u*1440u,&stats);
    LR_CHECK_EQ_INT(world.runtime[0].capturesPending, 0);
    LR_CHECK_EQ_INT(stats.appliedEvents, 0);
    LR_CHECK(world.rng.state == expected.state);
}

static void test_not_checklist(void) {
    LR_CHECK(test_world_init(&world, 1, 23, 0));
    LrEventCatalog catalog = {&training,1,1}; LrAbsenceStats stats;
    LrSummarizeAbsence(&world,&catalog,0,180u*1440u,&stats);
    LR_CHECK(world.runtime[0].trainingPoints > 0);
    LR_CHECK_EQ_INT(world.runtime[0].capturesPending, 0);
    LR_CHECK_EQ_INT(world.cores[0].badgesEarned, 0);
    LR_CHECK_EQ_INT(world.cores[0].goal1_type, LR_GOAL_NONE);
}

static void test_bulk_aging(void) {
    LR_CHECK(test_world_init(&world, 1, 23, 0));
    world.exts[0].memoryCount = 2; world.exts[0].memoryIntensities[0] = 60;
    world.exts[0].memoryIntensities[1] = 90; world.exts[0].memoryLongTerm[1] = 1;
    world.cores[0].fear = world.cores[0].notoriety = 100;
    LR_CHECK(LrSetTrackedGoal(&world,0,LR_GOAL_RICH,LR_GOAL_TARGET_NONE,0));
    LrEventCatalog catalog = {NULL,0,1}; LrAbsenceStats stats;
    LrSummarizeAbsence(&world,&catalog,0,180u*1440u,&stats);
    LR_CHECK_EQ_INT(world.exts[0].memoryCount, 1);
    LR_CHECK_EQ_INT(world.exts[0].memoryIntensities[0], 30);
    LR_CHECK_EQ_INT(world.cores[0].fear, 0);
    LR_CHECK_EQ_INT(world.cores[0].notoriety, 0);
    LR_CHECK_EQ_INT(world.cores[0].goal1_type, LR_GOAL_NONE);
}

static void test_no_player_ceiling(void) {
    LR_CHECK(test_world_init(&world, 1, 23, 0));
    world.cores[0].badgesEarned = 7; world.runtime[0].skill = 80;
    LrEventCatalog full = LrPrototypeEventCatalog(), catalog = {&full.events[4],1,1};
    LrAbsenceStats stats;
    LrSummarizeAbsence(&world,&catalog,0,180u*1440u,&stats);
    LR_CHECK_EQ_INT(world.cores[0].badgesEarned, 8);
    LR_CHECK_EQ_INT(stats.appliedEvents, 1);
}

static void test_summary_timestamp_order(void) {
    LR_CHECK(test_world_init(&world, 2, 23, 0));
    world.cores[0].stress = world.cores[1].stress = 100;
    world.cores[0].journeyPhase = world.cores[1].journeyPhase = LR_PHASE_MENTOR;
    LrEventCatalog full = LrPrototypeEventCatalog(), catalog = {&full.events[11],1,1};
    LrAbsenceStats stats;
    LrSummarizeAbsence(&world,&catalog,0,7u*1440u,&stats);
    LR_CHECK_EQ_INT(world.runtime[0].active, 0);
    LR_CHECK(stats.beats < 14);
    unsigned firstCount = 0;
    for (unsigned i = 0; i < world.chronicle.count; i++) {
        const LrWorldEvent *event = &world.chronicle.events[i];
        if (event->people[0] == 1000) firstCount++;
        if (i) LR_CHECK(world.chronicle.events[i-1].minute <= event->minute);
    }
    LR_CHECK_EQ_INT(firstCount, 1);
}

int main(void) {
    test_summary_bounds(); test_duration(); test_personalized_weights(); test_impossible_categories();
    test_not_checklist(); test_bulk_aging(); test_no_player_ceiling(); test_summary_timestamp_order();
    return lr_test_summary("test_absence");
}
