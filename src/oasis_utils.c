/* src/oasis_utils.c */
#include "oasis.h" /* This will now include oasis_endian.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

/* ... (kPathSeparator definition) ... */
const char kPathSeparator =
#ifdef _WIN32
'\\';
#else
'/';
#endif /* _WIN32 */


bool load_oasis_disk(sector_io_stream_t* img_stream, oasis_disk_layout_t* disk_layout) {
    uint8_t sector_buffer[SECTOR_SIZE];
    size_t map_size_in_sector1;
    ssize_t additional_am_sectors = 0; /* Use ssize_t for return from sector_io_read */
    const size_t OASIS_MAX_DISK_BLOCKS_CONST = OASIS_MAX_FS_BLOCKS;
    const size_t MAX_ALLOC_MAP_BYTES = OASIS_MAX_DISK_BLOCKS_CONST / 8;
    size_t calculated_map_size_bytes_from_image;

    if (!disk_layout || !img_stream) {
        fprintf(stderr, "load_oasis_disk: Error - Invalid arguments (null img_stream or disk_layout).\n");
        return false;
    }
    memset(disk_layout, 0, sizeof(oasis_disk_layout_t));

    if (sector_io_read(img_stream, 0, 1, (uint8_t*)&disk_layout->boot) != 1) {
        fprintf(stderr, "load_oasis_disk: Error reading boot sector.\n");
        return false;
    }

    if (sector_io_read(img_stream, 1, 1, sector_buffer) != 1) {
        fprintf(stderr, "load_oasis_disk: Error reading sector 1 (filesystem block and initial AM).\n");
        return false;
    }
    memcpy(&disk_layout->fsblock, sector_buffer, sizeof(filesystem_block_t));

    /* Convert filesystem_block_t uint16_t fields to host byte order */
    disk_layout->fsblock.reserved = le16toh(disk_layout->fsblock.reserved);
    disk_layout->fsblock.free_blocks = le16toh(disk_layout->fsblock.free_blocks);
    /* fs_flags is uint8_t, no conversion needed for it */

    additional_am_sectors = disk_layout->fsblock.fs_flags & ADDITIONAL_AM_SECTORS_MASK;
    map_size_in_sector1 = SECTOR_SIZE - sizeof(filesystem_block_t);
    calculated_map_size_bytes_from_image = map_size_in_sector1 + ((size_t)additional_am_sectors * SECTOR_SIZE);

    if (calculated_map_size_bytes_from_image > MAX_ALLOC_MAP_BYTES) {
        fprintf(stderr, "load_oasis_disk: Error - Disk image's allocation map size (%zu bytes based on additional_am_sectors=%zd) "
            "exceeds maximum allowed %zu bytes (for %zu blocks).\n",
            calculated_map_size_bytes_from_image, additional_am_sectors,
            MAX_ALLOC_MAP_BYTES, OASIS_MAX_DISK_BLOCKS_CONST);
        return false;
    }
    disk_layout->alloc_map.map_size_bytes = calculated_map_size_bytes_from_image;

    if (disk_layout->alloc_map.map_size_bytes > 0) {
        disk_layout->alloc_map.map_data = (uint8_t*)malloc(disk_layout->alloc_map.map_size_bytes);
        if (!disk_layout->alloc_map.map_data) {
            perror("load_oasis_disk: Error allocating memory for allocation map");
            return false;
        }
        if (map_size_in_sector1 > 0) {
            memcpy(disk_layout->alloc_map.map_data, sector_buffer + sizeof(filesystem_block_t), map_size_in_sector1);
        }
        if (additional_am_sectors > 0) {
            if (sector_io_read(img_stream, 2, (uint32_t)additional_am_sectors, disk_layout->alloc_map.map_data + map_size_in_sector1) != additional_am_sectors) {
                fprintf(stderr, "load_oasis_disk: Error reading additional allocation map sectors.\n");
                free(disk_layout->alloc_map.map_data); disk_layout->alloc_map.map_data = NULL;
                return false;
            }
        }
    }
    else {
        disk_layout->alloc_map.map_data = NULL;
    }

    size_t num_dir_entries = (size_t)disk_layout->fsblock.dir_sectors_max * (SECTOR_SIZE / sizeof(directory_entry_block_t));
    size_t dir_size_bytes = num_dir_entries * sizeof(directory_entry_block_t);
    uint32_t dir_start_sector = 1 + 1 + (uint32_t)additional_am_sectors;
    uint16_t dir_sectors_to_read = disk_layout->fsblock.dir_sectors_max;

    if (dir_size_bytes == 0) {
        disk_layout->directory = (oasis_directory_t*)malloc(sizeof(oasis_directory_t));
        if (!disk_layout->directory) {
            perror("load_oasis_disk: Error allocating memory for empty directory structure");
            if (disk_layout->alloc_map.map_data) { free(disk_layout->alloc_map.map_data); disk_layout->alloc_map.map_data = NULL; }
            return false;
        }
        disk_layout->directory->directory_size_bytes = 0;
    }
    else {
        disk_layout->directory = (oasis_directory_t*)malloc(sizeof(oasis_directory_t) + dir_size_bytes);
        if (!disk_layout->directory) {
            perror("load_oasis_disk: Error allocating memory for directory");
            if (disk_layout->alloc_map.map_data) { free(disk_layout->alloc_map.map_data); disk_layout->alloc_map.map_data = NULL; }
            return false;
        }
        disk_layout->directory->directory_size_bytes = dir_size_bytes;
        if (dir_sectors_to_read > 0) {
            if (sector_io_read(img_stream, dir_start_sector, dir_sectors_to_read, (uint8_t*)disk_layout->directory->directory) != dir_sectors_to_read) {
                fprintf(stderr, "load_oasis_disk: Error reading directory sectors.\n");
                if (disk_layout->alloc_map.map_data) { free(disk_layout->alloc_map.map_data); disk_layout->alloc_map.map_data = NULL; }
                free(disk_layout->directory); disk_layout->directory = NULL;
                return false;
            }
            /* After reading the raw directory data, convert DEB uint16_t fields */
            for (size_t i = 0; i < num_dir_entries; ++i) {
                directory_entry_block_t* deb = &disk_layout->directory->directory[i];
                deb->record_count = le16toh(deb->record_count);
                deb->block_count = le16toh(deb->block_count);
                deb->start_sector = le16toh(deb->start_sector);
                deb->file_format_dependent1 = le16toh(deb->file_format_dependent1);
                deb->file_format_dependent2 = le16toh(deb->file_format_dependent2);
            }
        }
    }
    return true;
}

/* ... (cleanup_oasis_disk, get_total_sectors, get_total_blocks, display_disk_info, list_single_deb, list_files, dump_hex) ... */
/* These functions do not directly handle raw struct serialization/deserialization for uint16_t fields,
   so they likely don't need endianness conversion changes within them, assuming they operate on
   data already converted to host byte order by load_oasis_disk. */

void cleanup_oasis_disk(oasis_disk_layout_t* disk_layout) {
    if (disk_layout) {
        if (disk_layout->alloc_map.map_data) {
            free(disk_layout->alloc_map.map_data);
            disk_layout->alloc_map.map_data = NULL;
            disk_layout->alloc_map.map_size_bytes = 0;
        }
        if (disk_layout->directory) {
            free(disk_layout->directory);
            disk_layout->directory = NULL;
        }
    }
}

size_t get_total_sectors(const filesystem_block_t* fs_block) {
    uint8_t heads;
    uint8_t cylinders;
    uint8_t sectors_per_track;
    size_t total_sectors;

    if (!fs_block) {
        return 0;
    }

    heads = fs_block->num_heads >> 4;
    cylinders = fs_block->num_cyl;
    sectors_per_track = fs_block->num_sectors;

    if (heads == 0 || cylinders == 0 || sectors_per_track == 0) {
        return 0;
    }
    total_sectors = (size_t)heads * cylinders * sectors_per_track;
    return total_sectors;
}

size_t get_total_blocks(const filesystem_block_t* fs_block) {
    size_t total_sectors_val = get_total_sectors(fs_block);
    if (SECTOR_SIZE == 0 || BLOCK_SIZE < SECTOR_SIZE) return 0; /* Avoid division by zero or invalid logic */
    return total_sectors_val / (BLOCK_SIZE / SECTOR_SIZE);
}

void display_disk_info(const oasis_disk_layout_t* disk_layout) {
    char time_str[20];
    char label_str[FNAME_LEN + 1];
    char backup_vol_str[FNAME_LEN + 1];

    if (!disk_layout) return;

    memcpy(label_str, disk_layout->fsblock.label, FNAME_LEN); label_str[FNAME_LEN] = '\0';
    memcpy(backup_vol_str, disk_layout->fsblock.backup_vol, FNAME_LEN); backup_vol_str[FNAME_LEN] = '\0';

    printf("--- Filesystem Information ---\n");
    printf("Label:          '%.*s'\n", FNAME_LEN, label_str);
    oasis_time_str(time_str, sizeof(time_str), &disk_layout->fsblock.timestamp);
    printf("Timestamp:      %s\n", time_str);
    printf("Backup Volume:  '%.*s'\n", FNAME_LEN, backup_vol_str);
    oasis_time_str(time_str, sizeof(time_str), &disk_layout->fsblock.backup_timestamp);
    printf("Backup Time:    %s\n", time_str);
    printf("Flags:          0x%02X\n", disk_layout->fsblock.flags);
    printf("Heads/Drive:    %d / 0x%X\n", disk_layout->fsblock.num_heads >> 4, disk_layout->fsblock.num_heads & 0x0F);
    printf("Cylinders:      %d\n", disk_layout->fsblock.num_cyl);
    printf("Sectors/Track:  %d\n", disk_layout->fsblock.num_sectors);
    printf("Total Sectors:  %zu\n", get_total_sectors(&disk_layout->fsblock));
    printf("Total Blocks:   %zu\n", get_total_blocks(&disk_layout->fsblock));
    printf("Max Dir Sectors:%d (%zu directory entries)\n", disk_layout->fsblock.dir_sectors_max, (size_t)disk_layout->fsblock.dir_sectors_max * DIR_ENTRIES_PER_SECTOR);
    printf("Free Blocks:    %d (%.2f MiB)\n", disk_layout->fsblock.free_blocks, (double)disk_layout->fsblock.free_blocks * BLOCK_SIZE / (1024.0 * 1024.0));
    printf("Extra AM Secs:  %d\n", disk_layout->fsblock.fs_flags & ADDITIONAL_AM_SECTORS_MASK);
    printf("Volume Status:  %s\n", (disk_layout->fsblock.fs_flags & FS_FLAGS_WP) ? "Protected" : "Write Enabled");
    printf("Other fs_flags: 0x%02x\n\n", disk_layout->fsblock.fs_flags & 0x78);

    printf("--- Allocation Map Summary ---\n");
    if (disk_layout->alloc_map.map_data) {
        printf("Map Size:       %zu bytes\n", disk_layout->alloc_map.map_size_bytes);
        printf("Map Max Blocks: %zu\n", get_allocation_map_maximum_blocks(&disk_layout->alloc_map));
        printf("Free Blocks:    %zu\n", count_total_free_blocks(&disk_layout->alloc_map));
        printf("Largest Contig: %zu blocks\n", find_largest_free_contiguous_blocks(&disk_layout->alloc_map));
    }
    else {
        printf("Allocation map data is not loaded.\n");
    }
    printf("\n");
}

void list_single_deb(const directory_entry_block_t* deb) {
    char host_filename[MAX_HOST_FILENAME_LEN];
    char time_str[20];
    char format_str[10] = "";
    uint8_t format_type_val_lsd;

    if (!oasis_deb_is_valid(deb)) { /* Assuming oasis_deb_is_valid works on host-order DEB */
        return;
    }
    if (!oasis_deb_to_host_filename(deb, host_filename, sizeof(host_filename))) {
        strcpy(host_filename, "[Filename Error]");
    }
    format_type_val_lsd = deb->file_format & FILE_FORMAT_MASK;
    if (format_type_val_lsd == FILE_FORMAT_SEQUENTIAL) strcpy(format_str, "SEQ");
    else if (format_type_val_lsd == FILE_FORMAT_DIRECT) strcpy(format_str, "DIR");
    else if (format_type_val_lsd == FILE_FORMAT_INDEXED) strcpy(format_str, "IDX");
    else if (format_type_val_lsd == FILE_FORMAT_KEYED) strcpy(format_str, "KEY");
    else if (format_type_val_lsd == FILE_FORMAT_RELOCATABLE) strcpy(format_str, "REL");
    else if (format_type_val_lsd == FILE_FORMAT_ABSOLUTE) strcpy(format_str, "ABS");
    else strcpy(format_str, "UNK");

    oasis_time_str(time_str, sizeof(time_str), &deb->timestamp);
    printf("%-30s %-6s %-8u %-8u %-10u %-17s %d/%d\n",
        host_filename, format_str, deb->record_count, deb->block_count,
        deb->start_sector, time_str, deb->owner_id, deb->shared_from_owner_id);
}

void list_files(const oasis_disk_layout_t* disk_layout, int owner_id_filter, const char* pattern) {
    size_t num_entries;
    int file_count = 0;
    size_t i;

    if (!disk_layout || !disk_layout->directory) {
        fprintf(stderr, "list_files: Error - Cannot list files, disk layout not loaded.\n");
        return;
    }
    if (disk_layout->directory->directory_size_bytes == 0) {
        printf("Directory is empty.\n");
        return;
    }
    num_entries = disk_layout->directory->directory_size_bytes / sizeof(directory_entry_block_t);
    printf("%-30s %-6s %-8s %-8s %-10s %-17s %s\n",
        "Host Filename", "Format", "Recs", "Blocks", "StartSec", "Timestamp", "Owner");
    printf("----------------------------------------------------------------------------------------------------\n");
    for (i = 0; i < num_entries; ++i) {
        const directory_entry_block_t* deb_entry_lf = &disk_layout->directory->directory[i];
        /* Assuming oasis_deb_is_valid and oasis_filename_wildcard_match work on host-order DEB */
        if (oasis_deb_is_valid(deb_entry_lf)) {
            if (owner_id_filter == OWNER_ID_WILDCARD || deb_entry_lf->owner_id == owner_id_filter) {
                if (pattern == NULL || strlen(pattern) == 0 || oasis_filename_wildcard_match(deb_entry_lf->file_name, deb_entry_lf->file_type, pattern)) {
                    list_single_deb(deb_entry_lf);
                    file_count++;
                }
            }
        }
    }
    printf("----------------------------------------------------------------------------------------------------\n");
    if (owner_id_filter == OWNER_ID_WILDCARD) {
        printf("Total valid files found (for User ID: Any Owner (*)");
    }
    else {
        printf("Total valid files found (for User ID %d", owner_id_filter);
    }
    if (pattern != NULL && strlen(pattern) > 0) {
        printf(", matching pattern '%s'", pattern);
    }
    printf("): %d\n", file_count);
}

void dump_hex(const uint8_t* data, size_t len) {
    size_t i;
    size_t ascii_idx = 0;
    char ascii_buf[17];

    if (!data || len == 0) {
        printf("\n\t(No data to dump)\n\n");
        return;
    }

    printf("\n");
    memset(ascii_buf, 0, sizeof(ascii_buf));

    for (i = 0; i < len; i++) {
        if (i % 16 == 0) {
            if (i > 0) {
                printf(" |%s|\n", ascii_buf);
            }
            printf("\t%04zx: ", i);
            ascii_idx = 0;
            memset(ascii_buf, ' ', sizeof(ascii_buf) - 1); /* Fill with spaces */
            ascii_buf[sizeof(ascii_buf) - 1] = '\0'; /* Null terminate */
        }

        printf("%02x ", data[i]);

        if (ascii_idx < 16) {
            ascii_buf[ascii_idx++] = (isprint(data[i])) ? (char)data[i] : '.';
        }
    }

    if (len > 0) {
        size_t last_line_bytes = len % 16;
        if (last_line_bytes == 0) { /* If len is a multiple of 16 */
            last_line_bytes = 16;
        }
        /* Pad the hex output for alignment if the last line is short */
        for (size_t pad_count = 0; pad_count < (16 - last_line_bytes) * 3; pad_count++) {
            printf(" ");
        }
        /* Ensure ASCII buffer is correctly terminated/padded for display */
        ascii_buf[last_line_bytes] = '\0'; /* Terminate at actual end of ASCII content */
        printf(" |%s|\n", ascii_buf);
    }
    printf("\n");
}
