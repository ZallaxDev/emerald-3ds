#include "lr_facet_tables.h"

/* Columns: Anger, Bravery, Ambition, Loyalty, Honesty, Cruelty, Social, Discipline.
 * Rows: Hothead, Coward, Proud, Cold, Greedy, Loyal, Drifter, Steady. */
static const int8_t sUniversalOffsets[LR_TEMPERAMENT_COUNT][LR_UNIVERSAL_FACET_COUNT] = {
    [LR_TEMP_HOTHEAD] = {  30,  10,   5,   0,   0,   0,  -5, -10 },
    [LR_TEMP_COWARD]  = { -10, -35,  -5,   0,  -5,   0,   5,   5 },
    [LR_TEMP_PROUD]   = {   0,  10,  10,   0,   0,   0,   0,   5 },
    [LR_TEMP_COLD]    = { -15,   5,   0,   0,   0,  12, -20,  10 },
    [LR_TEMP_GREEDY]  = {   0,   0,  10,   0, -15,   0,   0,  -5 },
    [LR_TEMP_LOYAL]   = {   0,   0,   0,  35,  10,   0,  10,   0 },
    [LR_TEMP_DRIFTER] = {   0,   0, -25, -15,   0,   0,  10, -10 },
    [LR_TEMP_STEADY]  = {   0,   0,   0,   0,   0,   0,   0,   0 },
};

static const int8_t sZeroOffsets[LR_UNIVERSAL_FACET_COUNT] = { 0, 0, 0, 0, 0, 0, 0, 0 };

static const LrRegionProfile sNeutralRegion = {
    "Neutral", 0, { 0 }, { { 0 } }, -1, -1, -1, -1, -1
};

static const LrRegionProfile sRegions[LR_REGION_COUNT] = {
    [LR_REGION_KANTO] = {
        .name = "Kanto",
        .facetCount = 3,
        .facetNames = { "Honor", "Innovation", "Underworld" },
        .temperamentOffsets = {
            [LR_TEMP_HOTHEAD] = { -5,  0,  5 },
            [LR_TEMP_COWARD]  = {  0,  0, -5 },
            [LR_TEMP_PROUD]   = { 10,  0,  0 },
            [LR_TEMP_COLD]    = { -5,  5,  5 },
            [LR_TEMP_GREEDY]  = { -5,  5,  5 },
            [LR_TEMP_LOYAL]   = {  5,  0,  0 },
            [LR_TEMP_DRIFTER] = { -10, 5,  0 },
            [LR_TEMP_STEADY]  = {  0,  0,  0 },
        },
        .familyFacet = -1, .lawFacet = 0, .spiritFacet = -1, .moneyFacet = 2, .prideFacet = 0,
    },
    [LR_REGION_JOHTO] = {
        .name = "Johto",
        .facetCount = 4,
        .facetNames = { "Tradition", "Harmony", "Craftsmanship", "Mysticism" },
        .temperamentOffsets = {
            [LR_TEMP_HOTHEAD] = { -5, -5, 0, 0 },
            [LR_TEMP_COWARD]  = {  0,  5, 0, 0 },
            [LR_TEMP_PROUD]   = {  5,  0, 5, 0 },
            [LR_TEMP_COLD]    = { -5,  0, 5, 5 },
            [LR_TEMP_GREEDY]  = { -5,  0, 5, 0 },
            [LR_TEMP_LOYAL]   = { 10,  5, 0, 0 },
            [LR_TEMP_DRIFTER] = { -10, 0, 0, 5 },
            [LR_TEMP_STEADY]  = {  0,  0, 0, 0 },
        },
        .familyFacet = 1, .lawFacet = 0, .spiritFacet = 3, .moneyFacet = -1, .prideFacet = 0,
    },
    [LR_REGION_HOENN] = {
        .name = "Hoenn",
        .facetCount = 3,
        .facetNames = { "Nature", "Explorer", "Ancient" },
        .temperamentOffsets = {
            [LR_TEMP_HOTHEAD] = { -5,  5,  0 },
            [LR_TEMP_COWARD]  = {  0, -10, 0 },
            [LR_TEMP_PROUD]   = {  0,  5,  5 },
            [LR_TEMP_COLD]    = {  0,  0,  5 },
            [LR_TEMP_GREEDY]  = {  0,  0,  5 },
            [LR_TEMP_LOYAL]   = {  5,  0,  0 },
            [LR_TEMP_DRIFTER] = {  5,  5,  0 },
            [LR_TEMP_STEADY]  = {  0,  0,  0 },
        },
        .familyFacet = 0, .lawFacet = -1, .spiritFacet = 2, .moneyFacet = -1, .prideFacet = 1,
    },
    [LR_REGION_SINNOH] = {
        .name = "Sinnoh",
        .facetCount = 3,
        .facetNames = { "Myth", "Research", "Willpower" },
        .temperamentOffsets = {
            [LR_TEMP_HOTHEAD] = { 0, -5,  5 },
            [LR_TEMP_COWARD]  = { 0,  0, -10 },
            [LR_TEMP_PROUD]   = { 0,  0, 10 },
            [LR_TEMP_COLD]    = { 5, 10,  0 },
            [LR_TEMP_GREEDY]  = { 0,  5,  0 },
            [LR_TEMP_LOYAL]   = { 5,  0,  5 },
            [LR_TEMP_DRIFTER] = { 5,  0, -5 },
            [LR_TEMP_STEADY]  = { 0,  0,  0 },
        },
        .familyFacet = -1, .lawFacet = -1, .spiritFacet = 0, .moneyFacet = -1, .prideFacet = 0,
    },
    [LR_REGION_UNOVA] = {
        .name = "Unova",
        .facetCount = 3,
        .facetNames = { "Ideals", "Industry", "Dragonspiral" },
        .temperamentOffsets = {
            [LR_TEMP_HOTHEAD] = {  0,  0, 5 },
            [LR_TEMP_COWARD]  = {  0,  0, 0 },
            [LR_TEMP_PROUD]   = {  5,  5, 0 },
            [LR_TEMP_COLD]    = { -5,  5, 0 },
            [LR_TEMP_GREEDY]  = {  0, 10, 0 },
            [LR_TEMP_LOYAL]   = { 10,  0, 0 },
            [LR_TEMP_DRIFTER] = {  0, -5, 5 },
            [LR_TEMP_STEADY]  = {  0,  0, 0 },
        },
        .familyFacet = -1, .lawFacet = -1, .spiritFacet = 2, .moneyFacet = 1, .prideFacet = 0,
    },
};

static const LrFactionProfile sNeutralFaction = {
    "Neutral", 0, { 0 }, { { 0 } }
};

static const LrFactionProfile sFactions[LR_FACTION_COUNT] = {
    [LR_FACTION_GYM_CIRCUIT] = {
        .name = "GymCircuit",
        .facetCount = 1,
        .facetNames = { "Prestige" },
        .archetypeOffsets = {
            [LR_ARCH_PRODIGY] = { 10 }, [LR_ARCH_GRINDER] = { 5 },
            [LR_ARCH_SPECIALIST] = { 5 }, [LR_ARCH_EXPERIMENTER] = { -5 },
            [LR_ARCH_LATE_BLOOMER] = { -5 }, [LR_ARCH_MENTOR] = { 5 },
            [LR_ARCH_MERCENARY] = { 10 }, [LR_ARCH_LEGACY] = { 5 },
        },
    },
    [LR_FACTION_ELITE_FOUR_PATH] = {
        .name = "EliteFourPath",
        .facetCount = 1,
        .facetNames = { "Sovereignty" },
        .archetypeOffsets = {
            [LR_ARCH_PRODIGY] = { 5 }, [LR_ARCH_GRINDER] = { 0 },
            [LR_ARCH_SPECIALIST] = { 0 }, [LR_ARCH_EXPERIMENTER] = { -5 },
            [LR_ARCH_LATE_BLOOMER] = { 0 }, [LR_ARCH_MENTOR] = { 5 },
            [LR_ARCH_MERCENARY] = { -5 }, [LR_ARCH_LEGACY] = { 10 },
        },
    },
    [LR_FACTION_TEAM_MAGMA] = {
        .name = "TeamMagma",
        .facetCount = 1,
        .facetNames = { "Zealotry" },
        .archetypeOffsets = {
            [LR_ARCH_PRODIGY] = { 0 }, [LR_ARCH_GRINDER] = { 5 },
            [LR_ARCH_SPECIALIST] = { 5 }, [LR_ARCH_EXPERIMENTER] = { 0 },
            [LR_ARCH_LATE_BLOOMER] = { 0 }, [LR_ARCH_MENTOR] = { 0 },
            [LR_ARCH_MERCENARY] = { 5 }, [LR_ARCH_LEGACY] = { 0 },
        },
    },
    [LR_FACTION_TEAM_AQUA] = {
        .name = "TeamAqua",
        .facetCount = 1,
        .facetNames = { "Zealotry" },
        .archetypeOffsets = {
            [LR_ARCH_PRODIGY] = { 0 }, [LR_ARCH_GRINDER] = { 5 },
            [LR_ARCH_SPECIALIST] = { 5 }, [LR_ARCH_EXPERIMENTER] = { 0 },
            [LR_ARCH_LATE_BLOOMER] = { 0 }, [LR_ARCH_MENTOR] = { 0 },
            [LR_ARCH_MERCENARY] = { 5 }, [LR_ARCH_LEGACY] = { 0 },
        },
    },
    [LR_FACTION_CONTEST_STAR] = {
        .name = "ContestStar",
        .facetCount = 1,
        .facetNames = { "Appeal" },
        .archetypeOffsets = {
            [LR_ARCH_PRODIGY] = { 5 }, [LR_ARCH_GRINDER] = { 0 },
            [LR_ARCH_SPECIALIST] = { 0 }, [LR_ARCH_EXPERIMENTER] = { 10 },
            [LR_ARCH_LATE_BLOOMER] = { 0 }, [LR_ARCH_MENTOR] = { 0 },
            [LR_ARCH_MERCENARY] = { -5 }, [LR_ARCH_LEGACY] = { 5 },
        },
    },
    [LR_FACTION_BATTLE_FRONTIER] = {
        .name = "BattleFrontier",
        .facetCount = 1,
        .facetNames = { "Adaptability" },
        .archetypeOffsets = {
            [LR_ARCH_PRODIGY] = { 5 }, [LR_ARCH_GRINDER] = { 5 },
            [LR_ARCH_SPECIALIST] = { -5 }, [LR_ARCH_EXPERIMENTER] = { 10 },
            [LR_ARCH_LATE_BLOOMER] = { 0 }, [LR_ARCH_MENTOR] = { 0 },
            [LR_ARCH_MERCENARY] = { 0 }, [LR_ARCH_LEGACY] = { 0 },
        },
    },
    [LR_FACTION_INDEPENDENT] = {
        .name = "Independent",
        .facetCount = 0,
    },
    [LR_FACTION_ROOKIE_LEAGUE] = {
        .name = "RookieLeague",
        .facetCount = 0,
    },
};

const LrRegionProfile *LrGetRegionProfile(uint8_t region) {
    if (region >= LR_REGION_COUNT) return &sNeutralRegion;
    return &sRegions[region];
}

const LrFactionProfile *LrGetFactionProfile(uint8_t faction) {
    if (faction >= LR_FACTION_COUNT) return &sNeutralFaction;
    return &sFactions[faction];
}

const int8_t *LrUniversalTemperamentOffsets(uint8_t temperament) {
    if (temperament >= LR_TEMPERAMENT_COUNT) return sZeroOffsets;
    return sUniversalOffsets[temperament];
}

uint8_t LrRegionFacetCount(uint8_t region) {
    return LrGetRegionProfile(region)->facetCount;
}

uint8_t LrFactionFacetCount(uint8_t faction) {
    return LrGetFactionProfile(faction)->facetCount;
}
