#ifndef LR_TEST_H
#define LR_TEST_H

#include <stdio.h>

static int lr_test_failures;
static int lr_test_checks;

#define LR_CHECK(cond) do { \
    lr_test_checks++; \
    if (!(cond)) { lr_test_failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define LR_CHECK_EQ_INT(actual, expected) do { \
    int lr_a = (int)(actual); int lr_e = (int)(expected); \
    lr_test_checks++; \
    if (lr_a != lr_e) { lr_test_failures++; printf("FAIL %s:%d: %s = %d, expected %d\n", __FILE__, __LINE__, #actual, lr_a, lr_e); } \
} while (0)

static int lr_test_summary(const char *name) {
    if (lr_test_failures) {
        printf("%s: %d/%d checks FAILED\n", name, lr_test_failures, lr_test_checks);
        return 1;
    }
    printf("%s: %d checks PASS\n", name, lr_test_checks);
    return 0;
}

#endif
