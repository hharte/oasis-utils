/*
 * oasis_transfer_utils.c - Shared utilities for OASIS send/receive programs.
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h> /* For strtol, EXIT_SUCCESS, EXIT_FAILURE */
#include <string.h>
#include <errno.h>
#include <limits.h> /* For INT_MAX, LONG_MIN, LONG_MAX */

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h> /* For usleep */
#endif

#include "oasis_transfer_utils.h"
#include "mm_serial.h"      /* For serial port functions */
#include "oasis_pcap.h"     /* For PCAP functions */

 /*
  * Initializes a transfer session.
  */
int initialize_transfer_session(const oasis_transfer_common_args_t* common_args, oasis_transfer_session_t* session) {
    if (common_args == NULL || session == NULL) {
        fprintf(stderr, "Error: Null arguments to initialize_transfer_session.\n");
        return -1;
    }

    session->common_args = common_args;
    session->pcap_stream = NULL;
    session->serial_fd = -1;

    if (common_args->pcap_filename[0] != '\0') {
        if (oasis_pcap_create(common_args->pcap_filename, &session->pcap_stream) != 0) {
            fprintf(stderr, "Warning: Could not create PCAP file '%s'. Continuing without PCAP logging.\n", common_args->pcap_filename);
            session->pcap_stream = NULL; /* Ensure it's NULL */
        }
        else {
            if (common_args->debug) {
                fprintf(stderr, "DEBUG: PCAP logging enabled to '%s'.\n", common_args->pcap_filename);
            }
        }
    }

    session->serial_fd = open_serial(common_args->port_path);
    if (session->serial_fd < 0) {
        /* open_serial prints its own error */
        oasis_pcap_close(session->pcap_stream); /* Close PCAP if it was opened */
        session->pcap_stream = NULL;
        return -1;
    }

    if (init_serial(session->serial_fd, common_args->baud_rate, common_args->flow_control) != 0) {
        /* init_serial prints its own error */
        close_serial(session->serial_fd);
        session->serial_fd = -1;
        oasis_pcap_close(session->pcap_stream);
        session->pcap_stream = NULL;
        return -1;
    }

    if (common_args->debug) {
        fprintf(stderr, "DEBUG: Serial port '%s' opened and initialized at %d baud. Flow control: %s.\n",
            common_args->port_path, common_args->baud_rate, common_args->flow_control ? "Enabled" : "Disabled");
    }
    return 0;
}

/*
 * Cleans up a transfer session.
 */
void cleanup_transfer_session(oasis_transfer_session_t* session) {
    if (session == NULL) {
        return;
    }
    if (session->serial_fd >= 0) {
        if (session->common_args && session->common_args->debug) {
            fprintf(stderr, "DEBUG: Closing serial port.\n");
        }
        close_serial(session->serial_fd);
        session->serial_fd = -1;
    }
    oasis_pcap_close(session->pcap_stream); /* Handles NULL stream gracefully */
    session->pcap_stream = NULL;
}

/*
 * Cross-platform sleep function.
 */
void sleep_ms_util(int milliseconds) {
    if (milliseconds <= 0) {
        return;
    }
#ifdef _WIN32
    Sleep(milliseconds);
#else
    usleep(milliseconds * 1000); /* usleep takes microseconds */
#endif
}

/*
 * Attempts to parse one common command-line option starting at argv[*idx_ptr].
 * If a common option is recognized and parsed successfully (including its value
 * if it takes one), common_args is updated, *idx_ptr is advanced past the
 * consumed argument(s), and the function returns 1.
 * If argv[*idx_ptr] is not a recognized common option, it returns 0 and *idx_ptr
 * is unchanged.
 * If a recognized common option has a parsing error (e.g., invalid value),
 * it prints an error and returns -1. *idx_ptr advancement is not guaranteed on error.
 * If a help option (-h, --help) is detected, it returns -2.
 *
 * Returns:
 * 1: Common option successfully parsed (*idx_ptr advanced accordingly).
 * 0: Argument at *idx_ptr is not a recognized common option (*idx_ptr unchanged).
 * -1: Error parsing a recognized common option.
 * -2: Help option detected.
 */
int parse_one_common_option(int argc, char* argv[], int* idx_ptr, oasis_transfer_common_args_t* common_args) {
    int i = *idx_ptr;
    char* current_arg = argv[i];
    char* arg_value;
    char* endptr;

    if (current_arg[0] != '-') {
        return 0; /* Not an option */
    }

    if (strcmp(current_arg, "--help") == 0) {
        *idx_ptr = i + 1; /* Consume --help */
        return -2; /* Help requested */
    }
    if (strcmp(current_arg, "-h") == 0) {
        /* Check if it's just -h or part of a group like -hxyz */
        if (strlen(current_arg) == 2) {
            *idx_ptr = i + 1; /* Consume -h */
            return -2; /* Help requested */
        }
        /* If it's part of a group, let short option logic handle it, */
        /* but -h alone is prioritized for help. */
    }


    if (current_arg[1] == '-') { /* Long option */
        if (strcmp(current_arg, "--ascii") == 0) {
            common_args->ascii_conversion = 1;
            *idx_ptr = i + 1;
            return 1;
        }
        else if (strcmp(current_arg, "--flow-control") == 0) {
            common_args->flow_control = 0; /* Flag disables it */
            *idx_ptr = i + 1;
            return 1;
        }
        else if (strncmp(current_arg, "--pacing-packet", 15) == 0) {
            arg_value = NULL;
            if (strlen(current_arg) > 16 && current_arg[15] == '=') {
                arg_value = &current_arg[16];
                *idx_ptr = i + 1; /* Consumed current_arg */
            }
            else if (i + 1 < argc && argv[i + 1][0] != '-') {
                arg_value = argv[i + 1];
                *idx_ptr = i + 2; /* Consumed current_arg and its value */
            }
            if (arg_value) {
                errno = 0;
                long ms = strtol(arg_value, &endptr, 10);
                if (endptr == arg_value || *endptr != '\0' || ((ms == LONG_MIN || ms == LONG_MAX) && errno == ERANGE) || errno != 0 || ms < 0 || ms > INT_MAX) {
                    fprintf(stderr, "Error: Invalid millisecond value for --pacing-packet: '%s'.\n", arg_value);
                    return -1;
                }
                common_args->pacing_packet_ms = (int)ms;
                return 1;
            }
            else {
                fprintf(stderr, "Error: Option '--pacing-packet' requires a millisecond value.\n");
                return -1;
            }
        }
        else if (strncmp(current_arg, "--pcap", 6) == 0) {
            arg_value = NULL;
            if (strlen(current_arg) > 7 && current_arg[6] == '=') {
                arg_value = &current_arg[7];
                *idx_ptr = i + 1; /* Consumed current_arg */
            }
            else if (i + 1 < argc && argv[i + 1][0] != '-') {
                arg_value = argv[i + 1];
                *idx_ptr = i + 2; /* Consumed current_arg and its value */
            }
            if (arg_value) {
                strncpy(common_args->pcap_filename, arg_value, sizeof(common_args->pcap_filename) - 1);
                common_args->pcap_filename[sizeof(common_args->pcap_filename) - 1] = '\0';
                return 1;
            }
            else {
                fprintf(stderr, "Error: Option '--pcap' requires a filename value.\n");
                return -1;
            }
        }
        return 0; /* Not a recognized common long option */
    }
    else { /* Short option */
        /* For short options, we parse char by char. This function consumes only ONE such option. */
        /* If -b takes a value, and it's -bVALUE, it's one argv. If -b VALUE, it's two. */
        char opt_char = current_arg[1];
        switch (opt_char) {
        case 'q':
            common_args->quiet = 1;
            if (strlen(current_arg) > 2) { /* Part of a group like -qd */
                /* Caller needs to re-process current_arg+2 if this was part of a group */
                /* This function is simplified to parse one option, group handling is complex */
                /* For now, assume if -q, the whole arg is consumed. */
            }
            *idx_ptr = i + 1;
            return 1;
        case 'd':
            common_args->debug = 1;
            *idx_ptr = i + 1;
            return 1;
        case 'a':
            common_args->ascii_conversion = 1;
            *idx_ptr = i + 1;
            return 1;
        case 'f':
            common_args->flow_control = 0; /* Flag disables it */
            *idx_ptr = i + 1;
            return 1;
        case 'h': /* Help */
            *idx_ptr = i + 1;
            return -2;
        case 'b':
            if (current_arg[2] != '\0') { /* Value attached e.g., -b9600 */
                arg_value = &current_arg[2];
                *idx_ptr = i + 1; /* Consumed current_arg */
            }
            else if (i + 1 < argc && argv[i + 1][0] != '-') { /* Value is next argument */
                arg_value = argv[i + 1];
                *idx_ptr = i + 2; /* Consumed current_arg and its value */
            }
            else {
                fprintf(stderr, "Error: Option '-b' requires a baud rate value.\n");
                return -1;
            }
            errno = 0;
            long rate = strtol(arg_value, &endptr, 10);
            if (endptr == arg_value || *endptr != '\0' || ((rate == LONG_MIN || rate == LONG_MAX) && errno == ERANGE) || errno != 0 || rate <= 0 || rate > INT_MAX) {
                fprintf(stderr, "Error: Invalid baud rate value '%s'.\n", arg_value);
                return -1;
            }
            common_args->baud_rate = (int)rate;
            return 1;
        default:
            return 0; /* Not a recognized common short option char */
        }
    }
    /* return 0; */ /* Should be unreachable if all paths return */
}
