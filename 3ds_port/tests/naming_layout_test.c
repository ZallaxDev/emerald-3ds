/*
 * The naming keyboard's geometry (include/3ds_naming_layout.h) for the shapes
 * the game's languages give it: the groups found from the game's own column
 * positions, and the keys laid out in the room the buttons leave - inside it,
 * apart, centred, and no narrower than a finger-sized 16 pixels.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "3ds_naming_layout.h"

#define AREA_X 8
#define AREA_W 230

static int failures;

static void check(int ok, const char *what, const char *name)
{
    if (!ok)
    {
        printf("FAIL %s: %s\n", name, what);
        ++failures;
    }
}

static void layout(const char *name, uint16_t spacer, uint16_t gaps, int cols, int keys, int groups)
{
    struct CtrNamingKeys k;
    int first = -1, last = -1, prev = -1, widths = 0;

    CtrNaming_LayoutKeys(spacer, gaps, cols, AREA_X, AREA_W, &k);
    check(k.count == keys, "the number of keys", name);
    check(k.width >= 16 && k.width <= CTR_NAMING_KEY_MAX_W, "the key's width", name);
    for (int c = 0; c < cols; ++c)
    {
        if (k.x[c] < 0)
        {
            check((spacer >> c) & 1, "a column with no key must be a spacer", name);
            continue;
        }
        check(k.x[c] >= AREA_X && k.x[c] + k.width <= AREA_X + AREA_W, "a key inside its room", name);
        check(prev < 0 || k.x[c] >= prev + k.width + CTR_NAMING_KEY_GAP, "keys apart", name);
        if (first < 0)
            first = k.x[c];
        last = k.x[c] + k.width;
        prev = k.x[c];
        ++widths;
    }
    check(widths == keys, "the keys placed", name);
    check(abs((first - AREA_X) - (AREA_X + AREA_W - last)) <= 1, "centred", name);
    (void)groups;
}

int main(void)
{
    /* The game's own tables (naming_screen.c, sPageColumnXPos). */
    static const uint8_t letters[8] = {0, 12, 24, 56, 68, 80, 92, 123};
    static const uint8_t symbols[6] = {0, 22, 44, 66, 88, 110};
    /* A nine-column page (French, German): the same groups with one more column. */
    static const uint8_t nine[9] = {0, 12, 24, 56, 68, 80, 92, 104, 135};
    /* The table's real shape when a page has nine columns of which the game's text fills eight: nine slots, the last 0. */
    static const uint8_t slots9[9] = {0, 12, 24, 56, 68, 80, 92, 123};
    uint16_t g;
    struct CtrNamingKeys k;

    g = CtrNaming_GapsFromPositions(letters, 8);
    check(g == ((1u << 2) | (1u << 6)), "the letters come in 3 + 4 + 1", "letters");
    check(CtrNaming_GapsFromPositions(symbols, 6) == 0, "the symbols are one group", "symbols");
    check(CtrNaming_GapsFromPositions(nine, 9) == ((1u << 2) | (1u << 7)), "nine columns: 3 + 5 + 1", "nine");

    /* A slot past the page's text is no step: no negative distance shortens the minimum or opens a gap. */
    check(CtrNaming_GapsFromPositions(slots9, 9) == ((1u << 2) | (1u << 6)), "nine slots, eight positions", "slots9");
    check(CtrNaming_GapsFromPositions(slots9, 8) == CtrNaming_GapsFromPositions(slots9, 9), "a trailing slot changes nothing", "slots9");
    {
        static const uint8_t flat[3] = {0, 0, 0};

        check(CtrNaming_GapsFromPositions(flat, 3) == 0, "positions that never advance", "flat");
    }

    /* The widths the English layout gives, as the approved mockup shows. */
    CtrNaming_LayoutKeys(0, g, 8, AREA_X, AREA_W, &k);
    check(k.width == 26 && k.x[0] == 8 && k.x[3] == 8 + 3 * 28 + 4 && k.x[7] == 8 + 230 - 26, "the mockup's letters", "letters");

    layout("letters", 0, g, 8, 8, 3);
    layout("symbols", 0, 0, 6, 6, 1);
    /* The nine-slot page with its last column empty (a spacer): the eight keys of the letters, as with eight columns. */
    layout("slots9", 1u << 8, CtrNaming_GapsFromPositions(slots9, 9), 9, 8, 2);
    CtrNaming_LayoutKeys(1u << 8, CtrNaming_GapsFromPositions(slots9, 9), 9, AREA_X, AREA_W, &k);
    check(k.width == 26 && k.x[8] == -1 && k.x[7] == 8 + 230 - 26, "an empty ninth column leaves the letters as they were", "slots9");
    layout("nine", 0, CtrNaming_GapsFromPositions(nine, 9), 9, 9, 3);
    /* Japanese: eleven columns, the sixth a space in every row between two groups of five. */
    layout("kana", 1u << 5, (1u << 4) | (1u << 5), 11, 10, 2);
    /* The widest page the view carries, with no groups. */
    layout("twelve", 0, 0, 12, 12, 1);
    /* Nothing laid out is no keys, not a division by zero. */
    CtrNaming_LayoutKeys(0xFFF, 0, 12, AREA_X, AREA_W, &k);
    check(k.count == 0 && k.width == 0, "a page of spacers", "empty");

    if (failures)
        return 1;
    printf("PASS naming layout: groups from the game's columns, letters, symbols, nine columns, kana, twelve; inside, apart, centred\n");
    return 0;
}
