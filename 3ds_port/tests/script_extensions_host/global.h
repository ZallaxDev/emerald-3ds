#ifndef GUARD_GLOBAL_H
#define GUARD_GLOBAL_H

/* Host-test stand-in for the decomp's include/global.h. Only the scalar types
 * and string helpers the extension layer needs; the real build uses the game
 * header. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef u8 bool8;

#define TRUE 1
#define FALSE 0

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

#endif /* GUARD_GLOBAL_H */
