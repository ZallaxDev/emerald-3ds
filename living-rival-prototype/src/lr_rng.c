#include "lr_rng.h"

#define LR_RNG_MUL 1103515245u
#define LR_RNG_ADD 24691u

void LrRngSeed(LrRng *rng, uint32_t seed) {
    rng->state = seed;
}

uint32_t LrRandomNext(LrRng *rng) {
    rng->state = LR_RNG_MUL * rng->state + LR_RNG_ADD;
    return rng->state >> 16;
}

int32_t LrRandomRange(LrRng *rng, int32_t lo, int32_t hi) {
    if (hi <= lo) return lo;
    return lo + (int32_t)(LrRandomNext(rng) % (uint32_t)(hi - lo));
}
