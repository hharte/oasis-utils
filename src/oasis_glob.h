/*
 * oasis_glob.h - Platform-Specific Filename Wildcard Expansion (Globbing) Interface
 *
 * This header file declares functions for expanding wildcard patterns (globbing)
 * into a list of matching filenames, primarily intended for Windows platforms
 * where the shell might not perform this expansion by default.
 * Part of the oasis-utils library.
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#ifndef OASIS_GLOB_H_
#define OASIS_GLOB_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Expands a wildcard pattern into a list of matching filenames (Windows-specific).
 *
 * This function uses Windows API calls (`FindFirstFileA`, `FindNextFileA`) to find
 * all files in the current working directory that match the given pattern.
 * It allocates memory for the list of found filenames.
 *
 * @param pattern The wildcard pattern (e.g., "*.txt", "file?.dat") to match against
 * files in the current directory.
 * @param found_files Pointer to a `char**` which, on success (return 0 or 1), will
 * be updated to point to a dynamically allocated array of C-strings.
 * Each string in the array is a matched filename. The caller is
 * responsible for freeing this array and each string within it
 * using `platform_free_glob_result()`. On error (return -1),
 * `*found_files` will be set to NULL.
 * @param num_found Pointer to an `int` where the number of found files (i.e., the
 * size of the `*found_files` array) will be stored. Set to 0 on
 * error or if no files match.
 *
 * @return 0 if one or more files were found and successfully listed.
 * 1 if no files matched the pattern (this is considered a success, but
 * `*num_found` will be 0 and `*found_files` will be NULL or empty).
 * -1 on system error (e.g., memory allocation failure, invalid pattern argument,
 * or other `FindFirstFileA`/`FindNextFileA` error other than no match).
 * On error, `*found_files` is NULL and `*num_found` is 0.
 */
int platform_glob_win32(const char* pattern, char*** found_files, int* num_found);

/**
 * @brief Frees the memory allocated by `platform_glob_win32` for the list of filenames.
 *
 * This function iterates through the array of filenames, freeing each individual
 * string, and then frees the array itself.
 *
 * @param found_files The array of filenames (char**) previously allocated by
 * `platform_glob_win32`. Can be NULL.
 * @param num_found The number of filenames in the `found_files` array.
 * If `found_files` is NULL, this value is ignored.
 */
void platform_free_glob_result(char** found_files, int num_found);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OASIS_GLOB_H_ */
