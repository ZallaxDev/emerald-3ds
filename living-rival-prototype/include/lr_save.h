#ifndef LR_SAVE_H
#define LR_SAVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lr_types.h"

#define LR_SAVE_MAGIC   0x314A524Cu   /* "LRJ1" in little-endian bytes */
#define LR_SAVE_VERSION 1u
#define LR_SAVE_HEADER_BYTES 16u

size_t LrSaveRecordBytes(void);
uint32_t LrCrc32(const void *data, size_t len);

bool LrSaveEncode(const TrainerJourneyCore *cores, const TrainerJourneyExtended *exts,
                  uint16_t count, uint8_t *out, size_t cap, size_t *outLen);
bool LrSaveDecode(const uint8_t *in, size_t len,
                  TrainerJourneyCore *cores, TrainerJourneyExtended *exts,
                  uint16_t maxCount, uint16_t *outCount);

#endif
