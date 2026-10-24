#ifndef CTR_LOCALE_TEXT_H
#define CTR_LOCALE_TEXT_H
#include <stddef.h>
#include <stdint.h>

/* The bottom screen uses the cartridge font, which expects game character
 * codes rather than UTF-8. Codes follow pret/pokeemerald's constants/characters.h;
 * no glyph pixels live here. Consume one complete code point per output byte. */
static inline uint8_t CtrLocale_Glyph(uint32_t c)
{
    if (c >= 'A' && c <= 'Z') return 0xBB + c - 'A';
    if (c >= 'a' && c <= 'z') return 0xD5 + c - 'a';
    if (c >= '0' && c <= '9') return 0xA1 + c - '0';
    switch (c)
    {
    case ' ': return 0x00;
    case '/': return 0xBA;
    case '-': return 0xAE;
    case '.': return 0xAD;
    case ',': return 0xB8;
    case ':': return 0xF0;
    case '!': return 0xAB;
    case '?': return 0xAC;
    case '\'': case 0x2019: return 0xB4;
    case '*': return 0x1B; /* Original POK*MON labels. */
    case '%': return 0x5B;
    case '&': return 0x2D;
    case 0x2026: return 0xB0;
    case 0x2642: return 0xB5;
    case 0x2640: return 0xB6;
    case 0xc0: return 0x01;
    case 0xc1: return 0x02;
    case 0xc2: return 0x03;
    case 0xc7: return 0x04;
    case 0xc8: return 0x05;
    case 0xc9: return 0x06;
    case 0xca: return 0x07;
    case 0xcb: return 0x08;
    case 0xcc: return 0x09;
    case 0xcd: return 0x5A;
    case 0xce: return 0x0B;
    case 0xcf: return 0x0C;
    case 0xd3: return 0x0E;
    case 0xd4: return 0x0F;
    case 0xd9: return 0x11;
    case 0xda: return 0x12;
    case 0xdb: return 0x13;
    case 0xe0: return 0x16;
    case 0xe1: return 0x17;
    case 0xe2: return 0x68;
    case 0xe7: return 0x19;
    case 0xe8: return 0x1A;
    case 0xe9: return 0x1B;
    case 0xea: return 0x1C;
    case 0xeb: return 0x1D;
    case 0xec: return 0x1E;
    case 0xed: return 0x6F;
    case 0xee: return 0x20;
    case 0xef: return 0x21;
    case 0xf3: return 0x23;
    case 0xf4: return 0x24;
    case 0xf9: return 0x26;
    case 0xfa: return 0x27;
    case 0xfb: return 0x28;
    case 0xf1: return 0x29;
    case 0xf2: return 0x22;
    case 0xd1: return 0x14;
    case 0xd2: return 0x0D;
    case 0xc4: return 0xF1;
    case 0xd6: return 0xF2;
    case 0xdc: return 0xF3;
    case 0xe4: return 0xF4;
    case 0xf6: return 0xF5;
    case 0xfc: return 0xF6;
    case 0xdf: return 0x15;
    case 0xa1: return 0x52;
    case 0xbf: return 0x51;
    /* Ã Õ ã õ: the codes the Portuguese build gives them (patch 0057) and
     * draws in every font (3ds_compat.c). */
    case 0xc3: return 0x2F;
    case 0xd5: return 0x30;
    case 0xe3: return 0x31;
    case 0xf5: return 0x32;
    default: return 0xAC;
    }
}

static inline size_t CtrLocale_ToGame(uint8_t *out, size_t capacity, const char *text)
{
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)text;
    if (!capacity) return 0;
    while (*p && n + 1 < capacity)
    {
        uint32_t c = *p++;
        unsigned extra = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 :
                         c >= 0xf0 && c <= 0xf4 ? 3 : 0;
        if (extra)
        {
            uint32_t value = c & ((1u << (6 - extra)) - 1);
            unsigned i;
            for (i = 0; i < extra && p[i] && (p[i] & 0xc0) == 0x80; ++i)
                value = (value << 6) | (p[i] & 0x3f);
            if (i == extra)
            {
                p += extra;
                c = value < (extra == 1 ? 0x80u : extra == 2 ? 0x800u : 0x10000u)
                    || (value >= 0xd800 && value <= 0xdfff) || value > 0x10ffff ? 0xfffd : value;
            }
            else c = 0xfffd;
        }
        out[n++] = CtrLocale_Glyph(c);
    }
    out[n] = 0xff;
    return n;
}
#endif
