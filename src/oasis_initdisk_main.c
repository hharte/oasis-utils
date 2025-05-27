/*
 * oasis_initdisk_main.c - OASIS Disk Initialization Utility (Main Program)
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

#include "oasis.h" /* Library functions */

/* Define CMAKE_VERSION_STR and GIT_VERSION_STR if not defined by build system */
/* These are typically passed by CMake. */
#ifndef CMAKE_VERSION_STR
#define CMAKE_VERSION_STR "0.1.4" /* Default if not set by build */
#endif
#ifndef GIT_VERSION_STR
#define GIT_VERSION_STR "dev"     /* Default if not set by build */
#endif

/* Static functions for UI and argument parsing */
static void print_initdisk_usage_main(const char* prog_name);
static int parse_initdisk_arguments_main(int argc, char* argv[], initdisk_options_lib_t* opts);
static void get_user_input_main(const char* prompt, char* buffer, size_t buffer_len);

/*
 * Main function for the oasis_initdisk utility.
 */
int main(int argc, char* argv[]) {
    initdisk_options_lib_t options_main; /* Use the library's options struct */
    int parse_result;

    /* Initialize options to defaults */
    memset(&options_main, 0, sizeof(options_main));
    options_main.num_heads = DEFAULT_NUM_HEADS_LIB;
    options_main.tracks_per_surface = DEFAULT_TRACKS_PER_SURFACE_LIB;
    options_main.sectors_per_track = DEFAULT_SECTORS_PER_TRACK_LIB;
    options_main.sector_increment = DEFAULT_SECTOR_INCREMENT_LIB;
    options_main.track_skew = DEFAULT_TRACK_SKEW_LIB;
    options_main.dir_size = DEFAULT_DIR_SIZE_LIB;
    options_main.drive_letter = 'A'; /* Default drive letter */
    /* disk_label_str will be prompted if needed and not specified */

    parse_result = parse_initdisk_arguments_main(argc, argv, &options_main);

    if (parse_result <= 0) { /* 0 means print usage, -1 means error */
        if (parse_result == 0) {
            print_initdisk_usage_main(argv[0]);
        }
        return EXIT_FAILURE;
    }

    /* Call the main library function to perform the disk operation */
    return initdisk_perform_operation(&options_main);
}

/*
 * Prints usage instructions for the oasis_initdisk utility.
 */
static void print_initdisk_usage_main(const char* prog_name) {
    fprintf(stderr, "OASIS INITDISK Utility %s [%s]\n", CMAKE_VERSION_STR, GIT_VERSION_STR);
    fprintf(stderr, "Copyright (C) 2021-2025 - Howard M. Harte - https://github.com/hharte/oasis-utils\n\n");
    fprintf(stderr, "Usage: %s <image_path_or_fd> [OPTION]...\n\n", prog_name);
    fprintf(stderr, "  <image_path_or_fd> Path to the disk image file, or a drive letter (A-Z, assumed to be image path for this util).\n");
    fprintf(stderr, "  Options (case-insensitive, space separated):\n");
    fprintf(stderr, "    BUILD          - Write bootstrap, label, directory to an already formatted disk.\n");
    fprintf(stderr, "    CLEAR / CL     - Erase all files, re-initialize directory.\n");
    fprintf(stderr, "    FORMAT / FMT   - Initialize entire disk format, then build filesystem.\n");
    fprintf(stderr, "    LABEL <name>   - Set or re-initialize disk label to <name> (max %d chars).\n", FNAME_LEN);
    fprintf(stderr, "    NOWP           - Remove software write protection.\n");
    fprintf(stderr, "    WP             - Enable software write protection.\n");
    fprintf(stderr, "    HEAD <n>       - (Requires FORMAT) Number of disk surfaces (1-255).\n");
    fprintf(stderr, "    INCR <n>       - (Requires FORMAT) Logical sector increment (1-255).\n");
    fprintf(stderr, "    SECTOR <n>     - (Requires FORMAT) Sectors per track (1-255).\n");
    fprintf(stderr, "    SIZE <n>       - (Requires FORMAT or CLEAR) Number of directory entries.\n");
    fprintf(stderr, "    SKEW <n>       - (Requires FORMAT) Track skew factor (0-255), affects starting sector ID.\n");
    fprintf(stderr, "    TRACKS <n>     - (Requires FORMAT) Tracks per surface (1-255).\n\n");
    fprintf(stderr, "Example:\n");
    fprintf(stderr, "  %s mydisk.img FORMAT SIZE 64 LABEL MYDISK\n", prog_name);
    fprintf(stderr, "  %s B CL\n", prog_name);
}

/*
 * Parses command-line arguments for the oasis_initdisk utility.
 */
static int parse_initdisk_arguments_main(int argc, char* argv[], initdisk_options_lib_t* opts) {
    int i;
    long val; /* For strtol results */
    char* endptr; /* For strtol error checking */

    if (argc < 2) {
        return 0; /* Not enough args, print usage */
    }

    /* First argument is image_path or drive letter */
    if (strlen(argv[1]) == 1 && toupper(argv[1][0]) >= 'A' && toupper(argv[1][0]) <= 'Z') {
        opts->drive_letter = (char)toupper(argv[1][0]);
        snprintf(opts->image_path, sizeof(opts->image_path), "%c.img", opts->drive_letter);
        fprintf(stdout, "Info: Drive letter '%c' specified, assuming image path '%s'.\n", opts->drive_letter, opts->image_path);
    } else {
        strncpy(opts->image_path, argv[1], sizeof(opts->image_path) - 1);
        opts->image_path[sizeof(opts->image_path) - 1] = '\0';
        /* Try to infer drive letter from image path, e.g. "A.IMG" -> 'A' */
        if (strlen(opts->image_path) > 0 && isalpha(opts->image_path[0]) && (opts->image_path[1] == '.' || opts->image_path[1] == '\0')) {
            opts->drive_letter = (char)toupper(opts->image_path[0]);
        } /* No else, drive_letter remains default 'A' if not inferable */
    }

    /* If no operations are specified after image_path, default to FORMAT */
    if (argc == 2) {
        fprintf(stderr, "Info: No operation specified. Defaulting to FORMAT with default parameters.\n");
        opts->format_op = 1;
    }

    for (i = 2; i < argc; ++i) {
        char* token = argv[i];
        int k; /* Loop var for toupper */
        for (k = 0; token[k]; k++) { /* Convert token to uppercase for case-insensitive match */
            token[k] = (char)toupper(token[k]);
        }

        if (strcmp(token, "BUILD") == 0) { opts->build_op = 1; }
        else if (strcmp(token, "CLEAR") == 0 || strcmp(token, "CL") == 0) { opts->clear_op = 1; }
        else if (strcmp(token, "FORMAT") == 0 || strcmp(token, "FMT") == 0) { opts->format_op = 1; }
        else if (strcmp(token, "LABEL") == 0) {
            opts->label_op = 1;
            opts->label_specified = 1;
            if (++i < argc) {
                strncpy(opts->disk_label_str, argv[i], FNAME_LEN);
                opts->disk_label_str[FNAME_LEN] = '\0'; /* Ensure null termination */
                /* Uppercase the label as it's stored in DEB */
                for(k = 0; opts->disk_label_str[k]; k++) opts->disk_label_str[k] = (char)toupper(opts->disk_label_str[k]);
            } else { fprintf(stderr, "Error: LABEL option requires a name.\n"); return -1; }
        }
        else if (strcmp(token, "NOWP") == 0) { opts->nowp_op = 1; }
        else if (strcmp(token, "WP") == 0) { opts->wp_op = 1; }
        else if (strcmp(token, "HEAD") == 0) {
            opts->heads_specified = 1;
            if (++i < argc) {
                val = strtol(argv[i], &endptr, 10);
                if (*endptr != '\0' || val <= 0 || val > 255) {
                    fprintf(stderr, "Error: HEAD option requires a valid number (1-255).\n"); return -1;
                }
                opts->num_heads = (int)val;
            } else { fprintf(stderr, "Error: HEAD option requires a number.\n"); return -1; }
        }
        else if (strcmp(token, "INCR") == 0) {
            opts->incr_specified = 1;
            if (++i < argc) {
                val = strtol(argv[i], &endptr, 10);
                if (*endptr != '\0' || val <= 0 || val > 255) {
                    fprintf(stderr, "Error: INCR option requires a valid number (1-255).\n"); return -1;
                }
                opts->sector_increment = (int)val;
            } else { fprintf(stderr, "Error: INCR option requires a number.\n"); return -1; }
        }
        else if (strcmp(token, "SECTOR") == 0) { /* Note: original struct field was "sector_specififed" */
            opts->sector_specififed = 1;
            if (++i < argc) {
                val = strtol(argv[i], &endptr, 10);
                if (*endptr != '\0' || val <= 0 || val > 255) {
                    fprintf(stderr, "Error: SECTOR option requires a valid number (1-255).\n"); return -1;
                }
                opts->sectors_per_track = (int)val;
            } else { fprintf(stderr, "Error: SECTOR option requires a number.\n"); return -1; }
        }
        else if (strcmp(token, "SIZE") == 0) {
            opts->size_specified = 1;
            if (++i < argc) {
                val = strtol(argv[i], &endptr, 10);
                if (*endptr != '\0' || val <= 0) { /* Directory entries must be positive */
                    fprintf(stderr, "Error: SIZE option requires a positive number for directory entries.\n"); return -1;
                }
                opts->dir_size = (int)val;
            } else { fprintf(stderr, "Error: SIZE option requires a number.\n"); return -1; }
        }
        else if (strcmp(token, "SKEW") == 0) {
            opts->skew_specified = 1;
            if (++i < argc) {
                val = strtol(argv[i], &endptr, 10);
                if (*endptr != '\0' || val < 0 || val > 255) {
                    fprintf(stderr, "Error: SKEW option requires a valid number (0-255).\n"); return -1;
                }
                opts->track_skew = (int)val;
            } else { fprintf(stderr, "Error: SKEW option requires a number.\n"); return -1; }
        }
        else if (strcmp(token, "TRACKS") == 0) {
            opts->tracks_specified = 1;
            if (++i < argc) {
                val = strtol(argv[i], &endptr, 10);
                 if (*endptr != '\0' || val <= 0 || val > 255) {
                    fprintf(stderr, "Error: TRACKS option requires a valid number (1-255).\n"); return -1;
                }
                opts->tracks_per_surface = (int)val;
            } else { fprintf(stderr, "Error: TRACKS option requires a number.\n"); return -1; }
        }
        else {
            fprintf(stderr, "Error: Unknown option '%s'.\n", token);
            return -1; /* Unknown option */
        }
    }

    /* Validate option combinations */
    int primary_ops_count = (opts->format_op > 0) + (opts->clear_op > 0) + (opts->build_op > 0) +
                           (opts->label_op > 0 && !opts->format_op && !opts->clear_op && !opts->build_op) + /* Standalone LABEL counts if no other primary op */
                           (opts->wp_op > 0) + (opts->nowp_op > 0);

    if (opts->wp_op && primary_ops_count > 1) {
        fprintf(stderr, "Error: WP must be used as the only primary operation (or combined with LABEL).\n"); return -1;
    }
    if (opts->nowp_op && primary_ops_count > 1) {
        fprintf(stderr, "Error: NOWP must be used as the only primary operation (or combined with LABEL).\n"); return -1;
    }
    if (opts->format_op + opts->clear_op + opts->build_op > 1) {
        fprintf(stderr, "Error: Conflicting primary operations (FORMAT, CLEAR, BUILD) specified. Choose only one.\n"); return -1;
    }

    /* Ensure at least one primary operation if options were given */
    if (primary_ops_count == 0 && argc > 2) { /* argc > 2 means options were provided beyond image_path */
         fprintf(stderr, "Error: No primary operation (FORMAT, CLEAR, BUILD, LABEL, WP, NOWP) specified among options.\n"); return -1;
    }
    /* If only image path given (argc==2), format_op was set as default. */


    /* Validate options that depend on FORMAT */
    if ((opts->heads_specified || opts->incr_specified || opts->sector_specififed || opts->skew_specified || opts->tracks_specified) && !opts->format_op) {
        fprintf(stderr, "Error: Disk geometry options (HEAD, INCR, SECTOR, SKEW, TRACKS) require the FORMAT operation.\n");
        return -1;
    }
    /* Validate SIZE option dependency */
    if (opts->size_specified && !(opts->format_op || opts->clear_op || opts->build_op)) {
        fprintf(stderr, "Error: SIZE option requires FORMAT, CLEAR, or BUILD operation.\n");
        return -1;
    }

    /* If no label specified for FORMAT or BUILD, it will be prompted by the library. */
    /* If LABEL op is chosen standalone, and no label name given, it will be prompted. */
    if (opts->label_op && !opts->label_specified && !opts->format_op && !opts->build_op && !opts->clear_op) {
        /* This case means "LABEL" was the command but no name followed. */
        /* The library's initdisk_handle_label_operation now calls get_user_input_main if label is empty. */
        /* Or we can prompt here. Let's assume lib handles it or we make get_user_input_main part of lib API. */
        /* For now, ensure label_specified is true if label_op is the primary op and a name was given, or if it's prompted for. */
        /* If label_op and no name, we could prompt now or let the library do it. */
        /* The library's `initdisk_perform_operation` will call `initdisk_handle_label_operation`. */
        /* If `opts->disk_label_str` is empty and `opts->label_specified` is false, `initdisk_handle_label_operation` (or a helper it calls) should prompt. */
        /* Let's assume `get_user_input_main` is used by the library if needed. */
    } else if ((opts->format_op || opts->build_op) && !opts->label_specified) {
        get_user_input_main("Enter disk label", opts->disk_label_str, FNAME_LEN + 1);
        opts->label_specified = 1; /* Mark that label was obtained */
    }


    return 1; /* Success */
}

/*
 * Gets user input for a prompt.
 */
static void get_user_input_main(const char* prompt, char* buffer, size_t buffer_len) {
    printf("%s: ", prompt);
    fflush(stdout);
    if (fgets(buffer, (int)buffer_len, stdin) != NULL) {
        /* Remove newline character if present */
        buffer[strcspn(buffer, "\n")] = 0;
    } else {
        buffer[0] = '\0'; /* Clear buffer on error or EOF */
    }
    /* Convert to uppercase, consistent with how labels are stored/compared */
    for(int i = 0; buffer[i]; i++) {
        buffer[i] = (char)toupper(buffer[i]);
    }
}
