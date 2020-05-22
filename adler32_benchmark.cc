#include <stdint.h>
#include <stdio.h>
#include <stdint.h>

#include "zbuild.h"

#include <benchmark/benchmark.h>

#define MAX_RANDOM_INTS (1024 * 1024)
static uint32_t *random_ints = NULL;

/* adler32 */
extern "C" uint32_t adler32_c(uint32_t adler, const unsigned char *buf, size_t len);
#if (defined(__ARM_NEON__) || defined(__ARM_NEON)) && defined(ARM_NEON_ADLER32)
extern "C" uint32_t adler32_neon(uint32_t adler, const unsigned char *buf, size_t len);
#endif
#ifdef X86_SSE3_ADLER32
extern "C" uint32_t adler32_sse3(uint32_t adler, const unsigned char *buf, size_t len);
#endif
#ifdef X86_SSE4_ADLER32
extern "C" uint32_t adler32_sse4(uint32_t adler, const unsigned char *buf, size_t len);
#endif
#ifdef X86_SSSE3_ADLER32
extern "C" uint32_t adler32_ssse3(uint32_t adler, const unsigned char *buf, size_t len);
#endif

static void adler32_c_bench(benchmark::State& state) {
    int32_t j = 0;
    uint32_t a = 0;
    while (state.KeepRunning()) {
        uint32_t hash = adler32_c(a, (const unsigned char *)random_ints, j * sizeof(uint32_t));
        benchmark::DoNotOptimize(hash);
        if (++j >= MAX_RANDOM_INTS) j = 0;
    }
}
BENCHMARK(adler32_c_bench);

static void adler32_sse3_bench(benchmark::State& state) {
    int32_t j = 0;
    uint32_t a = 0;
    while (state.KeepRunning()) {
        uint32_t hash = adler32_sse3(a, (const unsigned char *)random_ints, j * sizeof(uint32_t));
        benchmark::DoNotOptimize(hash);
        if (++j >= MAX_RANDOM_INTS) j = 0;
    }
}
BENCHMARK(adler32_sse3_bench);

static void adler32_ssse3_bench(benchmark::State& state) {
    int32_t j = 0;
    uint32_t a = 0;
    while (state.KeepRunning()) {
        uint32_t hash = adler32_ssse3(a, (const unsigned char *)random_ints, j * sizeof(uint32_t));
        benchmark::DoNotOptimize(hash);
        if (++j >= MAX_RANDOM_INTS) j = 0;
    }
}
BENCHMARK(adler32_ssse3_bench);

static void adler32_sse4_bench(benchmark::State& state) {
    int32_t j = 0;
    uint32_t a = 0;
    while (state.KeepRunning()) {
        uint32_t hash = adler32_sse4(a, (const unsigned char *)random_ints, j * sizeof(uint32_t));
        benchmark::DoNotOptimize(hash);
        if (++j >= MAX_RANDOM_INTS) j = 0;
    }
}
BENCHMARK(adler32_sse4_bench);

int main(int argc, char** argv)
{
    int32_t random_ints_size = MAX_RANDOM_INTS * sizeof(uint32_t);
    random_ints = (uint32_t *)malloc(random_ints_size);
    for (int32_t i = 0; i < MAX_RANDOM_INTS; i++) {
        random_ints[i] = rand();
    }
    ::benchmark::Initialize(&argc, argv);
    ::benchmark::RunSpecifiedBenchmarks();
    free(random_ints);
}