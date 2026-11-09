#include <stddef.h>
#include <string.h>
#include "lr_chronicle.h"

static bool valid_ring(const LrChronicle *log) {
    return log && log->count <= LR_CHRONICLE_CAPACITY &&
           log->head < LR_CHRONICLE_CAPACITY && log->nextId != 0 &&
           (log->count == LR_CHRONICLE_CAPACITY || log->head == log->count);
}

void LrChronicleInit(LrChronicle *log) {
    if (!log) return;
    memset(log, 0, sizeof(*log));
    log->nextId = 1;
}

bool LrChronicleAppend(LrChronicle *log, const LrWorldEvent *event, uint32_t *outId) {
    if (!valid_ring(log) || !event || log->nextId == UINT32_MAX ||
        event->kind >= LR_EVENT_KIND_COUNT || event->region >= LR_REGION_COUNT ||
        event->heard > 1 || event->minute > LR_MAX_MINUTE) return false;
    for (unsigned i = 0; i < 4; i++)
        if (event->factions[i] != 255 && event->factions[i] >= LR_FACTION_COUNT) return false;
    LrWorldEvent copy = *event;
    copy.id = log->nextId++;
    log->events[log->head] = copy;
    log->head = (uint16_t)((log->head + 1) % LR_CHRONICLE_CAPACITY);
    if (log->count < LR_CHRONICLE_CAPACITY) log->count++;
    if (outId) *outId = copy.id;
    return true;
}

const LrWorldEvent *LrChronicleFind(const LrChronicle *log, uint32_t id) {
    if (!valid_ring(log) || id == 0 || id == UINT32_MAX) return NULL;
    for (uint16_t i = 0; i < log->count; i++)
        if (log->events[i].id == id) return &log->events[i];
    return NULL;
}

bool LrChronicleMarkHeard(LrChronicle *log, uint32_t id) {
    const LrWorldEvent *event = LrChronicleFind(log, id);
    if (!event) return false;
    log->events[event - log->events].heard = 1;
    return true;
}
