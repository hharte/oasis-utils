/*
 * oasis_chkdsk.c - OASIS Disk Image Consistency Checker
 *
 * Verifies DEB integrity, consistency with allocation map, sequential file linkage,
 * detects shared sectors, and for IMD files, reports bad sectors.
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>

/* OASIS Library Headers */
#include "oasis.h"
/* For IMD_SDR_* flags when checking IMD images */
#include "libimdf.h"


/* Define CMAKE_VERSION_STR and GIT_VERSION_STR if not defined by build system */
#ifndef CMAKE_VERSION_STR
#define CMAKE_VERSION_STR "0.1.0"
#endif
#ifndef GIT_VERSION_STR
#define GIT_VERSION_STR "dev"
#endif


/* Structure to hold command line options */
typedef struct {
    const char* image_path;
    const char* file_pattern; /* Optional: specific file or wildcard pattern */
    bool verbose;
} checker_options_t;

/* Structure to keep track of sector usage by files */
typedef struct {
    uint32_t sector_lba;
    int      deb_index; /* Index of the DEB claiming this sector */
    char     filename[MAX_HOST_FILENAME_LEN]; /* Store filename for better error messages */
} sector_claim_t;

/* Structure to hold information about a bad sector found in an IMD image */
typedef struct {
    uint32_t oasis_lba;         /* The calculated OASIS LBA */
    uint8_t  imd_track_cyl;     /* IMD Track Cylinder */
    uint8_t  imd_track_head;    /* IMD Track Head */
    uint8_t  imd_sector_id;     /* IMD Sector ID (from smap) */
    uint8_t  imd_sector_flag;   /* IMD Sector Flag (sflag) */
    uint16_t imd_sector_size;   /* IMD Sector Size (128 or 256) */
} imd_bad_sector_info_t;


/* Forward declarations */
static void print_usage(const char* prog_name);
static bool parse_arguments(int argc, char* argv[], checker_options_t* opts);
static bool perform_consistency_checks(const oasis_disk_layout_t* disk_layout,
    sector_io_stream_t* img_stream,
    const checker_options_t* opts);

static bool check_deb_integrity(const directory_entry_block_t* deb,
    int deb_index,
    const oasis_disk_layout_t* disk_layout,
    bool verbose);

static bool check_deb_alloc_map_consistency(const directory_entry_block_t* deb,
    int deb_index,
    const char* current_file_host_name,
    const oasis_disk_layout_t* disk_layout,
    sector_io_stream_t* img_stream,
    uint8_t* overall_sector_bitmap,
    uint32_t total_map_sectors_capacity,
    sector_claim_t* sector_claims,
    int* next_sector_claim_idx,
    int max_sector_claims,
    bool verbose);

static bool check_sequential_file_linkage(const directory_entry_block_t* deb,
    int deb_index,
    const oasis_disk_layout_t* disk_layout,
    sector_io_stream_t* img_stream,
    uint8_t* temp_sector_buffer,
    bool* is_contiguous,
    uint32_t* actual_sector_count,
    bool verbose);

/* Helper to get the bit for a sector LBA in a bitmap */
static bool get_sector_bitmap_bit(const uint8_t* bitmap, uint32_t sector_lba) {
    uint32_t byte_index;
    uint8_t bit_offset;

    if (!bitmap) { /* Should not happen if bitmap is allocated */
        return false;
    }
    byte_index = sector_lba / 8;
    bit_offset = (uint8_t)(sector_lba % 8);
    return (bitmap[byte_index] >> bit_offset) & 1;
}

/* Helper to set the bit for a sector LBA in a bitmap */
static void set_sector_bitmap_bit(uint8_t* bitmap, uint32_t sector_lba) {
    uint32_t byte_index;
    uint8_t bit_offset;

    if (!bitmap) { /* Should not happen if bitmap is allocated */
        return;
    }
    byte_index = sector_lba / 8;
    bit_offset = (uint8_t)(sector_lba % 8);
    bitmap[byte_index] |= (1 << bit_offset);
}


int main(int argc, char* argv[]) {
    checker_options_t options = { 0 };
    oasis_disk_layout_t disk_layout = { 0 };
    sector_io_stream_t* img_stream = NULL;
    int exit_code = EXIT_FAILURE;

    if (!parse_arguments(argc, argv, &options)) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    img_stream = sector_io_open(options.image_path, "rb");
    if (!img_stream) {
        fprintf(stderr, "Error: Failed to open disk image '%s'.\n", options.image_path);
        return EXIT_FAILURE;
    }

    if (!load_oasis_disk(img_stream, &disk_layout)) {
        fprintf(stderr, "Error: Failed to load disk image metadata.\n");
        goto cleanup;
    }

    if (!perform_consistency_checks(&disk_layout, img_stream, &options)) {
        goto cleanup;
    }

    exit_code = EXIT_SUCCESS;

cleanup:
    cleanup_oasis_disk(&disk_layout);
    if (img_stream) {
        sector_io_close(img_stream);
    }

    return exit_code;
}

static void print_usage(const char* prog_name) {
    fprintf(stderr, "OASIS Disk Consistency Check Utility %s [%s]\n",
        CMAKE_VERSION_STR, GIT_VERSION_STR);
    fprintf(stderr, "Copyright (C) 2021-2025 - Howard M. Harte - https://github.com/hharte/oasis-utils\n\n");
    fprintf(stderr, "Usage: %s <disk_image_path> [-f <pattern>] [-v|--verbose]\n\n", prog_name);
    fprintf(stderr, "  <disk_image_path>  Path to the OASIS disk image file.\n");
    fprintf(stderr, "  -f, --file <pattern> Optional: Check only files matching the name or wildcard pattern.\n");
    fprintf(stderr, "                       Pattern is case-insensitive (e.g., \"FILE*.TXT\", \"MYPROG.BAS\").\n");
    fprintf(stderr, "  -v, --verbose        Enable verbose output.\n\n");
    fprintf(stderr, "Examples:\n");
    fprintf(stderr, "  %s my_disk.img\n", prog_name);
    fprintf(stderr, "  %s my_disk.img -f \"SYS.*\"\n", prog_name);
    fprintf(stderr, "  %s my_disk.img --file \"TEST?.BAS\" --verbose\n", prog_name);
}

static bool parse_arguments(int argc, char* argv[], checker_options_t* opts) {
    int i;

    if (argc < 2) {
        return false;
    }
    opts->image_path = NULL;
    opts->file_pattern = NULL;
    opts->verbose = false;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--file") == 0) {
            if (i + 1 < argc) {
                opts->file_pattern = argv[++i];
            }
            else {
                fprintf(stderr, "Error: %s option requires a pattern.\n", argv[i]);
                return false;
            }
        }
        else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            opts->verbose = true;
        }
        else if (argv[i][0] == '-') {
            fprintf(stderr, "Error: Unknown option '%s'.\n", argv[i]);
            return false;
        }
        else {
            if (opts->image_path == NULL) {
                opts->image_path = argv[i];
            }
            else {
                fprintf(stderr, "Error: Too many arguments. Disk image path already specified as '%s'.\n", opts->image_path);
                return false;
            }
        }
    }

    if (opts->image_path == NULL) {
        fprintf(stderr, "Error: Disk image path is required.\n");
        return false;
    }
    return true;
}


static bool perform_consistency_checks(const oasis_disk_layout_t* disk_layout,
    sector_io_stream_t* img_stream,
    const checker_options_t* opts) {
    size_t num_entries;
    int files_checked = 0;
    int errors_found = 0;
    uint32_t total_map_sectors_capacity = 0;
    uint8_t* overall_sector_bitmap = NULL;
    uint8_t sector_buffer_for_seq[SECTOR_SIZE];
    size_t map_max_blocks = 0;
    size_t bitmap_size_bytes;
    size_t physical_disk_blocks = 0;
    uint32_t physical_disk_sectors = 0;
    uint32_t i;
    size_t j_track; /* For IMD track iteration */

    int max_claims_possible = 0;
    sector_claim_t* sector_claims = NULL;
    int next_sector_claim_idx = 0;

    imd_bad_sector_info_t* bad_imd_sectors_list = NULL;
    int bad_imd_sector_count = 0;
    int bad_imd_sector_capacity = 0;


    if (!disk_layout || !disk_layout->directory || !img_stream) {
        fprintf(stderr, "Error: Disk layout, directory, or image stream not loaded.\n");
        return false;
    }

    physical_disk_blocks = get_total_blocks(&disk_layout->fsblock);
    if (physical_disk_blocks > 0) {
        if (physical_disk_blocks > (SIZE_MAX / SECTORS_PER_BLOCK)) { /* Check for overflow before multiplication */
            physical_disk_sectors = UINT32_MAX; /* Clamp or indicate error */
            fprintf(stderr, "Warning: Calculated physical_disk_blocks (%zu) would overflow physical_disk_sectors. Clamping.\n", physical_disk_blocks);
        }
        else {
            physical_disk_sectors = (uint32_t)(physical_disk_blocks * SECTORS_PER_BLOCK);
        }
    }
    else {
        physical_disk_sectors = 0;
    }


    if (disk_layout->alloc_map.map_data) {
        map_max_blocks = get_allocation_map_maximum_blocks(&disk_layout->alloc_map);
        if (map_max_blocks > 0) {
            if (map_max_blocks > (SIZE_MAX / SECTORS_PER_BLOCK)) { /* Check for overflow */
                total_map_sectors_capacity = UINT32_MAX; /* Clamp or indicate error */
                fprintf(stderr, "Warning: map_max_blocks (%zu) would overflow total_map_sectors_capacity. Clamping.\n", map_max_blocks);
            }
            else {
                total_map_sectors_capacity = (uint32_t)(map_max_blocks * SECTORS_PER_BLOCK);
            }
        }
        else {
            total_map_sectors_capacity = 0;
        }


        if (total_map_sectors_capacity > 0) {
            uint32_t additional_am_sectors;
            uint32_t am_start_lba;
            uint32_t dir_start_lba;

            bitmap_size_bytes = (total_map_sectors_capacity + 7) / 8;
            overall_sector_bitmap = (uint8_t*)calloc(bitmap_size_bytes, 1);
            if (!overall_sector_bitmap) {
                perror("Error allocating overall sector bitmap");
                total_map_sectors_capacity = 0; /* Indicate bitmap failure */
            }
            else {
                additional_am_sectors = disk_layout->fsblock.fs_flags & ADDITIONAL_AM_SECTORS_MASK;

                printf("Marking system reserved areas...\n");

                if (0 < total_map_sectors_capacity) { /* Check index valid before use */
                    set_sector_bitmap_bit(overall_sector_bitmap, 0);
                }
                if (opts->verbose) {
                    printf("  Marked LBA 0 (Boot Sector) as used.\n");
                }

                if (1 < total_map_sectors_capacity) { /* Check index valid before use */
                    set_sector_bitmap_bit(overall_sector_bitmap, 1);
                }
                if (opts->verbose) {
                    printf("  Marked LBA 1 (FS Block/Start of AM) as used.\n");
                }

                am_start_lba = 2;
                for (i = 0; i < additional_am_sectors; ++i) {
                    if ((am_start_lba + i) < total_map_sectors_capacity) {
                        set_sector_bitmap_bit(overall_sector_bitmap, am_start_lba + i);
                    }
                }
                if (opts->verbose && additional_am_sectors > 0) {
                    printf("  Marked LBAs %u to %u (Extra AM Sectors) as used.\n", am_start_lba, am_start_lba + additional_am_sectors - 1);
                }

                dir_start_lba = 1 + 1 + additional_am_sectors;
                for (i = 0; i < disk_layout->fsblock.dir_sectors_max; ++i) {
                    if ((dir_start_lba + i) < total_map_sectors_capacity) {
                        set_sector_bitmap_bit(overall_sector_bitmap, dir_start_lba + i);
                    }
                }
                if (opts->verbose && disk_layout->fsblock.dir_sectors_max > 0) {
                    printf("  Marked LBAs %u to %u (Directory Sectors) as used.\n", dir_start_lba, dir_start_lba + disk_layout->fsblock.dir_sectors_max - 1);
                }

                if (physical_disk_sectors < total_map_sectors_capacity) {
                    if (opts->verbose) {
                        printf("  Marking sectors from %u to %u (beyond physical disk) as used in bitmap.\n", physical_disk_sectors, total_map_sectors_capacity - 1);
                    }
                    for (i = physical_disk_sectors; i < total_map_sectors_capacity; ++i) {
                        set_sector_bitmap_bit(overall_sector_bitmap, i);
                    }
                }

                max_claims_possible = 0;
                for (size_t j = 0; j < disk_layout->directory->directory_size_bytes / sizeof(directory_entry_block_t); ++j) {
                    const directory_entry_block_t* deb = &disk_layout->directory->directory[j];
                    if (oasis_deb_is_valid(deb)) {
                        size_t sectors_for_deb = (size_t)deb->block_count * SECTORS_PER_BLOCK;
                        if ((unsigned int)max_claims_possible > (unsigned int)INT_MAX - sectors_for_deb) {
                            max_claims_possible = INT_MAX;
                            fprintf(stderr, "Warning: Potential overflow for sector_claims array size. Clamped.\n");
                            break;
                        }
                        max_claims_possible += (int)sectors_for_deb;
                    }
                }
                if (max_claims_possible == 0 && disk_layout->directory->directory_size_bytes > 0) {
                    max_claims_possible = 1000;
                }
                else if (max_claims_possible == 0) {
                    max_claims_possible = 1;
                }
            }
        }
    }
    if (max_claims_possible == 0) { /* Ensure it's at least 1 for malloc */
        max_claims_possible = 1;
    }

    sector_claims = (sector_claim_t*)malloc(sizeof(sector_claim_t) * (size_t)max_claims_possible);
    if (!sector_claims && total_map_sectors_capacity > 0) { /* Only error if we needed it */
        perror("Error allocating sector claims array");
        /* May need to free overall_sector_bitmap if allocated */
        if (overall_sector_bitmap) free(overall_sector_bitmap);
        return false; /* Cannot proceed without sector_claims if bitmap was intended */
    }

    /* --- IMD Bad Sector Identification --- */
    if (strcmp(img_stream->image_type, "IMD") == 0 && img_stream->imdf_handle != NULL) {
        size_t num_imd_tracks = 0;
        uint32_t current_oasis_lba_map = 0;
        printf("\n--- IMD Bad Sector Check ---\n");
        imdf_get_num_tracks(img_stream->imdf_handle, &num_imd_tracks);

        for (j_track = 0; j_track < num_imd_tracks; ++j_track) {
            const ImdTrackInfo* track = imdf_get_track_info(img_stream->imdf_handle, j_track);
            if (!track || !track->loaded) continue;

            for (uint8_t s_idx = 0; s_idx < track->num_sectors; ++s_idx) {
                bool current_imd_sector_is_bad = false;
                uint8_t current_sflag = track->sflag[s_idx];

                if (current_sflag == IMD_SDR_HAS_ERR(current_sflag) ||
                    current_sflag == IMD_SDR_UNAVAILABLE) {
                    current_imd_sector_is_bad = true;
                }

                if (current_imd_sector_is_bad) {
                    if (bad_imd_sector_count >= bad_imd_sector_capacity) {
                        bad_imd_sector_capacity = (bad_imd_sector_capacity == 0) ? 16 : bad_imd_sector_capacity * 2;
                        imd_bad_sector_info_t* new_list = (imd_bad_sector_info_t*)realloc(bad_imd_sectors_list, bad_imd_sector_capacity * sizeof(imd_bad_sector_info_t));
                        if (!new_list) {
                            perror("Error reallocating bad_imd_sectors_list");
                            /* Continue without storing more bad sectors if realloc fails */
                            bad_imd_sector_capacity = bad_imd_sector_count; /* Prevent further attempts */
                        }
                        else {
                            bad_imd_sectors_list = new_list;
                        }
                    }
                    if (bad_imd_sector_count < bad_imd_sector_capacity) {
                        bad_imd_sectors_list[bad_imd_sector_count].oasis_lba = current_oasis_lba_map;
                        bad_imd_sectors_list[bad_imd_sector_count].imd_track_cyl = track->cyl;
                        bad_imd_sectors_list[bad_imd_sector_count].imd_track_head = track->head;
                        bad_imd_sectors_list[bad_imd_sector_count].imd_sector_id = track->smap[s_idx];
                        bad_imd_sectors_list[bad_imd_sector_count].imd_sector_flag = current_sflag;
                        bad_imd_sectors_list[bad_imd_sector_count].imd_sector_size = (uint16_t)track->sector_size;
                        bad_imd_sector_count++;
                        printf("  IMD Bad Sector: Cyl %u, Head %u, ID %u (OASIS LBA %u), Flag 0x%02X, IMD Size %u\n",
                            track->cyl, track->head, track->smap[s_idx], current_oasis_lba_map, current_sflag, track->sector_size);
                        errors_found++;
                    }
                }
                /* Advance OASIS LBA based on IMD sector size */
                if (track->sector_size == 256) {
                    current_oasis_lba_map++;
                }
                else if (track->sector_size == 128) {
                    /* A pair of 128-byte sectors form one 256-byte OASIS LBA.
                     * We advance the OASIS LBA only after processing two 128-byte IMD sectors.
                     */
                    if ((s_idx + 1) % 2 != 0) { /* If s_idx is even (0, 2, 4...), this is the first of a pair */
                        /* For the first of a pair, if it's bad, the LBA is bad. */
                        /* If the *second* of a pair is bad, the LBA is also bad, and current_oasis_lba_map
                           would be the same as for the first of the pair.
                           The logic for adding to bad_imd_sectors_list correctly uses current_oasis_lba_map.
                        */
                    }
                    else { /* s_idx is odd (1, 3, 5...), this is the second of a pair */
                        current_oasis_lba_map++;
                    }
                }
            }
        }
        if (bad_imd_sector_count == 0) {
            printf("  No bad sectors found in IMD image based on flags.\n");
        }
    }


    printf("\n--- Starting File Consistency Checks ---\n");
    if (opts->file_pattern) {
        printf("Filtering files with pattern: %s\n", opts->file_pattern);
    }
    if (opts->verbose) {
        printf("Verbose mode enabled.\n");
    }
    printf("Physical Disk: %zu Blocks (%u Sectors)\n", physical_disk_blocks, physical_disk_sectors);
    printf("Allocation Map Capacity: %zu Blocks (%u Sectors)\n", map_max_blocks, total_map_sectors_capacity);

    num_entries = disk_layout->directory->directory_size_bytes / sizeof(directory_entry_block_t);

    for (size_t deb_idx = 0; deb_idx < num_entries; ++deb_idx) {
        const directory_entry_block_t* deb = &disk_layout->directory->directory[deb_idx];
        char current_file_host_name[MAX_HOST_FILENAME_LEN];
        bool should_check_this_file = true;
        bool current_file_errors = false;

        if (!oasis_deb_is_valid(deb)) {
            if (deb->file_format != FILE_FORMAT_EMPTY && deb->file_format != FILE_FORMAT_DELETED) {
                printf("DEB #%zu: Invalid DEB entry (Format: 0x%02X). Skipping further checks.\n", deb_idx, deb->file_format);
                errors_found++;
            }
            else if (opts->verbose) {
                printf("DEB #%zu: Entry is %s. Skipping.\n", deb_idx, deb->file_format == FILE_FORMAT_EMPTY ? "EMPTY" : "DELETED");
            }
            continue;
        }

        if (!oasis_deb_to_host_filename(deb, current_file_host_name, sizeof(current_file_host_name))) {
            snprintf(current_file_host_name, sizeof(current_file_host_name), "[DEB #%zu Name Error]", deb_idx);
        }

        if (opts->file_pattern) {
            if (!oasis_filename_wildcard_match(deb->file_name, deb->file_type, opts->file_pattern)) {
                should_check_this_file = false;
            }
        }

        if (!should_check_this_file) {
            continue;
        }

        files_checked++;
        printf("\n--- Checking File: %s (DEB #%zu) ---\n", current_file_host_name, deb_idx);

        if (!check_deb_integrity(deb, (int)deb_idx, disk_layout, opts->verbose)) {
            current_file_errors = true;
        }

        if (total_map_sectors_capacity > 0 && overall_sector_bitmap != NULL && sector_claims != NULL) {
            if (!check_deb_alloc_map_consistency(deb, (int)deb_idx, current_file_host_name, disk_layout, img_stream,
                overall_sector_bitmap, total_map_sectors_capacity,
                sector_claims, &next_sector_claim_idx, max_claims_possible,
                opts->verbose)) {
                current_file_errors = true;
            }
        }
        else {
            printf("  Skipping Allocation Map & Shared Sector checks (pre-requisite data missing or bitmap allocation failed).\n");
        }

        bool is_contiguous_flag = true; /* Default for non-sequential or empty */
        uint32_t actual_sector_count_in_chain = 0;

        if ((deb->file_format & FILE_FORMAT_MASK) == FILE_FORMAT_SEQUENTIAL) {
            if (!check_sequential_file_linkage(deb, (int)deb_idx, disk_layout, img_stream,
                sector_buffer_for_seq, &is_contiguous_flag,
                &actual_sector_count_in_chain, opts->verbose)) {
                current_file_errors = true;
            }
            else {
                if (opts->verbose || !current_file_errors) {
                    printf("  Sequential Linkage: OK.\n");
                }
            }
        }
        else { /* For contiguous files, calculate sectors directly */
            if (deb->block_count > 0) {
                actual_sector_count_in_chain = (uint32_t)deb->block_count * SECTORS_PER_BLOCK;
            }
        }
        if (opts->verbose || (!current_file_errors && deb->block_count > 0)) { /* Only print contiguity if relevant or no other errors */
            printf("  Contiguity: %s.\n", is_contiguous_flag ? "Yes" : "No");
        }
        if (opts->verbose && actual_sector_count_in_chain > 0) {
            printf("  Sectors claimed by file: %u.\n", actual_sector_count_in_chain);
        }


        /* Check if this file uses any of the bad IMD sectors */
        if (bad_imd_sector_count > 0) {
            bool file_uses_bad_sector = false;
            uint32_t first_bad_lba_found_for_file = 0;

            if ((deb->file_format & FILE_FORMAT_MASK) == FILE_FORMAT_SEQUENTIAL) {
                uint16_t current_lba_seq = deb->start_sector;
                uint32_t seq_sector_idx = 0;
                const uint32_t MAX_SEQ_CHECK_BAD = total_map_sectors_capacity > 0 ? total_map_sectors_capacity : 65535;
                uint8_t temp_seq_buf[SECTOR_SIZE];

                while (current_lba_seq != 0 && seq_sector_idx < MAX_SEQ_CHECK_BAD) {
                    for (int k = 0; k < bad_imd_sector_count; ++k) {
                        if (bad_imd_sectors_list[k].oasis_lba == current_lba_seq) {
                            file_uses_bad_sector = true;
                            first_bad_lba_found_for_file = current_lba_seq;
                            goto found_bad_sector_for_file_check; /* Exit loops */
                        }
                    }
                    if (sector_io_read(img_stream, current_lba_seq, 1, temp_seq_buf) != 1) break; /* Error reading chain */
                    memcpy(&current_lba_seq, temp_seq_buf + OASIS_SEQ_DATA_PER_SECTOR, sizeof(uint16_t));
                    seq_sector_idx++;
                }
            }
            else { /* Contiguous file */
                uint32_t num_sectors_in_file = (uint32_t)deb->block_count * SECTORS_PER_BLOCK;
                for (uint32_t s_off = 0; s_off < num_sectors_in_file; ++s_off) {
                    uint32_t file_lba = deb->start_sector + s_off;
                    for (int k = 0; k < bad_imd_sector_count; ++k) {
                        if (bad_imd_sectors_list[k].oasis_lba == file_lba) {
                            file_uses_bad_sector = true;
                            first_bad_lba_found_for_file = file_lba;
                            goto found_bad_sector_for_file_check; /* Exit loops */
                        }
                    }
                }
            }
        found_bad_sector_for_file_check:
            if (file_uses_bad_sector) {
                printf("    ERROR: File '%s' uses a bad IMD sector at OASIS LBA %u. File is corrupt.\n",
                    current_file_host_name, first_bad_lba_found_for_file);
                current_file_errors = true; /* This is now an error */
            }
        }


        if (current_file_errors) {
            errors_found++;
            printf("--- Errors found for file: %s ---\n", current_file_host_name);
        }
        else {
            printf("--- All checks OK for file: %s ---\n", current_file_host_name);
        }
    }


    if ((disk_layout->alloc_map.map_data && overall_sector_bitmap) && (!opts->file_pattern)) {
        int orphaned_blocks_found = 0;
        size_t check_limit_blocks = map_max_blocks;

        printf("\n--- Orphaned Allocated Blocks ---\n");

        for (size_t block_idx = 0; block_idx < check_limit_blocks; ++block_idx) {
            int alloc_map_state;
            if (get_block_state(&disk_layout->alloc_map, block_idx, &alloc_map_state) == 0) {
                if (alloc_map_state == 1) {
                    bool block_is_accounted_for = false;
                    for (uint32_t s_offset = 0; s_offset < SECTORS_PER_BLOCK; ++s_offset) {
                        uint32_t sector_lba = (uint32_t)(block_idx * SECTORS_PER_BLOCK) + s_offset;
                        if (sector_lba < total_map_sectors_capacity) {
                            if (get_sector_bitmap_bit(overall_sector_bitmap, sector_lba)) {
                                block_is_accounted_for = true;
                                break;
                            }
                        }
                    }

                    if (!block_is_accounted_for) {
                        if (block_idx < physical_disk_blocks) {
                            printf("    WARNING: Allocation Map Block %zu (Sectors %u-%u) is marked as USED, but no file or system area claims its sectors.\n",
                                block_idx,
                                (unsigned int)(block_idx * SECTORS_PER_BLOCK),
                                (unsigned int)(block_idx * SECTORS_PER_BLOCK + SECTORS_PER_BLOCK - 1));
                            orphaned_blocks_found++;
                            errors_found++;
                        }
                        else if (opts->verbose) {
                            printf("    INFO: Allocation Map Block %zu (beyond physical disk) is marked as USED but not found in overall bitmap (expected to be pre-marked if truly unused by files).\n", block_idx);
                        }
                    }
                }
            }
        }
        if (orphaned_blocks_found == 0) {
            printf("  No orphaned allocated blocks found within physical disk limits.\n");
        }
        else {
            printf("  Found %d orphaned allocated block(s).\n", orphaned_blocks_found);
        }
    }

    printf("\n--- Summary ---\n");
    printf("Total DEB entries processed (excluding empty/deleted): %d\n", files_checked);
    printf("Total errors/warnings found: %d\n", errors_found);

    if (overall_sector_bitmap) {
        free(overall_sector_bitmap);
    }
    if (sector_claims) {
        free(sector_claims);
    }
    if (bad_imd_sectors_list) {
        free(bad_imd_sectors_list);
    }

    return (errors_found > 0) ? false : true;
}


static bool check_deb_integrity(const directory_entry_block_t* deb,
    int deb_index,
    const oasis_disk_layout_t* disk_layout,
    bool verbose) {
    bool ok = true;
    size_t map_max_blocks = 0;
    uint32_t map_max_sectors = 0;
    uint8_t file_type_bits = deb->file_format & FILE_FORMAT_MASK;


    if (verbose) {
        printf("  DEB Integrity Checks for DEB #%d:\n", deb_index);
    }

    if (disk_layout->alloc_map.map_data) {
        map_max_blocks = get_allocation_map_maximum_blocks(&disk_layout->alloc_map);
        if (map_max_blocks > 0 && SECTORS_PER_BLOCK > 0) {
            if (map_max_blocks > (SIZE_MAX / SECTORS_PER_BLOCK)) {
                map_max_sectors = UINT32_MAX;
                if (verbose) {
                    printf("    Note: Total disk blocks representable by map (%zu) is very large, map_max_sectors clamped.\n", map_max_blocks);
                }
            }
            else {
                map_max_sectors = (uint32_t)(map_max_blocks * SECTORS_PER_BLOCK);
            }
        }
        else {
            map_max_sectors = 0;
        }
    }

    switch (file_type_bits) {
    case FILE_FORMAT_RELOCATABLE:
    case FILE_FORMAT_ABSOLUTE:
    case FILE_FORMAT_SEQUENTIAL:
    case FILE_FORMAT_DIRECT:
    case FILE_FORMAT_INDEXED:
    case FILE_FORMAT_KEYED:
        if (verbose) {
            printf("    File Format (0x%02X): Valid type bits (0x%02X).\n", deb->file_format, file_type_bits);
        }
        break;
    default:
        printf("    ERROR: DEB #%d: Invalid file type bits in file_format (0x%02X).\n", deb_index, file_type_bits);
        ok = false;
    }

    if (map_max_blocks > 0 && deb->block_count > map_max_blocks) {
        printf("    ERROR: DEB #%d: block_count (%u) exceeds total disk blocks representable by map (%zu).\n",
            deb_index, deb->block_count, map_max_blocks);
        ok = false;
    }
    else if (verbose) {
        printf("    Block Count (%u): Appears reasonable relative to map capacity.\n", deb->block_count);
    }

    if (deb->block_count > 0) {
        if (map_max_sectors > 0 && deb->start_sector >= map_max_sectors) {
            printf("    ERROR: DEB #%d: start_sector (%u) is out of map-described disk bounds (total map sectors %u).\n",
                deb_index, deb->start_sector, map_max_sectors);
            ok = false;
        }
        else if (deb->start_sector == 0 && file_type_bits != FILE_FORMAT_SEQUENTIAL) {
            if (deb->block_count > 0) {
                printf("    WARNING: DEB #%d: Non-sequential file has start_sector 0 but block_count %u > 0.\n",
                    deb_index, deb->block_count);
                /* This might be an error for some systems, but a warning for chkdsk */
            }
        }
        else if (deb->start_sector == 0 && file_type_bits == FILE_FORMAT_SEQUENTIAL && deb->block_count > 0) {
            printf("    WARNING: DEB #%d: Sequential file with block_count %u > 0 has start_sector 0.\n",
                deb_index, deb->block_count);
        }

        else if (verbose) {
            printf("    Start Sector (%u): Appears reasonable.\n", deb->start_sector);
        }
    }


    if (file_type_bits == FILE_FORMAT_SEQUENTIAL) {
        if (map_max_sectors > 0 && deb->file_format_dependent2 >= map_max_sectors && deb->file_format_dependent2 != 0) {
            printf("    ERROR: DEB #%d (Sequential): Last sector address (FFD2: %u) is out of map-described disk bounds (%u).\n",
                deb_index, deb->file_format_dependent2, map_max_sectors);
            ok = false;
        }
        else if (verbose) {
            printf("    FFD1 (Seq RecLen: %u), FFD2 (Seq LastSect: %u)\n",
                deb->file_format_dependent1, deb->file_format_dependent2);
        }
    }
    else if (file_type_bits == FILE_FORMAT_DIRECT) {
        if (verbose) {
            printf("    FFD1 (Dir RecLen: %u), FFD2 (Dir: %u, expected 0)\n",
                deb->file_format_dependent1, deb->file_format_dependent2);
        }
        if (deb->file_format_dependent2 != 0) {
            printf("    WARNING: DEB #%d (Direct): file_format_dependent2 is %u, expected 0.\n",
                deb_index, deb->file_format_dependent2);
            /* Not strictly an error if system ignores it, but unusual. */
        }
    }


    struct tm check_tm;
    oasis_convert_timestamp_to_tm(&deb->timestamp, &check_tm);
    if (check_tm.tm_mon < 0 || check_tm.tm_mon > 11 || check_tm.tm_mday < 1 || check_tm.tm_mday > 31) {
        printf("    WARNING: DEB #%d: Timestamp seems invalid (month/day out of typical range after conversion).\n", deb_index);
        /* This might be a soft error or just a note. */
    }
    else if (verbose) {
        char time_str[20];
        oasis_time_str(time_str, sizeof(time_str), &deb->timestamp);
        printf("    Timestamp: %s (raw: %02X %02X %02X)\n", time_str, deb->timestamp.raw[0], deb->timestamp.raw[1], deb->timestamp.raw[2]);
    }


    if (!ok) {
        printf("    DEB Integrity: FAILED for DEB #%d.\n", deb_index);
    }
    else if (verbose) {
        printf("    DEB Integrity: OK for DEB #%d.\n", deb_index);
    }
    return ok;
}


static bool check_deb_alloc_map_consistency(const directory_entry_block_t* deb,
    int deb_index,
    const char* current_file_host_name,
    const oasis_disk_layout_t* disk_layout,
    sector_io_stream_t* img_stream,
    uint8_t* overall_sector_bitmap,
    uint32_t total_map_sectors_capacity,
    sector_claim_t* sector_claims,
    int* next_sector_claim_idx,
    int max_sector_claims,
    bool verbose) {
    bool ok = true;
    uint8_t file_type_bits = deb->file_format & FILE_FORMAT_MASK;
    uint32_t sectors_in_file_processed = 0;
    uint8_t temp_sector_data[SECTOR_SIZE];
    uint32_t current_block_for_alloc_map;
    int alloc_state;

    if (verbose) {
        printf("  Allocation Map Consistency for DEB #%d (%s):\n", deb_index, current_file_host_name);
    }

    if (deb->block_count == 0) {
        if (verbose) {
            printf("    File has 0 blocks, skipping alloc map check.\n");
        }
        return true;
    }

    if (file_type_bits == FILE_FORMAT_SEQUENTIAL) {
        uint16_t current_lba = deb->start_sector;
        uint16_t next_lba;
        uint32_t sector_count_in_chain = 0;
        const uint32_t MAX_SEQ_SECTORS_CHECK = total_map_sectors_capacity > 0 ? total_map_sectors_capacity : 65535;


        while (current_lba != 0 && sector_count_in_chain < MAX_SEQ_SECTORS_CHECK) {
            int previous_claimer_idx;
            const char* previous_filename;

            if (total_map_sectors_capacity > 0 && current_lba >= total_map_sectors_capacity) {
                printf("    ERROR: DEB #%d (Seq): Sector LBA %u in chain is out of map-described disk bounds (%u).\n", deb_index, current_lba, total_map_sectors_capacity);
                return false; /* Critical error, stop checking this file */
            }

            if (get_sector_bitmap_bit(overall_sector_bitmap, current_lba)) {
                previous_claimer_idx = -1;
                previous_filename = "[Unknown/System]"; /* Default if not found in claims */
                if (sector_claims) {
                    for (int sc_idx = 0; sc_idx < *next_sector_claim_idx; ++sc_idx) {
                        if (sector_claims[sc_idx].sector_lba == current_lba) {
                            previous_claimer_idx = sector_claims[sc_idx].deb_index;
                            previous_filename = sector_claims[sc_idx].filename;
                            break;
                        }
                    }
                }
                printf("    ERROR: DEB #%d (%s): Sector LBA %u is SHARED! (Previously claimed by DEB #%d '%s' or system area).\n",
                    deb_index, current_file_host_name, current_lba, previous_claimer_idx, previous_filename);
                ok = false;
            }
            else {
                set_sector_bitmap_bit(overall_sector_bitmap, current_lba);
                if (sector_claims && *next_sector_claim_idx < max_sector_claims) {
                    sector_claims[*next_sector_claim_idx].sector_lba = current_lba;
                    sector_claims[*next_sector_claim_idx].deb_index = deb_index;
                    strncpy(sector_claims[*next_sector_claim_idx].filename, current_file_host_name, MAX_HOST_FILENAME_LEN - 1);
                    sector_claims[*next_sector_claim_idx].filename[MAX_HOST_FILENAME_LEN - 1] = '\0';
                    (*next_sector_claim_idx)++;
                }
                else if (sector_claims && *next_sector_claim_idx >= max_sector_claims) {
                    fprintf(stderr, "Warning: Sector claims array full, cannot record claim for LBA %u by %s.\n", current_lba, current_file_host_name);
                }
            }

            current_block_for_alloc_map = current_lba / SECTORS_PER_BLOCK;
            if (get_block_state(&disk_layout->alloc_map, current_block_for_alloc_map, &alloc_state) == 0) {
                if (alloc_state == 0) {
                    printf("    ERROR: DEB #%d (Seq): Sector LBA %u (Block %u) is part of file, but alloc map says block is FREE.\n",
                        deb_index, current_lba, current_block_for_alloc_map);
                    ok = false;
                }
            }
            else {
                printf("    ERROR: DEB #%d (Seq): Failed to get alloc state for block %u (from LBA %u).\n",
                    deb_index, current_block_for_alloc_map, current_lba);
                ok = false;
            }

            if (sector_io_read(img_stream, current_lba, 1, temp_sector_data) != 1) {
                printf("    ERROR: DEB #%d (Seq): Failed to read sector %u from image to follow chain.\n", deb_index, current_lba);
                return false; /* Critical error */
            }
            memcpy(&next_lba, temp_sector_data + OASIS_SEQ_DATA_PER_SECTOR, sizeof(uint16_t));
            current_lba = next_lba;
            sector_count_in_chain++;
        }
        if (sector_count_in_chain >= MAX_SEQ_SECTORS_CHECK && current_lba != 0) {
            printf("    ERROR: DEB #%d (Seq): Sector chain is too long or possibly cyclic (checked %u sectors).\n", deb_index, MAX_SEQ_SECTORS_CHECK);
            ok = false;
        }
        sectors_in_file_processed = sector_count_in_chain;

    }
    else { /* Contiguous files */
        uint32_t num_sectors_in_deb = (uint32_t)deb->block_count * SECTORS_PER_BLOCK;
        for (uint32_t s_offset = 0; s_offset < num_sectors_in_deb; ++s_offset) {
            uint32_t current_lba = deb->start_sector + s_offset;
            int previous_claimer_idx;
            const char* previous_filename;

            if (total_map_sectors_capacity > 0 && current_lba >= total_map_sectors_capacity) {
                printf("    ERROR: DEB #%d (Contig): Sector LBA %u in file extent is out of map-described disk bounds (%u).\n", deb_index, current_lba, total_map_sectors_capacity);
                ok = false;
                break; /* Stop checking this file's sectors */
            }

            if (get_sector_bitmap_bit(overall_sector_bitmap, current_lba)) {
                previous_claimer_idx = -1;
                previous_filename = "[Unknown/System]";
                if (sector_claims) {
                    for (int sc_idx = 0; sc_idx < *next_sector_claim_idx; ++sc_idx) {
                        if (sector_claims[sc_idx].sector_lba == current_lba) {
                            previous_claimer_idx = sector_claims[sc_idx].deb_index;
                            previous_filename = sector_claims[sc_idx].filename;
                            break;
                        }
                    }
                }
                printf("    ERROR: DEB #%d (%s): Sector LBA %u is SHARED! (Previously claimed by DEB #%d '%s' or system area).\n",
                    deb_index, current_file_host_name, current_lba, previous_claimer_idx, previous_filename);
                ok = false;
            }
            else {
                set_sector_bitmap_bit(overall_sector_bitmap, current_lba);
                if (sector_claims && *next_sector_claim_idx < max_sector_claims) {
                    sector_claims[*next_sector_claim_idx].sector_lba = current_lba;
                    sector_claims[*next_sector_claim_idx].deb_index = deb_index;
                    strncpy(sector_claims[*next_sector_claim_idx].filename, current_file_host_name, MAX_HOST_FILENAME_LEN - 1);
                    sector_claims[*next_sector_claim_idx].filename[MAX_HOST_FILENAME_LEN - 1] = '\0';
                    (*next_sector_claim_idx)++;
                }
                else if (sector_claims && *next_sector_claim_idx >= max_sector_claims) {
                    fprintf(stderr, "Warning: Sector claims array full, cannot record claim for LBA %u by %s.\n", current_lba, current_file_host_name);
                }
            }

            current_block_for_alloc_map = current_lba / SECTORS_PER_BLOCK;
            if (s_offset % SECTORS_PER_BLOCK == 0) { /* Check only first sector of each 1K block */
                if (get_block_state(&disk_layout->alloc_map, current_block_for_alloc_map, &alloc_state) == 0) {
                    if (alloc_state == 0) {
                        printf("    ERROR: DEB #%d (Contig): Block %u (from LBA %u) is part of file, but alloc map says block is FREE.\n",
                            deb_index, current_block_for_alloc_map, current_lba);
                        ok = false;
                    }
                }
                else {
                    printf("    ERROR: DEB #%d (Contig): Failed to get alloc state for block %u (from LBA %u).\n",
                        deb_index, current_block_for_alloc_map, current_lba);
                    ok = false;
                }
            }
        }
        sectors_in_file_processed = num_sectors_in_deb;
    }

    if (verbose) {
        if (ok) {
            printf("    Allocation Map & Shared Sector Checks: OK (%u sectors processed for this file).\n", sectors_in_file_processed);
        }
        else {
            printf("    Allocation Map & Shared Sector Checks: FAILED for DEB #%d.\n", deb_index);
        }
    }
    return ok;
}


static bool check_sequential_file_linkage(const directory_entry_block_t* deb,
    int deb_index,
    const oasis_disk_layout_t* disk_layout,
    sector_io_stream_t* img_stream,
    uint8_t* temp_sector_buffer,
    bool* is_contiguous,
    uint32_t* actual_sector_count,
    bool verbose) {
    bool ok = true;
    uint16_t current_lba;
    uint16_t expected_last_lba;
    uint16_t previous_lba;
    size_t map_max_blocks = 0;
    uint32_t map_max_sectors = 0;
    uint8_t* visited_sectors_in_chain = NULL;
    size_t visited_bitmap_size = 0;

    *is_contiguous = true; /* Assume contiguous until proven otherwise */
    *actual_sector_count = 0;

    current_lba = deb->start_sector;
    expected_last_lba = deb->file_format_dependent2;
    previous_lba = 0; /* Will be set to current_lba - 1 for first contiguity check if start_sector > 0 */
    if (current_lba > 0) {
        previous_lba = current_lba - 1; /* For the first check of contiguity */
    }


    if (disk_layout->alloc_map.map_data) {
        map_max_blocks = get_allocation_map_maximum_blocks(&disk_layout->alloc_map);
        if (map_max_blocks > 0 && SECTORS_PER_BLOCK > 0) {
            if (map_max_blocks > (SIZE_MAX / SECTORS_PER_BLOCK)) {
                map_max_sectors = UINT32_MAX;
            }
            else {
                map_max_sectors = (uint32_t)(map_max_blocks * SECTORS_PER_BLOCK);
            }
        }
        else {
            map_max_sectors = 0;
        }
    }

    if (map_max_sectors > 0) {
        visited_bitmap_size = (map_max_sectors + 7) / 8;
        visited_sectors_in_chain = (uint8_t*)calloc(visited_bitmap_size, 1);
        if (!visited_sectors_in_chain) {
            perror("    Warning: Could not allocate visited_sectors_in_chain bitmap for cycle detection");
            /* Continue without cycle detection if allocation fails, but it's risky. */
        }
    }

    if (verbose) {
        printf("  Sequential File Linkage for DEB #%d (Start: %u, Expected End: %u):\n",
            deb_index, deb->start_sector, expected_last_lba);
    }

    if (deb->start_sector == 0 && expected_last_lba == 0 && deb->block_count == 0) {
        if (verbose) {
            printf("    File is empty (start_sector=0, FFD2=0, block_count=0). Linkage OK.\n");
        }
        if (visited_sectors_in_chain) free(visited_sectors_in_chain);
        return true;
    }
    if (deb->start_sector == 0 && (expected_last_lba != 0 || deb->block_count != 0)) {
        printf("    ERROR: DEB #%d: File starts at sector 0 but FFD2 (%u) or block_count (%u) is non-zero.\n",
            deb_index, expected_last_lba, deb->block_count);
        if (visited_sectors_in_chain) free(visited_sectors_in_chain);
        return false;
    }


    while (current_lba != 0) {
        uint16_t next_lba;

        if (current_lba != previous_lba + 1) {
            *is_contiguous = false;
        }

        if (map_max_sectors > 0 && current_lba >= map_max_sectors) {
            printf("    ERROR: DEB #%d: Chain link to LBA %u is out of map-described disk bounds (%u).\n",
                deb_index, current_lba, map_max_sectors);
            ok = false;
            break;
        }

        if (visited_sectors_in_chain) {
            if (get_sector_bitmap_bit(visited_sectors_in_chain, current_lba)) {
                printf("    ERROR: DEB #%d: Cycle detected in sector chain at LBA %u.\n", deb_index, current_lba);
                ok = false;
                break;
            }
            set_sector_bitmap_bit(visited_sectors_in_chain, current_lba);
        }

        if (sector_io_read(img_stream, current_lba, 1, temp_sector_buffer) != 1) {
            printf("    ERROR: DEB #%d: Failed to read sector %u for chain link.\n", deb_index, current_lba);
            ok = false;
            break;
        }

        (*actual_sector_count)++;
        /* Check against a reasonable upper bound for sectors based on block_count, allowing some slack */
        uint32_t max_expected_sectors = (uint32_t)deb->block_count * SECTORS_PER_BLOCK;
        if (max_expected_sectors == 0 && deb->block_count > 0) max_expected_sectors = SECTORS_PER_BLOCK; /* Min 1 block if block_count > 0 */

        if (deb->block_count > 0 && *actual_sector_count > (max_expected_sectors + SECTORS_PER_BLOCK * 2)) { /* Allow 2 extra blocks worth of sectors as slack */
            printf("    WARNING: DEB #%d: Number of sectors in chain (%u) significantly exceeds allocated blocks (%u blocks -> %u max expected sectors).\n",
                deb_index, *actual_sector_count, deb->block_count, max_expected_sectors);
            /* This is a warning, not a hard error unless it becomes extreme */
        }


        memcpy(&next_lba, temp_sector_buffer + OASIS_SEQ_DATA_PER_SECTOR, sizeof(uint16_t));

        if (verbose && *actual_sector_count <= 20) { /* Limit verbose link printing */
            printf("    Link: Sector %u -> %u\n", current_lba, next_lba);
        }
        else if (verbose && *actual_sector_count == 21) {
            printf("    (Further link details suppressed for brevity)\n");
        }


        previous_lba = current_lba;
        current_lba = next_lba;

        /* Safety break for extremely long chains, independent of map_max_sectors if map is not available */
        uint32_t absolute_max_check = (map_max_sectors > 0) ? (map_max_sectors + 5) : (65535 + 5);
        if (*actual_sector_count > absolute_max_check) {
            printf("    ERROR: DEB #%d: Sequential chain appears excessively long (> %u sectors). Aborting check for this file.\n", deb_index, *actual_sector_count);
            ok = false;
            break;
        }
    }

    if (ok) { /* Only do final checks if no major error broke the loop */
        if (current_lba == 0) { /* Chain terminated as expected */
            if (previous_lba != expected_last_lba) {
                printf("    ERROR: DEB #%d: Last sector in chain (LBA %u) does not match DEB FFD2 (expected LBA %u).\n",
                    deb_index, previous_lba, expected_last_lba);
                ok = false;
            }
            else if (verbose) {
                printf("    Chain terminated correctly at LBA 0. Last data sector was %u (matches FFD2).\n", previous_lba);
            }
        }
        /* If current_lba != 0 here, it means a 'break' occurred due to error (cycle, read fail, bounds) */
        /* The error would have already been printed. */
    }

    if (visited_sectors_in_chain) {
        free(visited_sectors_in_chain);
    }
    return ok;
}
