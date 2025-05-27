/*
 * Utility to Receive a file from an OASIS system via Serial Port.
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

/* Standard C Headers */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <limits.h>
#include <sys/stat.h>

/* Platform-Specific Includes */
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <sys/types.h>
#endif /* _WIN32 */

/* Local Headers */
#include "oasis.h"
#include "mm_serial.h"
#include "oasis_transfer_utils.h"

/* Structure to hold command line arguments specific to oasis_recv */
typedef struct oasis_recv_specific_args {
    char output_path[256];
    /* Other recv-specific fields can be added here if necessary */
} oasis_recv_specific_args_t;

/* Combined arguments structure */
typedef struct oasis_recv_args {
    oasis_transfer_common_args_t common;
    oasis_recv_specific_args_t recv_specific;
} oasis_recv_args_t;


/* --- Forward Declarations --- */
static int parse_recv_args(int argc, char* argv[], oasis_recv_args_t* args);
static void print_recv_usage(const char* prog_name);

/* --- Main Function --- */
int main(int argc, char* argv[]) {
    uint8_t     comm_buffer[1024];
    uint8_t     decoded_buf[512];
    int         lrc_result;
    ssize_t     bytes_read;
    uint16_t    decoded_len;
    uint8_t     packet_cmd;
    directory_entry_block_t dir_entry_wire = { 0 }; /* For data as it comes off the wire (LE) */
    directory_entry_block_t dir_entry_host = { 0 }; /* For data in host byte order */
    directory_entry_block_t dir_entry = { 0 };      /* Primary working DEB in host byte order */
    FILE* ostream = NULL;
    oasis_recv_args_t args;
    int         toggle = 0;
    int         current_segment = 0;
    int         ret = EXIT_SUCCESS;
    int         ack_retries = 0;
    const int   MAX_ACK_RETRIES = 5;
    char        current_host_filepath[1024];
    size_t      total_bytes_written_for_current_file = 0;
    size_t      logical_file_size_from_deb = 0;
    uint8_t     current_file_type_mask = 0;
    oasis_transfer_session_t transfer_session;

    current_host_filepath[0] = '\0';
    memset(&transfer_session, 0, sizeof(transfer_session));
    /* dir_entry_wire, dir_entry_host, dir_entry are already zero-initialized */


    if (parse_recv_args(argc, argv, &args) <= 0) {
        /* parse_recv_args prints usage or error */
        return EXIT_FAILURE;
    }

    if (initialize_transfer_session(&args.common, &transfer_session) != 0) {
        return EXIT_FAILURE;
    }

    if (!args.common.quiet) {
        fprintf(stdout, "Waiting for Sending Station");
        fflush(stdout);
    }
    else if (args.common.debug) {
        fprintf(stderr, "DEBUG: Waiting for Sending Station (Quiet mode)\n");
    }

    /* Loop to wait for initial ENQ */
    for (int wait_retries = 300; ; wait_retries--) {
        bytes_read = read_serial(transfer_session.serial_fd, comm_buffer, 1);
        if (bytes_read > 0) {
            oasis_pcap_add_record(transfer_session.pcap_stream, OASIS_PCAP_RX, comm_buffer, (uint16_t)bytes_read);
            comm_buffer[0] &= 0x7F; /* Mask to 7-bit */
            if (comm_buffer[0] == ENQ) {
                if (!args.common.quiet) fprintf(stdout, "\n");
                fprintf(stdout, "Sender detected (ENQ received). Starting transfer.\n");
                break; /* Exit ENQ wait loop */
            }
            else {
                if (!args.common.quiet) fprintf(stdout, "\nWarning: Unexpected 0x%02x while waiting for ENQ.\n", comm_buffer[0]);
                else if (args.common.debug) fprintf(stderr, "DEBUG: Unexpected 0x%02x waiting for ENQ.\n", comm_buffer[0]);
                /* Continue waiting for ENQ */
            }
        }
        else if (bytes_read < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != ETIMEDOUT) {
            fprintf(stderr, "\nError reading from serial port: %s\n", strerror(errno));
            ret = EXIT_FAILURE; goto cleanup;
        }
        if (!args.common.quiet && (wait_retries > 0) && ((wait_retries - 1) % 10 == 0)) {
            fprintf(stdout, "."); fflush(stdout);
        }
        if (wait_retries == 0) {
            if (!args.common.quiet) fprintf(stderr, "\n");
            fprintf(stderr, "Timeout: No ENQ received.\n");
            ret = EXIT_FAILURE; goto cleanup;
        }
    }

    /* Main packet processing loop */
    while (1) {
        /* Send ACK for the previous valid packet or initial ENQ */
        if (oasis_send_ack(transfer_session.serial_fd, toggle, transfer_session.pcap_stream) != 0) {
            ret = EXIT_FAILURE; goto cleanup;
        }
        ack_retries = 0; /* Reset ACK retries for the new packet we are expecting */

    read_next_packet_recv: /* Label for retrying read after resending ACK */
        if (args.common.debug) fprintf(stderr, "DEBUG: Waiting for packet (expect toggle %d)...\n", toggle ^ 1);
        bytes_read = read_serial(transfer_session.serial_fd, comm_buffer, sizeof(comm_buffer));

        if (bytes_read > 0) {
            oasis_pcap_add_record(transfer_session.pcap_stream, OASIS_PCAP_RX, comm_buffer, (uint16_t)bytes_read);
        }

        if (bytes_read < 0) { /* Read error or timeout */
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) { /* Timeout */
                fprintf(stderr, "\nWarning: Read timeout waiting for packet (expect toggle %d).\n", toggle ^ 1);
                if (ack_retries < MAX_ACK_RETRIES) {
                    ack_retries++;
                    fprintf(stderr, "   Resending ACK %c (Attempt %d/%d)\n", '0' + (toggle & 1), ack_retries, MAX_ACK_RETRIES);
                    if (oasis_send_ack(transfer_session.serial_fd, toggle, transfer_session.pcap_stream) != 0) {
                        ret = EXIT_FAILURE;
                        goto cleanup;
                    }
                    goto read_next_packet_recv;
                }
                else { fprintf(stderr, "   Max ACK resends reached. Aborting.\n"); ret = EXIT_FAILURE; goto cleanup; }
            }
            else { /* Other read error */
                fprintf(stderr, "\nError reading packet: %s\n", strerror(errno));
                ret = EXIT_FAILURE; goto cleanup;
            }
        }
        else if (bytes_read == 0) { /* Should not happen with blocking read if port is open, but handle defensively */
            fprintf(stderr, "\nWarning: Read 0 bytes for packet (expect toggle %d).\n", toggle ^ 1);
            if (ack_retries < MAX_ACK_RETRIES) {
                ack_retries++;
                fprintf(stderr, "   Resending ACK %c (Attempt %d/%d)\n", '0' + (toggle & 1), ack_retries, MAX_ACK_RETRIES);
                if (oasis_send_ack(transfer_session.serial_fd, toggle, transfer_session.pcap_stream) != 0) {
                    ret = EXIT_FAILURE;
                    goto cleanup;
                }
                goto read_next_packet_recv;
            }
            else { fprintf(stderr, "   Max ACK resends reached. Aborting.\n"); ret = EXIT_FAILURE; goto cleanup; }
        }

        /* Mask to 7-bit if necessary, though protocol might not strictly require */
        for (uint16_t i_loop = 0; (ssize_t)i_loop < bytes_read; i_loop++) {
            comm_buffer[i_loop] &= 0x7F;
        }
        if (args.common.debug && bytes_read > 0) {
            fprintf(stderr, "DEBUG: Read %zd raw bytes (masked):\n", bytes_read);
            dump_hex(comm_buffer, (int)bytes_read);
        }

        /* Handle ENQ received mid-transfer (sender might be confused) */
        if (bytes_read == 1 && comm_buffer[0] == ENQ) {
            toggle = 0; /* Reset toggle for new handshake */
            if (!args.common.quiet) fprintf(stdout, "\nReceived ENQ mid-transfer, re-acking with toggle 0.\n");
            if (oasis_send_ack(transfer_session.serial_fd, toggle, transfer_session.pcap_stream) != 0) {
                ret = EXIT_FAILURE;
                goto cleanup;
            }
            goto read_next_packet_recv; /* Wait for packet again */
        }

        /* Handle EOT */
        if (bytes_read >= 2 && comm_buffer[0] == DLE && comm_buffer[1] == EOT) {
            if (!args.common.quiet && current_segment > 0) fprintf(stdout, "\n");
            fprintf(stdout, "End of Transmission (EOT) received.\n");
            /* Send final ACK with current toggle */
            oasis_send_ack(transfer_session.serial_fd, toggle, transfer_session.pcap_stream);
            ret = EXIT_SUCCESS; goto cleanup;
        }

        /* Process potential data packet */
        if (bytes_read >= 5) { /* Min valid data packet for decode is DLE STX CMD DLE ETX LRC RUB (7 bytes, but decode checks <5) */
            if (args.common.debug) fprintf(stderr, "DEBUG: Decoding packet (len=%zd)\n", bytes_read);
            lrc_result = oasis_packet_decode(comm_buffer, (uint16_t)bytes_read, decoded_buf, &decoded_len);

            if (lrc_result > 0) { /* Checksum OK */
                packet_cmd = comm_buffer[2]; /* Command is at a fixed offset in raw buffer */
                if (args.common.debug) fprintf(stderr, "DEBUG: Packet OK. Cmd='%c', Decoded Len=%u\n", isprint(packet_cmd) ? packet_cmd : '.', decoded_len);
                toggle ^= 1; /* Valid packet, toggle ACK for next send */

                switch (packet_cmd) {
                case OPEN:
                    if (!args.common.quiet && current_segment > 0) fprintf(stdout, "\n");
                    fprintf(stdout, "Received OPEN packet.\n");
                    if (decoded_len < sizeof(directory_entry_block_t)) {
                        fprintf(stderr, "Error: OPEN packet too short (%u bytes) for DEB.\n", decoded_len);
                        toggle ^= 1; /* Revert toggle, effectively NAKing */
                        continue;    /* Resend previous ACK by looping */
                    }
                    memcpy(&dir_entry_wire, decoded_buf, sizeof(directory_entry_block_t));

                    /* Convert received LE DEB to host byte order */
                    dir_entry_host.file_format = dir_entry_wire.file_format;
                    memcpy(dir_entry_host.file_name, dir_entry_wire.file_name, FNAME_LEN);
                    memcpy(dir_entry_host.file_type, dir_entry_wire.file_type, FTYPE_LEN);
                    memcpy(&dir_entry_host.timestamp, &dir_entry_wire.timestamp, sizeof(oasis_tm_t));
                    dir_entry_host.owner_id = dir_entry_wire.owner_id;
                    dir_entry_host.shared_from_owner_id = dir_entry_wire.shared_from_owner_id;
                    dir_entry_host.record_count = le16toh(dir_entry_wire.record_count);
                    dir_entry_host.block_count = le16toh(dir_entry_wire.block_count);
                    dir_entry_host.start_sector = le16toh(dir_entry_wire.start_sector);
                    dir_entry_host.file_format_dependent1 = le16toh(dir_entry_wire.file_format_dependent1);
                    dir_entry_host.file_format_dependent2 = le16toh(dir_entry_wire.file_format_dependent2);

                    dir_entry = dir_entry_host; /* Use host-order DEB for operations */

                    if (ostream != NULL) {
                        fprintf(stderr, "Warning: New OPEN received while a file ('%s') was already open. Closing previous.\n", current_host_filepath);
                        fclose(ostream);
                        ostream = NULL;
                        current_host_filepath[0] = '\0';
                    }
                    char hfn_open[MAX_HOST_FILENAME_LEN];
                    if (!oasis_deb_to_host_filename(&dir_entry, hfn_open, sizeof(hfn_open))) { /* Use dir_entry (host order) */
                        fprintf(stderr, "Error: Failed to generate host filename from received DEB.\n");
                        toggle ^= 1; ret = EXIT_FAILURE; goto cleanup;
                    }
                    snprintf(current_host_filepath, sizeof(current_host_filepath), "%s%c%s", args.recv_specific.output_path, kPathSeparator, hfn_open);

                    ret = create_and_open_oasis_file(args.recv_specific.output_path, hfn_open, &ostream, &dir_entry, args.common.quiet, args.common.debug);
                    if (ret != 0) {
                        toggle ^= 1; ret = (ret < 0) ? EXIT_FAILURE : ret; goto cleanup;
                    }
                    current_segment = 0;
                    total_bytes_written_for_current_file = 0;
                    logical_file_size_from_deb = 0;
                    current_file_type_mask = dir_entry.file_format & FILE_FORMAT_MASK;

                    if (current_file_type_mask != FILE_FORMAT_SEQUENTIAL) {
                        switch (current_file_type_mask) {
                        case FILE_FORMAT_DIRECT: logical_file_size_from_deb = (size_t)dir_entry.record_count * dir_entry.file_format_dependent1; break;
                        case FILE_FORMAT_INDEXED: case FILE_FORMAT_KEYED: logical_file_size_from_deb = (size_t)dir_entry.record_count * (dir_entry.file_format_dependent1 & 0x1FF); break;
                        case FILE_FORMAT_RELOCATABLE: logical_file_size_from_deb = (size_t)dir_entry.file_format_dependent2; break;
                        case FILE_FORMAT_ABSOLUTE:
                            if (dir_entry.file_format_dependent1 == SECTOR_SIZE) logical_file_size_from_deb = (size_t)dir_entry.record_count * SECTOR_SIZE;
                            else logical_file_size_from_deb = (size_t)dir_entry.block_count * BLOCK_SIZE;
                            break;
                        }
                        size_t max_disk_size = (size_t)dir_entry.block_count * BLOCK_SIZE;
                        if (logical_file_size_from_deb > max_disk_size && max_disk_size > 0) logical_file_size_from_deb = max_disk_size;
                        if (logical_file_size_from_deb == 0 && dir_entry.block_count > 0) logical_file_size_from_deb = max_disk_size;
                        if (args.common.debug) fprintf(stderr, "DEBUG: Logical size for '%s' is %zu bytes.\n", current_host_filepath, logical_file_size_from_deb);
                    }
                    break;
                case WRITE:
                    current_segment++;
                    if (!args.common.quiet) { fprintf(stdout, "\rReceived WRITE: Segment %d", current_segment); fflush(stdout); }
                    else if (args.common.debug) fprintf(stderr, "DEBUG: WRITE segment %d, Decoded Len=%u\n", current_segment, decoded_len);

                    if (ostream == NULL) {
                        fprintf(stderr, "\nWarning: WRITE packet received before OPEN. Discarding data.\n");
                        toggle ^= 1;
                        continue;
                    }
                    size_t bytes_to_write = decoded_len;
                    if ((dir_entry.file_format & FILE_FORMAT_MASK) == FILE_FORMAT_SEQUENTIAL) {
                        if (decoded_len >= sizeof(uint16_t)) {
                            bytes_to_write -= sizeof(uint16_t);
                        }
                        else {
                            fprintf(stderr, "\nWarning: Sequential WRITE segment %d is too short (%u bytes) to contain link.\n", current_segment, decoded_len);
                            /* This is a malformed packet for sequential, effectively NAK by not processing and resending previous ACK */
                        }
                    }
                    if (logical_file_size_from_deb > 0 && current_file_type_mask != FILE_FORMAT_SEQUENTIAL) {
                        if (total_bytes_written_for_current_file >= logical_file_size_from_deb) {
                            bytes_to_write = 0;
                        }
                        else if (total_bytes_written_for_current_file + bytes_to_write > logical_file_size_from_deb) {
                            bytes_to_write = logical_file_size_from_deb - total_bytes_written_for_current_file;
                        }
                    }

                    if (bytes_to_write > 0) {
                        if (fwrite(decoded_buf, 1, bytes_to_write, ostream) != bytes_to_write) {
                            perror("\nError writing to output file");
                            toggle ^= 1; ret = EXIT_FAILURE; goto cleanup;
                        }
                        total_bytes_written_for_current_file += bytes_to_write;
                    }
                    break;
                case CLOSE:
                    if (!args.common.quiet && current_segment > 0) fprintf(stdout, "\n");
                    fprintf(stdout, "Received CLOSE. Transfer complete for '%s'.\n", current_host_filepath[0] != '\0' ? current_host_filepath : "(unknown file)");
                    if (ostream != NULL) {
                        fclose(ostream);
                        ostream = NULL;
                        if (args.common.ascii_conversion && (current_file_type_mask == FILE_FORMAT_SEQUENTIAL) && current_host_filepath[0] != '\0') {
                            int conv_ret = oasis_ascii_file_to_host_file(current_host_filepath, NULL);
                            if (conv_ret == 0 && args.common.debug) fprintf(stdout, "ASCII conversion successful for '%s'.\n", current_host_filepath);
                            else if (conv_ret == OASIS_ERR_INVALID_INPUT && !args.common.quiet) fprintf(stdout, "File '%s' not 7-bit ASCII, skipping conversion.\n", current_host_filepath);
                            else if (conv_ret != 0) fprintf(stderr, "Warning: ASCII conversion failed for '%s' (Err %d).\n", current_host_filepath, conv_ret);
                        }
                        set_file_timestamp(current_host_filepath, &dir_entry.timestamp); /* Use dir_entry (host order) */
                    }
                    else {
                        fprintf(stderr, "\nWarning: CLOSE packet received but no file was open.\n");
                    }
                    memset(&dir_entry_host, 0, sizeof(dir_entry_host));
                    memset(&dir_entry_wire, 0, sizeof(dir_entry_wire));
                    dir_entry = dir_entry_host; /* Reset dir_entry to an empty host-order struct */
                    current_host_filepath[0] = '\0';
                    total_bytes_written_for_current_file = 0;
                    logical_file_size_from_deb = 0;
                    current_file_type_mask = 0;
                    current_segment = 0;
                    break;
                default:
                    fprintf(stderr, "\nWarning: Unknown packet type '%c' (0x%02X)\n", isprint(packet_cmd) ? packet_cmd : '.', packet_cmd);
                    toggle ^= 1; /* Revert toggle as we don't know how to handle this */
                    break;
                }
            }
            else if (lrc_result == 0) { /* Checksum mismatch */
                fprintf(stderr, "\nError: Checksum mismatch. Sending NAK (resending previous ACK).\n");
                if (args.common.debug || !args.common.quiet) dump_hex(comm_buffer, (int)bytes_read);
                /* Do not toggle ACK, loop will resend current ACK */
            }
            else { /* Other decode error */
                fprintf(stderr, "\nError: Packet decode failed (Code: %d). Sending NAK.\n", lrc_result);
                if (args.common.debug || !args.common.quiet) dump_hex(comm_buffer, (int)bytes_read);
                /* Do not toggle ACK */
            }
        }
        else { /* Short packet */
            /* Avoid erroring on EOT or ENQ that might be shorter */
            if (!(bytes_read >= 2 && comm_buffer[0] == DLE && comm_buffer[1] == EOT) &&
                !(bytes_read == 1 && (comm_buffer[0] & 0x7F) == ENQ)) {
                fprintf(stderr, "\nWarning: Short packet received (len=%zd). Sending NAK.\n", bytes_read);
                if (args.common.debug || !args.common.quiet) dump_hex(comm_buffer, (int)bytes_read);
                /* Do not toggle ACK */
            }
        }
        if (args.common.pacing_packet_ms > 0) sleep_ms_util(args.common.pacing_packet_ms);
    } /* End while(1) */

cleanup:
    if (ostream != NULL) {
        if (ret != EXIT_SUCCESS && current_host_filepath[0] != '\0') {
            fprintf(stderr, "Closing '%s' due to error.\n", current_host_filepath);
        }
        fclose(ostream);
    }
    cleanup_transfer_session(&transfer_session);
    if (ret == EXIT_SUCCESS) {
        if (!args.common.quiet) fprintf(stdout, "Receive operation completed successfully.\n");
    }
    else {
        fprintf(stderr, "Receive operation failed.\n");
    }
    return ret;
}

static void print_recv_usage(const char* prog_name) {
    fprintf(stderr, "OASIS Receive Utility %s [%s] (c) 2021-2025 - Howard M. Harte\n",
        CMAKE_VERSION_STR, GIT_VERSION_STR);
    fprintf(stderr, "https://github.com/hharte/oasis-utils\n\n");
    fprintf(stderr, "Usage: %s <port> [<output_dir>] [options]\n", prog_name);
    fprintf(stderr, "\t<port>             Serial Port device name (e.g., /dev/ttyS0, COM1).\n");
    fprintf(stderr, "\t<output_dir>       Optional output directory (default: current dir).\n");
    fprintf(stderr, "\tOptions:\n");
    fprintf(stderr, "\t     -q              Quiet: Suppress file detail listing.\n");
    fprintf(stderr, "\t     -d              Debug: Print debug messages to stderr.\n");
    fprintf(stderr, "\t     -a, --ascii     Convert received ASCII files to host line endings.\n");
    fprintf(stderr, "\t     -f, --flow-control Disable Hardware (RTS/CTS) Flow Control (Default: Enabled).\n");
    fprintf(stderr, "\t     -b <rate>       Baud rate (default: %d).\n", DEFAULT_BAUD_RATE);
    fprintf(stderr, "\t     --pcap <file>   Save raw communication to PCAP file.\n");
    fprintf(stderr, "\t     --pacing-packet <ms> Delay (ms) after receiving each packet (default: 0).\n");
    fprintf(stderr, "\t     --help          Display this help message.\n");
}

static int parse_recv_args(int argc, char* argv[], oasis_recv_args_t* args) {
    int i;
    int initial_arg_index = 1;
    int output_path_arg_found = 0;

    /* Initialize common args to defaults */
    args->common.quiet = 0;
    args->common.debug = 0;
    args->common.baud_rate = DEFAULT_BAUD_RATE;
    args->common.pacing_packet_ms = 0;
    args->common.pcap_filename[0] = '\0';
    args->common.ascii_conversion = 0;
    args->common.flow_control = 1; /* Enabled by default */
    args->common.port_path[0] = '\0';

    /* Initialize recv-specific args */
    strcpy(args->recv_specific.output_path, "."); /* Default output path */


    /* First positional argument must be port for oasis_recv */
    if (initial_arg_index < argc && argv[initial_arg_index][0] != '-') {
        strncpy(args->common.port_path, argv[initial_arg_index], sizeof(args->common.port_path) - 1);
        args->common.port_path[sizeof(args->common.port_path) - 1] = '\0';
        initial_arg_index++;
        /* Second positional could be output_dir */
        if (initial_arg_index < argc && argv[initial_arg_index][0] != '-') {
            strncpy(args->recv_specific.output_path, argv[initial_arg_index], sizeof(args->recv_specific.output_path) - 1);
            args->recv_specific.output_path[sizeof(args->recv_specific.output_path) - 1] = '\0';
            initial_arg_index++;
            output_path_arg_found = 1;
        }
    }
    else if (initial_arg_index < argc && (strcmp(argv[initial_arg_index], "--help") == 0 || strcmp(argv[initial_arg_index], "-h") == 0)) {
        print_recv_usage(argv[0]);
        return 0; /* Signal help shown */
    }

    for (i = initial_arg_index; i < argc; ) {
        int original_i = i;
        int common_parse_rc = 0;

        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_recv_usage(argv[0]);
            return 0; /* Help requested */
        }

        common_parse_rc = parse_one_common_option(argc, argv, &i, &args->common);

        if (common_parse_rc == 1) { /* Common option parsed, 'i' is advanced */
            continue;
        }
        else if (common_parse_rc < 0) { /* Error or help from common parser */
            if (common_parse_rc == -2) print_recv_usage(argv[0]);
            return 0;
        }

        if (argv[original_i][0] == '-') {
            fprintf(stderr, "Error: Unknown option '%s'.\n", argv[original_i]);
            print_recv_usage(argv[0]); return 0; /* Error */
        }

        /* Positional arguments for recv */
        if (args->common.port_path[0] == '\0') { /* Port not yet found (should have been first) */
            strncpy(args->common.port_path, argv[original_i], sizeof(args->common.port_path) - 1);
            args->common.port_path[sizeof(args->common.port_path) - 1] = '\0';
        }
        else if (!output_path_arg_found) { /* output_path not yet found from initial positional check */
            strncpy(args->recv_specific.output_path, argv[original_i], sizeof(args->recv_specific.output_path) - 1);
            args->recv_specific.output_path[sizeof(args->recv_specific.output_path) - 1] = '\0';
            output_path_arg_found = 1;
        }
        else {
            fprintf(stderr, "Error: Too many positional arguments. Unexpected: '%s'.\n", argv[original_i]);
            print_recv_usage(argv[0]); return 0; /* Error */
        }
        i++; /* Advance past this positional argument if it wasn't an option */
    }

    /* Final checks */
    if (args->common.port_path[0] == '\0') {
        fprintf(stderr, "Error: Serial port argument is required.\n");
        print_recv_usage(argv[0]);
        return 0;
    }
    return 1; /* Success */
}
