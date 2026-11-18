/*
 * The naming keyboard's geometry (include/3ds_naming_layout.h) for the shapes
 * the game's languages give it: the groups found from the game's own column
 * positions, and the keys laid out in the room the buttons leave - inside it,
 * apart and centred, plus the Japanese naming keys’ kana composition.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "3ds_naming_layout.h"
#include "3ds_naming_kana.h"

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

static void layout(const char *name, uint32_t spacer, uint32_t gaps, int cols, int keys, int minWidth)
{
    struct CtrNamingKeys k;
    int first = -1, last = -1, prev = -1, widths = 0;

    CtrNaming_LayoutKeys(spacer, gaps, cols, AREA_X, AREA_W, &k);
    check(k.count == keys, "the number of keys", name);
    check(k.width >= minWidth && k.width <= CTR_NAMING_KEY_MAX_W, "the key's width", name);
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
}

static void checkKanaMarks(void)
{
    for (int script = 0; script <= 0x50; script += 0x50)
    {
        for (int i = 0; i < 15; ++i)
        {
            int base = script + 0x06 + i;
            check(CtrNaming_ApplyKanaMark(base, CTR_NAMING_DAKUTEN) == script + 0x37 + i,
                  "dakuten applies to every K/S/T-row kana", "kana marks");
            check(CtrNaming_ApplyKanaMark(base, CTR_NAMING_HANDAKUTEN) == base,
                  "handakuten leaves K/S/T-row kana alone", "kana marks");
        }
        for (int i = 0; i < 5; ++i)
        {
            int base = script + 0x1A + i;
            int voiced = script + 0x46 + i;
            int semivoiced = script + 0x4B + i;
            check(CtrNaming_ApplyKanaMark(base, CTR_NAMING_DAKUTEN) == voiced,
                  "dakuten applies to every H-row kana", "kana marks");
            check(CtrNaming_ApplyKanaMark(base, CTR_NAMING_HANDAKUTEN) == semivoiced,
                  "handakuten applies to every H-row kana", "kana marks");
            check(CtrNaming_ApplyKanaMark(voiced, CTR_NAMING_HANDAKUTEN) == semivoiced
                  && CtrNaming_ApplyKanaMark(semivoiced, CTR_NAMING_DAKUTEN) == voiced,
                  "one mark can replace the other", "kana marks");
        }
    }
    check(CtrNaming_ApplyKanaMark(0x6D, CTR_NAMING_DAKUTEN) == 0x99, "ヘ becomes ベ", "kana marks");
    check(CtrNaming_ApplyKanaMark(0xFF, CTR_NAMING_DAKUTEN) == 0xFF, "empty name stays empty", "kana marks");
    check(CtrNaming_ApplyKanaMark(0x00, CTR_NAMING_DAKUTEN) == 0x00, "space stays a space", "kana marks");
    check(CtrNaming_ApplyKanaMark(0xA1, CTR_NAMING_HANDAKUTEN) == 0xA1, "number stays a number", "kana marks");
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

    layout("letters", 0, g, 8, 8, 16);
    layout("symbols", 0, 0, 6, 6, 16);
    /* The nine-slot page with its last column empty (a spacer): the eight keys of the letters, as with eight columns. */
    layout("slots9", 1u << 8, CtrNaming_GapsFromPositions(slots9, 9), 9, 8, 16);
    CtrNaming_LayoutKeys(1u << 8, CtrNaming_GapsFromPositions(slots9, 9), 9, AREA_X, AREA_W, &k);
    check(k.width == 26 && k.x[8] == -1 && k.x[7] == 8 + 230 - 26, "an empty ninth column leaves the letters as they were", "slots9");
    layout("nine", 0, CtrNaming_GapsFromPositions(nine, 9), 9, 9, 16);
    /* Japanese: eleven columns, the sixth a space in every row between two groups of five. */
    layout("kana", 1u << 5, (1u << 4) | (1u << 5), 11, 10, 16);
    /* The rightmost native key and the added mark column must remain reachable. */
    layout("wide kana", (1u << 5) | (1u << 16), 0, CTR_NAMING_LAYOUT_COLS, 18, 10);
    checkKanaMarks();
    /* Nothing laid out is no keys, not a division by zero. */
    CtrNaming_LayoutKeys(0xFFF, 0, 12, AREA_X, AREA_W, &k);
    check(k.count == 0 && k.width == 0, "a page of spacers", "empty");

    if (failures)
        return 1;
    printf("PASS naming layout and Japanese kana marks\n");
    return 0;
}
