/*
 * OASIS Data Structures and Definitions
 *
 * www.github.com/hharte/oasis-utils
 *
 * Copyright (c) 2021-2022, Howard M. Harte
 *
 * Reference: Directory Entry Block (DEB)
 * http://bitsavers.org/pdf/phaseOneSystems/oasis/Macro_Assembler_Reference_Manual_2ed.pdf
 * pp. 112
 *
 */

#ifndef OASIS_H_
#define OASIS_H_

#include <stdio.h>
#include <stdint.h>

#define BLOCK_SIZE      (256)
#define FNAME_LEN       (8)
#define FEXT_LEN        (8)

#define FILE_FORMAT_DELETED             (0xFF)
#define FILE_FORMAT_EMPTY               (0x00)
#define FILE_FORMAT_SYNONYM             (0x80)
#define FILE_FORMAT_RELOCATABLE         (0x01)
#define FILE_FORMAT_ABSOLUTE            (0x02)
#define FILE_FORMAT_SEQUENTIAL          (0x04)
#define FILE_FORMAT_DIRECT              (0x08)
#define FILE_FORMAT_INDEXED             (0x10)
#define FILE_FORMAT_KEYED               (0x18)

#pragma pack(push, 1)

/* OASIS Filesystem header */
typedef struct oasis_tm {
    uint8_t raw[3];
} oasis_tm_t;

typedef struct filesystem_block {
    char       label[FNAME_LEN];
    oasis_tm_t timestamp;
    uint8_t    unknown[12];
    uint8_t    num_heads;
    uint8_t    num_cyl;
    uint8_t    num_sectors;
    uint8_t    dir_entries_max;
    uint8_t    unknown2[2];
    uint8_t    free_blocks;
} filesystem_block_t;


/* OASIS Directory Entry */
typedef struct directory_entry_block {
    uint8_t    file_format;
    char       file_name[FNAME_LEN];
    char       file_type[FEXT_LEN];
    uint16_t   record_count;
    uint16_t   block_count;
    uint16_t   start_sector;
    uint16_t   file_format_dependent1;
    oasis_tm_t timestamp;
    uint8_t    owner_id;
    uint8_t    shared_from_owner_id;
    uint16_t   file_format_dependent2;
} directory_entry_block_t;
#pragma pack(pop)

#endif  // OASIS_H_
