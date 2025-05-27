/*
 * oasis_wildcard.c - OASIS Filename Wildcard Matching Implementation
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#include "oasis.h"
#include <string.h>
#include <ctype.h> /* For toupper */
#include <stdio.h> /* For snprintf */

/* Static helper function for recursive pattern matching */
static bool pattern_match_recursive(const char* text, const char* pattern) {
    /* Base cases */
    if (*pattern == '\0') { /* Pattern is exhausted */
        return *text == '\0'; /* True if text is also exhausted */
    }

    if (*pattern == '*') {
        /* '*' can match zero characters (try pattern+1 with current text)
           OR '*' can match one character from text and then '*' continues to match
           the rest of the text (current pattern with text+1 if text is not empty) */
        return (pattern_match_recursive(text, pattern + 1) ||
            (*text != '\0' && pattern_match_recursive(text + 1, pattern)));
    }

    /* If pattern is not '*' or '\0', but text is exhausted, then no match */
    if (*text == '\0') {
        return false;
    }

    /* If pattern char is '?' or matches text char (case-insensitive) */
    if (*pattern == '?' || (toupper((unsigned char)*text) == toupper((unsigned char)*pattern))) {
        return pattern_match_recursive(text + 1, pattern + 1);
    }

    /* All other cases (literal mismatch) */
    return false;
}

bool oasis_filename_wildcard_match(const char* fname_deb, const char* ftype_deb, const char* pattern_arg) {
    char actual_fname[FNAME_LEN + 1];
    char actual_ftype[FTYPE_LEN + 1];
    char full_filename_str[FNAME_LEN + FTYPE_LEN + 2]; /* FNAME + . + FTYPE + \0 */
    char pattern_to_match[256]; /* Use a mutable copy for the pattern */
    int i;

    if (!fname_deb || !ftype_deb || !pattern_arg) {
        return false;
    }

    /* Prepare DEB filename part */
    strncpy(actual_fname, fname_deb, FNAME_LEN);
    actual_fname[FNAME_LEN] = '\0';
    for (i = (int)strlen(actual_fname) - 1; i >= 0 && actual_fname[i] == ' '; i--);
    actual_fname[i + 1] = '\0';

    /* Prepare DEB filetype part */
    strncpy(actual_ftype, ftype_deb, FTYPE_LEN);
    actual_ftype[FTYPE_LEN] = '\0';
    for (i = (int)strlen(actual_ftype) - 1; i >= 0 && actual_ftype[i] == ' '; i--);
    actual_ftype[i + 1] = '\0';

    /* Construct "FNAME.FTYPE" string */
    if (strlen(actual_ftype) > 0) {
        snprintf(full_filename_str, sizeof(full_filename_str), "%s.%s", actual_fname, actual_ftype);
    } else {
        snprintf(full_filename_str, sizeof(full_filename_str), "%s.", actual_fname);
    }

    strncpy(pattern_to_match, pattern_arg, sizeof(pattern_to_match) - 1);
    pattern_to_match[sizeof(pattern_to_match) - 1] = '\0';

    /*
     * Case insensitivity is handled by pattern_match_recursive.
     * Forcing both to uppercase here ensures consistency if the recursive
     * function's behavior changes or for easier debugging.
     */
    for (i = 0; full_filename_str[i]; i++) {
        full_filename_str[i] = (char)toupper((unsigned char)full_filename_str[i]);
    }
    for (i = 0; pattern_to_match[i]; i++) {
        pattern_to_match[i] = (char)toupper((unsigned char)pattern_to_match[i]);
    }

    return pattern_match_recursive(full_filename_str, pattern_to_match);
}
