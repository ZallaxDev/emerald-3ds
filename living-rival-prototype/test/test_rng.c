#include "lr_test.h"
#include "lr_rng.h"

int main(void) {
    LrRng r;
    LrRngSeed(&r, 1);
    LR_CHECK_EQ_INT(LrRandomNext(&r), 16838);
    LR_CHECK_EQ_INT(LrRandomNext(&r), 44065);
    LR_CHECK_EQ_INT(LrRandomNext(&r), 53998);
    LR_CHECK_EQ_INT(LrRandomNext(&r), 8119);

    /* Same seed -> identical sequence. */
    LrRng a, b;
    LrRngSeed(&a, 12345);
    LrRngSeed(&b, 12345);
    for (int i = 0; i < 100; i++) LR_CHECK(LrRandomNext(&a) == LrRandomNext(&b));

    /* range(10,45): 13, 10, 38, 44 for seed 1. */
    LrRngSeed(&r, 1);
    LR_CHECK_EQ_INT(LrRandomRange(&r, 10, 45), 13);
    LR_CHECK_EQ_INT(LrRandomRange(&r, 10, 45), 10);
    LR_CHECK_EQ_INT(LrRandomRange(&r, 10, 45), 38);
    LR_CHECK_EQ_INT(LrRandomRange(&r, 10, 45), 44);

    /* Empty range returns lo and consumes nothing. */
    LrRngSeed(&r, 1);
    LR_CHECK_EQ_INT(LrRandomRange(&r, 5, 5), 5);
    LR_CHECK_EQ_INT(LrRandomNext(&r), 16838);

    /* Range bounds hold for negative and positive spans. */
    LrRngSeed(&r, 999);
    for (int i = 0; i < 1000; i++) {
        int32_t v = LrRandomRange(&r, -10, 11);
        LR_CHECK(v >= -10 && v <= 10);
    }

    return lr_test_summary("test_rng");
}
