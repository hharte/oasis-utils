/*
 * oasis_file_write.h - OASIS File Writing Utilities Interface
 *
 * This header file declares functions for writing data to files
 * within an OASIS disk image, supporting both sequential and contiguous
 * file types.
 * Part of the oasis-utils library.
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#ifndef OASIS_WRITE_FILE_H_
#define OASIS_WRITE_FILE_H_

/* Include oasis.h for core definitions */
#include "oasis.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Writes data to an OASIS disk image, creating or overwriting a file.
 *
 * This function handles the process of writing data to an OASIS disk:
 * 1. Allocates necessary 1K blocks from the `disk_layout->alloc_map`.
 * - For contiguous files, a single contiguous chunk is allocated.
 * - For sequential files, blocks are allocated one by one as needed.
 * 2. Writes the provided `data_buffer` to the allocated sectors on the disk image
 * via `img_stream`.
 * - For contiguous files, data is written directly. If `data_size` is not a
 * multiple of `BLOCK_SIZE`, the last block is zero-padded.
 * - For sequential files, data is written sector by sector (`OASIS_SEQ_DATA_PER_SECTOR`
 * bytes per sector), with the last two bytes of each sector forming a link to
 * the next sector. The final sector's link is 0.
 * 3. Updates the fields of the provided Directory Entry Block (`deb`) in memory
 * with the file's allocation details (e.g., `start_sector`, `block_count`,
 * `record_count`, `file_format_dependent2` for sequential files).
 * 4. Updates the `disk_layout->fsblock.free_blocks` count in memory.
 *
 * **Important:**
 * - The caller is responsible for pre-filling other DEB fields (filename, type,
 * format, owner ID, timestamp, and `file_format_dependent1` if applicable,
 * like record length for Direct files).
 * - This function modifies `disk_layout` (alloc_map, fsblock) and the `deb`
 * structure *in memory*. The caller is responsible for subsequently writing
 * the updated directory (containing the modified `deb`) and the system blocks
 * (fsblock, alloc_map) back to the disk image file to persist changes.
 * - If an error occurs during allocation or writing, allocated blocks are rolled back
 * (deallocated from the in-memory map).
 *
 * @param img_stream Pointer to the `sector_io_stream_t` for the open OASIS disk image.
 * Must be opened in a mode that allows writing (e.g., "r+b").
 * @param disk_layout Pointer to the `oasis_disk_layout_t` structure. Its `alloc_map`
 * and `fsblock.free_blocks` will be modified in memory.
 * @param deb Pointer to a `directory_entry_block_t` structure. This DEB's fields
 * (`start_sector`, `block_count`, etc.) will be updated in memory.
 * @param data_buffer Pointer to the buffer containing the data to be written.
 * Can be NULL if `data_size` is 0.
 * @param data_size The size of the data in `data_buffer` in bytes. If 0, an
 * empty file DEB entry is configured (0 blocks, 0 start sector).
 * @return true on success (data written to disk, DEB and disk_layout updated in memory).
 * false on failure (e.g., invalid arguments, no space on disk, write error,
 * file size exceeds DEB capacity). Error messages are printed to stderr.
 */
bool oasis_file_write_data(sector_io_stream_t* img_stream,
                           oasis_disk_layout_t* disk_layout, /* Allocation map and fsblock will be modified */
                           directory_entry_block_t* deb,     /* DEB will be updated with allocation info */
                           const uint8_t* data_buffer,
                           size_t data_size);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OASIS_WRITE_FILE_H_ */
