"""Exercise production encounter tables against the pinned upstream route data."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--tree', type=Path, required=True)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    backend = ROOT / 'src/3ds_encounter_tables.c'
    data = json.loads((args.tree / 'src/data/wild_encounters.json').read_text())
    routes = {entry['map']: entry for group in data['wild_encounter_groups']
              for entry in group['encounters'] if 'land_mons' in entry and 'map' in entry}
    rules = re.findall(r'RULE\((ROUTE\d+), (\d+), (\w+), (\w+), (\w+), (\w+)\)', backend.read_text())
    seen = set()
    for route, slot, expected, *_ in rules:
        assert (route, slot) not in seen
        seen.add((route, slot))
        assert routes['MAP_' + route]['land_mons']['mons'][int(slot)]['species'] == 'SPECIES_' + expected
    source = r'''
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "3ds_encounter_tables.c"
static int enabled = 1;
static bool outdoor = true, valid = true;
static unsigned current;
int CtrSettings_GetInt(const char *key, int fallback) {
    return strcmp(key, "timed_encounters") == 0 ? enabled : fallback;
}
bool CtrGame_IsOutdoor(void) { return outdoor; }
bool CtrDayNight_Time(unsigned *seconds, unsigned *rate) {
    *seconds = current; *rate = 120; return valid;
}
int main(void) {
    struct WildPokemon mons[12], backup[12];
    struct WildPokemonInfo original = {20, mons};
    for (unsigned index = 0; index < 12; ++index)
        mons[index] = (struct WildPokemon){2 + index, 4 + index, SPECIES_RALTS};
    for (unsigned ruleIndex = 0; ruleIndex < ARRAY_COUNT(sRules); ++ruleIndex) {
        const CtrTimedEncounterRule *rule = &sRules[ruleIndex];
        mons[rule->slot].species = rule->expectedSpecies;
        memcpy(backup, mons, sizeof(mons));
        for (unsigned phase = 0; phase < CTR_TIME_COUNT; ++phase) {
            current = phase == CTR_TIME_MORNING ? 4 * 3600 : phase == CTR_TIME_DAY ? 10 * 3600 : 20 * 3600;
            const struct WildPokemonInfo *result = CtrEncounters_Land(&original, rule->mapGroup, rule->mapNum);
            assert(result->encounterRate == original.encounterRate);
            for (unsigned index = 0; index < 12; ++index) {
                assert(result->wildPokemon[index].minLevel == mons[index].minLevel);
                assert(result->wildPokemon[index].maxLevel == mons[index].maxLevel);
                assert(result->wildPokemon[index].species == (index == rule->slot ? rule->species[phase] : mons[index].species));
            }
            assert(memcmp(backup, mons, sizeof(mons)) == 0);
        }
        enabled = 0;
        assert(CtrEncounters_Land(&original, rule->mapGroup, rule->mapNum) == &original);
        enabled = 99;
        assert(CtrEncounters_Land(&original, rule->mapGroup, rule->mapNum) == &original);
        enabled = 1; outdoor = false;
        assert(CtrEncounters_Land(&original, rule->mapGroup, rule->mapNum) == &original);
        outdoor = true; valid = false;
        assert(CtrEncounters_Land(&original, rule->mapGroup, rule->mapNum) == &original);
        valid = true;
        assert(CtrEncounters_Land(&original, 255, 255) == &original);
        mons[rule->slot].species = SPECIES_RALTS;
        assert(CtrEncounters_Land(&original, rule->mapGroup, rule->mapNum) == &original);
    }
    assert(CtrEncounters_Land(NULL, 0, 0) == NULL);
    original.wildPokemon = NULL;
    assert(CtrEncounters_Land(&original, 0, 0) == &original);
    puts("PASS production encounter tables: every rule/phase, pinned species, original fallback, unchanged levels/rates/data");
}
'''
    with tempfile.TemporaryDirectory(prefix='encounters-') as temporary:
        directory = Path(temporary)
        (directory / 'global.h').write_text(
            '#include <stdint.h>\n#include <stdbool.h>\n'
            'typedef uint8_t u8; typedef uint16_t u16; typedef bool bool8;\n'
            '#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))\n')
        (directory / 'test.c').write_text(source)
        executable = directory / 'test'
        flags = ['-fno-pie', '-no-pie'] if sys.platform.startswith('linux') else []
        subprocess.run([args.cc, '-std=c99', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', *flags, '-I' + str(directory),
                        '-I' + str(args.tree / 'include'), '-I' + str(ROOT / 'include'),
                        '-I' + str(ROOT / 'src'), str(directory / 'test.c'),
                        str(ROOT / 'src/3ds_encounters.c'), '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()
