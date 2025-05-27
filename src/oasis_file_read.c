/*
 * oasis_file_read.c - Implementation of OASIS File Reading Utilities
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#include "oasis.h"

#include <stdio.h>      /* For fprintf, perror */
#include <stdlib.h>     /* For malloc, free */
#include <string.h>     /* For strerror (via errno), memcpy */
#include <errno.h>

 /*
  * Implementation of oasis_read_sequential_file
  */
ssize_t oasis_read_sequential_file(const directory_entry_block_t* deb,
    sector_io_stream_t* img_stream, /* Changed parameter type */
    void* caller_buffer,
    size_t buffer_size) {

    uint16_t current_sector_addr;
    uint16_t expected_last_sector;
    uint16_t actual_last_sector_read = 0; /* Track the address of the last sector processed */
    size_t max_sectors;
    uint8_t sector_buffer[SECTOR_SIZE];
    ssize_t bytes_read_total = 0;
    size_t sectors_processed = 0;
    bool buffer_full_warning = false;
    ssize_t sectors_read_from_io; /* To store result from sector_io_read */

    /* --- Input Validation --- */
    if (deb == NULL) {
        fprintf(stderr, "Error: DEB pointer is NULL.\n");
        errno = EINVAL;
        return -1;
    }
    if (caller_buffer == NULL && buffer_size > 0) {
        fprintf(stderr, "Error: Caller buffer is NULL but buffer_size > 0.\n");
        errno = EINVAL;
        return -1;
    }
    if (img_stream == NULL) {
        fprintf(stderr, "Error: Invalid image stream (NULL).\n");
        errno = EINVAL; /* Or EBADF for "bad file object" */
        return -1;
    }

    /* Check if the file format is Sequential */
    if ((deb->file_format & FILE_FORMAT_MASK) != FILE_FORMAT_SEQUENTIAL) {
        fprintf(stderr, "Error: DEB does not specify a Sequential file (format: 0x%02X).\n", deb->file_format);
        errno = EINVAL;
        return -1;
    }

    /* Assume DEB fields are little-endian as per oasis.h comments. */
    current_sector_addr = deb->start_sector;
    expected_last_sector = deb->file_format_dependent2;

    /* Calculate max sectors based on allocated blocks */
    /* block_count is number of 1K blocks. 1 block = 4 sectors. */
    max_sectors = (size_t)deb->block_count * (BLOCK_SIZE / SECTOR_SIZE);


    /* Handle empty file case: If start sector is 0, the file is empty. */
    if (current_sector_addr == 0) {
        /* For consistency, check if last sector is also 0 */
        if (expected_last_sector != 0) {
            fprintf(stderr, "Warning: Empty file (start_sector=0) but DEB expected last sector %u.\n",
                expected_last_sector);
            /* Not necessarily fatal, proceed returning 0 bytes read */
        }
        if (deb->record_count != 0) {
            fprintf(stderr, "Warning: Empty file (start_sector=0) but DEB record count is %u.\n",
                deb->record_count);
        }
        return 0; /* 0 bytes read for an empty file */
    }

    /* --- Sector Reading Loop --- */
    while (current_sector_addr != 0) {
        sectors_processed++;

        /* Validation: Check sector count against allocation */
        if ((deb->block_count == 0 && sectors_processed > 0) || /* If DEB says 0 blocks, any sector is too many */
            (deb->block_count > 0 && sectors_processed > max_sectors)) { /* Otherwise, check against calculated max */
            fprintf(stderr, "Error: File sector chain (%zu sectors) inconsistent with DEB block_count %u (max_sectors: %zu).\n",
                sectors_processed, deb->block_count, max_sectors);
            errno = EIO; /* Link corruption or DEB inconsistency */
            return -1;
        }

        /* Store the address of the sector we are about to read/process */
        actual_last_sector_read = current_sector_addr;

        /* Read the entire sector using the new I/O layer */
        sectors_read_from_io = sector_io_read(img_stream, current_sector_addr, 1, sector_buffer);

        if (sectors_read_from_io != 1) {
            /* sector_io_read prints its own errors, but we add context here */
            fprintf(stderr, "Error: Failed to read sector %u for sequential file.\n", current_sector_addr);
            errno = EIO; /* Set a general I/O error */
            return -1;
        }


        /* Extract link (last 2 bytes, assuming little-endian) */
        uint16_t next_sector_addr_le;
        memcpy(&next_sector_addr_le, sector_buffer + OASIS_SEQ_DATA_PER_SECTOR, sizeof(uint16_t));
        uint16_t next_sector_addr = next_sector_addr_le; /* Assuming LE host */

        /* Copy data portion to caller buffer if space available */
        size_t bytes_remaining_in_buffer = buffer_size - (size_t)bytes_read_total;
        size_t bytes_to_copy = (bytes_remaining_in_buffer < OASIS_SEQ_DATA_PER_SECTOR)
            ? bytes_remaining_in_buffer
            : OASIS_SEQ_DATA_PER_SECTOR;

        if (bytes_to_copy > 0) {
            memcpy((uint8_t*)caller_buffer + bytes_read_total, sector_buffer, bytes_to_copy);
            bytes_read_total += (ssize_t)bytes_to_copy;
        }

        /* Update current_sector_addr for the next iteration */
        current_sector_addr = next_sector_addr;

        /* Check if buffer became full during this iteration */
        if (bytes_remaining_in_buffer <= OASIS_SEQ_DATA_PER_SECTOR) {
            /* If the buffer is full, but we haven't reached the end of the chain */
            if (current_sector_addr != 0) {
                buffer_full_warning = true;
            }
            /* Break the loop regardless, as we can't store more data */
            break;
        }
    } /* --- End of Sector Reading Loop --- */

    /* --- Post-Loop Validation --- */

    /* If the loop terminated naturally (link went to 0), validate the last sector */
    if (current_sector_addr == 0) {
        if (actual_last_sector_read != expected_last_sector) {
            fprintf(stderr, "Error: Last sector mismatch. Chain ended at sector %u, but DEB expected %u.\n",
                actual_last_sector_read, expected_last_sector);
            errno = EIO; /* Inconsistent filesystem state */
            return -1;
        }
        /* If we get here, the chain ended correctly and matched the DEB */
    }
    else {
        /* Loop terminated early because the buffer was full. */
        /* We cannot validate the final link or last sector address in this case. */
        /* Issue a warning if the flag was set. */
        if (buffer_full_warning) {
            fprintf(stderr, "Warning: Caller buffer filled before reaching end of file chain (last link read pointed to sector %u).\n", current_sector_addr);
        }
    }

    /* Return the total number of bytes actually copied to the buffer */
    return bytes_read_total;
}


bool oasis_file_read_data(sector_io_stream_t* img_stream,
    const directory_entry_block_t* deb,
    uint8_t** file_buffer_ptr,
    ssize_t* bytes_read_ptr) {
    uint8_t file_format_type;
    size_t actual_data_read_from_disk = 0;
    size_t logical_file_size = 0;
    uint8_t* temp_full_block_buffer = NULL;


    /* Initialize output parameters */
    if (file_buffer_ptr == NULL || bytes_read_ptr == NULL) {
        return false;
    }
    *file_buffer_ptr = NULL;
    *bytes_read_ptr = -1;

    if (img_stream == NULL || deb == NULL) {
        fprintf(stderr, "oasis_file_read_data: Error - NULL img_stream or deb.\n");
        return false;
    }

    if (!oasis_deb_is_valid(deb)) {
        fprintf(stderr, "oasis_file_read_data: Info - DEB is not valid (e.g. empty/deleted). Reporting 0 bytes read.\n");
        *bytes_read_ptr = 0;
        return true;
    }

    file_format_type = deb->file_format & FILE_FORMAT_MASK;

    /* Handle empty files (0 blocks allocated) */
    if (deb->block_count == 0) {
        if (file_format_type == FILE_FORMAT_SEQUENTIAL && deb->start_sector != 0) {
            fprintf(stderr, "oasis_file_read_data: Warning - Sequential DEB has 0 blocks but non-zero start sector %u.\n", deb->start_sector);
        }
        *bytes_read_ptr = 0;
        return true;
    }

    if (file_format_type == FILE_FORMAT_SEQUENTIAL) {
        /* For sequential files, the buffer allocation is based on block_count,
         * but the actual data read follows the chain and might be less.
         */
        size_t max_buffer_alloc_size = (size_t)deb->block_count * BLOCK_SIZE;
        if (max_buffer_alloc_size == 0 && deb->block_count > 0) { /* Should not happen with BLOCK_SIZE > 0 */
            fprintf(stderr, "oasis_file_read_data: Internal error - block_count > 0 but max_buffer_alloc_size is 0 for sequential.\n");
            return false;
        }
        *file_buffer_ptr = (uint8_t*)malloc(max_buffer_alloc_size > 0 ? max_buffer_alloc_size : 1 /*alloc min 1 byte if 0*/);
        if (!(*file_buffer_ptr)) {
            perror("oasis_file_read_data: Error allocating buffer for sequential file content");
            return false;
        }

        *bytes_read_ptr = oasis_read_sequential_file(deb, img_stream, *file_buffer_ptr, max_buffer_alloc_size);

        if (*bytes_read_ptr < 0) {
            fprintf(stderr, "oasis_file_read_data: Error reading sequential file.\n");
            free(*file_buffer_ptr);
            *file_buffer_ptr = NULL;
            return false;
        }
        /* If *bytes_read_ptr is less than max_buffer_alloc_size, we might want to realloc
         * down to the actual size, or the caller can use *bytes_read_ptr.
         * For now, leave the buffer as allocated; the caller knows the true length.
         */
    }
    else { /* Contiguous file types (Direct, Absolute, Relocatable, Indexed, Keyed) */
        uint16_t num_sectors_to_read_from_disk = deb->block_count * (BLOCK_SIZE / SECTOR_SIZE);
        ssize_t sectors_actually_read;
        size_t disk_read_buffer_size = (size_t)num_sectors_to_read_from_disk * SECTOR_SIZE;

        if (num_sectors_to_read_from_disk == 0) {
            /* This case should be covered by deb->block_count == 0 check earlier */
            fprintf(stderr, "oasis_file_read_data: Internal inconsistency - Contiguous file with block_count > 0 but num_sectors_to_read_from_disk is 0.\n");
            return false;
        }

        temp_full_block_buffer = (uint8_t*)malloc(disk_read_buffer_size);
        if (!temp_full_block_buffer) {
            perror("oasis_file_read_data: Error allocating temporary buffer for contiguous file read");
            return false;
        }

        sectors_actually_read = sector_io_read(img_stream, deb->start_sector, num_sectors_to_read_from_disk, temp_full_block_buffer);

        if (sectors_actually_read < 0) {
            fprintf(stderr, "oasis_file_read_data: Error reading contiguous file content via sector_io_read for %u sectors from LBA %u.\n", num_sectors_to_read_from_disk, deb->start_sector);
            free(temp_full_block_buffer);
            return false;
        }
        if ((uint16_t)sectors_actually_read != num_sectors_to_read_from_disk) {
            fprintf(stderr, "oasis_file_read_data: Error - Contiguous file read mismatch. Expected %u sectors, got %zd for LBA %u.\n",
                num_sectors_to_read_from_disk, sectors_actually_read, deb->start_sector);
            free(temp_full_block_buffer);
            *bytes_read_ptr = (ssize_t)sectors_actually_read * SECTOR_SIZE;
            return false;
        }
        actual_data_read_from_disk = (size_t)sectors_actually_read * SECTOR_SIZE;

        /* Determine logical file size */
        switch (file_format_type) {
        case FILE_FORMAT_DIRECT:
            logical_file_size = (size_t)deb->record_count * deb->file_format_dependent1;
            break;
        case FILE_FORMAT_INDEXED:
        case FILE_FORMAT_KEYED:
            logical_file_size = (size_t)deb->record_count * (deb->file_format_dependent1 & 0x1FF);
            break;
        case FILE_FORMAT_RELOCATABLE:
            logical_file_size = (size_t)deb->file_format_dependent2; /* Program Length */
            break;
        case FILE_FORMAT_ABSOLUTE:
        default: /* Includes ABSOLUTE and any other unanticipated contiguous types */
            logical_file_size = actual_data_read_from_disk;
            break;
        }

        /* Ensure logical_file_size does not exceed actual_data_read_from_disk */
        if (logical_file_size > actual_data_read_from_disk) {
            fprintf(stderr, "oasis_file_read_data: Warning - Logical file size (%zu bytes) for DEB '%s.%s' "
                "exceeds data read from disk based on block_count (%zu bytes). Using disk read size.\n",
                logical_file_size, deb->file_name, deb->file_type, actual_data_read_from_disk);
            logical_file_size = actual_data_read_from_disk;
        }
        if (logical_file_size == 0 && deb->block_count > 0) {
            /* If logical size calculates to 0 but blocks are allocated,
             * it might be a program file where ffd's aren't used for size,
             * or a data file with 0 records. Default to block size then.
             */
            fprintf(stderr, "oasis_file_read_data: Warning - Logical file size calculated to 0 for '%s.%s' "
                "but block_count is %u. Defaulting to full block size read (%zu bytes).\n",
                deb->file_name, deb->file_type, deb->block_count, actual_data_read_from_disk);
            logical_file_size = actual_data_read_from_disk;
        }


        *file_buffer_ptr = (uint8_t*)malloc(logical_file_size > 0 ? logical_file_size : 1);
        if (!(*file_buffer_ptr)) {
            perror("oasis_file_read_data: Error allocating final buffer for contiguous file");
            free(temp_full_block_buffer);
            return false;
        }

        if (logical_file_size > 0) {
            memcpy(*file_buffer_ptr, temp_full_block_buffer, logical_file_size);
        }
        *bytes_read_ptr = (ssize_t)logical_file_size;
        free(temp_full_block_buffer);
    }
    return true;
}
