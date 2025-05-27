/*
 * oasis_utils.h - General OASIS Disk Image Utilities Interface
 *
 * This header file declares utility functions for loading OASIS disk image
 * metadata, displaying disk information, listing directory contents, and
 * other general helper functions related to the oasis-utils library.
 * It also includes compatibility definitions for different compilers/platforms.
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#ifndef OASIS_UTILS_H_
#define OASIS_UTILS_H_

#include <stdio.h>

#include "oasis.h"


#ifdef __cplusplus
extern "C" {
#endif

    /* Define platform-specific includes based on CMake checks */
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif

/* --- MSVC Compatibility --- */
#ifdef _MSC_VER
/*
 * Define ssize_t for MSVC if not already defined.
 * It's typically provided by BaseTsd.h. This guard ensures it's
 * available if this header is included before others that might define it.
 */
#ifndef _SSIZE_T_DEFINED
# include <BaseTsd.h>      /* Should provide SSIZE_T */
    typedef SSIZE_T ssize_t;
#define _SSIZE_T_DEFINED
#endif /* _SSIZE_T_DEFINED */

/* Define off_t for MSVC if not already defined. */
#ifndef _OFF_T_DEFINED
    typedef long off_t;
#define _OFF_T_DEFINED
#endif /* _OFF_T_DEFINED */
#endif /* _MSC_VER */
    /* --- End MSVC Compatibility --- */

    /**
     * @brief Platform-specific path separator character.
     * Defined as '\\' for Windows (_WIN32) and '/' for other platforms.
     */
    extern const char kPathSeparator;

    /* --- Function Prototypes --- */

    /**
     * @brief Loads essential metadata from an OASIS disk image file.
     *
     * Reads the boot sector, filesystem header block, allocation map, and
     * directory entries from the disk image specified by `img_stream` and
     * populates the `disk_layout` structure. Memory is allocated for the
     * allocation map and directory data within `disk_layout`.
     *
     * @param img_stream Pointer to an initialized `sector_io_stream_t` representing
     * the open disk image file.
     * @param disk_layout Pointer to an `oasis_disk_layout_t` structure to be populated
     * with the loaded metadata.
     * @return true on successful loading of all essential metadata.
     * false if an error occurs (e.g., read error, memory allocation failure,
     * inconsistent metadata like an overly large allocation map).
     * Error messages are printed to stderr.
     */
    bool load_oasis_disk(sector_io_stream_t* img_stream, oasis_disk_layout_t* disk_layout);

    /**
     * @brief Frees memory allocated within an `oasis_disk_layout_t` structure.
     *
     * Specifically, this function frees the memory allocated for
     * `disk_layout->alloc_map.map_data` and `disk_layout->directory`.
     * It also nullifies these pointers and zeros related size fields.
     *
     * @param disk_layout Pointer to the `oasis_disk_layout_t` structure whose
     * internally allocated members are to be freed.
     * If NULL, the function does nothing.
     */
    void cleanup_oasis_disk(oasis_disk_layout_t* disk_layout);

    /**
      * @brief Calculates the total number of 256-byte sectors on the disk
      * based on its geometry stored in the filesystem block.
      *
      * The calculation uses heads, cylinders, and sectors-per-track values.
      *
      * @param fs_block A pointer to the `filesystem_block_t` structure containing
      * the disk geometry information.
      * @return The total number of 256-byte sectors on the disk.
      * Returns 0 if `fs_block` is NULL or if the geometry parameters
      * (heads, cylinders, sectors/track) are invalid (e.g., zero).
      */
    size_t get_total_sectors(const filesystem_block_t* fs_block);

    /**
      * @brief Calculates the total number of allocatable 1K blocks on the disk
      * based on its geometry.
      *
      * This is derived from the total number of sectors, where each 1K block
      * consists of (`BLOCK_SIZE` / `SECTOR_SIZE`) sectors (typically 4).
      *
      * @param fs_block A pointer to the `filesystem_block_t` structure containing
      * disk geometry information.
      * @return The total number of 1K blocks on the disk.
      * Returns 0 if `fs_block` is NULL or if the geometry leads to zero total sectors.
      */
    size_t get_total_blocks(const filesystem_block_t* fs_block);

    /**
      * @brief Displays detailed information about the loaded OASIS disk image.
      *
      * Prints filesystem header details (label, timestamps, geometry, free space, etc.)
      * and a summary of the allocation map (size, free blocks, largest contiguous chunk).
      *
      * @param disk_layout Pointer to a populated `oasis_disk_layout_t` structure.
      * If NULL, the function does nothing.
      */
    void display_disk_info(const oasis_disk_layout_t* disk_layout);

    /**
      * @brief Lists a single OASIS Directory Entry Block (DEB) in a formatted manner.
      *
      * Prints the host-compatible filename, format, record count, block count,
      * start sector, timestamp, and owner/shared ID for the given DEB.
      * Used as a helper for `list_files`.
      *
      * @param deb Pointer to the `directory_entry_block_t` to display.
      * If the DEB is not valid (per `oasis_deb_is_valid`), nothing is printed.
      */
    void list_single_deb(const directory_entry_block_t* deb);

    /**
      * @brief Lists the files stored in the OASIS disk image directory.
      *
      * Iterates through the directory entries in `disk_layout`. For each valid DEB,
      * it applies filtering based on `owner_id_filter` and, if provided, the
      * wildcard `pattern`. Matching files are then displayed using `list_single_deb`.
      * Finally, it prints a summary count of the files listed.
      *
      * @param disk_layout Pointer to a populated `oasis_disk_layout_t` structure.
      * @param owner_id_filter The owner ID to filter files by. Use `OWNER_ID_WILDCARD`
      * (defined in oasis_extract.h or similar) to list files for any owner.
      * @param pattern Optional wildcard pattern (e.g., "*.TXT", "FILE?") to filter files
      * by name. If NULL or an empty string, no pattern filtering is applied.
      */
    void list_files(const oasis_disk_layout_t* disk_layout, int owner_id_filter, const char* pattern);

    /**
      * @brief Prints a buffer's content in a standard hexadecimal and ASCII dump format.
      *
      * Displays 16 bytes per line, showing hex values followed by their printable
      * ASCII representation (using '.' for non-printable characters).
      *
      * @param data Pointer to the buffer containing the data to dump.
      * @param len Length of the data in the buffer, in bytes.
      * If `len` is 0 or `data` is NULL, a "No data to dump" message is printed.
      */
    void dump_hex(const uint8_t* data, size_t len);


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OASIS_UTILS_H_ */
