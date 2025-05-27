/*
 * oasis_file_rename.h - OASIS File Renaming Utilities Interface
 *
 * This header file declares functions for renaming files within an OASIS
 * disk image by modifying their Directory Entry Blocks (DEBs).
 * Part of the oasis-utils library.
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#ifndef OASIS_FILE_RENAME_H_
#define OASIS_FILE_RENAME_H_

/* Include oasis.h for core definitions */
#include "oasis.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Renames a single specified file by modifying its Directory Entry Block (DEB).
 *
 * This function updates the `file_name` and `file_type` fields of the provided
 * DEB structure in memory. It ensures that the new name and type parts are
 * correctly space-padded according to OASIS conventions.
 *
 * **Note:** This function only modifies the DEB in memory. It does NOT:
 * - Check for name collisions with existing files.
 * - Write the modified directory back to the disk image.
 * These responsibilities lie with the caller (e.g., `oasis_rename_file_by_pattern_and_name`).
 * The file's timestamp is typically preserved by this operation.
 *
 * @param deb_to_rename Pointer to the `directory_entry_block_t` of the file to be renamed.
 * This DEB structure will be modified in place. Must not be NULL.
 * @param new_fname_part The new filename part (e.g., "NEWNAME").
 * Must not be NULL. It will be truncated if longer than `FNAME_LEN`.
 * Caller should ensure it's already uppercased if required by OASIS.
 * @param new_ftype_part The new filetype part (e.g., "EXT").
 * Must not be NULL. It will be truncated if longer than `FTYPE_LEN`.
 * Caller should ensure it's already uppercased if required by OASIS.
 * @return true if the DEB was updated in memory successfully.
 * false if `deb_to_rename`, `new_fname_part`, or `new_ftype_part` is NULL.
 */
bool oasis_rename_single_file_deb(directory_entry_block_t* deb_to_rename,
                                  const char* new_fname_part,
                                  const char* new_ftype_part);

/**
 * @brief Renames a file on the OASIS disk image, identified by its old name (pattern)
 * and owner ID, to a new name.
 *
 * This function searches the disk directory for a file that matches the `options->pattern`
 * (interpreted as the exact old filename, e.g., "OLDFILE.TXT") and `options->owner_id_filter`.
 * - If a unique match is found, its DEB is updated with the `new_filename_str` (parsed into
 * FNAME and FTYPE, uppercased, and space-padded).
 * - Before renaming, it checks if the `new_filename_str` already exists for the same
 * owner ID (or any owner if `owner_id_filter` is wildcard), preventing duplicate names.
 * - If the rename is successful in memory, the modified directory is written back to the
 * disk image file.
 *
 * @param img_stream Pointer to the `sector_io_stream_t` for the open OASIS disk image.
 * The stream must be opened in a mode that allows writing (e.g., "r+b").
 * @param disk_layout Pointer to the `oasis_disk_layout_t` structure, which includes the
 * directory. The relevant DEB within this structure will be modified,
 * and the entire directory will be written back to disk.
 * @param options Pointer to the `cli_options_t` structure. `options->pattern` is used
 * as the exact old filename (e.g., "OLDFILE.TXT"). `options->owner_id_filter`
 * specifies the owner of the file to rename.
 * @param new_filename_str The new filename string (e.g., "NEWNAME.EXT" or "NEWNAMEONLY").
 * This will be parsed into FNAME and FTYPE parts.
 * @return true if the rename operation was successful (file found, new name valid,
 * directory written to disk) OR if no file matched the old pattern (nothing to rename).
 * false if an error occurred (e.g., NULL arguments, old name matched multiple files,
 * new name already exists, new name parts too long, error writing directory to disk).
 * Error messages are printed to stderr.
 */
bool oasis_rename_file_by_pattern_and_name(sector_io_stream_t* img_stream,
                                           oasis_disk_layout_t* disk_layout,
                                           const cli_options_t* options,
                                           const char* new_filename_str);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OASIS_FILE_RENAME_H_ */
