/*
 * Utility to Send a file to an OASIS system via Serial Port.
 *
 * www.github.com/hharte/oasis-utils
 *
 * (c) 2021, Howard M. Harte
 *
 * Reference: http://bitsavers.org/pdf/phaseOneSystems/oasis/Communications_Reference_Manual_Mar80.pdf pp. 8
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

#define COMM_PORT   "\\\\.\\COM12" /* COM12 */
typedef struct oasis_send_args {
    char port_path[256];
    char input_filename[256];
    int ascii;
    int quiet;
} oasis_send_args_t;

int parse_args(int argc, char* argv[], oasis_send_args_t* args);
int oasis_send_packet(HANDLE hComm, uint8_t* buf, uint16_t len, uint8_t cmd);
int oasis_wait_for_ack(HANDLE hComm);
int oasis_packet_encode(uint8_t* inbuf, uint16_t inlen, uint8_t* outbuf, uint16_t* outlen);
int oasis_open_file(directory_entry_block_t* dir_entry, FILE** ostream, char* path, int quiet);


int main(int argc, char *argv[])
{
    HANDLE hComm;
    DCB myDCB;
    COMMTIMEOUTS cto;
    uint8_t commBuffer[1024];
    uint8_t decoded_buf[512];
    BOOL Status;
    uint32_t bytes_written;
    uint32_t bytes_read;
    directory_entry_block_t DirEntry = { 0 };
    directory_entry_block_t* pDirEntry = &DirEntry;
    FILE* istream;
    struct tm oasis_tm;
    int file_len = 0;
    int toggle = 0;
    int positional_arg_cnt;
    oasis_send_args_t args;
    char fname_str[256];
    char ftype_str[256];
    char ffmt;
    uint8_t* sequential_buf;
    int send_len;
    uint16_t frec_len;
    WIN32_FIND_DATA ffd;
    HANDLE hFind;

    positional_arg_cnt = parse_args(argc, argv, &args);

    if (positional_arg_cnt == 0) {
        printf("OASIS Send Utility (c) 2021 - Howard M. Harte\n");
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
    cto.ReadTotalTimeoutConstant = 0;
    cto.ReadTotalTimeoutMultiplier = 0;
    SetCommTimeouts(hComm, &cto);

    hFind = FindFirstFileA(args.input_filename, &ffd);

    if (INVALID_HANDLE_VALUE == hFind)
    {
        printf("File not found.\n");
        exit(-1);
    }

    do
    {
        if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            printf("  %s\n", ffd.cFileName);

    istream = fopen(ffd.cFileName, "rb");
    send_len = 0;

    ffd.cFileName[sizeof(ffd.cFileName) - 1] = '\0';
    for (int i = 0; i < strlen(ffd.cFileName); i++) {
        if (ffd.cFileName[i] == '.' || ffd.cFileName[i] == '_') {
            ffd.cFileName[i] = ' ';
        }
    }

    sscanf(ffd.cFileName, "%8s %8s %c %hu", fname_str, ftype_str, &ffmt, &frec_len);
    strcat(fname_str, "        ");
    strcat(ftype_str, "        ");
    memcpy(pDirEntry->file_name, fname_str, 8);
    memcpy(pDirEntry->file_type, ftype_str, 8);

    switch (ffmt) {
    case 'D':
    case 'd':
        pDirEntry->file_format = FILE_FORMAT_DIRECT;

        fseek(istream, 0, SEEK_END);
        file_len = ftell(istream);
        fseek(istream, 0, SEEK_SET);

        pDirEntry->record_count = (file_len + (frec_len - 1)) / frec_len;
        pDirEntry->block_count = (file_len + 1023) / 1024;
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

        sequential_buf = calloc(pDirEntry->block_count * 1024, sizeof(uint8_t));
        memset(sequential_buf, SUB, sizeof(uint8_t) * pDirEntry->block_count * 1024);

        int line_count = 0;
        int max_len = 0;
        int cur_len = 0;
        int prev_cr = 0;
        while (!feof(istream)) {
            int c;
            c = fgetc(istream);
            if (args.ascii && prev_cr && c == LF) {
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

        pDirEntry->record_count = line_count;
        pDirEntry->file_format_dependent1 = max_len;
        pDirEntry->file_format_dependent2 = 0;
        break;
    }

    oasis_tm.tm_year = 91;
    oasis_tm.tm_mon = 11;
    oasis_tm.tm_mday = 02;
    oasis_tm.tm_hour = 04;
    oasis_tm.tm_min = 33;

    oasis_convert_tm_to_timestamp(&oasis_tm, &pDirEntry->timestamp);

    printf("Fname--- Ftype--  --Date-- Time- -Recs Blks Format- -Sect Own SOw Other-\n");
    oasis_list_dir_entry(pDirEntry);

    printf("Waiting for Receiving Station \n");

    commBuffer[0] = ENQ;
    Status = WriteFile(hComm, commBuffer, 1, &bytes_written, NULL);
    oasis_wait_for_ack(hComm);

    commBuffer[0] = ENQ;
    Status = WriteFile(hComm, commBuffer, 1, &bytes_written, NULL);
    oasis_wait_for_ack(hComm);

    printf("Start of file transfer\n");

    oasis_send_packet(hComm, (uint8_t *)pDirEntry, sizeof(directory_entry_block_t), OPEN);
    oasis_wait_for_ack(hComm);

    if (pDirEntry->file_format != FILE_FORMAT_SEQUENTIAL) {
        while (bytes_read = (uint32_t)fread(&decoded_buf, 1, BLOCK_SIZE, istream))
        {
            oasis_send_packet(hComm, decoded_buf, bytes_read, WRITE);
            oasis_wait_for_ack(hComm);
        }
    } else { /* Sequential File */
        uint16_t sector_cnt = 1;
        for (int bytes_sent = 0; bytes_sent <= send_len; bytes_sent += BLOCK_SIZE-2)
        {
            printf("\rSegment: %d", sector_cnt);
            memcpy(decoded_buf, &sequential_buf[bytes_sent], BLOCK_SIZE - 2);
            decoded_buf[BLOCK_SIZE - 2] = sector_cnt >> 8;
            decoded_buf[BLOCK_SIZE - 1] = sector_cnt && 0xFF;
            oasis_send_packet(hComm, decoded_buf, BLOCK_SIZE, WRITE);
            oasis_wait_for_ack(hComm);
            sector_cnt++;
        }
        free(sequential_buf);
    }

    fclose(istream);

    oasis_send_packet(hComm, NULL, 0, CLOSE);
    oasis_wait_for_ack(hComm);
    printf("\nEnd of File\n");

            }
    } while (FindNextFile(hFind, &ffd) != 0);


    commBuffer[0] = DLE;
    commBuffer[1] = EOT;
    Status = WriteFile(hComm, commBuffer, 2, &bytes_written, NULL);
    printf("End of Transmission\n");

    /* Close the serial port */
    CloseHandle(hComm);

    return 0;
}


int parse_args(int argc, char* argv[], oasis_send_args_t* args)
{
    int positional_arg_cnt = 0;

    memset(args, 0, sizeof(oasis_send_args_t));

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            switch (positional_arg_cnt) {
            case 0:
                snprintf(args->port_path, sizeof(args->port_path), "%s", argv[i]);
                break;
            case 1:
                snprintf(args->input_filename, sizeof(args->input_filename), "%s", argv[i]);
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


int oasis_send_packet(HANDLE hComm, uint8_t* buf, uint16_t len, uint8_t cmd)
{
    uint8_t commBuffer[1024];
    uint8_t decoded_buf[512];
    uint8_t cksum;
    BOOL Status;
    DWORD bytes_written;
    uint16_t encoded_len;

//    printf("Sending %d bytes, cmd='%c'\n", len, cmd);

    decoded_buf[0] = DLE;
    decoded_buf[1] = STX;
    decoded_buf[2] = cmd;
    if ((len > 0) && (buf != NULL)) {
        memcpy_s(&decoded_buf[3], sizeof(decoded_buf) - 3, buf, len);
    }

//    dump_hex(decoded_buf, len + 3);

    cksum = oasis_packet_encode(decoded_buf, len + 3, commBuffer, &encoded_len);
//    printf("LRCC: 0x%02x\n", cksum);
//    dump_hex(commBuffer, encoded_len);

    for (uint16_t i = 0; i < len; i++) {
        commBuffer[i] |= 0x80;
    }

    Status = WriteFile(hComm, commBuffer, encoded_len, &bytes_written, NULL);

    return 0;
}

int oasis_wait_for_ack(HANDLE hComm)
{
    BOOL Status;
    DWORD dNoOfBytesRead;
    uint8_t buf[2];
    int8_t toggle = -1;

    for (int retry = 0; retry < 5; retry++) {
        Status = ReadFile(hComm, buf, 2, &dNoOfBytesRead, NULL);

        if ((buf[0] != DLE) && ((buf[1] != '0') && (buf[1] != '1'))) {
            printf("Retrying...\n");
        }
        else {
            toggle = buf[1] & 1;
//            printf("Got ACK, toggle=%d", toggle);
            break;
        }
    }

    return (toggle);
}

int oasis_packet_encode(uint8_t* inbuf, uint16_t inlen, uint8_t* outbuf, uint16_t* outlen)
{
    uint8_t shift = 0;
    uint16_t j = 0;
    uint8_t cksum = 0;

    outbuf[0] = inbuf[0];
    outbuf[1] = inbuf[1];
    outbuf[2] = inbuf[2];
    j = 3;

    for (uint16_t i = 3; i < inlen; i++)
    {
        uint8_t src_data = inbuf[i];

        if ((src_data & 0x80) == shift) {
            outbuf[j] = src_data;
            if (shift) {
                outbuf[j] -= shift;
            }
            j++;
        }
        else {
            outbuf[j++] = DLE;
            if (shift) {
                outbuf[j++] = SO;
                shift = 0;
                outbuf[j++] = src_data;
            }
            else {
                outbuf[j++] = SI;
                shift = 0x80;
                outbuf[j++] = src_data - shift;
            }
        }
        /* Send DLE as DLE,DKE */
        if (outbuf[j - 1] == DLE) {
            outbuf[j++] = DLE;
        } else if (outbuf[j - 1] == ESC) { /* Handle ESC as DLE,CAN */
            outbuf[j - 1] = DLE;
            outbuf[j++] = CAN;
        }

        /* Four or more same bytes, compress. */
        if ((inbuf[i] == inbuf[i + 1]) && (src_data == inbuf[i + 2]) && (src_data == inbuf[i + 3])) {
            uint8_t run_length = 0;
            while ((i < (inlen - 1) && (inbuf[i] == inbuf[i + 1]))) {
                i++;
                run_length++;
            }
            while (run_length > 0) {
                if (run_length > 126) {
                    outbuf[j++] = DLE;
                    outbuf[j++] = VT;
                    outbuf[j++] = 126;
                    run_length -= 126;
                }
                else if (run_length > 3) {
                    outbuf[j++] = DLE;
                    outbuf[j++] = VT;
                    switch (run_length) {
                        case DLE: /* Encode 0x10 as DLE,DLE */
                            outbuf[j++] = DLE;
                            outbuf[j++] = DLE;
                            break;
                        case ESC: /* Encode 0x1B as DLE,CAN */
                            outbuf[j++] = DLE;
                            outbuf[j++] = CAN;
                            break;
                        default:
                            outbuf[j++] = run_length;
                            break;
                    }
                    run_length = 0;
                }
                else {
                    while (run_length > 0) {
                        outbuf[j++] = src_data;
                        run_length--;
                    }
                }
            }
        }
    }

    outbuf[j++] = DLE;
    outbuf[j++] = ETX;

    cksum = oasis_lrcc(&outbuf[0], j);
    outbuf[j++] = cksum;
    outbuf[j++] = RUB;
    *outlen = j;

    return cksum;
}
