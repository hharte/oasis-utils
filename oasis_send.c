/*
 * Utility to Send a file to an OASIS system via Serial Port.
 *
 * www.github.com/hharte/oasis-utils
 *
 * Copyright (c) 2021-2022, Howard M. Harte
 *
 * Reference:
 * http://bitsavers.org/pdf/phaseOneSystems/oasis/Communications_Reference_Manual_Mar80.pdf
 * pp. 8
 *
 */

#define _CRT_SECURE_NO_DEPRECATE

#ifdef _WIN32
# include <windows.h>
#else  /* ifdef _WIN32 */
# include <string.h>
# include <stdlib.h>
# include <errno.h>
#endif /* _WIN32 */

#include <sys/stat.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>

#include "./oasis.h"
#include "./oasis_utils.h"
#include "./oasis_sendrecv.h"
#include "./mm_serial.h"

typedef struct oasis_send_args {
    char port_path[256];
    int  input_filename_index;
    int  ascii;
    int  quiet;
} oasis_send_args_t;

int parse_args(int argc, char *argv[], oasis_send_args_t *args);
int oasis_send_packet(int fd, uint8_t *buf, uint16_t len, uint8_t cmd);
int oasis_wait_for_ack(int fd);
int oasis_packet_encode(uint8_t *inbuf, uint16_t inlen, uint8_t *outbuf, uint16_t *outlen);
int oasis_open_file(directory_entry_block_t *dir_entry, FILE **ostream, char *path, int quiet);
int check_regular_file(const char *path);

int main(int argc, char *argv[]) {
    int fd;
    uint8_t commBuffer[1024] = { 0 };
    uint8_t decoded_buf[512];
    ssize_t bytes_read;
    directory_entry_block_t  DirEntry  = { 0 };
    directory_entry_block_t *pDirEntry = &DirEntry;
    FILE *istream;
    struct tm oasis_tm = { 0 };
    int file_len       = 0;
    int positional_arg_cnt;
    oasis_send_args_t args;
    char fname_str[256];
    char ftype_str[256];
    char ffmt;
    int  send_len;
    uint16_t frec_len;

    positional_arg_cnt = parse_args(argc, argv, &args);

    if (positional_arg_cnt == 0) {
        printf("OASIS Send Utility [%s] (c) 2021 - Howard M. Harte\n", VERSION);
        printf("https://github.com/hharte/oasis-utils\n\n");

        printf("usage is: %s <port> [-q] [-a] <filename>|<path>\n",    argv[0]);
        printf("\t<port> Serial Port filename.\n");
        printf("\tFlags:\n");
        printf("\t      -q       Quiet: Don't list file details during extraction.\n");
        printf("\t      -a       ASCII: Convert line endings and truncate output file at EOF.\n");
        return -EINVAL;
    }

    if ((fd = open_serial(args.port_path)) < 0) {
        printf("Error opening %s.\n", args.port_path);
        return -ENOENT;
    }

    if (init_serial(fd, 9600) != 0) {
        printf("Error initializing %s.\n", args.port_path);
        return -EIO;
    }

    for (int i = args.input_filename_index; i < argc; i++) {
        char send_filename[256];
        int  num_filled;

        strncpy(send_filename, argv[i], sizeof(send_filename) - 1);
        send_filename[sizeof(send_filename) - 1] = '\0';

        if (check_regular_file(send_filename)) {
            uint8_t *sequential_buf = NULL;

            istream  = fopen(send_filename, "rb");
            send_len = 0;
            int retries = 20;

            send_filename[sizeof(send_filename) - 1] = '\0';

            for (size_t i = 0; i < strlen(send_filename); i++) {
                if ((send_filename[i] == '.') || (send_filename[i] == '_')) {
                    send_filename[i] = ' ';
                }
            }

            num_filled = sscanf(send_filename, "%8s %8s %c %hu", fname_str, ftype_str, &ffmt, &frec_len);

            if (num_filled < 4) {
                ffmt     = 'S';
                frec_len = 0;
            }

            memcpy(pDirEntry->file_name, fname_str, 8);
            memcpy(pDirEntry->file_type, ftype_str, 8);

            switch (ffmt) {
                case 'D':
                case 'd':
                    pDirEntry->file_format = FILE_FORMAT_DIRECT;

                    fseek(istream, 0, SEEK_END);
                    file_len = ftell(istream);
                    fseek(istream, 0, SEEK_SET);

                    if (frec_len > 0) {
                        pDirEntry->record_count = (file_len + (frec_len - 1)) / frec_len;
                    } else {
                        printf("Error: frec_len = 0!\n");
                    }
                    pDirEntry->block_count            = (file_len + 1023) / 1024;
                    pDirEntry->file_format_dependent1 = frec_len;
                    pDirEntry->file_format_dependent2 = 0;
                    break;
                case 'S':
                case 's':
                default:
                    pDirEntry->file_format = FILE_FORMAT_SEQUENTIAL;

                    fseek(istream, 0, SEEK_END);
                    file_len = ftell(istream);
                    fseek(istream, 0, SEEK_SET);

                    pDirEntry->block_count = (file_len + 1023) / 1024;

                    sequential_buf = (uint8_t *)(calloc((size_t)pDirEntry->block_count * 1024, sizeof(uint8_t)));

                    if (sequential_buf == NULL) {
                        printf("Memory allocation of %d bytes failed.\n", pDirEntry->block_count * 1024);
                        return -ENOMEM;
                    }

                    memset(sequential_buf, SUB, sizeof(uint8_t) * pDirEntry->block_count * 1024);

                    int line_count = 0;
                    int max_len    = 0;
                    int cur_len    = 0;
                    int prev_cr    = 0;

                    while (!feof(istream)) {
                        int c;
                        c = fgetc(istream);

                        if (args.ascii && prev_cr && (c == LF)) {
                            /* Remove LF following CR */
                            prev_cr = 0;
                            continue;
                        }
                        sequential_buf[send_len] = c;
                        cur_len++;
                        send_len++;

                        if (c == CR) {
                            if (cur_len > max_len) max_len = cur_len - 1;
                            cur_len = 0;
                            line_count++;
                            prev_cr = 1;
                        }
                    }

                    pDirEntry->record_count           = line_count;
                    pDirEntry->file_format_dependent1 = max_len;
                    pDirEntry->file_format_dependent2 = 0;
                    break;
            }

            oasis_tm.tm_year = 91;
            oasis_tm.tm_mon  = 11;
            oasis_tm.tm_mday = 02;
            oasis_tm.tm_hour = 04;
            oasis_tm.tm_min  = 33;

            oasis_convert_tm_to_timestamp(&oasis_tm, &pDirEntry->timestamp);

            printf("Fname--- Ftype--  --Date-- Time- -Recs Blks Format- -Sect Own SOw Other-\n");
            oasis_list_dir_entry(pDirEntry);

            printf("Waiting for Receiving Station");

            do {
                commBuffer[0] = ENQ;

                if (write_serial(fd, commBuffer, 1) != 1) {
                    printf("Error writing to serial port.\n");
                }

                retries--;
                printf(".");

                if (retries == 0) {
                    printf("\nTimeout waiting for sending station.\n");
                    return -ETIME;
                }
            } while (oasis_wait_for_ack(fd) == -1);

            commBuffer[0] = ENQ;

            if (write_serial(fd, commBuffer, 1) != 1) {
                printf("Error writing to serial port.\n");
            }
            oasis_wait_for_ack(fd);

            printf("\nStart of file transfer\n");

            oasis_send_packet(fd, (uint8_t *)pDirEntry, sizeof(directory_entry_block_t), OPEN);
            oasis_wait_for_ack(fd);

            if (pDirEntry->file_format == FILE_FORMAT_DIRECT) {
                while ((bytes_read = (uint32_t)fread(&decoded_buf, 1, BLOCK_SIZE, istream))) {
                    oasis_send_packet(fd, decoded_buf, (uint16_t)bytes_read, WRITE);
                    oasis_wait_for_ack(fd);
                }
            } else { /* Sequential File */
                uint16_t sector_cnt = 1;

                if (sequential_buf != NULL) {
                    for (int bytes_sent = 0; bytes_sent <= send_len; bytes_sent += BLOCK_SIZE - 2) {
                        printf("\rSegment: %d", sector_cnt);
                        memcpy(decoded_buf, &sequential_buf[bytes_sent], BLOCK_SIZE - 2);
                        decoded_buf[BLOCK_SIZE - 2] = sector_cnt >> 8;
                        decoded_buf[BLOCK_SIZE - 1] = sector_cnt & 0xFF;
                        oasis_send_packet(fd, decoded_buf, BLOCK_SIZE, WRITE);
                        oasis_wait_for_ack(fd);
                        sector_cnt++;
                    }
                }
            }

            if (sequential_buf != NULL) free(sequential_buf);
            fclose(istream);

            oasis_send_packet(fd, NULL, 0, CLOSE);
            oasis_wait_for_ack(fd);
            printf("\nEnd of File\n");
        }
    }

    commBuffer[0] = DLE;
    commBuffer[1] = EOT;

    if (write_serial(fd, commBuffer, 2) != 2) {
        printf("Error writing to serial port.\n");
    }
    printf("End of Transmission\n");

    /* Close the serial port */
    close_serial(fd);

    return 0;
}

int parse_args(int argc, char *argv[], oasis_send_args_t *args) {
    int positional_arg_cnt = 0;

    memset(args, 0, sizeof(oasis_send_args_t));

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            switch (positional_arg_cnt) {
                case 0:
                    snprintf(args->port_path, sizeof(args->port_path), "%s", argv[i]);
                    break;
                case 1:
                    args->input_filename_index = i;
                    break;
            }
            positional_arg_cnt++;
        } else {
            char flag = argv[i][1];

            switch (flag) {
                case 'a':
                    args->ascii = 1;
                    break;
                case 'q':
                    args->quiet = 1;
                    break;
                default:
                    printf("Unknown option '-%c'\n", flag);
                    break;
            }
        }
    }
    return positional_arg_cnt;
}

#if !defined(S_ISREG) && defined(S_IFMT) && defined(S_IFREG)
# define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif /* if !defined(S_ISREG) && defined(S_IFMT) && defined(S_IFREG) */

int check_regular_file(const char *path) {
    struct stat path_stat;

    if (0 != stat(path, &path_stat)) {
        printf("stat() error.\n");
        return -1;
    }
    return S_ISREG(path_stat.st_mode);
}
