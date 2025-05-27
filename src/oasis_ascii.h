/*
 * oasis_ascii.h - OASIS ASCII Line Ending Conversion Utilities Interface
 *
 * This header file declares functions for converting text between OASIS
 * (CR line endings, SUB for EOF) and host native line endings (LF or CRLF).
 * It also includes a utility to check if a buffer contains only 7-bit ASCII.
 * Part of the oasis-utils library.
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#ifndef OASIS_ASCII_H_
#define OASIS_ASCII_H_

#include <stdint.h>
#include <stddef.h>
#include <stdio.h> /* For FILE in oasis_ascii_file_to_host_file */


#ifdef __cplusplus
extern "C" {
#endif

    /**
     * @brief Structure to hold the results of a line ending conversion operation.
     */
    typedef struct {
        size_t output_chars; /**< Total characters (bytes) written to the output buffer. */
        size_t output_lines; /**< Total lines detected/written in the output buffer. */
        size_t max_line_len; /**< Length of the longest line in the output (excluding line ending). */
    } conversion_result_t;

    /* Error Codes for ASCII conversion functions */
#define OASIS_ERR_NULL_POINTER     -1 /**< Input/Output buffer or result pointer is NULL. */
#define OASIS_ERR_BUFFER_TOO_SMALL -2 /**< Output buffer capacity is insufficient. */
#define OASIS_ERR_INVALID_INPUT    -3 /**< Input data is not valid (e.g., non-7-bit ASCII for ASCII conversion). */
#define OASIS_ERR_FILE_IO          -4 /**< General file I/O error during file operations. */
#define OASIS_ERR_MEMORY_ALLOC     -5 /**< Memory allocation error during file operations. */


    /**
     * @brief Converts text from OASIS format (CR line endings) to the host's native format.
     *
     * Detects the host OS at compile time to determine the target line ending
     * (CRLF for Windows, LF for Linux/macOS). Stops conversion if an
     * OASIS_SUB_CHAR (0x1A) is encountered in the input, treating it as EOF.
     *
     * @param input_buffer Pointer to the buffer containing OASIS formatted text.
     * @param input_len Length of the data in the input buffer.
     * @param output_buffer Pointer to the buffer where the host formatted text will be written.
     * @param output_buffer_capacity The maximum number of bytes that can be written to output_buffer.
     * @param result Pointer to a conversion_result_t struct to store conversion statistics.
     * @return The number of bytes written to output_buffer on success.
     * A negative error code (OASIS_ERR_*) on failure.
     */
    int32_t ascii_oasis_to_host(const uint8_t* input_buffer, size_t input_len,
        uint8_t* output_buffer, size_t output_buffer_capacity,
        conversion_result_t* result);

    /**
     * @brief Converts text from the host's native format to OASIS format (CR line endings).
     *
     * Detects the host OS at compile time to determine the source line ending
     * (CRLF for Windows, LF for Linux/macOS). Handles conversion from both
     * LF and CRLF (on Windows) to CR.
     *
     * @param input_buffer Pointer to the buffer containing host formatted text.
     * @param input_len Length of the data in the input buffer.
     * @param output_buffer Pointer to the buffer where the OASIS formatted text will be written.
     * @param output_buffer_capacity The maximum number of bytes that can be written to output_buffer.
     * @param result Pointer to a conversion_result_t struct to store conversion statistics.
     * @return The number of bytes written to output_buffer on success.
     * A negative error code (OASIS_ERR_*) on failure.
     */
    int32_t ascii_host_to_oasis(const uint8_t* input_buffer, size_t input_len,
        uint8_t* output_buffer, size_t output_buffer_capacity,
        conversion_result_t* result);

    /**
     * @brief Checks if a buffer contains only 7-bit ASCII characters.
     *
     * Examines each byte in the buffer. If any byte has its most significant bit
     * (bit 7) set, the buffer is considered non-ASCII.
     *
     * @param buffer Pointer to the buffer to check.
     * @param len Length of the data in the buffer.
     * @return 1 if the buffer contains only 7-bit ASCII characters or if len is 0.
     * 0 otherwise (non-ASCII or NULL buffer with len > 0).
     */
    int is_ascii(const uint8_t* buffer, size_t len);

    /**
    * @brief Reads an OASIS ASCII file, converts its line endings to the host's
    * native format, and writes it to an output file or back to the input file.
    *
    * The function reads the entire input file into memory. If the content is not
    * 7-bit ASCII, it returns an OASIS_ERR_INVALID_INPUT error. Otherwise, it
    * performs the line ending conversion and then writes the result. OASIS ASCII
    * files use CR (0x0D) as line endings and may use SUB (0x1A) as an EOF marker.
    * Host native line endings are LF (0x0A) for Unix-like systems and CRLF (0x0D0A)
    * for Windows. The SUB character is removed during conversion to host format.
    *
    * @param inputFileName Constant C string specifying the path to the input OASIS ASCII file.
    * @param outputFileName Constant C string specifying the path to the output host ASCII file.
    * If NULL, the converted content will overwrite the input file specified
    * by `inputFileName`.
    * @return 0 on success.
    * A negative error code (OASIS_ERR_*) on failure, such as:
    * OASIS_ERR_NULL_POINTER if `inputFileName` is NULL.
    * OASIS_ERR_FILE_IO if there's an error opening, reading from, or writing to files.
    * OASIS_ERR_MEMORY_ALLOC if memory allocation fails.
    * OASIS_ERR_INVALID_INPUT if the input file contains non-7-bit ASCII characters.
    */
    int oasis_ascii_file_to_host_file(const char* inputFileName, const char* outputFileName);


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OASIS_ASCII_H_ */
