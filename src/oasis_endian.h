/*
 * oasis_endian.h - OASIS Endian handling macros.
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#ifndef OASIS_ENDIAN_H_
#define OASIS_ENDIAN_H_

#include <stdint.h>

/*
 * Endianness detection.
 * Tries to use compiler-provided macros first.
 * Falls back to a common set of checks.
 */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__ || \
    defined(__BIG_ENDIAN__) || defined(__BIGENDIAN__) || \
    defined(__ARMEB__) || defined(__THUMBEB__) || defined(__AARCH64EB__) || \
    defined(_MIPSEB) || defined(__MIPSEB) || defined(__MIPSEB__)
  /* Host is Big-Endian */
  #define OASIS_HOST_IS_BIG_ENDIAN 1
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || \
    defined(__LITTLE_ENDIAN__) || defined(__LITTLEENDIAN__) || \
    defined(__ARMEL__) || defined(__THUMBEL__) || defined(__AARCH64EL__) || \
    defined(_MIPSEL) || defined(__MIPSEL) || defined(__MIPSEL__) || \
    defined(_WIN32) || defined(__i386__) || defined(__x86_64__) || defined(__amd64__)
  /* Host is Little-Endian */
  #define OASIS_HOST_IS_LITTLE_ENDIAN 1
#else
  /*
   * Could not determine host endianness at compile time.
   * You may need to define OASIS_HOST_IS_BIG_ENDIAN or OASIS_HOST_IS_LITTLE_ENDIAN manually.
   * Defaulting to little-endian as it's more common for desktop/server CPUs.
   */
  #warning "Could not determine host endianness at compile time. Assuming little-endian."
  #define OASIS_HOST_IS_LITTLE_ENDIAN 1
#endif

/* Byte swap utility for 16-bit unsigned integers */
static inline uint16_t oasis_bswap16(uint16_t val) {
    return (uint16_t)((val >> 8) | (val << 8));
}

#ifndef htole16
/* Host to Little-Endian 16-bit conversion */
#if defined(OASIS_HOST_IS_BIG_ENDIAN)
  #define htole16(x) oasis_bswap16(x)
#else /* Host is Little-Endian or assumed Little-Endian */
  #define htole16(x) (x)
#endif
#endif /* htole16 */

/* Little-Endian 16-bit to Host conversion */
#ifndef le16toh
#if defined(OASIS_HOST_IS_BIG_ENDIAN)
  #define le16toh(x) oasis_bswap16(x)
#else /* Host is Little-Endian or assumed Little-Endian */
  #define le16toh(x) (x)
#endif
#endif /* le16toh */

#endif /* OASIS_ENDIAN_H_ */
