/*
 * Utility to Receive a file from an OASIS system via Serial Port.
 *
 * www.github.com/hharte/oasis-utils
 *
 * (c) 2021-2022, Howard M. Harte
 *
 * Reference: http://bitsavers.org/pdf/phaseOneSystems/oasis/Communications_Reference_Manual_Mar80.pdf pp. 8
 *
 * Tricky files for regression testing:
 * PATCH.COMMAND
 * DESPOOL.COMMAND
 * SYSTEM.ERRMSG
 *
 */

#define _CRT_SECURE_NO_DEPRECATE

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#include "oasis.h"
#include "oasis_utils.h"
#include "oasis_sendrecv.h"
#include "mm_serial.h"

typedef struct oasis_recv_args {
    char port_path[256];
    char output_path[256];
    int ascii;
    int quiet;
} oasis_recv_args_t;


int parse_args(int argc, char* argv[], oasis_recv_args_t* args);
int oasis_packet_decode(uint8_t* inbuf, uint16_t inlen, uint8_t* outbuf, uint16_t* outlen);
int oasis_open_file(directory_entry_block_t* dir_entry, FILE** ostream, char* path, int quiet);


int main(int argc, char *argv[])
{
    int fd;
    uint8_t commBuffer[1024];
    uint8_t decoded_buf[512];
    uint8_t cksum;
    ssize_t bytes_written;
    ssize_t bytes_read;
    uint16_t decoded_len;
    OASIS_PACKET_HEADER_T* pHdr;
    directory_entry_block_t DirEntry = { 0 };
    directory_entry_block_t* pDirEntry = &DirEntry;
    FILE* ostream = NULL;
    int file_len = 0;
    int num_segments = 0;
    int debug_segment = 999;
    int positional_arg_cnt;
    oasis_recv_args_t args;

    positional_arg_cnt = parse_args(argc, argv, &args);

    if (positional_arg_cnt == 0) {
        printf("OASIS Receive Utility [%s] (c) 2021 - Howard M. Harte\n", VERSION);
        printf("https://github.com/hharte/oasis-utils\n\n");

        printf("usage is: %s <port> [<filename>|<path>] [-q] [-a]\n", argv[0]);
        printf("\t<port> Serial Port filename.\n");
        printf("\tFlags:\n");
        printf("\t      -q       Quiet: Don't list file details during extraction.\n");
        printf("\t      -a       ASCII: Convert line endings and truncate output file at EOF.\n");
        return (-1);
    }

    if ((fd = open_serial(args.port_path)) < 0) {
        printf("Error opening %s.\n", args.port_path);
        return (-1);
    }

    if (init_serial(fd, 9600) != 0) {
        printf("Error initializing %s.\n", args.port_path);
        return (-1);
    }

    printf("Waiting for Sending Station");

    for (int retries = 100;; retries--) {
        bytes_read = read_serial(fd, commBuffer, 1);

        if (commBuffer[0] == ENQ) {
            printf("\nStart of file transfer\n");
            break;
        }
        else {
            if ((retries-1) % 10 == 0) {
                printf(".");
            }
        }

        if (retries == 0) {
            printf("\nTimeout waiting for sending station.\n");
            return (-2);
        }
    }

    int toggle = 0;
    int current_segment = 0;
    while (1) {
        toggle++;
        commBuffer[0] = DLE;
        commBuffer[1] = '0' + (toggle & 1);

        bytes_written = write_serial(fd, commBuffer, 2);

        bytes_read = read_serial(fd, commBuffer, sizeof(commBuffer));

        for (uint16_t i = 0; i < bytes_read; i++) {
            commBuffer[i] &= 0x7F;          /* Strip off MSB */
        }

//        printf("\nPacket %d, len=%d:\n", toggle, bytes_read);
        fflush(stdout);
        if (current_segment == debug_segment) dump_hex(commBuffer, (int)bytes_read);

        if (bytes_read > 3) {
            cksum = oasis_packet_decode(commBuffer, (uint16_t)bytes_read, decoded_buf, &decoded_len);

            if (cksum) {
                pHdr = (OASIS_PACKET_HEADER_T*)commBuffer;

                switch (pHdr->rectype) {
                case CLOSE: /* File information */
                    printf("\nEnd of File                                    \n");
//                    dump_hex(decoded_buf, decoded_len);
                    if (ostream != NULL) fclose(ostream);

                    /* Preserve original file date/time */
//                    oasis_convert_timestamp_to_tm(&pDirEntry->timestamp, &oasis_tm);
//                    set_file_time(output_filename, &oasis_tm);

                    break;
                case OPEN: /* File information */
//                    printf("Open file:\n");
//                    dump_hex(decoded_buf, decoded_len);
                    memcpy(pDirEntry, decoded_buf, sizeof(directory_entry_block_t));
//                    printf("Fname--- Ftype--  --Date-- Time- -Recs Blks Format- -Sect Own SOw Other-\n");
//                    oasis_list_dir_entry(pDirEntry);
                    file_len = oasis_open_file(pDirEntry, &ostream, ".", 0);
                    current_segment = 0;
                    num_segments = ((pDirEntry->record_count * pDirEntry->file_format_dependent1) / BLOCK_SIZE);
                    break;
                case WRITE: /* Data Packet */
                {
//                    printf("Write Data packet, len=%d:\n", decoded_len);
                    if (current_segment == debug_segment)  dump_hex(decoded_buf, decoded_len);

                    if ((pDirEntry->file_format & 0x1F) != FILE_FORMAT_SEQUENTIAL) {
                        current_segment++;
                        if (current_segment <= num_segments) {
                            printf("\rSegment: S %d", current_segment);
                            fwrite(decoded_buf, decoded_len, 1, ostream);
                        }
                        else {
                            printf("\rSegment %d (skipped.)", current_segment);
                        }
                    }
                    else {
                        uint16_t link;

                        char text_buf[BLOCK_SIZE * 2] = { 0 };
                        char* text_ptr = text_buf;
                        int text_len;
                        int i;

                        current_segment++;

                        link = decoded_buf[BLOCK_SIZE - 1] << 8;
                        link |= decoded_buf[BLOCK_SIZE - 2];
                        for (i = 0; i < BLOCK_SIZE - 2; i++) {
                            if (args.ascii && (decoded_buf[i] == SUB)) {
                                /* Text file EOF */
                                break;
                            }
                            if (args.ascii && (decoded_buf[i] == CR)) {
                                /* OASIS uses CR for line endings: convert CR to CR/LF (for Windows) or LF (for UNIX) */
#if defined(_WIN32)
                                *text_ptr++ = decoded_buf[i];
#endif
                                *text_ptr++ = LF;	/* Add LF */
                            }
                            else {
                                *text_ptr++ = decoded_buf[i];
                            }
                        }

                        text_len = (int)(text_ptr - text_buf);
                        printf("\rSegment: %d", current_segment);
                        fwrite(text_buf, text_len, 1, ostream);

                        if (current_segment >= pDirEntry->block_count * 4) {
                            printf("Corrupted link: %d >= %d", current_segment, pDirEntry->block_count * 4);
                            break;
                        }
                    }
                    break;
                }
                default:
                    printf("Unknown record type 0x%02x\n", pHdr->rectype);
                    break;
                }
            }
            else {
                printf("Error decoding packet, send NAK.\n");
                printf("Raw packet:\n");
                dump_hex(&commBuffer[0], (int)bytes_read);
            }
        }
        else {
//            dump_hex(&commBuffer[0], bytes_read);
        }

        if ((commBuffer[0] == DLE) && (commBuffer[1] == EOT)) {
            printf("End of Transmission\n");
            break;
        }
    }

    /* Close the serial port */
    close_serial(fd);

    return 0;
}


int parse_args(int argc, char* argv[], oasis_recv_args_t* args)
{
    int positional_arg_cnt = 0;

    memset(args, 0, sizeof(oasis_recv_args_t));

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            switch (positional_arg_cnt) {
            case 0:
                snprintf(args->port_path, sizeof(args->port_path), "%s", argv[i]);
                break;
            case 1:
                snprintf(args->output_path, sizeof(args->output_path), "%s", argv[i]);
                break;
            }
            positional_arg_cnt++;
        }
        else {
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

int oasis_open_file(directory_entry_block_t* dir_entry, FILE** ostream, char* path, int quiet)
{
    char oasis_fname[FNAME_LEN + 1];
    char fname[FNAME_LEN + 1];
    char oasis_ftype[FEXT_LEN + 1];
    char ftype[FEXT_LEN + 1];

    if (dir_entry->file_format == 0) {
        return (-ENOENT);
    }

    snprintf(oasis_fname, sizeof(oasis_fname), "%s", dir_entry->file_name);
    snprintf(oasis_ftype, sizeof(oasis_ftype), "%s", dir_entry->file_type);

    /* Truncate the filename if a space is encountered. */
    for (unsigned int j = 0; j < strnlen(oasis_fname, sizeof(oasis_fname)); j++) {
        if (oasis_fname[j] == ' ') oasis_fname[j] = '\0';
    }

    snprintf(fname, sizeof(fname), "%s", oasis_fname);

    /* Truncate the type if a space is encountered. */
    for (unsigned int j = 0; j < strnlen(oasis_ftype, sizeof(oasis_ftype)); j++) {
        if (oasis_ftype[j] == ' ') oasis_ftype[j] = '\0';
    }

    snprintf(ftype, sizeof(fname), "%s", oasis_ftype);

    int file_len = 0;
    char output_filename[256];

    switch (dir_entry->file_format & 0x1f) {
    case FILE_FORMAT_RELOCATABLE:
        file_len = dir_entry->file_format_dependent2;
        snprintf(output_filename, sizeof(output_filename), "%s%c%s.%s_R_%d", path, kPathSeparator, fname, ftype, dir_entry->file_format_dependent1);
        break;
    case FILE_FORMAT_ABSOLUTE:
        file_len = dir_entry->record_count * dir_entry->file_format_dependent1;
        snprintf(output_filename, sizeof(output_filename), "%s%c%s.%s_A_%d", path, kPathSeparator, fname, ftype, dir_entry->file_format_dependent2);
        break;
    case FILE_FORMAT_SEQUENTIAL:
        file_len = (dir_entry->file_format_dependent2 + 1 - dir_entry->start_sector) * BLOCK_SIZE;
        snprintf(output_filename, sizeof(output_filename), "%s%c%s.%s_S_%d", path, kPathSeparator, fname, ftype, dir_entry->file_format_dependent1);
        break;
    case FILE_FORMAT_DIRECT:
        file_len = dir_entry->block_count * 1024;
        snprintf(output_filename, sizeof(output_filename), "%s%c%s.%s_D_%d", path, kPathSeparator, fname, ftype, dir_entry->file_format_dependent1);
        break;
    case FILE_FORMAT_INDEXED:
        printf("Skipping INDEXED file: %s.%s\n", oasis_fname, oasis_ftype);
        break;
    case FILE_FORMAT_KEYED:
        printf("Skipping KEYED file: %s.%s\n", oasis_fname, oasis_ftype);
        break;
    default:
        break;
    }

    output_filename[sizeof(output_filename) - 1] = '\0';
    if (!(*ostream = fopen(output_filename, "wb"))) {
        printf("Error Openening %s\n", output_filename);
        return (-ENOENT);
    }

    if (!quiet) printf("Receiving \"%s.%s\" -> %s (%d bytes)\n", oasis_fname, oasis_ftype, output_filename, file_len);
    return (file_len);
}
