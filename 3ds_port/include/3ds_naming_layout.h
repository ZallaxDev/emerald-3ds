#ifndef GUARD_3DS_NAMING_LAYOUT_H
#define GUARD_3DS_NAMING_LAYOUT_H

/*
 * The naming keyboard's geometry, pure so a host test can check it for every
 * language: which columns start a new group (from the game's own x positions
 * of its columns, or the spaces of a Japanese page), and where the keys of a
 * page go in the room the buttons leave. Used by 3ds_naming_view.inc (the
 * game's side) and 3ds_bottom_naming.inc (the drawing).
 */

#include <stdint.h>

#define CTR_NAMING_LAYOUT_COLS 12
#define CTR_NAMING_KEY_GAP 2        /* between keys of a group */
#define CTR_NAMING_GROUP_GAP 6      /* between groups */
#define CTR_NAMING_KEY_MAX_W 30

/*
 * Bit n of the result: a gap after column n. The game spaces its columns at
 * fixed x positions (sPageColumnXPos): a distance over one and a half times
 * the shortest one starts a new group (the letters 3 + 4 + 1, the symbols 6).
 * A table has a slot for every column the widest page has: the slots past a
 * page's own columns are 0, a step that does not go forward, and is no step.
 */
static inline uint16_t CtrNaming_GapsFromPositions(const uint8_t *xs, int cols)
{
    int min = 0xFF;
    uint16_t gaps = 0;

    for (int c = 1; c < cols; ++c)
    {
        int step = xs[c] - xs[c - 1];

        if (step > 0 && step < min)
            min = step;
    }
    for (int c = 1; c < cols; ++c)
    {
        int step = xs[c] - xs[c - 1];

        if (step > 0 && step * 2 > min * 3)
            gaps |= (uint16_t)(1u << (c - 1));
    }
    return gaps;
}

struct CtrNamingKeys
{
    int count;                              /* the keys of a row */
    int width;                              /* each key's width */
    int x[CTR_NAMING_LAYOUT_COLS];          /* a column's left edge; -1 for a column with no key */
};

/*
 * Lays the columns out in [areaX, areaX + areaW), centred: spacer columns
 * (a space in every row of a Japanese page) hold no key and open a gap; so
 * does a gap flagged after a column. The keys are as wide as the room allows,
 * up to CTR_NAMING_KEY_MAX_W.
 */
static inline void CtrNaming_LayoutKeys(uint16_t spacer, uint16_t gapAfter, int cols, int areaX, int areaW,
                                         struct CtrNamingKeys *out)
{
    int n = 0, groups = 0, prev = -1, x;
    int pending = 0;

    if (cols > CTR_NAMING_LAYOUT_COLS)
        cols = CTR_NAMING_LAYOUT_COLS;
    for (int c = 0; c < cols; ++c)
    {
        out->x[c] = -1;
        if ((spacer >> c) & 1)
        {
            pending = 1;
            continue;
        }
        if (prev >= 0 && (pending || ((gapAfter >> prev) & 1)))
            ++groups;
        pending = 0;
        prev = c;
        ++n;
    }
    out->count = n;
    out->width = 0;
    if (n == 0)
        return;
    out->width = (areaW - (n - 1) * CTR_NAMING_KEY_GAP - groups * (CTR_NAMING_GROUP_GAP - CTR_NAMING_KEY_GAP)) / n;
    if (out->width > CTR_NAMING_KEY_MAX_W)
        out->width = CTR_NAMING_KEY_MAX_W;
    x = areaX + (areaW - (n * out->width + (n - 1) * CTR_NAMING_KEY_GAP
                          + groups * (CTR_NAMING_GROUP_GAP - CTR_NAMING_KEY_GAP))) / 2;
    prev = -1;
    pending = 0;
    for (int c = 0; c < cols; ++c)
    {
        if ((spacer >> c) & 1)
        {
            pending = 1;
            continue;
        }
        if (prev >= 0)
            x += out->width + CTR_NAMING_KEY_GAP
               + ((pending || ((gapAfter >> prev) & 1)) ? CTR_NAMING_GROUP_GAP - CTR_NAMING_KEY_GAP : 0);
        pending = 0;
        out->x[c] = x;
        prev = c;
    }
}

#endif
