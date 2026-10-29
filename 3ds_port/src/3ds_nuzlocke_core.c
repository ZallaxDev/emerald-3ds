/*
 * The Nuzlocke tracker's state, record and write rules (3ds_nuzlocke_core.h).
 * No SDK, no game header, no I/O.
 */
#include <string.h>
#include "3ds_nuzlocke_core.h"

static const char sMagic[8] = {'E', 'M', '3', 'D', 'N', 'U', 'Z', '2'};
static const uint8_t sGymCaps[8] = {15, 19, 24, 29, 31, 33, 42, 46};
#define CHAMPION_CAP 58u
#define AREAS_AT CTR_NUZ_HEADER_BYTES
#define FALLEN_AT (AREAS_AT + CTR_NUZ_AREA_COUNT)
#define CRC_AT (FALLEN_AT + CTR_NUZ_FALLEN_MAX * 9u)

/* The reflected IEEE polynomial's byte table (0xEDB88320): the same values
 * as the bit-by-bit loop, eight times fewer steps. */
static const uint32_t sCrcTable[256] = {
    0x00000000u, 0x77073096u, 0xEE0E612Cu, 0x990951BAu, 0x076DC419u, 0x706AF48Fu,
    0xE963A535u, 0x9E6495A3u, 0x0EDB8832u, 0x79DCB8A4u, 0xE0D5E91Eu, 0x97D2D988u,
    0x09B64C2Bu, 0x7EB17CBDu, 0xE7B82D07u, 0x90BF1D91u, 0x1DB71064u, 0x6AB020F2u,
    0xF3B97148u, 0x84BE41DEu, 0x1ADAD47Du, 0x6DDDE4EBu, 0xF4D4B551u, 0x83D385C7u,
    0x136C9856u, 0x646BA8C0u, 0xFD62F97Au, 0x8A65C9ECu, 0x14015C4Fu, 0x63066CD9u,
    0xFA0F3D63u, 0x8D080DF5u, 0x3B6E20C8u, 0x4C69105Eu, 0xD56041E4u, 0xA2677172u,
    0x3C03E4D1u, 0x4B04D447u, 0xD20D85FDu, 0xA50AB56Bu, 0x35B5A8FAu, 0x42B2986Cu,
    0xDBBBC9D6u, 0xACBCF940u, 0x32D86CE3u, 0x45DF5C75u, 0xDCD60DCFu, 0xABD13D59u,
    0x26D930ACu, 0x51DE003Au, 0xC8D75180u, 0xBFD06116u, 0x21B4F4B5u, 0x56B3C423u,
    0xCFBA9599u, 0xB8BDA50Fu, 0x2802B89Eu, 0x5F058808u, 0xC60CD9B2u, 0xB10BE924u,
    0x2F6F7C87u, 0x58684C11u, 0xC1611DABu, 0xB6662D3Du, 0x76DC4190u, 0x01DB7106u,
    0x98D220BCu, 0xEFD5102Au, 0x71B18589u, 0x06B6B51Fu, 0x9FBFE4A5u, 0xE8B8D433u,
    0x7807C9A2u, 0x0F00F934u, 0x9609A88Eu, 0xE10E9818u, 0x7F6A0DBBu, 0x086D3D2Du,
    0x91646C97u, 0xE6635C01u, 0x6B6B51F4u, 0x1C6C6162u, 0x856530D8u, 0xF262004Eu,
    0x6C0695EDu, 0x1B01A57Bu, 0x8208F4C1u, 0xF50FC457u, 0x65B0D9C6u, 0x12B7E950u,
    0x8BBEB8EAu, 0xFCB9887Cu, 0x62DD1DDFu, 0x15DA2D49u, 0x8CD37CF3u, 0xFBD44C65u,
    0x4DB26158u, 0x3AB551CEu, 0xA3BC0074u, 0xD4BB30E2u, 0x4ADFA541u, 0x3DD895D7u,
    0xA4D1C46Du, 0xD3D6F4FBu, 0x4369E96Au, 0x346ED9FCu, 0xAD678846u, 0xDA60B8D0u,
    0x44042D73u, 0x33031DE5u, 0xAA0A4C5Fu, 0xDD0D7CC9u, 0x5005713Cu, 0x270241AAu,
    0xBE0B1010u, 0xC90C2086u, 0x5768B525u, 0x206F85B3u, 0xB966D409u, 0xCE61E49Fu,
    0x5EDEF90Eu, 0x29D9C998u, 0xB0D09822u, 0xC7D7A8B4u, 0x59B33D17u, 0x2EB40D81u,
    0xB7BD5C3Bu, 0xC0BA6CADu, 0xEDB88320u, 0x9ABFB3B6u, 0x03B6E20Cu, 0x74B1D29Au,
    0xEAD54739u, 0x9DD277AFu, 0x04DB2615u, 0x73DC1683u, 0xE3630B12u, 0x94643B84u,
    0x0D6D6A3Eu, 0x7A6A5AA8u, 0xE40ECF0Bu, 0x9309FF9Du, 0x0A00AE27u, 0x7D079EB1u,
    0xF00F9344u, 0x8708A3D2u, 0x1E01F268u, 0x6906C2FEu, 0xF762575Du, 0x806567CBu,
    0x196C3671u, 0x6E6B06E7u, 0xFED41B76u, 0x89D32BE0u, 0x10DA7A5Au, 0x67DD4ACCu,
    0xF9B9DF6Fu, 0x8EBEEFF9u, 0x17B7BE43u, 0x60B08ED5u, 0xD6D6A3E8u, 0xA1D1937Eu,
    0x38D8C2C4u, 0x4FDFF252u, 0xD1BB67F1u, 0xA6BC5767u, 0x3FB506DDu, 0x48B2364Bu,
    0xD80D2BDAu, 0xAF0A1B4Cu, 0x36034AF6u, 0x41047A60u, 0xDF60EFC3u, 0xA867DF55u,
    0x316E8EEFu, 0x4669BE79u, 0xCB61B38Cu, 0xBC66831Au, 0x256FD2A0u, 0x5268E236u,
    0xCC0C7795u, 0xBB0B4703u, 0x220216B9u, 0x5505262Fu, 0xC5BA3BBEu, 0xB2BD0B28u,
    0x2BB45A92u, 0x5CB36A04u, 0xC2D7FFA7u, 0xB5D0CF31u, 0x2CD99E8Bu, 0x5BDEAE1Du,
    0x9B64C2B0u, 0xEC63F226u, 0x756AA39Cu, 0x026D930Au, 0x9C0906A9u, 0xEB0E363Fu,
    0x72076785u, 0x05005713u, 0x95BF4A82u, 0xE2B87A14u, 0x7BB12BAEu, 0x0CB61B38u,
    0x92D28E9Bu, 0xE5D5BE0Du, 0x7CDCEFB7u, 0x0BDBDF21u, 0x86D3D2D4u, 0xF1D4E242u,
    0x68DDB3F8u, 0x1FDA836Eu, 0x81BE16CDu, 0xF6B9265Bu, 0x6FB077E1u, 0x18B74777u,
    0x88085AE6u, 0xFF0F6A70u, 0x66063BCAu, 0x11010B5Cu, 0x8F659EFFu, 0xF862AE69u,
    0x616BFFD3u, 0x166CCF45u, 0xA00AE278u, 0xD70DD2EEu, 0x4E048354u, 0x3903B3C2u,
    0xA7672661u, 0xD06016F7u, 0x4969474Du, 0x3E6E77DBu, 0xAED16A4Au, 0xD9D65ADCu,
    0x40DF0B66u, 0x37D83BF0u, 0xA9BCAE53u, 0xDEBB9EC5u, 0x47B2CF7Fu, 0x30B5FFE9u,
    0xBDBDF21Cu, 0xCABAC28Au, 0x53B39330u, 0x24B4A3A6u, 0xBAD03605u, 0xCDD70693u,
    0x54DE5729u, 0x23D967BFu, 0xB3667A2Eu, 0xC4614AB8u, 0x5D681B02u, 0x2A6F2B94u,
    0xB40BBE37u, 0xC30C8EA1u, 0x5A05DF1Bu, 0x2D02EF8Du
};

uint32_t CtrNuz_Crc32(uint32_t crc, const void *data, size_t size)
{
    const uint8_t *bytes = data;

    crc = ~crc;
    while (size-- > 0)
        crc = (crc >> 8) ^ sCrcTable[(crc ^ *bytes++) & 0xFFu];
    return ~crc;
}

static void Put(uint8_t *bytes, uint32_t value, unsigned size)
{
    for (unsigned index = 0; index < size; ++index)
        bytes[index] = (uint8_t)(value >> (index * 8u));
}

static uint32_t Get(const uint8_t *bytes, unsigned size)
{
    uint32_t value = 0;

    for (unsigned index = 0; index < size; ++index)
        value |= (uint32_t)bytes[index] << (index * 8u);
    return value;
}

void CtrNuz_Reset(CtrNuzState *state)
{
    memset(state, 0, sizeof(*state));
}

bool CtrNuz_IsEmpty(const CtrNuzState *state)
{
    if (state->fallenCount != 0)
        return false;
    for (unsigned area = 0; area < CTR_NUZ_AREA_COUNT; ++area)
        if (state->area[area] != CTR_NUZ_UNUSED)
            return false;
    return true;
}

CtrNuzStatus CtrNuz_Area(const CtrNuzState *state, unsigned area)
{
    if (area >= CTR_NUZ_AREA_COUNT || state->area[area] >= CTR_NUZ_STATUS_COUNT)
        return CTR_NUZ_UNUSED;
    return (CtrNuzStatus)state->area[area];
}

bool CtrNuz_SetArea(CtrNuzState *state, unsigned area, CtrNuzStatus status)
{
    if (area >= CTR_NUZ_AREA_COUNT || (unsigned)status >= CTR_NUZ_STATUS_COUNT)
        return false;
    state->area[area] = (uint8_t)status;
    return true;
}

static int FindFallen(const CtrNuzState *state, uint32_t personality, uint32_t otId, bool shedinja)
{
    for (unsigned index = 0; index < state->fallenCount && index < CTR_NUZ_FALLEN_MAX; ++index)
        if (state->fallen[index].personality == personality && state->fallen[index].otId == otId && state->fallen[index].shedinja == shedinja)
            return (int)index;
    return -1;
}

bool CtrNuz_IsFallen(const CtrNuzState *state, uint32_t personality, uint32_t otId, bool shedinja)
{
    return FindFallen(state, personality, otId, shedinja) >= 0;
}

bool CtrNuz_SetFallen(CtrNuzState *state, uint32_t personality, uint32_t otId, bool shedinja, bool fallen)
{
    int index = FindFallen(state, personality, otId, shedinja);

    if (fallen)
    {
        if (index >= 0 || state->fallenCount >= CTR_NUZ_FALLEN_MAX)
            return false;
        state->fallen[state->fallenCount].personality = personality;
        state->fallen[state->fallenCount].otId = otId;
        state->fallen[state->fallenCount].shedinja = shedinja;
        state->fallenCount++;
        return true;
    }
    if (index < 0)
        return false;
    /* Kept packed, the slots past the count zero (the record checks both). */
    memmove(&state->fallen[index], &state->fallen[index + 1],
            (state->fallenCount - (unsigned)index - 1u) * sizeof(state->fallen[0]));
    state->fallenCount--;
    memset(&state->fallen[state->fallenCount], 0, sizeof(state->fallen[0]));
    return true;
}

void CtrNuz_Counts(const CtrNuzState *state, unsigned *caught, unsigned *missed, unsigned *fallen)
{
    unsigned c = 0, m = 0;

    for (unsigned area = 0; area < CTR_NUZ_AREA_COUNT; ++area)
    {
        c += state->area[area] == CTR_NUZ_CAUGHT;
        m += state->area[area] == CTR_NUZ_MISSED;
    }
    *caught = c;
    *missed = m;
    *fallen = state->fallenCount;
}

unsigned CtrNuz_NextCap(unsigned badges, unsigned *leader)
{
    if (badges >= 8)
    {
        *leader = 9;
        return CHAMPION_CAP;
    }
    *leader = badges + 1;
    return sGymCaps[badges];
}

void CtrNuz_Encode(const CtrNuzState *state, uint32_t saveCrc, uint32_t saveSize,
                   uint8_t record[CTR_NUZ_RECORD_BYTES])
{
    memset(record, 0, CTR_NUZ_RECORD_BYTES);
    memcpy(record, sMagic, sizeof(sMagic));
    Put(record + 8, saveCrc, 4);
    Put(record + 12, saveSize, 4);
    Put(record + 16, state->fallenCount, 2);
    memcpy(record + AREAS_AT, state->area, CTR_NUZ_AREA_COUNT);
    for (unsigned index = 0; index < state->fallenCount && index < CTR_NUZ_FALLEN_MAX; ++index)
    {
        Put(record + FALLEN_AT + index * 9u, state->fallen[index].personality, 4);
        Put(record + FALLEN_AT + index * 9u + 4u, state->fallen[index].otId, 4);
        record[FALLEN_AT + index * 9u + 8u] = state->fallen[index].shedinja;
    }
    Put(record + CRC_AT, CtrNuz_Crc32(0, record, CRC_AT), 4);
}

bool CtrNuz_Decode(const uint8_t record[CTR_NUZ_RECORD_BYTES], CtrNuzState *state,
                   uint32_t *saveCrc, uint32_t *saveSize)
{
    return CtrNuz_DecodeSized(record, CTR_NUZ_RECORD_BYTES, state, saveCrc, saveSize);
}

bool CtrNuz_DecodeSized(const uint8_t *record, size_t bytes, CtrNuzState *state,
                        uint32_t *saveCrc, uint32_t *saveSize)
{
    CtrNuzState decoded;
    unsigned count, stride;
    size_t crcAt;

    if (bytes == CTR_NUZ_RECORD_V1_BYTES)
        stride = 8;
    else if (bytes == CTR_NUZ_RECORD_BYTES)
        stride = 9;
    else
        return false;
    crcAt = bytes - 4;
    if (memcmp(record, sMagic, sizeof(sMagic) - 1) != 0
     || record[7] != (stride == 8 ? '1' : '2')
     || Get(record + crcAt, 4) != CtrNuz_Crc32(0, record, crcAt))
        return false;
    for (unsigned index = 18; index < CTR_NUZ_HEADER_BYTES; ++index)
        if (record[index] != 0)
            return false;
    count = Get(record + 16, 2);
    if (count > CTR_NUZ_FALLEN_MAX)
        return false;
    memset(&decoded, 0, sizeof(decoded));
    for (unsigned area = 0; area < CTR_NUZ_AREA_COUNT; ++area)
    {
        if (record[AREAS_AT + area] >= CTR_NUZ_STATUS_COUNT)
            return false;
        decoded.area[area] = record[AREAS_AT + area];
    }
    for (unsigned index = 0; index < CTR_NUZ_FALLEN_MAX; ++index)
    {
        uint32_t personality = Get(record + FALLEN_AT + index * stride, 4);
        uint32_t otId = Get(record + FALLEN_AT + index * stride + 4u, 4);
        uint8_t shedinja = stride == 9 ? record[FALLEN_AT + index * stride + 8u] : 0;

        if (shedinja > 1)
            return false;

        if (index >= count)
        {
            if (personality != 0 || otId != 0 || shedinja != 0)
                return false;
            continue;
        }
        if (CtrNuz_IsFallen(&decoded, personality, otId, shedinja))
            return false;
        decoded.fallen[index].personality = personality;
        decoded.fallen[index].otId = otId;
        decoded.fallen[index].shedinja = shedinja;
        decoded.fallenCount = (uint16_t)(index + 1);
    }
    *state = decoded;
    *saveCrc = Get(record + 8, 4);
    *saveSize = Get(record + 12, 4);
    return true;
}

void CtrNuzSession_NewGame(CtrNuzSession *session)
{
    CtrNuz_Reset(&session->state);
    session->link = CTR_NUZ_LINK_NONE;
    /* The new game's first save replaces the old game's record. */
    session->owned = true;
    session->dirty = false;
    session->writeFailed = false;
}

void CtrNuzSession_Loaded(CtrNuzSession *session, CtrNuzLink link)
{
    if (link != CTR_NUZ_LINK_MATCHED)
        CtrNuz_Reset(&session->state);
    session->link = link;
    session->owned = link == CTR_NUZ_LINK_MATCHED;
    session->dirty = false;
    session->writeFailed = false;
}

void CtrNuzSession_Edited(CtrNuzSession *session)
{
    session->dirty = true;
}

bool CtrNuzSession_ShouldWrite(const CtrNuzSession *session, bool enabled)
{
    bool empty = CtrNuz_IsEmpty(&session->state);

    /* The tracker off and nothing to keep: no SD writes on every game save.
     * A record that holds marks is still rewritten (it names the save). */
    if (!enabled && !session->dirty && empty)
        return false;
    return session->owned || session->dirty || !empty;
}

void CtrNuzSession_Written(CtrNuzSession *session, bool ok)
{
    if (!ok)
    {
        session->writeFailed = true;
        return;
    }
    session->link = CTR_NUZ_LINK_MATCHED;
    session->owned = true;
    session->dirty = false;
    session->writeFailed = false;
}

CtrNuzLabel CtrNuzSession_Label(const CtrNuzSession *session)
{
    if (session->writeFailed)
        return CTR_NUZ_LABEL_WRITE_FAILED;
    if (session->dirty)
        return CTR_NUZ_LABEL_UNSAVED;
    switch (session->link)
    {
    case CTR_NUZ_LINK_MATCHED:
        return CTR_NUZ_LABEL_THIS_SAVE;
    case CTR_NUZ_LINK_OTHER:
        return CTR_NUZ_LABEL_OTHER_SAVE;
    case CTR_NUZ_LINK_DAMAGED:
        return CTR_NUZ_LABEL_DAMAGED;
    case CTR_NUZ_LINK_NONE:
    default:
        return CTR_NUZ_LABEL_NEW;
    }
}
