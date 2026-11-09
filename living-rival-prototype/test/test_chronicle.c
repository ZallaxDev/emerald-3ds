#include <string.h>
#include "lr_test.h"
#include "lr_chronicle.h"

static LrChronicle logbook, before;

static void test_ring_ids(void) {
    LrWorldEvent event = {0};
    event.people[0] = 1000;
    event.people[1] = 2000;
    LrChronicleInit(&logbook);
    for (uint32_t i = 1; i <= 501; i++) {
        uint32_t id = 0;
        event.minute = i;
        LR_CHECK(LrChronicleAppend(&logbook, &event, &id));
        LR_CHECK(id == i);
    }
    LR_CHECK_EQ_INT(logbook.count, 500);
    LR_CHECK_EQ_INT(logbook.nextId, 502);
    LR_CHECK(LrChronicleFind(&logbook, 1) == NULL);
    LR_CHECK(LrChronicleFind(&logbook, 2) != NULL);
    const LrWorldEvent *found = LrChronicleFind(&logbook, 501);
    LR_CHECK(found != NULL);
    if (found) {
        LR_CHECK_EQ_INT(found->people[0], 1000);
        LR_CHECK_EQ_INT(found->people[1], 2000);
    }
}

static void test_stale_heard(void) {
    LR_CHECK(!LrChronicleMarkHeard(&logbook, 1));
    LR_CHECK_EQ_INT(LrChronicleFind(&logbook, 501)->heard, 0);
    LR_CHECK(LrChronicleMarkHeard(&logbook, 501));
    LR_CHECK_EQ_INT(LrChronicleFind(&logbook, 501)->heard, 1);
}

static void test_exhaustion(void) {
    LrWorldEvent event = {0};
    logbook.nextId = UINT32_MAX;
    before = logbook;
    uint32_t id = 42;
    LR_CHECK(!LrChronicleAppend(&logbook, &event, &id));
    LR_CHECK_EQ_INT(id, 42);
    LR_CHECK(memcmp(&before, &logbook, sizeof(before)) == 0);
}

static void test_invalid(void) {
    LrWorldEvent event = {0};
    LrChronicleInit(&logbook);
    event.kind = 255;
    LR_CHECK(!LrChronicleAppend(&logbook, &event, NULL));
    event.kind = 0; event.region = 255;
    LR_CHECK(!LrChronicleAppend(&logbook, &event, NULL));
    event.region = 0; event.heard = 2;
    LR_CHECK(!LrChronicleAppend(&logbook, &event, NULL));
    LR_CHECK(!LrChronicleAppend(NULL, &event, NULL));
    LR_CHECK(LrChronicleFind(NULL, 1) == NULL);
    LR_CHECK(!LrChronicleMarkHeard(NULL, 1));
    event.heard = 0; logbook.head = 500;
    LR_CHECK(!LrChronicleAppend(&logbook, &event, NULL));
    logbook.head = 0; logbook.count = 501;
    LR_CHECK(LrChronicleFind(&logbook, 1) == NULL);
}

static void test_reject_invalid_factions(void) {
    LrWorldEvent event = {0};
    LrChronicleInit(&logbook);
    for (unsigned i = 0; i < 4; i++) {
        const uint8_t invalid[] = {LR_FACTION_COUNT,254};
        for (unsigned j = 0; j < 2; j++) {
            event.factions[i] = invalid[j]; before = logbook;
            uint32_t id = 42;
            LR_CHECK(!LrChronicleAppend(&logbook,&event,&id));
            LR_CHECK_EQ_INT(id,42);
            LR_CHECK(memcmp(&logbook,&before,sizeof(logbook)) == 0);
        }
        event.factions[i] = 255;
    }
    LR_CHECK(LrChronicleAppend(&logbook,&event,NULL));
    for (unsigned i = 0; i < 4; i++) event.factions[i] = LR_FACTION_COUNT-1;
    LR_CHECK(LrChronicleAppend(&logbook,&event,NULL));
}

int main(void) {
    test_ring_ids();
    test_stale_heard();
    test_exhaustion();
    test_invalid();
    test_reject_invalid_factions();
    return lr_test_summary("test_chronicle");
}
