#!/usr/bin/env python3
"""Compile-test the real read-only Bag snapshot with synthetic pocket data.

No Pokemon ROM or devkitARM is needed. Keep the production snapshot function
in the test, not a reimplementation: this catches indexing and clamp errors.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
options = (ROOT / "3ds_port/src/3ds_bottom_options.inc").read_text(encoding="utf-8")
extras = (ROOT / "3ds_port/src/3ds_extras.c").read_text(encoding="utf-8")
actions = (ROOT / "3ds_port/src/3ds_bottom_actions.inc").read_text(encoding="utf-8")
# Both toggles are registered default-off and exposed once, with no silent
# truncation of group rows or category tabs.
for key in ("live_party", "live_bag"):
    assert options.count(f'EXTRA_ROW("{key}")') == 1
    assert re.search(rf'"{key}", 2, 0, gCtrExtrasOffOn', extras)
rows = dict(re.findall(r'static const OptRowDef (sRows\w+)\[\] = \{(.*?)\n\};', options, re.S))
groups = re.findall(r'static const OptGroupDef (sGroups\w+)\[\] = \{(.*?)\n\};', options, re.S)
assert "sRowsLive" in rows
assert len(re.findall(r'EXTRA_ROW\(', rows["sRowsLive"])) == 2
assert any("sRowsLive" in body and name == "sGroupsGeneral" for name, body in groups)
for name, body in rows.items():
    assert len(re.findall(r'GAME_ROW\(|EXTRA_ROW\(|\{ROW_INFO', body)) <= 6, name
for name, body in groups:
    assert len(re.findall(r'\bGROUP\(', body)) <= 4, name

snapshot = (ROOT / "3ds_port/src/3ds_bottom_snapshot.inc").read_text(encoding="utf-8")
start = snapshot.index("static void SnapshotLiveBag(ViewState *s)\n{")
end = snapshot.index("\n/* A stored selection can outlive", start)
fn = snapshot[start:end]
swipe_start = actions.index("static int LiveBagSwipeRows(int startY, int endY)\n{")
swipe_end = actions.index("\n}\n", swipe_start) + len("\n}\n")
swipe_fn = actions[swipe_start:swipe_end]

fixture = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef unsigned char u8;
typedef unsigned short u16;
typedef u8 bool8;
#define TRUE 1
#define FALSE 0
#define FEMALE 1
#define ITEM_NONE 0
#define ITEMS_POCKET 0
#define TMHM_POCKET 2
#define BERRIES_POCKET 3
#define KEYITEMS_POCKET 4
#define POCKETS_COUNT 5
#define CTR_BAG_ROWS 8
#define BAG_ROW_H 18
#define CTR_BAG_NAME 24
#define CTR_VIEW_TEXT 160
#define CTR_BAG_VIEW_LIST 1
#define CTR_BAG_ROW_COUNT 1
#define CTR_BAG_ROW_REGISTERED 2

struct ItemSlot { u16 itemId, quantity; };
struct BagPocket { struct ItemSlot *itemSlots; u8 capacity; };
struct BagPocket gBagPockets[POCKETS_COUNT];
struct SaveBlock1 { u16 registeredItem; };
static struct SaveBlock1 saveBlock;
static struct SaveBlock1 *gSaveBlock1Ptr = &saveBlock;
struct CtrBagView {
    u8 state, pocket, bagFrame, female, pocketSwitch, numRows, cursor;
    signed char swapRow;
    u16 scroll, total, items[CTR_BAG_ROWS], counts[CTR_BAG_ROWS];
    u8 flags[CTR_BAG_ROWS], names[CTR_BAG_ROWS][CTR_BAG_NAME];
    u16 item;
    u8 text[CTR_VIEW_TEXT];
};
typedef struct { struct CtrBagView bv; u8 gender; } ViewState;
static u8 sLiveBagPocket;
static u8 sLiveBagCursor[POCKETS_COUNT];
static u8 sLiveBagScroll[POCKETS_COUNT];

static void CopyText(u8 *dst, int size, const u8 *src)
{
    int n = 0;
    while (src && n < size - 1 && src[n])
        dst[n] = src[n], ++n;
    dst[n] = 0;
}
static u16 BagGetItemIdByPocketPosition(u8 pocket, u16 pos)
{
    assert(pocket >= 1 && pocket <= POCKETS_COUNT);
    assert(pos < gBagPockets[pocket - 1].capacity);
    return gBagPockets[pocket - 1].itemSlots[pos].itemId;
}
static u16 BagGetQuantityByPocketPosition(u8 pocket, u16 pos)
{
    assert(pocket >= 1 && pocket <= POCKETS_COUNT);
    assert(pos < gBagPockets[pocket - 1].capacity);
    return gBagPockets[pocket - 1].itemSlots[pos].quantity;
}
static const u8 *GetItemName(u16 item) { (void)item; return (const u8 *)"POTION"; }
static const u8 *GetItemDescription(u16 item) { (void)item; return (const u8 *)"A useful item"; }

/* PRODUCTION_SNAPSHOT */

int main(void)
{
    struct ItemSlot items[14] = {{0}};
    struct ItemSlot keys[3] = {{42, 1}, {43, 1}, {0, 0}};
    struct ItemSlot tms[4] = {{50, 1}, {0, 0}, {30, 1}, {40, 1}};
    unsigned total;
    ViewState state = {0};

    /* Horizontal dragging and vertical jitter must not cause scrolling. */
    assert(LiveBagSwipeRows(100, 100) == 0);
    assert(LiveBagSwipeRows(100, 99) == 0);
    assert(LiveBagSwipeRows(100, 108) == 0);
    assert(LiveBagSwipeRows(100, 89) == 1);
    assert(LiveBagSwipeRows(100, 111) == -1);
    assert(LiveBagSwipeRows(100, 64) == 2);
    assert(LiveBagSwipeRows(100, 136) == -2);

    /* Overfull list: selected row #12 must be visible in the final page. */
    for (unsigned i = 0; i < 13; ++i)
    {
        items[i].itemId = i + 1;
        items[i].quantity = i + 2;
    }
    gBagPockets[ITEMS_POCKET].itemSlots = items;
    gBagPockets[ITEMS_POCKET].capacity = 14;
    sLiveBagCursor[ITEMS_POCKET] = 12;
    SnapshotLiveBag(&state);
    assert(state.bv.state == CTR_BAG_VIEW_LIST);
    assert(state.bv.total == 13 && state.bv.numRows == 8);
    assert(state.bv.scroll == 5 && state.bv.cursor == 7);
    assert(state.bv.item == 13 && state.bv.items[7] == 13 && state.bv.counts[7] == 14);
    assert((state.bv.flags[7] & CTR_BAG_ROW_COUNT) != 0);

    /* Consumption outside the Bag leaves holes. Never stop at the first
     * ITEM_NONE, and keep the selected item's compact ordinal. */
    memset(&state, 0, sizeof(state));
    items[3].itemId = ITEM_NONE;
    SnapshotLiveBag(&state);
    assert(state.bv.total == 12 && state.bv.scroll == 0);
    assert(state.bv.numRows == 8 && state.bv.cursor == 0 && state.bv.item == 1);
    assert(state.bv.items[3] == 5 && state.bv.counts[3] == 6);
    assert(LiveBagNativeIndex(ITEMS_POCKET, 3, &total) == 3 && total == 12);

    /* Scroll beyond a gap; display the actual physical item at the selected
     * occupied position, while never mutating or compacting the save. */
    memset(&state, 0, sizeof(state));
    sLiveBagCursor[ITEMS_POCKET] = 11;
    SnapshotLiveBag(&state);
    assert(state.bv.scroll == 4 && state.bv.cursor == 7);
    assert(state.bv.item == 13 && state.bv.items[7] == 13);
    assert(items[3].itemId == ITEM_NONE);

    /* A gap in the first slot must not make the pocket look empty or block
     * ACTIONS from opening the original Bag menu. */
    memset(&state, 0, sizeof(state));
    items[0].itemId = ITEM_NONE;
    sLiveBagCursor[ITEMS_POCKET] = 0;
    SnapshotLiveBag(&state);
    assert(state.bv.total == 11 && state.bv.item == 2);
    assert(state.bv.items[0] == 2 && state.bv.items[2] == 5);
    assert(LiveBagNativeIndex(ITEMS_POCKET, 0, &total) == 0 && total == 11);

    /* The real Bag sorts TMs/berries as it opens: the selected item's
     * native position is its stable item-ID rank, not its physical slot. */
    memset(&state, 0, sizeof(state));
    gBagPockets[TMHM_POCKET].itemSlots = tms;
    gBagPockets[TMHM_POCKET].capacity = 4;
    sLiveBagPocket = TMHM_POCKET;
    sLiveBagCursor[TMHM_POCKET] = 2;
    SnapshotLiveBag(&state);
    assert(state.bv.total == 3 && state.bv.item == 40);
    assert(LiveBagNativeIndex(TMHM_POCKET, 2, &total) == 1 && total == 3);
    assert(tms[0].itemId == 50 && tms[1].itemId == ITEM_NONE);

    /* Registered key items are marked, and all five pockets remain valid. */
    memset(&state, 0, sizeof(state));
    gBagPockets[KEYITEMS_POCKET].itemSlots = keys;
    gBagPockets[KEYITEMS_POCKET].capacity = 3;
    gSaveBlock1Ptr->registeredItem = 42;
    sLiveBagPocket = KEYITEMS_POCKET;
    state.gender = FEMALE;
    SnapshotLiveBag(&state);
    assert(state.bv.pocket == KEYITEMS_POCKET && state.bv.female);
    assert(state.bv.numRows == 2 && state.bv.item == 42);
    assert((state.bv.flags[0] & CTR_BAG_ROW_REGISTERED) != 0);
    assert((state.bv.flags[1] & CTR_BAG_ROW_REGISTERED) == 0);

    /* Empty/uninitialized pockets never dereference their slot pointer. */
    memset(&state, 0, sizeof(state));
    gBagPockets[ITEMS_POCKET].itemSlots = NULL;
    gBagPockets[ITEMS_POCKET].capacity = 0;
    sLiveBagPocket = ITEMS_POCKET;
    SnapshotLiveBag(&state);
    assert(state.bv.total == 0 && state.bv.numRows == 0 && state.bv.item == ITEM_NONE);
    assert(LiveBagNativeIndex(ITEMS_POCKET, 0, &total) == 0xFFFF && total == 0);

    puts("live Bag snapshot fixture passed");
    return 0;
}
'''
source = fixture.replace("/* PRODUCTION_SNAPSHOT */", fn + "\n" + swipe_fn)
assert source != fixture
with tempfile.TemporaryDirectory() as tmp:
    inp = Path(tmp) / "live_bag.c"
    out = Path(tmp) / "live_bag"
    inp.write_text(source, encoding="utf-8")
    subprocess.run(["gcc", "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                    str(inp), "-o", str(out)], check=True)
    subprocess.run([str(out)], check=True)
