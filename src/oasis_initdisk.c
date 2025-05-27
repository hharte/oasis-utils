/*
 * oasis_initdisk.c - OASIS Disk Initialization Library Implementation
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <limits.h>

/* Include OASIS library headers */
#include "oasis.h"
#include "libimdf.h" /* For IMD specific operations */

/* Forward declarations for static helper functions */
static int file_exists_lib(const char* filepath);

/*
 * Checks if a file exists.
 *
 * @param filepath Path to the file.
 * @return 1 if file exists, 0 otherwise.
 */
static int file_exists_lib(const char* filepath) {
    FILE* f = fopen(filepath, "r");
    if (f) {
        fclose(f);
        return 1;
    }
    return 0;
}

#ifdef _MSC_VER
static int strcasecmp(const char *s1, const char *s2) {
    return _stricmp(s1, s2);
}
#endif

/*
 * Creates an empty IMD (ImageDisk) file with a valid header.
 * This is used by the FORMAT operation if the target IMD file does not exist.
 *
 * @param image_path Path where the new IMD file should be created.
 * @return 1 on success, 0 on failure.
 */
int initdisk_create_empty_imd_file(const char* image_path) {
    FILE* fout = fopen(image_path, "wb");
    if (!fout) {
        fprintf(stderr, "initdisk_create_empty_imd_file: Error creating IMD file '%s': %s\n", image_path, strerror(errno));
        return 0;
    }
    if (imd_write_file_header(fout, "1.18") != 0) { /* Default IMD version */
        fprintf(stderr, "initdisk_create_empty_imd_file: Error writing IMD header to '%s'.\n", image_path);
        fclose(fout);
        remove(image_path); /* Attempt to clean up */
        return 0;
    }
    if (imd_write_comment_block(fout, NULL, 0) != 0) { /* Write empty comment block with terminator */
        fprintf(stderr, "initdisk_create_empty_imd_file: Error writing IMD comment terminator to '%s'.\n", image_path);
        fclose(fout);
        remove(image_path); /* Attempt to clean up */
        return 0;
    }
    if (fclose(fout) != 0) {
        fprintf(stderr, "initdisk_create_empty_imd_file: Error closing newly created IMD file '%s': %s\n", image_path, strerror(errno));
        /* File might still be partially created. */
        return 0;
    }
    printf("Info: Created new empty IMD file '%s'.\n", image_path);
    return 1;
}

/*
 * Initializes the core filesystem structures.
 */
int initdisk_initialize_filesystem_structures(sector_io_stream_t* sio,
                                              initdisk_options_lib_t* opts,
                                              oasis_disk_layout_t* disk_layout) {
    uint8_t sector_buffer[SECTOR_SIZE];
    time_t now;
    struct tm *local_time;
    size_t am_bytes_in_sector1;
    uint32_t additional_am_sectors;
    uint32_t dir_start_sector_lba;
    uint32_t i;
    uint32_t total_1k_blocks_on_disk;
    uint32_t num_dir_sectors;
    size_t actual_map_bytes_needed;
    uint32_t allocated_blocks_count = 0;

    /* Zero out the filesystem block in the layout */
    memset(&disk_layout->fsblock, 0, sizeof(filesystem_block_t));

    /* Set disk label */
    memset(disk_layout->fsblock.label, ' ', FNAME_LEN);
    strncpy(disk_layout->fsblock.label, opts->disk_label_str, strlen(opts->disk_label_str) > FNAME_LEN ? FNAME_LEN : strlen(opts->disk_label_str));

    /* Set timestamp */
    time(&now);
    local_time = localtime(&now);
    if (local_time != NULL) {
        oasis_convert_tm_to_timestamp(local_time, &disk_layout->fsblock.timestamp);
    }

    /* Set geometry from options */
    disk_layout->fsblock.num_heads = (uint8_t)(opts->num_heads << 4); /* Store heads in high nibble, opts->num_heads is int but clamped */
    disk_layout->fsblock.num_cyl = (uint8_t)opts->tracks_per_surface; /* opts->tracks_per_surface is int but clamped */
    disk_layout->fsblock.num_sectors = (uint8_t)opts->sectors_per_track; /* opts->sectors_per_track is int but clamped */

    /* Calculate and set directory sectors max */
    num_dir_sectors = (opts->dir_size + DIR_ENTRIES_PER_SECTOR - 1) / DIR_ENTRIES_PER_SECTOR;
    if (num_dir_sectors > 255) {
        fprintf(stderr, "Warning: Requested directory size results in > 255 directory sectors. Clamping to 255.\n");
        num_dir_sectors = 255;
    }
    disk_layout->fsblock.dir_sectors_max = (uint8_t)num_dir_sectors; /* num_dir_sectors is uint32_t, clamped to 255, safe for uint8_t */


    /* Calculate total 1K blocks on disk */
    total_1k_blocks_on_disk = (uint32_t)get_total_blocks(&disk_layout->fsblock);
    if (total_1k_blocks_on_disk == 0 && (opts->format_op || opts->build_op)) {
        fprintf(stderr, "Error: Invalid disk geometry results in 0 total blocks.\n");
        cleanup_oasis_disk(disk_layout); /* Ensure cleanup on failure path */
        return EXIT_FAILURE;
    }
    if (total_1k_blocks_on_disk > OASIS_MAX_FS_BLOCKS) {
        fprintf(stderr, "Error: Disk geometry results in %u 1K blocks, exceeding system maximum of %u.\n",
                total_1k_blocks_on_disk, OASIS_MAX_FS_BLOCKS);
        cleanup_oasis_disk(disk_layout);
        return EXIT_FAILURE;
    }

    /* Calculate allocation map size and additional sectors */
    actual_map_bytes_needed = (total_1k_blocks_on_disk + 7) / 8;
    am_bytes_in_sector1 = SECTOR_SIZE - sizeof(filesystem_block_t);

    if (actual_map_bytes_needed <= am_bytes_in_sector1) {
        additional_am_sectors = 0;
    } else {
        /* Defensively ensure intermediate calculation for additional_am_sectors doesn't overflow size_t before converting to uint32_t */
        size_t bytes_needing_more_sectors = actual_map_bytes_needed - am_bytes_in_sector1;
        additional_am_sectors = (uint32_t)((bytes_needing_more_sectors + SECTOR_SIZE - 1) / SECTOR_SIZE);
    }


    if (additional_am_sectors > 7) {
        fprintf(stderr, "Error: Disk size requires %u additional AM sectors, but system max is 7. Disk too large for this AM scheme.\n", additional_am_sectors);
        cleanup_oasis_disk(disk_layout);
        return EXIT_FAILURE;
    }
    disk_layout->fsblock.fs_flags = (uint8_t)additional_am_sectors; /* LSBs are additional AM sectors */
    if (opts->wp_op && !opts->nowp_op) { /* Apply WP flag if requested and not overridden by NOWP */
        disk_layout->fsblock.fs_flags |= FS_FLAGS_WP;
    }

    /* Allocate and initialize allocation map */
    disk_layout->alloc_map.map_size_bytes = actual_map_bytes_needed;
    if (disk_layout->alloc_map.map_data) { /* Free previous map if any (e.g., from CLEAR operation) */
        free(disk_layout->alloc_map.map_data);
        disk_layout->alloc_map.map_data = NULL;
    }
    if (disk_layout->alloc_map.map_size_bytes == 0 && total_1k_blocks_on_disk > 0) {
        fprintf(stderr, "Error: Internal error - alloc_map.map_size_bytes is 0 for a non-empty disk.\n");
        cleanup_oasis_disk(disk_layout);
        return EXIT_FAILURE;
    } else if (disk_layout->alloc_map.map_size_bytes == 0 && total_1k_blocks_on_disk == 0) {
        /* Valid for an empty disk, map_data remains NULL */
        disk_layout->alloc_map.map_data = NULL;
    } else {
        disk_layout->alloc_map.map_data = (uint8_t*)calloc(disk_layout->alloc_map.map_size_bytes, 1);
        if (!disk_layout->alloc_map.map_data) {
            perror("Error allocating memory for allocation map");
            cleanup_oasis_disk(disk_layout);
            return EXIT_FAILURE;
        }
    }

    /* Mark system-used blocks in allocation map */
    if (total_1k_blocks_on_disk > 0 && disk_layout->alloc_map.map_data) {
        /* Block 0 (boot sector, fs block, part of AM) */
        set_block_state(&disk_layout->alloc_map, 0, 1);
    }
    /* Blocks for additional AM sectors (if any) */
    for(i = 0; i < additional_am_sectors; ++i) {
        /* Additional AM sectors start at LBA 2. LBA / (sectors per 1K block) gives 1K block index. */
        uint32_t lba = 2 + i;
        uint32_t k_block = lba / SECTORS_PER_BLOCK; /* Assuming SECTORS_PER_BLOCK is 4 */
        if (k_block < total_1k_blocks_on_disk && disk_layout->alloc_map.map_data) {
            set_block_state(&disk_layout->alloc_map, k_block, 1);
        }
    }
    /* Blocks for directory sectors */
    dir_start_sector_lba = 2 + additional_am_sectors; /* LBA of first directory sector */
    for(i = 0; i < num_dir_sectors; ++i) {
        uint32_t lba = dir_start_sector_lba + i;
        uint32_t k_block = lba / SECTORS_PER_BLOCK;
        if (k_block < total_1k_blocks_on_disk && disk_layout->alloc_map.map_data) {
            set_block_state(&disk_layout->alloc_map, k_block, 1);
        }
    }

    /* Calculate and set free blocks */
    allocated_blocks_count = 0;
    if (disk_layout->alloc_map.map_data) {
        for (i = 0; i < total_1k_blocks_on_disk; ++i) {
            int state;
            if (get_block_state(&disk_layout->alloc_map, i, &state) == 0 && state == 1) {
                allocated_blocks_count++;
            }
        }
    }
    if (total_1k_blocks_on_disk >= allocated_blocks_count) {
        disk_layout->fsblock.free_blocks = (uint16_t)(total_1k_blocks_on_disk - allocated_blocks_count);
    } else {
        fprintf(stderr, "Error: Calculated allocated blocks (%u) exceeds total disk blocks (%u).\n", allocated_blocks_count, total_1k_blocks_on_disk);
        disk_layout->fsblock.free_blocks = 0; /* Set to 0 on error. */
        cleanup_oasis_disk(disk_layout);
        return EXIT_FAILURE;
    }

    /* Allocate and initialize directory */
    size_t dir_data_bytes = (size_t)num_dir_sectors * SECTOR_SIZE;
    if (disk_layout->directory) { /* Free previous directory if any */
        free(disk_layout->directory);
        disk_layout->directory = NULL;
    }
    /* Malloc for the struct and the flexible array member */
    disk_layout->directory = (oasis_directory_t*)malloc(sizeof(oasis_directory_t) + dir_data_bytes);
    if (!disk_layout->directory) {
        perror("Error allocating memory for directory");
        cleanup_oasis_disk(disk_layout);
        return EXIT_FAILURE;
    }
    disk_layout->directory->directory_size_bytes = dir_data_bytes;
    /* Initialize all DEBs to FILE_FORMAT_EMPTY */
    for (i = 0; i < num_dir_sectors * DIR_ENTRIES_PER_SECTOR; ++i) {
        memset(&disk_layout->directory->directory[i], 0, sizeof(directory_entry_block_t));
        disk_layout->directory->directory[i].file_format = FILE_FORMAT_EMPTY;
    }

    /* Write Boot Sector (Sector 0 - typically all zeros for data disks) */
    memset(sector_buffer, 0, SECTOR_SIZE);
    if (sector_io_write(sio, 0, 1, sector_buffer) != 1) {
        fprintf(stderr, "Error writing boot sector.\n"); cleanup_oasis_disk(disk_layout); return EXIT_FAILURE;
    }

    /* Write Filesystem Block and initial part of Allocation Map (Sector 1) */
    memset(sector_buffer, 0, SECTOR_SIZE); /* Clear buffer */
    memcpy(sector_buffer, &disk_layout->fsblock, sizeof(filesystem_block_t));
    if (disk_layout->alloc_map.map_data) {
        size_t bytes_of_am_in_s1 = am_bytes_in_sector1;
        if (disk_layout->alloc_map.map_size_bytes < am_bytes_in_sector1) {
            bytes_of_am_in_s1 = disk_layout->alloc_map.map_size_bytes;
        }
        memcpy(sector_buffer + sizeof(filesystem_block_t), disk_layout->alloc_map.map_data, bytes_of_am_in_s1);
    }
    if (sector_io_write(sio, 1, 1, sector_buffer) != 1) {
        fprintf(stderr, "Error writing filesystem block (sector 1).\n"); cleanup_oasis_disk(disk_layout); return EXIT_FAILURE;
    }

    /* Write additional Allocation Map sectors (if any) */
    if (additional_am_sectors > 0 && disk_layout->alloc_map.map_data) {
        size_t am_written_in_s1 = am_bytes_in_sector1;
        if (disk_layout->alloc_map.map_size_bytes < am_bytes_in_sector1) {
            am_written_in_s1 = disk_layout->alloc_map.map_size_bytes; /* Correctly reflect actual bytes in S1 */
        }
        /* Only write additional sectors if there's more map data beyond what fit in sector 1 */
        if (disk_layout->alloc_map.map_size_bytes > am_written_in_s1) {
            if (sector_io_write(sio, 2, additional_am_sectors, disk_layout->alloc_map.map_data + am_written_in_s1) != (ssize_t)additional_am_sectors) {
                fprintf(stderr, "Error writing additional allocation map sectors.\n"); cleanup_oasis_disk(disk_layout); return EXIT_FAILURE;
            }
        }
    }

    /* Write Directory Sectors */
    /* dir_start_sector_lba was already calculated: 2 (boot+fsb/am1) + additional_am_sectors */
    if (num_dir_sectors > 0 && disk_layout->directory) {
        if (sector_io_write(sio, dir_start_sector_lba, num_dir_sectors, (uint8_t*)disk_layout->directory->directory) != (ssize_t)num_dir_sectors) {
            fprintf(stderr, "Error writing directory sectors.\n"); cleanup_oasis_disk(disk_layout); return EXIT_FAILURE;
        }
    }
    printf("Disk initialized successfully.\n");
    return EXIT_SUCCESS;
}

/*
 * Handles the FORMAT operation.
 */
int initdisk_handle_format_operation(sector_io_stream_t* sio,
                                     initdisk_options_lib_t* opts,
                                     oasis_disk_layout_t* disk_layout) {
    uint32_t total_disk_sectors;
    uint32_t track_lba_iter;
    uint8_t  pattern_byte = 0xE5; /* Standard format pattern */
    uint8_t sector_write_buf[SECTOR_SIZE];
    uint32_t cyl_idx, head_idx;
    uint8_t first_sect_id_for_format;

    printf("\nFormatting disk with specified geometry...\n");
    printf("  Heads: %d, Tracks/Surface: %d, Sectors/Track: %d\n", opts->num_heads, opts->tracks_per_surface, opts->sectors_per_track);
    printf("  Sector Increment: %d, Track Skew: %d\n", opts->sector_increment, opts->track_skew);
    printf("  Directory Entries: %d, Label: '%s'\n", opts->dir_size, opts->disk_label_str);

    memset(sector_write_buf, pattern_byte, SECTOR_SIZE);

    if (strcmp(sio->image_type, "IMD") == 0) {
        if (!sio->imdf_handle) {
            fprintf(stderr, "Error: IMD handle is null for IMD image type during FORMAT.\n");
            return EXIT_FAILURE;
        }
        if (opts->sectors_per_track == 0) {
             fprintf(stderr, "Error: Sectors per track cannot be 0 for IMD formatting.\n");
             return EXIT_FAILURE;
        }
        first_sect_id_for_format = 1;

        for (cyl_idx = 0; (int)cyl_idx < opts->tracks_per_surface; ++cyl_idx) {
            int current_track_skew = (opts->track_skew * cyl_idx) % opts->sectors_per_track;
            printf("Formatting Track: %u\r", cyl_idx);
            fflush(stdout);
            for (head_idx = 0; (int)head_idx < opts->num_heads; ++head_idx) {
                int imdf_ret = imdf_format_track(sio->imdf_handle,
                                     (uint8_t)cyl_idx,
                                     (uint8_t)head_idx,
                                     IMD_MODE_MFM_250, /* Default mode for OASIS typically */
                                     (uint8_t)opts->sectors_per_track,
                                     SECTOR_SIZE, /* OASIS uses 256-byte sectors */
                                     first_sect_id_for_format,
                                     (uint8_t)opts->sector_increment,
                                     current_track_skew,
                                     pattern_byte);
                if (imdf_ret != IMDF_ERR_OK) {
                    fprintf(stderr, "\nError formatting IMD track C%u H%u (libimdf error: %d).\n", cyl_idx, head_idx, imdf_ret);
                    return EXIT_FAILURE;
                }
            }
        }
        printf("\nIMD Low-level format complete.\n");
        /* After formatting, libimdf might have updated its internal track/sector count. Reload total_sectors. */
        uint32_t new_total_sectors_imd = 0;
        size_t num_img_tracks_imd = 0;
        imdf_get_num_tracks(sio->imdf_handle, &num_img_tracks_imd);
        for (size_t i_trk = 0; i_trk < num_img_tracks_imd; ++i_trk) {
            const ImdTrackInfo* ti = imdf_get_track_info(sio->imdf_handle, i_trk);
            if (ti && ti->loaded) { /* Check if track info is valid and loaded */
                if (ti->sector_size == 256) {
                    new_total_sectors_imd += ti->num_sectors;
                } else if (ti->sector_size == 128 && ti->num_sectors % 2 == 0) {
                    new_total_sectors_imd += ti->num_sectors / 2;
                } /* Other sector sizes were ruled out in sector_io_open */
            }
        }
        sio->total_sectors = new_total_sectors_imd;
    } else { /* RAW image: Simulate low-level format by writing pattern to all sectors */
        total_disk_sectors = (uint32_t)opts->num_heads * (uint32_t)opts->tracks_per_surface * (uint32_t)opts->sectors_per_track; /* Perform multiplication with uint32_t */
        for (track_lba_iter = 0; track_lba_iter < total_disk_sectors; ++track_lba_iter) {
            if (track_lba_iter % (uint32_t)opts->sectors_per_track == 0) { /* Print progress per track */
                printf("Formatting Track: %u\r", track_lba_iter / opts->sectors_per_track);
                fflush(stdout);
            }
            if (sector_io_write(sio, track_lba_iter, 1, sector_write_buf) != 1) {
                fprintf(stderr, "\nError writing pattern to sector %u for RAW image format.\n", track_lba_iter);
                return EXIT_FAILURE;
            }
        }
        printf("\nRAW image pattern write complete.\n");
        sio->total_sectors = total_disk_sectors; /* Update total_sectors in sio stream for RAW */
    }

    /* After low-level format (real or simulated), initialize OASIS filesystem structures */
    return initdisk_initialize_filesystem_structures(sio, opts, disk_layout);
}

/*
 * Handles the BUILD operation.
 */
int initdisk_handle_build_operation(sector_io_stream_t* sio,
                                    initdisk_options_lib_t* opts,
                                    oasis_disk_layout_t* disk_layout) {
    /* For BUILD, we assume the disk is already low-level formatted.
     * We need to determine the disk geometry.
     * If geometry options were provided with BUILD, use them.
     * Otherwise, try to read Sector 1 to get existing geometry.
     */
    if (!opts->heads_specified && !opts->tracks_specified && !opts->sector_specififed) {
        /* No geometry provided, attempt to read from existing FS Block */
        uint8_t sector1_buffer_build[SECTOR_SIZE];
        if (sector_io_read(sio, 1, 1, sector1_buffer_build) == 1) {
            filesystem_block_t existing_fsb;
            memcpy(&existing_fsb, sector1_buffer_build, sizeof(filesystem_block_t));
            opts->num_heads = existing_fsb.num_heads >> 4;
            opts->tracks_per_surface = existing_fsb.num_cyl;
            opts->sectors_per_track = existing_fsb.num_sectors;
            printf("Info: Using existing disk geometry for BUILD: H:%d, T:%d, S:%d\n",
                   opts->num_heads, opts->tracks_per_surface, opts->sectors_per_track);
            if (opts->num_heads == 0 || opts->tracks_per_surface == 0 || opts->sectors_per_track == 0) {
                 fprintf(stderr, "Error: Existing disk geometry read from Sector 1 is invalid. Cannot proceed with BUILD.\n"
                                "Please specify geometry (HEAD, TRACKS, SECTOR) or FORMAT the disk first.\n");
                return EXIT_FAILURE;
            }
        } else {
            fprintf(stderr, "Error: Could not read Sector 1 to determine existing geometry for BUILD.\n"
                            "Please specify geometry (HEAD, TRACKS, SECTOR) or FORMAT the disk first.\n");
            return EXIT_FAILURE;
        }
    } else {
        printf("Info: Using user-specified geometry for BUILD: H:%d, T:%d, S:%d\n",
               opts->num_heads, opts->tracks_per_surface, opts->sectors_per_track);
    }
    if(sio->total_sectors == 0) { /* If image was just created or is empty */
        sio->total_sectors = (uint32_t)opts->num_heads * opts->tracks_per_surface * opts->sectors_per_track;
    }

    return initdisk_initialize_filesystem_structures(sio, opts, disk_layout);
}

/*
 * Handles the CLEAR operation.
 */
int initdisk_handle_clear_operation(sector_io_stream_t* sio,
                                    initdisk_options_lib_t* opts,
                                    oasis_disk_layout_t* disk_layout) {
    uint32_t i;
    uint32_t total_1k_blocks_on_disk;
    uint32_t allocated_blocks_count = 0;
    size_t am_bytes_in_sector1;
    uint32_t additional_am_sectors;
    uint32_t dir_start_sector_lba;
    uint32_t num_dir_sectors_from_fsb;

    /* Load existing disk metadata to get fsblock, alloc_map, directory for modification */
    if (!load_oasis_disk(sio, disk_layout)) {
        fprintf(stderr, "Error: Failed to load existing disk metadata for CLEAR operation.\n");
        return EXIT_FAILURE;
    }

    printf("Clearing disk directory and allocation map...\n");

    /* 1. Re-initialize the allocation map to all free, then mark system reserved blocks. */
    if (disk_layout->alloc_map.map_data) {
        memset(disk_layout->alloc_map.map_data, 0, disk_layout->alloc_map.map_size_bytes);
    } else {
        /* This case should ideally not happen if load_oasis_disk succeeded and map_size_bytes > 0 */
        /* If map_data is NULL but map_size_bytes > 0, it implies an issue. */
        /* However, if map_size_bytes is also 0 (e.g., unformatted disk), this is fine. */
        if (disk_layout->alloc_map.map_size_bytes > 0) {
            fprintf(stderr, "Warning: Allocation map data is NULL but size is non-zero during CLEAR. Proceeding cautiously.\n");
        }
    }

    total_1k_blocks_on_disk = (uint32_t)get_total_blocks(&disk_layout->fsblock);
    additional_am_sectors = disk_layout->fsblock.fs_flags & ADDITIONAL_AM_SECTORS_MASK;
    num_dir_sectors_from_fsb = disk_layout->fsblock.dir_sectors_max;

    /* Mark system-used blocks in allocation map */
    allocated_blocks_count = 0;
    if (total_1k_blocks_on_disk > 0 && disk_layout->alloc_map.map_data) {
        /* Block 0 (boot sector, fs block, part of AM) */
        if (set_block_state(&disk_layout->alloc_map, 0, 1) == 0) {
            allocated_blocks_count++;
        }

        /* Blocks for additional AM sectors (if any) */
        for (i = 0; i < additional_am_sectors; ++i) {
            uint32_t lba = 2 + i;
            uint32_t k_block = lba / SECTORS_PER_BLOCK;
            if (k_block < total_1k_blocks_on_disk) {
                /* Check if already counted to avoid double counting if k_block is 0 (unlikely for AM sectors > S1) */
                int current_state;
                if (get_block_state(&disk_layout->alloc_map, k_block, &current_state) == 0 && current_state == 0) {
                    if (set_block_state(&disk_layout->alloc_map, k_block, 1) == 0) {
                        allocated_blocks_count++;
                    }
                }
            }
        }
        /* Blocks for directory sectors */
        dir_start_sector_lba = 2 + additional_am_sectors; /* LBA of first directory sector */
        for (i = 0; i < num_dir_sectors_from_fsb; ++i) {
            uint32_t lba = dir_start_sector_lba + i;
            uint32_t k_block = lba / SECTORS_PER_BLOCK;
            if (k_block < total_1k_blocks_on_disk) {
                int current_state;
                if (get_block_state(&disk_layout->alloc_map, k_block, &current_state) == 0 && current_state == 0) {
                     if (set_block_state(&disk_layout->alloc_map, k_block, 1) == 0) {
                        allocated_blocks_count++;
                    }
                } else if (current_state == 1 && k_block == 0 && i < SECTORS_PER_BLOCK) {
                    /* If directory sectors share block 0, it's already counted. */
                }
            }
        }
    }

    /* 2. Update free_blocks in fsblock */
    if (total_1k_blocks_on_disk >= allocated_blocks_count) {
        disk_layout->fsblock.free_blocks = (uint16_t)(total_1k_blocks_on_disk - allocated_blocks_count);
    } else {
        /* This indicates a serious inconsistency if allocated (system) blocks exceed total. */
        fprintf(stderr, "Error: Calculated system allocated blocks (%u) exceeds total disk blocks (%u) during CLEAR.\n", allocated_blocks_count, total_1k_blocks_on_disk);
        disk_layout->fsblock.free_blocks = 0; /* Set to 0 on error. */
        /* No need to call cleanup_oasis_disk here, let caller handle full cleanup. */
        return EXIT_FAILURE;
    }

    /* 3. Clear all directory entries (mark as empty) */
    if (disk_layout->directory) {
        for (i = 0; i < disk_layout->fsblock.dir_sectors_max * DIR_ENTRIES_PER_SECTOR; ++i) {
            if (i * sizeof(directory_entry_block_t) < disk_layout->directory->directory_size_bytes) {
                memset(&disk_layout->directory->directory[i], 0, sizeof(directory_entry_block_t));
                disk_layout->directory->directory[i].file_format = FILE_FORMAT_EMPTY;
            }
        }
    }

    /* If LABEL was specified with CLEAR, update it and timestamp */
    if (opts->label_specified) {
         memset(disk_layout->fsblock.label, ' ', FNAME_LEN);
         strncpy(disk_layout->fsblock.label, opts->disk_label_str, strlen(opts->disk_label_str) > FNAME_LEN ? FNAME_LEN : strlen(opts->disk_label_str));
         time_t now_label_clear; time(&now_label_clear); struct tm *lt_label_clear = localtime(&now_label_clear);
         if (lt_label_clear) {
            oasis_convert_tm_to_timestamp(lt_label_clear, &disk_layout->fsblock.timestamp);
         }
    }
    /* Update WP/NOWP status if specified alongside CLEAR */
    if (opts->wp_op) disk_layout->fsblock.fs_flags |= FS_FLAGS_WP;
    if (opts->nowp_op) disk_layout->fsblock.fs_flags &= ~FS_FLAGS_WP;


    /* Write back modified FS block, AM, and directory */
    uint8_t sector1_buffer_clear[SECTOR_SIZE];
    memcpy(sector1_buffer_clear, &disk_layout->fsblock, sizeof(filesystem_block_t));
    am_bytes_in_sector1 = SECTOR_SIZE - sizeof(filesystem_block_t);
    if (disk_layout->alloc_map.map_data) {
        size_t bytes_to_copy_s1_clear = disk_layout->alloc_map.map_size_bytes < am_bytes_in_sector1 ? disk_layout->alloc_map.map_size_bytes : am_bytes_in_sector1;
        memcpy(sector1_buffer_clear + sizeof(filesystem_block_t), disk_layout->alloc_map.map_data, bytes_to_copy_s1_clear);
        if (bytes_to_copy_s1_clear < am_bytes_in_sector1) { /* Zero out rest of AM part in sector 1 if map is smaller */
            memset(sector1_buffer_clear + sizeof(filesystem_block_t) + bytes_to_copy_s1_clear, 0, am_bytes_in_sector1 - bytes_to_copy_s1_clear);
        }
    } else if (am_bytes_in_sector1 > 0) { /* No map data, but space for it in sector 1 */
        memset(sector1_buffer_clear + sizeof(filesystem_block_t), 0, am_bytes_in_sector1);
    }

    if (sector_io_write(sio, 1, 1, sector1_buffer_clear) != 1) {
        fprintf(stderr, "Error writing filesystem block for CLEAR operation.\n"); return EXIT_FAILURE;
    }
    uint32_t add_am_clear_op = disk_layout->fsblock.fs_flags & ADDITIONAL_AM_SECTORS_MASK;
    if (add_am_clear_op > 0 && disk_layout->alloc_map.map_data) {
        size_t am_already_in_s1_clear = disk_layout->alloc_map.map_size_bytes < am_bytes_in_sector1 ? disk_layout->alloc_map.map_size_bytes : am_bytes_in_sector1;
        if (disk_layout->alloc_map.map_size_bytes > am_already_in_s1_clear) {
           if(sector_io_write(sio, 2, add_am_clear_op, disk_layout->alloc_map.map_data + am_already_in_s1_clear) != (ssize_t)add_am_clear_op) {
               fprintf(stderr, "Error writing additional allocation map sectors for CLEAR operation.\n"); return EXIT_FAILURE;
           }
        } else if (disk_layout->alloc_map.map_size_bytes <= am_already_in_s1_clear && add_am_clear_op > 0) {
            /* If all map data fit in sector 1, but fs_flags indicates additional AM sectors,
               these additional sectors should be zeroed out on disk. */
            uint8_t zeroed_sector[SECTOR_SIZE];
            memset(zeroed_sector, 0, SECTOR_SIZE);
            for(uint32_t sec_idx = 0; sec_idx < add_am_clear_op; ++sec_idx) {
                if(sector_io_write(sio, 2 + sec_idx, 1, zeroed_sector) != 1) {
                    fprintf(stderr, "Error zeroing out unused additional allocation map sector %u for CLEAR operation.\n", 2 + sec_idx);
                    /* Non-fatal for the main clear, but an inconsistency. */
                }
            }
        }
    }
    uint32_t dir_start_clear_op = 2 + add_am_clear_op; /* Corrected from 1+1+... */
    if (disk_layout->fsblock.dir_sectors_max > 0 && disk_layout->directory) {
        if (sector_io_write(sio, dir_start_clear_op, disk_layout->fsblock.dir_sectors_max, (uint8_t*)disk_layout->directory->directory) != (ssize_t)disk_layout->fsblock.dir_sectors_max) {
            fprintf(stderr, "Error writing cleared directory sectors.\n"); return EXIT_FAILURE;
        }
    }

    printf("Disk directory and allocation map cleared.\n");
    return EXIT_SUCCESS;
}

/*
 * Handles the LABEL operation.
 */
int initdisk_handle_label_operation(sector_io_stream_t* sio,
                                    initdisk_options_lib_t* opts,
                                    oasis_disk_layout_t* disk_layout) {
    uint8_t sector1_buffer_label[SECTOR_SIZE];
    time_t now_label_op;
    struct tm* local_time_label_op;

    /* Read existing sector 1 to get current fsblock */
    if (sector_io_read(sio, 1, 1, sector1_buffer_label) != 1) {
        fprintf(stderr, "Error reading sector 1 for LABEL operation.\n");
        return EXIT_FAILURE;
    }
    memcpy(&disk_layout->fsblock, sector1_buffer_label, sizeof(filesystem_block_t));

    /* Set new label and update timestamp */
    memset(disk_layout->fsblock.label, ' ', FNAME_LEN);
    strncpy(disk_layout->fsblock.label, opts->disk_label_str, strlen(opts->disk_label_str) > FNAME_LEN ? FNAME_LEN : strlen(opts->disk_label_str));

    time(&now_label_op);
    local_time_label_op = localtime(&now_label_op);
    if (local_time_label_op != NULL) {
        oasis_convert_tm_to_timestamp(local_time_label_op, &disk_layout->fsblock.timestamp);
    }

    /* Apply WP/NOWP status if specified alongside LABEL */
    if (opts->wp_op) disk_layout->fsblock.fs_flags |= FS_FLAGS_WP;
    if (opts->nowp_op) disk_layout->fsblock.fs_flags &= ~FS_FLAGS_WP;


    /* Write modified fsblock part back to sector 1 buffer */
    memcpy(sector1_buffer_label, &disk_layout->fsblock, sizeof(filesystem_block_t));
    if (sector_io_write(sio, 1, 1, sector1_buffer_label) != 1) {
        fprintf(stderr, "Error writing updated label to sector 1.\n");
        return EXIT_FAILURE;
    }
    printf("Disk label changed to '%.*s'.\n", FNAME_LEN, disk_layout->fsblock.label);
    return EXIT_SUCCESS;
}

/*
 * Handles setting or clearing the software write-protection flag.
 */
int initdisk_handle_wp_operation(sector_io_stream_t* sio, int write_protect) {
    uint8_t sector1_buffer_wp_op[SECTOR_SIZE];
    filesystem_block_t fsb_wp_op;

    if (sector_io_read(sio, 1, 1, sector1_buffer_wp_op) != 1) {
        fprintf(stderr, "Error reading sector 1 for WP/NOWP operation.\n");
        return EXIT_FAILURE;
    }
    memcpy(&fsb_wp_op, sector1_buffer_wp_op, sizeof(filesystem_block_t));

    if (write_protect) {
        fsb_wp_op.fs_flags |= FS_FLAGS_WP;
        printf("Disk is now software write-protected.\n");
    } else {
        fsb_wp_op.fs_flags &= ~FS_FLAGS_WP;
        printf("Disk software write-protection removed.\n");
    }

    memcpy(sector1_buffer_wp_op, &fsb_wp_op, sizeof(filesystem_block_t));
    if (sector_io_write(sio, 1, 1, sector1_buffer_wp_op) != 1) {
        fprintf(stderr, "Error writing updated fs_flags to sector 1 for WP/NOWP.\n");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}


/*
 * Main dispatch function for disk initialization operations.
 */
int initdisk_perform_operation(initdisk_options_lib_t* opts) {
    sector_io_stream_t* sio = NULL;
    oasis_disk_layout_t disk_layout_lib; /* Keep this local to the library function scope */
    int result = EXIT_FAILURE; /* Default to failure */
    int needs_write_access = 0;
    const char* open_mode = "rb"; /* Default to read-only */
    int attempt_create_image = 0;

    memset(&disk_layout_lib, 0, sizeof(disk_layout_lib));

    /* Determine if write access is needed and if image creation should be attempted */
    if (opts->format_op || opts->clear_op || opts->build_op || opts->label_op || opts->wp_op || opts->nowp_op) {
        needs_write_access = 1;
    }

    if (needs_write_access) {
        open_mode = "r+b"; /* Read-write */
        if (opts->format_op || opts->build_op) { /* FORMAT or BUILD can create a new image */
            attempt_create_image = 1;
        }
    }

    /* Attempt to open/create the disk image */
    if (attempt_create_image && !file_exists_lib(opts->image_path)) {
        printf("Info: Disk image '%s' does not exist.\n", opts->image_path);
        const char* ext_check_lib = strrchr(opts->image_path, '.');
        if (ext_check_lib != NULL && (strcasecmp(ext_check_lib, ".imd") == 0)) {
            if (!initdisk_create_empty_imd_file(opts->image_path)) {
                return EXIT_FAILURE; /* Failed to create empty IMD */
            }
            /* After creating, open it in r+b mode */
            sio = sector_io_open(opts->image_path, "r+b");
        } else { /* Assume .img or no extension, try to create with w+b */
            sio = sector_io_open(opts->image_path, "w+b");
            if (sio != NULL) {
                 printf("Info: Created new disk image '%s'.\n", opts->image_path);
            }
        }
    } else {
        /* File exists, or no creation attempt needed, open with determined mode */
        sio = sector_io_open(opts->image_path, open_mode);
    }

    if (sio == NULL) {
        fprintf(stderr, "Error: Could not open disk image '%s' with mode '%s'.\n", opts->image_path, open_mode);
        return EXIT_FAILURE;
    }

    printf("INITDISK Library: Processing image '%s' (Drive %c)\n", opts->image_path, opts->drive_letter);

    /* Dispatch to the appropriate handler based on operation flags */
    if (opts->format_op) {
        result = initdisk_handle_format_operation(sio, opts, &disk_layout_lib);
    } else if (opts->clear_op) {
        result = initdisk_handle_clear_operation(sio, opts, &disk_layout_lib);
    } else if (opts->build_op) {
        result = initdisk_handle_build_operation(sio, opts, &disk_layout_lib);
    } else if (opts->label_op && !opts->format_op && !opts->clear_op && !opts->build_op) {
        /* Label op can be standalone or part of format/clear/build.
           If standalone, call its handler. If combined, it's handled within those.
           The main utility will ensure LABEL is not the *only* primary op if others are present.
           This library function assumes `opts` is set up correctly by the caller.
        */
        result = initdisk_handle_label_operation(sio, opts, &disk_layout_lib);
    } else if (opts->wp_op) {
        result = initdisk_handle_wp_operation(sio, 1); /* 1 for write-protect */
    } else if (opts->nowp_op) {
        result = initdisk_handle_wp_operation(sio, 0); /* 0 for remove write-protect */
    } else {
        fprintf(stderr, "Error: No primary operation specified for initdisk_perform_operation.\n");
        result = EXIT_FAILURE;
    }

    /* Cleanup resources */
    cleanup_oasis_disk(&disk_layout_lib); /* Frees alloc_map.map_data and directory if allocated */
    if (sio != NULL) {
        sector_io_close(sio);
    }
    return result;
}
