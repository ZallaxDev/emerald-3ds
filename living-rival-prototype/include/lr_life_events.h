#ifndef LR_LIFE_EVENTS_H
#define LR_LIFE_EVENTS_H
#include "lr_runtime.h"

#define LR_MAX_LIFE_EVENTS 270
#define LR_CATEGORY_ANY 255
typedef enum { LR_CATEGORY_TRAINING, LR_CATEGORY_PERSONAL, LR_CATEGORY_ACTIVITY,
               LR_CATEGORY_TEAM, LR_CATEGORY_LIFE, LR_CATEGORY_COUNT } LrEventCategory;
typedef enum { LR_COND_BADGES, LR_COND_PHASE, LR_COND_AMBITION, LR_COND_SOCIAL,
               LR_COND_RESOURCES, LR_COND_SKILL, LR_COND_COLLECTOR, LR_COND_STRESS,
               LR_COND_COUNT } LrConditionField;
typedef enum { LR_OP_GE, LR_OP_LE, LR_OP_EQ } LrConditionOp;
typedef enum { LR_FX_NEED, LR_FX_RESOURCE, LR_FX_TRAINING, LR_FX_CAPTURE_INTENT,
               LR_FX_EVOLVE_INTENT, LR_FX_REORGANIZE_INTENT, LR_FX_HATE, LR_FX_FEAR,
               LR_FX_GOAL, LR_FX_REGION, LR_FX_STRESS, LR_FX_BADGE, LR_FX_PHASE,
               LR_FX_COUNT } LrEffectKind;
typedef struct { uint8_t field, op; int32_t value; } LrEventCondition;
typedef struct { uint8_t kind, index; int16_t amount; } LrEventEffect;
typedef struct {
    uint16_t id, titleId, phraseId, odds;
    uint8_t category, pool, kind, place, conditionCount, effectCount;
    LrEventCondition conditions[4];
    LrEventEffect effects[4];
} LrLifeEvent;
typedef struct { const LrLifeEvent *events; uint16_t count; uint32_t version; } LrEventCatalog;

bool LrCatalogValidate(const LrEventCatalog *catalog);
bool LrEventFits(const LrWorldState *world, uint16_t slot, const LrLifeEvent *event);
uint16_t LrWeightedIndex(const uint16_t *weights, uint16_t count, uint32_t roll);
const LrLifeEvent *LrPickLifeEvent(LrWorldState *world, uint16_t slot,
                                  const LrEventCatalog *catalog, uint32_t minute, uint8_t category);
bool LrApplyLifeEvent(LrWorldState *world, uint16_t slot, const LrLifeEvent *event,
                      uint32_t minute, uint8_t scaleDays);
LrEventCatalog LrPrototypeEventCatalog(void);
#endif
