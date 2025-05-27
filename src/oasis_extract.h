/*
 * oasis_extract.h - OASIS Disk Image File Extraction Utilities Interface
 *
 * This header file declares functions for extracting files from OASIS
 * disk images to the host filesystem. It supports filtering by owner ID
 * and filename patterns, as well as ASCII line ending conversion.
 * Part of the oasis-utils library.
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#ifndef OASIS_EXTRACT_H_
#define OASIS_EXTRACT_H_

#include <stdio.h>   /* For FILE */

/* Include oasis.h for definitions like sector_io_stream_t, oasis_disk_layout_t, etc. */
#include "oasis.h"

#ifdef __cplusplus
extern "C" {
#endif

    /**
     * @brief Structure to hold command line options relevant to extraction operations.
     *
     * This structure is used by various extraction functions to determine behavior
     * such as filtering, pattern matching, and ASCII conversion.
     */
    typedef struct {
        const char* image_path;       /**< Path to the OASIS disk image file. */
        const char* operation;        /**< String representing the operation (e.g., "extract"). */
        const char* pattern;          /**< Wildcard pattern for filename matching (e.g., "*.TXT", "FILE?.BAS"). NULL or empty for all files. */
        bool ascii_conversion;        /**< Flag: true to enable ASCII line ending conversion, false otherwise. */
        int owner_id_filter;          /**< User ID to filter files by. Use OWNER_ID_WILDCARD for any owner. */
    } cli_options_t;

    /** @brief Defines the value used for wildcard owner ID filtering. */
#define OWNER_ID_WILDCARD -1


/* --- File Extraction Functions --- */

/**
 * @brief Extracts all valid files from the OASIS disk image to the host filesystem,
 * filtered by options.
 *
 * This function serves as a primary entry point for extraction. It typically
 * calls `extract_files_matching_pattern` internally, using the pattern and
 * owner ID specified in the `options` structure. If `options->pattern` is
 * NULL or "*.*" (or similar wildcard for all), all files matching the
 * `owner_id_filter` are extracted.
 *
 * @param img_stream Pointer to the open `sector_io_stream_t` for the OASIS disk image.
 * @param disk_layout Pointer to the populated `oasis_disk_layout_t` structure.
 * @param output_dir_path Path to the directory on the host filesystem where files will be extracted.
 * The directory will be created if it doesn't exist.
 * @param options Pointer to the `cli_options_t` structure containing extraction parameters
 * (like `owner_id_filter`, `pattern`, `ascii_conversion`).
 * If NULL, defaults are applied (typically user 0, no pattern, no ASCII conversion).
 * @return true if the overall operation completed without critical errors (some individual
 * file errors might still occur and be reported to stderr).
 * false if a critical setup error occurred (e.g., cannot create output directory).
 */
    bool extract_files(sector_io_stream_t* img_stream,
        const oasis_disk_layout_t* disk_layout,
        const char* output_dir_path,
        const cli_options_t* options);

    /**
     * @brief Extracts files from the OASIS disk image that match a given pattern and owner ID.
     *
     * Iterates through the disk directory, filters files based on `options->owner_id_filter`
     * and `options->pattern`. For each matching file, it reads the data from the disk image,
     * performs ASCII conversion if `options->ascii_conversion` is true and the file is 7-bit ASCII,
     * and writes the content to the `output_dir_path` using a host-compatible filename
     * derived from the OASIS DEB. Timestamps are preserved if supported by the host.
     *
     * @param img_stream Pointer to the open `sector_io_stream_t` for the OASIS disk image.
     * @param disk_layout Pointer to the populated `oasis_disk_layout_t` structure.
     * @param output_dir_path Path to the directory where matching files will be extracted.
     * The directory will be created if it doesn't exist.
     * @param options Pointer to the `cli_options_t` structure specifying the pattern,
     * owner ID, and ASCII conversion settings. Must not be NULL.
     * @return true if the operation completed and all selected files were extracted successfully.
     * false if a critical setup error occurred or if any selected file failed to extract.
     */
    bool extract_files_matching_pattern(sector_io_stream_t* img_stream,
        const oasis_disk_layout_t* disk_layout,
        const char* output_dir_path,
        const cli_options_t* options);

    /* --- Utility Functions used by Extraction --- */

    /**
     * @brief Sets the access and modification timestamp of a file on the host system.
     *
     * Attempts to set the host file's last modification time to match the
     * timestamp provided from the OASIS DEB. The access time is typically set
     * to the same value.
     *
     * @param filepath Path to the host file whose timestamp is to be set.
     * @param oasis_ts Pointer to the `oasis_tm_t` structure containing the desired timestamp.
     * @return true on success. Returns true even if timestamping is unsupported
     * on the platform (typically with a warning printed to stderr), as it's
     * considered non-critical. Returns false on a definitive error during
     * the timestamp setting attempt (e.g., file not found, permission issue).
     */
    bool set_file_timestamp(const char* filepath, const oasis_tm_t* oasis_ts);

    /**
     * @brief Creates the output directory (if it doesn't exist) and opens a host file for writing.
     *
     * This utility is used during file extraction or reception to prepare the
     * output file on the host system. It ensures the target directory exists
     * and then opens the specified file in write-binary mode ("wb").
     * If not in quiet mode, it prints information about the file being created.
     *
     * @param output_dir_path Path to the output directory on the host system.
     * @param base_filename The name for the host file (e.g., "MYFILE.TXT_S_128").
     * @param ostream Pointer to a `FILE*` which will be set to the opened stream on success.
     * The caller is responsible for closing this stream.
     * @param dir_entry Pointer to the OASIS DEB (primarily for logging purposes).
     * @param quiet If non-zero, suppress informational output.
     * @param debug If non-zero, enable debug output (may be more verbose than non-quiet).
     * @return 0 on success.
     * A negative errno code on failure (e.g., -EINVAL for bad args, -ENOTDIR if
     * `output_dir_path` is not a directory, -EACCES for permission issues, etc.).
     */
    int create_and_open_oasis_file(const char* output_dir_path,
        const char* base_filename,
        FILE** ostream,
        const directory_entry_block_t* dir_entry,
        int quiet,
        int debug);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OASIS_EXTRACT_H_ */
