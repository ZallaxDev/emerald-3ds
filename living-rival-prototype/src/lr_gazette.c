#include <string.h>
#include "lr_gazette.h"

static unsigned score(const LrWorldState *w, const LrWorldEvent *e) {
    static const unsigned weights[LR_EVENT_KIND_COUNT]={10,30,60,50,100};
    unsigned fame=0;
    for (unsigned i=0;i<4;i++) {
        int32_t slot=LrFindTrainer(w,e->people[i]);
        if (slot>=0 && w->cores[slot].fameLevel/16u>fame) fame=w->cores[slot].fameLevel/16u;
    }
    return weights[e->kind]*(6-(w->nowMinute-e->minute)/1440)+fame;
}

static bool better(const LrWorldState *w, const LrWorldEvent *a, const LrWorldEvent *b) {
    unsigned as=score(w,a), bs=score(w,b);
    if (as!=bs) return as>bs;
    if (a->minute!=b->minute) return a->minute>b->minute;
    return a->id<b->id;
}

uint8_t LrGazetteQuery(const LrWorldState *w, const LrGazetteFilter *filter,
                       uint8_t maxCount, LrWorldEvent *out) {
    if (!w || !filter || !out || !maxCount || w->trainerCount>LR_MAX_TRAINERS ||
        w->chronicle.count>LR_CHRONICLE_CAPACITY ||
        (filter->region!=255 && filter->region>=LR_REGION_COUNT)) return 0;
    unsigned limit=maxCount>5 ? 5 : maxCount, count=0;
    const LrWorldEvent *selected[5];
    for (unsigned i=0;i<w->chronicle.count;i++) {
        const LrWorldEvent *e=&w->chronicle.events[i];
        if (!e->id || e->heard || e->kind>=LR_EVENT_KIND_COUNT || e->minute>w->nowMinute ||
            w->nowMinute-e->minute>7200 || (filter->region!=255 && e->region!=filter->region) ||
            (filter->place!=255 && e->place!=filter->place)) continue;
        if (filter->trainerId!=LR_GOAL_TARGET_NONE) {
            bool found=false;
            for (unsigned p=0;p<4;p++) if (e->people[p]==filter->trainerId) found=true;
            if (!found) continue;
        }
        unsigned pos=0;
        while (pos<count && !better(w,e,selected[pos])) pos++;
        if (pos>=limit) continue;
        if (count<limit) count++;
        for (unsigned n=count-1;n>pos;n--) selected[n]=selected[n-1];
        selected[pos]=e;
    }
    /* Group only the selected news, preserving rank within each town. */
    for (unsigned i=1;i<count;i++) {
        const LrWorldEvent *e=selected[i]; unsigned pos=i;
        while (pos && selected[pos-1]->place>e->place) { selected[pos]=selected[pos-1]; pos--; }
        selected[pos]=e;
    }
    for (unsigned i=0;i<count;i++) out[i]=*selected[i];
    return (uint8_t)count;
}

static void append_text(const char *text, char *out, size_t capacity, size_t *used, bool *fits) {
    for (;*text;text++) {
        if (*used<capacity-1) out[(*used)++]=*text;
        else *fits=false;
    }
}

bool LrGazetteFormat(const LrWorldEvent *event, LrTextLookup title, LrTextLookup trainer,
                     LrTextLookup place, void *context, char *out, size_t capacity) {
    if (!event || !out || !capacity) return false;
    const char *text=title ? title(context,event->titleId) : NULL;
    if (!text) text="An event occurred.";
    size_t used=0; bool fits=true;
    while (*text) {
        if (strncmp(text,"{trainer}",9)==0) {
            const char *name=trainer ? trainer(context,event->people[0]) : NULL;
            append_text(name ? name : "Unknown trainer",out,capacity,&used,&fits); text+=9;
        } else if (strncmp(text,"{place}",7)==0) {
            const char *name=place ? place(context,event->place) : NULL;
            append_text(name ? name : "Unknown place",out,capacity,&used,&fits); text+=7;
        } else {
            char literal[2]={*text++,0}; append_text(literal,out,capacity,&used,&fits);
        }
    }
    out[used]=0;
    return fits;
}
