/*
 * oasis_deb.c - Implementation for OASIS Directory Entry Block (DEB) manipulation.
 *
 * Provides functions to convert between OASIS DEB structures and host filenames,
 * extract basic file identifiers, and validate DEBs.
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#include "oasis.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdint.h>

/* Define constants based on oasis.h for clarity in logic */
#define MASK_FILE_TYPE (FILE_FORMAT_MASK)
#define MASK_ATTR      (FILE_ATTRIBUTE_MASK)
#define ATTR_R         (FILE_FORMAT_READ_PROTECTED)
#define ATTR_W         (FILE_FORMAT_WRITE_PROTECTED)
#define ATTR_D         (FILE_FORMAT_DELETE_PROTECTED) /* Use define from oasis.h */

/* --- Helper Functions --- */

/**
 * @brief Trims trailing spaces from a string in place.
 * @param str The string to trim.
 */
static void trim_trailing_spaces(char* str) {
    /* Check if the string is NULL. */
    if (str == NULL) return;
    int len = (int)strlen(str);
    /* Iterate backwards from the end of the string. */
    while (len > 0 && isspace((unsigned char)str[len - 1])) {
        len--; /* Decrement length if a space is found. */
    }
    str[len] = '\0'; /* Null-terminate the trimmed string. */
}

/**
 * @brief Modifies the pointed-to string pointer to skip leading spaces.
 * @param str_ptr Pointer to the char pointer of the string.
 */
static void trim_leading_spaces(char** str_ptr) {
    /* Check if the pointer or the string it points to is NULL. */
    if (str_ptr == NULL || *str_ptr == NULL) return;
    /* Advance the pointer while the character is a space. */
    while (**str_ptr != '\0' && isspace((unsigned char)**str_ptr)) {
        (*str_ptr)++;
    }
}


/* --- Library Function Implementations --- */

/**
 * @brief Checks if a Directory Entry Block (DEB) is valid.
 */
bool oasis_deb_is_valid(const directory_entry_block_t* deb) {
    uint8_t file_format;
    uint8_t file_type;

    if (deb == NULL) {
        return false; /* Cannot validate a NULL pointer */
    }

    file_format = deb->file_format;

    /* Check if the entry is marked as empty or deleted */
    if (file_format == FILE_FORMAT_EMPTY || file_format == FILE_FORMAT_DELETED) {
        return false;
    }

    /* Extract the file type bits */
    file_type = file_format & MASK_FILE_TYPE;

    /* Check if the file type is one of the known valid types */
    switch (file_type) {
    case FILE_FORMAT_RELOCATABLE:
    case FILE_FORMAT_ABSOLUTE:
    case FILE_FORMAT_SEQUENTIAL:
    case FILE_FORMAT_DIRECT:
    case FILE_FORMAT_INDEXED:
    case FILE_FORMAT_KEYED:
        return true; /* Valid type found */
    default:
        return false; /* Unknown or invalid file type */
    }
}


/**
 * @brief Extracts the OASIS filename and filetype from a DEB.
 */
bool oasis_deb_get_fname_ftype(const directory_entry_block_t* deb, char* fname_ftype_str, size_t buffer_size) {
    char fname_buf_for_trim[FNAME_LEN + 1];
    char ftype_buf_for_trim[FTYPE_LEN + 1];
    char* fname_trimmed_ptr;
    char* ftype_trimmed_ptr;
    int written;

    if (deb == NULL || fname_ftype_str == NULL || buffer_size == 0) {
        if (fname_ftype_str != NULL && buffer_size > 0) {
            fname_ftype_str[0] = '\0';
        }
        return false;
    }
    /* Check DEB validity after ensuring fname_ftype_str can be cleared on error */
    if (!oasis_deb_is_valid(deb)) {
        if (fname_ftype_str != NULL && buffer_size > 0) {
            fname_ftype_str[0] = '\0';
        }
        return false;
    }


    /* Copy and null-terminate FNAME and FTYPE */
    strncpy(fname_buf_for_trim, deb->file_name, FNAME_LEN);
    fname_buf_for_trim[FNAME_LEN] = '\0';
    strncpy(ftype_buf_for_trim, deb->file_type, FTYPE_LEN);
    ftype_buf_for_trim[FTYPE_LEN] = '\0';

    /* Point to the start for leading space trimming */
    fname_trimmed_ptr = fname_buf_for_trim;
    ftype_trimmed_ptr = ftype_buf_for_trim;

    trim_leading_spaces(&fname_trimmed_ptr);
    trim_leading_spaces(&ftype_trimmed_ptr);

    /* Trim trailing spaces (from the potentially advanced pointers) */
    trim_trailing_spaces(fname_trimmed_ptr);
    trim_trailing_spaces(ftype_trimmed_ptr);


    /* Combine into FNAME.FTYPE format */
    written = snprintf(fname_ftype_str, buffer_size, "%s.%s", fname_trimmed_ptr, ftype_trimmed_ptr);

    /* Check for truncation or encoding errors */
    return (written > 0 && (size_t)written < buffer_size);
}

/**
 * @brief Converts an OASIS DEB structure to a host filename string.
 */
bool oasis_deb_to_host_filename(const directory_entry_block_t* deb, char* host_filename, size_t buffer_size) {
    char fname_ftype_buf[MAX_FNAME_FTYPE_LEN];
    char suffix[MAX_HOST_FILENAME_LEN]; /* Buffer for the metadata suffix */
    char type_char = '?';
    char attr_str[4] = { 0 }; /* Max 3 attrs (RWD) + null terminator */
    int attr_idx = 0;
    uint16_t record_len_val = 0;
    uint16_t key_len_val = 0;
    uint16_t load_addr_val = 0;
    int written_len = 0;
    uint8_t file_type;
    uint8_t attributes;


    /* Check validity first */
    if (deb == NULL || host_filename == NULL || buffer_size == 0 || !oasis_deb_is_valid(deb)) {
        if (host_filename != NULL && buffer_size > 0) {
            host_filename[0] = '\0'; /* Clear output buffer on error */
        }
        return false;
    }

    /* Get the base FNAME.FTYPE by calling the corrected local function */
    if (!oasis_deb_get_fname_ftype(deb, fname_ftype_buf, sizeof(fname_ftype_buf))) {
        return false; /* Error creating base filename part */
    }

    /* Determine File Type Character and specific fields */
    file_type = deb->file_format & MASK_FILE_TYPE;
    attributes = deb->file_format & MASK_ATTR;

    /* Determine attributes string */
    if (attributes & ATTR_R) attr_str[attr_idx++] = 'R';
    if (attributes & ATTR_W) attr_str[attr_idx++] = 'W';
    if (attributes & ATTR_D) attr_str[attr_idx++] = 'D';
    attr_str[attr_idx] = '\0'; /* Null terminate attributes */

    suffix[0] = '\0'; /* Initialize suffix */

    /* Extract format-dependent fields and build suffix */
    switch (file_type) {
    case FILE_FORMAT_SEQUENTIAL:
        type_char = 'S';
        record_len_val = deb->file_format_dependent1; /* Longest record */
        /* Only append record length if it's non-zero, or if attributes exist.
           If FFD1 is 0 and no attributes, suffix is just "_S".
        */
        if (record_len_val > 0 || strlen(attr_str) > 0) {
            written_len = snprintf(suffix, sizeof(suffix), "_%c%s_%u",
                type_char, attr_str, record_len_val);
        }
        else { /* No attributes and FFD1 is 0 means simple NAME.TYPE_S */
            written_len = snprintf(suffix, sizeof(suffix), "_%c", type_char);
        }
        break;

    case FILE_FORMAT_DIRECT:
        type_char = 'D';
        record_len_val = deb->file_format_dependent1; /* Allocated Record Length */
        written_len = snprintf(suffix, sizeof(suffix), "_%c%s_%u",
            type_char, attr_str, record_len_val);
        break;

    case FILE_FORMAT_RELOCATABLE:
        type_char = 'R';
        record_len_val = deb->file_format_dependent1; /* Should equal SECTOR_LEN */
        written_len = snprintf(suffix, sizeof(suffix), "_%c%s_%u",
            type_char, attr_str, record_len_val);
        break;

    case FILE_FORMAT_ABSOLUTE:
        type_char = 'A';
        record_len_val = deb->file_format_dependent1; /* Should equal SECTOR_LEN */
        load_addr_val = deb->file_format_dependent2;  /* Load address */
        written_len = snprintf(suffix, sizeof(suffix), "_%c%s_%u_%04X", /* Hex, 4 digits, uppercase */
            type_char, attr_str, record_len_val, load_addr_val);
        break;

    case FILE_FORMAT_INDEXED:
    case FILE_FORMAT_KEYED:
        if (file_type == FILE_FORMAT_KEYED) {
            type_char = 'K';
        }
        else {
            type_char = 'I';
        }
        record_len_val = deb->file_format_dependent1 & 0x1FF; /* Bits 8:0 */
        key_len_val = (deb->file_format_dependent1 >> 9) & 0x7F; /* Bits 15:9 */
        written_len = snprintf(suffix, sizeof(suffix), "_%c%s_%u_%u",
            type_char, attr_str, record_len_val, key_len_val);
        break;

    default:
        /* This case should ideally not be reached if oasis_deb_is_valid passed */
        strncpy(suffix, "_INVALIDTYPE", sizeof(suffix) - 1);
        suffix[sizeof(suffix) - 1] = '\0';
        written_len = (int)strlen(suffix);
        break;
    }

    if (written_len <= 0 || (size_t)written_len >= sizeof(suffix)) {
        /* Suffix generation error or truncation */
    }


    written_len = snprintf(host_filename, buffer_size, "%s%s", fname_ftype_buf, suffix);
    return (written_len > 0 && (size_t)written_len < buffer_size);
}


/**
 * @brief Converts a host filename string (conforming to the convention) back to an OASIS DEB structure.
 */
bool host_filename_to_oasis_deb(const char* host_filename, directory_entry_block_t* deb) {
    char filename_copy_for_metadata[MAX_HOST_FILENAME_LEN]; /* For parsing metadata part with strtok or sscanf */
    char temp_fname_part[FNAME_LEN + 1] = { 0 };
    char temp_ftype_part[FTYPE_LEN + 1] = { 0 };

    char* current_parse_ptr_mutable = NULL;
    char type_char_from_name = 0;
    uint8_t file_type_val = 0;
    uint8_t attributes_val = 0;
    unsigned long val1 = 0, val2 = 0; /* For sscanf results */
    int n_chars_consumed = 0; /* For sscanf %n */

    const char* p_orig_host_filename;
    const char* dot_separator_ptr;
    const char* metadata_separator_ptr; /* Points to the first '_' indicating metadata */
    size_t part_len;

    if (host_filename == NULL || deb == NULL) {
        return false;
    }

    /* Initialize DEB with zeros then spaces for name/type */
    memset(deb, 0, sizeof(directory_entry_block_t));
    memset(deb->file_name, ' ', FNAME_LEN);
    memset(deb->file_type, ' ', FTYPE_LEN);

    p_orig_host_filename = host_filename;
    dot_separator_ptr = strchr(p_orig_host_filename, '.');
    metadata_separator_ptr = strchr(p_orig_host_filename, '_');

    /*
     * Extract FNAME part
     */
    if (dot_separator_ptr != NULL && (metadata_separator_ptr == NULL || dot_separator_ptr < metadata_separator_ptr)) {
        /* Case: "FNAME.TYPE_META" or ".TYPE_META" or "FNAME." */
        if (dot_separator_ptr == p_orig_host_filename) { /* Starts with '.', FNAME is empty */
            /* temp_fname_part remains empty (all zeros) */
        }
        else { /* "FNAME. ..." */
            part_len = dot_separator_ptr - p_orig_host_filename;
            if (part_len > FNAME_LEN) { fprintf(stderr, "Error: FNAME part too long in '%s'.\n", host_filename); return false; }
            memcpy(temp_fname_part, p_orig_host_filename, part_len);
            temp_fname_part[part_len] = '\0'; /* Null terminate */
        }
    }
    else if (metadata_separator_ptr != NULL) { /* No dot, or dot is part of metadata (e.g. FNAME_META.ext) - FNAME is up to metadata */
        /* Case: "FNAME_META" */
        part_len = metadata_separator_ptr - p_orig_host_filename;
        if (part_len > FNAME_LEN) { fprintf(stderr, "Error: FNAME part too long in '%s'.\n", host_filename); return false; }
        memcpy(temp_fname_part, p_orig_host_filename, part_len);
        temp_fname_part[part_len] = '\0';
    }
    else { /* No dot, no underscore, e.g., "FNAMEONLY" */
        part_len = strlen(p_orig_host_filename);
        if (part_len > FNAME_LEN) { fprintf(stderr, "Error: Invalid FNAME part in '%s'.\n", host_filename); return false; }
        memcpy(temp_fname_part, p_orig_host_filename, part_len);
        temp_fname_part[part_len] = '\0';
    }

    /*
     * Extract FTYPE part
     */
    if (dot_separator_ptr != NULL) { /* Dot exists */
        const char* type_candidate_start = dot_separator_ptr + 1;
        /* End of FTYPE is either end of string, or start of metadata */
        const char* type_end_ptr = (metadata_separator_ptr != NULL && metadata_separator_ptr > type_candidate_start) ? metadata_separator_ptr : (p_orig_host_filename + strlen(p_orig_host_filename));

        part_len = type_end_ptr - type_candidate_start;
        if (part_len > FTYPE_LEN) { fprintf(stderr, "Error: FTYPE part too long in '%s'.\n", host_filename); return false; }
        if (part_len > 0) {
            memcpy(temp_ftype_part, type_candidate_start, part_len);
            temp_ftype_part[part_len] = '\0';
        }
        /* If part_len is 0 (e.g. "FNAME."), temp_ftype_part remains empty */
    }
    else {
        /* No dot, so FTYPE is implicitly empty */
        /* temp_ftype_part remains empty */
    }

    /*
     * Validate specific problematic cases like "."
     * If FNAME and FTYPE are both empty, and there's no metadata, it's invalid.
     * The primary case for "Invalid FNAME part" is filename like "FNAMEONLYTOOLONG"
     * or if after parsing, the fname part is truly invalid (e.g. only delimiters)
     */
    if (strcmp(host_filename, ".") == 0) {
        fprintf(stderr, "Error: Invalid FNAME part in '%s'.\n", host_filename);
        return false;
    }
    /* An empty FNAME is valid (e.g. ".PROFILE"), but an empty FNAME *and* empty FTYPE with no metadata is not implied by test */

    /* Convert to uppercase before storing in DEB */
    for (char *p = temp_fname_part; *p; ++p) *p = (char)toupper((unsigned char)*p);
    for (char *p = temp_ftype_part; *p; ++p) *p = (char)toupper((unsigned char)*p);

    /* Copy extracted and validated parts to DEB, ensuring space padding by initial memset */
    memcpy(deb->file_name, temp_fname_part, strlen(temp_fname_part));
    memcpy(deb->file_type, temp_ftype_part, strlen(temp_ftype_part));

    /*
     * Parse metadata suffix if it exists
     */
    if (metadata_separator_ptr == NULL || *(metadata_separator_ptr + 1) == '\0') {
        /* No metadata suffix (e.g. "FNAME.TYPE", ".PROFILE", "FNAMEONLY") */
        /* Default to Sequential format, record length 0 (to be calculated by caller for writes) */
        deb->file_format = FILE_FORMAT_SEQUENTIAL;
        deb->file_format_dependent1 = 0;
        return true;
    }

    /* Copy metadata part (after '_') to a mutable buffer for parsing */
    strncpy(filename_copy_for_metadata, metadata_separator_ptr + 1, sizeof(filename_copy_for_metadata) - 1);
    filename_copy_for_metadata[sizeof(filename_copy_for_metadata) - 1] = '\0';

    current_parse_ptr_mutable = filename_copy_for_metadata;
    if (*current_parse_ptr_mutable == '\0') { /* Should be caught by above, but defensive */
        deb->file_format = FILE_FORMAT_SEQUENTIAL; deb->file_format_dependent1 = 0; return true;
    }
    type_char_from_name = (char)toupper((unsigned char)current_parse_ptr_mutable[0]);
    current_parse_ptr_mutable++; /* Advance past type character */

    /* Parse attributes (R, W, D) */
    while (*current_parse_ptr_mutable != '\0' && *current_parse_ptr_mutable != '_') {
        switch (toupper(*current_parse_ptr_mutable)) {
        case 'R': attributes_val |= ATTR_R; break;
        case 'W': attributes_val |= ATTR_W; break;
        case 'D': attributes_val |= ATTR_D; break;
        default:
            fprintf(stderr, "Error: Invalid attribute character '%c' in host filename '%s'.\n", *current_parse_ptr_mutable, host_filename);
            return false;
        }
        current_parse_ptr_mutable++;
    }

    /* Expect underscore before numeric parts, or end of string if numbers are optional */
    if (*current_parse_ptr_mutable == '_') {
        current_parse_ptr_mutable++; /* Skip the underscore */
    }
    else if (*current_parse_ptr_mutable != '\0') {
        /* If not EOL and not '_', then it's an invalid format after attributes. E.g. FNAME.TYPE_SRX */
        fprintf(stderr, "Error: Invalid character '%c' after attributes in host filename '%s'. Expected '_' or EOL.\n", *current_parse_ptr_mutable, host_filename);
        return false;
    }

    /* Parse numeric parts based on type_char_from_name */
    switch (type_char_from_name) {
    case 'S':
        file_type_val = FILE_FORMAT_SEQUENTIAL;
        if (*current_parse_ptr_mutable == '\0') { /* NAME.TYPE_S or NAME.TYPE_S[ATTRS] (no record length specified) */
            val1 = 0; /* Default to 0, to be calculated by caller */
        }
        /* sscanf to read the number and check if anything remains using %n */
        else if (sscanf(current_parse_ptr_mutable, "%lu%n", &val1, &n_chars_consumed) != 1 || current_parse_ptr_mutable[n_chars_consumed] != '\0') {
            fprintf(stderr, "Error: Invalid record length for Sequential file in '%s'.\n", host_filename);
            return false;
        }
        deb->file_format_dependent1 = (uint16_t)val1;
        /* FFD2 for Sequential (last sector) is set by the writing logic based on actual file content. */
        break;

    case 'D':
        file_type_val = FILE_FORMAT_DIRECT;
        if (*current_parse_ptr_mutable == '\0' || sscanf(current_parse_ptr_mutable, "%lu%n", &val1, &n_chars_consumed) != 1 || current_parse_ptr_mutable[n_chars_consumed] != '\0') {
            fprintf(stderr, "Error: Missing or invalid record length for Direct file in '%s'.\n", host_filename);
            return false;
        }
        if (val1 == 0) { /* Record length for Direct must be non-zero */
            fprintf(stderr, "Error: Record length for Direct file cannot be 0 in '%s'.\n", host_filename);
            return false;
        }
        deb->file_format_dependent1 = (uint16_t)val1;
        deb->file_format_dependent2 = 0; /* Always 0 for Direct */
        break;

    case 'R':
        file_type_val = FILE_FORMAT_RELOCATABLE;
        if (*current_parse_ptr_mutable == '\0' || sscanf(current_parse_ptr_mutable, "%lu%n", &val1, &n_chars_consumed) != 1 || current_parse_ptr_mutable[n_chars_consumed] != '\0') {
            fprintf(stderr, "Error: Missing or invalid record length for Relocatable file in '%s'.\n", host_filename);
            return false;
        }
        deb->file_format_dependent1 = (uint16_t)val1; /* Record Length (SECTOR_LEN) */
        /* FFD2 (Program Length) set by caller based on file size. */
        break;

    case 'A':
        file_type_val = FILE_FORMAT_ABSOLUTE;
        /* For Absolute, expect RRR_LLLL */
        if (*current_parse_ptr_mutable == '\0' || sscanf(current_parse_ptr_mutable, "%lu_%lx%n", &val1, &val2, &n_chars_consumed) != 2 || current_parse_ptr_mutable[n_chars_consumed] != '\0') {
            fprintf(stderr, "Error: Missing or invalid record length/load address for Absolute file in '%s'.\n", host_filename);
            return false;
        }
        deb->file_format_dependent1 = (uint16_t)val1; /* Record Length (SECTOR_LEN) */
        deb->file_format_dependent2 = (uint16_t)val2; /* Load address */
        break;

    case 'I':
    case 'K':
        file_type_val = (type_char_from_name == 'I') ? FILE_FORMAT_INDEXED : FILE_FORMAT_KEYED;
        /* Expect RRR_KKK */
        if (*current_parse_ptr_mutable == '\0' || sscanf(current_parse_ptr_mutable, "%lu_%lu%n", &val1, &val2, &n_chars_consumed) != 2 || current_parse_ptr_mutable[n_chars_consumed] != '\0') {
            fprintf(stderr, "Error: Missing or invalid record/key length for %s file in '%s'.\n", (type_char_from_name == 'I' ? "Indexed" : "Keyed"), host_filename);
            return false;
        }
        if (val1 > 0x1FF || val2 > 0x7F) { /* Validate parsed record_len (val1) and key_len (val2) */
            fprintf(stderr, "Error: Record length (max 511) or key length (max 127) out of range for %s file in '%s'.\n", (type_char_from_name == 'I' ? "Indexed" : "Keyed"), host_filename);
            return false;
        }
        deb->file_format_dependent1 = ((uint16_t)val2 << 9) | ((uint16_t)val1 & 0x1FF); /* val2 is key_len, val1 is rec_len */
        /* FFD2 (Allocated Size) set by caller based on file size. */
        break;

    default:
        fprintf(stderr, "Error: Unknown file type character '%c' in host filename '%s'.\n", type_char_from_name, host_filename);
        return false;
    }

    deb->file_format = file_type_val | attributes_val;
    return true;
}
