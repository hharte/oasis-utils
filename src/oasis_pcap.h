/*
 * oasis_pcap.h - OASIS Protocol Packet Capture (PCAP) Utilities Interface
 *
 * This header file declares structures and functions for creating and writing
 * PCAP files specifically for logging OASIS serial communication data.
 * It uses LINKTYPE_RTAC_SERIAL (250) for the PCAP link-layer header type.
 * Part of the oasis-utils library.
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#ifndef OASIS_PCAP_H_
#define OASIS_PCAP_H_

#include <stdint.h> /* Required for uint32_t, uint16_t, int32_t */
#include <stdio.h>  /* Required for FILE */

/* Link Type for RTAC_SERIAL */
#define LINKTYPE_RTAC_SERIAL 250

/* PCAP Global Header structure */
/* Defines the format of the global header at the beginning of a PCAP file. */
struct pcap_hdr {
    uint32_t magic_number;   /* Magic number (0xa1b2c3d4 for standard PCAP). */
    uint16_t version_major;  /* Major version number of the PCAP format (typically 2). */
    uint16_t version_minor;  /* Minor version number of the PCAP format (typically 4). */
    int32_t  thiszone;       /* GMT to local correction (usually 0). */
    uint32_t sigfigs;        /* Accuracy of timestamps (usually 0). */
    uint32_t snaplen;        /* Max length of captured packets, in octets (e.g., 65535). */
    uint32_t network;        /* Data link type (LINKTYPE_RTAC_SERIAL for this application). */
};

/* PCAP Record Header structure */
/* Defines the format of the header preceding each captured packet in a PCAP file. */
struct pcaprec_hdr {
    uint32_t ts_sec;         /* Timestamp seconds since epoch. */
    uint32_t ts_usec;        /* Timestamp microseconds. */
    uint32_t incl_len;       /* Number of octets of packet saved in file. */
    uint32_t orig_len;       /* Actual length of packet. */
};

/*
 * RTAC_SERIAL pseudo-header structure (rtacser_hdr_t)
 * This is the specific pseudo-header format used with LINKTYPE_RTAC_SERIAL.
 * Reference: https://www.tcpdump.org/linktypes/LINKTYPE_RTAC_SERIAL.html
 */
struct rtacser_hdr {
    uint32_t ts_sec;             /* Timestamp seconds since epoch. (Big Endian) */
    uint32_t ts_usec;            /* Timestamp microseconds. (Big Endian) */
    uint8_t  event_type;         /* Event Type */
    uint8_t  control_line_state; /* Bit masks for RS-232 serial control lines */
    uint8_t  footer[2];          /* Reserved for future use, set to 0. */
};

/* Ensure structures are packed without padding for correct file format */
#pragma pack(push, 1)
typedef struct pcap_hdr pcap_hdr_t;
typedef struct pcaprec_hdr pcaprec_hdr_t;
typedef struct rtacser_hdr rtacser_hdr_t; /* Typedef for the RTAC_SERIAL pseudo-header */
#pragma pack(pop)

/* Constants for the direction input to oasis_pcap_add_record, used to map to rtacser_hdr.event_type */
#define OASIS_PCAP_RX 0x00 /**< Indicates data received from the serial port. */
#define OASIS_PCAP_TX 0x01 /**< Indicates data transmitted to the serial port. */

/* Define a maximum size for the temporary buffer used for masking payload data */
#define MAX_MASK_BUFFER_SIZE 1024

#ifdef __cplusplus
extern "C" {
#endif

    /**
     * @brief Creates and initializes a PCAP file for OASIS communication logging.
     *
     * Opens the specified file in binary write mode ("wb"), writes the global PCAP
     * header configured for LINKTYPE_RTAC_SERIAL (250), and returns a file stream pointer.
     *
     * @param filename Path to the PCAP file to be created. If the file exists, it will be overwritten.
     * @param pcapstream_ptr Pointer to a `FILE*` variable. On success, this will be updated
     * to point to the opened file stream. On error, it's set to NULL.
     * @return 0 on success.
     * -1 on error (e.g., `filename` or `pcapstream_ptr` is NULL, file cannot be opened,
     * or header write fails). Error messages are printed to stderr.
     */
    int oasis_pcap_create(const char* filename, FILE** pcapstream_ptr);

    /**
     * @brief Adds a raw data record to the open PCAP file using the RTAC_SERIAL pseudo-header.
     *
     * This function captures the current UTC time for the pcap record timestamp, which is also
     * used for the `rtacser_hdr_t` timestamp fields. It then writes a PCAP record header,
     * followed by the `rtacser_hdr_t` pseudo-header, and then the provided raw OASIS
     * message data. The raw data bytes are masked to 7 bits (`& 0x7F`) before being written.
     *
     * @param pcapstream The `FILE` stream of the open PCAP file. If NULL, the function
     * is a no-op and returns 0.
     * @param direction The direction of the OASIS data: `OASIS_PCAP_RX` (0x00) for received,
     * or `OASIS_PCAP_TX` (0x01) for transmitted. This determines the
     * `event_type` field in the `rtacser_hdr_t`.
     * @param data Pointer to the raw OASIS message data.
     * @param len Length of the OASIS message data in `data`, in bytes.
     *
     * @return 0 on success.
     * -1 on error (e.g., invalid arguments, time retrieval failure, write failure,
     * or if `len` exceeds internal buffer limits for masking). Error messages
     * are printed to stderr.
     */
    int oasis_pcap_add_record(FILE* pcapstream, int direction,
        const uint8_t* data, uint16_t len);

    /**
     * @brief Closes the PCAP file stream.
     *
     * @param pcapstream The `FILE` stream to close. If NULL, the function is a no-op.
     * @return 0 on successful close.
     * EOF on error (as returned by `fclose`). An error message is printed to stderr.
     */
    int oasis_pcap_close(FILE* pcapstream);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OASIS_PCAP_H_ */
