#ifndef GUARD_3DS_NAMING_KANA_H
#define GUARD_3DS_NAMING_KANA_H

#include <stdint.h>

/* Keyboard actions only. These values are never written into a name. */
enum
{
    CTR_NAMING_DAKUTEN = 0xF7,
    CTR_NAMING_HANDAKUTEN = 0xF8,
};

static inline int CtrNaming_IsKanaMark(uint8_t key)
{
    return key == CTR_NAMING_DAKUTEN || key == CTR_NAMING_HANDAKUTEN;
}

static inline uint8_t CtrNaming_ApplyKanaMark(uint8_t ch, uint8_t mark)
{
    static const struct
    {
        uint8_t plain, voiced, semivoiced, count;
    } rows[] = {
        {0x06, 0x37, 0,    15}, /* K, S and T rows */
        {0x1A, 0x46, 0x4B,  5}, /* H row */
    };
    int script = ch >= 0x51 && ch <= 0xA0 ? 0x50 : 0;
    int kana = ch - script;

    for (unsigned int i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i)
    {
        int offset = kana - rows[i].plain;

        if (kana >= rows[i].voiced && kana < rows[i].voiced + rows[i].count)
            offset = kana - rows[i].voiced;
        if (rows[i].semivoiced && kana >= rows[i].semivoiced && kana < rows[i].semivoiced + rows[i].count)
            offset = kana - rows[i].semivoiced;
        if (offset < 0 || offset >= rows[i].count)
            continue;
        if (mark == CTR_NAMING_DAKUTEN)
            return script + rows[i].voiced + offset;
        if (mark == CTR_NAMING_HANDAKUTEN && rows[i].semivoiced)
            return script + rows[i].semivoiced + offset;
    }
    return ch;
}

#endif
