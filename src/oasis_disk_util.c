/*
 * OASIS Disk Utility
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 *
 */

#include <stdio.h>
#include <stdlib.h> /* Required for strtol, exit codes */
#include <string.h>
#include <stdbool.h> /* For bool type */
#include <errno.h>  /* Required for errno with strtol */
#include <limits.h> /* Required for LONG_MIN, LONG_MAX with strtol */

 /* OASIS Library Headers */
#include "oasis.h"

/* Define version strings - replace with actual build system values if available */
#ifndef CMAKE_VERSION_STR
#define CMAKE_VERSION_STR "0.2.5" /* Updated version */
#endif
#ifndef GIT_VERSION_STR
#define GIT_VERSION_STR "dev" /* Placeholder git revision */
#endif

/* Function to print usage instructions */
void print_usage(const char* prog_name) {
    fprintf(stderr, "OASIS Disk Utility %s [%s]\n",
        CMAKE_VERSION_STR, GIT_VERSION_STR);
    fprintf(stderr, "Copyright (C) 2021-2025 - Howard M. Harte - https://github.com/hharte/oasis-utils\n\n");
    fprintf(stderr, "Usage: %s <disk_image_path> <OPERATION> [ARGS...]\n\n", prog_name);
    fprintf(stderr, "Manipulates OASIS disk image files.\n\n");
    fprintf(stderr, "Operations:\n");
    fprintf(stderr, "  list (l)          List files in the disk image.\n");
    fprintf(stderr, "                    Args: [pattern] [-u user_id]\n");
    fprintf(stderr, "  extract (ex)      Extract files. Order of optional arguments matters.\n");
    fprintf(stderr, "                    Args: [pattern] [output_path] [-a|--ascii] [-u user_id]\n");
    fprintf(stderr, "  info (i)          Display detailed information about the disk image.\n");
    fprintf(stderr, "  erase (er)        Mark file(s) as deleted and free their blocks.\n");
    fprintf(stderr, "                    Args: <filename_pattern> [-u user_id]\n");
    fprintf(stderr, "  rename (r)        Rename a file.\n");
    fprintf(stderr, "                    Args: <old_filename> <new_filename> [-u user_id]\n");
    fprintf(stderr, "  copyfile (c, co)  Copy host file to disk image, optionally naming it.\n");
    fprintf(stderr, "  insert (in, ins)  Alias for copyfile.\n");
    fprintf(stderr, "                    Args: <host_filepath> [oasis_filename] [-a|--ascii] [-u user_id]\n");
    fprintf(stderr, "                    - '<host_filepath>': Path to the file on the host system.\n");
    fprintf(stderr, "                    - '[oasis_filename]': Optional. Target name on OASIS disk (e.g., MYPROG.BAS_S).\n");
    fprintf(stderr, "                                        If omitted, derived from host filename.\n\n");
    fprintf(stderr, "Options for all operations (unless specified otherwise):\n");
    fprintf(stderr, "  -u, --user <id>  Limit files to the specified user ID (0-255).\n");
    fprintf(stderr, "                   Use <id> = '*' or '-1' for wildcard owner.\n");
    fprintf(stderr, "                   Default for 'list': '*' (all users).\n");
    fprintf(stderr, "                   Default for 'extract', 'info', 'erase', 'rename', 'copyfile': '0'.\n\n");
    fprintf(stderr, "Options for EXTRACT and COPYFILE/INSERT (must appear after pattern and output_path if they are specified for extract):\n");
    fprintf(stderr, "  -a, --ascii      Convert ASCII files' line endings during operation.\n");
    fprintf(stderr, "                   For EXTRACT: OASIS (CR) -> Host (LF/CRLF), SUB removed.\n");
    fprintf(stderr, "                   For COPYFILE/INSERT: Host (LF/CRLF) -> OASIS (CR), SUB potentially added.\n\n");
    fprintf(stderr, "Example:\n");
    fprintf(stderr, "  %s my_disk.img l\n", prog_name);
    fprintf(stderr, "  %s my_disk.img er \"OLDFILE.TXT\"\n", prog_name);
    fprintf(stderr, "  %s my_disk.img r \"OLDFILE.TXT\" \"NEWFILE.TXT\"\n", prog_name);
    fprintf(stderr, "  %s my_disk.img c ./myfile.txt MYOASIS.TXT_S\n", prog_name);
}

int main(int argc, char* argv[]) {
    cli_options_t options;
    sector_io_stream_t* sector_stream = NULL;
    oasis_disk_layout_t disk_layout; /* Will be in host byte order after load_oasis_disk */
    bool success = false;
    int exit_code = EXIT_FAILURE;
    const char* output_dir_for_extract = ".";
    const char* new_filename_for_rename_op = NULL;
    const char* host_filepath_for_copy = NULL;
    const char* oasis_filename_for_copy = NULL;
    int current_arg_idx;
    bool is_list = false;
    bool is_extract = false;
    bool is_info = false;
    bool is_erase = false;
    bool is_rename = false;
    bool is_copyfile = false;
    size_t op_len;
    int i;

    memset(&options, 0, sizeof(options));
    memset(&disk_layout, 0, sizeof(disk_layout));


    options.owner_id_filter = 0;
    options.ascii_conversion = false;
    options.pattern = NULL;

    if (argc < 3) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    options.image_path = argv[1];
    options.operation = argv[2];
    op_len = strlen(options.operation);

    /* Shortest unique prefix matching for operations */
    if (op_len > 0) {
        if (op_len >= 1 && strncmp(options.operation, "list", op_len) == 0 && op_len <= strlen("list")) is_list = true;
        else if (op_len >= 2 && strncmp(options.operation, "extract", op_len) == 0 && op_len <= strlen("extract")) is_extract = true;
        else if (op_len >= 1 && strncmp(options.operation, "info", op_len) == 0 && op_len <= strlen("info")) is_info = true;
        else if (op_len >= 2 && strncmp(options.operation, "erase", op_len) == 0 && op_len <= strlen("erase")) is_erase = true;
        else if (op_len >= 1 && strncmp(options.operation, "rename", op_len) == 0 && op_len <= strlen("rename")) is_rename = true;
        else if ((op_len >= 1 && strncmp(options.operation, "copyfile", op_len) == 0 && op_len <= strlen("copyfile")) ||
            (op_len >= 2 && strncmp(options.operation, "insert", op_len) == 0 && op_len <= strlen("insert"))) is_copyfile = true;
        else {
            if (op_len == 1) {
                if (*options.operation == 'l') is_list = true;
                else if (*options.operation == 'i') is_info = true;
                else if (*options.operation == 'r') is_rename = true;
                else if (*options.operation == 'c') is_copyfile = true;
            }
        }
    }

    if (!(is_list || is_extract || is_info || is_erase || is_rename || is_copyfile)) {
        fprintf(stderr, "Error: Invalid or ambiguous operation '%s'.\n", options.operation);
        print_usage(argv[0]); return EXIT_FAILURE;
    }


    if (is_list) {
        options.owner_id_filter = OWNER_ID_WILDCARD; /* Default for list is all users */
    }
    /* For other ops, default owner_id_filter is 0 (set initially) */

    current_arg_idx = 3; /* Argument after <image_path> and <OPERATION> */

    /* Parse operation-specific positional arguments */
    if (is_erase) {
        if (argc > current_arg_idx && argv[current_arg_idx][0] != '-') {
            options.pattern = argv[current_arg_idx++];
        }
        else {
            fprintf(stderr, "Error: 'erase' operation requires a filename pattern.\n");
            print_usage(argv[0]); return EXIT_FAILURE;
        }
    }
    else if (is_rename) {
        if (argc > current_arg_idx && argv[current_arg_idx][0] != '-') {
            options.pattern = argv[current_arg_idx++]; /* old_filename */
        }
        else {
            fprintf(stderr, "Error: 'rename' operation requires an old filename.\n");
            print_usage(argv[0]); return EXIT_FAILURE;
        }
        if (argc > current_arg_idx && argv[current_arg_idx][0] != '-') {
            new_filename_for_rename_op = argv[current_arg_idx++];
        }
        else {
            fprintf(stderr, "Error: 'rename' operation requires a new filename.\n");
            print_usage(argv[0]); return EXIT_FAILURE;
        }
    }
    else if (is_copyfile) {
        if (argc > current_arg_idx && argv[current_arg_idx][0] != '-') {
            host_filepath_for_copy = argv[current_arg_idx++];
        }
        else {
            fprintf(stderr, "Error: '%s' operation requires a host filepath.\n", options.operation);
            print_usage(argv[0]); return EXIT_FAILURE;
        }
        if (argc > current_arg_idx && argv[current_arg_idx][0] != '-') {
            oasis_filename_for_copy = argv[current_arg_idx++];
        }
    }
    else if (is_list) { /* list [pattern] */
        if (argc > current_arg_idx && argv[current_arg_idx][0] != '-') {
            options.pattern = argv[current_arg_idx++];
        }
    }
    else if (is_extract) { /* extract [pattern] [output_path] */
        /* Positional 1: pattern OR output_path */
        if (argc > current_arg_idx && argv[current_arg_idx][0] != '-') {
            /* If it looks like a pattern or if the next arg is an option, assume it's a pattern */
            if (strchr(argv[current_arg_idx], '*') != NULL || strchr(argv[current_arg_idx], '?') != NULL ||
                (current_arg_idx + 1 < argc && argv[current_arg_idx + 1][0] == '-') ||
                (current_arg_idx + 1 >= argc) /* It's the last arg, could be pattern or path */
                ) {
                /* Heuristic: if it contains wildcards, or if the next arg is an option,
                   or if it's the last argument, treat it as a pattern.
                   This might misinterpret a path like "mydir*" as a pattern if it's the only arg.
                   A more robust CLI parser might be needed for complex cases.
                   For now, if only one positional arg after "extract", it's output_path unless it has wildcards.
                */
                if (current_arg_idx + 1 < argc && argv[current_arg_idx + 1][0] != '-') {
                    /* If there's another non-option arg, then current is pattern, next is path */
                    options.pattern = argv[current_arg_idx++];
                    output_dir_for_extract = argv[current_arg_idx++];
                }
                else if (strchr(argv[current_arg_idx], '*') != NULL || strchr(argv[current_arg_idx], '?') != NULL) {
                    /* Contains wildcards, so it's a pattern. Output path is default. */
                    options.pattern = argv[current_arg_idx++];
                }
                else {
                    /* No wildcards, and no further non-option arg, so it's output_path. Pattern is default. */
                    output_dir_for_extract = argv[current_arg_idx++];
                }
            }
            else { /* No wildcards, and next arg is not an option, so this must be output_path */
                output_dir_for_extract = argv[current_arg_idx++];
            }
        }
        /* Check for a second positional argument if the first was a pattern */
        if (options.pattern != NULL && argc > current_arg_idx && argv[current_arg_idx][0] != '-') {
            output_dir_for_extract = argv[current_arg_idx++];
        }
    }
    /* For 'info', no specific positional args */


    /* Option parsing loop for -u and -a */
    for (i = current_arg_idx; i < argc; ++i) {
        if (strcmp(argv[i], "-u") == 0 || strcmp(argv[i], "--user") == 0) {
            if (i + 1 < argc) {
                char* user_arg = argv[++i];
                if (strcmp(user_arg, "*") == 0 || strcmp(user_arg, "\'*\'") == 0 || strcmp(user_arg, "\"-1\"") == 0 || strcmp(user_arg, "-1") == 0) {
                    options.owner_id_filter = OWNER_ID_WILDCARD;
                }
                else {
                    char* endptr;
                    long val;
                    errno = 0;
                    val = strtol(user_arg, &endptr, 10);
                    if (endptr == user_arg || *endptr != '\0' ||
                        ((val == LONG_MIN || val == LONG_MAX) && errno == ERANGE) ||
                        val < 0 || val > 255) {
                        fprintf(stderr, "Error: Invalid user ID '%s'. Must be 0-255 or '*'/-1.\n", user_arg);
                        print_usage(argv[0]); return EXIT_FAILURE;
                    }
                    options.owner_id_filter = (int)val;
                }
            }
            else {
                fprintf(stderr, "Error: Option '%s' requires a user ID value.\n", argv[i]);
                print_usage(argv[0]); return EXIT_FAILURE;
            }
        }
        else if ((is_extract || is_copyfile) && (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--ascii") == 0)) {
            options.ascii_conversion = true;
        }
        else {
            fprintf(stderr, "Error: Unknown or misplaced option/argument '%s'.\n", argv[i]);
            print_usage(argv[0]); return EXIT_FAILURE;
        }
    }

    /* --- Open Disk Image --- */
    const char* open_mode = (is_erase || is_rename || is_copyfile) ? "r+b" : "rb";
    sector_stream = sector_io_open(options.image_path, open_mode);
    if (!sector_stream) {
        fprintf(stderr, "Error: Failed to open disk image file '%s' in mode '%s'.\n", options.image_path, open_mode);
        return EXIT_FAILURE;
    }

    printf("Loading disk image metadata from '%s'...\n", options.image_path);
    if (!load_oasis_disk(sector_stream, &disk_layout)) { /* load_oasis_disk now handles le16toh */
        fprintf(stderr, "Error: Failed to load disk image metadata.\n");
        goto cleanup;
    }

    printf("Filtering files for User ID: ");
    if (options.owner_id_filter == OWNER_ID_WILDCARD) {
        printf("Any Owner (*)\n");
    }
    else {
        printf("%d\n", options.owner_id_filter);
    }

    /* --- Perform Operation --- */
    if (is_info) {
        display_disk_info(&disk_layout); /* display_disk_info operates on host-order data */
        success = true;
    }
    else if (is_list) {
        if (options.pattern != NULL) printf("Listing files matching pattern: '%s'\n", options.pattern);
        else printf("Listing all files\n");
        list_files(&disk_layout, options.owner_id_filter, options.pattern); /* list_files operates on host-order data */
        success = true;
    }
    else if (is_extract) {
        printf("Extracting files ");
        if (options.pattern != NULL) printf("matching pattern: '%s' ", options.pattern);
        else printf("all files ");
        printf("to '%s'\n", output_dir_for_extract);
        success = extract_files_matching_pattern(sector_stream, &disk_layout, output_dir_for_extract, &options);
        if (!success) fprintf(stderr, "Extraction operation completed with errors.\n");
        else printf("Extraction operation completed successfully.\n");
    }
    else if (is_erase) {
        success = oasis_erase_files_by_pattern(sector_stream, &disk_layout, &options); /* This will handle htole16 internally before writing */
        if (!success) fprintf(stderr, "Erase operation completed with errors.\n");
        else printf("Erase operation completed.\n");
    }
    else if (is_rename) {
        success = oasis_rename_file_by_pattern_and_name(sector_stream, &disk_layout, &options, new_filename_for_rename_op); /* This will handle htole16 internally */
        if (!success) fprintf(stderr, "Rename operation completed with errors.\n");
        else printf("Rename operation completed.\n");
    }
    else if (is_copyfile) {
        cli_options_t copy_op_options = options;
        if (copy_op_options.owner_id_filter == OWNER_ID_WILDCARD) {
            copy_op_options.owner_id_filter = 0;
        }
        printf("Copying host file '%s' to OASIS disk as '%s' (User ID: %d, ASCII: %s)\n",
            host_filepath_for_copy,
            oasis_filename_for_copy ? oasis_filename_for_copy : "<derived_from_host_name>",
            copy_op_options.owner_id_filter,
            copy_op_options.ascii_conversion ? "Yes" : "No");
        success = oasis_copy_host_file_to_disk(sector_stream, &disk_layout,
            host_filepath_for_copy,
            oasis_filename_for_copy,
            &copy_op_options); /* This will handle htole16 internally */
        if (!success) fprintf(stderr, "%s operation failed.\n", options.operation);
        else printf("%s operation completed.\n", options.operation);
    }

    if (success) {
        exit_code = EXIT_SUCCESS;
    }

cleanup:
    cleanup_oasis_disk(&disk_layout);
    if (sector_stream) {
        if (sector_io_close(sector_stream) != 0) {
            perror("Warning: Error closing disk image via sector_io_close");
            if (exit_code == EXIT_SUCCESS) {
                exit_code = EXIT_FAILURE;
            }
        }
    }
    return exit_code;
}
