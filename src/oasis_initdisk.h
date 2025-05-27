/*
 * oasis_initdisk_lib.h - OASIS Disk Initialization Library Interface
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#ifndef OASIS_INITDISK_LIB_H_
#define OASIS_INITDISK_LIB_H_

#include "oasis.h"     /* For sector_io_stream_t, oasis_disk_layout_t, etc. */
#include "libimdf.h"   /* For IMD specific operations */

/* Default disk geometry parameters if not specified by options */
#define DEFAULT_NUM_HEADS_LIB 1
#define DEFAULT_TRACKS_PER_SURFACE_LIB 77
#define DEFAULT_SECTORS_PER_TRACK_LIB 13
#define DEFAULT_SECTOR_INCREMENT_LIB 1
#define DEFAULT_TRACK_SKEW_LIB 0
#define DEFAULT_DIR_SIZE_LIB 32

/* Structure to hold the options for disk initialization operations. */
/* This structure will be populated by the calling application (e.g., oasis_initdisk_main.c) */
/* and passed to the library functions. */
typedef struct {
    char image_path[1024];         /* Path to the disk image file. */
    char drive_letter;             /* Drive letter associated with the image (e.g., 'A'). */

    int build_op;                  /* Flag: 1 if BUILD operation is selected. */
    int clear_op;                  /* Flag: 1 if CLEAR operation is selected. */
    int format_op;                 /* Flag: 1 if FORMAT operation is selected. */
    int label_op;                  /* Flag: 1 if LABEL operation is selected. */
    int nowp_op;                   /* Flag: 1 if NOWP (remove write-protection) operation is selected. */
    int wp_op;                     /* Flag: 1 if WP (enable write-protection) operation is selected. */

    int num_heads;                 /* Number of disk heads/surfaces. */
    int sector_increment;          /* Logical sector increment (interleave factor). */
    int dir_size;                  /* Number of directory entries. */
    int track_skew;                /* Track skew factor. */
    int tracks_per_surface;        /* Tracks per disk surface. */
    int sectors_per_track;         /* Sectors per track. */
    char disk_label_str[FNAME_LEN + 1]; /* Disk label string. */

    /* Flags to indicate if a geometry/label parameter was explicitly specified by the user, */
    /* differentiating from default values. */
    int heads_specified;
    int incr_specified;
    int size_specified;
    int skew_specified;
    int tracks_specified;
    int sector_specififed; /* Note: Consistent with original typo "sector_specififed" */
    int label_specified;

} initdisk_options_lib_t;

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/*
 * Performs the main disk initialization logic based on the provided options.
 * This function will dispatch to specific handlers (format, build, clear, etc.)
 *
 * @param opts Pointer to an initdisk_options_lib_t structure containing all
 * parameters and operation flags for the initialization.
 * @return EXIT_SUCCESS on successful completion of the operation,
 * EXIT_FAILURE if any error occurs.
 */
int initdisk_perform_operation(initdisk_options_lib_t* opts);

/*
 * Initializes the core filesystem structures (Filesystem Block, Allocation Map, Directory)
 * on an already low-level formatted disk or a newly created image.
 * This is a lower-level function typically called by operations like FORMAT or BUILD.
 *
 * @param sio Pointer to an open sector_io_stream_t for the disk image.
 * The stream must be writable.
 * @param opts Pointer to an initdisk_options_lib_t structure containing disk geometry,
 * label, and directory size information.
 * @param disk_layout Pointer to an oasis_disk_layout_t structure that will be
 * populated with the new filesystem structures. This function
 * will allocate memory for disk_layout->alloc_map.map_data and
 * disk_layout->directory. The caller is responsible for calling
 * cleanup_oasis_disk() on this structure when done.
 * @return EXIT_SUCCESS on successful initialization of structures and writing them to disk.
 * EXIT_FAILURE if an error occurs (e.g., memory allocation, write error,
 * invalid parameters leading to impossible geometry).
 */
int initdisk_initialize_filesystem_structures(sector_io_stream_t* sio,
                                              initdisk_options_lib_t* opts,
                                              oasis_disk_layout_t* disk_layout);

/*
 * Handles the FORMAT operation. This includes low-level formatting (simulated for .img,
 * actual for .imd via libimdf) and then initializing filesystem structures.
 *
 * @param sio Pointer to an open sector_io_stream_t for the disk image.
 * The stream must be writable.
 * @param opts Pointer to an initdisk_options_lib_t structure with format parameters.
 * @param disk_layout Pointer to an oasis_disk_layout_t structure to be used and populated.
 * @return EXIT_SUCCESS on successful formatting and filesystem initialization.
 * EXIT_FAILURE if an error occurs.
 */
int initdisk_handle_format_operation(sector_io_stream_t* sio,
                                     initdisk_options_lib_t* opts,
                                     oasis_disk_layout_t* disk_layout);

/*
 * Handles the BUILD operation. This writes bootstrap, label, and directory
 * to an already formatted disk.
 *
 * @param sio Pointer to an open sector_io_stream_t for the disk image.
 * @param opts Pointer to an initdisk_options_lib_t structure.
 * @param disk_layout Pointer to an oasis_disk_layout_t structure to be used/populated.
 * @return EXIT_SUCCESS or EXIT_FAILURE.
 */
int initdisk_handle_build_operation(sector_io_stream_t* sio,
                                    initdisk_options_lib_t* opts,
                                    oasis_disk_layout_t* disk_layout);

/*
 * Handles the CLEAR operation. Erases all files and re-initializes the directory.
 *
 * @param sio Pointer to an open sector_io_stream_t for the disk image.
 * @param opts Pointer to an initdisk_options_lib_t structure.
 * @param disk_layout Pointer to an oasis_disk_layout_t structure to be used/populated.
 * @return EXIT_SUCCESS or EXIT_FAILURE.
 */
int initdisk_handle_clear_operation(sector_io_stream_t* sio,
                                    initdisk_options_lib_t* opts,
                                    oasis_disk_layout_t* disk_layout);

/*
 * Handles the LABEL operation. Sets or re-initializes the disk label.
 *
 * @param sio Pointer to an open sector_io_stream_t for the disk image.
 * @param opts Pointer to an initdisk_options_lib_t structure containing the new label.
 * @param disk_layout Pointer to an oasis_disk_layout_t structure (fsblock will be read/modified).
 * @return EXIT_SUCCESS or EXIT_FAILURE.
 */
int initdisk_handle_label_operation(sector_io_stream_t* sio,
                                    initdisk_options_lib_t* opts,
                                    oasis_disk_layout_t* disk_layout);

/*
 * Handles setting or clearing the software write-protection flag.
 *
 * @param sio Pointer to an open sector_io_stream_t for the disk image.
 * @param write_protect Integer flag: 1 to enable write protection, 0 to disable.
 * @return EXIT_SUCCESS or EXIT_FAILURE.
 */
int initdisk_handle_wp_operation(sector_io_stream_t* sio, int write_protect);


/*
 * Creates an empty IMD (ImageDisk) file with a valid header.
 * This is used by the FORMAT operation if the target IMD file does not exist.
 *
 * @param image_path Path where the new IMD file should be created.
 * @return 1 on success, 0 on failure.
 */
int initdisk_create_empty_imd_file(const char* image_path);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* OASIS_INITDISK_LIB_H_ */
