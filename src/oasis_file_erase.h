/*
 * oasis_file_erase.h - OASIS File Erasing Utilities Interface
 *
 * This header file declares functions for erasing files from OASIS disk images.
 * Erasing involves marking the Directory Entry Block (DEB) as deleted and
 * deallocating the associated blocks in the allocation map.
 * Part of the oasis-utils library.
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#ifndef OASIS_FILE_ERASE_H_
#define OASIS_FILE_ERASE_H_

/* Include oasis.h for core definitions */
#include "oasis.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Erases a single specified file from the OASIS disk image.
 *
 * This function marks the given Directory Entry Block (DEB) as deleted,
 * clears its metadata (name, type, block count, start sector, etc.),
 * and deallocates its blocks from the in-memory allocation map.
 * The updated `disk_layout->fsblock.free_blocks` count reflects the freed space.
 *
 * **Note:** This function modifies the in-memory `disk_layout`. The caller
 * (`oasis_erase_files_by_pattern`) is responsible for writing the modified
 * directory and allocation map back to the disk image file.
 *
 * @param img_stream Pointer to the `sector_io_stream_t` for the open OASIS disk image.
 * Used to read sector chains for sequential files to identify all blocks.
 * @param disk_layout Pointer to the `oasis_disk_layout_t` structure, which includes the
 * directory and allocation map. This structure will be modified in memory.
 * @param deb_to_erase Pointer to the `directory_entry_block_t` of the file to be erased.
 * This DEB entry within `disk_layout->directory` will be modified.
 * @param deb_index The index of the DEB in the `disk_layout->directory->directory` array.
 * Used for logging and error reporting.
 * @return true if the erase operation for this specific DEB was successful (blocks deallocated
 * from map and DEB marked deleted in memory).
 * false if an error occurred (e.g., failure to read sector chain for a sequential file,
 * error deallocating blocks in the map). Errors are reported to stderr.
 */
bool oasis_erase_single_file(sector_io_stream_t* img_stream,
                             oasis_disk_layout_t* disk_layout,
                             directory_entry_block_t* deb_to_erase,
                             size_t deb_index);

/**
 * @brief Erases files matching a pattern from the OASIS disk image.
 *
 * This function iterates through the directory, finds files matching the
 * specified pattern and owner ID (from `options`), and then calls
 * `oasis_erase_single_file` for each matched file. After processing all
 * matching files, if any modifications were made to the directory or
 * allocation map, it writes these changes back to the disk image file.
 *
 * @param img_stream Pointer to the `sector_io_stream_t` for the open OASIS disk image.
 * The stream must be opened in a mode that allows writing (e.g., "r+b").
 * @param disk_layout Pointer to the `oasis_disk_layout_t` structure, which includes the
 * directory and allocation map. This structure will be modified.
 * @param options Pointer to the `cli_options_t` structure, containing the `pattern`
 * for filename matching and `owner_id_filter` to select files for erasure.
 * @return true if the overall operation completed successfully (all matched files erased
 * and changes written to disk).
 * false if a fundamental setup error occurred (e.g., NULL arguments) or if
 * writing the updated metadata back to the disk image failed. Individual
 * file erase errors are reported but might not cause this function to return false
 * if the final disk write is successful.
 */
bool oasis_erase_files_by_pattern(sector_io_stream_t* img_stream,
                                  oasis_disk_layout_t* disk_layout,
                                  const cli_options_t* options);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OASIS_FILE_ERASE_H_ */
