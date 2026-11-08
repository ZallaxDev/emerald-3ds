/*
 * memcpy for the whole program (-Wl,--wrap=memcpy, Makefile). devkitARM's
 * newlib carries the generic C memcpy: unless the source and the destination
 * are both word aligned it copies a byte at a time - the bottom screen's plate
 * columns, rows of halfwords at odd offsets, ran so at about four instructions
 * a byte. This one aligns the destination and copies by aligned words: the
 * source's own when it then lines up too, else two of them shifted into one.
 *
 * Its results are memcpy's. Short copies, both pointers already aligned (newlib
 * copies those by words), and overlapping ones (undefined for memcpy; kept as
 * newlib did them) go to newlib's own. A word read may take bytes of the
 * source's last word past its end: never past that word, so never another page.
 */
#include <stddef.h>
#include <stdint.h>

void *__real_memcpy(void *dst, const void *src, size_t n);
void *__wrap_memcpy(void *dst, const void *src, size_t n);

typedef uint32_t __attribute__((may_alias)) Word;

/* The loops below must not be turned back into calls of memcpy: that would
 * now be this function. */
#pragma GCC optimize("no-tree-loop-distribute-patterns")

void *__wrap_memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    uintptr_t to = (uintptr_t)d, from = (uintptr_t)s;

    if (n < 16 || (to < from + n && from < to + n) || !((to | from) & 3))
        return __real_memcpy(dst, src, n);
    /* At most three bytes: n stays above 12. */
    while ((uintptr_t)d & 3)
    {
        *d++ = *s++;
        --n;
    }
    unsigned k = (uintptr_t)s & 3;

    if (k == 0)
    {
        __real_memcpy(d, s, n);
        return dst;
    }
    /* Little endian: the destination word is the bytes from k on of one
     * aligned source word, then the first k of the next. */
    const Word *w = (const Word *)(s - k);
    Word *out = (Word *)d;
    unsigned right = 8 * k, left = 32 - right;
    uint32_t low = *w++;

    for (; n >= 16; n -= 16)
    {
        uint32_t a = w[0], b = w[1], c = w[2], e = w[3];

        out[0] = (low >> right) | (a << left);
        out[1] = (a >> right) | (b << left);
        out[2] = (b >> right) | (c << left);
        out[3] = (c >> right) | (e << left);
        low = e;
        w += 4;
        out += 4;
    }
    for (; n >= 4; n -= 4)
    {
        uint32_t a = *w++;

        *out++ = (low >> right) | (a << left);
        low = a;
    }
    d = (uint8_t *)out;
    s = (const uint8_t *)w - 4 + k;
    while (n--)
        *d++ = *s++;
    return dst;
}
