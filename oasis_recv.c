/*
 * Utility to Receive a file from an OASIS system via Serial Port.
 *
 * www.github.com/hharte/oasis-utils
 *
 * (c) 2021, Howard M. Harte
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

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>

#include "oasis.h"
#include "oasis_utils.h"
#include "oasis_sendrecv.h"

#define COMM_PORT   "\\\\.\\COM12"   /* COM12 */

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
    HANDLE hComm;
    DCB myDCB;
    COMMTIMEOUTS cto;
    uint8_t commBuffer[1024];
    uint8_t decoded_buf[512];
    uint8_t cksum;
    BOOL Status;
    uint32_t bytes_written;
    uint32_t bytes_read;
    uint16_t decoded_len;
    OASIS_PACKET_HEADER_T* pHdr;
    directory_entry_block_t DirEntry = { 0 };
    directory_entry_block_t* pDirEntry = &DirEntry;
    FILE* ostream;
    struct tm oasis_tm;
    int file_len = 0;
    int num_segments;
    int debug_segment = 999;
    int positional_arg_cnt;
    oasis_recv_args_t args;

    positional_arg_cnt = parse_args(argc, argv, &args);

    if (positional_arg_cnt == 0) {
        printf("OASIS Receive Utility (c) 2021 - Howard M. Harte\n");
        printf("https://github.com/hharte/oasis-utils\n\n");

        printf("usage is: %s <port> [<filename>|<path>] [-q] [-a]\n", argv[0]);
        printf("\t<port> Serial Port filename.\n");
        printf("\tFlags:\n");
        printf("\t      -q       Quiet: Don't list file details during extraction.\n");
        printf("\t      -a       ASCII: Convert line endings and truncate output file at EOF.\n");
        return (-1);
    }

    hComm = CreateFileA(args.port_path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);

    if (hComm == INVALID_HANDLE_VALUE) {
        printf("Error opening %s.\n", COMM_PORT);
    }
    else {
        printf("Opened %s successfully.\n", COMM_PORT);
    }

    GetCommState(hComm, &myDCB);

    myDCB.BaudRate = 9600;
    SetCommState(hComm, &myDCB);

    GetCommTimeouts(hComm, &cto);
    // Set the new timeouts
    cto.ReadIntervalTimeout = 100;
    cto.ReadTotalTimeoutConstant = 100;
    cto.ReadTotalTimeoutMultiplier = 100;
    SetCommTimeouts(hComm, &cto);

    printf("Waiting for Sending Station\n\n");

    while (1) {
        Status = ReadFile(hComm, commBuffer, 1, &bytes_read, NULL);

        if (commBuffer[0] == ENQ) {
            printf("Start of file transfer\n");
            break;
        }
        else {
//            printf("Retrying...\n");
        }
    }

    int toggle = 0;
    int current_segment = 0;
    while (1) {
        toggle++;
        commBuffer[0] = DLE;
        commBuffer[1] = '0' + (toggle & 1);

        Status = WriteFile(hComm, commBuffer, 2, &bytes_written, NULL);

        Status = ReadFile(hComm, commBuffer, sizeof(commBuffer), &bytes_read, NULL);

        for (uint16_t i = 0; i < bytes_read; i++) {
            commBuffer[i] &= 0x7F;          /* Strip off MSB */
        }

//        printf("\nPacket %d, len=%d:\n", toggle, bytes_read);
        fflush(stdout);
        if (current_segment == debug_segment) dump_hex(commBuffer, bytes_read);

        if (bytes_read > 3) {
            cksum = oasis_packet_decode(commBuffer, (uint16_t)bytes_read, decoded_buf, &decoded_len);

            if (cksum) {
                pHdr = (OASIS_PACKET_HEADER_T*)commBuffer;

                switch (pHdr->rectype) {
                case CLOSE: /* File information */
                    printf("\nEnd of File                                    \n");
//                    dump_hex(decoded_buf, decoded_len);
                    fclose(ostream);

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
                            printf("\rSegment: %d", current_segment);
                            fwrite(decoded_buf, decoded_len, 1, ostream);
                        }
                        else {
                            printf("\rSegment %d (skipped.)", current_segment);
                        }
                    }
                    else {
                        uint16_t link;

                        char text_buf[BLOCK_SIZE * 2];
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
                            printf("Corrupted link: ");
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
                dump_hex(&commBuffer[0], bytes_read);
            }
        }
        else {
//            dump_hex(&commBuffer[0], bytes_read);
        }

        if ((commBuffer[0] == DLE) && (commBuffer[1] == EOT)) {
            printf("\nEnd of Transmission\n");
            break;
        }
    }

    /* Close the serial port */
    CloseHandle(hComm);

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


int oasis_packet_decode(uint8_t* inbuf, uint16_t inlen, uint8_t* outbuf, uint16_t* outlen)
{
    uint8_t shift = 0;
    uint16_t j = 0;
    uint8_t src_data;
    uint8_t dle_arg;
    uint8_t last_processed_char;
    uint8_t cksum = 0;

    j = 0;
    for (uint16_t i = 3; i < inlen; i++)
    {
        src_data = inbuf[i];
        /* Process DLE (Data Link Escape) */
        if (src_data == DLE) {
            i++;
            dle_arg = inbuf[i];
            switch (dle_arg) {
            case SI:    /* Shift in (add 0x80 to the following data) */
                shift = 0x80;
                break;
            case SO:    /* Shift out (don't add 0x80 to the following data) */
                shift = 0x00;
                break;
            case DLE:   /* DLE+DLE emits DLE (0x10) into the output buffer. */
                last_processed_char = DLE | shift;
                outbuf[j] = last_processed_char;
                j++;
                break;
            case VT:    /* VT: repeat last_processed_char tablen times. */
            {
                int tablen = inbuf[++i];
                if (tablen == DLE) {
                    switch (inbuf[++i]) {
                    case DLE:
                        tablen = DLE;
                        break;
                    case CAN:
                        tablen = ESC;
                        break;
                    default:
                        printf("\n\nERROR: Unknown Tablen 0x%x after DLE.\n", inbuf[i-1]);
                        break;
                    }
                }
                for (int k = 0; k < tablen; k++) {
                    outbuf[j] = last_processed_char;
                    j++;
                }
                break;
            }
            case ETX:
                //                printf("<ETX> at %d\n", i);
                cksum = oasis_lrcc(inbuf, i + 1);
                if (cksum != inbuf[i + 1]) {
                    printf("Bad Checksum!\n");
                    return 0;
                    *outlen = 0;
                }
                else {
                    *outlen = j;
                    return cksum;
                }
                break;
            case CAN:
                last_processed_char = ESC | shift;
                outbuf[j] = last_processed_char;
                j++;
                break;
            default:
                printf("At %d: Unknown DLE=0x%2x\n", i, inbuf[i]);
                break;
            }
        }
        else {    /* Process normal character */
            last_processed_char = inbuf[i] + shift;
            outbuf[j] = last_processed_char;
            j++;
        }
    }

    printf("Decoded %d input bytes to %d output bytes, without reaching <ETX>.\n", inlen, j);
    *outlen = 0;

    return 0;
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
