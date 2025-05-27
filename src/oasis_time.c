/*
 * oasis_time.c - OASIS Time Conversion Utilities Implementation
 *
 * Implements functions defined in oasis_time.h for converting
 * between OASIS timestamps and struct tm, and formatting timestamps.
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>   /* For snprintf, strftime */
#include <string.h>  /* For memset */
#include <time.h>    /* For struct tm */
#include <stdint.h>  /* For standard integer types */

#include "oasis.h"

/* --- Constants --- */
#define OASIS_YEAR_BASE 1977
#define OASIS_YEAR_MIN 0  /* Corresponds to 1977 */
#define OASIS_YEAR_MAX 15 /* Corresponds to 1992 */

/* --- Helper Functions --- */

/* Simple clamp function */
static int clamp(int value, int min_val, int max_val) {
    if (value < min_val) return min_val;
    if (value > max_val) return max_val;
    return value;
}

/* --- Public Function Implementations --- */

/* Convert OASIS timestamp to struct tm */
extern void oasis_convert_timestamp_to_tm(const oasis_tm_t* timestamp, struct tm* tmout) {
    uint8_t month, day, year, hour, minute;

    /* Ensure tmout is initialized */
    memset(tmout, 0, sizeof(struct tm));

    /* Extract components from raw bytes */
    month  = (timestamp->raw[0] >> 4) & 0x0F;
    day    = ((timestamp->raw[0] & 0x0F) << 1) | ((timestamp->raw[1] >> 7) & 0x01);
    year   = (timestamp->raw[1] >> 3) & 0x0F; /* 0-15, relative to 1977 */
    hour   = ((timestamp->raw[1] & 0x07) << 2) | ((timestamp->raw[2] >> 6) & 0x03);
    minute = timestamp->raw[2] & 0x3F;

    /* Clamp values to valid ranges */
    month  = (uint8_t)clamp((int)month, 1, 12);
    day    = (uint8_t)clamp((int)day, 1, 31);
    /* Year is already constrained by 4 bits (0-15) */
    hour   = (uint8_t)clamp((int)hour, 0, 23);
    minute = (uint8_t)clamp((int)minute, 0, 59);

    /* Populate struct tm */
    tmout->tm_year = (year + OASIS_YEAR_BASE) - 1900; /* tm_year is years since 1900 */
    tmout->tm_mon  = month - 1;                       /* tm_mon is 0-11 */
    tmout->tm_mday = day;
    tmout->tm_hour = hour;
    tmout->tm_min  = minute;
    tmout->tm_sec  = 0; /* OASIS does not store seconds */

    /* Indicate DST status is unknown */
    tmout->tm_isdst = -1;

    /* tm_wday and tm_yday are not calculated here.
     * They can be calculated using mktime() if needed,
     * but that requires a valid date which clamping helps ensure.
     */
}

/* Convert struct tm to OASIS timestamp */
extern void oasis_convert_tm_to_timestamp(const struct tm* tmin, oasis_tm_t* timestamp) {
    int year_tm = tmin->tm_year + 1900; /* Full year */
    int oasis_year, month, day, hour, minute;

    /* Clamp input values to fit OASIS ranges */
    oasis_year = clamp(year_tm - OASIS_YEAR_BASE, OASIS_YEAR_MIN, OASIS_YEAR_MAX);
    month      = clamp(tmin->tm_mon + 1, 1, 12); /* tm_mon is 0-11 */
    day        = clamp(tmin->tm_mday, 1, 31);
    hour       = clamp(tmin->tm_hour, 0, 23);
    minute     = clamp(tmin->tm_min, 0, 59);

    /* Pack components into 3 bytes */
    timestamp->raw[0] = (uint8_t)(((month & 0x0F) << 4) | ((day >> 1) & 0x0F));
    timestamp->raw[1] = (uint8_t)(((day & 0x01) << 7) | ((oasis_year & 0x0F) << 3) | ((hour >> 2) & 0x07));
    timestamp->raw[2] = (uint8_t)(((hour & 0x03) << 6) | (minute & 0x3F));
}

/* Format OASIS timestamp to string */
extern size_t oasis_time_str(char *dest, size_t dest_size, const oasis_tm_t* timestamp) {
    struct tm tm_temp;
    size_t result;

    if (dest == NULL || dest_size == 0) {
        return 0; /* Invalid arguments */
    }

    /* Convert OASIS time to struct tm first */
    oasis_convert_timestamp_to_tm(timestamp, &tm_temp);

    /* Format the struct tm into the destination buffer */
    /* Format: MM/DD/YY HH:MM (e.g., 04/23/85 14:30) */
    result = strftime(dest, dest_size, "%m/%d/%y %H:%M", &tm_temp);

    /* strftime returns 0 if the buffer is too small or on other errors.
       It includes the null terminator if there's space. */
    if (result == 0 && dest_size > 0) {
        dest[0] = '\0'; /* Ensure null termination on error if possible */
    }

    return result;
}
