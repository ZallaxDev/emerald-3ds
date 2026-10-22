/*
 * The seeded species randomizer's OPTIONS cells (include/3ds_randomizer.h),
 * for the coordinator to put in gCtrExtras[] of src/3ds_extras.c, before its
 * last line ({0}), with #include "3ds_randomizer.h" at the top of that file:
 *
 *     #include "3ds_randomizer_extras.inc"
 *
 * A RANDOMIZER cell on CHEATS' first screen opens a third screen of its own.
 * Every value is off (seed 0) until set; the game reads them from
 * settings.txt (rand_wild, rand_starters, rand_trainers, rand_seed) whether
 * or not these cells are in the build.
 */
    {CTR_EXTRAS_CHEATS, CTR_TEXT("RANDOMIZER", "ALEATORIO", "ALÉATOIRE", "CASUALE", "RANDOMIZER", "ALEATÓRIO"), "rand_open", 0, 0, gCtrRandomizerOpenText, CtrRandomizer_Open, NULL, NULL},
    {CTR_EXTRAS_SCREEN(CTR_EXTRAS_CHEATS, 2), CTR_TEXT("RANDOMIZER", "ALEATORIO", "ALÉATOIRE", "CASUALE", "RANDOMIZER", "ALEATÓRIO"), "rand_back", 0, 0, gCtrRandomizerBackText, CtrRandomizer_Back, NULL, NULL},
    {CTR_EXTRAS_SCREEN(CTR_EXTRAS_CHEATS, 2), CTR_TEXT("RANDOM WILD", "SALVAJES AL AZAR", "SAUV. ALÉAT.", "SELV. CASUALI", "WILDE PKMN ZUF.", "SELVAGENS ALEAT."), "rand_wild", 2, 0, gCtrExtrasOffOn, NULL, NULL, NULL},
    {CTR_EXTRAS_SCREEN(CTR_EXTRAS_CHEATS, 2), CTR_TEXT("RANDOM STARTERS", "INICIALES AL AZAR", "DÉPARTS ALÉAT.", "INIZIALI CAS.", "STARTER ZUF.", "INICIAIS ALEAT."), "rand_starters", 2, 0, gCtrExtrasOffOn, NULL, NULL, NULL},
    {CTR_EXTRAS_SCREEN(CTR_EXTRAS_CHEATS, 2), CTR_TEXT("RANDOM TRAINERS", "ENTREN. AL AZAR", "DRESS. ALÉAT.", "ALLEN. CASUALI", "TRAINER-PKMN ZUF.", "TREIN. ALEAT."), "rand_trainers", 2, 0, gCtrExtrasOffOn, NULL, NULL, NULL},
    {CTR_EXTRAS_SCREEN(CTR_EXTRAS_CHEATS, 2), CTR_TEXT("SEED", "SEMILLA", "GRAINE", "SEME", "SEED", "SEMENTE"), "rand_seed", 0, 0, NULL, NULL, CtrRandomizer_StepSeed, CtrRandomizer_SeedText},
    {CTR_EXTRAS_SCREEN(CTR_EXTRAS_CHEATS, 2), CTR_TEXT("NEW SEED", "NUEVA SEMILLA", "NOUV. GRAINE", "NUOVO SEME", "NEUER SEED", "NOVA SEMENTE"), "rand_new_seed", 0, 0, gCtrRandomizerNewSeedText, CtrRandomizer_NewSeed, NULL, NULL},
