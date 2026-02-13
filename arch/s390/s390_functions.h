/* s390_functions.h -- s390 implementations for arch-specific functions.
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#ifndef S390_FUNCTIONS_H_
#define S390_FUNCTIONS_H_

#ifdef S390_CRC32_VX
uint32_t crc32_s390_vx(uint32_t crc, const uint8_t *buf, size_t len);
uint32_t crc32_copy_s390_vx(uint32_t crc, uint8_t *dst, const uint8_t *src, size_t len);

#ifdef __clang__
#  if ((__clang_major__ == 18) || (__clang_major__ == 19 && (__clang_minor__ < 1 || (__clang_minor__ == 1 && __clang_patchlevel__ < 2))))
# error CRC32-VX optimizations are broken due to compiler bug in Clang versions: 18.0.0 <= clang_version < 19.1.2. \
        Either disable the zlib-ng CRC32-VX optimization, or switch to another compiler/compiler version.
#  endif
#endif

#endif

#ifdef S390_VX
uint32_t adler32_s390_vx(uint32_t adler, const uint8_t *buf, size_t len);
uint32_t adler32_copy_s390_vx(uint32_t adler, uint8_t *dst, const uint8_t *src, size_t len);
uint8_t* chunkmemset_safe_s390_vx(uint8_t *out, uint8_t *from, unsigned len, unsigned left);
uint32_t compare256_s390_vx(const uint8_t *src0, const uint8_t *src1);
void inflate_fast_s390_vx(PREFIX3(stream) *strm, uint32_t start);
uint32_t longest_match_s390_vx(deflate_state *const s, uint32_t cur_match);
uint32_t longest_match_slow_s390_vx(deflate_state *const s, uint32_t cur_match);
void slide_hash_s390_vx(deflate_state *s);
#endif

#ifdef DISABLE_RUNTIME_CPU_DETECTION
#  if defined(S390_CRC32_VX) && defined(__zarch__) && __ARCH__ >= 11 && defined(__VX__)
#    undef native_crc32
#    define native_crc32 crc32_s390_vx
#    undef native_crc32_copy
#    define native_crc32_copy crc32_copy_s390_vx
#  endif
#  if defined(S390_VX) && defined(__VX__)
#    undef native_adler32
#    define native_adler32 adler32_s390_vx
#    undef native_adler32_copy
#    define native_adler32_copy adler32_copy_s390_vx
#    undef native_chunkmemset_safe
#    define native_chunkmemset_safe chunkmemset_safe_s390_vx
#    undef native_compare256
#    define native_compare256 compare256_s390_vx
#    undef native_inflate_fast
#    define native_inflate_fast inflate_fast_s390_vx
#    undef native_longest_match
#    define native_longest_match longest_match_s390_vx
#    undef native_longest_match_slow
#    define native_longest_match_slow longest_match_slow_s390_vx
#    undef native_slide_hash
#    define native_slide_hash slide_hash_s390_vx
#  endif
#endif

#endif
