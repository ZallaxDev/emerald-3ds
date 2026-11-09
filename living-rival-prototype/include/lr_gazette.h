#ifndef LR_GAZETTE_H
#define LR_GAZETTE_H
#include <stddef.h>
#include "lr_runtime.h"
typedef struct { uint8_t region, place; uint16_t trainerId; } LrGazetteFilter;
typedef const char *(*LrTextLookup)(void *context, uint16_t id);

uint8_t LrGazetteQuery(const LrWorldState *world, const LrGazetteFilter *filter,
                       uint8_t maxCount, LrWorldEvent *out);
bool LrGazetteFormat(const LrWorldEvent *event, LrTextLookup title, LrTextLookup trainer,
                     LrTextLookup place, void *context, char *out, size_t capacity);
#endif
