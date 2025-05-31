# OASIS On-Disk Data Structures Specification

This specification details the layout and composition of data as stored on an OASIS-formatted disk.

---

## 1. General Disk Layout

An OASIS disk is structured with specific data areas appearing in a defined order. The fundamental units of disk space are **sectors** (256 bytes) and **blocks** (1024 bytes, composed of 4 sectors). The general layout is as follows:

* **Sector 0:** Boot Sector.
* **Sector 1:**
    * **Filesystem Header Block (`filesystem_block_t`):** The first 32 bytes of this sector.
    * **Allocation Map (start):** The remainder of Sector 1 is the beginning of the allocation map.
* **Sectors 2+:**
    * **Additional Allocation Map Sectors:** As specified in the `filesystem_block_t.fs_flags`, 0 to 63 additional sectors can be used for the allocation map.
    * **Following Allocation Map: Directory Sectors:** These sectors store the directory entries, with each sector holding eight **Directory Entry Blocks** (`directory_entry_block_t`). The maximum number of directory sectors is defined in `filesystem_block_t.dir_sectors_max`.
    * **Following Directory Sectors: File Data:** The actual content of the files resides in this area.

This structure is outlined in `oasis.h` and further elaborated in the System Reference Manual.

---

## 2. Boot Sector

* **Location:** Sector 0 of the disk.
* **Size:** 256 bytes (`SECTOR_SIZE`).
* **Structure:** `boot_sector_t` (defined in `oasis.h` as an array of 256 `uint8_t` values).
* **Content:** Contains the bootstrap loader program necessary to initiate the OASIS operating system. The `INITDISK` command with the `FORMAT` option writes this loader to the disk.

---

## 3. Filesystem Header Block (`filesystem_block_t`)

This 32-byte structure contains critical information about the filesystem and the volume. It is typically located at the beginning of Sector 1.

```c
typedef struct filesystem_block {
    char        label[FNAME_LEN];           /* 8 bytes */
    oasis_tm_t  timestamp;                  /* 3 bytes */
    uint8_t     backup_vol[FNAME_LEN];      /* 8 bytes */
    oasis_tm_t  backup_timestamp;           /* 3 bytes */
    uint8_t     flags;
    uint8_t     num_heads;
    uint8_t     num_cyl;
    uint8_t     num_sectors;
    uint8_t     dir_sectors_max;
    uint16_t    reserved;
    uint16_t    free_blocks;                /* In 1K units */
    uint8_t     fs_flags;
} filesystem_block_t;
```

**Field Descriptions:**

* **`label`**: 8-byte volume label. This is ASCII, uppercase, space-padded, and not null-terminated. It's established during disk initialization with `INITDISK`.
* **`timestamp`**: 3-byte packed OASIS timestamp (`oasis_tm_t`) indicating the filesystem creation or last update time.
* **`backup_vol`**: 8-byte label of the volume this disk was last backed up from. ASCII, uppercase, space-padded, not null-terminated.
* **`backup_timestamp`**: 3-byte packed OASIS timestamp of the last backup operation.
* **`flags`**: 1-byte filesystem flags. The exact meaning of these flags within `oasis.h` is marked as TBD (To Be Determined) but may correspond to system-specific flags or UCB (Unit Control Block) byte 12 as mentioned in `oasis.h`.
* **`num_heads`**: 1-byte field. The high nibble likely represents the number of disk heads, and the low nibble may indicate the drive type.
* **`num_cyl`**: 1-byte field for the number of cylinders on the disk.
* **`num_sectors`**: 1-byte field for the number of sectors per track.
* **`dir_sectors_max`**: 1-byte field specifying the maximum number of 256-byte sectors allocated for directory entries. Each sector holds eight 32-byte directory entries.
* **`reserved`**: 2-byte field, typically zero.
* **`free_blocks`**: 2-byte field (little-endian) indicating the count of free 1024-byte blocks on the volume.
* **`fs_flags`**: 1-byte field with filesystem status flags, referencing Appendix C (p. 147) of the OASIS Macro Assembler Language Reference Manual (`Macro_Assembler_Reference_Manual_Mar80.pdf`) for UCB Byte 13 definitions. `oasis.h` defines the following bits:
    * **Bits 5:0 (Mask: `ADDITIONAL_AM_SECTORS_MASK` - `0x3F`):** Number of additional allocation map sectors (0-63).
    * **Bit 6 (Mask: `FS_FLAGS_IBM_FORMAT` - `1 << 6`):** If set, Track 0 is in IBM single-density format.
    * **Bit 7 (Mask: `FS_FLAGS_WP` - `1 << 7`):** If set, the disk is software write-protected. This protection can be set using the `INITDISK` command.

---

## 4. Allocation Map (`oasis_alloc_map_t`)

The Allocation Map is a bitmap that tracks the usage of allocatable blocks on the disk.

* **Location:** Starts immediately after the `filesystem_block_t` in Sector 1 and can extend into subsequent sectors as defined by `filesystem_block_t.fs_flags` (bits 5:0).

```c
typedef struct {
    uint8_t* map_data;       /* Pointer to the raw byte array */
    size_t   map_size_bytes; /* Total size of the map in bytes */
} oasis_alloc_map_t;
```
*(Note: `map_data` is a pointer in the C structure for in-memory representation; on disk, it's a contiguous byte stream).*

**Functionality:**

* Each bit in the allocation map corresponds to a single 1024-byte block on the disk.
* A '0' bit indicates a free block.
* A '1' bit indicates an allocated block.

**Bit Mapping Convention:**

* The most significant bit (MSB, bit 7) of a byte in the map represents the first block in the group of eight blocks covered by that byte.
* The least significant bit (LSB, bit 0) represents the last block in that group.

**Initialization:** The `INITDISK` command initializes the allocation map to indicate that the entire disk is available (all '0's, except for blocks used by the system structures themselves).

---

## 5. Directory (`oasis_directory_t` and `directory_entry_block_t`)

The directory contains entries for all files and programs on the disk. It is an array of **Directory Entry Blocks (DEBs)**.

* **Location:** Follows the Allocation Map.
* **DEB Size:** Each Directory Entry Block is 32 bytes.
* **DEBs per Sector:** 8 DEBs per 256-byte sector (`DIR_ENTRIES_PER_SECTOR`).
* **Directory Size:** The total size is determined by `filesystem_block_t.dir_sectors_max` multiplied by `SECTOR_SIZE`.

**Structure Definition (`oasis_directory_t` in `oasis.h`):**

```c
typedef struct {
    size_t   directory_size_bytes;
    directory_entry_block_t directory[]; /* Flexible array member */
} oasis_directory_t;
```
*(Note: The flexible array member `directory[]` is for in-memory representation; on disk, it's a sequence of `directory_entry_block_t` structures).*

**Directory Entry Block Structure (`directory_entry_block_t` in `oasis.h`):**

```c
typedef struct directory_entry_block {
    uint8_t     file_format;
    char        file_name[FNAME_LEN];       /* 8 bytes */
    char        file_type[FTYPE_LEN];       /* 8 bytes */
    uint16_t    record_count;               /* little-endian */
    uint16_t    block_count;                /* little-endian, # of 1K blocks */
    uint16_t    start_sector;               /* little-endian, logical sector # */
    uint16_t    file_format_dependent1;     /* little-endian */
    oasis_tm_t  timestamp;                  /* 3 bytes */
    uint8_t     owner_id;
    uint8_t     shared_from_owner_id;
    uint16_t    file_format_dependent2;     /* little-endian */
} directory_entry_block_t;
```

**Field Descriptions for `directory_entry_block_t`:**

* **`file_format`**: 1-byte field indicating file type and attributes.
    * `FILE_FORMAT_DELETED` (`0xFF`): Entry is deleted.
    * `FILE_FORMAT_EMPTY` (`0x00`): Entry is empty/unused.
    * **File Format (Bits 4:0, Mask: `FILE_FORMAT_MASK` - `0x1F`):**
        * `FILE_FORMAT_RELOCATABLE` (`0x01`): Program file, relocatable.
        * `FILE_FORMAT_ABSOLUTE` (`0x02`): Program file, absolute.
        * `FILE_FORMAT_SEQUENTIAL` (`0x04`): Data file, ASCII sequential.
        * `FILE_FORMAT_DIRECT` (`0x08`): Data file, direct access.
        * `FILE_FORMAT_INDEXED` (`0x10`): Data file, indexed sequential.
        * `FILE_FORMAT_KEYED` (`0x18`): Data file, keyed (Direct + Indexed). (This represents `FILE_FORMAT_DIRECT | FILE_FORMAT_INDEXED`).
    * **File Attributes (Bits 7:5, Mask: `FILE_ATTRIBUTE_MASK` - `0xE0`):**
        * `FILE_FORMAT_READ_PROTECTED` (`0x20`): Read-protected.
        * `FILE_FORMAT_WRITE_PROTECTED` (`0x40`): Write-protected.
        * `FILE_FORMAT_DELETE_PROTECTED` (`0x80`): Delete-protected.
* **`file_name`**: 8-byte file name. ASCII, uppercase, space-padded, not null-terminated.
* **`file_type`**: 8-byte file extension/type. ASCII, uppercase, space-padded, not null-terminated.
* **`record_count`**: 2-byte (little-endian) number of records in the file.
* **`block_count`**: 2-byte (little-endian) number of 1024-byte blocks allocated to the file.
* **`start_sector`**: 2-byte (little-endian) logical sector number (LBA) of the file's first sector.
* **`file_format_dependent1`**: 2-byte (little-endian) field whose meaning depends on `file_format` (details in `oasis.h` comments):
    * **Indexed (I), Keyed (K):** Bits 15:9 are Key length, bits 8:0 are record length.
    * **Sequential (S):** Record length of the longest record.
    * **Direct (D):** Allocated Record Length.
    * **Absolute (A), Relocatable (R):** Record Length (usually `SECTOR_SIZE`).
* **`timestamp`**: 3-byte packed OASIS timestamp (`oasis_tm_t`) of file creation or last modification.
* **`owner_id`**: 1-byte ID of the file owner.
* **`shared_from_owner_id`**: 1-byte ID of the owner if the file is shared.
* **`file_format_dependent2`**: 2-byte (little-endian) field whose meaning depends on `file_format` (details in `oasis.h` comments):
    * **Indexed (I), Keyed (K):** Allocated file size.
    * **Sequential (S):** Disk address of the last sector in the file.
    * **Direct (D):** Always zero.
    * **Relocatable (R):** Program length.
    * **Absolute (A):** Origin address (0000-FFFF hex).

---

## 6. OASIS Timestamp (`oasis_tm_t`)

This is a 3-byte packed representation of date and time.

**Structure Definition (`oasis.h`):**

```c
typedef struct oasis_tm {
    uint8_t raw[3];
} oasis_tm_t;
```

**Byte Breakdown (`oasis.h` comments):**

* `raw[0]` bits 7:4 = Month (1-12)
* `raw[0]` bits 3:0, `raw[1]` bit 7 = Day (1-31)
* `raw[1]` bits 6:3 = Year (0-15, where 0 is 1977 and 15 is 1992)
* `raw[1]` bits 2:0, `raw[2]` bits 7:6 = Hour (0-23)
* `raw[2]` bits 5:0 = Minutes (0-59)

**Notes:**

* The year is an offset from 1977.
* Seconds are not stored in this timestamp structure.
* The system prompts for time (HH:MM:SS) and date (MM/DD/YY) upon startup.
