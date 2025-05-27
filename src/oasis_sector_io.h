/*
 * oasis_sector_io.h - Sector-Based I/O Abstraction Layer Interface
 *
 * This header file declares structures and functions for performing sector-based
 * I/O operations on disk image files. It provides an abstraction layer that
 * supports raw disk images (.img) and ImageDisk (.IMD) files, specifically
 * handling OASIS's 256-byte sector view, including logical pairing of
 * 128-byte IMD sectors.
 * Part of the oasis-utils library.
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#ifndef OASIS_SECTOR_IO_H_
#define OASIS_SECTOR_IO_H_

#include <stdio.h>   /* For FILE */

/* Include libimdf.h for ImdImageFile definition */
#include "libimdf.h"

/* Define extern "C" guards for C++ compatibility */
#ifdef __cplusplus
extern "C" {
#endif

/*
 * Define ssize_t for MSVC if not already defined elsewhere (e.g., by mm_serial.h
 * which might be included if this header is part of a larger project context).
 * This ensures portability for functions returning ssize_t.
 */
#ifdef _MSC_VER
#ifndef _SSIZE_T_DEFINED
# include <BaseTsd.h>      /* Contains SSIZE_T definition */
    typedef SSIZE_T ssize_t;
#define _SSIZE_T_DEFINED
#endif /* _SSIZE_T_DEFINED */
#else
    /* For POSIX systems, ssize_t is typically in unistd.h or sys/types.h */
#include <unistd.h> /* Often provides ssize_t */
#endif /* _MSC_VER */

/**
 * @brief Structure to hold the state of an open sector I/O stream.
 *
 * This structure maintains context for an opened disk image, whether it's
 * a raw file or an IMD file.
 */
    typedef struct sector_io_stream_s {
        FILE* file_ptr;             /**< File pointer, used for RAW image access. NULL for IMD. */
        ImdImageFile* imdf_handle;  /**< Handle for IMD images, managed by libimdf. NULL for RAW. */
        char image_type[16];        /**< String indicating image type, e.g., "RAW" or "IMD". */
        uint32_t total_sectors;     /**< Total number of 256-byte equivalent logical sectors
                                         addressable in the disk image. Calculated on open. */
    } sector_io_stream_t;

    /**
     * @brief Opens a disk image file for sector-based I/O.
     *
     * Detects the image type (RAW or IMD) based on the file extension.
     * For IMD files, it validates that sectors are either 128 or 256 bytes,
     * that any 128-byte sectors precede 256-byte sectors on a given track,
     * and that tracks containing 128-byte sectors have an even count of them
     * (to form logical 256-byte OASIS sectors).
     * Calculates `total_sectors` based on a 256-byte sector view.
     *
     * @param image_path Path to the disk image file (e.g., "mydisk.img", "another.imd").
     * @param mode File open mode string (e.g., "rb" for read-binary, "r+b" for read/write-binary).
     * For IMD files, write capability depends on libimdf and IMD file properties.
     * @return A pointer to an allocated `sector_io_stream_t` structure on success.
     * NULL on failure (e.g., file not found, memory allocation error,
     * invalid IMD format for OASIS use). Error messages are printed to stderr.
     */
    sector_io_stream_t* sector_io_open(const char* image_path, const char* mode);

    /**
     * @brief Closes a disk image file previously opened with `sector_io_open`.
     *
     * Frees resources associated with the stream, including closing file pointers
     * or IMD handles.
     *
     * @param stream Pointer to the `sector_io_stream_t` structure to close.
     * If NULL, the function does nothing.
     * @return 0 on success.
     * EOF if `fclose` fails for a RAW image file. Other errors are typically
     * handled internally by libimdf or are non-critical.
     */
    int sector_io_close(sector_io_stream_t* stream);

    /**
     * @brief Reads one or more 256-byte logical sectors from the disk image.
     *
     * For IMD files, this function maps the 256-byte logical OASIS sector LBA
     * to either one 256-byte IMD sector or a pair of consecutive 128-byte
     * IMD sectors (ordered by their logical sector IDs on the track).
     * If an IMD sector is marked as bad/unavailable, the corresponding 256-byte
     * portion of the buffer is zero-filled.
     * For RAW files, it reads directly from the file offset.
     *
     * @param stream Pointer to an initialized `sector_io_stream_t` structure.
     * @param sector_lba_oasis The starting 256-byte logical sector number (LBA) to read from.
     * @param num_sectors_oasis The number of 256-byte logical sectors to read.
     * @param buffer Pointer to the buffer where the read data will be stored.
     * The buffer must be large enough to hold
     * (`num_sectors_oasis` * `SECTOR_SIZE` (256)) bytes.
     * @return The number of 256-byte logical sectors successfully read. This may be
     * less than `num_sectors_oasis` if the end of the image is reached or an
     * error occurs during a multi-sector read.
     * Returns -1 on critical error (e.g., invalid arguments, stream error).
     * Returns 0 if `num_sectors_oasis` is 0 or if `sector_lba_oasis` is at or
     * beyond the end of the image.
     */
    ssize_t sector_io_read(sector_io_stream_t* stream, uint32_t sector_lba_oasis, uint32_t num_sectors_oasis, uint8_t* buffer);

    /**
     * @brief Writes one or more 256-byte logical sectors to the disk image.
     *
     * For IMD files, maps the 256-byte logical OASIS sector LBA to either one
     * 256-byte IMD sector or a pair of consecutive 128-byte IMD sectors.
     * Writing to IMD sectors marked bad may depend on libimdf's behavior.
     * For RAW files, it writes directly to the file offset. The file may be extended.
     *
     * @param stream Pointer to an initialized `sector_io_stream_t` structure.
     * The stream must have been opened in a mode that permits writing.
     * @param sector_lba_oasis The starting 256-byte logical sector number (LBA) to write to.
     * @param num_sectors_oasis The number of 256-byte logical sectors to write.
     * @param buffer Pointer to the buffer containing the data to write.
     * Must contain (`num_sectors_oasis` * `SECTOR_SIZE` (256)) bytes.
     * @return The number of 256-byte logical sectors successfully written. This may be
     * less than `num_sectors_oasis` if an error occurs (e.g., disk full).
     * Returns -1 on critical error (e.g., invalid arguments, stream not writable,
     * IMD write-protected).
     * Returns 0 if `num_sectors_oasis` is 0.
     */
    ssize_t sector_io_write(sector_io_stream_t* stream, uint32_t sector_lba_oasis, uint32_t num_sectors_oasis, const uint8_t* buffer);

    /**
     * @brief Gets the total number of 256-byte logical sectors in the opened disk image.
     *
     * For IMD files, this value is calculated during `sector_io_open` based on the
     * validated track and sector layout, representing the 256-byte equivalent capacity.
     * For RAW files, it's calculated from the file size.
     *
     * @param stream Pointer to an initialized `sector_io_stream_t` structure.
     * @return The total number of 256-byte logical sectors available in the image.
     * Returns 0 if `stream` is NULL or if the image is effectively empty.
     */
    uint32_t sector_io_get_total_sectors(sector_io_stream_t* stream);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OASIS_SECTOR_IO_H_ */
