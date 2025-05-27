/*
 * oasis_ascii.c - Implementation of OASIS ASCII line ending conversion
 *
 * Provides functions to convert between host and OASIS line endings.
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#include "oasis.h"

#include <stdio.h>      /* For FILE, fseek, ftell, fread, fwrite, fopen, fclose, perror */
#include <stdlib.h>     /* For malloc, free */
#include <string.h>     /* For memset */

/* Platform-specific line endings */
#ifdef _WIN32
    /* Windows uses CRLF */
#define HOST_LINE_ENDING "\r\n"
#define HOST_LINE_ENDING_LEN 2
#elif defined(__APPLE__) || defined(__linux__) || defined(__unix__)
    /* macOS (OS X and later), Linux, Unix use LF */
#define HOST_LINE_ENDING "\n"
#define HOST_LINE_ENDING_LEN 1
#else
#warning "Unknown host OS, defaulting to LF line endings."
#define HOST_LINE_ENDING "\n"
#define HOST_LINE_ENDING_LEN 1
#endif

#define OASIS_LINE_ENDING '\r'
#define OASIS_LINE_ENDING_LEN 1
#define OASIS_SUB_CHAR 0x1A /* Standard SUB character (Ctrl-Z) */


/* --- Helper Functions --- */

/* Safely updates the maximum line length */
static void update_max_len(size_t current_len, size_t* max_len) {
    if (current_len > *max_len) {
        *max_len = current_len;
    }
}

/* --- Public API Functions --- */

int32_t ascii_oasis_to_host(const uint8_t* input_buffer, size_t input_len,
    uint8_t* output_buffer, size_t output_buffer_capacity,
    conversion_result_t* result)
{
    size_t i = 0; /* Input index */
    size_t out_idx = 0; /* Output index */
    size_t current_line_len = 0;
    int trailing_char_exists = 0;

    /* Input validation */
    if ((input_len > 0 && !input_buffer) ||
        (output_buffer_capacity > 0 && !output_buffer) ||
        !result) {
        return OASIS_ERR_NULL_POINTER;
    }

    /* Initialize result struct */
    memset(result, 0, sizeof(conversion_result_t));

    if (input_len == 0) {
        return 0;
    }

    while (i < input_len) {
        uint8_t current_char = input_buffer[i];

        if (current_char == OASIS_SUB_CHAR) { /* SUB character is EOF for OASIS ASCII */
            break;
        }

        if (current_char == OASIS_LINE_ENDING) {
            if (out_idx + HOST_LINE_ENDING_LEN > output_buffer_capacity) {
                result->output_chars = out_idx;
                return OASIS_ERR_BUFFER_TOO_SMALL;
            }
#ifdef _WIN32
            output_buffer[out_idx++] = '\r';
            output_buffer[out_idx++] = '\n';
#else
            output_buffer[out_idx++] = '\n';
#endif
            result->output_lines++;
            update_max_len(current_line_len, &result->max_line_len);
            current_line_len = 0;
            trailing_char_exists = 0;
            i++;
        }
        else {
            if (out_idx + 1 > output_buffer_capacity) {
                result->output_chars = out_idx;
                return OASIS_ERR_BUFFER_TOO_SMALL;
            }
            output_buffer[out_idx++] = current_char;
            current_line_len++;
            trailing_char_exists = 1;
            i++;
        }
    }

    if (trailing_char_exists || current_line_len > 0) {
        result->output_lines++;
        update_max_len(current_line_len, &result->max_line_len);
    }

    result->output_chars = out_idx;
    return (int32_t)out_idx;
}

int32_t ascii_host_to_oasis(const uint8_t* input_buffer, size_t input_len,
    uint8_t* output_buffer, size_t output_buffer_capacity,
    conversion_result_t* result)
{
    size_t i = 0;
    size_t out_idx = 0;
    size_t current_line_len = 0;
    int trailing_char_exists = 0;

    if ((input_len > 0 && !input_buffer) ||
        (output_buffer_capacity > 0 && !output_buffer) ||
        !result) {
        return OASIS_ERR_NULL_POINTER;
    }

    memset(result, 0, sizeof(conversion_result_t));

    if (input_len == 0) {
        return 0;
    }

    while (i < input_len) {
        uint8_t current_char = input_buffer[i];
        int line_ending_found = 0;

#ifdef _WIN32
        if (current_char == '\r' && (i + 1) < input_len && input_buffer[i + 1] == '\n') {
            line_ending_found = 1;
            i += 2;
        }
        else if (current_char == '\n' || current_char == '\r') {
            line_ending_found = 1;
            i += 1;
        }
#else
        if (current_char == '\n') {
            line_ending_found = 1;
            i += 1;
        }
#endif

        if (line_ending_found) {
            if (out_idx + OASIS_LINE_ENDING_LEN > output_buffer_capacity) {
                result->output_chars = out_idx;
                return OASIS_ERR_BUFFER_TOO_SMALL;
            }
            output_buffer[out_idx++] = OASIS_LINE_ENDING;
            result->output_lines++;
            update_max_len(current_line_len, &result->max_line_len);
            current_line_len = 0;
            trailing_char_exists = 0;
        }
        else {
            if (out_idx + 1 > output_buffer_capacity) {
                result->output_chars = out_idx;
                return OASIS_ERR_BUFFER_TOO_SMALL;
            }
            output_buffer[out_idx++] = current_char;
            current_line_len++;
            trailing_char_exists = 1;
            i++;
        }
    }

    if (trailing_char_exists || current_line_len > 0) {
        result->output_lines++;
        update_max_len(current_line_len, &result->max_line_len);
    }

    result->output_chars = out_idx;
    return (int32_t)out_idx;
}


int is_ascii(const uint8_t* buffer, size_t len)
{
    size_t i;

    if (len > 0 && !buffer) {
        return 0;
    }

    for (i = 0; i < len; ++i) {
        if (buffer[i] & 0x80) {
            return 0;
        }
    }
    return 1;
}


int oasis_ascii_file_to_host_file(const char* inputFileName, const char* outputFileName) {
    FILE* inputFile = NULL;
    FILE* actualOutputFile = NULL;
    unsigned char* fileBuffer = NULL;
    unsigned char* convertedBuffer = NULL;
    long fileSize = 0;
    size_t bytesRead;
    conversion_result_t conversionResult;
    int32_t conversionStatus;
    int final_status = 0; /* Success by default */

    /* --- Input Validation --- */
    if (inputFileName == NULL) {
        return OASIS_ERR_NULL_POINTER;
    }

    /* --- Open input file --- */
    inputFile = fopen(inputFileName, "rb");
    if (inputFile == NULL) {
        return OASIS_ERR_FILE_IO;
    }

    /* --- Read entire file into memory --- */
    if (fseek(inputFile, 0, SEEK_END) != 0) {
        fclose(inputFile);
        return OASIS_ERR_FILE_IO;
    }
    fileSize = ftell(inputFile);
    if (fileSize < 0) {
        fclose(inputFile);
        return OASIS_ERR_FILE_IO;
    }
    if (fseek(inputFile, 0, SEEK_SET) != 0) { /* Rewind */
        fclose(inputFile);
        return OASIS_ERR_FILE_IO;
    }

    if (fileSize == 0) { /* Empty input file */
        fclose(inputFile);
        inputFile = NULL;

        const char* targetWriteName = (outputFileName == NULL) ? inputFileName : outputFileName;
        actualOutputFile = fopen(targetWriteName, "wb");
        if (actualOutputFile == NULL) {
            return OASIS_ERR_FILE_IO;
        }
        if (fclose(actualOutputFile) != 0) {
            return OASIS_ERR_FILE_IO;
        }
        return 0;
    }

    fileBuffer = (unsigned char*)malloc(fileSize);
    if (fileBuffer == NULL) {
        fclose(inputFile);
        return OASIS_ERR_MEMORY_ALLOC;
    }

    bytesRead = fread(fileBuffer, 1, fileSize, inputFile);
    fclose(inputFile);
    inputFile = NULL;

    if (bytesRead != (size_t)fileSize) {
        free(fileBuffer);
        return OASIS_ERR_FILE_IO;
    }

    /* --- Check if content is 7-bit ASCII --- */
    if (!is_ascii(fileBuffer, bytesRead)) {
        free(fileBuffer);
        return OASIS_ERR_INVALID_INPUT; /* Not 7-bit ASCII */
    }

    /* --- Convert line endings --- */
    size_t estimatedOutputCapacity = (size_t)fileSize * HOST_LINE_ENDING_LEN + 1;
    if (HOST_LINE_ENDING_LEN == OASIS_LINE_ENDING_LEN) {
        estimatedOutputCapacity = (size_t)fileSize + 1;
    }
    convertedBuffer = (unsigned char*)malloc(estimatedOutputCapacity);
    if (convertedBuffer == NULL) {
        free(fileBuffer);
        return OASIS_ERR_MEMORY_ALLOC;
    }

    conversionStatus = ascii_oasis_to_host(fileBuffer, bytesRead,
        convertedBuffer, estimatedOutputCapacity,
        &conversionResult);
    free(fileBuffer); /* Original buffer no longer needed */

    if (conversionStatus < 0) {
        free(convertedBuffer);
        return (int)conversionStatus; /* Return error from conversion */
    }

    /* --- Open output file and write converted data --- */
    const char* targetWriteName = (outputFileName == NULL) ? inputFileName : outputFileName;
    actualOutputFile = fopen(targetWriteName, "wb"); /* "wb" truncates/creates for writing */
    if (actualOutputFile == NULL) {
        free(convertedBuffer);
        return OASIS_ERR_FILE_IO;
    }

    if (conversionResult.output_chars > 0) {
        if (fwrite(convertedBuffer, 1, conversionResult.output_chars, actualOutputFile) != conversionResult.output_chars) {
            final_status = OASIS_ERR_FILE_IO;
            /* Fall through to cleanup */
        }
    }

    /* --- Cleanup --- */
    free(convertedBuffer);
    if (actualOutputFile != NULL) {
        if (fclose(actualOutputFile) != 0) {
            if (final_status == 0) { /* If no prior error, this becomes the error */
                final_status = OASIS_ERR_FILE_IO;
            }
        }
    }
    return final_status;
}
