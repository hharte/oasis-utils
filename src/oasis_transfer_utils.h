/*
 * oasis_transfer_utils.h - Shared Utilities for OASIS Serial Transfer Programs
 *
 * This header file declares common structures and functions used by both
 * oasis_send and oasis_recv utilities, such as argument parsing helpers,
 * session management, and miscellaneous utilities like cross-platform sleep.
 * Part of the oasis-utils library.
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#ifndef OASIS_TRANSFER_UTILS_H_
#define OASIS_TRANSFER_UTILS_H_

#include <stdio.h>  /* For FILE */
#include <stdint.h> /* For standard integer types */

/* Forward declaration for oasis.h contents if SECTOR_SIZE is needed here, or include oasis.h */
#include "oasis.h" /* For SECTOR_SIZE needed by DEFAULT_RECORD_LENGTH */

/* Default baud rate if not specified by the user. */
#define DEFAULT_BAUD_RATE 19200
/* Default record length for certain file types if not determinable otherwise. */
#define DEFAULT_RECORD_LENGTH SECTOR_SIZE

/**
 * @brief Structure to hold common command-line arguments for send/receive utilities.
 */
typedef struct oasis_transfer_common_args {
    char    port_path[256];       /**< Path to the serial port device (e.g., "COM1", "/dev/ttyS0"). */
    int     quiet;                /**< If non-zero, suppress non-essential informational output. */
    int     debug;                /**< If non-zero, enable verbose debug messages to stderr. */
    int     baud_rate;            /**< Serial port baud rate (e.g., 9600, 19200). */
    int     pacing_packet_ms;     /**< Optional delay in milliseconds after sending/receiving each packet. */
    char    pcap_filename[256];   /**< Filename for PCAP logging. Empty string if not enabled. */
    int     ascii_conversion;     /**< If non-zero, enable ASCII mode (line ending conversion, SUB as EOF). */
    int     flow_control;         /**< If non-zero (default), hardware flow control (RTS/CTS) is enabled. 0 to disable. */
} oasis_transfer_common_args_t;

/**
 * @brief Structure to hold the state of an active serial transfer session.
 */
typedef struct oasis_transfer_session {
    int   serial_fd;              /**< File descriptor for the open serial port. -1 if not open. */
    FILE* pcap_stream;            /**< File stream for PCAP logging. NULL if PCAP is not active. */
    const oasis_transfer_common_args_t* common_args; /**< Pointer to the common arguments for this session. */
} oasis_transfer_session_t;

#ifdef __cplusplus
extern "C" {
#endif

    /**
     * @brief Initializes a transfer session.
     *
     * This function performs the necessary setup for a serial transfer session:
     * 1. Opens the serial port specified in `common_args->port_path`.
     * 2. Initializes the serial port with settings from `common_args` (baud rate, flow control).
     * 3. If a PCAP filename is provided in `common_args->pcap_filename`, creates and
     * opens the PCAP file for logging.
     * The initialized state (serial file descriptor, PCAP file stream) is stored in the `session` structure.
     *
     * @param common_args Pointer to a `oasis_transfer_common_args_t` structure containing
     * the parsed command-line arguments and settings for the session.
     * @param session Pointer to an `oasis_transfer_session_t` structure that will be
     * initialized with the session state.
     * @return 0 on successful initialization.
     * -1 on failure (e.g., cannot open serial port, cannot initialize serial port,
     * PCAP file creation failed if specified as critical). Error messages
     * are printed to stderr.
     */
    int initialize_transfer_session(const oasis_transfer_common_args_t* common_args, oasis_transfer_session_t* session);

    /**
     * @brief Cleans up and finalizes a transfer session.
     *
     * This function closes the serial port and the PCAP file stream (if they were opened)
     * associated with the session.
     *
     * @param session Pointer to the `oasis_transfer_session_t` structure to clean up.
     * If NULL, the function does nothing.
     */
    void cleanup_transfer_session(oasis_transfer_session_t* session);

    /**
     * @brief Pauses execution for a specified number of milliseconds.
     *
     * This is a cross-platform utility function for introducing small delays.
     * Uses `Sleep()` on Windows and `usleep()` on POSIX-compliant systems.
     *
     * @param milliseconds The duration to sleep, in milliseconds. If non-positive,
     * the function returns immediately.
     */
    void sleep_ms_util(int milliseconds);

    /**
     * @brief Attempts to parse one common command-line option from `argv`.
     *
     * This function inspects the argument at `argv[*idx_ptr]`. If it matches a
     * recognized common option (e.g., -q, --debug, -b <rate>), it parses the
     * option (and its value, if required), updates the `common_args` structure,
     * and advances `*idx_ptr` past the consumed argument(s).
     *
     * @param argc The total number of command-line arguments (from `main`).
     * @param argv The array of command-line argument strings (from `main`).
     * @param idx_ptr Pointer to an integer representing the current index in `argv`
     * to be processed. This index will be advanced by the function
     * if an option and its value are consumed.
     * @param common_args Pointer to the `oasis_transfer_common_args_t` structure
     * where parsed common arguments will be stored.
     *
     * @return
     * 1: A common option was successfully parsed. `*idx_ptr` is advanced accordingly.
     * 0: The argument at `argv[*idx_ptr]` is not a recognized common option.
     * `*idx_ptr` remains unchanged.
     * -1: A recognized common option was found, but there was an error parsing its
     * value (e.g., missing value, invalid format). An error message is printed
     * to stderr. `*idx_ptr`'s state is not guaranteed on error.
     * -2: A help option (-h or --help) was detected. `*idx_ptr` is advanced.
     */
    int parse_one_common_option(int argc, char* argv[], int* idx_ptr, oasis_transfer_common_args_t* common_args);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OASIS_TRANSFER_UTILS_H_ */
