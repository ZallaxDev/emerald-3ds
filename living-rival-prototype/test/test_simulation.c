#include "lr_test.h"
#include "lr_fixtures.h"
#include "lr_simulation.h"

static LrWorldState world, other, before;

static void test_partial_minutes(void) {
    LR_CHECK(test_world_init(&world,1,23,0));
    LrEventCatalog catalog = {NULL,0,1}; LrAdvanceStats s;
    LR_CHECK(LrSimulateTo(&world,&catalog,59,&s)); LR_CHECK_EQ_INT(s.hourlyTicks,0);
    LR_CHECK(LrSimulateTo(&world,&catalog,60,&s)); LR_CHECK_EQ_INT(s.hourlyTicks,1);
    LR_CHECK(LrSimulateTo(&world,&catalog,61,&s)); LR_CHECK_EQ_INT(s.hourlyTicks,0);
    LR_CHECK(LrSimulateTo(&world,&catalog,120,&s)); LR_CHECK_EQ_INT(s.hourlyTicks,1);
}

static void test_midnight_order(void) {
    LR_CHECK(test_world_init(&world,1,23,1380));
    world.cores[0].stress = 0; memset(world.cores[0].needs,0,5); world.cores[0].needs[0] = 69;
    LrLifeEvent event = {.id=1,.titleId=1,.odds=20,.pool=255,.conditionCount=1,.effectCount=1,
        .conditions={{LR_COND_STRESS,LR_OP_GE,2}},.effects={{LR_FX_STRESS,0,-2}}};
    LrEventCatalog catalog = {&event,1,1}; LrAdvanceStats s;
    LR_CHECK(LrSimulateTo(&world,&catalog,1440,&s));
    LR_CHECK_EQ_INT(world.chronicle.count,1); LR_CHECK_EQ_INT(world.cores[0].stress,0);
    LR_CHECK_EQ_INT(s.dailyTicks,1);
}

static void test_incremental_equivalence(void) {
    LR_CHECK(test_world_init(&world,3,23,0)); other = world;
    LrEventCatalog catalog = LrPrototypeEventCatalog(); LrAdvanceStats s;
    LR_CHECK(LrSimulateTo(&world,&catalog,2880,&s));
    for (uint32_t minute=1; minute<=2880; minute++) LR_CHECK(LrSimulateTo(&other,&catalog,minute,&s));
    LR_CHECK(memcmp(&world,&other,sizeof(world)) == 0);
}

static void test_caps(void) {
    LR_CHECK(test_world_init(&world,80,23,0));
    LrEventCatalog catalog = LrPrototypeEventCatalog(); LrAdvanceStats s;
    LR_CHECK(LrSimulateTo(&world,&catalog,180u*1440u,&s));
    LR_CHECK_EQ_INT(s.hourlyTicks,48); LR_CHECK_EQ_INT(s.dailyTicks,2);
    LR_CHECK(s.summaryBeats<=560); LR_CHECK_EQ_INT(world.nowMinute,259200);
    LR_CHECK_EQ_INT(s.summaryEndMinute,256320);
    LR_CHECK(LrWorldValidate(&world));
}

static void test_summary_split_contract(void) {
    LR_CHECK(test_world_init(&world,2,23,0)); other = world;
    LrEventCatalog catalog = LrPrototypeEventCatalog(); LrAdvanceStats s;
    LR_CHECK(LrSimulateTo(&world,&catalog,180u*1440u,&s));
    LR_CHECK(LrSimulateTo(&other,&catalog,180u*1440u,&s));
    LR_CHECK(memcmp(&world,&other,sizeof(world)) == 0);
}

static void test_noop_backward_limits(void) {
    LR_CHECK(test_world_init(&world,1,23,100)); before=world;
    LrEventCatalog catalog = {NULL,0,1}; LrAdvanceStats s;
    LR_CHECK(LrSimulateTo(&world,&catalog,100,&s));
    LR_CHECK_EQ_INT(s.hourlyTicks,0); LR_CHECK(memcmp(&world,&before,sizeof(world)) == 0);
    memset(&s,0xAA,sizeof(s)); LrAdvanceStats saved=s;
    LR_CHECK(!LrSimulateTo(&world,&catalog,99,&s));
    LR_CHECK(!LrSimulateTo(&world,&catalog,94371840u,&s));
    LR_CHECK(memcmp(&world,&before,sizeof(world)) == 0);
    LR_CHECK(memcmp(&s,&saved,sizeof(s)) == 0);
}

static void test_versions(void) {
    LR_CHECK(test_world_init(&world,1,23,0)); before=world;
    LrEventCatalog catalog = {NULL,0,2}; LrAdvanceStats s;
    LR_CHECK(!LrSimulateTo(&world,&catalog,100,&s));
    LR_CHECK(memcmp(&world,&before,sizeof(world)) == 0);
}

static void test_offline_clock(void) {
    LR_CHECK(test_world_init(&world,1,23,0));
    LrEventCatalog catalog = {NULL,0,1}; LrAdvanceStats s;
    LR_CHECK(LrAnchorOfflineClock(&world,1000));
    LR_CHECK(LrResumeOffline(&world,&catalog,1060,&s)); LR_CHECK_EQ_INT(world.nowMinute,60);
    const uint64_t samples[]={1060,900,1000,1060};
    for (unsigned i=0;i<4;i++) {
        LR_CHECK(LrResumeOffline(&world,&catalog,samples[i],&s)); LR_CHECK_EQ_INT(world.nowMinute,60);
    }
    LR_CHECK(LrResumeOffline(&world,&catalog,1120,&s)); LR_CHECK_EQ_INT(world.nowMinute,120);
    LR_CHECK(LrSimulateTo(&world,&catalog,240,&s));
    LR_CHECK(LrAnchorOfflineClock(&world,1240));
    LR_CHECK(LrResumeOffline(&world,&catalog,1300,&s)); LR_CHECK_EQ_INT(world.nowMinute,300);
    before=world; memset(&s,0xAA,sizeof(s)); LrAdvanceStats saved=s;
    LR_CHECK(!LrResumeOffline(&world,&catalog,UINT64_MAX,&s));
    LR_CHECK(memcmp(&world,&before,sizeof(world)) == 0);
    LR_CHECK(memcmp(&s,&saved,sizeof(s)) == 0);
    LR_CHECK(test_world_init(&world,0,23,0));
    LR_CHECK(LrResumeOffline(&world,&catalog,1000,&s));
    LR_CHECK_EQ_INT(world.nowMinute,0); LR_CHECK(world.rtcHighWater==1000);
}

static void test_offline_equivalence(void) {
    LR_CHECK(test_world_init(&world,1,23,0)); LR_CHECK(LrAnchorOfflineClock(&world,1000)); other=world;
    LrEventCatalog catalog = {NULL,0,1}; LrAdvanceStats s;
    LR_CHECK(LrResumeOffline(&world,&catalog,1060,&s));
    LR_CHECK(LrSimulateTo(&other,&catalog,60,&s)); other.rtcHighWater=1060;
    LR_CHECK(memcmp(&world,&other,sizeof(world)) == 0);
}

int main(void) {
    test_partial_minutes(); test_midnight_order(); test_incremental_equivalence(); test_caps();
    test_summary_split_contract(); test_noop_backward_limits(); test_versions();
    test_offline_clock(); test_offline_equivalence();
    return lr_test_summary("test_simulation");
}
