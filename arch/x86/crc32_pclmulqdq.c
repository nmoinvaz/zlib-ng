/* crc32_pclmulqdq.c -- PCLMULQDQ-based CRC32.
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#ifdef X86_PCLMULQDQ_CRC

#include "crc32_pclmulqdq_tpl.h"

Z_INTERNAL uint32_t crc32_pclmulqdq(uint32_t crc, const uint8_t *buf, size_t len) {
    return crc32_copy_impl(crc, NULL, buf, len, 0);
}

Z_INTERNAL uint32_t crc32_copy_pclmulqdq(uint32_t crc, uint8_t *dst, const uint8_t *src, size_t len) {
    return crc32_copy_impl(crc, dst, src, len, 1);
}
#endif
