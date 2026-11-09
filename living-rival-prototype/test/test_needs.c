#include <string.h>
#include "lr_test.h"
#include "lr_needs.h"

static LrWorldState world, before;

static void test_need_schedule(void) {
    TrainerJourneyCore c = {0}; TrainerJourneyExtended e = {0};
    c.faction = LR_FACTION_INDEPENDENT;
    const uint32_t minutes[] = {60,180,360,720};
    const uint8_t want[][5] = {{5,0,0,0,1},{5,0,0,8,1},{5,5,0,8,1},{5,5,10,8,1}};
    for (unsigned i = 0; i < 4; i++) {
        memset(c.needs, 0, sizeof(c.needs));
        LrTickNeeds(&c, &e, minutes[i]);
        LR_CHECK(memcmp(c.needs, want[i], 5) == 0);
    }
    for (unsigned f = LR_FACTION_TEAM_MAGMA; f <= LR_FACTION_TEAM_AQUA; f++) {
        c.faction = (uint8_t)f; c.needs[LR_NEED_VIOLENCE] = 0;
        LrTickNeeds(&c, &e, 60);
        LR_CHECK_EQ_INT(c.needs[LR_NEED_VIOLENCE], 3);
    }
    memset(c.needs, 99, sizeof(c.needs));
    LrTickNeeds(&c, &e, 720);
    for (unsigned i = 0; i < 5; i++) LR_CHECK_EQ_INT(c.needs[i], 100);
}

static void test_stress(void) {
    TrainerJourneyCore c = {0}; TrainerJourneyExtended e = {0};
    c.needs[0] = 69; e.memoryCount = 2;
    e.memoryLongTerm[0] = e.memoryLongTerm[1] = 1;
    e.memoryEmotions[0] = LR_MEMORY_ANGER; e.memoryEmotions[1] = LR_MEMORY_JOY;
    LrTickNeeds(&c, &e, 60);
    LR_CHECK_EQ_INT(c.stress, 3);
    c.stress = 99; e.memoryCount = 255;
    LrTickNeeds(&c, &e, 60);
    LR_CHECK_EQ_INT(c.stress, 100);
}

static void setup(void) {
    memset(&world, 0, sizeof(world));
    world.trainerCount = 3;
    LrChronicleInit(&world.chronicle);
    for (unsigned i = 0; i < 3; i++) {
        world.cores[i].trainerId = (uint16_t)(1000 + i * 1000);
        world.cores[i].faction = LR_FACTION_TEAM_MAGMA;
        world.cores[i].currentRegion = LR_REGION_HOENN;
        world.runtime[i].active = 1;
        world.runtime[i].hostileTo = LR_GOAL_TARGET_NONE;
    }
    world.cores[0].stress = 85;
    world.rng.state = 23;
}

static void test_snap(void) {
    setup(); world.runtime[2].active = 0;
    LR_CHECK(LrTrySnap(&world, 0, 60));
    LR_CHECK_EQ_INT(world.cores[0].stress, 65);
    LR_CHECK_EQ_INT(world.runtime[0].hostileTo, 2000);
    const LrWorldEvent *event = LrChronicleFind(&world.chronicle, 1);
    LR_CHECK(event != NULL);
    if (event) {
        LR_CHECK_EQ_INT(event->kind, LR_EVENT_CLASH);
        LR_CHECK_EQ_INT(event->people[0], 1000);
        LR_CHECK_EQ_INT(event->people[1], 2000);
        LR_CHECK_EQ_INT(event->outcomes[0], 1);
        LR_CHECK_EQ_INT(event->outcomes[1], 2);
    }
    LR_CHECK_EQ_INT(world.cores[1].fear, 0);
    setup(); world.rng.state = 86;
    LR_CHECK(!LrTrySnap(&world, 0, 60));
    LR_CHECK_EQ_INT(world.chronicle.count, 0);
    LR_CHECK(world.rng.state != 86);
    setup(); world.cores[0].stress = 84; before = world;
    LR_CHECK(!LrTrySnap(&world, 0, 60));
    LR_CHECK(memcmp(&before, &world, sizeof(world)) == 0);
}

static void test_victims(void) {
    for (unsigned mode = 0; mode < 4; mode++) {
        setup(); world.runtime[2].active = 0;
        if (mode == 0) world.cores[1].faction = LR_FACTION_TEAM_AQUA;
        if (mode == 1) world.cores[1].currentRegion = LR_REGION_JOHTO;
        if (mode == 2) world.runtime[1].active = 0;
        if (mode == 3) world.cores[0].faction = world.cores[1].faction = LR_FACTION_INDEPENDENT;
        before = world;
        LR_CHECK(!LrTrySnap(&world, 0, 60));
        LR_CHECK(memcmp(&before, &world, sizeof(world)) == 0);
    }
    setup(); world.chronicle.nextId = UINT32_MAX; before = world;
    LR_CHECK(!LrTrySnap(&world, 0, 60));
    LR_CHECK(memcmp(&before, &world, sizeof(world)) == 0);
    setup(); world.runtime[0].active = 0;
    LrHourlyTick(&world, 60);
    LR_CHECK_EQ_INT(world.cores[0].needs[0], 0);
    LR_CHECK_EQ_INT(world.cores[1].needs[0], 5);
}

int main(void) {
    test_need_schedule(); test_stress(); test_snap(); test_victims();
    return lr_test_summary("test_needs");
}
