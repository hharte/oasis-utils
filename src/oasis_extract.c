/*
 * oasis_extract.c - Implementation of OASIS Disk Image File Extraction Utilities
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 *
 */

#include "oasis.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>  /* For toupper, isprint */

 /* Platform-specific headers */
#ifdef HAVE_SYS_UTIME_H
#include <sys/utime.h>
#elif defined(HAVE_UTIME_H)
#include <utime.h>
#endif

#ifdef _WIN32
#include <windows.h>
#include <direct.h> /* For _mkdir */
#else
#include <sys/stat.h> /* For mkdir, stat */
#include <sys/types.h> /* For mode_t */
#endif /* _WIN32 */


/* --- Static Helper Function Prototypes --- */
static bool convert_file_if_ascii(const cli_options_t* options,
    uint8_t** current_buffer_ptr,
    size_t* current_size_ptr,
    const char* host_filename,
    bool* was_converted_ptr);
static bool write_buffer_to_host_file(const char* full_file_path,
    const uint8_t* buffer_to_write,
    size_t size_to_write,
    const oasis_tm_t* timestamp);


/* --- File Extraction Function Implementations --- */

bool extract_files(sector_io_stream_t* img_stream,
    const oasis_disk_layout_t* disk_layout,
    const char* output_dir_path,
    const cli_options_t* options) {
    cli_options_t local_options;

    if (options == NULL) {
        /* If original options is NULL, create a default set */
        memset(&local_options, 0, sizeof(cli_options_t));
        local_options.pattern = NULL; /* Extract all */
        local_options.ascii_conversion = false;
        local_options.owner_id_filter = 0; /* Default to user 0 */
    }
    else {
        local_options = *options; /* Copy all options */
        /* If options->pattern was NULL, local_options.pattern will also be NULL,
           which extract_files_matching_pattern handles as "match all".
           owner_id_filter is correctly copied or already set to default if options was NULL.
        */
    }
    /* Ensure wildcard is also considered if options was NULL or didn't specify it */
    if (options == NULL) {
        local_options.owner_id_filter = 0; /* Explicitly default to 0 if original options were NULL */
    }


    return extract_files_matching_pattern(img_stream, disk_layout, output_dir_path, &local_options);
}

bool extract_files_matching_pattern(sector_io_stream_t* img_stream,
    const oasis_disk_layout_t* disk_layout,
    const char* output_dir_path,
    const cli_options_t* options) {
    if (!disk_layout || !img_stream || !options || !output_dir_path) {
        fprintf(stderr, "extract_files_matching_pattern: Error - Invalid arguments (NULL pointer for essential parameter).\n");
        return false;
    }
    if (!disk_layout->directory) {
        fprintf(stderr, "extract_files_matching_pattern: Error - disk_layout->directory is NULL.\n");
        return false;
    }

#ifdef _WIN32
    DWORD path_attrib_efmp = GetFileAttributesA(output_dir_path);
    if (path_attrib_efmp == INVALID_FILE_ATTRIBUTES) {
        if (_mkdir(output_dir_path) != 0) { /* CreateDirectoryA is for full paths, _mkdir for single dir */
            fprintf(stderr, "extract_files_matching_pattern: Error creating output directory '%s': Windows Error %lu, errno %d\n", output_dir_path, GetLastError(), errno);
            return false;
        }
        printf("extract_files_matching_pattern: Info - Created output directory '%s'.\n", output_dir_path);
    }
    else if (!(path_attrib_efmp & FILE_ATTRIBUTE_DIRECTORY)) {
        fprintf(stderr, "extract_files_matching_pattern: Error - Output path '%s' exists but is not a directory.\n", output_dir_path);
        return false;
    }
#else /* POSIX */
    struct stat st_dir_info_efmp;
    if (stat(output_dir_path, &st_dir_info_efmp) != 0) {
        if (errno == ENOENT) {
            if (mkdir(output_dir_path, 0755) != 0) { /* 0755 gives rwx for owner, rx for group/other */
                fprintf(stderr, "extract_files_matching_pattern: Error creating output directory '%s': %s\n", output_dir_path, strerror(errno));
                return false;
            }
            printf("extract_files_matching_pattern: Info - Created output directory '%s'.\n", output_dir_path);
        }
        else {
            fprintf(stderr, "extract_files_matching_pattern: Error accessing output directory '%s': %s\n", output_dir_path, strerror(errno));
            return false;
        }
    }
    else if (!S_ISDIR(st_dir_info_efmp.st_mode)) {
        fprintf(stderr, "extract_files_matching_pattern: Error - Output path '%s' exists but is not a directory.\n", output_dir_path);
        return false;
    }
#endif

    if (disk_layout->directory->directory_size_bytes == 0) {
        printf("extract_files_matching_pattern: Directory is empty, no files to extract.\n");
        return true;
    }

    size_t num_entries = disk_layout->directory->directory_size_bytes / sizeof(directory_entry_block_t);
    bool overall_operation_critical_failure = false; /* Not currently used, but good for future */
    bool all_selected_files_extracted_successfully = true;
    int files_extracted_count = 0;
    int individual_file_errors = 0;
    char pattern_to_use[256];
    bool match_all_files_flag = false;

    if (options->pattern == NULL || strcmp(options->pattern, "*.*") == 0 || strcmp(options->pattern, "*") == 0) {
        match_all_files_flag = true;
    }
    else {
        strncpy(pattern_to_use, options->pattern, sizeof(pattern_to_use) - 1);
        pattern_to_use[sizeof(pattern_to_use) - 1] = '\0';
    }

    printf("extract_files_matching_pattern: Starting file extraction to '%s' for User ID ", output_dir_path);
    if (options->owner_id_filter == OWNER_ID_WILDCARD) {
        printf("Any Owner (*)");
    }
    else {
        printf("%d", options->owner_id_filter);
    }

    if (!match_all_files_flag) {
        printf(" with pattern '%s'", options->pattern);
    }
    printf("...\n");

    for (size_t i = 0; i < num_entries; ++i) {
        const directory_entry_block_t* deb_entry = &disk_layout->directory->directory[i];
        if (!oasis_deb_is_valid(deb_entry)) continue;

        /* Apply User ID Filter */
        if (options->owner_id_filter != OWNER_ID_WILDCARD && deb_entry->owner_id != options->owner_id_filter) {
            continue;
        }

        bool current_file_should_be_extracted = match_all_files_flag;
        if (!match_all_files_flag) {
            current_file_should_be_extracted = oasis_filename_wildcard_match(deb_entry->file_name, deb_entry->file_type, pattern_to_use);
        }

        if (current_file_should_be_extracted) {
            char host_filename_generated[MAX_HOST_FILENAME_LEN];
            char full_host_path[1024 + MAX_HOST_FILENAME_LEN + 2]; /* Ensure enough space for dir + separator + filename */
            bool current_file_processing_ok = true;

            if (!oasis_deb_to_host_filename(deb_entry, host_filename_generated, sizeof(host_filename_generated))) {
                fprintf(stderr, "extract_files_matching_pattern: Error generating host filename for DEB #%zu. Skipping.\n", i);
                individual_file_errors++; current_file_processing_ok = false;
            }

            if (current_file_processing_ok) {
                int path_len = snprintf(full_host_path, sizeof(full_host_path), "%s%c%s", output_dir_path, kPathSeparator, host_filename_generated);
                if (path_len < 0 || (size_t)path_len >= sizeof(full_host_path)) {
                    fprintf(stderr, "extract_files_matching_pattern: Error - Output path too long for '%s'. Skipping.\n", host_filename_generated);
                    individual_file_errors++; current_file_processing_ok = false;
                }
            }

            if (current_file_processing_ok) {
                printf("Extracting '%s' (Owner: %d) to '%s'... ", host_filename_generated, deb_entry->owner_id, full_host_path); fflush(stdout);

                uint8_t* raw_file_data_buffer = NULL;
                ssize_t bytes_read_from_oasis = -1;
                bool conversion_applied = false;

                if (!oasis_file_read_data(img_stream, deb_entry, &raw_file_data_buffer, &bytes_read_from_oasis)) {
                    individual_file_errors++; current_file_processing_ok = false;
                    printf("Failed (reading OASIS file data).\n");
                    if (raw_file_data_buffer) free(raw_file_data_buffer);
                }
                else if (bytes_read_from_oasis < 0) {
                    individual_file_errors++; current_file_processing_ok = false;
                    printf("Failed (read operation indicated an error with negative byte count).\n");
                    if (raw_file_data_buffer) free(raw_file_data_buffer);
                }
                else {
                    if (bytes_read_from_oasis == 0) {
                        if (write_buffer_to_host_file(full_host_path, NULL, 0, &deb_entry->timestamp)) {
                            printf("Done (0 bytes).\n"); files_extracted_count++;
                        }
                        else {
                            individual_file_errors++; current_file_processing_ok = false;
                            printf("Failed (writing 0-byte file to host).\n");
                        }
                    }
                    else {
                        size_t data_size_for_host = (size_t)bytes_read_from_oasis;
                        uint8_t* data_to_write = raw_file_data_buffer;

                        if (!convert_file_if_ascii(options, &data_to_write, &data_size_for_host, host_filename_generated, &conversion_applied)) {
                            individual_file_errors++; current_file_processing_ok = false;
                            printf("Failed (ASCII conversion process reported an issue).\n");
                            if (data_to_write != raw_file_data_buffer && data_to_write != NULL) free(data_to_write);
                            /* else if (raw_file_data_buffer) free(raw_file_data_buffer); *//* This else-if is problematic; data_to_write may point to raw_file_data_buffer if convert_file_if_ascii did not reassign it due to no conversion needed but still returned false for some reason. Correct logic is to only free data_to_write if it's different from raw_file_data_buffer and not NULL, or always free the original raw_file_data_buffer if data_to_write was just a pointer to it and then set raw_file_data_buffer to NULL. Given data_to_write is a copy of raw_file_data_buffer at start, and then potentially reassigned, it's safer to free raw_file_data_buffer if data_to_write is different AND non-NULL. Or, just free data_to_write (if not NULL) which holds the final buffer (original or new).
                            The current free(data_to_write) in the subsequent block handles all cases correctly if convert_file_if_ascii doesn't double-free or misuse pointers.
                            */
                            if (raw_file_data_buffer && data_to_write != raw_file_data_buffer) { /* If conversion allocated new and then failed */
                                free(raw_file_data_buffer);
                            }
                            if (data_to_write) { free(data_to_write); data_to_write = NULL; }

                        }

                        if (current_file_processing_ok) {
                            if (!write_buffer_to_host_file(full_host_path, data_to_write, data_size_for_host, &deb_entry->timestamp)) {
                                individual_file_errors++; current_file_processing_ok = false;
                                printf("Failed (writing data to host file).\n");
                            }
                            else {
                                printf("Done (%zu bytes%s).\n", data_size_for_host, conversion_applied ? ", ASCII converted" : "");
                                files_extracted_count++;
                            }
                        }
                        /* data_to_write is either raw_file_data_buffer or a new buffer from conversion. It needs to be freed. */
                        if (data_to_write) free(data_to_write);
                        raw_file_data_buffer = NULL; /* Ensure original pointer is nulled if it was what data_to_write pointed to */
                    }
                }
            }
            if (!current_file_processing_ok) {
                all_selected_files_extracted_successfully = false;
            }
        }
    }

    printf("extract_files_matching_pattern: Finished. Files extracted (for User ID ");
    if (options->owner_id_filter == OWNER_ID_WILDCARD) {
        printf("Any Owner (*)");
    }
    else {
        printf("%d", options->owner_id_filter);
    }
    printf("): %d, Individual file errors: %d\n", files_extracted_count, individual_file_errors);


    if (overall_operation_critical_failure) {
        return false;
    }
    return all_selected_files_extracted_successfully;
}

/* Static helper implementations (convert_file_if_ascii, write_buffer_to_host_file) */
/* These remain unchanged from your provided version but are included for completeness */
static bool convert_file_if_ascii(const cli_options_t* options,
    uint8_t** current_buffer_ptr,
    size_t* current_size_ptr,
    const char* host_filename,
    bool* was_converted_ptr) {
    *was_converted_ptr = false;
    if (options == NULL || current_buffer_ptr == NULL || *current_buffer_ptr == NULL || current_size_ptr == NULL || host_filename == NULL) {
        return false;
    }

    if (options->ascii_conversion && is_ascii(*current_buffer_ptr, *current_size_ptr)) {
        size_t original_data_len = *current_size_ptr;
        size_t effective_data_len_for_conversion = original_data_len;

        for (size_t k = 0; k < original_data_len; ++k) {
            if ((*current_buffer_ptr)[k] == SUB) {
                effective_data_len_for_conversion = k;
                printf("(SUB EOF found at %zu) ", k);
                break;
            }
        }

        size_t converted_buffer_alloc_capacity = effective_data_len_for_conversion * 2 + 1;
        if (effective_data_len_for_conversion == 0) { converted_buffer_alloc_capacity = 1; }

        uint8_t* new_converted_buffer = (uint8_t*)malloc(converted_buffer_alloc_capacity);
        if (!new_converted_buffer) {
            perror("convert_file_if_ascii: Error allocating buffer for ASCII conversion");
            return false;
        }

        conversion_result_t conversion_stats;
        int32_t actual_converted_length = ascii_oasis_to_host(
            *current_buffer_ptr,
            effective_data_len_for_conversion,
            new_converted_buffer,
            converted_buffer_alloc_capacity,
            &conversion_stats
        );

        if (actual_converted_length >= 0) {
            /* ASCII conversion successful */
            free(*current_buffer_ptr);
            *current_buffer_ptr = new_converted_buffer;
            *current_size_ptr = (size_t)actual_converted_length;
            *was_converted_ptr = true;
            return true;
        }
        else {
            fprintf(stderr, "\nconvert_file_if_ascii: Warning - ASCII conversion call failed for %s (Error %d). Original content will be used by caller.\n", host_filename, actual_converted_length);
            free(new_converted_buffer);
            *was_converted_ptr = false;
            return false;
        }
    }
    return true; /* No conversion attempted or needed */
}

static bool write_buffer_to_host_file(const char* full_file_path,
    const uint8_t* buffer_to_write,
    size_t size_to_write,
    const oasis_tm_t* timestamp) {
    FILE* out_stream_wbhf;

    if (full_file_path == NULL) return false;

    out_stream_wbhf = fopen(full_file_path, "wb");
    if (!out_stream_wbhf) {
        char error_message_wbhf[1024];
        snprintf(error_message_wbhf, sizeof(error_message_wbhf), "\nwrite_buffer_to_host_file: Error opening output file '%s' for writing", full_file_path);
        perror(error_message_wbhf);
        return false;
    }

    if (size_to_write > 0) {
        if (buffer_to_write == NULL) {
            fprintf(stderr, "\nwrite_buffer_to_host_file: Error - Buffer is NULL but size is %zu for file %s.\n", size_to_write, full_file_path);
            fclose(out_stream_wbhf);
            return false;
        }
        size_t actual_bytes_written_wbhf = fwrite(buffer_to_write, 1, size_to_write, out_stream_wbhf);
        if (actual_bytes_written_wbhf != size_to_write) {
            fprintf(stderr, "\nwrite_buffer_to_host_file: Error - Incomplete write to output file %s. Expected %zu, wrote %zu.\n",
                full_file_path, size_to_write, actual_bytes_written_wbhf);
            if (ferror(out_stream_wbhf)) {
                perror(" fwrite error details");
            }
            fclose(out_stream_wbhf);
            remove(full_file_path); /* Attempt to remove partially written file */
            return false;
        }
    }

    if (fclose(out_stream_wbhf) != 0) {
        perror("\nwrite_buffer_to_host_file: Error closing output file");
        /* File might still be created and (partially) written */
        return false;
    }

    if (timestamp != NULL) {
        if (!set_file_timestamp(full_file_path, timestamp)) {
            /* This is a warning, not a failure of writing content itself */
        }
    }
    return true;
}

/* set_file_timestamp and create_and_open_oasis_file function implementations */
/* (These would be the same as in your existing oasis_extract.c) */
/* ... */
bool set_file_timestamp(const char* filepath, const oasis_tm_t* oasis_ts) {
#if defined(HAVE_SYS_UTIME_H) || defined(HAVE_UTIME_H)
    struct tm file_tm_val_sft;
    time_t file_time_t_val_sft;
    struct utimbuf utimes_buf_val_sft;

    if (oasis_ts == NULL || filepath == NULL) return false;

    oasis_convert_timestamp_to_tm(oasis_ts, &file_tm_val_sft);
    file_tm_val_sft.tm_isdst = -1;
    file_time_t_val_sft = mktime(&file_tm_val_sft);

    if (file_time_t_val_sft != (time_t)-1) {
        utimes_buf_val_sft.actime = file_time_t_val_sft;
        utimes_buf_val_sft.modtime = file_time_t_val_sft;
        if (utime(filepath, &utimes_buf_val_sft) != 0) {
            perror("set_file_timestamp: Warning - utime failed");
            return false;
        }
        return true;
    }
    else {
        fprintf(stderr, "set_file_timestamp: Warning - mktime failed for %s\n", filepath);
        return false;
    }
#elif defined(_WIN32)
    struct tm file_tm_win_sft; SYSTEMTIME sys_time_win_sft;
    FILETIME local_file_time_win_sft, create_time_dummy, access_time_dummy; /* Dummies for CreateFileTime, LastAccessTime */
    HANDLE h_file_sft;

    if (oasis_ts == NULL || filepath == NULL) return false;
    oasis_convert_timestamp_to_tm(oasis_ts, &file_tm_win_sft);
    sys_time_win_sft.wYear = (WORD)(file_tm_win_sft.tm_year + 1900);
    sys_time_win_sft.wMonth = (WORD)(file_tm_win_sft.tm_mon + 1);
    sys_time_win_sft.wDayOfWeek = (WORD)file_tm_win_sft.tm_wday; /* May not be correctly set by oasis_convert */
    sys_time_win_sft.wDay = (WORD)file_tm_win_sft.tm_mday;
    sys_time_win_sft.wHour = (WORD)file_tm_win_sft.tm_hour;
    sys_time_win_sft.wMinute = (WORD)file_tm_win_sft.tm_min;
    sys_time_win_sft.wSecond = (WORD)file_tm_win_sft.tm_sec;
    sys_time_win_sft.wMilliseconds = 0;

    if (SystemTimeToFileTime(&sys_time_win_sft, &local_file_time_win_sft)) {
        h_file_sft = CreateFileA(filepath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h_file_sft != INVALID_HANDLE_VALUE) {
            /* Keep creation and last access times as they are, only change modification time */
            if (!GetFileTime(h_file_sft, &create_time_dummy, &access_time_dummy, NULL)) {
                fprintf(stderr, "set_file_timestamp: Warning - GetFileTime failed for %s (Error: %lu)\n", filepath, GetLastError());
                /* Proceed to try setting, maybe it works without preserving */
            }
            if (!SetFileTime(h_file_sft, &create_time_dummy, &access_time_dummy, &local_file_time_win_sft)) {
                fprintf(stderr, "set_file_timestamp: Warning - SetFileTime failed for %s (Error: %lu)\n", filepath, GetLastError());
                CloseHandle(h_file_sft); return false;
            } CloseHandle(h_file_sft); return true;
        }
        else { fprintf(stderr, "set_file_timestamp: Warning - CreateFileA for %s failed (Error: %lu)\n", filepath, GetLastError()); return false; }
    }
    else { fprintf(stderr, "set_file_timestamp: Warning - SystemTimeToFileTime for %s failed (Error: %lu)\n", filepath, GetLastError()); return false; }
#else
    fprintf(stderr, "set_file_timestamp: Warning - Timestamp setting not supported on this platform for %s\n", filepath);
    (void)oasis_ts; (void)filepath;
    return true; /* Return true as it's non-critical */
#endif
}

int create_and_open_oasis_file(const char* output_dir_path,
    const char* base_filename,
    FILE** ostream,
    const directory_entry_block_t* dir_entry,
    int quiet,
    int debug) {
#ifdef _WIN32
    DWORD path_attrib_caof_ext;
#else
    struct stat path_stat_info_caof_ext;
#endif
    char full_output_path_caof_ext[1024 + MAX_HOST_FILENAME_LEN + 2];
    int snprintf_ret_caof_ext, saved_errno_caof_ext;

    if (!output_dir_path || !base_filename || !ostream || !dir_entry) { errno = EINVAL; return -EINVAL; }
    *ostream = NULL; errno = 0;
    if (debug) fprintf(stderr, "DEBUG (create_and_open): Dir '%s'.\n", output_dir_path);
#ifdef _WIN32
    path_attrib_caof_ext = GetFileAttributesA(output_dir_path);
    if (path_attrib_caof_ext == INVALID_FILE_ATTRIBUTES) {
        saved_errno_caof_ext = (int)GetLastError();
        if (saved_errno_caof_ext == ERROR_PATH_NOT_FOUND || saved_errno_caof_ext == ERROR_FILE_NOT_FOUND) {
            if (!quiet || debug) printf("Info (create_and_open): Creating dir '%s'.\n", output_dir_path);
            if (_mkdir(output_dir_path) != 0) { /* _mkdir for single directory creation */
                saved_errno_caof_ext = errno; fprintf(stderr, "Error (create_and_open): Failed create dir '%s': %s (WinErr: %lu)\n", output_dir_path, strerror(saved_errno_caof_ext), GetLastError()); return -saved_errno_caof_ext;
            }
            if (!quiet || debug) printf("Info (create_and_open): Dir '%s' created.\n", output_dir_path);
        }
        else { fprintf(stderr, "Error (create_and_open): Cannot access dir '%s': WinErr %d\n", output_dir_path, saved_errno_caof_ext); return -EACCES; }
    }
    else if (!(path_attrib_caof_ext & FILE_ATTRIBUTE_DIRECTORY)) { fprintf(stderr, "Error (create_and_open): Path '%s' not a dir.\n", output_dir_path); return -ENOTDIR; }
    else if (debug) fprintf(stderr, "DEBUG (create_and_open): Dir '%s' exists.\n", output_dir_path);
#else /* POSIX */
    int mkdir_ret_caof_ext;
    mode_t mode = S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH; /* 0755 */

    if (stat(output_dir_path, &path_stat_info_caof_ext) != 0) {
        saved_errno_caof_ext = errno;
        if (saved_errno_caof_ext == ENOENT) {
            if (!quiet || debug) printf("Info (create_and_open): Creating dir '%s'.\n", output_dir_path);
            mkdir_ret_caof_ext = mkdir(output_dir_path, mode);
            if (mkdir_ret_caof_ext != 0) { saved_errno_caof_ext = errno; fprintf(stderr, "Error (create_and_open): Failed create dir '%s': %s\n", output_dir_path, strerror(saved_errno_caof_ext)); return -saved_errno_caof_ext; }
            if (!quiet || debug) printf("Info (create_and_open): Dir '%s' created.\n", output_dir_path);
        }
        else { fprintf(stderr, "Error (create_and_open): Cannot access dir '%s': %s\n", output_dir_path, strerror(saved_errno_caof_ext)); return -saved_errno_caof_ext; }
    }
    else if (!S_ISDIR(path_stat_info_caof_ext.st_mode)) { fprintf(stderr, "Error (create_and_open): Path '%s' not a dir.\n", output_dir_path); return -ENOTDIR; }
    else if (debug) fprintf(stderr, "DEBUG (create_and_open): Dir '%s' exists.\n", output_dir_path);
#endif
    snprintf_ret_caof_ext = snprintf(full_output_path_caof_ext, sizeof(full_output_path_caof_ext), "%s%c%s", output_dir_path, kPathSeparator, base_filename);
    if (snprintf_ret_caof_ext < 0 || (size_t)snprintf_ret_caof_ext >= sizeof(full_output_path_caof_ext)) { fprintf(stderr, "Error (create_and_open): Path too long for '%s'.\n", base_filename); return -ENAMETOOLONG; }
    if (debug) fprintf(stderr, "DEBUG (create_and_open): Opening host file: '%s'\n", full_output_path_caof_ext);
    *ostream = fopen(full_output_path_caof_ext, "wb");
    if (*ostream == NULL) { saved_errno_caof_ext = errno; fprintf(stderr, "Error (create_and_open): Cannot create host file '%s': %s\n", full_output_path_caof_ext, strerror(saved_errno_caof_ext)); return -saved_errno_caof_ext; }
    if (!quiet) {
        char fname_ftype_log_caof[MAX_FNAME_FTYPE_LEN];
        printf("Receiving/Extracting \"%s\" ", base_filename);
        if (oasis_deb_get_fname_ftype(dir_entry, fname_ftype_log_caof, sizeof(fname_ftype_log_caof))) printf("(OASIS: %s) ", fname_ftype_log_caof);
        printf("-> %s\n", full_output_path_caof_ext);
        printf("%-30s %-6s %-8s %-8s %-10s %-17s %s\n",
            "Host Filename", "Format", "Recs", "Blocks", "StartSec", "Timestamp", "Owner");
        printf("----------------------------------------------------------------------------------------------------\n");
        list_single_deb(dir_entry); /* list_single_deb is in oasis_utils.c */
    }
    else if (debug) { fprintf(stderr, "DEBUG (create_and_open): Receiving/Extracting \"%s\" -> %s\n", base_filename, full_output_path_caof_ext); }
    return 0;
}
