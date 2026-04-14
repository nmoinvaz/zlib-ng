/* bloat_test.c -- intentional bloat to exercise the bloaty workflow.
 *
 * Adds ~64 KiB of .rodata plus a pair of never-called helper functions so the
 * bloaty diff against develop has something to show in both the sections and
 * compile-unit breakdowns. Safe to delete when the workflow has been verified.
 */

#include <stdint.h>

#include "zbuild.h"

#define BLOAT_ENTRIES 16384

__attribute__((used))
const uint32_t zng_bloat_table[BLOAT_ENTRIES] = {
    0x12345678, 0xdeadbeef, 0xcafebabe, 0xf00dface,
    0xbaadc0de, 0x0badbeef, 0xdeafbabe, 0xfacefeed,
    0x01234567, 0x89abcdef, 0xfedcba98, 0x76543210,
    0xa5a5a5a5, 0x5a5a5a5a, 0xc3c3c3c3, 0x3c3c3c3c,
};

__attribute__((used))
uint32_t Z_INTERNAL zng_bloat_fold(uint32_t seed) {
    uint32_t acc = seed;
    for (int i = 0; i < BLOAT_ENTRIES; i++) {
        acc ^= zng_bloat_table[i];
        acc = (acc << 1) | (acc >> 31);
    }
    return acc;
}

__attribute__((used))
uint32_t Z_INTERNAL zng_bloat_xor(const uint32_t *data, size_t n) {
    uint32_t acc = 0;
    for (size_t i = 0; i < n; i++)
        acc ^= data[i];
    return acc;
}
