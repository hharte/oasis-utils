/*
 * oasis_file_copy.c - Implementation of OASIS File Copying Utilities
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#include "oasis_file_copy.h"
#include "oasis.h" /* Core definitions, includes other necessary oasis headers */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h> /* For stat to get file modification time */
#include <time.h>     /* For time_t, struct tm, localtime */


 /*
  * Writes the filesystem block and allocation map (initial part) to Sector 1.
  * Converts uint16_t fields in fsblock to little-endian before writing.
  * (Local helper, similar to one in oasis_file_erase.c)
  */
static bool write_fsblock_and_initial_am_copy(sector_io_stream_t* img_stream, oasis_disk_layout_t* disk_layout) {
    uint8_t sector1_buffer[SECTOR_SIZE];
    filesystem_block_t fs_block_le; /* Temporary LE version */
    size_t map_in_sector1;

    if (!img_stream || !disk_layout || !disk_layout->alloc_map.map_data) {
        fprintf(stderr, "write_fsblock_and_initial_am_copy: Invalid arguments.\n");
        return false;
    }

    memcpy(&fs_block_le, &disk_layout->fsblock, sizeof(filesystem_block_t));
    fs_block_le.reserved = htole16(disk_layout->fsblock.reserved);
    fs_block_le.free_blocks = htole16(disk_layout->fsblock.free_blocks);

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
        fprintf(stderr, "Error: Failed to write updated FS block and initial AM to Sector 1 (copy op).\n");
        return false;
    }
    printf("Info: Successfully wrote updated FS Block/AM (Sector 1) (copy op).\n");
    return true;
}

/*
 * Writes additional allocation map sectors (if any) to disk.
 * (Local helper, similar to one in oasis_file_erase.c)
 */
static bool write_additional_am_sectors_copy(sector_io_stream_t* img_stream, oasis_disk_layout_t* disk_layout) {
    uint32_t additional_am_sectors_count;
    size_t map_in_sector1;

    if (!img_stream || !disk_layout) return true; /* Not an error if no disk_layout */
    if (!(disk_layout->fsblock.fs_flags & ADDITIONAL_AM_SECTORS_MASK)) return true; /* No additional sectors */
    if (!disk_layout->alloc_map.map_data) {
        fprintf(stderr, "write_additional_am_sectors_copy: map_data is NULL but additional AM sectors are indicated.\n");
        return false;
    }


    additional_am_sectors_count = disk_layout->fsblock.fs_flags & ADDITIONAL_AM_SECTORS_MASK;
    if (additional_am_sectors_count > 0) {
        map_in_sector1 = SECTOR_SIZE - sizeof(filesystem_block_t);
        if (disk_layout->alloc_map.map_size_bytes > map_in_sector1) {
            if (sector_io_write(img_stream, 2, additional_am_sectors_count, disk_layout->alloc_map.map_data + map_in_sector1) != (ssize_t)additional_am_sectors_count) {
                fprintf(stderr, "Error: Failed to write additional allocation map sectors (copy op).\n");
                return false;
            }
            printf("Info: Successfully wrote updated additional AM sectors (copy op).\n");
        }
    }
    return true;
}

/*
 * Writes the directory to disk.
 * Converts uint16_t fields in each DEB to little-endian before writing.
 * (Local helper, similar to one in oasis_file_erase.c)
 */
static bool write_directory_to_disk_copy(sector_io_stream_t* img_stream, oasis_disk_layout_t* disk_layout) {
    uint32_t dir_start_sector_lba;
    size_t num_debs;
    uint8_t* le_dir_buffer = NULL;

    if (!img_stream || !disk_layout || !disk_layout->directory) {
        fprintf(stderr, "write_directory_to_disk_copy: Invalid arguments.\n");
        return false;
    }
    if (disk_layout->fsblock.dir_sectors_max == 0 || disk_layout->directory->directory_size_bytes == 0) {
        return true;
    }

    dir_start_sector_lba = 1 + 1 + (disk_layout->fsblock.fs_flags & ADDITIONAL_AM_SECTORS_MASK);
    num_debs = disk_layout->directory->directory_size_bytes / sizeof(directory_entry_block_t);

    le_dir_buffer = (uint8_t*)malloc(disk_layout->directory->directory_size_bytes);
    if (!le_dir_buffer) {
        perror("write_directory_to_disk_copy: Failed to allocate buffer for LE directory");
        return false;
    }

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
        fprintf(stderr, "Error: Failed to write updated directory to disk image (copy op).\n");
        free(le_dir_buffer);
        return false;
    }
    free(le_dir_buffer);
    printf("Info: Successfully wrote updated directory to disk (copy op).\n");
    return true;
}


/* Helper to get basename from a path */
static const char* get_path_basename_cf(const char* path) {
    const char* basename_ptr = path;
    const char* last_sep = NULL;
    char current_char;

    if (path == NULL) return NULL;

    while ((current_char = *path) != '\0') {
        if (current_char == '/' || current_char == '\\') {
            last_sep = path;
        }
        path++;
    }
    if (last_sep != NULL) {
        basename_ptr = last_sep + 1;
    }
    return basename_ptr;
}

bool oasis_copy_host_file_to_disk(sector_io_stream_t* img_stream,
    oasis_disk_layout_t* disk_layout, /* In host byte order */
    const char* host_filepath,
    const char* oasis_filename_override,
    const cli_options_t* options) {
    FILE* host_file_stream = NULL;
    uint8_t* host_file_buffer = NULL;
    uint8_t* data_to_write = NULL;
    long host_file_size = 0;
    size_t bytes_read_from_host = 0;
    size_t data_size_to_write = 0;
    directory_entry_block_t target_deb; /* Will be in host byte order */
    directory_entry_block_t* existing_deb_to_overwrite = NULL;
    size_t existing_deb_idx = 0;
    bool success = false;
    bool ascii_converted = false;
    uint16_t num_1k_blocks_needed;
    size_t i;
    struct stat host_stat_info;
    time_t host_mod_time;
    struct tm* host_tm_info;
    char actual_oasis_name_for_msg[MAX_FNAME_FTYPE_LEN];


    if (!img_stream || !disk_layout || !host_filepath || !options) {
        fprintf(stderr, "oasis_copy_host_file_to_disk: Error - NULL arguments.\n");
        return false;
    }

    host_file_stream = fopen(host_filepath, "rb");
    if (!host_file_stream) {
        fprintf(stderr, "Error: Cannot open host file '%s': %s\n", host_filepath, strerror(errno));
        return false;
    }

    if (fseek(host_file_stream, 0, SEEK_END) != 0 || (host_file_size = ftell(host_file_stream)) < 0 || fseek(host_file_stream, 0, SEEK_SET) != 0) {
        fprintf(stderr, "Error: Cannot determine size of host file '%s': %s\n", host_filepath, strerror(errno));
        fclose(host_file_stream);
        return false;
    }

    if (host_file_size == 0) {
        data_to_write = NULL;
        data_size_to_write = 0;
    }
    else {
        host_file_buffer = (uint8_t*)malloc(host_file_size);
        if (!host_file_buffer) {
            perror("Error allocating buffer for host file content");
            fclose(host_file_stream);
            return false;
        }
        bytes_read_from_host = fread(host_file_buffer, 1, host_file_size, host_file_stream);
        if (bytes_read_from_host != (size_t)host_file_size) {
            fprintf(stderr, "Error: Failed to read entire host file '%s'. Read %zu of %ld bytes.\n", host_filepath, bytes_read_from_host, host_file_size);
            fclose(host_file_stream);
            free(host_file_buffer);
            return false;
        }
        data_to_write = host_file_buffer;
        data_size_to_write = bytes_read_from_host;
    }
    fclose(host_file_stream);
    host_file_stream = NULL;


    memset(&target_deb, 0, sizeof(target_deb));
    if (oasis_filename_override) {
        if (!host_filename_to_oasis_deb(oasis_filename_override, &target_deb)) {
            fprintf(stderr, "Error: Invalid OASIS filename override format: '%s'.\n", oasis_filename_override);
            if (data_to_write && data_to_write == host_file_buffer) {
                if (host_file_buffer) free(host_file_buffer);
            }
            else if (data_to_write) {
                free(data_to_write);
            }
            return false;
        }
    }
    else {
        const char* host_basename = get_path_basename_cf(host_filepath);
        if (!host_basename || !host_filename_to_oasis_deb(host_basename, &target_deb)) {
            fprintf(stderr, "Error: Could not derive OASIS filename from host file: '%s'.\n", host_filepath);
            if (data_to_write && data_to_write == host_file_buffer) {
                if (host_file_buffer) free(host_file_buffer);
            }
            else if (data_to_write) {
                free(data_to_write);
            }
            return false;
        }
    }

    target_deb.owner_id = (options->owner_id_filter == OWNER_ID_WILDCARD) ? 0 : (uint8_t)options->owner_id_filter;

    if (stat(host_filepath, &host_stat_info) == 0) {
        host_mod_time = host_stat_info.st_mtime;
        host_tm_info = localtime(&host_mod_time);
        if (host_tm_info) {
            oasis_convert_tm_to_timestamp(host_tm_info, &target_deb.timestamp);
        }
    }
    else { /* Fallback to current time if stat fails */
        time_t current_time; time(&current_time);
        host_tm_info = localtime(&current_time);
        if (host_tm_info) oasis_convert_tm_to_timestamp(host_tm_info, &target_deb.timestamp);
    }

    if (options->ascii_conversion && data_size_to_write > 0) {
        if (is_ascii(data_to_write, data_size_to_write)) {
            conversion_result_t conv_res;
            size_t estimated_oasis_capacity = data_size_to_write + 1; /* For potential SUB char */
            uint8_t* converted_ascii_buffer = (uint8_t*)malloc(estimated_oasis_capacity);
            int32_t converted_len;

            if (!converted_ascii_buffer) {
                perror("Error allocating buffer for ASCII conversion");
                if (data_to_write == host_file_buffer && host_file_buffer != NULL) free(host_file_buffer);
                else if (data_to_write != NULL) free(data_to_write);
                return false;
            }

            converted_len = ascii_host_to_oasis(data_to_write, data_size_to_write, converted_ascii_buffer, estimated_oasis_capacity, &conv_res);

            if (converted_len < 0) {
                fprintf(stderr, "Error: ASCII conversion from host to OASIS failed for '%s' (Code %d).\n", host_filepath, converted_len);
                if (data_to_write == host_file_buffer && host_file_buffer != NULL) free(host_file_buffer);
                else if (data_to_write != NULL) free(data_to_write);
                free(converted_ascii_buffer);
                return false;
            }

            if (host_file_buffer && data_to_write == host_file_buffer) { /* If data_to_write was pointing to original buffer */
                free(host_file_buffer);
                host_file_buffer = NULL;
            }
            else if (data_to_write != host_file_buffer && data_to_write != NULL) { /* If data_to_write was already a different buffer (should not happen here) */
                free(data_to_write);
            }

            data_to_write = converted_ascii_buffer;
            data_size_to_write = (size_t)converted_len;
            ascii_converted = true;
            target_deb.file_format = (target_deb.file_format & FILE_ATTRIBUTE_MASK) | FILE_FORMAT_SEQUENTIAL;
            if (target_deb.file_format_dependent1 == 0 && conv_res.max_line_len > 0 && conv_res.max_line_len <= UINT16_MAX) {
                target_deb.file_format_dependent1 = (uint16_t)conv_res.max_line_len;
            }
            else if (target_deb.file_format_dependent1 == 0 && data_size_to_write > 0) {
                target_deb.file_format_dependent1 = SECTOR_SIZE; /* Default record length for ASCII */
            }
        }
    }

    oasis_deb_get_fname_ftype(&target_deb, actual_oasis_name_for_msg, sizeof(actual_oasis_name_for_msg));
    printf("Info: Target OASIS file: %s, User: %d, Format: 0x%02X\n",
        actual_oasis_name_for_msg, target_deb.owner_id, target_deb.file_format);

    if (ascii_converted && (target_deb.file_format & FILE_FORMAT_MASK) == FILE_FORMAT_SEQUENTIAL && data_size_to_write > 0) {
        if (data_to_write[data_size_to_write - 1] != SUB) {
            uint8_t* temp_buffer_for_sub = (uint8_t*)realloc(data_to_write, data_size_to_write + 1);
            if (!temp_buffer_for_sub) {
                perror("Error reallocating for SUB character");
                free(data_to_write);
                return false;
            }
            data_to_write = temp_buffer_for_sub;
            data_to_write[data_size_to_write] = SUB;
            data_size_to_write++;
        }
    }

    for (i = 0; i < disk_layout->directory->directory_size_bytes / sizeof(directory_entry_block_t); ++i) {
        directory_entry_block_t* current_disk_deb = &disk_layout->directory->directory[i]; /* Host order */
        if (oasis_deb_is_valid(current_disk_deb) &&
            current_disk_deb->owner_id == target_deb.owner_id &&
            strncmp(current_disk_deb->file_name, target_deb.file_name, FNAME_LEN) == 0 &&
            strncmp(current_disk_deb->file_type, target_deb.file_type, FTYPE_LEN) == 0) {
            existing_deb_to_overwrite = current_disk_deb;
            existing_deb_idx = i;
            printf("Info: Target OASIS file '%s' already exists. Erasing it first.\n", actual_oasis_name_for_msg);
            if (!oasis_erase_single_file(img_stream, disk_layout, existing_deb_to_overwrite, existing_deb_idx)) {
                fprintf(stderr, "Error: Failed to erase existing file '%s'. Aborting copy.\n", actual_oasis_name_for_msg);
                if (data_to_write) free(data_to_write);
                return false;
            }
            /* After erase, the DEB slot is marked deleted/empty and blocks are freed.
               We need to write the updated directory and AM to disk before proceeding with copy.
            */
            if (!write_directory_to_disk_copy(img_stream, disk_layout)) { /* Writes LE directory */
                fprintf(stderr, "Error: Failed to write directory after erasing existing file.\n");
                if (data_to_write) free(data_to_write);
                return false;
            }
            if (!write_fsblock_and_initial_am_copy(img_stream, disk_layout)) { /* Writes LE fsblock */
                fprintf(stderr, "Error: Failed to write FSBlock/AM after erasing existing file.\n");
                if (data_to_write) free(data_to_write);
                return false;
            }
            if (!write_additional_am_sectors_copy(img_stream, disk_layout)) {
                fprintf(stderr, "Error: Failed to write additional AM after erasing existing file.\n");
                if (data_to_write) free(data_to_write);
                return false;
            }
            break;
        }
    }

    num_1k_blocks_needed = (uint16_t)((data_size_to_write + BLOCK_SIZE - 1) / BLOCK_SIZE);
    if (data_size_to_write == 0) num_1k_blocks_needed = 0;

    if (num_1k_blocks_needed > disk_layout->fsblock.free_blocks) {
        fprintf(stderr, "Error: Not enough free space on OASIS disk for '%s'.\n", actual_oasis_name_for_msg);
        fprintf(stderr, "  Needed: %u blocks (for %zu bytes). Available: %u blocks.\n",
            num_1k_blocks_needed, data_size_to_write, disk_layout->fsblock.free_blocks);
        if (data_to_write) free(data_to_write);
        return false;
    }

    directory_entry_block_t* deb_for_new_file = NULL;
    if (existing_deb_to_overwrite && existing_deb_idx < (disk_layout->directory->directory_size_bytes / sizeof(directory_entry_block_t))) {
        /* If we erased, that DEB slot is now marked deleted. We can reuse it. */
        deb_for_new_file = &disk_layout->directory->directory[existing_deb_idx];
        printf("Info: Reusing DEB slot #%zu for '%s'.\n", existing_deb_idx, actual_oasis_name_for_msg);
    }
    if (!deb_for_new_file) { /* Find first empty/deleted slot if not reusing */
        for (i = 0; i < disk_layout->directory->directory_size_bytes / sizeof(directory_entry_block_t); ++i) {
            if (disk_layout->directory->directory[i].file_format == FILE_FORMAT_EMPTY ||
                disk_layout->directory->directory[i].file_format == FILE_FORMAT_DELETED) {
                deb_for_new_file = &disk_layout->directory->directory[i];
                printf("Info: Using empty/deleted DEB slot #%zu for '%s'.\n", i, actual_oasis_name_for_msg);
                break;
            }
        }
    }
    if (!deb_for_new_file) {
        fprintf(stderr, "Error: OASIS disk directory is full. Cannot copy '%s'.\n", actual_oasis_name_for_msg);
        if (data_to_write) free(data_to_write);
        return false;
    }

    /* Populate the chosen DEB slot with target_deb info (which is in host order) */
    memcpy(deb_for_new_file->file_name, target_deb.file_name, FNAME_LEN);
    memcpy(deb_for_new_file->file_type, target_deb.file_type, FTYPE_LEN);
    deb_for_new_file->owner_id = target_deb.owner_id;
    deb_for_new_file->shared_from_owner_id = target_deb.shared_from_owner_id;
    deb_for_new_file->file_format = target_deb.file_format;
    deb_for_new_file->timestamp = target_deb.timestamp;
    deb_for_new_file->file_format_dependent1 = target_deb.file_format_dependent1;
    /* block_count, start_sector, record_count, ffd2 will be set by oasis_file_write_data */
    deb_for_new_file->block_count = 0;
    deb_for_new_file->start_sector = 0;
    deb_for_new_file->record_count = 0;
    deb_for_new_file->file_format_dependent2 = 0;

    printf("Info: Writing OASIS file '%s' (%zu bytes)...\n", actual_oasis_name_for_msg, data_size_to_write);
    if (!oasis_file_write_data(img_stream, disk_layout, deb_for_new_file, data_to_write, data_size_to_write)) {
        fprintf(stderr, "Error: Failed to write data for '%s' to OASIS disk image.\n", actual_oasis_name_for_msg);
        /* If write failed, the DEB slot might have been partially modified or blocks allocated.
           A robust solution would be to mark this DEB as deleted again if it was reused.
           For now, we rely on the fact that oasis_file_write_data should roll back its own allocations on failure.
        */
        if (data_to_write) free(data_to_write);
        return false;
    }
    success = true;
    printf("Info: Successfully wrote data for '%s'.\n", actual_oasis_name_for_msg);

    /* Write updated directory and FSBlock/AM back to disk */
    if (!write_directory_to_disk_copy(img_stream, disk_layout)) success = false;
    if (success && !write_fsblock_and_initial_am_copy(img_stream, disk_layout)) success = false;
    if (success && !write_additional_am_sectors_copy(img_stream, disk_layout)) success = false;

    if (!success) {
        fprintf(stderr, "Error: Failed to write updated disk structures for '%s'.\n", actual_oasis_name_for_msg);
    }

    if (data_to_write) {
        free(data_to_write);
    }
    /* host_file_buffer would have been freed or pointed to by data_to_write then freed */

    return success;
}
