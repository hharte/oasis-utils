/*
 * oasis_glob_win32.c - Windows-specific wildcard expansion (globbing)
 *
 * Implements globbing functionality using Windows API calls
 * (FindFirstFileA, FindNextFileA).
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#if defined(_WIN32) || defined(_WIN64)

#include "oasis.h"

#include <windows.h>
#include <stdio.h>  /* For fprintf, perror */
#include <stdlib.h> /* For malloc, realloc, free */
#include <string.h> /* For strdup, strcpy */

int platform_glob_win32(const char* pattern, char*** found_files, int* num_found) {
    WIN32_FIND_DATAA find_file_data;
    HANDLE h_find = INVALID_HANDLE_VALUE;
    char** result_list = NULL;
    int count = 0;
    int capacity = 0;
    DWORD dw_error = 0;

    /* Initialize output parameters */
    *found_files = NULL;
    *num_found = 0;

    if (pattern == NULL) {
        return -1; /* Invalid argument */
    }

    h_find = FindFirstFileA(pattern, &find_file_data);

    if (h_find == INVALID_HANDLE_VALUE) {
        dw_error = GetLastError();
        if (dw_error == ERROR_FILE_NOT_FOUND) {
            return 1; /* No files match the pattern - not an error for glob */
        }
        /*
         * fprintf(stderr, "Error: FindFirstFileA failed with error %lu for pattern '%s'\n", dw_error, pattern);
         */
        return -1; /* Other error */
    }

    do {
        /* Skip directories, process only files */
        if (!(find_file_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            if (count >= capacity) {
                int new_capacity = (capacity == 0) ? 8 : capacity * 2;
                char** new_list = (char**)realloc(result_list, new_capacity * sizeof(char*));
                if (new_list == NULL) {
                    perror("Error: realloc failed in platform_glob_win32");
                    FindClose(h_find);
                    platform_free_glob_result(result_list, count); /* Clean up what we have */
                    return -1;
                }
                result_list = new_list;
                capacity = new_capacity;
            }
            result_list[count] = _strdup(find_file_data.cFileName);
            if (result_list[count] == NULL) {
                perror("Error: _strdup failed in platform_glob_win32");
                FindClose(h_find);
                platform_free_glob_result(result_list, count);
                return -1;
            }
            count++;
        }
    } while (FindNextFileA(h_find, &find_file_data) != 0);

    dw_error = GetLastError();
    FindClose(h_find);

    if (dw_error != ERROR_NO_MORE_FILES) {
        /*
         * fprintf(stderr, "Error: FindNextFileA failed with error %lu\n", dw_error);
         */
        platform_free_glob_result(result_list, count);
        return -1;
    }

    if (count > 0) {
        *found_files = result_list;
        *num_found = count;
        return 0; /* Success, files found */
    } else {
        /* No files matched (after filtering out directories) or only directories matched */
        platform_free_glob_result(result_list, count); /* Should be NULL, 0 if no files */
        return 1; /* Success, no files matched */
    }
}

void platform_free_glob_result(char** found_files, int num_found) {
    if (found_files != NULL) {
        for (int i = 0; i < num_found; i++) {
            if (found_files[i] != NULL) {
                free(found_files[i]);
            }
        }
        free(found_files);
    }
}

#endif /* defined(_WIN32) || defined(_WIN64) */
