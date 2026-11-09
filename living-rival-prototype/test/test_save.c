#include <string.h>
#include "lr_test.h"
#include "lr_types.h"
#include "lr_save.h"

#define REC 320
#define BLOB (16 + 3 * REC)

static void fill(TrainerJourneyCore *c, TrainerJourneyExtended *e, uint16_t id) {
    memset(c, 0, sizeof(*c));
    memset(e, 0, sizeof(*e));
    c->trainerId = id;
    c->temperament = LR_TEMP_HOTHEAD;
    c->personalSeed = 0xDEADBEEFu + id;
    c->universalFacets[0] = 77;
    e->memoryCount = 3;
    e->memoryEmotions[0] = 1;
    e->memoryTimestamps[0] = 1234;
    e->historyTextIds[0] = 9;
    e->memoryTargetFacets[0] = 7;
}

int main(void) {
    LR_CHECK_EQ_INT(LrSaveRecordBytes(), REC);

    /* CRC32 known vector. */
    LR_CHECK(LrCrc32("123456789", 9) == 0xCBF43926u);

    /* Round-trip three trainers. */
    TrainerJourneyCore cores[3], dcores[3];
    TrainerJourneyExtended exts[3], dexts[3];
    for (int i = 0; i < 3; i++) fill(&cores[i], &exts[i], (uint16_t)(i + 1));
    cores[0].trainerId = 0x1234;

    uint8_t blob[BLOB];
    size_t len = 0;
    LR_CHECK(LrSaveEncode(cores, exts, 3, blob, sizeof(blob), &len));
    LR_CHECK_EQ_INT(len, BLOB);

    uint16_t count = 0;
    LR_CHECK(LrSaveDecode(blob, len, dcores, dexts, 3, &count));
    LR_CHECK_EQ_INT(count, 3);
    LR_CHECK(memcmp(cores, dcores, sizeof(cores)) == 0);
    LR_CHECK(memcmp(exts, dexts, sizeof(exts)) == 0);

    /* Header and record bodies are laid out little-endian. */
    LR_CHECK_EQ_INT(blob[0], 0x4C);
    LR_CHECK_EQ_INT(blob[1], 0x52);
    LR_CHECK_EQ_INT(blob[2], 0x4A);
    LR_CHECK_EQ_INT(blob[3], 0x31);
    LR_CHECK_EQ_INT(blob[4], 1);
    LR_CHECK_EQ_INT(blob[5], 0);
    LR_CHECK_EQ_INT(blob[6], 3);
    LR_CHECK_EQ_INT(blob[7], 0);
    LR_CHECK_EQ_INT(blob[8], 0xC0);   /* payload 960 = 0x000003C0 */
    LR_CHECK_EQ_INT(blob[9], 0x03);
    LR_CHECK_EQ_INT(blob[10], 0);
    LR_CHECK_EQ_INT(blob[11], 0);
    LR_CHECK_EQ_INT(blob[16], 0x34);        /* trainerId 0x1234, core offset 0 */
    LR_CHECK_EQ_INT(blob[17], 0x12);
    LR_CHECK_EQ_INT(blob[16 + 87 + 28], 3); /* extended.memoryCount */
    LR_CHECK_EQ_INT(blob[16 + 87 + 221], 7);/* extended.memoryTargetFacets[0] */

    /* Corrupt payload -> false, destinations and outCount untouched. */
    uint8_t bad[sizeof(blob)];
    memcpy(bad, blob, sizeof(blob));
    bad[16 + 20] ^= 0xFF;
    memset(dcores, 0xAA, sizeof(dcores));
    memset(dexts, 0xAA, sizeof(dexts));
    count = 0x7777;
    LR_CHECK(!LrSaveDecode(bad, len, dcores, dexts, 3, &count));
    LR_CHECK_EQ_INT(count, 0x7777);
    for (size_t i = 0; i < sizeof(dcores); i++) LR_CHECK(((uint8_t *)dcores)[i] == 0xAA);
    for (size_t i = 0; i < sizeof(dexts); i++) LR_CHECK(((uint8_t *)dexts)[i] == 0xAA);

    /* Truncation -> false. */
    LR_CHECK(!LrSaveDecode(blob, 15, dcores, dexts, 3, &count));
    LR_CHECK(!LrSaveDecode(blob, len - 1, dcores, dexts, 3, &count));

    /* Wrong magic -> false, destinations and outCount untouched. */
    memset(dcores, 0x55, sizeof(dcores));
    memset(dexts, 0x55, sizeof(dexts));
    memcpy(bad, blob, sizeof(blob));
    bad[0] ^= 0xFF;
    count = 1234;
    LR_CHECK(!LrSaveDecode(bad, len, dcores, dexts, 3, &count));
    LR_CHECK_EQ_INT(count, 1234);
    for (size_t i = 0; i < sizeof(dexts); i++) LR_CHECK(((uint8_t *)dexts)[i] == 0x55);

    /* Wrong version / count -> false. */
    memcpy(bad, blob, sizeof(blob));
    bad[4] = 2;
    LR_CHECK(!LrSaveDecode(bad, len, dcores, dexts, 3, &count));
    memcpy(bad, blob, sizeof(blob));
    bad[6] = 4;   /* count 4 but only 3 records present */
    LR_CHECK(!LrSaveDecode(bad, len, dcores, dexts, 3, &count));

    /* Capacity checks. */
    LR_CHECK(!LrSaveEncode(cores, exts, 3, blob, BLOB - 1, &len));
    LR_CHECK(!LrSaveDecode(blob, BLOB, dcores, dexts, 2, &count));

    /* An empty blob round-trips as 16 bytes and zero records. */
    size_t len0 = 0;
    LR_CHECK(LrSaveEncode(cores, exts, 0, blob, sizeof(blob), &len0));
    LR_CHECK_EQ_INT(len0, 16);
    uint16_t c0 = 99;
    LR_CHECK(LrSaveDecode(blob, len0, dcores, dexts, 3, &c0));
    LR_CHECK_EQ_INT(c0, 0);

    /* NULL arrays with a non-zero count are rejected, not dereferenced. */
    LR_CHECK(!LrSaveEncode(0, 0, 1, blob, sizeof(blob), &len));

    return lr_test_summary("test_save");
}
