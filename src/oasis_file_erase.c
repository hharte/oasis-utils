/*
 * oasis_file_erase.c - Implementation of OASIS File Erasing Utilities
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#include "oasis.h" /* For core definitions, including FILE_FORMAT_MASK, etc. */

#include <stdio.h>  /* For fprintf, stderr, printf */
#include <string.h> /* For memset, strncpy */
#include <stdlib.h> /* For realloc, free */

/* Helper structure for tracking unique 1K blocks of a sequential file during erase */
typedef struct {
    uint16_t* blocks;    /* Dynamically allocated array of 1K block indices */
    size_t count;        /* Number of unique blocks currently stored */
    size_t capacity;     /* Current allocated capacity of the blocks array */
} unique_block_tracker_erase_t;

/* Initializes the unique block tracker */
static void init_unique_block_tracker_erase(unique_block_tracker_erase_t* tracker) {
    if (tracker == NULL) return;
    tracker->blocks = NULL;
    tracker->count = 0;
    tracker->capacity = 0;
}

/* Adds a block index to the tracker if not already present, resizing if necessary */
static bool add_unique_block_to_tracker_erase(unique_block_tracker_erase_t* tracker, uint16_t block_idx) {
    size_t i;
    size_t new_capacity;
    uint16_t* new_blocks_ptr;

    if (tracker == NULL) return false;

    /* Check if block already tracked */
    for (i = 0; i < tracker->count; ++i) {
        if (tracker->blocks[i] == block_idx) {
            return true; /* Already tracked, success */
        }
    }

    /* Add new block, resize if necessary */
    if (tracker->count >= tracker->capacity) {
        new_capacity = (tracker->capacity == 0) ? 8 : tracker->capacity * 2;
        if (new_capacity > OASIS_MAX_FS_BLOCKS) {
            new_capacity = OASIS_MAX_FS_BLOCKS;
        }
        if (new_capacity <= tracker->count && new_capacity < OASIS_MAX_FS_BLOCKS) {
            fprintf(stderr, "add_unique_block_to_tracker_erase: Error - Cannot expand tracker capacity further.\n");
            return false;
        }

        new_blocks_ptr = (uint16_t*)realloc(tracker->blocks, new_capacity * sizeof(uint16_t));
        if (!new_blocks_ptr) {
            perror("add_unique_block_to_tracker_erase: Failed to realloc for tracker");
            return false;
        }
        tracker->blocks = new_blocks_ptr;
        tracker->capacity = new_capacity;
    }
    tracker->blocks[tracker->count++] = block_idx;
    return true;
}

/* Frees memory used by the unique block tracker */
static void free_unique_block_tracker_erase(unique_block_tracker_erase_t* tracker) {
    if (tracker == NULL) return;
    if (tracker->blocks) {
        free(tracker->blocks);
        tracker->blocks = NULL;
    }
    tracker->count = 0;
    tracker->capacity = 0;
}

/*
 * Writes the filesystem block and allocation map (initial part) to Sector 1.
 * Converts uint16_t fields in fsblock to little-endian before writing.
 */
static bool write_fsblock_and_initial_am(sector_io_stream_t* img_stream, oasis_disk_layout_t* disk_layout) {
    uint8_t sector1_buffer[SECTOR_SIZE];
    filesystem_block_t fs_block_le; /* Temporary LE version */
    size_t map_in_sector1;

    if (!img_stream || !disk_layout || !disk_layout->alloc_map.map_data) {
        fprintf(stderr, "write_fsblock_and_initial_am: Invalid arguments.\n");
        return false;
    }

    /* Prepare little-endian version of fsblock */
    memcpy(&fs_block_le, &disk_layout->fsblock, sizeof(filesystem_block_t));
    fs_block_le.reserved = htole16(disk_layout->fsblock.reserved);
    fs_block_le.free_blocks = htole16(disk_layout->fsblock.free_blocks);
    /* fs_block_le.fs_flags is uint8_t, no conversion needed */

    memset(sector1_buffer, 0, SECTOR_SIZE);
    memcpy(sector1_buffer, &fs_block_le, sizeof(filesystem_block_t));

    if (disk_layout->alloc_map.map_data && disk_layout->alloc_map.map_size_bytes > 0) {
        map_in_sector1 = SECTOR_SIZE - sizeof(filesystem_block_t);
        if (map_in_sector1 > disk_layout->alloc_map.map_size_bytes) {
            map_in_sector1 = disk_layout->alloc_map.map_size_bytes;
        }
        memcpy(sector1_buffer + sizeof(filesystem_block_t), disk_layout->alloc_map.map_data, map_in_sector1);
    }

    if (sector_io_write(img_stream, 1, 1, sector1_buffer) != 1) {
        fprintf(stderr, "Error: Failed to write updated FS block and initial AM to Sector 1.\n");
        return false;
    }
    printf("Successfully wrote updated FS block/AM to Sector 1.\n");
    return true;
}

/*
 * Writes additional allocation map sectors (if any) to disk.
 * Allocation map data itself is a bitmap, no endian conversion needed for its content.
 */
static bool write_additional_am_sectors(sector_io_stream_t* img_stream, oasis_disk_layout_t* disk_layout) {
    uint32_t additional_am_sectors_count;
    size_t map_in_sector1;

    if (!img_stream || !disk_layout || !disk_layout->alloc_map.map_data) {
        /* Not necessarily an error if map_data is NULL and no additional sectors */
        if (disk_layout && (disk_layout->fsblock.fs_flags & ADDITIONAL_AM_SECTORS_MASK) > 0 && !disk_layout->alloc_map.map_data) {
            fprintf(stderr, "write_additional_am_sectors: map_data is NULL but additional AM sectors are indicated.\n");
            return false;
        }
        return true; /* No additional sectors to write or no map data */
    }

    additional_am_sectors_count = disk_layout->fsblock.fs_flags & ADDITIONAL_AM_SECTORS_MASK;
    if (additional_am_sectors_count > 0) {
        map_in_sector1 = SECTOR_SIZE - sizeof(filesystem_block_t);
        if (disk_layout->alloc_map.map_size_bytes > map_in_sector1) {
            if (sector_io_write(img_stream, 2, additional_am_sectors_count, disk_layout->alloc_map.map_data + map_in_sector1) != (ssize_t)additional_am_sectors_count) {
                fprintf(stderr, "Error: Failed to write additional allocation map sectors.\n");
                return false;
            }
            printf("Successfully wrote updated additional AM sectors.\n");
        }
    }
    return true;
}

/*
 * Writes the directory to disk.
 * Converts uint16_t fields in each DEB to little-endian before writing.
 */
static bool write_directory_to_disk(sector_io_stream_t* img_stream, oasis_disk_layout_t* disk_layout) {
    uint32_t dir_start_sector_lba;
    size_t num_debs;
    uint8_t* le_dir_buffer = NULL; /* Buffer for little-endian directory data */

    if (!img_stream || !disk_layout || !disk_layout->directory) {
        fprintf(stderr, "write_directory_to_disk: Invalid arguments.\n");
        return false;
    }
    if (disk_layout->fsblock.dir_sectors_max == 0 || disk_layout->directory->directory_size_bytes == 0) {
        printf("Directory is empty or not configured to be written. Skipping directory write.\n");
        return true; /* Not an error if directory is meant to be empty */
    }

    dir_start_sector_lba = 1 + 1 + (disk_layout->fsblock.fs_flags & ADDITIONAL_AM_SECTORS_MASK);
    num_debs = disk_layout->directory->directory_size_bytes / sizeof(directory_entry_block_t);

    le_dir_buffer = (uint8_t*)malloc(disk_layout->directory->directory_size_bytes);
    if (!le_dir_buffer) {
        perror("write_directory_to_disk: Failed to allocate buffer for LE directory");
        return false;
    }

    /* Convert all DEBs to little-endian in the temporary buffer */
    for (size_t i = 0; i < num_debs; ++i) {
        directory_entry_block_t* src_deb_host = &disk_layout->directory->directory[i];
        directory_entry_block_t* dst_deb_le = (directory_entry_block_t*)(le_dir_buffer + (i * sizeof(directory_entry_block_t)));

        dst_deb_le->file_format = src_deb_host->file_format;
        memcpy(dst_deb_le->file_name, src_deb_host->file_name, FNAME_LEN);
        memcpy(dst_deb_le->file_type, src_deb_host->file_type, FTYPE_LEN);
        memcpy(&dst_deb_le->timestamp, &src_deb_host->timestamp, sizeof(oasis_tm_t));
        dst_deb_le->owner_id = src_deb_host->owner_id;
        dst_deb_le->shared_from_owner_id = src_deb_host->shared_from_owner_id;

        dst_deb_le->record_count = htole16(src_deb_host->record_count);
        dst_deb_le->block_count = htole16(src_deb_host->block_count);
        dst_deb_le->start_sector = htole16(src_deb_host->start_sector);
        dst_deb_le->file_format_dependent1 = htole16(src_deb_host->file_format_dependent1);
        dst_deb_le->file_format_dependent2 = htole16(src_deb_host->file_format_dependent2);
    }

    if (sector_io_write(img_stream, dir_start_sector_lba, disk_layout->fsblock.dir_sectors_max, le_dir_buffer) != disk_layout->fsblock.dir_sectors_max) {
        fprintf(stderr, "Error: Failed to write updated directory to disk image.\n");
        free(le_dir_buffer);
        return false;
    }
    free(le_dir_buffer);
    printf("Successfully wrote updated directory to disk image.\n");
    return true;
}


bool oasis_erase_single_file(sector_io_stream_t* img_stream,
    oasis_disk_layout_t* disk_layout,
    directory_entry_block_t* deb_to_erase, /* This DEB is in host byte order */
    size_t deb_index) {
    char host_filename[MAX_HOST_FILENAME_LEN];
    uint16_t blocks_freed_for_this_file = 0;
    bool current_file_erase_success = true;

    if (!img_stream || !disk_layout || !disk_layout->alloc_map.map_data || !deb_to_erase) {
        fprintf(stderr, "oasis_erase_single_file: Error - Invalid arguments.\n");
        return false;
    }

    oasis_deb_to_host_filename(deb_to_erase, host_filename, sizeof(host_filename));
    printf("  Erasing DEB #%zu: %s (Format: 0x%02X, Blocks: %u, StartLBA: %u)\n",
        deb_index, host_filename, deb_to_erase->file_format, deb_to_erase->block_count, deb_to_erase->start_sector);

    if (deb_to_erase->block_count > 0 && deb_to_erase->start_sector != 0) {
        uint8_t file_type_bits = deb_to_erase->file_format & FILE_FORMAT_MASK;

        if (file_type_bits == FILE_FORMAT_SEQUENTIAL) {
            unique_block_tracker_erase_t unique_blocks;
            uint16_t current_lba_seq = deb_to_erase->start_sector; /* Host order */
            uint8_t sector_buffer_seq[SECTOR_SIZE];
            uint32_t sectors_walked_seq = 0;
            const uint32_t MAX_SECTORS_TO_WALK_SEQ = 65535;
            size_t j;
            uint16_t next_lba_le; /* For reading link from disk */

            init_unique_block_tracker_erase(&unique_blocks);
            printf("    Sequential file: Walking sector chain to identify 1K blocks...\n");

            while (current_lba_seq != 0 && sectors_walked_seq < MAX_SECTORS_TO_WALK_SEQ) {
                sectors_walked_seq++;
                uint16_t current_1k_block_idx = current_lba_seq / (BLOCK_SIZE / SECTOR_SIZE);

                if (!add_unique_block_to_tracker_erase(&unique_blocks, current_1k_block_idx)) {
                    fprintf(stderr, "    Error: Failed to track 1K block %u for deallocation. Skipping further block freeing for this file.\n", current_1k_block_idx);
                    current_file_erase_success = false;
                    break;
                }

                if (sector_io_read(img_stream, current_lba_seq, 1, sector_buffer_seq) != 1) {
                    fprintf(stderr, "    Error: Failed to read sector %u in chain for file %s. Deallocation for this file might be incomplete.\n", current_lba_seq, host_filename);
                    current_file_erase_success = false;
                    break;
                }
                memcpy(&next_lba_le, sector_buffer_seq + OASIS_SEQ_DATA_PER_SECTOR, sizeof(uint16_t));
                current_lba_seq = le16toh(next_lba_le); /* Convert link to host order for loop condition */
            }
            if (sectors_walked_seq >= MAX_SECTORS_TO_WALK_SEQ && current_lba_seq != 0) {
                fprintf(stderr, "    Warning: Sequential file chain for %s is excessively long or cyclic. Deallocation might be incomplete.\n", host_filename);
                current_file_erase_success = false;
            }

            if (unique_blocks.blocks != NULL) {
                printf("    Identified %zu unique 1K blocks for sequential file '%s'. Attempting deallocation.\n", unique_blocks.count, host_filename);
                for (j = 0; j < unique_blocks.count; ++j) {
                    if (deallocate_blocks(&disk_layout->alloc_map, unique_blocks.blocks[j], 1) == 0) {
                        blocks_freed_for_this_file++;
                    }
                    else {
                        fprintf(stderr, "    Warning: Failed to deallocate 1K block %u (from sequential file '%s') in allocation map.\n", unique_blocks.blocks[j], host_filename);
                    }
                }
                free_unique_block_tracker_erase(&unique_blocks);
            }
        }
        else { /* Contiguous files */
            uint16_t start_1k_block_idx = deb_to_erase->start_sector / (BLOCK_SIZE / SECTOR_SIZE);
            if (deallocate_blocks(&disk_layout->alloc_map, start_1k_block_idx, deb_to_erase->block_count) == 0) {
                blocks_freed_for_this_file = deb_to_erase->block_count;
            }
            else {
                fprintf(stderr, "    Error: Failed to deallocate blocks for contiguous file %s in allocation map.\n", host_filename);
                current_file_erase_success = false;
            }
        }

        if (blocks_freed_for_this_file > 0) {
            disk_layout->fsblock.free_blocks += blocks_freed_for_this_file;
            printf("    Freed %u block(s) from allocation map for '%s'.\n", blocks_freed_for_this_file, host_filename);
        }
    }
    else if (deb_to_erase->block_count > 0 && deb_to_erase->start_sector == 0) {
        printf("    File '%s' has block_count > 0 but start_sector is 0. Cannot deallocate blocks from map.\n", host_filename);
    }

    /* Mark DEB as deleted (all fields zeroed or space-padded as appropriate) */
    deb_to_erase->file_format = FILE_FORMAT_DELETED;
    memset(deb_to_erase->file_name, ' ', FNAME_LEN);
    memset(deb_to_erase->file_type, ' ', FTYPE_LEN);
    deb_to_erase->block_count = 0;
    deb_to_erase->record_count = 0;
    deb_to_erase->start_sector = 0;
    deb_to_erase->file_format_dependent1 = 0;
    deb_to_erase->file_format_dependent2 = 0;
    /* Timestamp and owner ID could also be cleared or left as-is.
       Current behavior is to clear numeric fields and space-pad names.
    */

    return current_file_erase_success;
}


bool oasis_erase_files_by_pattern(sector_io_stream_t* img_stream,
    oasis_disk_layout_t* disk_layout, /* This is in host byte order */
    const cli_options_t* options) {
    size_t num_entries;
    int files_erased_count = 0;
    bool overall_success = true;
    bool directory_needs_write = false;
    bool alloc_map_needs_write = false;
    size_t i;

    if (!disk_layout || !disk_layout->directory || !img_stream || !options || !options->pattern) {
        fprintf(stderr, "oasis_erase_files_by_pattern: Error - Invalid arguments.\n");
        return false;
    }

    num_entries = disk_layout->directory->directory_size_bytes / sizeof(directory_entry_block_t);

    printf("oasis_erase_files_by_pattern: Processing %zu DEBs, searching for files matching pattern '%s' for User ID ",
        num_entries, options->pattern);
    if (options->owner_id_filter == OWNER_ID_WILDCARD) {
        printf("Any Owner (*)...\n");
    }
    else {
        printf("%d...\n", options->owner_id_filter);
    }

    for (i = 0; i < num_entries; ++i) {
        directory_entry_block_t* deb_entry = &disk_layout->directory->directory[i]; /* Host order */

        if (!oasis_deb_is_valid(deb_entry)) {
            continue;
        }

        if (options->owner_id_filter != OWNER_ID_WILDCARD && deb_entry->owner_id != options->owner_id_filter) {
            continue;
        }

        if (oasis_filename_wildcard_match(deb_entry->file_name, deb_entry->file_type, options->pattern)) {
            uint16_t original_block_count = deb_entry->block_count; /* Host order */

            if (oasis_erase_single_file(img_stream, disk_layout, deb_entry, i)) {
                if (original_block_count > 0) {
                    alloc_map_needs_write = true;
                }
                directory_needs_write = true;
                files_erased_count++;
            }
            else {
                overall_success = false;
            }
        }
    }

    if (files_erased_count == 0) {
        printf("No files found matching the pattern and user ID to erase.\n");
        return true;
    }

    /* Write changes back to disk image if any modifications were made */
    if (directory_needs_write) {
        if (!write_directory_to_disk(img_stream, disk_layout)) {
            overall_success = false;
        }
    }

    if (alloc_map_needs_write) { /* This implies fsblock.free_blocks changed or map bits changed */
        if (!write_fsblock_and_initial_am(img_stream, disk_layout)) {
            overall_success = false;
        }
        if (overall_success) { /* Only write additional AM if fsblock write was ok */
            if (!write_additional_am_sectors(img_stream, disk_layout)) {
                overall_success = false;
            }
        }
    }

    printf("%d file(s) erased.\n", files_erased_count);
    return overall_success;
}
