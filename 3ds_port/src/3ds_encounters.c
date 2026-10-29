#include "3ds_encounters.h"

unsigned CtrEncounters_Phase(unsigned seconds)
{
    unsigned hour = (seconds % 86400u) / 3600u;

    if (hour >= 4 && hour < 10)
        return CTR_TIME_MORNING;
    return hour >= 10 && hour < 20 ? CTR_TIME_DAY : CTR_TIME_NIGHT;
}

uint16_t CtrEncounters_Resolve(const CtrTimedEncounterRule *rules, size_t count,
                             unsigned mapGroup, unsigned mapNum, unsigned slot,
                             uint16_t original, unsigned phase)
{
    if (phase >= CTR_TIME_COUNT)
        return original;
    for (size_t index = 0; index < count; ++index)
        if (rules[index].mapGroup == mapGroup && rules[index].mapNum == mapNum
         && rules[index].slot == slot && rules[index].expectedSpecies == original)
            return rules[index].species[phase];
    return original;
}

bool CtrEncounters_PlayerInWater(bool surfing, bool underwater)
{
    return surfing || underwater;
}
