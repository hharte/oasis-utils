/*
 * oasis_file_read.h - OASIS File Reading Utilities Interface
 *
 * This header file declares functions for reading data from files stored
 * within an OASIS disk image, supporting both sequential and contiguous file types.
 * Part of the oasis-utils library.
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#ifndef OASIS_FILE_READ_H_
#define OASIS_FILE_READ_H_

#include "oasis.h"

#ifdef __cplusplus
extern "C" {
#endif

    /**
     * @brief Reads the raw data content of an OASIS file from the disk image.
     *
     * This function handles both sequential and contiguous file types based on the
     * provided Directory Entry Block (DEB). It allocates a buffer to store the
     * file content. The caller is responsible for freeing this buffer using `free()`.
     *
     * For sequential files, it uses `oasis_read_sequential_file` internally.
     * For contiguous files (Direct, Absolute, Relocatable, Indexed, Keyed), it reads
     * the allocated blocks directly. The logical size of contiguous files is determined
     * based on DEB fields (e.g., `record_count * record_length` for Direct,
     * `file_format_dependent2` for Relocatable program length). If the calculated
     * logical size exceeds the physically allocated size (`block_count * BLOCK_SIZE`),
     * the read data is truncated to the allocated size. If logical size is 0 but
     * blocks are allocated, the full allocated size is read.
     *
     * @param img_stream Pointer to the `sector_io_stream_t` for the open OASIS disk image.
     * @param deb Pointer to the `directory_entry_block_t` for the file to be read.
     * @param file_buffer_ptr Pointer to a `uint8_t*`. On successful read of a non-empty file,
     * this will be updated to point to a newly allocated buffer
     * containing the file data. If the file is empty or an error
     * occurs, it will be set to NULL.
     * @param bytes_read_ptr Pointer to an `ssize_t` where the number of bytes read into
     * `*file_buffer_ptr` will be stored. This will be 0 for an empty file.
     * Set to -1 on critical failure.
     * @return true on success (even if the file is empty and 0 bytes are read).
     * false on critical failure (e.g., memory allocation error, or if an
     * underlying read operation like `oasis_read_sequential_file` fails
     * and returns -1).
     */
    bool oasis_file_read_data(sector_io_stream_t* img_stream,
        const directory_entry_block_t* deb,
        uint8_t** file_buffer_ptr,
        ssize_t* bytes_read_ptr);

    /**
     * @brief Reads data from an OASIS sequential file into a caller-provided buffer.
     *
     * This function follows the sector chain of a sequential file, starting from
     * the `start_sector` specified in the DEB. It reads the data portion (first
     * `OASIS_SEQ_DATA_PER_SECTOR` bytes) of each linked sector and copies it into
     * the `caller_buffer`. The process continues until the end of the file chain
     * (link points to 0) is reached or the `caller_buffer` is full.
     *
     * Validation checks performed:
     * - Ensures the DEB is for a sequential file.
     * - Verifies that the number of sectors processed does not exceed the number
     * of sectors implied by the DEB's `block_count`.
     * - If the entire file chain is read (i.e., the buffer was large enough),
     * it verifies that the last sector found matches the `file_format_dependent2`
     * field (last sector address) stored in the DEB.
     *
     * @param deb Pointer to the `directory_entry_block_t` for the sequential file.
     * Must be non-NULL, and its `file_format` must indicate a sequential file.
     * @param img_stream Pointer to the `sector_io_stream_t` for the open OASIS disk image.
     * Must be valid and opened for reading.
     * @param caller_buffer Pointer to the buffer where the file data will be stored.
     * Must be non-NULL if `buffer_size > 0`.
     * @param buffer_size The maximum capacity of `caller_buffer` in bytes.
     *
     * @return On success, returns the total number of bytes read into `caller_buffer`.
     * This can be less than the actual file size if `buffer_size` was too small.
     * Returns 0 for an empty file (if `deb->start_sector` is 0).
     * Returns -1 on error. Specific errors are printed to stderr, and `errno`
     * may be set (e.g., EIO for read/seek errors or filesystem inconsistencies,
     * EINVAL for invalid arguments, EBADF if `img_stream` is invalid).
     */
    ssize_t oasis_read_sequential_file(const directory_entry_block_t* deb,
        sector_io_stream_t* img_stream,
        void* caller_buffer,
        size_t buffer_size);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OASIS_READ_FILE_H_ */
