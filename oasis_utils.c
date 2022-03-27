/*
 * OASIS Utility Functions
 *
 * www.github.com/hharte/oasis
 *
 * Copyright (c) 2021-2022, Howard M. Harte
 *
 * Reference: http://bitsavers.org/pdf/phaseOneSystems/oasis/
 *
 */

#if defined(_WIN32)
# define _CRT_SECURE_NO_DEPRECATE
# include <windows.h>
#else  /* if defined(_WIN32) */
# include <sys/types.h>
# include <utime.h>
#endif /* if defined(_WIN32) */

#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "./oasis.h"
#include "./oasis_utils.h"

const char kPathSeparator =
#ifdef _WIN32
    '\\';
#else  /* ifdef _WIN32 */
    '/';
#endif /* ifdef _WIN32 */

/* Packed timestamp:
 * +-------------------------------------------------+
 * | BYTE  |      0      |      1      |      2      |
 * | FIELD | MMMM | DDDD | DYYY | YHHH | HHMM | MMMM |
 * | BIT   | 7         0 | 7         0 | 7         0 |
 * +-------------------------------------------------+
 */
void oasis_convert_timestamp_to_tm(oasis_tm_t *timestamp, struct tm *tmout) {
    tmout->tm_mon  = ((timestamp->raw[0] & 0xF0) >> 4) - 1;
    tmout->tm_mday =
        ((timestamp->raw[0] & 0x0F) << 1) | ((timestamp->raw[1] & 0x80) >> 7);

    /* 0 = 1977, highest valid year is 1992. */
    tmout->tm_year = ((timestamp->raw[1] & 0x78) >> 3) + 77;
    tmout->tm_hour =
        ((timestamp->raw[1] & 0x07) << 2) | ((timestamp->raw[2] & 0xc0) >> 6);
    tmout->tm_min   = timestamp->raw[2] & 0x3F;
    tmout->tm_sec   = 0;
    tmout->tm_isdst = 0;
}

void oasis_convert_tm_to_timestamp(struct tm *tmin, oasis_tm_t *timestamp) {
    timestamp->raw[0]  = ((tmin->tm_mon & 0xF) + 1) << 4;
    timestamp->raw[0] |= ((tmin->tm_mday >> 1) & 0x0F);
    timestamp->raw[1]  = ((tmin->tm_mday & 1) << 7);
    timestamp->raw[1] |= ((tmin->tm_year - 77) & 0x0F) << 3;
    timestamp->raw[1] |= ((tmin->tm_hour >> 2) & 0x07);
    timestamp->raw[2]  = ((tmin->tm_hour & 0x03) << 6);
    timestamp->raw[2] |= ((tmin->tm_min & 0x3F));
}

void oasis_list_dir_entry(directory_entry_block_t *dir_entry) {
    char fname[FNAME_LEN + 1];
    char fext[FEXT_LEN + 1];
    char format[8];
    char other_str[8];
    struct tm tmout;
    char buf[17];

    if (dir_entry->file_format == 0) {
        return;
    }

    snprintf(fname,     sizeof(fname),     "%s", dir_entry->file_name);
    snprintf(fext,      sizeof(fext),      "%s", dir_entry->file_type);
    snprintf(other_str, sizeof(other_str), "       ");

    switch (dir_entry->file_format) {
        case FILE_FORMAT_DELETED:
            snprintf(format, sizeof(format), "Deleted");
            break;
        case FILE_FORMAT_EMPTY:
            snprintf(format, sizeof(format), "Empty  ");
            break;
        case FILE_FORMAT_SYNONYM:
            snprintf(format, sizeof(format), "Synonym");
            break;
        default:

            switch (dir_entry->file_format & 0x1F) {
                case FILE_FORMAT_RELOCATABLE:
                    snprintf(format,
                             sizeof(format),
                             "R %5d",
                             dir_entry->file_format_dependent1);

                    if (dir_entry->file_format_dependent2 > 0) {
                        snprintf(other_str,
                                 sizeof(other_str),
                                 "%5d L",
                                 dir_entry->file_format_dependent2);
                    }
                    break;
                case FILE_FORMAT_ABSOLUTE:
                    snprintf(format,
                             sizeof(format),
                             "A %5d",
                             dir_entry->file_format_dependent1);
                    snprintf(other_str,
                             sizeof(other_str),
                             "%5d O",
                             dir_entry->file_format_dependent2);
                    break;
                case FILE_FORMAT_SEQUENTIAL:
                    snprintf(format,
                             sizeof(format),
                             "S %5d",
                             dir_entry->file_format_dependent1);

                    if (dir_entry->file_format_dependent2 > 0) {
                        snprintf(other_str,
                                 sizeof(other_str),
                                 "%5d E",
                                 dir_entry->file_format_dependent2);
                    }
                    break;
                case FILE_FORMAT_DIRECT:
                    snprintf(format,
                             sizeof(format),
                             "D %5d",
                             dir_entry->file_format_dependent1);
                    break;
                case FILE_FORMAT_INDEXED:
                    snprintf(format,
                             sizeof(format),
                             "I%3d/%3d",
                             dir_entry->file_format_dependent1 & 0x1F,
                             (dir_entry->file_format_dependent1 & 0xFE) >> 9);
                    break;
                case FILE_FORMAT_KEYED:
                    snprintf(format,
                             sizeof(format),
                             "K%3d/%3d",
                             dir_entry->file_format_dependent1 & 0x1F,
                             (dir_entry->file_format_dependent1 & 0xFE) >> 9);
                    break;
                default:
                    printf("Unsupported file type %x\n", dir_entry->file_format);
                    break;
            }
    }

    oasis_convert_timestamp_to_tm(&dir_entry->timestamp, &tmout);
    strftime(buf, 17, "%m/%d/%y %H:%M", &tmout);
    printf("%s %s %s %5d %4d %s %5d %3d %3d  %s\n",
           fname,
           fext,
           buf,
           dir_entry->record_count,
           dir_entry->block_count,
           format,
           dir_entry->start_sector,
           dir_entry->owner_id,
           dir_entry->shared_from_owner_id,
           other_str);
}

void set_file_time(char *output_filename, struct tm *tmin) {
#if defined(_WIN32)
    FILETIME   file_time;
    SYSTEMTIME system_time;
    HANDLE     hOstream;

    GetSystemTime(&system_time);

    system_time.wDay    = tmin->tm_mday;
    system_time.wMonth  = tmin->tm_mon;
    system_time.wYear   = tmin->tm_year + 1900;
    system_time.wHour   = tmin->tm_hour;
    system_time.wMinute = tmin->tm_min;
    system_time.wSecond = 0;

    SystemTimeToFileTime(&system_time, &file_time);

    hOstream = CreateFileA(output_filename,
                           FILE_WRITE_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL,
                           OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL,
                           NULL);
    SetFileTime(hOstream, &file_time, &file_time, &file_time);
#else /* POSIX */
    struct utimbuf file_time;

    file_time.actime  = mktime(tmin);
    file_time.modtime = mktime(tmin);

    if (utime(output_filename, &file_time)) {
        fprintf(stderr, "Error setting timestamp for %s\n", output_filename);
    }
#endif /* if defined(_WIN32) */
}

void dump_hex(uint8_t *data, int len) {
    uint8_t  ascii[32] = { 0 };
    uint8_t *pascii    = ascii;

    printf("\n");

    if (len > 0) {
        uint16_t i;
        printf("\tData:");

        for (i = 0; i < len; i++) {
            if (i % 16 == 0) {
                if (i > 0) {
                    *pascii++ = '\0';
                    printf("%s", ascii);
                }
                printf("\n\t%03x: ", i);
                pascii = ascii;
            }
            printf("%02x, ", data[i]);

            if (((data[i]) >= 0x20) && (data[i] < 0x7F)) {
                *pascii++ = data[i];
            } else {
                *pascii++ = '.';
            }
        }
        *pascii++ = '\0';

        if (strnlen((char *)ascii, sizeof(ascii)) > 0) {
            for (i = 0; i < 16 - strnlen((char *)ascii, sizeof(ascii)); i++) {
                printf("    ");
            }
            printf("%s", ascii);
        }
    }
    printf("\n");
}
