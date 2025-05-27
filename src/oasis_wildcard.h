/*
 * oasis_wildcard.h - OASIS Filename Wildcard Matching Utilities Interface
 *
 * This header file declares functions for matching OASIS filenames
 * (composed of an 8-character name and an 8-character type/extension)
 * against wildcard patterns.
 * Part of the oasis-utils library.
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#ifndef OASIS_WILDCARD_H_
#define OASIS_WILDCARD_H_

/* Include oasis.h for FNAME_LEN, FTYPE_LEN if not implicitly available */
/* Assuming these constants are defined, typically in oasis.h or a core types header */
#ifndef FNAME_LEN
#define FNAME_LEN 8
#endif
#ifndef FTYPE_LEN
#define FTYPE_LEN 8
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Matches an OASIS filename (name and type parts) against a wildcard pattern.
 *
 * The function first constructs a canonical representation of the OASIS filename
 * from the Directory Entry Block (DEB) fields:
 * - `fname_deb` (8 chars) and `ftype_deb` (8 chars) are taken.
 * - Trailing spaces are trimmed from both parts.
 * - They are combined into a "FNAME.FTYPE" string. If FTYPE is blank after
 * trimming, it becomes "FNAME.". If FNAME is blank, it's ".FTYPE".
 * If both are blank, it's ".".
 * This combined string is then matched against the provided `pattern`.
 *
 * Pattern Matching Rules:
 * - `*` (asterisk) matches zero or more characters.
 * - `?` (question mark) matches any single character.
 * - Matching is case-insensitive. Both the combined filename string and the
 * pattern are effectively converted to uppercase for comparison.
 *
 * @param fname_deb Pointer to the 8-character filename string from the DEB
 * (typically space-padded, not necessarily null-terminated if full).
 * @param ftype_deb Pointer to the 8-character filetype string from the DEB
 * (similarly space-padded).
 * @param pattern The wildcard pattern to match against (e.g., "TEST*.BAS", "*.TXT", "FILE?.DAT").
 * @return true if the combined OASIS filename matches the pattern.
 * false otherwise, or if any of the input pointers are NULL.
 */
bool oasis_filename_wildcard_match(const char* fname_deb,
                                   const char* ftype_deb,
                                   const char* pattern);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OASIS_WILDCARD_H_ */
