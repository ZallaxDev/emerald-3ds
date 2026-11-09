#ifndef LR_RNG_H
#define LR_RNG_H

#include <stdint.h>

typedef struct { uint32_t state; } LrRng;

void LrRngSeed(LrRng *rng, uint32_t seed);
uint32_t LrRandomNext(LrRng *rng);
int32_t LrRandomRange(LrRng *rng, int32_t lo, int32_t hi);

#endif
