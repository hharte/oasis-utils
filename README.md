# OASIS Utilities

[![Codacy Badge](https://app.codacy.com/project/badge/Grade/62b0c5d0f2104b0896e5000851cb53bc)](https://app.codacy.com/gh/hharte/oasis-utils/dashboard?utm_source=gh&utm_medium=referral&utm_content=&utm_campaign=Badge_grade)

[![Coverity Scan Build Status](https://scan.coverity.com/projects/24657/badge.svg)](https://scan.coverity.com/projects/hharte-oasis-utils)

## Overview

This project provides a collection of command-line utilities for interacting with the OASIS operating system, specifically for:

1. **Disk Image Manipulation:** Listing contents, extracting files from, copying files to, renaming files within, erasing files from, and checking the integrity of OASIS `.img` and ImageDisk `.imd` disk images.
2. **Disk Image Initialization:** Formatting and initializing new OASIS disk images (`.img` or `.imd`), setting disk labels, geometry, and directory sizes.
3. **Serial File Transfer:** Sending files to and receiving files from an OASIS system over a serial connection using the [OASIS Send / Receive Protocol](http://bitsavers.org/pdf/phaseOneSystems/oasis/Communications_Reference_Manual_Mar80.pdf).
4. **IMD Image Verification:** A Python script is included to batch-check `.imd` files using `oasis_chkdsk`.

## Features

The suite includes the following utilities:

* `oasis_disk_util`: Inspects and manipulates OASIS `.img` or `.imd` disk images.
  * Lists directory contents, showing file names, types, dates, sizes, and other metadata. Supports filtering by User ID.
  * Extracts files from the disk image to the host filesystem. Supports filtering by User ID and filename pattern.
  * Displays detailed information about the disk image structure and metadata.
  * Copies files from the host system into the disk image.
  * Renames files within the disk image.
  * Erases (marks as deleted) files within the disk image and frees their allocated blocks.
* `oasis_chkdsk`: Verifies the integrity of OASIS `.img` or `.imd` disk images.
  * Checks Directory Entry Blocks (DEBs).
  * Ensures consistency between DEBs and the disk's allocation map.
  * Validates sequential file linkage.
  * Detects shared disk sectors between files.
  * Supports filtering files to check by filename pattern.
  * For IMD images, reports bad sectors (based on IMD flags) and identifies files potentially affected by them.
* `oasis_initdisk`: Initializes or formats an OASIS disk image file (`.img` or `.imd`).
  * `FORMAT`: Performs a low-level format (simulated for `.img`, actual for `.imd` using `libimdf`) and then initializes the OASIS filesystem structures (boot sector, filesystem block, allocation map, directory).
  * `BUILD`: Writes essential filesystem structures (bootstrap, label, directory) to an already formatted disk. Assumes existing low-level format.
  * `CLEAR`: Erases all files by re-initializing the directory and allocation map, effectively making the disk appear empty while preserving the low-level format.
  * `LABEL <name>`: Sets or changes the disk's volume label.
  * `WP` / `NOWP`: Enables or removes software write-protection for the disk.
  * Supports specifying disk geometry (heads, tracks/surface, sectors/track), sector interleave, track skew, and directory size.
* `oasis_send`: Sends files from the host system to an OASIS system via a serial port.
  * Determines OASIS filename, type, format, and record length/load address/key length from the host filename structure (see Filename Convention below).
  * Allows specifying the User ID for the sent file's Directory Entry Block (DEB).
  * Supports various OASIS file formats (Sequential, Direct, Absolute, Relocatable, Indexed, Keyed).
  * Handles ASCII mode conversion (CR/LF to CR, SUB as EOF).
  * Implements the OASIS serial transfer protocol.
* `oasis_recv`: Receives files from an OASIS system onto the host system via a serial port.
  * Handles the OASIS serial transfer protocol.
  * Supports ASCII mode conversion (received CR to host OS standard, SUB as EOF).
  * Creates output files based on received OASIS directory information, using the host filename convention.
* `oasis_check_imd.py`: A Python script that recursively finds all `.imd` files in a given directory and runs `oasis_chkdsk` on them. Reports successes and failures.

### Common Features for Serial Utilities (`oasis_send`, `oasis_recv`)

* **Cross-Platform Serial Support:** Uses the `mm_serial` library for serial communication on both POSIX (Linux, macOS) and Win32 systems.
* **PCAP Logging:** Optionally logs the raw serial communication (including protocol details) to a `.pcap` file (using `LINKTYPE_USER2`, value 149) for debugging and analysis with tools like Wireshark.
* **Configurable Baud Rate:** Allows specifying the serial port speed.
* **Debug Mode:** Provides verbose output for troubleshooting.
* **Quiet Mode:** Suppresses non-essential output.
* **Transfer Pacing:** Optional delays can be configured after sending/receiving packets.
* **Hardware Flow Control:** RTS/CTS flow control is enabled by default but can be disabled.

### General Features

* **User ID Filtering:** `oasis_disk_util` (for `list`, `extract`, `erase`, `rename` operations) and `oasis_chkdsk` can filter files based on User ID. `oasis_send` and `oasis_disk_util` (for `copyfile`/`insert`) can set the User ID for outgoing/copied files.
* **Wildcard Support:** `oasis_disk_util` (for `list`, `extract`, `erase` operations) and `oasis_chkdsk` (for `-f` option) support `*` and `?` wildcards for filename matching. `oasis_send` supports wildcard expansion for input filenames on Windows (via internal globbing) or relies on shell expansion on POSIX.

## Usage

### `oasis_disk_util` (Disk Image Utility)
```
oasis_disk_util <disk_image_path> <OPERATION> [ARGS...]
```

* `OPERATION`:
  * `list` (or `l`): List files. Args: `[pattern] [-u user_id]`
    * `pattern`: Optional wildcard (e.g., `"FILE*.TXT"`, `"?INFO.DAT"`). Default: all files.
  * `extract` (or `ex`): Extract files. Args: `[pattern] [output_path] [-a|--ascii] [-u user_id]`
    * `pattern`: Optional wildcard. Default: all files.
    * `output_path`: Directory to extract to. Default: current directory.
  * `info` (or `i`): Display disk image information. Args: `[-u user_id]` (user filter applies to some detailed stats if relevant).
  * `erase` (or `er`): Mark file(s) as deleted and free their blocks. Args: `<filename_pattern> [-u user_id]`
  * `rename` (or `r`): Rename a file. Args: `<old_filename> <new_filename> [-u user_id]`
    * `<old_filename>`: The exact current name of the file on the OASIS disk (e.g., "OLDFILE.TXT"). Does not support wildcards.
    * `<new_filename>`: The new desired name for the file (e.g., "NEWFILE.DAT"). Does not support wildcards.
  * `copyfile` (or `c`, `co`, `insert`, `in`, `ins`): Copy host file to disk image. Args: `<host_filepath> [oasis_filename] [-a|--ascii] [-u user_id]`
    * `<host_filepath>`: Path to the file on the host system.
    * `[oasis_filename]`: Optional. Target name on OASIS disk (e.g., `MYPROG.BAS_S`). If omitted, derived from host filename.
* Options:
  * `-u, --user <id>`: User ID (0-255) or `*` (or `-1`) for any owner. Default for `list`: `*`. Default for `extract`/`info`/`erase`/`rename`/`copyfile`: `0`.
  * `-a, --ascii` (for `extract`, `copyfile`): Convert ASCII files' line endings. For `extract`: OASIS (CR) to Host (LF/CRLF), SUB removed. For `copyfile`: Host (LF/CRLF) to OASIS (CR), SUB potentially added.

### `oasis_chkdsk` (Disk Integrity Check Utility)
```
oasis_chkdsk <disk_image_path> [-f <pattern>] [-v|--verbose]
```
* `<disk_image_path>`: Path to the OASIS disk image file (`.img` or `.imd`).
* `-f, --file <pattern>`: Optional. Check only files matching the name or wildcard pattern.
* `-v, --verbose`: Enable verbose output.

### `oasis_initdisk` (Disk Initialization Utility)
```
oasis_initdisk <image_path_or_fd> [OPTION]...
```
* `<image_path_or_fd>`: Path to the disk image file, or a drive letter (A-Z).
* Options (case-insensitive, space separated):
    * `BUILD`: Write bootstrap, label, directory to an already formatted disk.
    * `CLEAR` / `CL`: Erase all files, re-initialize directory.
    * `FORMAT` / `FMT`: Initialize entire disk format, then build filesystem.
    * `LABEL <name>`: Set or re-initialize disk label to `<name>` (max 8 chars).
    * `NOWP`: Remove software write protection.
    * `WP`: Enable software write protection.
    * `HEAD <n>`: (Requires FORMAT) Number of disk surfaces (1-255).
    * `INCR <n>`: (Requires FORMAT) Logical sector increment (1-255).
    * `SECTOR <n>`: (Requires FORMAT) Sectors per track (1-255).
    * `SIZE <n>`: (Requires FORMAT or CLEAR or BUILD) Number of directory entries.
    * `SKEW <n>`: (Requires FORMAT) Track skew factor (0-255).
    * `TRACKS <n>`: (Requires FORMAT) Tracks per surface (1-255).

### `oasis_send` (Serial File Sender)
```
oasis_send <port> [options] <filename_or_pattern...>
```
* `<port>`: Serial port device name (e.g., `/dev/ttyS0`, `COM1`).
* `<filename_or_pattern...>`: One or more host filenames or patterns to send.
    * **Filename Convention:** The host filename **must** encode the target OASIS filename, type, format, attributes, and (optionally) record/key/load address details.
        * Format: `FNAME.FTYPE_[S|D|A|R|I|K][ATTRS][_RRR[_KKK_LLLL]]`
        * `FNAME`: Max 8 chars.
        * `FTYPE`: Max 8 chars.
        * `S|D|A|R|I|K`: Single character for file format (Sequential, Direct, Absolute, Relocatable, Indexed, Keyed).
        * `ATTRS`: Optional attributes (R=Read-prot, W=Write-prot, D=Delete-prot). E.g., `RW`, `D`.
        * `RRR`: Record Length. Required for Direct, Indexed, Keyed. For Absolute/Relocatable, it's usually SECTOR_SIZE (256). For Sequential, it's the longest record length (if 0, `oasis_send` may calculate it or use a default).
        * `KKK`: Key Length (for Indexed/Keyed).
        * `LLLL`: Load Address (hex, for Absolute).
        * Examples: `MYPROG.BAS_S`, `DATA.REC_D_128`, `SYSTEM.CMD_AR_256_E000` (Absolute, Read-prot, RecLen 256, Load Addr E000).
    * If only `FNAME.TYPE` is given, Sequential format is assumed.
    * Wildcards in `<filename_or_pattern>` are expanded by `oasis_send` on Windows. On POSIX, they are typically expanded by the shell.
* Options:
    * `-q`: Quiet mode.
    * `-d`: Debug mode.
    * `-a, --ascii`: ASCII mode (CR/LF to CR, SUB as EOF). *Implies Sequential format.*
    * `-f, --flow-control`: Disable Hardware (RTS/CTS) Flow Control (default: enabled).
    * `-b <rate>`: Baud rate (default: 19200).
    * `-u <id>`: User ID (0-255) for DEB (default: 0).
    * `--pcap <file>`: Log communication to PCAP file.
    * `--pacing-packet <ms>`: Delay (ms) after sending each packet.

### `oasis_recv` (Serial File Receiver)
```
oasis_recv <port> [<output_dir>] [options]
```
* `<port>`: Serial port device name.
* `[<output_dir>]`: Optional output directory (default: current directory).
* Options:
    * `-q`: Quiet mode.
    * `-d`: Debug mode.
    * `-a, --ascii`: Convert received ASCII files to host line endings.
    * `-f, --flow-control`: Disable Hardware (RTS/CTS) Flow Control (default: enabled).
    * `-b <rate>`: Baud rate (default: 19200).
    * `--pcap <file>`: Log communication to PCAP file.
    * `--pacing-packet <ms>`: Delay (ms) after receiving each packet (before sending ACK).

### `oasis_check_imd.py` (IMD Batch Checker)
```
python oasis_check_imd.py <directory> [-v|--verbose] [--chkdsk-verbose] [--chkdsk-path <path>]
```
* `<directory>`: Directory to search recursively for `.imd` files.
* `-v, --verbose`: Verbose output from the script itself.
* `--chkdsk-verbose`: Pass the `-v` flag to `oasis_chkdsk`.
* `--chkdsk-path <path>`: Path to the `oasis_chkdsk` executable (default: assumes it's in PATH).

## Serial Transfer Protocol

The `oasis_send` and `oasis_recv` utilities implement a serial communication protocol based on the OASIS Communications Reference Manual. Key aspects include:

* **Packet Structure:** Data is transferred in packets prefixed with `DLE STX` and suffixed with `DLE ETX LRC`. An additional `RUB` (0x7F) character is sent after the LRC as padding/terminator by `oasis_send`.
* **DLE Stuffing:** `DLE` characters within the data are escaped by doubling them (`DLE DLE`). `ESC` is escaped as `DLE CAN`.
* **Shift States:** `DLE SI` and `DLE SO` are used to indicate whether the subsequent data bytes have their most significant bit (MSB) set (Shift-In adds 0x80).
* **Compression:** A simple run-length encoding using `DLE VT count` can repeat the previously sent character. The count byte itself is escaped if it's `DLE` or `ESC`.
* **Checksum:** An 8-bit Longitudinal Redundancy Check (LRC) is appended to each packet (before the final RUB) for error detection. The LRC is calculated on the packet content up to and including `DLE ETX`, then `OR`ed with 0xC0 and `AND`ed with 0x7F.
* **Handshake:** Uses `ENQ` (Enquiry) and `DLE ACK` (`0`/`1`) for establishing connection and acknowledging packets. A toggle mechanism in the ACK (`0`/`1`) ensures packets are received in order.
* **Packet Types:** Specific commands (`OPEN`, `WRITE`, `CLOSE`) are used within packets to manage the file transfer process. The `OPEN` packet includes the Directory Entry Block (DEB) for the file being transferred.
* **Termination:** `DLE EOT` signals the end of the transmission session.

## Building

These utilities are written in C (with C++ for GoogleTest) and require a C/C++ compiler and CMake.

1.  **Prerequisites:**
    * CMake (version 3.24 or higher).
    * A C compiler (GCC, Clang, MSVC). C11 standard is used.
    * A C++ compiler supporting C++17 (for GoogleTest).
    * Git (optional, for automatic versioning).
    * Python3 (optional, for running the `test_oasis_send_recv_integration.py` script and `oasis_check_imd.py`).
    * `socat` (optional, for `test_oasis_send_recv_integration.py` on POSIX systems if not using real serial ports).

2.  **Configure:** Create a build directory and run CMake from within it.
    ```bash
    mkdir build
    cd build
    cmake ..
    ```
    * CMake automatically detects the platform and selects the correct `mm_serial` implementation.
    * GoogleTest and `libimd` (ImageDisk library) are fetched automatically by CMake.

3.  **Build:** Compile the project using the build tool specified by your CMake generator.
    ```bash
    cmake --build . --config Release
    # Or, if using Make:
    # make
    # Or, if using Ninja:
    # ninja
    ```
    The executables (`oasis_disk_util`, `oasis_chkdsk`, `oasis_send`, `oasis_recv`, `oasis_initdisk`) will be placed in the build directory (or a configuration-specific subdirectory like `build/Release`).

4.  **(Optional) Testing:** Run the test suite using CTest.
    ```bash
    ctest -C Release --output-on-failure
    ```
    Test data (disk images) for some tests are located in the `disk-images` directory. The CMake variable `OASIS_DISK_IMAGE_FOR_TESTS` points to an image used by several C++ tests. The Python integration test `test_oasis_send_recv_integration.py` requires specific setup (socat or COM ports).

5.  **(Optional) Installation:** Install the built executables and library.
    ```bash
    cmake --install . --prefix /path/to/install --config Release
    ```
    This installs binaries to `<prefix>/bin`, `liboasis` to `<prefix>/lib`, headers to `<prefix>/include/oasis_utils`, and the Lua Wireshark dissector to `<prefix>/share/oasis_utils/wireshark`.

### Windows Build Notes

The `CMakeLists.txt` supports both 32-bit (x86) and 64-bit (x64) builds using MSVC.
* **Visual Studio Generators:** Use the `-A` flag (e.g., `-A Win32` for x86, `-A x64` for x64).
* **Ninja or NMake Makefiles:** Run `cmake` from the correct Developer Command Prompt environment (e.g., "x86 Native Tools" for 32-bit, "x64 Native Tools" for 64-bit).
* The `CMakeSettings.json` file provides configurations for Visual Studio integration using the Ninja generator.
* The GitHub Actions workflow also uses `-A Win32` in the CMake configure step for the 32-bit Windows build.

## Wireshark Dissector

A Lua script (`wireshark/oasis_wireshark_dissector.lua`) is provided to dissect the OASIS serial transfer protocol. This is useful for analyzing PCAP files generated by `oasis_send` or `oasis_recv`.

See `wireshark/README.md` for installation and usage instructions.

## Continuous Integration / Continuous Deployment (CI/CD)

This project uses GitHub Actions for CI/CD:
* **Coverity Scan:** (`.github/workflows/coverity.yml`)
    * Runs Coverity static analysis on pushes to the `main` branch.
* **Release Workflow:** (`.github/workflows/release.yml`)
    * **Triggers:** Runs on pushes to the `main` branch and when a release is manually created on GitHub.
    * **Pre-Releases (on push to `main`):**
        1.  A tag `commit-<short-sha>` is generated.
        2.  A GitHub Pre-Release is created using this tag.
        3.  Build artifacts for Linux (x64), macOS (x64), and Windows (x64, x86) are created and uploaded.
    * **Full Releases (on release creation event):**
        1.  The workflow uses the actual tag from the release event.
        2.  Builds, tests, and uploads artifacts similarly to pre-releases.
    * **Build, Test, and Upload Process:**
        1.  Code is checked out.
        2.  Project configured using CMake for Linux-x64, macOS-x64, Windows-x64, Windows-x86.
        3.  Project built.
        4.  Tests executed using CTest.
        5.  Artifacts packaged using CPack (`.tar.gz` for Linux/macOS, `.zip` for Windows).
        6.  Packaged artifacts uploaded to the GitHub Release.

## Dependencies

* **Build System:** CMake (version 3.24 or higher).
* **C/C++ Compiler:** Standard C11 compiler (e.g., GCC, Clang, MSVC) and C++17 for tests.
* **Git:** Optional, but recommended for versioning.
* **[libimd](https://github.com/hharte/libimd):** Fetched by CMake. Used for `.imd` file support.
* **GoogleTest:** Fetched by CMake. Used for C++ unit tests.
* **Python 3:** (Optional) For `oasis_check_imd.py` and some integration tests.
    * `termcolor` Python package (for `oasis_check_imd.py` colored output).
* **socat:** (Optional) For POSIX serial integration testing.

## License

Licensed under the MIT License.

## Related Projects

* [OASIS Users' Group](https://github.com/hharte/oasis_users_group) - Archive of disk images for the OASIS Users' Group (1981-1985)
* [Digitex 8200 Computer](https://github.com/hharte/digitex) - Digitex 8200 S-100 machine, can run OASIS 5.6 and CP/M.  Simulated by the [SIMH simulator](https://github.com/open-simh/simh).
* [Integrated Business Computers](https://github.com/hharte/integrated-business-computers) - The IBC CADET (MegaStar) and MIDDI CADET (MultiStar) run OASIS and can be simulated by the [SIMH simulator](https://github.com/open-simh/simh).
* [Bitsavers Phase One Systems Archive](http://bitsavers.org/pdf/phaseOneSystems/oasis/) - Archive of manuals for OASIS.
* [THEOS / OASIS User's Handbook](https://bitsavers.org/pdf/phaseOneSystems/THEOS_OASIS_Users_Handbook_1985.pdf) - PDF of the THEOS/OASIS User's Handbook from 1985.
* [libimd](https://github.com/hharte/libimd) - Cross-Platform ImageDisk (.imd) library for manipulating floppy disk images.
