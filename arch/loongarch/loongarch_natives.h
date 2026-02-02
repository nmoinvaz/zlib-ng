/* loongarch_natives.h -- LoongArch compile-time feature detection macros.
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#ifndef LOONGARCH_NATIVES_H_
#define LOONGARCH_NATIVES_H_

/* Compile-time feature detection macros */
#if defined(LOONGARCH_LSX) && defined(__loongarch_sx)
#  define LOONGARCH_LSX_NATIVE
#endif
#if defined(LOONGARCH_LASX) && defined(__loongarch_asx)
#  define LOONGARCH_LASX_NATIVE
#endif

#endif /* LOONGARCH_NATIVES_H_ */
