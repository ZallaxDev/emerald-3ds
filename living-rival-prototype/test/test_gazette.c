#include "lr_test.h"
#include "lr_fixtures.h"
#include "lr_gazette.h"
#include "lr_simulation.h"
#include "lr_snapshot.h"

static LrWorldState world, other, before;
static uint8_t bytes[90000], second[90000];

static uint32_t append(uint8_t kind,uint8_t place,uint32_t minute,uint16_t person) {
    LrWorldEvent event={0}; event.kind=kind; event.place=place; event.minute=minute;
    event.region=LR_REGION_HOENN; event.people[0]=person;
    for (unsigned i=1;i<4;i++) event.people[i]=LR_GOAL_TARGET_NONE;
    uint32_t id=0; LR_CHECK(LrChronicleAppend(&world.chronicle,&event,&id)); return id;
}

static void test_filters_age_score_group(void) {
    LR_CHECK(test_world_init(&world,2,23,10000));
    append(LR_EVENT_LIFE,2,10000,1003); append(LR_EVENT_LIFE,1,10000,1003);
    append(LR_EVENT_BADGE,2,10000,1000); append(LR_EVENT_DEATH,1,10000,1003);
    append(LR_EVENT_RETIREMENT,2,10000,1003); append(LR_EVENT_CLASH,1,10000,1003);
    LrGazetteFilter filter={255,255,LR_GOAL_TARGET_NONE}; LrWorldEvent out[5];
    LR_CHECK_EQ_INT(LrGazetteQuery(&world,&filter,255,out),5);
    const uint32_t ids[]={4,6,3,5,1};
    for (unsigned i=0;i<5;i++) LR_CHECK(out[i].id==ids[i]);
    LR_CHECK_EQ_INT(LrGazetteQuery(&world,&filter,1,out),1); LR_CHECK_EQ_INT(out[0].id,4);
    filter.trainerId=1000;
    LR_CHECK_EQ_INT(LrGazetteQuery(&world,&filter,5,out),1); LR_CHECK_EQ_INT(out[0].id,3);
    filter.trainerId=LR_GOAL_TARGET_NONE; filter.place=1;
    LR_CHECK_EQ_INT(LrGazetteQuery(&world,&filter,5,out),3);
    filter.place=255; filter.region=LR_REGION_JOHTO;
    LR_CHECK_EQ_INT(LrGazetteQuery(&world,&filter,5,out),0);
    LR_CHECK(test_world_init(&world,1,23,10000));
    append(LR_EVENT_LIFE,0,2800,1000); append(LR_EVENT_LIFE,0,2799,1000);
    append(LR_EVENT_LIFE,0,10001,1000);
    filter.region=255;
    LR_CHECK_EQ_INT(LrGazetteQuery(&world,&filter,5,out),1); LR_CHECK_EQ_INT(out[0].id,1);
    filter.region=200; out[0].id=123;
    LR_CHECK_EQ_INT(LrGazetteQuery(&world,&filter,5,out),0); LR_CHECK_EQ_INT(out[0].id,123);
}

static void test_heard_and_overwrite(void) {
    LR_CHECK(test_world_init(&world,1,23,10000));
    for (unsigned i=0;i<501;i++) append(LR_EVENT_LIFE,0,10000,1000);
    before=world;
    LrGazetteFilter filter={255,255,LR_GOAL_TARGET_NONE}; LrWorldEvent out[5];
    LR_CHECK_EQ_INT(LrGazetteQuery(&world,&filter,5,out),5);
    LR_CHECK(memcmp(&world,&before,sizeof(world))==0);
    LR_CHECK(!LrChronicleMarkHeard(&world.chronicle,1));
    LR_CHECK(LrChronicleMarkHeard(&world.chronicle,out[0].id));
    uint32_t heard=out[0].id;
    LR_CHECK_EQ_INT(LrGazetteQuery(&world,&filter,5,out),5);
    for (unsigned i=0;i<5;i++) LR_CHECK(out[i].id!=heard);
}

static const char *title_lookup(void *context,uint16_t id) {
    (void)context;
    static const char *titles[]={NULL,
        "{trainer} trained near {place}.","{trainer} refined a specialization.",
        "{trainer} found a new purpose.","{trainer} aims to rise.",
        "{trainer} earned a badge!","{trainer} explored Johto.",
        "{trainer} planned a new capture.","{trainer} prepared an evolution.",
        "{trainer} reorganized a team.","{trainer} took a break.",
        "{trainer} rebuilt their resources.","{trainer} retired.",
        "100% {trainer} {place} {unknown}"};
    return id<sizeof(titles)/sizeof(titles[0]) ? titles[id] : NULL;
}
static const char *trainer_lookup(void *context,uint16_t id) {
    (void)context;
    static const char *names[]={"Wally","May","Roxanne","Brawly","Flannery",
        "Norman","Winona","Juan","Steven","Scott"};
    if (id>=1000 && id<=1027 && (id-1000)%3==0) return names[(id-1000)/3];
    if (id==2000) return "{place}";
    return NULL;
}
static const char *place_lookup(void *context,uint16_t id) {
    (void)context; return id==0 ? "an unknown town" : NULL;
}

static void test_format_bounds(void) {
    LrWorldEvent event={0}; event.titleId=13; event.people[0]=1000;
    char out[100];
    LR_CHECK(LrGazetteFormat(&event,title_lookup,trainer_lookup,place_lookup,NULL,out,sizeof(out)));
    LR_CHECK(strcmp(out,"100% Wally an unknown town {unknown}")==0);
    size_t exact=strlen(out)+1;
    LR_CHECK(LrGazetteFormat(&event,title_lookup,trainer_lookup,place_lookup,NULL,out,exact));
    LR_CHECK(!LrGazetteFormat(&event,title_lookup,trainer_lookup,place_lookup,NULL,out,exact-1));
    LR_CHECK_EQ_INT(out[exact-2],0);
    out[0]='X'; LR_CHECK(!LrGazetteFormat(&event,NULL,NULL,NULL,NULL,out,0)); LR_CHECK_EQ_INT(out[0],'X');
    LR_CHECK(!LrGazetteFormat(&event,NULL,NULL,NULL,NULL,out,1)); LR_CHECK_EQ_INT(out[0],0);
    event.titleId=1; event.people[0]=2000;
    LR_CHECK(LrGazetteFormat(&event,title_lookup,trainer_lookup,place_lookup,NULL,out,sizeof(out)));
    LR_CHECK(strcmp(out,"{place} trained near an unknown town.")==0);
    LR_CHECK(LrGazetteFormat(&event,title_lookup,NULL,NULL,NULL,out,sizeof(out)));
    LR_CHECK(strcmp(out,"Unknown trainer trained near Unknown place.")==0);
    event.titleId=99;
    LR_CHECK(LrGazetteFormat(&event,title_lookup,NULL,NULL,NULL,out,sizeof(out)));
    LR_CHECK(strcmp(out,"An event occurred.")==0);
}

static void test_thirty_day_demo(void) {
    LR_CHECK(test_world_init(&world,10,23,0));
    for (unsigned i=0;i<10;i++) {
        world.runtime[i].collector=(uint8_t)(i*10); world.runtime[i].skill=(uint8_t)(20+i*8);
        world.cores[i].universalFacets[LR_FACET_AMBITION]=(int8_t)(20+i*8);
    }
    LrEventCatalog catalog=LrPrototypeEventCatalog(); catalog.count=11;
    LrAdvanceStats stats;
    for (unsigned day=1;day<=15;day++) LR_CHECK(LrSimulateTo(&world,&catalog,day*1440,&stats));
    size_t length=0, len2=0;
    LR_CHECK(LrSnapshotWrite(&world,bytes,sizeof(bytes),&length));
    LR_CHECK(LrSnapshotRestore(&other,bytes,length,1));
    for (unsigned day=16;day<=30;day++) {
        LR_CHECK(LrSimulateTo(&world,&catalog,day*1440,&stats));
        LR_CHECK(LrSimulateTo(&other,&catalog,day*1440,&stats));
    }
    LR_CHECK(LrSnapshotWrite(&world,bytes,sizeof(bytes),&length));
    LR_CHECK(LrSnapshotWrite(&other,second,sizeof(second),&len2));
    LR_CHECK(length==len2 && memcmp(bytes,second,length)==0);
    LrGazetteFilter filter={255,255,LR_GOAL_TARGET_NONE}; LrWorldEvent out[5];
    uint8_t count=LrGazetteQuery(&world,&filter,5,out);
    LR_CHECK(count>0 && count<=5);
    puts("Gazette sample (representative content; team changes remain pending):");
    for (unsigned i=0;i<count;i++) {
        char text[160];
        LR_CHECK(LrGazetteFormat(&out[i],title_lookup,trainer_lookup,place_lookup,NULL,text,sizeof(text)));
        printf("  %s\n",text);
    }
    LR_CHECK(test_world_init(&world,2,23,0));
    world.runtime[0].skill=80; world.runtime[0].resources=80; world.runtime[0].collector=0;
    world.cores[0].universalFacets[LR_FACET_AMBITION]=90;
    world.cores[0].universalFacets[LR_FACET_DISCIPLINE]=90;
    world.runtime[1].collector=80; world.cores[1].universalFacets[LR_FACET_AMBITION]=30;
    LR_CHECK(LrAnchorOfflineClock(&world,1000)); other=world;
    LR_CHECK(LrResumeOffline(&world,&catalog,1000+180u*1440u,&stats));
    LR_CHECK(LrResumeOffline(&other,&catalog,1000+180u*1440u,&stats));
    LR_CHECK(memcmp(&world,&other,sizeof(world))==0);
    LR_CHECK_EQ_INT(stats.hourlyTicks,48);
    for (unsigned i=0;i<2;i++) {
        LR_CHECK(world.cores[i].badgesEarned<=8);
        printf("  %s after six months: training=%u badges=%u region=%u pending captures=%u (not a generated party)\n",
            i ? "Collector" : "Competitor",(unsigned)world.runtime[i].trainingPoints,
            world.cores[i].badgesEarned,world.cores[i].currentRegion,world.runtime[i].capturesPending);
    }
}

int main(void) {
    test_filters_age_score_group(); test_heard_and_overwrite(); test_format_bounds(); test_thirty_day_demo();
    return lr_test_summary("test_gazette");
}
