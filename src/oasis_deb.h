/*
 * oasis_deb.h - OASIS Directory Entry Block (DEB) Manipulation Utilities Interface
 *
 * This header file declares functions for converting between OASIS Directory
 * Entry Block (DEB) structures and host-compatible filename strings. It also
 * provides utilities for validating DEBs and extracting filename components.
 * Part of the oasis-utils library.
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#ifndef OASIS_DEB_H_
#define OASIS_DEB_H_

#include "oasis.h"


#ifdef __cplusplus
extern "C" {
#endif

 /* Define a reasonable maximum length for the generated host filename */
#define MAX_HOST_FILENAME_LEN 256

/* Define a maximum length for the FNAME.FTYPE string */
#define MAX_FNAME_FTYPE_LEN (FNAME_LEN + 1 + FTYPE_LEN + 1) /* FNAME + '.' + FTYPE + '\0' */

/* --- Function Declarations --- */

/**
 * @brief Converts an OASIS DEB structure to a host filename string.
 *
 * Encodes the file format, attributes, and format-dependent information
 * from the DEB into a string according to the oasis-utils convention.
 * The convention aims to make OASIS file metadata readable and usable
 * in host filenames (e.g., "FILENAME.EXT_S_128"). Trailing spaces in
 * FNAME and FTYPE are removed from the host filename representation.
 *
 * @param deb Pointer to the source directory_entry_block_t structure.
 * @param host_filename Buffer to store the resulting host filename string.
 * @param buffer_size Size of the host_filename buffer.
 * @return true if conversion was successful and the full name fit the buffer,
 * false otherwise (e.g., NULL input, invalid DEB, buffer too small).
 */
bool oasis_deb_to_host_filename(const directory_entry_block_t* deb, char* host_filename, size_t buffer_size);

/**
 * @brief Converts a host filename string (conforming to convention) to an OASIS DEB structure.
 *
 * Parses the encoded metadata from the host filename and populates the
 * fields of the provided DEB structure. Assumes the input filename strictly
 * follows the oasis-utils convention. FNAME and FTYPE fields in the DEB
 * will be space-padded.
 *
 * @param host_filename The host filename string to parse.
 * @param deb Pointer to the directory_entry_block_t structure to populate.
 * @return true if parsing was successful and a valid DEB could be formed,
 * false otherwise (e.g., NULL input, invalid filename format).
 */
bool host_filename_to_oasis_deb(const char* host_filename, directory_entry_block_t* deb);

/**
 * @brief Extracts the OASIS filename and filetype from a DEB into "FNAME.FTYPE" format.
 *
 * Creates a string in the format "FNAME.FTYPE", removing trailing spaces
 * from both the filename and filetype parts.
 *
 * @param deb Pointer to the source directory_entry_block_t structure.
 * @param fname_ftype_str Buffer to store the resulting "FNAME.FTYPE" string.
 * @param buffer_size Size of the fname_ftype_str buffer.
 * @return true if extraction was successful and the string fit the buffer,
 * false otherwise (e.g., NULL input, invalid DEB, buffer too small).
 */
bool oasis_deb_get_fname_ftype(const directory_entry_block_t* deb, char* fname_ftype_str, size_t buffer_size);

/**
 * @brief Checks if a Directory Entry Block (DEB) is valid for use.
 *
 * A DEB is considered valid if it is not marked as EMPTY (0x00) or
 * DELETED (0xFF), and its file format type (bits 4:0 of file_format byte)
 * corresponds to a recognized OASIS file type.
 *
 * @param deb Pointer to the directory_entry_block_t structure to check.
 * @return true if the DEB represents an active, valid file entry, false otherwise or if deb is NULL.
 */
bool oasis_deb_is_valid(const directory_entry_block_t* deb);


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OASIS_DEB_H_ */
