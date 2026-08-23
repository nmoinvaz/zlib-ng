/* test_gzblock.cc - Test gzsetblocksize() and gzsetthreads() through the gz* API */

#include "zbuild.h"
#ifdef ZLIB_COMPAT
#  include "zlib.h"
#else
#  include "zlib-ng.h"
#endif

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include <gtest/gtest.h>

#include "test_shared.h"

#define BLOCKFILE "blocks.gz"

namespace {

const size_t kLen = 1000003;
const uint32_t kBlock = 65536;
const char kTail[] = "tail of the file\n";

std::vector<uint8_t> make_data() {
    std::vector<uint8_t> data(kLen);
    uint32_t seed = 1;
    for (size_t i = 0; i < kLen; i++) {
        seed = seed * 1103515245u + 12345u;
        data[i] = (uint8_t)"abcdefghij klmnop\n"[(seed >> 16) % 18];
    }
    return data;
}

/* Read the whole file back in pieces of piece bytes on the given thread count. */
std::vector<uint8_t> read_back(int threads, unsigned piece, unsigned buffer) {
    std::vector<uint8_t> out(kLen + sizeof(kTail) + 4096);
    size_t got = 0;
    int r;
    gzFile file = PREFIX(gzopen)(BLOCKFILE, "rb");
    EXPECT_TRUE(file != NULL);
    if (buffer != 0)
        EXPECT_EQ(PREFIX(gzbuffer)(file, buffer), 0);
    EXPECT_EQ(PREFIX(gzsetthreads)(file, threads), Z_OK);
    while ((r = PREFIX(gzread)(file, out.data() + got, piece)) > 0)
        got += (size_t)r;
    EXPECT_EQ(r, 0);
    EXPECT_TRUE(PREFIX(gzeof)(file));
    EXPECT_EQ(PREFIX(gzdirect)(file), 0);
    EXPECT_EQ(PREFIX(gzclose)(file), Z_OK);
    out.resize(got);
    return out;
}

}  // namespace

TEST(gzip, blocks_readwrite) {
#ifdef NO_GZCOMPRESS
    fprintf(stderr, "NO_GZCOMPRESS -- gz* functions cannot compress\n");
    GTEST_SKIP();
#else
    std::vector<uint8_t> data = make_data();
    std::vector<uint8_t> want(data);
    want.insert(want.end(), kTail, kTail + sizeof(kTail) - 1);
    gzFile file;

    /* Write blocks of 64K on four threads through the assorted write calls, with a flush and a
       parameter change part way, which keep the block in one stream. */
    file = PREFIX(gzopen)(BLOCKFILE, "wb6");
    ASSERT_TRUE(file != NULL);
    EXPECT_EQ(PREFIX(gzsetblocksize)(file, kBlock), Z_OK);
    EXPECT_EQ(PREFIX(gzsetthreads)(file, 4), Z_OK);
    EXPECT_EQ(PREFIX(gzwrite)(file, data.data(), 300000), 300000);
    EXPECT_EQ(PREFIX(gzputc)(file, data[300000]), data[300000]);
    EXPECT_EQ(PREFIX(gzflush)(file, Z_SYNC_FLUSH), Z_OK);
    EXPECT_EQ(PREFIX(gzsetparams)(file, 9, Z_DEFAULT_STRATEGY), Z_OK);
    EXPECT_EQ(PREFIX(gzfwrite)(data.data() + 300001, 1, 400000, file), 400000u);
    EXPECT_EQ(PREFIX(gzwrite)(file, data.data() + 700001, (unsigned)(kLen - 700001)), (int)(kLen - 700001));
    EXPECT_NE(PREFIX(gzsetblocksize)(file, 1), Z_OK);    /* too late */
    EXPECT_EQ(PREFIX(gzclose)(file), Z_OK);

    /* Append a plain member. */
    file = PREFIX(gzopen)(BLOCKFILE, "ab");
    ASSERT_TRUE(file != NULL);
    EXPECT_EQ(PREFIX(gzputs)(file, kTail), (int)(sizeof(kTail) - 1));
    EXPECT_EQ(PREFIX(gzclose)(file), Z_OK);

    /* Block reader on all CPUs with odd read sizes, on three threads with large direct reads, and
       plain inflate on the calling thread. */
    EXPECT_EQ(read_back(0, 7777, 0), want);
    EXPECT_EQ(read_back(3, 500000, 8192), want);
    EXPECT_EQ(read_back(1, 65536, 0), want);

    /* Positioning on the block reader. */
    file = PREFIX(gzopen)(BLOCKFILE, "rb");
    ASSERT_TRUE(file != NULL);
    EXPECT_EQ(PREFIX(gzsetthreads)(file, 2), Z_OK);
    EXPECT_EQ(PREFIX(gzgetc)(file), data[0]);
    EXPECT_EQ(PREFIX(gzungetc)(data[0], file), data[0]);
    EXPECT_EQ(PREFIX(gzgetc)(file), data[0]);
    EXPECT_EQ(PREFIX(gzseek)(file, 654321, SEEK_SET), 654321);
    EXPECT_EQ(PREFIX(gzgetc)(file), data[654321]);
    EXPECT_EQ(PREFIX(gzseek)(file, 100, SEEK_SET), 100);
    EXPECT_EQ(PREFIX(gzgetc)(file), data[100]);
    EXPECT_EQ(PREFIX(gzrewind)(file), 0);
    EXPECT_EQ(PREFIX(gzgetc)(file), data[0]);
    EXPECT_EQ(PREFIX(gzclose)(file), Z_OK);

    /* Transparent writing cannot use blocks, and the block size is bounded. */
    file = PREFIX(gzopen)(BLOCKFILE, "wbT");
    ASSERT_TRUE(file != NULL);
    EXPECT_EQ(PREFIX(gzsetblocksize)(file, kBlock), Z_STREAM_ERROR);
    EXPECT_EQ(PREFIX(gzclose)(file), Z_OK);
    file = PREFIX(gzopen)(BLOCKFILE, "wb");
    ASSERT_TRUE(file != NULL);
    EXPECT_EQ(PREFIX(gzsetblocksize)(file, 0xffffffffu), Z_STREAM_ERROR);
    EXPECT_EQ(PREFIX(gzsetblocksize)(file, kBlock), Z_OK);
    EXPECT_EQ(PREFIX(gzwrite)(file, data.data(), 1000), 1000);
    EXPECT_EQ(PREFIX(gzclose)(file), Z_OK);

    /* A header claiming an absurd block size must not drive allocations, the member is simply
       inflated the plain way. Patch the ZB subfield in place, it sits at byte 16 of the header. */
    {
        FILE *raw = fopen(BLOCKFILE, "r+b");
        ASSERT_TRUE(raw != NULL);
        uint8_t hdr[20];
        ASSERT_EQ(fread(hdr, 1, sizeof(hdr), raw), sizeof(hdr));
        ASSERT_EQ(hdr[12], 'Z');
        ASSERT_EQ(hdr[13], 'B');
        memset(hdr + 16, 0xff, 4);
        ASSERT_EQ(fseek(raw, 0, SEEK_SET), 0);
        ASSERT_EQ(fwrite(hdr, 1, sizeof(hdr), raw), sizeof(hdr));
        fclose(raw);
    }
    file = PREFIX(gzopen)(BLOCKFILE, "rb");
    ASSERT_TRUE(file != NULL);
    EXPECT_EQ(PREFIX(gzsetthreads)(file, 0), Z_OK);
    std::vector<uint8_t> small(2000);
    EXPECT_EQ(PREFIX(gzread)(file, small.data(), 2000), 1000);
    EXPECT_EQ(memcmp(small.data(), data.data(), 1000), 0);
    EXPECT_EQ(PREFIX(gzclose)(file), Z_OK);

    remove(BLOCKFILE);
#endif
}
