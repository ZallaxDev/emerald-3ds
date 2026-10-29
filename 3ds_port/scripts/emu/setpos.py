"""Put the player of the emulator's save at a map and tile, to look at a place
of the game in Azahar without walking there.

    python setpos.py SAVE MAP_GROUP MAP_NUM X Y

Writes the newest save slot's SaveBlock1 position, location, layout and the
player's object event (sector 1) and fixes its checksum. The map group and number are those of
include/constants/map_groups.h (Route 106: 0 21). Keep a copy of the save.
"""
import json
import os
import struct
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
SECTOR = 4096
SIZES = {0: 3884, 13: 2000}


def checksum(data):
    total = 0
    for i in range(0, len(data) - len(data) % 4, 4):
        total = (total + struct.unpack_from("<I", data, i)[0]) & 0xFFFFFFFF
    return ((total >> 16) + (total & 0xFFFF)) & 0xFFFF


def layout_id(group, num):
    """The map's layout as SaveBlock1.mapLayoutId counts it (1-based)."""
    data = os.path.join(ROOT, "data", "maps")
    groups = json.load(open(os.path.join(data, "map_groups.json"), encoding="utf-8"))
    name = groups[groups["group_order"][group]][num]
    layout = json.load(open(os.path.join(data, name, "map.json"), encoding="utf-8"))["layout"]
    layouts = json.load(open(os.path.join(ROOT, "data", "layouts", "layouts.json"),
                             encoding="utf-8"))["layouts"]
    return next(i + 1 for i, e in enumerate(layouts) if e.get("id") == layout)


def main():
    path, group, num, x, y = sys.argv[1], *map(int, sys.argv[2:6])
    layout = layout_id(group, num)
    b = bytearray(open(path, "rb").read())
    best = None
    for slot in range(2):
        counter = struct.unpack_from("<I", b, slot * 14 * SECTOR + 0xFFC)[0]
        if best is None or counter > best[0]:
            best = (counter, slot)
    base = best[1] * 14 * SECTOR
    for i in range(14):
        o = base + i * SECTOR
        sid = struct.unpack_from("<H", b, o + 0xFF4)[0]
        if sid != 1:
            continue
        old = struct.unpack_from("<hhbb", b, o)
        size = SIZES.get(sid, 3968)
        assert checksum(b[o:o + size]) == struct.unpack_from("<H", b, o + 0xFF6)[0], "checksum model is wrong"
        struct.pack_into("<hhbb", b, o, x, y, group, num)
        # the saved view of the map round the player (SaveBlock1.mapView): on
        # Continue it is copied over the map, so a moved player would find
        # the old surroundings pasted into the new place
        b[o + 0x34:o + 0x34 + 0x200] = bytes(0x200)
        # and the layout a Continue loads the map with (GetMapLayout): left
        # as it was, the new place is drawn with the old map's cells
        struct.pack_into("<H", b, o + 0x32, layout)
        # the saved object events (SaveBlock1.objectEvents, 16 of 0x24
        # bytes): a Continue restores the player's from them, not from pos,
        # so the player is moved there too (map coords + MAP_OFFSET 7), and
        # the old map's people are dropped rather than left standing about
        for e in range(16):
            at = o + 0xA30 + e * 0x24
            if b[at] & 1 and b[at + 2] & 1:
                b[at + 9], b[at + 10] = num, group
                for c in (0x0C, 0x10, 0x14):
                    struct.pack_into("<hh", b, at + c, x + 7, y + 7)
            else:
                b[at] &= ~1
        # the warps a Continue may use as well: continueGameWarp, dynamicWarp
        for at in (0x0C, 0x14):
            struct.pack_into("<bbbxhh", b, o + at, group, num, -1, x, y)
        struct.pack_into("<H", b, o + 0xFF6, checksum(b[o:o + size]))
        print("slot %d sector %d: %s -> %s" % (best[1], i, old, (x, y, group, num)))
        open(path, "wb").write(b)
        return
    raise SystemExit("no sector 1")


main()
