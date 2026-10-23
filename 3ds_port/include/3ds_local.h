#ifndef CTR_LOCAL_H
#define CTR_LOCAL_H

/*
 * Local wireless play (3ds_local.c): other 3DS systems nearby, through the
 * system's local wireless (the UDS service). It is the radio of the emulated
 * Wireless Adapter (3ds_rfu_adapter.c), so the game's own wireless features
 * (the Union Room, the Direct Corner, the minigames...) find each other.
 *
 * Players meet in a session: at a link counter the game joins the session of
 * that counter a nearby system is hosting, or hosts one itself. Everyone in a
 * session sees everyone else, as Wireless Adapters in range do.
 *
 * Native, but the adapter and the counters' session script
 * (src/local_session.c) are game units: only fixed-size types here.
 */
#include <stdint.h>

#define LOCAL_MAX_PEERS 8    /* a session holds 5; room to spare */
#define LOCAL_ADVERT_SIZE 24 /* a Wireless Adapter's broadcast data */

struct LocalPeer
{
    uint8_t valid;
    uint8_t hasAdvert;
    uint8_t hosting;
    uint8_t nextSlot;
    uint16_t id;
    uint8_t advert[LOCAL_ADVERT_SIZE];
    uint32_t lastHostingMs; /* when it was last hosting, 0 if never */
    uint32_t hostedAgoMs;   /* filled in by LocalLink_GetPeers: 0 if hosting now */
};

enum
{
    LOCAL_EV_RECV, /* data from peer `id` */
    LOCAL_EV_LEFT, /* peer `id` left the session (or we did) */
};

struct LocalEvent
{
    uint8_t type;
    uint16_t id;
    uint16_t len;
    uint8_t data[160];
};

/* At start and exit. Nothing uses the wireless until a session starts. */
void LocalLink_Init(void);
void LocalLink_Shutdown(void);
uint8_t LocalLink_Enabled(void);     /* the adapter is there (always, once set up) */
uint16_t LocalLink_MyId(void);       /* our id in the session; 0 if not in one */
uint32_t LocalLink_Generation(void); /* changes with every session */
/* Our adapter's broadcast while hosting (data NULL: none). */
void LocalLink_SetAdvert(uint8_t hosting, uint8_t nextSlot, const uint8_t *data);
int LocalLink_GetPeers(struct LocalPeer *out, int max);
void LocalLink_Send(uint16_t dest, const void *data, uint32_t len);
uint8_t LocalLink_PollEvent(struct LocalEvent *ev);
uint32_t LocalLink_Milliseconds(void);

/* Sessions. LocalLink_StartSession leaves the current one and joins the
 * nearby session of that kind (LOCAL_KIND_*), or hosts one if there is none;
 * LocalLink_GetSession says how that went (LOCAL_SESSION_*). */
void LocalLink_StartSession(uint8_t kind);
void LocalLink_LeaveSession(void);
uint8_t LocalLink_GetSession(void);
uint8_t LocalLink_GetSessionKind(void);
uint8_t LocalLink_InSession(void); /* in a session, or looking for one */
/* The system is going to sleep (the lid closed): the session is left now,
 * waiting a moment for it to be, so the others see the link end at once
 * rather than a partner gone silent. */
void LocalLink_LeaveBeforeSleep(void);
/* And it woke: the wireless service is started again for the next session. */
void LocalLink_AfterWake(void);

#endif /* CTR_LOCAL_H */
