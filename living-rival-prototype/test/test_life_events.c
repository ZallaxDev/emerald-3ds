#include "lr_test.h"
#include "lr_fixtures.h"
#include "lr_life_events.h"

static LrWorldState world, before;
static LrLifeEvent events[3];
static LrEventCatalog catalog;

static void setup(void) {
    LR_CHECK(test_world_init(&world, 1, 23, 300u * 1440u));
    memset(events, 0, sizeof(events));
    for (unsigned i = 0; i < 3; i++) {
        events[i].id = events[i].titleId = events[i].phraseId = (uint16_t)(i+1);
        events[i].odds = 20; events[i].pool = 255;
    }
    catalog = (LrEventCatalog){events, 3, 1};
}

static void test_conditions_and_pools(void) {
    setup();
    events[0].conditionCount = 2;
    events[0].conditions[0] = (LrEventCondition){LR_COND_SKILL, LR_OP_GE, 50};
    events[0].conditions[1] = (LrEventCondition){LR_COND_BADGES, LR_OP_LE, 7};
    LR_CHECK(LrEventFits(&world, 0, &events[0]));
    world.runtime[0].skill = 49;
    LR_CHECK(!LrEventFits(&world, 0, &events[0]));
    world.runtime[0].skill = 50; events[0].pool = LR_FACTION_TEAM_AQUA;
    LR_CHECK(!LrEventFits(&world, 0, &events[0]));
    events[0].pool = LR_FACTION_INDEPENDENT;
    LR_CHECK(LrEventFits(&world, 0, &events[0]));
}

static void test_recent_and_phrase(void) {
    setup();
    world.runtime[0].historyCount = world.runtime[0].historyHead = 2;
    world.runtime[0].historyEventIds[0] = 1;
    world.runtime[0].historyMinutes[0] = world.nowMinute - 4319;
    world.runtime[0].historyPhraseIds[1] = 2;
    world.runtime[0].historyMinutes[1] = 1;
    const LrLifeEvent *picked = LrPickLifeEvent(&world, 0, &catalog, world.nowMinute, 255);
    LR_CHECK(picked && picked->id == 3);
    world.runtime[0].historyPhraseIds[1] = 0;
    events[1].odds = events[2].odds = 0;
    world.runtime[0].historyMinutes[0] = world.nowMinute - 4320;
    picked = LrPickLifeEvent(&world, 0, &catalog, world.nowMinute, 255);
    LR_CHECK(picked && picked->id == 1);
}

static void test_fallback(void) {
    setup(); catalog.count = 1;
    world.runtime[0].historyCount = world.runtime[0].historyHead = 1;
    world.runtime[0].historyPhraseIds[0] = 1;
    const LrLifeEvent *picked = LrPickLifeEvent(&world, 0, &catalog, world.nowMinute, 255);
    LR_CHECK(picked && picked->id == 1);
    events[0].pool = LR_FACTION_TEAM_MAGMA;
    before = world;
    LR_CHECK(LrPickLifeEvent(&world, 0, &catalog, world.nowMinute, 255) == NULL);
    LR_CHECK(memcmp(&world, &before, sizeof(world)) == 0);
}

static void test_zero_weights(void) {
    setup(); for (unsigned i = 0; i < 3; i++) events[i].odds = 0;
    before = world;
    LR_CHECK(LrPickLifeEvent(&world, 0, &catalog, world.nowMinute, 255) == NULL);
    LR_CHECK(world.rng.state == before.rng.state);
    const uint16_t weights[] = {65535,65535,1};
    LR_CHECK_EQ_INT(LrWeightedIndex(weights, 3, 131070), 2);
    LR_CHECK_EQ_INT(LrWeightedIndex(weights, 3, 65535), 1);
    LR_CHECK_EQ_INT(LrWeightedIndex(weights, 3, 0), 0);
    const uint16_t zero[] = {0};
    LR_CHECK_EQ_INT(LrWeightedIndex(zero, 1, 99), 1);
}

static void test_effects_and_causes(void) {
    setup();
    events[0].effectCount = 3;
    events[0].effects[0] = (LrEventEffect){LR_FX_HATE,0,100};
    events[0].effects[1] = (LrEventEffect){LR_FX_TRAINING,0,10};
    events[0].effects[2] = (LrEventEffect){LR_FX_RESOURCE,0,5};
    LR_CHECK(LrApplyLifeEvent(&world, 0, &events[0], world.nowMinute, 30));
    LR_CHECK_EQ_INT(world.exts[0].memoryCauses[0], LR_CAUSE_WORLD);
    LR_CHECK_EQ_INT(world.cores[0].goal1_type, LR_GOAL_NONE);
    LR_CHECK_EQ_INT(world.runtime[0].trainingPoints, 300);
    LR_CHECK_EQ_INT(world.runtime[0].resources, 100);
    LR_CHECK_EQ_INT(world.chronicle.count, 1);
    LR_CHECK_EQ_INT(world.runtime[0].historyCount, 1);
    LR_CHECK_EQ_INT(world.runtime[0].historyMinutes[0], 432000);
    world.chronicle.nextId = UINT32_MAX; before = world;
    LR_CHECK(!LrApplyLifeEvent(&world, 0, &events[0], world.nowMinute, 1));
    LR_CHECK(memcmp(&world, &before, sizeof(world)) == 0);
}

static void test_pending_not_party(void) {
    setup();
    events[0].effectCount = 4;
    events[0].effects[0] = (LrEventEffect){LR_FX_CAPTURE_INTENT,0,1};
    events[0].effects[1] = (LrEventEffect){LR_FX_EVOLVE_INTENT,0,1};
    events[0].effects[2] = (LrEventEffect){LR_FX_REORGANIZE_INTENT,0,1};
    events[0].effects[3] = (LrEventEffect){LR_FX_PHASE,LR_PHASE_RETIRED,0};
    world.runtime[0].capturesPending = 65535;
    LR_CHECK(LrApplyLifeEvent(&world, 0, &events[0], world.nowMinute, 30));
    LR_CHECK_EQ_INT(world.runtime[0].capturesPending, 65535);
    LR_CHECK_EQ_INT(world.runtime[0].evolutionsPending, 1);
    LR_CHECK_EQ_INT(world.runtime[0].reorganizationsPending, 1);
    LR_CHECK_EQ_INT(world.cores[0].teamVersion, 0);
    LR_CHECK_EQ_INT(world.runtime[0].active, 0);
}

static void test_catalog_validation(void) {
    setup(); LR_CHECK(LrCatalogValidate(&catalog));
    LrEventCatalog prototype = LrPrototypeEventCatalog();
    LR_CHECK_EQ_INT(prototype.count, 12); LR_CHECK(LrCatalogValidate(&prototype));
    events[0].effects[0] = (LrEventEffect){LR_FX_HATE,0,101}; events[0].effectCount = 1;
    LR_CHECK(!LrCatalogValidate(&catalog));
    events[0].effects[0].amount = 100; events[0].effects[0].kind = 255;
    LR_CHECK(!LrCatalogValidate(&catalog));
    events[0].effectCount = 0; events[0].conditionCount = 1;
    events[0].conditions[0].field = 255;
    LR_CHECK(!LrCatalogValidate(&catalog));
    events[0].conditionCount = 0; events[1].id = 1;
    LR_CHECK(!LrCatalogValidate(&catalog));
    catalog.count = 271; LR_CHECK(!LrCatalogValidate(&catalog));
    setup(); events[0].effectCount = 1;
    events[0].effects[0] = (LrEventEffect){LR_FX_NEED,255,1};
    before = world;
    LR_CHECK(!LrApplyLifeEvent(&world, 0, &events[0], world.nowMinute, 1));
    LR_CHECK(memcmp(&world, &before, sizeof(world)) == 0);
}

static void test_retirement_phase_gate(void) {
    setup();
    LrEventCatalog prototype = LrPrototypeEventCatalog();
    const LrLifeEvent *retire = &prototype.events[prototype.count-1];
    world.cores[0].stress = 100;
    LR_CHECK(!LrEventFits(&world,0,retire));
    world.cores[0].journeyPhase = LR_PHASE_MENTOR;
    LR_CHECK(LrEventFits(&world,0,retire));
    LR_CHECK(LrApplyLifeEvent(&world,0,retire,world.nowMinute,1));
    LR_CHECK_EQ_INT(world.cores[0].journeyPhase,LR_PHASE_RETIRED);
    LR_CHECK_EQ_INT(world.runtime[0].active,0);
}

int main(void) {
    test_conditions_and_pools(); test_recent_and_phrase(); test_fallback(); test_zero_weights();
    test_effects_and_causes(); test_pending_not_party(); test_catalog_validation();
    test_retirement_phase_gate();
    return lr_test_summary("test_life_events");
}
