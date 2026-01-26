/* benchmark_tailcopy.cc -- benchmark different copy strategies for tail handling
 * Copyright (C) 2022 Nathan Moinvaziri
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#include <benchmark/benchmark.h>
#include <cstring>
#include <cstdint>

extern "C" {
#  include "zbuild.h"
#  include "zutil.h"
}

#define Z_RESTRICT __restrict__

// Strategy 1: Byte-by-byte loop with restrict (compiler auto-vectorizes)
static void copy_loop_restrict(uint8_t* Z_RESTRICT dst, const uint8_t* Z_RESTRICT src, size_t len) {
    while (len--) {
        *dst++ = *src++;
    }
}

// Strategy 2: Byte-by-byte loop without restrict
static void copy_loop_no_restrict(uint8_t* dst, const uint8_t* src, size_t len) {
    while (len--) {
        *dst++ = *src++;
    }
}

// Strategy 3: Fixed-size memcpy piecemeal (16/8/4/2/1)
static void copy_piecemeal_16(uint8_t* dst, const uint8_t* src, size_t len) {
    while (len >= 16) {
        memcpy(dst, src, 16);
        dst += 16;
        src += 16;
        len -= 16;
    }
    if (len & 8) {
        memcpy(dst, src, 8);
        dst += 8;
        src += 8;
    }
    if (len & 4) {
        memcpy(dst, src, 4);
        dst += 4;
        src += 4;
    }
    if (len & 2) {
        memcpy(dst, src, 2);
        dst += 2;
        src += 2;
    }
    if (len & 1) {
        *dst = *src;
    }
}

// Strategy 4: Fixed-size memcpy piecemeal (8/4/2/1)
static void copy_piecemeal_8(uint8_t* dst, const uint8_t* src, size_t len) {
    while (len >= 8) {
        memcpy(dst, src, 8);
        dst += 8;
        src += 8;
        len -= 8;
    }
    if (len & 4) {
        memcpy(dst, src, 4);
        dst += 4;
        src += 4;
    }
    if (len & 2) {
        memcpy(dst, src, 2);
        dst += 2;
        src += 2;
    }
    if (len & 1) {
        *dst = *src;
    }
}

// Strategy 5: Single memcpy
static void copy_single_memcpy(uint8_t* dst, const uint8_t* src, size_t len) {
    memcpy(dst, src, len);
}

// Strategy 6: Overlapping copies (copy 8 from start and 8 from end for sizes 9-16, etc.)
static void copy_overlap(uint8_t* dst, const uint8_t* src, size_t len) {
    if (len >= 32) {
        while (len >= 32) {
            memcpy(dst, src, 32);
            dst += 32;
            src += 32;
            len -= 32;
        }
        if (len > 16) {
            memcpy(dst, src, 16);
            memcpy(dst + len - 16, src + len - 16, 16);
            return;
        }
    }
    if (len > 16) {
        memcpy(dst, src, 16);
        memcpy(dst + len - 16, src + len - 16, 16);
    } else if (len > 8) {
        memcpy(dst, src, 8);
        memcpy(dst + len - 8, src + len - 8, 8);
    } else if (len > 4) {
        memcpy(dst, src, 4);
        memcpy(dst + len - 4, src + len - 4, 4);
    } else if (len > 2) {
        memcpy(dst, src, 2);
        memcpy(dst + len - 2, src + len - 2, 2);
    } else if (len == 2) {
        memcpy(dst, src, 2);
    } else if (len == 1) {
        *dst = *src;
    }
}

// Strategy 7: Piecemeal with 32-byte chunks
static void copy_piecemeal_32(uint8_t* dst, const uint8_t* src, size_t len) {
    while (len >= 32) {
        memcpy(dst, src, 32);
        dst += 32;
        src += 32;
        len -= 32;
    }
    if (len & 16) {
        memcpy(dst, src, 16);
        dst += 16;
        src += 16;
    }
    if (len & 8) {
        memcpy(dst, src, 8);
        dst += 8;
        src += 8;
    }
    if (len & 4) {
        memcpy(dst, src, 4);
        dst += 4;
        src += 4;
    }
    if (len & 2) {
        memcpy(dst, src, 2);
        dst += 2;
        src += 2;
    }
    if (len & 1) {
        *dst = *src;
    }
}

class tailcopy : public benchmark::Fixture {
private:
    uint8_t *srcbuf;
    uint8_t *dstbuf;

public:
    void SetUp(::benchmark::State& state) {
        srcbuf = (uint8_t *)zng_alloc_aligned(128, 64);
        dstbuf = (uint8_t *)zng_alloc_aligned(128, 64);
        if (srcbuf == NULL || dstbuf == NULL) {
            state.SkipWithError("malloc failed");
            return;
        }
        for (int i = 0; i < 128; i++) {
            srcbuf[i] = (uint8_t)(rand() & 0xff);
        }
    }

    void TearDown(const ::benchmark::State&) {
        zng_free_aligned(srcbuf);
        zng_free_aligned(dstbuf);
    }

    void BenchLoopRestrict(benchmark::State& state) {
        size_t len = (size_t)state.range(0);
        for (auto _ : state) {
            copy_loop_restrict(dstbuf, srcbuf, len);
            benchmark::DoNotOptimize(dstbuf);
            benchmark::ClobberMemory();
        }
    }

    void BenchLoopNoRestrict(benchmark::State& state) {
        size_t len = (size_t)state.range(0);
        for (auto _ : state) {
            copy_loop_no_restrict(dstbuf, srcbuf, len);
            benchmark::DoNotOptimize(dstbuf);
            benchmark::ClobberMemory();
        }
    }

    void BenchPiecemeal16(benchmark::State& state) {
        size_t len = (size_t)state.range(0);
        for (auto _ : state) {
            copy_piecemeal_16(dstbuf, srcbuf, len);
            benchmark::DoNotOptimize(dstbuf);
            benchmark::ClobberMemory();
        }
    }

    void BenchPiecemeal8(benchmark::State& state) {
        size_t len = (size_t)state.range(0);
        for (auto _ : state) {
            copy_piecemeal_8(dstbuf, srcbuf, len);
            benchmark::DoNotOptimize(dstbuf);
            benchmark::ClobberMemory();
        }
    }

    void BenchMemcpy(benchmark::State& state) {
        size_t len = (size_t)state.range(0);
        for (auto _ : state) {
            copy_single_memcpy(dstbuf, srcbuf, len);
            benchmark::DoNotOptimize(dstbuf);
            benchmark::ClobberMemory();
        }
    }

    void BenchOverlap(benchmark::State& state) {
        size_t len = (size_t)state.range(0);
        for (auto _ : state) {
            copy_overlap(dstbuf, srcbuf, len);
            benchmark::DoNotOptimize(dstbuf);
            benchmark::ClobberMemory();
        }
    }

    void BenchPiecemeal32(benchmark::State& state) {
        size_t len = (size_t)state.range(0);
        for (auto _ : state) {
            copy_piecemeal_32(dstbuf, srcbuf, len);
            benchmark::DoNotOptimize(dstbuf);
            benchmark::ClobberMemory();
        }
    }
};

BENCHMARK_DEFINE_F(tailcopy, loop_restrict)(benchmark::State& state) {
    BenchLoopRestrict(state);
}
BENCHMARK_REGISTER_F(tailcopy, loop_restrict)->Arg(7)->Arg(15)->Arg(31)->Arg(63);

BENCHMARK_DEFINE_F(tailcopy, loop_no_restrict)(benchmark::State& state) {
    BenchLoopNoRestrict(state);
}
BENCHMARK_REGISTER_F(tailcopy, loop_no_restrict)->Arg(7)->Arg(15)->Arg(31)->Arg(63);

BENCHMARK_DEFINE_F(tailcopy, piecemeal_16)(benchmark::State& state) {
    BenchPiecemeal16(state);
}
BENCHMARK_REGISTER_F(tailcopy, piecemeal_16)->Arg(7)->Arg(15)->Arg(31)->Arg(63);

BENCHMARK_DEFINE_F(tailcopy, piecemeal_8)(benchmark::State& state) {
    BenchPiecemeal8(state);
}
BENCHMARK_REGISTER_F(tailcopy, piecemeal_8)->Arg(7)->Arg(15)->Arg(31)->Arg(63);

BENCHMARK_DEFINE_F(tailcopy, memcpy)(benchmark::State& state) {
    BenchMemcpy(state);
}
BENCHMARK_REGISTER_F(tailcopy, memcpy)->Arg(7)->Arg(15)->Arg(31)->Arg(63);

BENCHMARK_DEFINE_F(tailcopy, overlap)(benchmark::State& state) {
    BenchOverlap(state);
}
BENCHMARK_REGISTER_F(tailcopy, overlap)->Arg(7)->Arg(15)->Arg(31)->Arg(63);

BENCHMARK_DEFINE_F(tailcopy, piecemeal_32)(benchmark::State& state) {
    BenchPiecemeal32(state);
}
BENCHMARK_REGISTER_F(tailcopy, piecemeal_32)->Arg(7)->Arg(15)->Arg(31)->Arg(63);
