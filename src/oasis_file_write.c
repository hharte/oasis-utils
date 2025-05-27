/*
 * oasis_file_write.c - Implementation of OASIS File Writing Utilities
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */


#include "oasis.h"

#include <stdio.h>       /* For fprintf, perror */
#include <stdlib.h>      /* For calloc, free, realloc, malloc */
#include <string.h>      /* For memcpy, memset */
#include <errno.h>
#include <limits.h>      /* For UINT16_MAX */

 /*
  * Helper structure to keep track of allocated 1K blocks for a single file write operation.
  * This is used for rollback in case of an error during sequential file writing.
  */
typedef struct {
    uint16_t* block_indices;
    size_t count;
    size_t capacity;
} allocated_block_tracker_t;

/* Initializes the block tracker */
static void init_block_tracker(allocated_block_tracker_t* tracker) {
    /* Initialize tracker members. */
    tracker->block_indices = NULL;
    tracker->count = 0;
    tracker->capacity = 0;
}

/* Adds a block index to the tracker, resizing if necessary */
static bool add_tracked_block(allocated_block_tracker_t* tracker, uint16_t block_idx) {
    /* Check if tracker count would exceed UINT16_MAX. */
    /* This check is crucial because deb->block_count is uint16_t. */
    if (tracker->count >= UINT16_MAX) {
        fprintf(stderr, "add_tracked_block: Error - block count would exceed DEB capacity (%u).\n", UINT16_MAX);
        return false; /* Prevent overflow of tracker for DEB. */
    }

    /* Check if tracker needs to be resized. */
    if (tracker->count >= tracker->capacity) {
        size_t new_capacity = (tracker->capacity == 0) ? 16 : tracker->capacity * 2;
        /* Prevent excessive capacity if new_capacity would somehow overflow size_t. */
        if (new_capacity < tracker->capacity) { /* Overflow check for new_capacity itself */
            new_capacity = UINT16_MAX; /* Cap at a reasonable maximum related to DEB limits */
        }
        if (new_capacity > UINT16_MAX) { /* Still ensure it doesn't grow beyond what's meaningful for DEB block_count */
            new_capacity = UINT16_MAX;
        }


        /* Attempt to reallocate memory for block_indices. */
        uint16_t* new_indices = (uint16_t*)realloc(tracker->block_indices, new_capacity * sizeof(uint16_t));
        if (!new_indices) {
            perror("add_tracked_block: Failed to realloc for tracker");
            return false; /* Return false if reallocation fails. */
        }
        tracker->block_indices = new_indices;
        tracker->capacity = new_capacity;
    }
    /* Add the new block index and increment count. */
    tracker->block_indices[tracker->count++] = block_idx;
    return true; /* Return true on success. */
}

/* Frees memory used by the block tracker */
static void free_block_tracker(allocated_block_tracker_t* tracker) {
    /* Free the block_indices array if it's not NULL. */
    if (tracker->block_indices) {
        free(tracker->block_indices);
        tracker->block_indices = NULL; /* Set to NULL after freeing. */
    }
    /* Reset count and capacity. */
    tracker->count = 0;
    tracker->capacity = 0;
}

/* Deallocates all blocks recorded in the tracker */
static void rollback_allocations(oasis_disk_layout_t* disk_layout, allocated_block_tracker_t* tracker) {
    /* Iterate through tracked blocks and deallocate them. */
    if (!disk_layout || !tracker) {
        return;
    }
    for (size_t i = 0; i < tracker->count; ++i) {
        /* Assuming block_indices[i] is the 1K block number. */
        /* It's important that deallocate_blocks can handle being called even if
        // some blocks in the list weren't successfully marked in the map (e.g. if set_block_state failed)
        // The current deallocate_blocks checks if blocks are allocated before freeing.
        */
        deallocate_blocks(&disk_layout->alloc_map, tracker->block_indices[i], 1);
    }
    /* Free the tracker itself. */
    free_block_tracker(tracker);
}


bool oasis_file_write_data(sector_io_stream_t* img_stream,
    oasis_disk_layout_t* disk_layout,
    directory_entry_block_t* deb,
    const uint8_t* data_buffer,
    size_t data_size) {
    /* Validate input parameters. */
    if (!img_stream || !disk_layout || !disk_layout->alloc_map.map_data || !deb || (!data_buffer && data_size > 0)) {
        fprintf(stderr, "oasis_file_write_data: Error - Invalid arguments.\n");
        return false; /* Return false if arguments are invalid. */
    }

    uint8_t file_format_type = deb->file_format & FILE_FORMAT_MASK;

    /* Handle writing an empty file */
    if (data_size == 0) {
        /* Set DEB fields for an empty file. */
        deb->start_sector = 0;
        deb->block_count = 0;
        deb->record_count = 0; /* Default record_count to 0 for empty. */
        if (file_format_type == FILE_FORMAT_SEQUENTIAL) {
            deb->file_format_dependent2 = 0; /* Last sector LBA for sequential. */
        }
        /* For other types, FFD2 meaning varies, caller responsibility or default to 0 if appropriate. */
        return true; /* Successfully "wrote" an empty file. */
    }

    /* --- Contiguous File Writing Logic --- */
    if (file_format_type == FILE_FORMAT_DIRECT ||
        file_format_type == FILE_FORMAT_ABSOLUTE ||
        file_format_type == FILE_FORMAT_RELOCATABLE ||
        file_format_type == FILE_FORMAT_INDEXED ||
        file_format_type == FILE_FORMAT_KEYED) {

        size_t num_1k_blocks_needed = (data_size + BLOCK_SIZE - 1) / BLOCK_SIZE;
        int start_block_num_alloc; /* 1K block number from allocate_blocks. */
        uint16_t start_sector_lba_write;

        if (num_1k_blocks_needed == 0 && data_size > 0) { /* Should not happen with ceil division. */
            num_1k_blocks_needed = 1;
        }

        if (num_1k_blocks_needed > UINT16_MAX) { /* DEB block_count is uint16_t. */
            fprintf(stderr, "oasis_file_write_data: Error - File size requires %zu 1K blocks, which exceeds DEB block_count capacity (max %u).\n", num_1k_blocks_needed, UINT16_MAX);
            return false; /* Return false if block count exceeds capacity. */
        }
        if (num_1k_blocks_needed == 0) { /* Should be caught by data_size == 0, but as safeguard. */
            fprintf(stderr, "oasis_file_write_data: Error - Calculated 0 blocks needed for non-empty data.\n");
            return false; /* Return false if 0 blocks needed for non-empty data. */
        }

        /* Check against system-wide maximum blocks */
        if (num_1k_blocks_needed > OASIS_MAX_FS_BLOCKS) {
            fprintf(stderr, "oasis_file_write_data: Error - File requires %zu 1K blocks, "
                "exceeding system maximum of %u blocks.\n",
                num_1k_blocks_needed, OASIS_MAX_FS_BLOCKS);
            return false;
        }

        /* Allocate contiguous blocks. */
        start_block_num_alloc = allocate_blocks(&disk_layout->alloc_map, num_1k_blocks_needed);

        if (start_block_num_alloc < 0) {
            fprintf(stderr, "oasis_file_write_data: Error - Failed to allocate %zu contiguous 1K blocks. Disk may be full or too fragmented.\n", num_1k_blocks_needed);
            return false; /* Return false if allocation fails. */
        }

        /* Update free blocks count */
        disk_layout->fsblock.free_blocks -= (uint16_t)num_1k_blocks_needed;

        /* Convert 1K block number to starting sector LBA. */
        start_sector_lba_write = (uint16_t)((size_t)start_block_num_alloc * (BLOCK_SIZE / SECTOR_SIZE));

        /* Prepare a buffer that is a multiple of BLOCK_SIZE, zero-padded for the write operation. */
        size_t total_bytes_to_write_to_disk = num_1k_blocks_needed * BLOCK_SIZE;
        uint8_t* block_aligned_buffer = (uint8_t*)calloc(total_bytes_to_write_to_disk, 1);
        if (!block_aligned_buffer) {
            perror("oasis_file_write_data: Error - Failed to allocate block-aligned buffer for writing");
            /* Deallocate the blocks that were just allocated. */
            deallocate_blocks(&disk_layout->alloc_map, (size_t)start_block_num_alloc, num_1k_blocks_needed);
            return false; /* Return false if buffer allocation fails. */
        }
        memcpy(block_aligned_buffer, data_buffer, data_size); /* User data copied, rest is zeros. */

        uint16_t num_sectors_to_write_val = (uint16_t)(num_1k_blocks_needed * (BLOCK_SIZE / SECTOR_SIZE));

        /* Write the aligned buffer to disk. */
        ssize_t sectors_written = sector_io_write(img_stream, start_sector_lba_write, num_sectors_to_write_val, block_aligned_buffer);
        free(block_aligned_buffer); /* Free the temporary buffer. */

        if (sectors_written < 0 || (uint16_t)sectors_written != num_sectors_to_write_val) {
            fprintf(stderr, "oasis_file_write_data: Error - Failed to write all sectors for contiguous file. Expected %u, wrote %zd.\n", num_sectors_to_write_val, sectors_written);
            /* Attempt to deallocate the blocks that were reserved. */
            deallocate_blocks(&disk_layout->alloc_map, (size_t)start_block_num_alloc, num_1k_blocks_needed);
            return false; /* Return false if write fails. */
        }

        /* Update DEB with allocation details. */
        deb->start_sector = start_sector_lba_write;
        deb->block_count = (uint16_t)num_1k_blocks_needed;
        /*
         * Caller is responsible for:
         * - deb->record_count (often 0 for these types, or based on FFD1)
         * - deb->file_format_dependent1 (e.g., record length for Direct, SECTOR_LEN for Abs/Rel)
         * - deb->file_format_dependent2 (e.g., Program Length for Rel, Load Address for Abs)
         */
        return true; /* Return true on success for contiguous files. */

    }
    else if (file_format_type == FILE_FORMAT_SEQUENTIAL) {
        /* Sequential file writing logic begins here. */
        size_t data_remaining = data_size;
        const uint8_t* current_data_ptr = data_buffer;
        uint16_t current_sector_lba = 0;
        uint16_t prev_sector_lba = 0;
        int current_1k_block_idx = -1; /* LBA index of the current 1K block. */
        int sectors_used_in_1k_block = 0; /* Number of 256B sectors used in current_1k_block_idx */

        allocated_block_tracker_t tracker;
        init_block_tracker(&tracker);

        deb->start_sector = 0;
        deb->record_count = 0; /* Will count sectors written. */
        deb->block_count = 0;  /* Will be sum of unique 1K blocks. */

        /* Loop while there's data to write. */
        while (data_remaining > 0) {
            uint8_t sector_write_buffer[SECTOR_SIZE];
            memset(sector_write_buffer, 0, SECTOR_SIZE); /* Initialize sector buffer. */

            size_t bytes_for_this_sector = (data_remaining > OASIS_SEQ_DATA_PER_SECTOR) ? OASIS_SEQ_DATA_PER_SECTOR : data_remaining;

            /* Determine/Allocate LBA for the current 256-byte sector. */
            if (current_1k_block_idx == -1 || sectors_used_in_1k_block >= (BLOCK_SIZE / SECTOR_SIZE)) {
                /* Need a new 1K block. */
                /* Check if adding another block would exceed DEB's capacity. */
                if (tracker.count >= UINT16_MAX) {
                    fprintf(stderr, "oasis_file_write_data (SEQ): Error - Required 1K blocks would exceed DEB capacity (%u).\n", UINT16_MAX);
                    rollback_allocations(disk_layout, &tracker);
                    return false;
                }

                /* tracker.count is the number of unique 1K blocks already allocated for this file. */
                /* If tracker.count is already OASIS_MAX_FS_BLOCKS, we cannot allocate another. */
                if (tracker.count >= OASIS_MAX_FS_BLOCKS) {
                    fprintf(stderr, "oasis_file_write_data (SEQ): Error - File would require more than "
                        "system maximum of %u 1K blocks (currently at %zu).\n",
                        OASIS_MAX_FS_BLOCKS, tracker.count);
                    rollback_allocations(disk_layout, &tracker);
                    return false;
                }

                int allocated_block_lba_idx = allocate_blocks(&disk_layout->alloc_map, 1);
                if (allocated_block_lba_idx < 0) {
                    fprintf(stderr, "oasis_file_write_data (SEQ): Failed to allocate a 1K block.\n");
                    rollback_allocations(disk_layout, &tracker); /* Rollback previous allocations. */
                    return false; /* Return false on allocation failure. */
                }
                if (!add_tracked_block(&tracker, (uint16_t)allocated_block_lba_idx)) {
                    /* Failed to add to tracker, probably memory error. Rollback. */
                    fprintf(stderr, "oasis_file_write_data (SEQ): Failed to track allocated block.\n");
                    deallocate_blocks(&disk_layout->alloc_map, (size_t)allocated_block_lba_idx, 1); /* Deallocate the one we just got. */
                    rollback_allocations(disk_layout, &tracker); /* Deallocate others. */
                    return false; /* Return false. */
                }
                disk_layout->fsblock.free_blocks--;
                current_1k_block_idx = allocated_block_lba_idx;
                sectors_used_in_1k_block = 0;
            }

            current_sector_lba = (uint16_t)(((size_t)current_1k_block_idx * (BLOCK_SIZE / SECTOR_SIZE)) + sectors_used_in_1k_block);
            sectors_used_in_1k_block++;

            if (deb->start_sector == 0) {
                deb->start_sector = current_sector_lba;
            }

            /* Prepare data portion of the sector. */
            if (bytes_for_this_sector > 0) {
                memcpy(sector_write_buffer, current_data_ptr, bytes_for_this_sector);
            }
            /* The link will be set to 0 for now.
            // It will be updated if there's a next sector.
            // The last sector's link will remain 0.
            */
            uint16_t next_link_lba_placeholder = 0; /* Default to 0 (end of chain) */
            memcpy(sector_write_buffer + OASIS_SEQ_DATA_PER_SECTOR, &next_link_lba_placeholder, sizeof(uint16_t));


            /* Write current sector's data and initial link (0). */
            if (sector_io_write(img_stream, current_sector_lba, 1, sector_write_buffer) != 1) {
                fprintf(stderr, "oasis_file_write_data (SEQ): Failed to write sector %u.\n", current_sector_lba);
                rollback_allocations(disk_layout, &tracker);
                return false; /* Return false on write failure. */
            }

            /* If there was a previous sector, update its link to point to the current sector. */
            if (prev_sector_lba != 0) {
                uint8_t prev_sector_read_buffer[SECTOR_SIZE];
                if (sector_io_read(img_stream, prev_sector_lba, 1, prev_sector_read_buffer) != 1) {
                    fprintf(stderr, "oasis_file_write_data (SEQ): Failed to re-read previous sector %u to write link.\n", prev_sector_lba);
                    rollback_allocations(disk_layout, &tracker); /* Rollback includes current_sector_lba if it was tracked. */
                    return false; /* Return false on read failure. */
                }
                memcpy(prev_sector_read_buffer + OASIS_SEQ_DATA_PER_SECTOR, &current_sector_lba, sizeof(uint16_t));
                if (sector_io_write(img_stream, prev_sector_lba, 1, prev_sector_read_buffer) != 1) {
                    fprintf(stderr, "oasis_file_write_data (SEQ): Failed to write link to previous sector %u.\n", prev_sector_lba);
                    rollback_allocations(disk_layout, &tracker);
                    return false; /* Return false on write failure. */
                }
            }

            /* Advance data pointer and remaining count. */
            current_data_ptr += bytes_for_this_sector;
            data_remaining -= bytes_for_this_sector;

            /* Update DEB fields and state for next iteration. */
            deb->file_format_dependent2 = current_sector_lba; /* LBA of the last sector written. */
            deb->record_count++;
            prev_sector_lba = current_sector_lba;
        }
        /* The loop ensures the last sector written already has its link field as 0
        // because next_link_lba_placeholder was 0 and wasn't overwritten by a subsequent iteration's link update.
        */

        /* Update DEB block_count based on unique 1K blocks used. */
        /* tracker.count should reflect unique 1K blocks because we only add to tracker when a new block is allocated. */
        if (tracker.count > UINT16_MAX) { /* This should have been caught earlier by add_tracked_block. */
            fprintf(stderr, "oasis_file_write_data (SEQ): Error - Number of unique 1K blocks (%zu) exceeds DEB capacity (final check).\n", tracker.count);
            rollback_allocations(disk_layout, &tracker); /* Rollback on error. */
            return false; /* Return false if block count exceeds capacity. */
        }
        deb->block_count = (uint16_t)tracker.count;

        free_block_tracker(&tracker); /* Clean up the tracker. */
        return true; /* Return true on success for sequential files. */

    }
    else {
        fprintf(stderr, "oasis_file_write_data: Error - Unsupported file format (0x%02X) for writing.\n", file_format_type);
        return false; /* Return false for unsupported file formats. */
    }
}
