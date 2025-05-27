/*
 * oasis_file_rename.c - Implementation of OASIS File Renaming Utilities
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#include "oasis.h" /* For core definitions, including FILE_FORMAT_MASK, etc. */

#include <stdio.h>  /* For fprintf, stderr, printf */
#include <string.h> /* For memset, strncpy, strlen, strrchr, toupper */
#include <ctype.h>  /* For toupper */

/*
 * Writes the directory to disk.
 * Converts uint16_t fields in each DEB to little-endian before writing.
 * This is a local helper, similar to one that might be in oasis_file_erase.c or a common utils_write.c
 */
static bool write_directory_to_disk_rename(sector_io_stream_t* img_stream, oasis_disk_layout_t* disk_layout) {
    uint32_t dir_start_sector_lba;
    size_t num_debs;
    uint8_t* le_dir_buffer = NULL; /* Buffer for little-endian directory data */

    if (!img_stream || !disk_layout || !disk_layout->directory) {
        fprintf(stderr, "write_directory_to_disk_rename: Invalid arguments.\n");
        return false;
    }
    if (disk_layout->fsblock.dir_sectors_max == 0 || disk_layout->directory->directory_size_bytes == 0) {
        /* This case might be valid if the directory is truly empty and no sectors are allocated for it.
           However, if dir_sectors_max > 0 but directory_size_bytes is 0, it's an inconsistency.
           For rename, we expect a directory to exist if files are being renamed.
        */
        fprintf(stderr, "write_directory_to_disk_rename: Directory is empty or not configured for writing.\n");
        return false;
    }

    dir_start_sector_lba = 1 + 1 + (disk_layout->fsblock.fs_flags & ADDITIONAL_AM_SECTORS_MASK);
    num_debs = disk_layout->directory->directory_size_bytes / sizeof(directory_entry_block_t);

    le_dir_buffer = (uint8_t*)malloc(disk_layout->directory->directory_size_bytes);
    if (!le_dir_buffer) {
        perror("write_directory_to_disk_rename: Failed to allocate buffer for LE directory");
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


bool oasis_rename_single_file_deb(directory_entry_block_t* deb_to_rename, /* host byte order */
    const char* new_fname_part,
    const char* new_ftype_part) {
    if (!deb_to_rename || !new_fname_part || !new_ftype_part) {
        fprintf(stderr, "oasis_rename_single_file_deb: Error - Invalid arguments (NULL DEB or name parts).\n");
        return false;
    }

    /* Update DEB with new name and type, ensuring space padding */
    memset(deb_to_rename->file_name, ' ', FNAME_LEN);
    /* strncpy might not null-terminate if src is >= FNAME_LEN.
       Since we pre-fill with spaces, this is okay for DEB format.
       The length for strncpy should be the minimum of actual string length and FNAME_LEN.
    */
    size_t len_to_copy_fname = strlen(new_fname_part);
    if (len_to_copy_fname > FNAME_LEN) len_to_copy_fname = FNAME_LEN;
    strncpy(deb_to_rename->file_name, new_fname_part, len_to_copy_fname);


    memset(deb_to_rename->file_type, ' ', FTYPE_LEN);
    size_t len_to_copy_ftype = strlen(new_ftype_part);
    if (len_to_copy_ftype > FTYPE_LEN) len_to_copy_ftype = FTYPE_LEN;
    strncpy(deb_to_rename->file_type, new_ftype_part, len_to_copy_ftype);

    /* Timestamp is preserved. Endianness of uint16_t fields is handled when writing to disk. */
    return true;
}

bool oasis_rename_file_by_pattern_and_name(sector_io_stream_t* img_stream,
    oasis_disk_layout_t* disk_layout, /* in host byte order */
    const cli_options_t* options,
    const char* new_filename_str) {
    size_t num_entries;
    int files_matched_count = 0;
    directory_entry_block_t* deb_to_rename_ptr = NULL;
    size_t deb_to_rename_idx = 0; /* To avoid checking the file against itself for new name collision */
    char old_host_filename[MAX_HOST_FILENAME_LEN];

    char new_fname_part_parsed[FNAME_LEN + 1] = { 0 };
    char new_ftype_part_parsed[FTYPE_LEN + 1] = { 0 };
    const char* dot_in_new_name;
    size_t len_new_name_part;
    size_t len_new_type_part;
    char* p;
    size_t i;

    /* For matching the old name from pattern */
    char old_fname_target[FNAME_LEN + 1] = { 0 };
    char old_ftype_target[FTYPE_LEN + 1] = { 0 };
    const char* dot_in_old_name;

    /* For duplicate check with new name (space padded) */
    char padded_new_fname_for_check[FNAME_LEN];
    char padded_new_ftype_for_check[FTYPE_LEN];


    if (!disk_layout || !disk_layout->directory || !img_stream || !options || !options->pattern || !new_filename_str) {
        fprintf(stderr, "oasis_rename_file_by_pattern_and_name: Error - Invalid arguments.\n");
        return false;
    }

    /* Parse the old filename from options->pattern into name and type parts */
    dot_in_old_name = strrchr(options->pattern, '.');
    if (dot_in_old_name != NULL) {
        len_new_name_part = dot_in_old_name - options->pattern; /* Use len_new_name_part temporarily */
        if (len_new_name_part > FNAME_LEN) {
            fprintf(stderr, "Error: Old filename part '%.*s' is longer than %d characters.\n", (int)len_new_name_part, options->pattern, FNAME_LEN);
            return false;
        }
        strncpy(old_fname_target, options->pattern, len_new_name_part);
        old_fname_target[len_new_name_part] = '\0';

        len_new_type_part = strlen(dot_in_old_name + 1); /* Use len_new_type_part temporarily */
        if (len_new_type_part > FTYPE_LEN) {
            fprintf(stderr, "Error: Old filetype part '%s' is longer than %d characters.\n", dot_in_old_name + 1, FTYPE_LEN);
            return false;
        }
        strcpy(old_ftype_target, dot_in_old_name + 1);
    }
    else {
        len_new_name_part = strlen(options->pattern);
        if (len_new_name_part > FNAME_LEN) {
            fprintf(stderr, "Error: Old filename '%s' (no extension) is longer than %d characters.\n", options->pattern, FNAME_LEN);
            return false;
        }
        strcpy(old_fname_target, options->pattern);
    }
    for (p = old_fname_target; *p; ++p) *p = (char)toupper((unsigned char)*p);
    for (p = old_ftype_target; *p; ++p) *p = (char)toupper((unsigned char)*p);

    /* Parse the new filename string */
    num_entries = disk_layout->directory->directory_size_bytes / sizeof(directory_entry_block_t);
    dot_in_new_name = strrchr(new_filename_str, '.');

    if (dot_in_new_name != NULL) {
        len_new_name_part = dot_in_new_name - new_filename_str;
        if (len_new_name_part > FNAME_LEN) {
            fprintf(stderr, "Error: New filename part '%.*s' is longer than %d characters.\n", (int)len_new_name_part, new_filename_str, FNAME_LEN);
            return false;
        }
        strncpy(new_fname_part_parsed, new_filename_str, len_new_name_part);
        new_fname_part_parsed[len_new_name_part] = '\0';

        len_new_type_part = strlen(dot_in_new_name + 1);
        if (len_new_type_part > FTYPE_LEN) {
            fprintf(stderr, "Error: New filetype part '%s' is longer than %d characters.\n", dot_in_new_name + 1, FTYPE_LEN);
            return false;
        }
        strcpy(new_ftype_part_parsed, dot_in_new_name + 1);
    }
    else {
        len_new_name_part = strlen(new_filename_str);
        if (len_new_name_part > FNAME_LEN) {
            fprintf(stderr, "Error: New filename '%s' (no extension) is longer than %d characters.\n", new_filename_str, FNAME_LEN);
            return false;
        }
        strcpy(new_fname_part_parsed, new_filename_str);
        /* new_ftype_part_parsed remains empty (all zeros) */
    }

    for (p = new_fname_part_parsed; *p; ++p) *p = (char)toupper((unsigned char)*p);
    for (p = new_ftype_part_parsed; *p; ++p) *p = (char)toupper((unsigned char)*p);

    printf("oasis_rename_file_by_pattern_and_name: Searching for file '%s.%s' for User ID %d to rename to '%s.%s'...\n",
        old_fname_target, old_ftype_target, options->owner_id_filter, new_fname_part_parsed, new_ftype_part_parsed);

    /* Find the file to rename */
    for (i = 0; i < num_entries; ++i) {
        directory_entry_block_t* deb_entry = &disk_layout->directory->directory[i]; /* Host order */
        /* Use oasis_filename_wildcard_match as it handles DEB's space padding and case insensitivity */
        if (oasis_deb_is_valid(deb_entry) &&
            (options->owner_id_filter == OWNER_ID_WILDCARD || deb_entry->owner_id == options->owner_id_filter) &&
            oasis_filename_wildcard_match(deb_entry->file_name, deb_entry->file_type, options->pattern)) {

            if (files_matched_count == 0) {
                deb_to_rename_ptr = deb_entry;
                deb_to_rename_idx = i;
                oasis_deb_to_host_filename(deb_entry, old_host_filename, sizeof(old_host_filename));
                printf("  Found file to rename: %s (DEB #%zu)\n", old_host_filename, i);
            }
            else {
                fprintf(stderr, "  Error: Pattern '%s' matches multiple files for user ID %d. Rename requires a unique match.\n",
                    options->pattern, options->owner_id_filter);
                return false;
            }
            files_matched_count++;
        }
    }

    if (!deb_to_rename_ptr) {
        printf("  No file found matching pattern '%s' for user ID %d to rename.\n", options->pattern, options->owner_id_filter);
        return true; /* No match is not an error */
    }

    /* Prepare space-padded new name/type for exact DEB comparison */
    memset(padded_new_fname_for_check, ' ', FNAME_LEN);
    strncpy(padded_new_fname_for_check, new_fname_part_parsed, strlen(new_fname_part_parsed));
    memset(padded_new_ftype_for_check, ' ', FTYPE_LEN);
    strncpy(padded_new_ftype_for_check, new_ftype_part_parsed, strlen(new_ftype_part_parsed));

    /* Check if the new name already exists */
    for (i = 0; i < num_entries; ++i) {
        const directory_entry_block_t* deb_entry = &disk_layout->directory->directory[i]; /* Host order */
        if (i == deb_to_rename_idx || !oasis_deb_is_valid(deb_entry)) {
            continue;
        }
        if (options->owner_id_filter != OWNER_ID_WILDCARD && deb_entry->owner_id != deb_to_rename_ptr->owner_id) {
            continue; /* Only check for duplicates under the same owner if not wildcard */
        }

        if (memcmp(deb_entry->file_name, padded_new_fname_for_check, FNAME_LEN) == 0 &&
            memcmp(deb_entry->file_type, padded_new_ftype_for_check, FTYPE_LEN) == 0) {
            fprintf(stderr, "  Error: New filename '%s.%s' already exists (DEB #%zu, Owner ID %d).\n",
                new_fname_part_parsed, new_ftype_part_parsed, i, deb_entry->owner_id);
            return false;
        }
    }

    if (!oasis_rename_single_file_deb(deb_to_rename_ptr, new_fname_part_parsed, new_ftype_part_parsed)) {
        return false;
    }

    /* Write the modified directory (which is in host byte order) back to disk */
    if (!write_directory_to_disk_rename(img_stream, disk_layout)) {
        fprintf(stderr, "  Error: Failed to write updated directory to disk image after rename.\n");
        /* TODO: Consider rollback of in-memory DEB change if disk write fails. */
        return false;
    }

    printf("  File '%s' successfully renamed to '%s.%s'.\n", old_host_filename, new_fname_part_parsed, new_ftype_part_parsed);
    return true;
}
