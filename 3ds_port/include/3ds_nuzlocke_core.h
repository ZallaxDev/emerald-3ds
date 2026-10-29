#ifndef CTR_NUZLOCKE_CORE_H
#define CTR_NUZLOCKE_CORE_H

/*
 * The optional Nuzlocke tracker (ENHANCEMENTS): a record kept beside the
 * game's save, never in it. It enforces nothing: the player marks each area's
 * first encounter (unused, caught, missed) and the Pokemon that fell, and
 * reads the next level cap from the badges.
 *
 * SDK-free and free of game headers: the state, its record on the SD card and
 * the rules of when that record is written, tested on the host
 * (tests/nuzlocke_test.c).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Areas are the game's region map sections (below 256). */
#define CTR_NUZ_AREA_COUNT 256u
#define CTR_NUZ_FALLEN_MAX 128u
/* "EM3DNUZ2", save CRC, save size, fallen count, 6 reserved bytes, areas,
 * fallen Pokemon (personality, OT id, Shedinja flag), record CRC. */
#define CTR_NUZ_HEADER_BYTES 24u
#define CTR_NUZ_RECORD_V1_BYTES (CTR_NUZ_HEADER_BYTES + CTR_NUZ_AREA_COUNT + CTR_NUZ_FALLEN_MAX * 8u + 4u)
#define CTR_NUZ_RECORD_BYTES (CTR_NUZ_HEADER_BYTES + CTR_NUZ_AREA_COUNT + CTR_NUZ_FALLEN_MAX * 9u + 4u)

typedef enum
{
    CTR_NUZ_UNUSED,
    CTR_NUZ_CAUGHT,
    CTR_NUZ_MISSED,
    CTR_NUZ_STATUS_COUNT
} CtrNuzStatus;

/* A Pokemon by who it is, not where it is: party order, the PC and trades
 * keep both numbers. Shedinja is copied from Ninjask, so it needs its own
 * identity bit; other species changes must not lose a fallen mark. */
typedef struct
{
    uint32_t personality;
    uint32_t otId;
    bool shedinja;
} CtrNuzMon;

typedef struct
{
    uint8_t area[CTR_NUZ_AREA_COUNT];
    uint16_t fallenCount;
    CtrNuzMon fallen[CTR_NUZ_FALLEN_MAX];
} CtrNuzState;

/* What the record on the card is to the loaded save. */
typedef enum
{
    CTR_NUZ_LINK_NONE,      /* no record */
    CTR_NUZ_LINK_MATCHED,   /* a valid record of this very save */
    CTR_NUZ_LINK_OTHER,     /* valid, but of another save: not applied */
    CTR_NUZ_LINK_DAMAGED    /* a record that fails its checks: not applied */
} CtrNuzLink;

/* CRC-32 (IEEE), of the save image and of the record. */
uint32_t CtrNuz_Crc32(uint32_t crc, const void *data, size_t size);

void CtrNuz_Reset(CtrNuzState *state);
bool CtrNuz_IsEmpty(const CtrNuzState *state);
CtrNuzStatus CtrNuz_Area(const CtrNuzState *state, unsigned area);
bool CtrNuz_SetArea(CtrNuzState *state, unsigned area, CtrNuzStatus status);
bool CtrNuz_IsFallen(const CtrNuzState *state, uint32_t personality, uint32_t otId, bool shedinja);
/* False when the list is full (marking) or nothing changed. */
bool CtrNuz_SetFallen(CtrNuzState *state, uint32_t personality, uint32_t otId, bool shedinja, bool fallen);
void CtrNuz_Counts(const CtrNuzState *state, unsigned *caught, unsigned *missed, unsigned *fallen);
/* The level of the next gym leader's strongest Pokemon for this many badges
 * (15, 19, 24, 29, 31, 33, 42, 46), then the Champion's (58); *leader is the
 * gym, 1 to 8, or 9 for the Champion. */
unsigned CtrNuz_NextCap(unsigned badges, unsigned *leader);

void CtrNuz_Encode(const CtrNuzState *state, uint32_t saveCrc, uint32_t saveSize,
                   uint8_t record[CTR_NUZ_RECORD_BYTES]);
/* False, *state untouched, unless every check holds (magic, CRC, reserved
 * bytes, statuses, count, unused entries, no repeated Pokemon). */
bool CtrNuz_Decode(const uint8_t record[CTR_NUZ_RECORD_BYTES], CtrNuzState *state,
                   uint32_t *saveCrc, uint32_t *saveSize);

/* Accept v1 records too: ambiguous legacy marks stay on the original lineage,
 * never implicitly mark its copied Shedinja. The next save writes v2. */
bool CtrNuz_DecodeSized(const uint8_t *record, size_t bytes, CtrNuzState *state,
                        uint32_t *saveCrc, uint32_t *saveSize);

/*
 * The tracker of the running game. A record is written after each successful
 * game save when this game owns it (it came from this save's record, or a new
 * game began) or holds anything: a record of another save is never replaced
 * by an empty tracker that merely failed to match it. With the tracker
 * switched off, an empty unedited tracker is not written at all.
 */
typedef enum
{
    CTR_NUZ_LABEL_THIS_SAVE,
    CTR_NUZ_LABEL_NEW,
    CTR_NUZ_LABEL_OTHER_SAVE,
    CTR_NUZ_LABEL_DAMAGED,
    CTR_NUZ_LABEL_UNSAVED,
    CTR_NUZ_LABEL_WRITE_FAILED,
    CTR_NUZ_LABEL_COUNT
} CtrNuzLabel;

typedef struct
{
    CtrNuzState state;
    CtrNuzLink link;
    bool owned;
    bool dirty;
    bool writeFailed;
} CtrNuzSession;

void CtrNuzSession_NewGame(CtrNuzSession *session);
/* After the store loaded session->state for the save (reset unless matched). */
void CtrNuzSession_Loaded(CtrNuzSession *session, CtrNuzLink link);
void CtrNuzSession_Edited(CtrNuzSession *session);
bool CtrNuzSession_ShouldWrite(const CtrNuzSession *session, bool enabled);
void CtrNuzSession_Written(CtrNuzSession *session, bool ok);
CtrNuzLabel CtrNuzSession_Label(const CtrNuzSession *session);

#endif
