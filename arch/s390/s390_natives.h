/* s390_natives.h -- s390 compile-time feature detection macros.
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#ifndef S390_NATIVES_H_
#define S390_NATIVES_H_

/* Compile-time feature detection macros */
#if defined(S390_CRC32_VX) && defined(__zarch__) && __ARCH__ >= 11 && defined(__VX__)
#  define S390_CRC32_VX_NATIVE
#endif

#endif /* S390_NATIVES_H_ */
