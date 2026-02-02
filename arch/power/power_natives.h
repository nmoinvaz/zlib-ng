/* power_natives.h -- POWER compile-time feature detection macros.
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#ifndef POWER_NATIVES_H_
#define POWER_NATIVES_H_

/* Compile-time feature detection macros */
#if defined(PPC_VMX) && defined(__ALTIVEC__)
#  define PPC_VMX_NATIVE
#endif
#if defined(POWER8_VSX) && defined(_ARCH_PWR8) && defined(__VSX__)
#  define POWER8_VSX_NATIVE
#endif
#if defined(POWER8_VSX_CRC32) && defined(_ARCH_PWR8) && defined(__VSX__)
#  define POWER8_VSX_CRC32_NATIVE
#endif
#if defined(POWER9) && defined(_ARCH_PWR9)
#  define POWER9_NATIVE
#endif

#endif /* POWER_NATIVES_H_ */
