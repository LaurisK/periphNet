#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int test_failures;

#define TEST_ASSERT(cond)                                                   \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
            test_failures++;                                                \
        }                                                                   \
    } while (0)

#define TEST_ASSERT_MEM_EQ(a, b, len)                                       \
    do {                                                                    \
        if (memcmp((a), (b), (len)) != 0) {                                 \
            printf("FAIL %s:%d: memcmp(%s, %s, %s)\n",                      \
                   __FILE__, __LINE__, #a, #b, #len);                       \
            test_failures++;                                                \
        }                                                                   \
    } while (0)

#define RUN_TEST(fn)                                                        \
    do {                                                                    \
        int before = test_failures;                                         \
        fn();                                                               \
        printf("%-40s %s\n", #fn, test_failures == before ? "ok" : "FAILED"); \
    } while (0)

/* Parse a hex string into bytes; returns byte count. */
static inline uint32_t hex2bin(const char *hex, uint8_t *out, uint32_t max)
{
    uint32_t n = 0;
    while (hex[0] && hex[1] && n < max) {
        unsigned byte;
        if (sscanf(hex, "%2x", &byte) != 1) {
            break;
        }
        out[n++] = (uint8_t)byte;
        hex += 2;
    }
    return n;
}

#endif /* TEST_UTIL_H */
