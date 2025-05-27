/*
 * oasis.h - Core OASIS Data Structures and Definitions
 *
 * This header file defines fundamental constants, data structures (like
 * Directory Entry Blocks, Filesystem Blocks, Timestamps), and includes
 * other core OASIS utility headers for the oasis-utils library.
 *
 * Reference:
 * http://bitsavers.org/pdf/phaseOneSystems/oasis/Macro_Assembler_Reference_Manual_2ed.pdf
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#ifndef OASIS_H_
#define OASIS_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

 /* Constants */
#define SECTOR_SIZE		256
#define SECTORS_PER_BLOCK	4
#define BLOCK_SIZE		(SECTOR_SIZE * SECTORS_PER_BLOCK)

#define FNAME_LEN		8
#define FTYPE_LEN		8 /* Note: Original doc calls this 'type' */

/* Amount of user data per sector in a sequential file */
#define OASIS_SEQ_DATA_PER_SECTOR (SECTOR_SIZE - sizeof(uint16_t))

#define OASIS_MAX_FS_BLOCKS 16384		/* Maximum number of blocks for an OASIS filesystem */

/* File Format Definitions (from directory_entry_block_t.file_format) */
#define FILE_FORMAT_DELETED		0xFF
#define FILE_FORMAT_EMPTY		0x00

/* File formats: Bits 4:0 */
#define FILE_FORMAT_MASK			0x1F	/* Mask for file types */
#define FILE_FORMAT_RELOCATABLE		0x01	/* Program file, relocatable */
#define FILE_FORMAT_ABSOLUTE		0x02	/* Program file, absolute */
#define FILE_FORMAT_SEQUENTIAL		0x04	/* Data file, sequential */
#define FILE_FORMAT_DIRECT			0x08	/* Data file, direct access */
#define FILE_FORMAT_INDEXED			0x10	/* Data file, indexed sequential */
#define FILE_FORMAT_KEYED			0x18	/* Data file, keyed (Direct + Indexed) */

/* Attributes: Bits 7:5 */
#define FILE_ATTRIBUTE_MASK				0xE0	/* Mask for file attributes */
#define FILE_FORMAT_READ_PROTECTED		0x20	/* R - Read-protected, user cannot read */
#define FILE_FORMAT_WRITE_PROTECTED		0x40	/* W - Write-protected, user cannot write */
#define FILE_FORMAT_DELETE_PROTECTED	0x80	/* D - Delete-protected, user cannot delete */

/* OASIS Communication Control Characters */
#define STX 0x02 /* Start of Text */
#define ETX 0x03 /* End of Text */
#define EOT 0x04 /* End of Transmission */
#define ENQ 0x05 /* Enquiry */
#define VT  0x0B /* Vertical Tab (Manual says ACK, code uses DLE+'0'/'1') */
#define SO  0x0E /* Shift Out (Manual says NAK, code resends prev ACK) */
#define SI  0x0F /* Shift In */
#define DLE 0x10 /* Data Link Escape */
#define CAN 0x18 /* Cancel */
#define ESC 0x1B /* Escape */
#define SUB 0x1A /* Substitute (Used as EOF marker in ASCII mode) */
#define LF  0x0A /* Line Feed */
#define RUB 0x7F /* Rubout / Delete */

#define ADDITIONAL_AM_SECTORS_MASK		(0x07)
#define FS_FLAGS_WP						(1 << 7)

/*
 * Ensure structures are packed tightly, matching the on-disk format.
 * This is crucial for compatibility across different compilers (GCC, MSVC).
 */
#pragma pack(push, 1)

 /**
  * struct oasis_tm - Packed representation of OASIS timestamp.
  * @raw:	3 bytes representing the timestamp. Needs conversion.
  *
  * raw[0] bits 7:4 = Month (1-12)
  * raw[0] bits 3:0, raw[1] bit 7 = Day(1 - 31)
  * raw[1] bits 6:3 = Year (0-15, where 0 is 1977 and 15 is 1992)
  * raw[1] bits 2:0, raw[2] bits 7:6 = Hour (0-23)
  * raw[2] bits 5:0 = Minutes (0-59)
  *
  * Year is offset from 1977. Seconds are not stored.
  */
typedef struct oasis_tm {
	uint8_t raw[3];
} oasis_tm_t;

typedef struct boot_sector {
	uint8_t data[SECTOR_SIZE];
} boot_sector_t;

/**
 * struct filesystem_block - Structure of the OASIS Filesystem Header Block.
 * Typically located at sector 1 (offset 256).
 * @label:		Volume label (ASCII, space-padded).
 * @timestamp:		Timestamp of filesystem creation/last update.
 * @backup_vol:		Label of the volume this was backed up from (ASCII, space-padded).
 * @backup_timestamp: Timestamp of the last backup operation.
 * @flags:              Filesystem flags (exact meaning TBD).
 * @num_heads:		Number of heads (high nibble) & drive type (low nibble).
 * @num_cyl:		Number of cylinders.
 * @num_sectors:	Number of sectors per track.
 * @dir_sectors_max: Maximum number of directory entry sectors.
 * @reserved:		Reserved, always zero?
 * @fs_flags:		Number of additional allocation map sectors.
 * @free_blocks:	Count of free blocks on the volume (1K units).
 */
typedef struct filesystem_block {
	char		label[FNAME_LEN];
	oasis_tm_t	timestamp;
	uint8_t		backup_vol[FNAME_LEN];
	oasis_tm_t	backup_timestamp;
	uint8_t		flags;		/* Meaning unclear from ref */
	uint8_t		num_heads;      /* High nibble = heads, Low nibble = drive type? */
	uint8_t		num_cyl;
	uint8_t		num_sectors;
	uint8_t		dir_sectors_max; /* Number of sectors containing eight 32-byte entries each. */
	uint16_t	reserved;		/* Reserved, always 0? */
	uint16_t	free_blocks;    /* In 1K units */
	uint8_t		fs_flags;		/* This is actually fs_flags.
									 * The three LSBs are fs_flags: Number of additional allocation map sectors.
									 * Bit 7 indicates that the disk is (software) write protected. */
} filesystem_block_t;

/*
 * Structure to hold the allocation map data and its size.
 * map_data: Pointer to the raw byte array representing the bitmap.
 * map_size_bytes: The total size of the allocation map in bytes.
 */
typedef struct {
	uint8_t* map_data;
	size_t   map_size_bytes;
} oasis_alloc_map_t;

/**
 * struct directory_entry_block - Structure of an OASIS Directory Entry (DEB).
 * Each entry is 32 bytes.
 * @file_format:		Type and attributes of the file (see FILE_FORMAT_* defines).
 * @file_name:			File name (ASCII, space-padded).
 * @file_type:			File extension/type (ASCII, space-padded).
 * @record_count:		Number of records in the file (little-endian).
 * @block_count:		Number of blocks (1024 bytes) allocated (little-endian).
 * @start_sector:		Logical sector number (LBA) of the first sector (little-endian).
 * @file_format_dependent1:	Meaning depends on file_format (e.g., record length) (little-endian).
 * @timestamp:			Timestamp of file creation/last modification.
 * @owner_id:			ID of the file owner.
 * @shared_from_owner_id:	ID of the owner if shared (?). Check reference.
 * @file_format_dependent2:	Meaning depends on file_format (e.g., Link/Origin address) (little-endian).
 */
typedef struct directory_entry_block {
	uint8_t		file_format;	/* See FILE_FORMAT_* definitions above */
	char		file_name[FNAME_LEN];	/* 8 character file name, space padded */
	char		file_type[FTYPE_LEN];	/* 8 character file type, space padded */
	uint16_t	record_count;
	uint16_t	block_count;	/* Number of 1K Blocks occupied by the file */
	uint16_t	start_sector;
	uint16_t	file_format_dependent1;	/* Variable by file format:
	                                     * I, K = Bits 15:9 are Key length, bits 8:0 are record length.
										 * S = Record length of longest record.
										 * D = Allocated Record Length.
										 * A, R = Record Length (SECTOR_LEN)
										 */
	oasis_tm_t	timestamp;	/* 3-byte packed timestamp */
	uint8_t		owner_id;	/* Owner ID */
	uint8_t		shared_from_owner_id;
	uint16_t	file_format_dependent2;	/* Variable by file format:
	                                     * I, K = Allocated file size.
										 * S = Disk address of last sector in file.
										 * D = Always zero.
										 * R = Program length.
										 * A = Origin address (0000-FFFF in hex)
										 */
} directory_entry_block_t;

#define DIR_ENTRIES_PER_SECTOR		(SECTOR_SIZE / sizeof(directory_entry_block_t))

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200) /* Disable nonstandard extension used: zero-sized array */
#endif

/*
 * Structure to hold the directory (an array of directory_entry_block_t structures.)
 * directory: Pointer to the raw byte array representing DEB data structures comprising the directory.
 * directory_size_bytes: The total size of the directory in bytes.
 *
 * The number of DEBs in the directory is (filesystem_block_t.dir_sectors_max * SECTOR_SIZE) / sizeof(directory_entry_block_t)
 */
typedef struct {
	size_t   directory_size_bytes;
	directory_entry_block_t directory[];
} oasis_directory_t;

#ifdef _MSC_VER
#pragma warning(pop)
#endif


/*
 * The Allocation Map is a contiguous array of bytes on the disk, directly
 * following the OASIS Filesystem Header Block (filesystem_block.)  The allocation
 * map consists of the remainder of the sector following the Filesystem Header Block,
 * followed by additional sectors as specified in filesystem_block.fs_flags.
 *
 * Block Size: Each bit within the allocation map corresponds to a single
 * allocatable unit of disk space, defined as a 1024-byte block.
 *
 * Sector Size: The underlying disk geometry is assumed to use 256-byte sectors.
 * This implies that each 1024-byte block consists of 4 sectors.
 *
 * Bit Mapping Convention: A standard mapping convention is assumed where bits
 * within a byte correspond to consecutive block numbers. Specifically, the most
 * significant bit (MSB, bit 7) of a byte in the map represents the first block in
 * the group of eight covered by that byte, and the least significant bit (LSB,
 * bit 0) represents the last block in that group. A '0' bit indicates a free
 * block, and a '1' bit indicates an allocated block.
 *
 */

 /*
  * The OASIS disk layout is as follows:
  * Sector 0: Boot sector.
  * Sector 1: filesystem_block_t (32 bytes)
  * allocation map (remainder of sector 1)
  *
  * Sectors 2+ Additional allocation map sectors (0-7)
  * Following allocation map: Directory Sectors (dir_sectors_max): eight DEB's per sector.
  * Following Directory Sectors: File Data
  */
typedef struct oasis_disk_layout {
	boot_sector_t	boot;		/* Sector 0 */
	filesystem_block_t fsblock;	/* Sector 1 */
	oasis_alloc_map_t	alloc_map;	/* Allocation Map */
	oasis_directory_t *directory;	/* Pointer to array of DEBs. */
} oasis_disk_layout_t;

/* Restore previous packing alignment */
#pragma pack(pop)


#include "oasis_endian.h"
#include "oasis_alloc.h"
#include "oasis_ascii.h"
#include "oasis_deb.h"
#include "oasis_wildcard.h"
#include "oasis_sector_io.h"
#include "oasis_utils.h"
#include "oasis_file_write.h"
#include "oasis_file_read.h"
#include "oasis_extract.h"
#include "oasis_glob.h"
#include "oasis_pcap.h"
#include "oasis_time.h"
#include "oasis_file_erase.h"
#include "oasis_file_rename.h"
#include "oasis_file_copy.h"
#include "oasis_initdisk.h"
#include "oasis_sendrecv.h"


#endif /* OASIS_H_ */
