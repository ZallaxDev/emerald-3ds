#ifndef LR_CHRONICLE_H
#define LR_CHRONICLE_H

#include <stdbool.h>
#include <stdint.h>
#include "lr_types.h"

#define LR_CHRONICLE_CAPACITY 500
#define LR_MAX_MINUTE 94371839u

typedef enum {
    LR_EVENT_LIFE = 0, LR_EVENT_CLASH, LR_EVENT_BADGE,
    LR_EVENT_RETIREMENT, LR_EVENT_DEATH, LR_EVENT_KIND_COUNT
} LrEventKind;

typedef struct {
    uint32_t id, minute;
    uint16_t contentId, titleId, lineIds[4], people[4];
    uint8_t kind, region, place, factions[4], outcomes[4], heard;
} LrWorldEvent;

typedef struct {
    LrWorldEvent events[LR_CHRONICLE_CAPACITY];
    uint16_t count, head;
    uint32_t nextId;
} LrChronicle;

void LrChronicleInit(LrChronicle *log);
bool LrChronicleAppend(LrChronicle *log, const LrWorldEvent *event, uint32_t *outId);
const LrWorldEvent *LrChronicleFind(const LrChronicle *log, uint32_t id);
bool LrChronicleMarkHeard(LrChronicle *log, uint32_t id);

#endif
