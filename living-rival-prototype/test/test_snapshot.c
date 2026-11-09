#include "lr_test.h"
#include "lr_fixtures.h"
#include "lr_snapshot.h"
#include "lr_simulation.h"
#include "lr_daily.h"
#include "lr_save.h"

static LrWorldState world, restored, before;
static uint8_t bytes[90000], second[90000], corrupt[90000];
static size_t length;

static void fix_crc(uint8_t *data, size_t size) {
    uint32_t crc=LrCrc32(data+20,size-20);
    for (unsigned i=0;i<4;i++) data[16+i]=(uint8_t)(crc>>(8*i));
}

static void test_complete_roundtrip(void) {
    LR_CHECK(test_world_init(&world,3,23,1000));
    LR_CHECK(LrSetTrackedGoal(&world,0,LR_GOAL_RICH,LR_GOAL_TARGET_NONE,1000));
    world.runtime[0].capturesPending=17; world.runtime[1].evolutionsPending=3;
    world.runtime[0].historyCount=world.runtime[0].historyHead=1;
    world.runtime[0].historyMinutes[0]=999; world.runtime[0].historyEventIds[0]=4;
    world.runtime[0].historyPhraseIds[0]=9;
    LR_CHECK(LrAnchorOfflineClock(&world,500000));
    LrWorldEvent event={0}; event.people[0]=1000; event.minute=1000;
    for (unsigned i=0;i<501;i++) LR_CHECK(LrChronicleAppend(&world.chronicle,&event,NULL));
    LR_CHECK(LrChronicleMarkHeard(&world.chronicle,501));
    LR_CHECK_EQ_INT(LrSnapshotBytes(&world),20045+3*841);
    LR_CHECK(LrSnapshotWrite(&world,bytes,sizeof(bytes),&length));
    LR_CHECK_EQ_INT(length,22568);
    LR_CHECK(LrSnapshotRestore(&restored,bytes,length,1));
    LR_CHECK(LrWorldValidate(&restored));
    size_t len2=0;
    LR_CHECK(LrSnapshotWrite(&restored,second,sizeof(second),&len2));
    LR_CHECK(len2==length && memcmp(bytes,second,length)==0);
    LrEventCatalog catalog=LrPrototypeEventCatalog(); LrAdvanceStats stats;
    LR_CHECK(LrSimulateTo(&world,&catalog,10000,&stats));
    LR_CHECK(LrSimulateTo(&restored,&catalog,10000,&stats));
    LR_CHECK(LrSnapshotWrite(&world,bytes,sizeof(bytes),&length));
    LR_CHECK(LrSnapshotWrite(&restored,second,sizeof(second),&len2));
    LR_CHECK(len2==length && memcmp(bytes,second,length)==0);
}

static void test_rejected_restore(void) {
    before=restored;
    LR_CHECK(!LrSnapshotRestore(&restored,bytes,19,1));
    LR_CHECK(!LrSnapshotRestore(&restored,bytes,length-1,1));
    LR_CHECK(!LrSnapshotRestore(&restored,bytes,length+1,1));
    LR_CHECK(!LrSnapshotRestore(&restored,bytes,length,2));
    const unsigned offsets[]={0,4,6,8,12,16,25};
    for (unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++) {
        memcpy(corrupt,bytes,length); corrupt[offsets[i]]^=0x80;
        LR_CHECK(!LrSnapshotRestore(&restored,corrupt,length,1));
        LR_CHECK(memcmp(&restored,&before,sizeof(before))==0);
    }
    memcpy(corrupt,bytes,length); corrupt[length-1]=2; fix_crc(corrupt,length);
    LR_CHECK(!LrSnapshotRestore(&restored,corrupt,length,1));
    LR_CHECK(memcmp(&restored,&before,sizeof(before))==0);
    memcpy(corrupt,bytes,length); corrupt[876]=61; fix_crc(corrupt,length);
    LR_CHECK(!LrSnapshotRestore(&restored,corrupt,length,1));
    LR_CHECK(memcmp(&restored,&before,sizeof(before))==0);
}

static void test_rejected_write(void) {
    size_t written=123;
    memset(corrupt,0xAA,sizeof(corrupt));
    LR_CHECK(!LrSnapshotWrite(&world,corrupt,length-1,&written));
    LR_CHECK_EQ_INT(written,123);
    LR_CHECK_EQ_INT(corrupt[0],0xAA); LR_CHECK_EQ_INT(corrupt[length-1],0xAA);
    LR_CHECK(!LrSnapshotWrite(&world,NULL,sizeof(corrupt),&written));
    LR_CHECK(!LrSnapshotWrite(NULL,corrupt,sizeof(corrupt),&written));
    LR_CHECK(!LrSnapshotWrite(&world,corrupt,sizeof(corrupt),NULL));
    before=world;
    LR_CHECK(!LrSnapshotWrite(&world,(uint8_t *)&world,sizeof(world),&written));
    LR_CHECK(memcmp(&world,&before,sizeof(world))==0);
}

static void test_unused_tail(void) {
    LR_CHECK(test_world_init(&world,0,23,0));
    LR_CHECK(LrSnapshotWrite(&world,bytes,sizeof(bytes),&length));
    LR_CHECK_EQ_INT(length,20045);
    memcpy(corrupt,bytes,length); corrupt[length-2]=1; fix_crc(corrupt,length);
    before=restored;
    LR_CHECK(!LrSnapshotRestore(&restored,corrupt,length,1));
    LR_CHECK(memcmp(&restored,&before,sizeof(before))==0);
}

int main(void) {
    test_complete_roundtrip(); test_rejected_restore(); test_rejected_write(); test_unused_tail();
    return lr_test_summary("test_snapshot");
}
