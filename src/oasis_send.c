/*
 * Utility to Send a file to an OASIS system via Serial Port.
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#ifdef _MSC_VER
#include <io.h>      /* For _access */
#endif

/* Standard C Headers */
#include <stdio.h>
#include <stdlib.h> /* For strtol, EXIT_SUCCESS, EXIT_FAILURE, calloc, free, malloc */
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>    /* For toupper */
#include <sys/stat.h> /* For stat */
#include <time.h>     /* For time, localtime (for timestamping DEB) */
#include <limits.h>   /* For INT_MAX, LONG_MIN, LONG_MAX */

/* Local Headers */
#include "oasis.h"
#include "mm_serial.h"
#include "oasis_transfer_utils.h"

/* Structure to hold command line arguments specific to oasis_send */
typedef struct oasis_send_specific_args {
    int     user_id;              /* User ID for the DEB */
    int     input_filename_start_index; /* Index in argv where filenames start */
    /* Other send-specific fields can be added here if necessary */
} oasis_send_specific_args_t;

/* Combined arguments structure */
typedef struct oasis_send_args {
    oasis_transfer_common_args_t common;
    oasis_send_specific_args_t send_specific;
} oasis_send_args_t;


/* --- Helper Functions (specific to send, if any, or keep static) --- */
static int check_regular_file(const char* path) {
    struct stat path_stat;
    int saved_errno;

    if (stat(path, &path_stat) != 0) {
        saved_errno = errno;
        perror(path);
        return -saved_errno;
    }

#if defined(S_ISREG)
    if (!S_ISREG(path_stat.st_mode)) {
        fprintf(stderr, "Error: '%s' is not a regular file.\n", path);
        return 0;
    }
#elif defined(_WIN32) && defined(_S_IFMT) && defined(_S_IFREG)
    if (!((path_stat.st_mode & _S_IFMT) == _S_IFREG)) {
        fprintf(stderr, "Error: '%s' is not a regular file (checked using _S_IFREG).\n", path);
        return 0;
    }
#else
    fprintf(stderr, "Warning: S_ISREG macro not available, file type check for '%s' may be limited.\n", path);
    /* Add more specific checks if possible for other platforms if S_ISREG is missing */
#endif

#if defined(_WIN32) || defined(_WIN64)
    if (_access(path, 04) != 0) {
        saved_errno = errno;
        fprintf(stderr, "Error: Cannot read file '%s': %s\n", path, strerror(saved_errno));
        return -EACCES;
    }
#else
    if (access(path, R_OK) != 0) {
        saved_errno = errno;
        fprintf(stderr, "Error: Cannot read file '%s': %s\n", path, strerror(saved_errno));
        return (saved_errno == EACCES) ? -EACCES : -saved_errno;
    }
#endif
    return 1;
}

static const char* get_basename_const(const char* path_str) {
    const char* basename_ptr = path_str;
    const char* last_separator = NULL;
    const char* last_fwd_slash = strrchr(path_str, '/');
    const char* last_back_slash = strrchr(path_str, '\\');

    if (last_fwd_slash && last_back_slash) {
        last_separator = (last_fwd_slash > last_back_slash) ? last_fwd_slash : last_back_slash;
    }
    else if (last_fwd_slash) {
        last_separator = last_fwd_slash;
    }
    else {
        last_separator = last_back_slash;
    }

    if (last_separator != NULL) {
        basename_ptr = last_separator + 1;
    }
    return basename_ptr;
}

/* --- Forward Declarations --- */
static int parse_send_args(int argc, char* argv[], oasis_send_args_t* args);
static void print_send_usage(const char* prog_name);

/* --- Main Function --- */
int main(int argc, char* argv[]) {
    uint8_t     comm_buffer[1024] = { 0 };    /* Buffer for serial comms. */
    uint8_t     file_data_buf[XFR_BLOCK_SIZE]; /* Buffer for file data blocks. */
    directory_entry_block_t dir_entry = { 0 }; /* OASIS directory entry. */
    FILE* istream = NULL;               /* Input file stream. */
    size_t      host_file_len_inner = 0;    /* Length of the current host input file. */
    oasis_send_args_t args;                 /* Parsed arguments. */
    int         ret_val_main;                   /* Generic return code. */
    int         i_argv;                         /* Loop counter for argv. */
    int         files_processed_count = 0;      /* Count of host files processed. */
    int         files_transferred = 0;          /* Number of files actually transferred. */
    int         current_ack_toggle = 0;         /* ACK toggle state (0 or 1). */
    const int   MAX_SEND_RETRIES = 5;         /* Max times to retry sending a packet. */
    int         overall_exit_code = EXIT_SUCCESS; /* Assume success. */
    oasis_transfer_session_t transfer_session;

    memset(&transfer_session, 0, sizeof(transfer_session));

    if (parse_send_args(argc, argv, &args) <= 0) {
        /* parse_send_args prints usage or error */
        return EXIT_FAILURE;
    }

    if (initialize_transfer_session(&args.common, &transfer_session) != 0) {
        return EXIT_FAILURE;
    }

    for (i_argv = args.send_specific.input_filename_start_index; i_argv < argc; i_argv++) {
        char* current_arg_pattern = argv[i_argv];
        char** files_to_process_list = NULL;
        int num_files_in_list = 0;
        int glob_status = 1;

#if defined(_WIN32) || defined(_WIN64)
        if (strchr(current_arg_pattern, '*') != NULL || strchr(current_arg_pattern, '?') != NULL) {
            if (args.common.debug) {
                fprintf(stderr, "DEBUG: Windows: Detected wildcard pattern: '%s'. Attempting glob.\n", current_arg_pattern);
            }
            glob_status = platform_glob_win32(current_arg_pattern, &files_to_process_list, &num_files_in_list);
            if (glob_status == -1) {
                fprintf(stderr, "Error: Failed to process wildcard pattern '%s' on Windows.\n", current_arg_pattern);
                overall_exit_code = EXIT_FAILURE; continue;
            }
            else if (glob_status == 1) {
                if (!args.common.quiet) {
                    fprintf(stdout, "Info: No files matched pattern '%s' on Windows.\n", current_arg_pattern);
                }
                platform_free_glob_result(files_to_process_list, num_files_in_list);
                files_to_process_list = NULL; continue;
            }
        }
        else {
            files_to_process_list = (char**)malloc(sizeof(char*));
            if (files_to_process_list) {
#if defined(_MSC_VER)
                files_to_process_list[0] = _strdup(current_arg_pattern);
#else
                files_to_process_list[0] = strdup(current_arg_pattern);
#endif
                if (files_to_process_list[0]) { num_files_in_list = 1; glob_status = 0; }
                else { perror("Error: strdup failed"); free(files_to_process_list); files_to_process_list = NULL; num_files_in_list = 0; overall_exit_code = EXIT_FAILURE; glob_status = -1; }
            }
            else { perror("Error: malloc failed"); num_files_in_list = 0; overall_exit_code = EXIT_FAILURE; glob_status = -1; }
        }
#else
        files_to_process_list = (char**)malloc(sizeof(char*));
        if (files_to_process_list) {
            files_to_process_list[0] = strdup(current_arg_pattern);
            if (files_to_process_list[0]) { num_files_in_list = 1; glob_status = 0; }
            else { perror("Error: strdup failed"); free(files_to_process_list); files_to_process_list = NULL; num_files_in_list = 0; overall_exit_code = EXIT_FAILURE; glob_status = -1; }
        }
        else { perror("Error: malloc failed"); num_files_in_list = 0; overall_exit_code = EXIT_FAILURE; glob_status = -1; }
#endif
        if (glob_status == -1) {
            if (files_to_process_list) {
#if defined(_WIN32) || defined(_WIN64)
                platform_free_glob_result(files_to_process_list, num_files_in_list);
#else
                if (num_files_in_list > 0 && files_to_process_list[0]) free(files_to_process_list[0]);
                free(files_to_process_list);
#endif
                files_to_process_list = NULL;
            }
            continue;
        }

        for (int k_file_idx = 0; k_file_idx < num_files_in_list; k_file_idx++) {
            char host_filename_to_send[FILENAME_MAX];
            const char* host_basename_ptr_const;
            uint8_t* raw_file_buffer_inner = NULL;
            uint8_t* data_to_send_buffer_inner = NULL;
            size_t final_send_len_inner = 0;
            int actual_max_line_len_for_deb_inner = 0;
            struct stat host_file_stat_inner;
            int current_send_retry_count_inner = 0;
            struct tm* local_time_for_ts_inner;
            int segment_count = 0;
            size_t bytes_sent_for_file = 0;
            bool parsed_by_host_fn_to_deb;

            files_processed_count++;
            strncpy(host_filename_to_send, files_to_process_list[k_file_idx], sizeof(host_filename_to_send) - 1);
            host_filename_to_send[sizeof(host_filename_to_send) - 1] = '\0';

            ret_val_main = check_regular_file(host_filename_to_send);
            if (ret_val_main <= 0) { fprintf(stderr, "Warning: Skipping '%s'.\n", host_filename_to_send); overall_exit_code = EXIT_FAILURE; continue; }

            if (stat(host_filename_to_send, &host_file_stat_inner) != 0) { perror(host_filename_to_send); overall_exit_code = EXIT_FAILURE; continue; }
            host_file_len_inner = (size_t)host_file_stat_inner.st_size;

            istream = fopen(host_filename_to_send, "rb");
            if (istream == NULL) { fprintf(stderr, "Error: Cannot open input file '%s': %s\n", host_filename_to_send, strerror(errno)); overall_exit_code = EXIT_FAILURE; continue; }

            if (host_file_len_inner > 0) {
                raw_file_buffer_inner = (uint8_t*)malloc(host_file_len_inner);
                if (raw_file_buffer_inner == NULL) { fprintf(stderr, "Error: Malloc failed for raw file buffer ('%s').\n", host_filename_to_send); fclose(istream); overall_exit_code = EXIT_FAILURE; continue; }
                if (fread(raw_file_buffer_inner, 1, host_file_len_inner, istream) != host_file_len_inner) { fprintf(stderr, "Error: Failed to read file '%s'.\n", host_filename_to_send); fclose(istream); free(raw_file_buffer_inner); overall_exit_code = EXIT_FAILURE; continue; }
            }
            fclose(istream); istream = NULL;

            host_basename_ptr_const = get_basename_const(host_filename_to_send);
            memset(&dir_entry, 0, sizeof(dir_entry));
            parsed_by_host_fn_to_deb = host_filename_to_oasis_deb(host_basename_ptr_const, &dir_entry);
            if (!parsed_by_host_fn_to_deb) { fprintf(stderr, "Error: Could not parse OASIS filename metadata from '%s'. Skipping.\n", host_basename_ptr_const); if (raw_file_buffer_inner) free(raw_file_buffer_inner); overall_exit_code = EXIT_FAILURE; continue; }

            dir_entry.owner_id = (uint8_t)args.send_specific.user_id;
            local_time_for_ts_inner = localtime(&host_file_stat_inner.st_mtime);
            if (local_time_for_ts_inner) { oasis_convert_tm_to_timestamp(local_time_for_ts_inner, &dir_entry.timestamp); }
            else { time_t ct = time(NULL); local_time_for_ts_inner = localtime(&ct); if (local_time_for_ts_inner) oasis_convert_tm_to_timestamp(local_time_for_ts_inner, &dir_entry.timestamp); }

            if (args.common.ascii_conversion && ((dir_entry.file_format & FILE_FORMAT_MASK) == FILE_FORMAT_SEQUENTIAL) && host_file_len_inner > 0 && raw_file_buffer_inner && is_ascii(raw_file_buffer_inner, host_file_len_inner)) {
                if (args.common.debug) fprintf(stderr, "DEBUG: Performing ASCII conversion for '%s'.\n", host_filename_to_send);
                size_t converted_capacity = host_file_len_inner + 1;
                data_to_send_buffer_inner = (uint8_t*)malloc(converted_capacity);
                if (!data_to_send_buffer_inner) { fprintf(stderr, "Error: Malloc failed for ASCII conversion buffer ('%s').\n", host_filename_to_send); if (raw_file_buffer_inner) free(raw_file_buffer_inner); overall_exit_code = EXIT_FAILURE; continue; }
                conversion_result_t conv_stats;
                int32_t converted_len = ascii_host_to_oasis(raw_file_buffer_inner, host_file_len_inner, data_to_send_buffer_inner, converted_capacity, &conv_stats);
                if (converted_len < 0) { fprintf(stderr, "Error: ASCII conversion failed for '%s' (Code %d).\n", host_filename_to_send, converted_len); if (raw_file_buffer_inner) free(raw_file_buffer_inner); free(data_to_send_buffer_inner); overall_exit_code = EXIT_FAILURE; continue; }
                final_send_len_inner = (size_t)converted_len;
                actual_max_line_len_for_deb_inner = (int)conv_stats.max_line_len;
                if (raw_file_buffer_inner) { free(raw_file_buffer_inner); raw_file_buffer_inner = NULL; }
            }
            else {
                if (args.common.ascii_conversion && host_file_len_inner > 0 && raw_file_buffer_inner && !is_ascii(raw_file_buffer_inner, host_file_len_inner) && !args.common.quiet) {
                    fprintf(stdout, "Info: File '%s' not 7-bit ASCII, sending binary.\n", host_filename_to_send);
                }
                data_to_send_buffer_inner = raw_file_buffer_inner; raw_file_buffer_inner = NULL;
                final_send_len_inner = host_file_len_inner;
                if ((dir_entry.file_format & FILE_FORMAT_MASK) == FILE_FORMAT_SEQUENTIAL && dir_entry.file_format_dependent1 == 0 && final_send_len_inner > 0 && data_to_send_buffer_inner) {
                    int cll = 0; for (size_t k = 0; k < final_send_len_inner; ++k) { cll++; if (data_to_send_buffer_inner[k] == '\r') { if (cll - 1 > actual_max_line_len_for_deb_inner) actual_max_line_len_for_deb_inner = cll - 1; cll = 0; } } if (cll > actual_max_line_len_for_deb_inner) actual_max_line_len_for_deb_inner = cll;
                }
            }

            if ((dir_entry.file_format & FILE_FORMAT_MASK) == FILE_FORMAT_SEQUENTIAL && dir_entry.file_format_dependent1 == 0) {
                if (actual_max_line_len_for_deb_inner > 0 && actual_max_line_len_for_deb_inner <= UINT16_MAX) dir_entry.file_format_dependent1 = (uint16_t)actual_max_line_len_for_deb_inner;
                else if (final_send_len_inner > 0) dir_entry.file_format_dependent1 = DEFAULT_RECORD_LENGTH; else dir_entry.file_format_dependent1 = 0;
            }
            dir_entry.block_count = (uint16_t)((final_send_len_inner + (BLOCK_SIZE - 1)) / BLOCK_SIZE);
            if (final_send_len_inner == 0) dir_entry.block_count = 0;
            if ((dir_entry.file_format & FILE_FORMAT_MASK) != FILE_FORMAT_SEQUENTIAL) {
                if (dir_entry.file_format_dependent1 > 0 && final_send_len_inner > 0) { size_t cr = (final_send_len_inner + (dir_entry.file_format_dependent1 - 1)) / dir_entry.file_format_dependent1; dir_entry.record_count = (cr > UINT16_MAX) ? UINT16_MAX : (uint16_t)cr; }
                else if (final_send_len_inner == 0) dir_entry.record_count = 0;
            }

            if (!args.common.quiet) { printf("Sending: %s (User ID: %d)\n", host_filename_to_send, dir_entry.owner_id); list_single_deb(&dir_entry); }
            else if (args.common.debug) { char thfn[MAX_HOST_FILENAME_LEN]; oasis_deb_to_host_filename(&dir_entry, thfn, sizeof(thfn)); fprintf(stderr, "DEBUG: Sending %s as %s (User ID: %d)\n", host_filename_to_send, thfn, dir_entry.owner_id); }

            printf("Waiting for Receiving Station for %s", host_filename_to_send); fflush(stdout);
            ret_val_main = ACK_TIMEOUT; current_ack_toggle = 0; int enq_retries = 20;
            do {
                comm_buffer[0] = ENQ;
                if (args.common.debug) fprintf(stderr, "DEBUG: -> Sending ENQ\n");
                if (write_serial(transfer_session.serial_fd, comm_buffer, 1) == 1) oasis_pcap_add_record(transfer_session.pcap_stream, OASIS_PCAP_TX, comm_buffer, 1); else sleep_ms_util(1000);
                sleep_ms_util(args.common.pacing_packet_ms); enq_retries--; printf("."); fflush(stdout);
                if (enq_retries == 0) { ret_val_main = ACK_TIMEOUT; break; }
                ret_val_main = oasis_receive_ack(transfer_session.serial_fd, current_ack_toggle, transfer_session.pcap_stream);
            } while (ret_val_main != ACK_OK && ret_val_main != ACK_WRONG_TOGGLE && enq_retries > 0);

            if (ret_val_main != ACK_OK && ret_val_main != ACK_WRONG_TOGGLE) { fprintf(stderr, "\nHandshake failed (Error %d) for '%s'.\n", ret_val_main, host_filename_to_send); if (data_to_send_buffer_inner) free(data_to_send_buffer_inner); overall_exit_code = EXIT_FAILURE; continue; }
            if (ret_val_main == ACK_WRONG_TOGGLE && args.common.debug) fprintf(stderr, "DEBUG: ACK w/ wrong toggle for ENQ, proceeding.\n");
            current_ack_toggle ^= 1; printf("\nReceiver Ready. Starting transfer of '%s'...\n", host_filename_to_send);

            current_send_retry_count_inner = 0;
        send_open_packet_retry_loop:
            if (args.common.debug) fprintf(stderr, "DEBUG: Sending OPEN (toggle %d)\n", current_ack_toggle);
            ret_val_main = oasis_send_packet(transfer_session.serial_fd, (uint8_t*)&dir_entry, sizeof(dir_entry), OPEN, transfer_session.pcap_stream);
            if (ret_val_main < 0) { fprintf(stderr, "Error sending OPEN for '%s'.\n", host_filename_to_send); if (data_to_send_buffer_inner) free(data_to_send_buffer_inner); overall_exit_code = EXIT_FAILURE; continue; }
            sleep_ms_util(args.common.pacing_packet_ms);
            ret_val_main = oasis_receive_ack(transfer_session.serial_fd, current_ack_toggle, transfer_session.pcap_stream);
            if (ret_val_main == ACK_OK) { current_ack_toggle ^= 1; current_send_retry_count_inner = 0; }
            else if ((ret_val_main == ACK_TIMEOUT || ret_val_main == ACK_WRONG_TOGGLE) && current_send_retry_count_inner < MAX_SEND_RETRIES) { current_send_retry_count_inner++; fprintf(stderr, "Warning: No/Wrong ACK for OPEN ('%s'). Retry (%d/%d)...\n", host_filename_to_send, current_send_retry_count_inner, MAX_SEND_RETRIES); goto send_open_packet_retry_loop; }
            else { fprintf(stderr, "Error: Failed ACK for OPEN ('%s', Err %d).\n", host_filename_to_send, ret_val_main); if (data_to_send_buffer_inner) free(data_to_send_buffer_inner); overall_exit_code = EXIT_FAILURE; continue; }

            segment_count = 0; bytes_sent_for_file = 0;
            while (bytes_sent_for_file < final_send_len_inner || (final_send_len_inner == 0 && segment_count == 0 && (dir_entry.file_format & FILE_FORMAT_MASK) == FILE_FORMAT_SEQUENTIAL)) {
                size_t current_chunk_payload_len, packet_data_len;
                if ((dir_entry.file_format & FILE_FORMAT_MASK) == FILE_FORMAT_SEQUENTIAL) {
                    packet_data_len = XFR_BLOCK_SIZE; current_chunk_payload_len = (final_send_len_inner - bytes_sent_for_file);
                    if (current_chunk_payload_len > (XFR_BLOCK_SIZE - 2)) current_chunk_payload_len = (XFR_BLOCK_SIZE - 2);
                    if (current_chunk_payload_len > 0 && data_to_send_buffer_inner) memcpy(file_data_buf, data_to_send_buffer_inner + bytes_sent_for_file, current_chunk_payload_len);
                    if (current_chunk_payload_len < (XFR_BLOCK_SIZE - 2)) memset(file_data_buf + current_chunk_payload_len, SUB, (XFR_BLOCK_SIZE - 2) - current_chunk_payload_len);
                    file_data_buf[XFR_BLOCK_SIZE - 2] = (uint8_t)((segment_count + 1) & 0xFF); file_data_buf[XFR_BLOCK_SIZE - 1] = (uint8_t)(((segment_count + 1) >> 8) & 0xFF);
                }
                else {
                    current_chunk_payload_len = final_send_len_inner - bytes_sent_for_file;
                    if (current_chunk_payload_len > XFR_BLOCK_SIZE) current_chunk_payload_len = XFR_BLOCK_SIZE;
                    packet_data_len = current_chunk_payload_len;
                    if (current_chunk_payload_len > 0 && data_to_send_buffer_inner) memcpy(file_data_buf, data_to_send_buffer_inner + bytes_sent_for_file, current_chunk_payload_len);
                }
                if (packet_data_len == 0 && final_send_len_inner > 0 && !((final_send_len_inner == 0) && ((dir_entry.file_format & FILE_FORMAT_MASK) == FILE_FORMAT_SEQUENTIAL))) break;

                segment_count++; current_send_retry_count_inner = 0;
            send_write_packet_retry_loop:
                if (args.common.debug) fprintf(stderr, "DEBUG: Sending WRITE %d (toggle %d, len %zu)\n", segment_count, current_ack_toggle, packet_data_len);
                ret_val_main = oasis_send_packet(transfer_session.serial_fd, file_data_buf, (uint16_t)packet_data_len, WRITE, transfer_session.pcap_stream);
                if (ret_val_main < 0) { fprintf(stderr, "\nError sending WRITE %d for '%s'.\n", segment_count, host_filename_to_send); overall_exit_code = EXIT_FAILURE; break; }
                sleep_ms_util(args.common.pacing_packet_ms);
                ret_val_main = oasis_receive_ack(transfer_session.serial_fd, current_ack_toggle, transfer_session.pcap_stream);
                if (ret_val_main == ACK_OK) {
                    current_ack_toggle ^= 1; current_send_retry_count_inner = 0;
                    if (!args.common.quiet) { printf("\rSegment: %d", segment_count); fflush(stdout); }
                    bytes_sent_for_file += current_chunk_payload_len;
                }
                else if ((ret_val_main == ACK_TIMEOUT || ret_val_main == ACK_WRONG_TOGGLE) && current_send_retry_count_inner < MAX_SEND_RETRIES) {
                    current_send_retry_count_inner++; fprintf(stderr, "\nWarning: No/Wrong ACK for WRITE %d ('%s'). Retry (%d/%d)...\n", segment_count, host_filename_to_send, current_send_retry_count_inner, MAX_SEND_RETRIES); goto send_write_packet_retry_loop;
                }
                else { fprintf(stderr, "\nError: Failed ACK for WRITE %d ('%s', Err %d).\n", segment_count, host_filename_to_send, ret_val_main); overall_exit_code = EXIT_FAILURE; break; }
                if (final_send_len_inner == 0 && (dir_entry.file_format & FILE_FORMAT_MASK) == FILE_FORMAT_SEQUENTIAL) break;
            } /* End while data to send */

            if (!args.common.quiet && segment_count > 0) printf("\n");
            if (data_to_send_buffer_inner) { free(data_to_send_buffer_inner); data_to_send_buffer_inner = NULL; }
            if (raw_file_buffer_inner) { free(raw_file_buffer_inner); raw_file_buffer_inner = NULL; }

            if (overall_exit_code == EXIT_FAILURE && files_transferred == 0 && bytes_sent_for_file == 0) { /* No-op, error handled by continue/break */ }

            current_send_retry_count_inner = 0;
        send_close_packet_retry_loop:
            if (args.common.debug) fprintf(stderr, "DEBUG: Sending CLOSE (toggle %d)\n", current_ack_toggle);
            ret_val_main = oasis_send_packet(transfer_session.serial_fd, NULL, 0, CLOSE, transfer_session.pcap_stream);
            if (ret_val_main < 0) fprintf(stderr, "Error sending CLOSE for '%s'.\n", host_filename_to_send);
            else {
                sleep_ms_util(args.common.pacing_packet_ms);
                ret_val_main = oasis_receive_ack(transfer_session.serial_fd, current_ack_toggle, transfer_session.pcap_stream);
                if (ret_val_main == ACK_OK) { current_ack_toggle ^= 1; current_send_retry_count_inner = 0; if (args.common.debug) fprintf(stderr, "DEBUG: ACK for CLOSE ('%s').\n", host_filename_to_send); }
                else if ((ret_val_main == ACK_TIMEOUT || ret_val_main == ACK_WRONG_TOGGLE) && current_send_retry_count_inner < MAX_SEND_RETRIES) { current_send_retry_count_inner++; fprintf(stderr, "Warning: No/Wrong ACK for CLOSE ('%s'). Retry (%d/%d)...\n", host_filename_to_send, current_send_retry_count_inner, MAX_SEND_RETRIES); goto send_close_packet_retry_loop; }
                else fprintf(stderr, "Warning: Failed ACK for CLOSE ('%s', Err %d).\n", host_filename_to_send, ret_val_main);
            }
            printf("Finished sending: %s\n", host_filename_to_send);
            files_transferred++;
        } /* End for k_file_idx */

#if defined(_WIN32) || defined(_WIN64)
        platform_free_glob_result(files_to_process_list, num_files_in_list);
#else
        if (files_to_process_list) { if (num_files_in_list > 0 && files_to_process_list[0]) free(files_to_process_list[0]); free(files_to_process_list); }
#endif
        files_to_process_list = NULL;
    } /* End for i_argv */

    if (files_transferred > 0) {
        comm_buffer[0] = DLE; comm_buffer[1] = EOT;
        if (args.common.debug) fprintf(stderr, "DEBUG: -> Sending EOT\n");
        if (write_serial(transfer_session.serial_fd, comm_buffer, 2) == 2) {
            oasis_pcap_add_record(transfer_session.pcap_stream, OASIS_PCAP_TX, comm_buffer, 2);
            sleep_ms_util(args.common.pacing_packet_ms); printf("End of Transmission signal sent.\n");
            if (args.common.debug) fprintf(stderr, "DEBUG: Waiting for final ACK after EOT (expecting toggle %d)\n", current_ack_toggle);
            ret_val_main = oasis_receive_ack(transfer_session.serial_fd, current_ack_toggle, transfer_session.pcap_stream);
            if (ret_val_main == ACK_OK) { if (!args.common.quiet) printf("Receiver acknowledged EOT.\n"); if (args.common.debug) fprintf(stderr, "DEBUG: Received final ACK for EOT.\n"); }
            else { if (args.common.debug) fprintf(stderr, "DEBUG: Did not receive final ACK after EOT (Error: %d).\n", ret_val_main); fprintf(stderr, "Warning: Did not receive final ACK after EOT (Error: %d).\n", ret_val_main); }
        }
        else fprintf(stderr, "Warning: Error writing EOT to serial port.\n");
    }
    else {
        printf("No files were transferred.\n");
        if (files_processed_count == 0 && args.send_specific.input_filename_start_index < argc) {
            fprintf(stderr, "Please check input filenames/patterns and read permissions.\n");
        }
    }

    cleanup_transfer_session(&transfer_session);
    return overall_exit_code;
}

static void print_send_usage(const char* prog_name) {
    fprintf(stderr, "OASIS Send Utility %s [%s] (c) 2021-2025 - Howard M. Harte\n",
        CMAKE_VERSION_STR, GIT_VERSION_STR);
    fprintf(stderr, "https://github.com/hharte/oasis-utils\n\n");
    fprintf(stderr, "Usage: %s <port> [options] <filename1_or_pattern> [filename2_or_pattern ...]\n", prog_name);
    fprintf(stderr, "\t<port>      Serial Port device name (e.g., /dev/ttyS0, COM1).\n");
    fprintf(stderr, "\t<filename_or_pattern> File to send or wildcard pattern (e.g., *.txt).\n");
    fprintf(stderr, "\t            Host filename implies OASIS NAME.TYPE[_FMT[_RL]].\n");
    fprintf(stderr, "\t            If only NAME.TYPE is given, Sequential format is assumed.\n");
    fprintf(stderr, "\t            On Windows, wildcards are expanded by this program.\n");
    fprintf(stderr, "\t            On POSIX, wildcards are typically expanded by the shell.\n");
    fprintf(stderr, "\tOptions:\n");
    fprintf(stderr, "\t      -q              Quiet: Suppress file detail listing.\n");
    fprintf(stderr, "\t      -d              Debug: Print debug messages to stderr.\n");
    fprintf(stderr, "\t      -a, --ascii     ASCII: Convert CR/LF to CR, treat SUB as EOF.\n");
    fprintf(stderr, "\t      -f, --flow-control Disable Hardware (RTS/CTS) Flow Control (Default: Enabled).\n");
    fprintf(stderr, "\t      -b <rate>       Baud rate (default: %d).\n", DEFAULT_BAUD_RATE);
    fprintf(stderr, "\t      -u <id>         User ID (0-255) for DEB (default: 0).\n");
    fprintf(stderr, "\t      --pcap <file>   Save raw communication to PCAP file.\n");
    fprintf(stderr, "\t      --pacing-packet <ms> Delay (ms) after sending each packet (default: 0).\n");
    fprintf(stderr, "\t      --help          Display this help message.\n");
}


static int parse_send_args(int argc, char* argv[], oasis_send_args_t* args) {
    int i;
    char* arg_value;
    char* endptr_uid;

    /* Initialize common args to defaults */
    args->common.quiet = 0;
    args->common.debug = 0;
    args->common.baud_rate = DEFAULT_BAUD_RATE;
    args->common.pacing_packet_ms = 0;
    args->common.pcap_filename[0] = '\0';
    args->common.ascii_conversion = 0;
    args->common.flow_control = 1; /* Enabled by default */
    args->common.port_path[0] = '\0';

    /* Initialize send-specific args */
    args->send_specific.user_id = 0; /* Default user ID */
    args->send_specific.input_filename_start_index = -1; /* Not found yet */

    int current_arg_idx = 1; /* Start after program name */

    /* First positional argument must be port for oasis_send */
    if (current_arg_idx < argc && argv[current_arg_idx][0] != '-') {
        strncpy(args->common.port_path, argv[current_arg_idx], sizeof(args->common.port_path) - 1);
        args->common.port_path[sizeof(args->common.port_path) - 1] = '\0';
        current_arg_idx++;
    }
    else if (current_arg_idx < argc && (strcmp(argv[current_arg_idx], "--help") == 0 || strcmp(argv[current_arg_idx], "-h") == 0)) {
        print_send_usage(argv[0]);
        return 0; /* Signal help shown */
    }
    /* Port path will be checked for non-emptiness later */


    for (i = current_arg_idx; i < argc; ) {
        int original_i = i; /* Store i before potential modification by parsers */
        int common_parse_rc = 0;
        int specific_option_parsed = 0;

        /* Handle --help or -h explicitly here as they can appear anywhere */
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_send_usage(argv[0]);
            return 0; /* Help requested, stop parsing */
        }

        /* 1. Try send-specific option -u */
        if (strcmp(argv[i], "-u") == 0) {
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                arg_value = argv[i + 1];
                errno = 0;
                long uid_val = strtol(arg_value, &endptr_uid, 10);
                if (endptr_uid == arg_value || *endptr_uid != '\0' || ((uid_val == LONG_MIN || uid_val == LONG_MAX) && errno == ERANGE) || errno != 0 || uid_val < 0 || uid_val > 255) {
                    fprintf(stderr, "Error: Invalid user ID '%s'. Must be 0-255.\n", arg_value);
                    print_send_usage(argv[0]); return 0; /* Error */
                }
                args->send_specific.user_id = (int)uid_val;
                i += 2; /* Consumed -u and its value */
                specific_option_parsed = 1;
            }
            else {
                fprintf(stderr, "Error: Option '-u' requires a user ID value.\n");
                print_send_usage(argv[0]); return 0; /* Error */
            }
        }
        /* Add other send-specific options here if any, using similar structure */

        if (specific_option_parsed) {
            continue;
        }

        /* 2. Try common options using the helper */
        /* parse_one_common_option advances 'i' internally if it consumes args */
        common_parse_rc = parse_one_common_option(argc, argv, &i, &args->common);

        if (common_parse_rc == 1) { /* Common option parsed, 'i' is already advanced */
            continue;
        }
        else if (common_parse_rc < 0) { /* Error or help from common parser */
            if (common_parse_rc == -2) print_send_usage(argv[0]); /* parse_one_common_option returns -2 for help */
            return 0; /* Error or help shown */
        }
        /* If common_parse_rc is 0, it means argv[original_i] was not a common option */

        /* 3. If it's an option but wasn't specific and wasn't common, it's unknown */
        if (argv[original_i][0] == '-') {
            fprintf(stderr, "Error: Unknown option '%s'.\n", argv[original_i]);
            print_send_usage(argv[0]); return 0; /* Error */
        }

        /* 4. Otherwise, it's a positional argument (filename for send) */
        if (args->send_specific.input_filename_start_index == -1) {
            args->send_specific.input_filename_start_index = original_i;
        }
        /* For send, all subsequent arguments are filenames if not options.
           The main() will iterate from input_filename_start_index.
           If an option appears AFTER the first filename, this loop will pick it up.
        */
        i++; /* Advance past this positional argument if it wasn't an option */
    }

    /* Final checks */
    if (args->common.port_path[0] == '\0') {
        fprintf(stderr, "Error: Serial port argument is required.\n");
        print_send_usage(argv[0]);
        return 0;
    }
    if (args->send_specific.input_filename_start_index == -1 || args->send_specific.input_filename_start_index >= argc) {
        /* This means no filenames were found among the arguments.
           If input_filename_start_index is still -1, it means all arguments after port
           were processed as options, and no positional filename was encountered.
        */
        fprintf(stderr, "Error: At least one filename to send is required.\n");
        print_send_usage(argv[0]);
        return 0;
    }

    return 1; /* Success, indicates args parsed (actual count not critical for caller) */
}
