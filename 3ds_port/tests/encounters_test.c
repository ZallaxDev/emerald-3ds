#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include "3ds_encounters.h"

int main(void)
{
    static const CtrTimedEncounterRule rules[] = {
        {0, 16, 2, 10, {20, 10, 30}},
        {0, 17, 2, 10, {40, 50, 60}},
    };

    assert(CtrEncounters_Phase(0) == CTR_TIME_NIGHT);
    assert(CtrEncounters_Phase(4 * 3600 - 1) == CTR_TIME_NIGHT);
    assert(CtrEncounters_Phase(4 * 3600) == CTR_TIME_MORNING);
    assert(CtrEncounters_Phase(10 * 3600 - 1) == CTR_TIME_MORNING);
    assert(CtrEncounters_Phase(10 * 3600) == CTR_TIME_DAY);
    assert(CtrEncounters_Phase(20 * 3600 - 1) == CTR_TIME_DAY);
    assert(CtrEncounters_Phase(20 * 3600) == CTR_TIME_NIGHT);
    assert(CtrEncounters_Phase(86400) == CTR_TIME_NIGHT);
    assert(CtrEncounters_Phase(86400 + 4 * 3600) == CTR_TIME_MORNING);
    for (unsigned phase = 0; phase < CTR_TIME_COUNT; ++phase)
    {
        assert(CtrEncounters_Resolve(rules, 2, 0, 16, 2, 10, phase) == rules[0].species[phase]);
        assert(CtrEncounters_Resolve(rules, 2, 0, 17, 2, 10, phase) == rules[1].species[phase]);
        assert(CtrEncounters_Resolve(rules, 2, 1, 16, 2, 10, phase) == 10);
        assert(CtrEncounters_Resolve(rules, 2, 0, 18, 2, 10, phase) == 10);
        assert(CtrEncounters_Resolve(rules, 2, 0, 16, 3, 10, phase) == 10);
        assert(CtrEncounters_Resolve(rules, 2, 0, 16, 2, 99, phase) == 99);
    }
    assert(CtrEncounters_Resolve(rules, 2, 0, 16, 2, 10, CTR_TIME_COUNT) == 10);
    assert(CtrEncounters_Resolve(NULL, 0, 0, 16, 2, 10, CTR_TIME_NIGHT) == 10);
    /* The water ones are met surfing and diving (on an underwater map's
     * seaweed the player is not surfing); the land ones only on foot. */
    assert(!CtrEncounters_PlayerInWater(false, false));
    assert(CtrEncounters_PlayerInWater(true, false));
    assert(CtrEncounters_PlayerInWater(false, true));
    assert(CtrEncounters_PlayerInWater(true, true));
    puts("PASS encounters: time boundaries, midnight, route/slot/species guards, original fallbacks, "
         "water while surfing or diving");
    return 0;
}
