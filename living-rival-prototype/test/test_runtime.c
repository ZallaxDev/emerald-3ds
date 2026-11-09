#include <string.h>
#include "lr_test.h"
#include "lr_mind.h"
#include "lr_runtime.h"
#include "lr_simulation.h"
#include "lr_snapshot.h"

static LrWorldState world, before;
static TrainerJourneyCore cores[2];
static TrainerJourneyExtended exts[2];
static LrTrainerTraits traits[2];

static void setup(void) {
    memset(cores, 0, sizeof(cores));
    memset(exts, 0, sizeof(exts));
    memset(traits, 0, sizeof(traits));
    for (unsigned i = 0; i < 2; i++) {
        cores[i].trainerId = (uint16_t)(1000 + i);
        cores[i].temperament = LR_TEMP_STEADY;
        cores[i].faction = LR_FACTION_INDEPENDENT;
        cores[i].archetype = LR_ARCH_EXPERIMENTER;
        traits[i].traitsRolled = 1;
        traits[i].wealth = LR_WEALTH_RICH;
        traits[i].rank = 4;
        LrRollMind(&cores[i], &exts[i], &traits[i], i + 7);
    }
}

static void test_defaults(void) {
    setup();
    LR_CHECK(LrWorldInit(&world, cores, exts, traits, 2, 123, 1440, 1));
    LR_CHECK(LrWorldValidate(&world));
    LR_CHECK_EQ_INT(world.runtime[0].resources, 80);
    LR_CHECK_EQ_INT(world.runtime[0].skill, 52);
    LR_CHECK_EQ_INT(world.runtime[0].collector, 80);
    LR_CHECK_EQ_INT(world.runtime[0].goalProgressMinute[0], 1440);
    LR_CHECK_EQ_INT(world.runtime[0].hostileTo, LR_GOAL_TARGET_NONE);
    LR_CHECK_EQ_INT(world.rng.state, 123);
    LR_CHECK_EQ_INT(world.chronicle.nextId, 1);
    LR_CHECK(memcmp(cores, world.cores, sizeof(cores)) == 0);
    LR_CHECK(memcmp(exts, world.exts, sizeof(exts)) == 0);
    LR_CHECK_EQ_INT(LrFindTrainer(&world, 1000), 0);
    LR_CHECK_EQ_INT(LrFindTrainer(&world, 2), -1);
}

static void test_owned_copy(void) {
    cores[0].hate = 99;
    traits[0].wealth = LR_WEALTH_POOR;
    LR_CHECK_EQ_INT(world.cores[0].hate, 0);
    LR_CHECK_EQ_INT(world.traits[0].wealth, LR_WEALTH_RICH);
}

static void test_invalid_init(void) {
    setup();
    before = world;
    LR_CHECK(!LrWorldInit(&world, NULL, NULL, NULL, 81, 1, 0, 1));
    LR_CHECK(!LrWorldInit(&world, cores, exts, traits, 2, 1, LR_MAX_MINUTE + 1, 1));
    cores[1].trainerId = cores[0].trainerId;
    LR_CHECK(!LrWorldInit(&world, cores, exts, traits, 2, 1, 0, 1));
    cores[1].trainerId = 999;
    LR_CHECK(!LrWorldInit(&world, cores, exts, traits, 2, 1, 0, 1));
    cores[1].trainerId = LR_TRAINER_PLAYER;
    LR_CHECK(!LrWorldInit(&world, cores, exts, traits, 2, 1, 0, 1));
    setup(); cores[0].temperament = 255;
    LR_CHECK(!LrWorldInit(&world, cores, exts, traits, 2, 1, 0, 1));
    setup(); exts[0].memoryCount = 13;
    LR_CHECK(!LrWorldInit(&world, cores, exts, traits, 2, 1, 0, 1));
    LR_CHECK(memcmp(&before, &world, sizeof(world)) == 0);
}

static void test_validation(void) {
    before = world;
#define REJECT(field, value) do { world = before; world.field = (value); LR_CHECK(!LrWorldValidate(&world)); } while (0)
    REJECT(cores[0].needs[0], 101);
    REJECT(traits[0].wealth, 255);
    REJECT(traits[0].aggression, 101);
    REJECT(runtime[0].historyCount, 61);
    REJECT(runtime[0].goalProgressMinute[0], 1441);
    REJECT(cores[0].goal1_type, 255);
    REJECT(cores[0].goal1_target, LR_TRAINER_PLAYER);
    REJECT(exts[0].memoryCount, 255);
    world = before;
    world.runtime[0].historyCount = 1; world.runtime[0].historyHead = 1;
    world.runtime[0].historyMinutes[0] = 1441;
    LR_CHECK(!LrWorldValidate(&world));
    world = before;
    LrWorldEvent event = {0}; event.minute = 1441;
    LR_CHECK(LrChronicleAppend(&world.chronicle, &event, NULL));
    LR_CHECK(!LrWorldValidate(&world));
    world = before;
    LR_CHECK(LrWorldInit(&world, NULL, NULL, NULL, 0, 1, 0, 1));
    LR_CHECK(LrWorldValidate(&world));
    setup();
    LR_CHECK(LrWorldInit(&world, cores, exts, NULL, 2, 1, 0, 1));
    LR_CHECK_EQ_INT(world.runtime[0].resources, 50);
#undef REJECT
}

static void test_rejected_event_preserves_world(void) {
    setup();
    LR_CHECK(LrWorldInit(&world,cores,exts,traits,2,1,0,1));
    before = world;
    LrWorldEvent event = {0}; event.factions[0] = LR_FACTION_COUNT;
    uint32_t id = 42;
    LR_CHECK(!LrChronicleAppend(&world.chronicle,&event,&id));
    LR_CHECK_EQ_INT(id,42);
    LR_CHECK(memcmp(&world,&before,sizeof(world)) == 0);
    LR_CHECK(LrWorldValidate(&world));
    LrEventCatalog catalog = {NULL,0,1}; LrAdvanceStats stats;
    LR_CHECK(LrSimulateTo(&world,&catalog,60,&stats));
    static uint8_t buffer[90000]; size_t written = 0;
    LR_CHECK(LrSnapshotWrite(&world,buffer,sizeof(buffer),&written));
}

int main(void) {
    test_defaults(); test_owned_copy(); test_invalid_init(); test_validation();
    test_rejected_event_preserves_world();
    return lr_test_summary("test_runtime");
}
