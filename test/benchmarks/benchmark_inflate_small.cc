/* benchmark_inflate_small.cc -- benchmark inflate() with small output buffers
 * Copyright (C) 2026 Nathan Moinvaziri
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#include <stdio.h>
#include <assert.h>
#include <benchmark/benchmark.h>

extern "C" {
#  include "zbuild.h"
#  include "zutil_p.h"
#  if defined(ZLIB_COMPAT)
#    include "zlib.h"
#  else
#    include "zlib-ng.h"
#  endif
#  include "compressible_data_p.h"
}

/* Total uncompressed data size for the benchmark. */
#define TOTAL_SIZE (256 * 1024)

class inflate_small_bench: public benchmark::Fixture {
private:
    uint8_t *inbuff;
    uint8_t *outbuff;
    uint8_t *compressed;
    z_uintmax_t compressed_size;

public:
    void SetUp(::benchmark::State& state) {
        int err;
        outbuff = (uint8_t *)malloc(TOTAL_SIZE);
        if (outbuff == NULL) {
            state.SkipWithError("malloc failed");
            return;
        }

        inbuff = gen_compressible_data(TOTAL_SIZE);
        if (inbuff == NULL) {
            free(outbuff);
            outbuff = NULL;
            state.SkipWithError("gen_compressible_data() failed");
            return;
        }

        PREFIX3(stream) strm;
        strm.zalloc = NULL;
        strm.zfree = NULL;
        strm.opaque = NULL;
        strm.total_in = 0;
        strm.total_out = 0;

        err = PREFIX(deflateInit2)(&strm, Z_BEST_COMPRESSION, Z_DEFLATED, -15, MAX_MEM_LEVEL, Z_DEFAULT_STRATEGY);
        if (err != Z_OK) {
            state.SkipWithError("deflateInit2 failed");
            return;
        }

        compressed_size = PREFIX(deflateBound)(&strm, TOTAL_SIZE);
        compressed = (uint8_t *)malloc(compressed_size);
        if (compressed == NULL) {
            state.SkipWithError("malloc failed");
            PREFIX(deflateEnd)(&strm);
            return;
        }

        strm.avail_in = TOTAL_SIZE;
        strm.next_in = (z_const uint8_t *)inbuff;
        strm.next_out = compressed;
        strm.avail_out = (uint32_t)compressed_size;

        err = PREFIX(deflate)(&strm, Z_FINISH);
        if (err != Z_STREAM_END) {
            state.SkipWithError("deflate did not return Z_STREAM_END");
            PREFIX(deflateEnd)(&strm);
            return;
        }

        compressed_size = strm.total_out;
        PREFIX(deflateEnd)(&strm);
    }

    void Bench(benchmark::State& state) {
        int err;
        uint32_t row_size = (uint32_t)state.range(0);

        PREFIX3(stream) strm;
        strm.zalloc = NULL;
        strm.zfree = NULL;
        strm.opaque = NULL;
        strm.next_in = NULL;
        strm.avail_in = 0;

        err = PREFIX(inflateInit2)(&strm, -15);
        if (err != Z_OK) {
            state.SkipWithError("inflateInit2 failed");
            return;
        }

        for (auto _ : state) {
            err = PREFIX(inflateReset)(&strm);
            if (err != Z_OK) {
                state.SkipWithError("inflateReset failed");
                return;
            }

            strm.avail_in = (uint32_t)compressed_size;
            strm.next_in = compressed;

            /* Inflate row-by-row, like libpng does */
            uint8_t *out_pos = outbuff;
            uint32_t remaining = TOTAL_SIZE;

            while (remaining > 0) {
                uint32_t chunk_size = (remaining < row_size) ? remaining : row_size;
                strm.next_out = out_pos;
                strm.avail_out = chunk_size;

                err = PREFIX(inflate)(&strm, Z_NO_FLUSH);
                if (err == Z_STREAM_END)
                    break;
                if (err != Z_OK) {
                    state.SkipWithError("inflate failed");
                    PREFIX(inflateEnd)(&strm);
                    return;
                }

                uint32_t processed = chunk_size - strm.avail_out;
                out_pos += processed;
                remaining -= processed;
            }
        }

        PREFIX(inflateEnd)(&strm);
    }

    void TearDown(const ::benchmark::State&) {
        free(inbuff);
        free(outbuff);
        free(compressed);
    }
};

/* Arg values are avail_out per inflate() call, simulating PNG row widths */
BENCHMARK_DEFINE_F(inflate_small_bench, small_output)(benchmark::State& state) {
    Bench(state);
}
BENCHMARK_REGISTER_F(inflate_small_bench, small_output)
    ->Arg(64)->Arg(128)->Arg(256)->Arg(512)
    ->Arg(1024)->Arg(2048)->Arg(4096)->Arg(16384);
