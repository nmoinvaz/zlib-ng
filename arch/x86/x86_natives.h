/* x86_natives.h -- x86 compile-time feature detection macros.
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#ifndef X86_NATIVES_H_
#define X86_NATIVES_H_

/* Compile-time feature detection macros */
#if defined(X86_SSE2) && (defined(__SSE2__) || (defined(ARCH_X86) && defined(ARCH_64BIT)))
#  define X86_SSE2_NATIVE
#endif
#if defined(X86_SSSE3) && defined(__SSSE3__)
#  define X86_SSSE3_NATIVE
#endif
#if defined(X86_SSE41) && defined(__SSE4_1__)
#  define X86_SSE41_NATIVE
#endif
#if defined(X86_SSE42) && defined(__SSE4_2__)
#  define X86_SSE42_NATIVE
#endif
#if defined(X86_PCLMULQDQ_CRC) && defined(__PCLMUL__)
#  define X86_PCLMULQDQ_NATIVE
#endif
#if defined(X86_AVX2) && defined(__AVX2__)
#  define X86_AVX2_NATIVE
#endif
#if defined(X86_AVX512) && defined(__AVX512F__) && defined(__AVX512DQ__) && defined(__AVX512BW__) && defined(__AVX512VL__)
#  define X86_AVX512_NATIVE
#endif
#if defined(X86_AVX512VNNI) && defined(__AVX512VNNI__)
#  define X86_AVX512VNNI_NATIVE
#endif
#if defined(__PCLMUL__) && defined(__AVX512F__) && defined(__VPCLMULQDQ__)
#  define X86_VPCLMULQDQ_NATIVE
#endif

#endif /* X86_NATIVES_H_ */
