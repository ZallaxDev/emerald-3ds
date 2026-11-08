/*
 * Host test of the program's memcpy (src/3ds_memcpy.c): every length up to
 * 300 bytes from every pair of source and destination offsets in a word,
 * and some longer ones, against a byte copy - the bytes copied, the bytes
 * around them untouched, the destination returned, and newlib's own memcpy
 * (here a byte copy that counts its calls) used where it should be.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned sRealCalls;

void *__real_memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    ++sRealCalls;
    while (n--)
        *d++ = *s++;
    return dst;
}

#include "../src/3ds_memcpy.c"

#define GUARD 16
#define SPAN 4096

static uint8_t sSrc[SPAN + 2 * GUARD], sDst[SPAN + 2 * GUARD], sWant[SPAN + 2 * GUARD];

static int Check(size_t n, unsigned at, unsigned from)
{
    uint8_t *dst = sDst + GUARD + at;
    const uint8_t *src = sSrc + GUARD + from;
    void *got;

    for (size_t i = 0; i < sizeof(sDst); ++i)
        sDst[i] = sWant[i] = (uint8_t)(0xa5 ^ i);
    for (size_t i = 0; i < n; ++i)
        sWant[GUARD + at + i] = src[i];
    sRealCalls = 0;
    got = __wrap_memcpy(dst, src, n);
    if (got != dst)
    {
        printf("FAIL n=%zu at=%u from=%u: returned %p, not %p\n", n, at, from, got, (void *)dst);
        return 1;
    }
    if (memcmp(sDst, sWant, sizeof(sDst)) != 0)
    {
        for (size_t i = 0; i < sizeof(sDst); ++i)
            if (sDst[i] != sWant[i])
            {
                printf("FAIL n=%zu at=%u from=%u: byte %zd is %02x, not %02x\n", n, at, from,
                       (ptrdiff_t)i - GUARD - (ptrdiff_t)at, sDst[i], sWant[i]);
                break;
            }
        return 1;
    }
    /* Short copies and both pointers aligned: newlib's alone. */
    if ((n < 16 || !(((uintptr_t)dst | (uintptr_t)src) & 3)) && sRealCalls != 1)
    {
        printf("FAIL n=%zu at=%u from=%u: newlib's memcpy called %u times, not once\n", n, at, from, sRealCalls);
        return 1;
    }
    return 0;
}

int main(void)
{
    static const size_t longer[] = {511, 512, 513, 1023, 2048, 3001, SPAN - 8};
    unsigned failures = 0, cases = 0;

    for (size_t i = 0; i < sizeof(sSrc); ++i)
        sSrc[i] = (uint8_t)(i * 37 + (i >> 8) * 11 + 1);
    for (unsigned at = 0; at < 8; ++at)
        for (unsigned from = 0; from < 8; ++from)
        {
            for (size_t n = 0; n <= 300; ++n, ++cases)
                failures += Check(n, at, from);
            for (size_t i = 0; i < sizeof(longer) / sizeof(longer[0]); ++i, ++cases)
                failures += Check(longer[i], at, from);
        }
    /* Overlapping copies go to newlib's untouched. */
    {
        uint8_t buffer[64];

        for (unsigned i = 0; i < sizeof(buffer); ++i)
            buffer[i] = (uint8_t)i;
        sRealCalls = 0;
        __wrap_memcpy(buffer + 1, buffer, 40);
        ++cases;
        if (sRealCalls != 1)
        {
            printf("FAIL overlap: newlib's memcpy called %u times, not once\n", sRealCalls);
            ++failures;
        }
    }
    if (failures)
    {
        printf("memcpy_test: %u of %u cases failed\n", failures, cases);
        return 1;
    }
    printf("memcpy_test: %u cases passed\n", cases);
    return 0;
}
