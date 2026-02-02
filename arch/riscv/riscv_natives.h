/* riscv_natives.h -- RISCV compile-time feature detection macros.
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#ifndef RISCV_NATIVES_H_
#define RISCV_NATIVES_H_

/* Compile-time feature detection macros */
#if defined(RISCV_RVV) && defined(__riscv_v) && defined(__linux__)
#  define RISCV_RVV_NATIVE
#endif
#if defined(RISCV_CRC32_ZBC) && defined(__riscv_zbc)
#  define RISCV_ZBC_NATIVE
#endif

#endif /* RISCV_NATIVES_H_ */
