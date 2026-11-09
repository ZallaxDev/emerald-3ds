#include <string.h>
#include "lr_snapshot.h"
#include "lr_save.h"
#include "lr_runtime_internal.h"

#define HEADER_BYTES 20u
#define RECORD_BYTES 841u
#define FIXED_BYTES 20045u
#define SNAPSHOT_MAGIC 0x3153524Cu

typedef struct { uint8_t *write; const uint8_t *read; size_t pos; } Cursor;

static uint64_t number(Cursor *s, uint64_t value, unsigned width) {
    uint64_t result = s->read ? 0 : value;
    for (unsigned i = 0; i < width; i++) {
        if (s->read) result |= (uint64_t)s->read[s->pos++] << (i * 8);
        else s->write[s->pos++] = (uint8_t)(value >> (i * 8));
    }
    return result;
}

static void raw(Cursor *s, void *data, size_t size) {
    if (s->read) memcpy(data,s->read+s->pos,size);
    else memcpy(s->write+s->pos,data,size);
    s->pos += size;
}

#define FIELD(value, width) ((value) = number(s,(value),(width)))
static void trainer(Cursor *s, TrainerJourneyCore *c, TrainerJourneyExtended *e,
                    LrTrainerTraits *t, LrTrainerRuntime *r) {
    raw(s,c,sizeof(*c)); raw(s,e,sizeof(*e));
    FIELD(t->aggression,1); FIELD(t->disposition,1); FIELD(t->devotion,1);
    FIELD(t->smarts,1); FIELD(t->wealth,1); FIELD(t->rank,1); FIELD(t->traitsRolled,1); FIELD(t->pastEvents,2);
    FIELD(r->active,1); FIELD(r->resources,1); FIELD(r->skill,1); FIELD(r->collector,1);
    FIELD(r->trainingPoints,4);
    FIELD(r->capturesPending,2); FIELD(r->evolutionsPending,2); FIELD(r->reorganizationsPending,2); FIELD(r->hostileTo,2);
    for (unsigned i=0;i<2;i++) FIELD(r->trackedGoalTypes[i],1);
    for (unsigned i=0;i<2;i++) FIELD(r->trackedGoalTargets[i],2);
    for (unsigned i=0;i<2;i++) FIELD(r->goalProgressMinute[i],4);
    for (unsigned i=0;i<LR_HISTORY_SIZE;i++) FIELD(r->historyEventIds[i],2);
    for (unsigned i=0;i<LR_HISTORY_SIZE;i++) FIELD(r->historyPhraseIds[i],2);
    for (unsigned i=0;i<LR_HISTORY_SIZE;i++) FIELD(r->historyMinutes[i],4);
    FIELD(r->historyCount,1); FIELD(r->historyHead,1);
}

static void event_record(Cursor *s, LrWorldEvent *e) {
    FIELD(e->id,4); FIELD(e->minute,4); FIELD(e->contentId,2); FIELD(e->titleId,2);
    for (unsigned i=0;i<4;i++) FIELD(e->lineIds[i],2);
    for (unsigned i=0;i<4;i++) FIELD(e->people[i],2);
    FIELD(e->kind,1); FIELD(e->region,1); FIELD(e->place,1);
    for (unsigned i=0;i<4;i++) FIELD(e->factions[i],1);
    for (unsigned i=0;i<4;i++) FIELD(e->outcomes[i],1);
    FIELD(e->heard,1);
}
#undef FIELD

static bool overlap(const void *a, size_t an, const void *b, size_t bn) {
    uintptr_t ap=(uintptr_t)a, bp=(uintptr_t)b;
    if (an > UINTPTR_MAX-ap || bn > UINTPTR_MAX-bp) return true;
    return an && bn && ap < bp+bn && bp < ap+an;
}

size_t LrSnapshotBytes(const LrWorldState *world) {
    return LrWorldValidate(world) ? FIXED_BYTES + world->trainerCount * RECORD_BYTES : 0;
}

bool LrSnapshotWrite(const LrWorldState *w, uint8_t *buffer, size_t capacity, size_t *written) {
    size_t length=LrSnapshotBytes(w);
    if (!length || !buffer || !written || capacity < length ||
        overlap(w,sizeof(*w),buffer,length) || overlap(w,sizeof(*w),written,sizeof(*written)) ||
        overlap(buffer,length,written,sizeof(*written))) return false;
    Cursor stream={buffer,NULL,0}, *s=&stream;
    number(s,SNAPSHOT_MAGIC,4); number(s,LR_WORLD_SCHEMA,2); number(s,w->trainerCount,2);
    number(s,w->contentVersion,4); number(s,length-HEADER_BYTES,4); number(s,0,4);
    number(s,w->nowMinute,4); number(s,w->rtcHighWater,8); number(s,w->rtcAnchored,1); number(s,w->rng.state,4);
    for (unsigned i=0;i<w->trainerCount;i++) {
        TrainerJourneyCore c=w->cores[i]; TrainerJourneyExtended e=w->exts[i];
        LrTrainerTraits t=w->traits[i]; LrTrainerRuntime r=w->runtime[i];
        trainer(s,&c,&e,&t,&r);
    }
    number(s,w->chronicle.count,2); number(s,w->chronicle.head,2); number(s,w->chronicle.nextId,4);
    for (unsigned i=0;i<LR_CHRONICLE_CAPACITY;i++) {
        LrWorldEvent e={0};
        if (i<w->chronicle.count) e=w->chronicle.events[i];
        event_record(s,&e);
    }
    Cursor crc={buffer,NULL,16};
    number(&crc,LrCrc32(buffer+HEADER_BYTES,length-HEADER_BYTES),4);
    *written=length;
    return true;
}

/* First pass validates every field using only one trainer/event of scratch space. */
static bool decode(LrWorldState *dest, const uint8_t *buffer, size_t length, uint32_t contentVersion) {
    Cursor stream={NULL,buffer,0}, *s=&stream;
    if (length<HEADER_BYTES || number(s,0,4)!=SNAPSHOT_MAGIC || number(s,0,2)!=LR_WORLD_SCHEMA) return false;
    uint16_t count=(uint16_t)number(s,0,2);
    if (count>LR_MAX_TRAINERS || number(s,0,4)!=contentVersion ||
        number(s,0,4)!=length-HEADER_BYTES || length!=FIXED_BYTES+count*RECORD_BYTES ||
        number(s,0,4)!=LrCrc32(buffer+HEADER_BYTES,length-HEADER_BYTES)) return false;
    uint32_t minute=(uint32_t)number(s,0,4);
    uint64_t rtc=number(s,0,8);
    uint8_t anchored=(uint8_t)number(s,0,1);
    uint32_t rng=(uint32_t)number(s,0,4);
    if (minute>LR_MAX_MINUTE || anchored>1 || (!anchored && rtc)) return false;
    if (dest) {
        memset(dest,0,sizeof(*dest)); dest->schemaVersion=LR_WORLD_SCHEMA; dest->trainerCount=count;
        dest->contentVersion=contentVersion; dest->nowMinute=minute; dest->rtcHighWater=rtc;
        dest->rtcAnchored=anchored; dest->rng.state=rng;
    }
    uint16_t previous=0;
    for (unsigned i=0;i<count;i++) {
        TrainerJourneyCore c={0}; TrainerJourneyExtended e={0};
        LrTrainerTraits t={0}; LrTrainerRuntime r={0};
        trainer(s,&c,&e,&t,&r);
        if ((i && previous>=c.trainerId) || !LrTrainerStateValidate(&c,&e,&t,&r,minute)) return false;
        previous=c.trainerId;
        if (dest) { dest->cores[i]=c; dest->exts[i]=e; dest->traits[i]=t; dest->runtime[i]=r; }
    }
    uint16_t eventCount=(uint16_t)number(s,0,2), head=(uint16_t)number(s,0,2);
    uint32_t nextId=(uint32_t)number(s,0,4);
    if (eventCount>LR_CHRONICLE_CAPACITY || head>=LR_CHRONICLE_CAPACITY || nextId<=eventCount ||
        (eventCount<LR_CHRONICLE_CAPACITY && head!=eventCount)) return false;
    if (dest) { dest->chronicle.count=eventCount; dest->chronicle.head=head; dest->chronicle.nextId=nextId; }
    for (unsigned i=0;i<LR_CHRONICLE_CAPACITY;i++) {
        size_t start=s->pos;
        LrWorldEvent e={0}; event_record(s,&e);
        if (i<eventCount) {
            unsigned offset=eventCount==LR_CHRONICLE_CAPACITY ? (i+LR_CHRONICLE_CAPACITY-head)%LR_CHRONICLE_CAPACITY : i;
            if (!LrEventStateValidate(&e,minute) || e.id!=nextId-eventCount+offset) return false;
        } else {
            for (size_t n=start;n<s->pos;n++) if (buffer[n]) return false;
        }
        if (dest) dest->chronicle.events[i]=e;
    }
    return s->pos==length;
}

bool LrSnapshotRestore(LrWorldState *world, const uint8_t *buffer, size_t length, uint32_t contentVersion) {
    if (!world || !buffer || overlap(world,sizeof(*world),buffer,length) ||
        !decode(NULL,buffer,length,contentVersion)) return false;
    return decode(world,buffer,length,contentVersion);
}
