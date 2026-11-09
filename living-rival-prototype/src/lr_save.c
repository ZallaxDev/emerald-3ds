#include <string.h>
#include "lr_save.h"

static void write_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void write_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

size_t LrSaveRecordBytes(void) {
    return sizeof(TrainerJourneyCore) + sizeof(TrainerJourneyExtended);
}

uint32_t LrCrc32(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 1u) crc = (crc >> 1) ^ 0xEDB88320u;
            else          crc = crc >> 1;
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

bool LrSaveEncode(const TrainerJourneyCore *cores, const TrainerJourneyExtended *exts,
                  uint16_t count, uint8_t *out, size_t cap, size_t *outLen) {
    if (!out || !outLen) return false;
    if (count > 0 && (!cores || !exts)) return false;

    const size_t record = LrSaveRecordBytes();
    const size_t payload = (size_t)count * record;
    const size_t total = LR_SAVE_HEADER_BYTES + payload;
    if (cap < total) return false;

    write_u32(out + 0, LR_SAVE_MAGIC);
    write_u16(out + 4, LR_SAVE_VERSION);
    write_u16(out + 6, count);
    write_u32(out + 8, (uint32_t)payload);

    for (uint16_t i = 0; i < count; i++) {
        uint8_t *dst = out + LR_SAVE_HEADER_BYTES + (size_t)i * record;
        memcpy(dst, &cores[i], sizeof(TrainerJourneyCore));
        memcpy(dst + sizeof(TrainerJourneyCore), &exts[i], sizeof(TrainerJourneyExtended));
    }

    write_u32(out + 12, LrCrc32(out + LR_SAVE_HEADER_BYTES, payload));
    *outLen = total;
    return true;
}

bool LrSaveDecode(const uint8_t *in, size_t len,
                  TrainerJourneyCore *cores, TrainerJourneyExtended *exts,
                  uint16_t maxCount, uint16_t *outCount) {
    if (!in || !cores || !exts || !outCount) return false;
    if (len < LR_SAVE_HEADER_BYTES) return false;

    if (read_u32(in + 0) != LR_SAVE_MAGIC) return false;
    if (read_u16(in + 4) != LR_SAVE_VERSION) return false;

    const uint16_t count = read_u16(in + 6);
    const uint32_t payload = read_u32(in + 8);
    const size_t record = LrSaveRecordBytes();

    if (count > maxCount) return false;
    if (payload != (uint32_t)(len - LR_SAVE_HEADER_BYTES)) return false;
    if (payload != (uint32_t)((size_t)count * record)) return false;
    if (LrCrc32(in + LR_SAVE_HEADER_BYTES, payload) != read_u32(in + 12)) return false;

    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *src = in + LR_SAVE_HEADER_BYTES + (size_t)i * record;
        memcpy(&cores[i], src, sizeof(TrainerJourneyCore));
        memcpy(&exts[i], src + sizeof(TrainerJourneyCore), sizeof(TrainerJourneyExtended));
    }

    *outCount = count;
    return true;
}
