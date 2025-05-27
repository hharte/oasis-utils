/*
 * oasis_time.h - OASIS Time and Date Conversion Utilities Interface
 *
 * This header file declares functions for converting between the OASIS
 * 3-byte packed timestamp format and the standard C `struct tm` representation.
 * It also provides a utility to format an OASIS timestamp into a human-readable string.
 * Part of the oasis-utils library.
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#ifndef OASIS_TIME_H_
#define OASIS_TIME_H_

#include <time.h>    /* For struct tm and size_t */
#include <stdint.h>  /* For standard integer types (used in oasis_tm_t via oasis.h) */

/* Include oasis.h for the definition of oasis_tm_t */
#include "oasis.h"

/* Define extern "C" guards for C++ compatibility */
#ifdef __cplusplus
extern "C" {
#endif

/* --- Function Prototypes --- */

/**
 * @brief Converts an OASIS 3-byte timestamp to a standard `struct tm`.
 *
 * Decodes the packed OASIS timestamp format (Month, Day, Year, Hour, Minute)
 * into the corresponding fields of a `struct tm`.
 * - Year in `struct tm` is years since 1900. OASIS year is 0-15 (1977-1992).
 * - Month in `struct tm` is 0-11. OASIS month is 1-12.
 * - Day, Hour, Minute are directly mapped.
 * - Seconds (`tm_sec`) are set to 0 as OASIS timestamps do not store seconds.
 * - Daylight Saving Time (`tm_isdst`) is set to -1 (information not available).
 * - `tm_wday` (day of week) and `tm_yday` (day of year) are not calculated by this function.
 * Input values from the OASIS timestamp that are out of typical ranges (e.g., month 15)
 * are clamped to valid `struct tm` ranges (e.g., month becomes 11 for December).
 *
 * @param timestamp Pointer to the source `oasis_tm_t` structure containing the
 * 3-byte packed OASIS timestamp.
 * @param tmout     Pointer to the destination `struct tm` structure to be populated.
 * This structure is zero-initialized by the function before filling.
 */
extern void oasis_convert_timestamp_to_tm(const oasis_tm_t* timestamp, struct tm* tmout);

/**
 * @brief Converts a standard `struct tm` to an OASIS 3-byte timestamp.
 *
 * Encodes the date and time fields from a `struct tm` into the packed
 * 3-byte OASIS timestamp format.
 * - Year: Input `tmin->tm_year` (years since 1900) is converted to OASIS year
 * (0-15, representing 1977-1992). Values outside this range are clamped.
 * - Month: Input `tmin->tm_mon` (0-11) is converted to OASIS month (1-12).
 * - Day, Hour, Minute: Values are taken from `tmin` and clamped if necessary
 * to fit OASIS field constraints (e.g., day 1-31, hour 0-23, min 0-59).
 * - Seconds from `tmin->tm_sec` are ignored.
 *
 * @param tmin      Pointer to the source `struct tm` structure.
 * @param timestamp Pointer to the destination `oasis_tm_t` structure where the
 * 3-byte packed OASIS timestamp will be stored.
 */
extern void oasis_convert_tm_to_timestamp(const struct tm* tmin, oasis_tm_t* timestamp);

/**
 * @brief Formats an OASIS timestamp into a human-readable string.
 *
 * The output format is "MM/DD/YY HH:MM" (e.g., "04/23/85 14:30").
 * The function internally converts the OASIS timestamp to a `struct tm`
 * before using `strftime` for formatting.
 *
 * @param dest      Pointer to the destination character buffer where the formatted
 * string will be written.
 * @param dest_size Size of the `dest` buffer in bytes. Should be sufficient to hold
 * the formatted string plus a null terminator (e.g., 15 bytes for
 * "MM/DD/YY HH:MM\0").
 * @param timestamp Pointer to the source `oasis_tm_t` structure.
 * @return          The number of characters written to `dest` (excluding the null
 * terminator), identical to `strftime`'s return value.
 * Returns 0 if an error occurred (e.g., `dest` is NULL,
 * `dest_size` is too small, or `timestamp` is NULL implicitly
 * handled by internal conversion). On error with `dest_size > 0`,
 * `dest[0]` is set to `\0`.
 */
extern size_t oasis_time_str(char *dest, size_t dest_size, const oasis_tm_t* timestamp);


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OASIS_TIME_H_ */
