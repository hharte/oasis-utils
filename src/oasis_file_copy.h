/*
 * oasis_file_copy.h - OASIS File Copying Utilities Interface
 *
 * This header file declares functions for copying files from the host
 * system into an OASIS disk image.
 * Part of the oasis-utils library.
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#ifndef OASIS_FILE_COPY_H_
#define OASIS_FILE_COPY_H_

#include "oasis.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Copies a host file to the OASIS disk image.
 *
 * This function performs the following steps:
 * 1. Reads the specified file from the host system.
 * 2. Optionally performs ASCII conversion from host to OASIS line endings if specified
 * in `options` and the file is 7-bit ASCII. A SUB character (0x1A) may be
 * appended if ASCII conversion is done for a sequential file.
 * 3. Determines the target OASIS filename, type, format, and attributes. This can be
 * explicitly provided via `oasis_filename_override` or derived from the
 * `host_filepath` according to the oasis-utils naming convention.
 * 4. Sets the owner ID for the new file based on `options->owner_id_filter`
 * (defaults to 0 if `OWNER_ID_WILDCARD` is used).
 * 5. Sets the file timestamp in the DEB based on the host file's modification time.
 * 6. Checks if a file with the same name and owner ID already exists on the OASIS disk.
 * If it does, the existing file is erased first.
 * 7. Verifies that there is sufficient free space on the OASIS disk image.
 * 8. Finds an empty or deleted Directory Entry Block (DEB) slot.
 * 9. Writes the new file data to the allocated space on the disk image.
 *10. Updates the DEB, allocation map, and filesystem block in memory.
 *11. Writes these updated metadata structures back to the disk image file.
 *
 * @param img_stream Pointer to the `sector_io_stream_t` for the open OASIS disk image.
 * The stream must be opened in a mode that allows writing (e.g., "r+b").
 * @param disk_layout Pointer to the `oasis_disk_layout_t` structure representing the
 * disk's current state. This structure will be modified in memory
 * (DEB, allocation map, fsblock) and these changes will be
 * written back to the disk image file.
 * @param host_filepath The path to the source file on the host system.
 * @param oasis_filename_override Optional. If not NULL, this string is parsed to determine
 * the OASIS filename, type, format, and attributes.
 * If NULL, these are derived from `host_filepath`.
 * @param options Pointer to the `cli_options_t` structure, providing flags like
 * `ascii_conversion` and `owner_id_filter` (used for the target owner ID).
 * @return true if the file was successfully copied to the OASIS disk image and all
 * metadata was updated and written back.
 * false if an error occurred (e.g., host file not found, disk full,
 * write error, directory full, invalid filename format).
 */
bool oasis_copy_host_file_to_disk(sector_io_stream_t* img_stream,
                                  oasis_disk_layout_t* disk_layout,
                                  const char* host_filepath,
                                  const char* oasis_filename_override,
                                  const cli_options_t* options);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OASIS_FILE_COPY_H_ */
