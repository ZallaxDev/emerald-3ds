#!/usr/bin/env python3
"""Underwater (Dive) wild encounters: the data reaches them, the visible ones meet them.

Run from 3ds_port: python3 tests/underwater_encounters_test.py --tree ..

The game meets wild Pokemon under the sea on the seaweed: a tile whose
behaviour is surfable and has encounters (MetatileBehavior_IsWaterWildEncounter),
with the map's water table (StandardWildEncounter). Checked here from the
tree's own data, with no generated header: every underwater map with a wild
table has its water one and seaweed one can swim onto, and the visible wild
Pokemon (wild_encounter.c) count a diving player as in the water, as they do
a surfing one.
"""
import argparse
import json
from pathlib import Path
import re
import struct

NUM_METATILES_IN_PRIMARY = 512
METATILE_ID_MASK = 0x03FF
COLLISION_MASK = 0x0C00
BEHAVIOR_MASK = 0x00FF


def behaviors(tree):
    header = (tree / 'include/constants/metatile_behaviors.h').read_text()
    names = re.findall(r'^\s+(MB_\w+)\s*(?:=\s*\w+)?\s*,', header, re.M)
    return {name: index for index, name in enumerate(names)}


def water_encounter_behaviors(tree, ids):
    source = (tree / 'src/metatile_behavior.c').read_text(encoding='latin-1')
    flags = {}
    for name, value in re.findall(r'\[(MB_\w+)\]\s*=\s*([^,\n]+),', source):
        flags[ids[name]] = value
    return {mb for mb, value in flags.items()
            if 'TILE_FLAG_SURFABLE' in value and 'TILE_FLAG_HAS_ENCOUNTERS' in value}


def tileset_dirs(tree):
    """Tileset symbol -> its directory, from the headers' attribute incbins."""
    dirs = {}
    for header in ('src/data/tilesets/headers.h', 'src/data/tilesets/metatiles.h'):
        text = (tree / header).read_text(encoding='latin-1')
        for label, path in re.findall(r'(gMetatileAttributes_\w+)\[\]\s*=\s*INCBIN_U16\("([^"]+)"\)', text):
            dirs[label] = Path(path).parent
    headers = (tree / 'src/data/tilesets/headers.h').read_text(encoding='latin-1')
    tilesets = {}
    for name, body in re.findall(r'const struct Tileset (gTileset_\w+)\s*=\s*\{(.*?)\};', headers, re.S):
        match = re.search(r'\.metatileAttributes\s*=\s*(gMetatileAttributes_\w+)', body)
        if match and match.group(1) in dirs:
            tilesets[name] = dirs[match.group(1)]
    return tilesets


def attributes(tree, directory):
    data = (tree / directory / 'metatile_attributes.bin').read_bytes()
    return struct.unpack('<%dH' % (len(data) // 2), data)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--tree', type=Path, required=True)
    args = parser.parse_args()
    tree = args.tree.resolve()

    ids = behaviors(tree)
    seaweed = water_encounter_behaviors(tree, ids)
    assert ids['MB_SEAWEED'] in seaweed and ids['MB_SEAWEED_NO_SURFACING'] in seaweed, seaweed

    layouts = {layout['id']: layout for layout in
               json.loads((tree / 'data/layouts/layouts.json').read_text())['layouts'] if 'id' in layout}
    tilesets = tileset_dirs(tree)
    underwater = {}
    for map_json in sorted((tree / 'data/maps').glob('*/map.json')):
        data = json.loads(map_json.read_text())
        if data.get('map_type') == 'MAP_TYPE_UNDERWATER':
            underwater[data['id']] = data['layout']

    wild = json.loads((tree / 'src/data/wild_encounters.json').read_text())
    entries = {entry['map']: entry for group in wild['wild_encounter_groups']
               for entry in group['encounters'] if 'map' in entry}
    met = sorted(name for name in underwater if name in entries)
    assert {'MAP_UNDERWATER_ROUTE124', 'MAP_UNDERWATER_ROUTE126'} <= set(met), met

    for name in met:
        entry = entries[name]
        water = entry.get('water_mons')
        assert water is not None, name + ': an underwater table with no water Pokemon'
        assert water['encounter_rate'] > 0 and len(water['mons']) == 5, name
        assert 'land_mons' not in entry, name + ': under the sea every encounter is a water one'

        layout = layouts[underwater[name]]
        primary = attributes(tree, tilesets[layout['primary_tileset']])
        secondary = attributes(tree, tilesets[layout['secondary_tileset']])
        blocks = (tree / layout['blockdata_filepath']).read_bytes()
        swimmable = 0
        for block in struct.unpack('<%dH' % (len(blocks) // 2), blocks):
            metatile = block & METATILE_ID_MASK
            table, index = (primary, metatile) if metatile < NUM_METATILES_IN_PRIMARY \
                else (secondary, metatile - NUM_METATILES_IN_PRIMARY)
            if index < len(table) and (table[index] & BEHAVIOR_MASK) in seaweed and not block & COLLISION_MASK:
                swimmable += 1
        assert swimmable > 0, name + ': no seaweed to meet its Pokemon on'

    # The visible wild Pokemon: the player is in the water surfing or diving,
    # and nowhere in their code is the water told by the surfing flag alone.
    source = (tree / 'src/wild_encounter.c').read_text(encoding='latin-1')
    start = source.index('Visible wild Pokemon (OPTIONS, ENHANCEMENTS)')
    end = source.index('bool8 StandardWildEncounter(')
    visible = source[start:end]
    helper = re.search(r'static bool8 VisiblePlayerInWater\(void\)\s*\{(.*?)\n\}', visible, re.S)
    assert helper, 'VisiblePlayerInWater is gone'
    assert 'PLAYER_AVATAR_FLAG_UNDERWATER' in helper.group(1) and 'PLAYER_AVATAR_FLAG_SURFING' in helper.group(1)
    outside = visible.replace(helper.group(0), '')
    assert 'PLAYER_AVATAR_FLAG_SURFING' not in outside, 'a visible wild check reads surfing, not VisiblePlayerInWater'
    assert len(re.findall(r'v->water [!=]= inWater', outside)) >= 3, 'the touch checks no longer compare with the water'

    print('PASS underwater encounters: %s have water tables and seaweed to swim onto; '
          'the visible ones meet a diving player' % ', '.join(met))


if __name__ == '__main__':
    main()
