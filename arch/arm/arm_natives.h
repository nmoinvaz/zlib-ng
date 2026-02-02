/* arm_natives.h -- ARM compile-time feature detection macros.
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#ifndef ARM_NATIVES_H_
#define ARM_NATIVES_H_

/* Compile-time feature detection macros */
#if defined(ARM_SIMD) && defined(__ARM_FEATURE_SIMD32)
#  define ARM_SIMD_NATIVE
#endif
/* NEON is guaranteed on ARM64 (like SSE2 on x86-64) */
#if defined(ARM_NEON) && (defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(ARCH_64BIT))
#  define ARM_NEON_NATIVE
#endif
/* CRC32 is optional in ARMv8.0, mandatory in ARMv8.1+ */
#if defined(ARM_CRC32) && (defined(__ARM_FEATURE_CRC32) || (defined(__ARM_ARCH) && __ARM_ARCH >= 801))
#  define ARM_CRC32_NATIVE
#endif
#if defined(ARM_PMULL_EOR3) && defined(__ARM_FEATURE_CRC32) && defined(__ARM_FEATURE_CRYPTO) && defined(__ARM_FEATURE_SHA3)
#  define ARM_PMULL_EOR3_NATIVE
#endif

#endif /* ARM_NATIVES_H_ */
