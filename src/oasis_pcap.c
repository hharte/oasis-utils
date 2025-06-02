/*
 * OASIS Packet Capture (PCAP) Implementation
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdint.h>
#include <time.h>   /* For timespec_get (C11), time_t, time (fallback) */
#include <stdlib.h>
#include <string.h> /* For memset, strerror */
#include <errno.h>

#include "oasis_pcap.h" /* Updated to include rtacser_hdr definitions */
#include "oasis_endian.h"

 /* oasis.h is not directly needed here anymore unless OASIS_PROTOCOL_VERSION was used
    for something other than the old rtac_proto, which it isn't. */

    /**
     * @brief Creates and initializes a PCAP file.
     * @param filename Path to the PCAP file to be created.
     * @param pcapstream_ptr Pointer to a FILE* which will be set to the opened stream.
     *
     * Opens the specified file in binary write mode, writes the global PCAP
     * header with LINKTYPE_RTAC_SERIAL, and returns a file stream pointer.
     *
     * @return 0 on success, -1 on error.
     */
int oasis_pcap_create(const char* filename, FILE** pcapstream_ptr) {
    pcap_hdr_t pcap_hdr;
    FILE* stream = NULL;

    /* Check for NULL pcapstream_ptr first */
    if (!pcapstream_ptr) {
        fprintf(stderr, "%s: Error: pcapstream_ptr is NULL.\n", __func__);
        return -1;
    }
    *pcapstream_ptr = NULL; /* Initialize output parameter */

    if (!filename) {
        fprintf(stderr, "%s: Error: filename is NULL.\n", __func__);
        return -1;
    }

    /* Open the capture file in binary write mode */
    stream = fopen(filename, "wb");
    if (!stream) {
        int saved_errno = errno;
        fprintf(stderr, "%s: Error opening file '%s': %s\n",
            __func__, filename, strerror(saved_errno));
        return -1;
    }

    /* Initialize the PCAP global header */
    memset(&pcap_hdr, 0, sizeof(pcap_hdr));
    pcap_hdr.magic_number = 0xa1b2c3d4; /* Standard magic for pcap files */
    pcap_hdr.version_major = 2;
    pcap_hdr.version_minor = 4;
    pcap_hdr.thiszone = 0;     /* GMT timezone */
    pcap_hdr.sigfigs = 0;     /* Accuracy of timestamps */
    pcap_hdr.snaplen = 65535; /* Max length of captured packets */
    pcap_hdr.network = LINKTYPE_RTAC_SERIAL; /* Set to RTAC_SERIAL link type */

    /* Write the global header to the file */
    if (fwrite(&pcap_hdr, sizeof(pcap_hdr_t), 1, stream) != 1) {
        fprintf(stderr, "%s: Error writing PCAP header to '%s'.\n", __func__, filename);
        fclose(stream); /* Clean up */
        return -1;
    }

    /* Success: Assign the stream to the output parameter */
    *pcapstream_ptr = stream;

    return 0; /* Success */
}

/**
 * @brief Adds a raw data record to the PCAP file using the RTAC_SERIAL pseudo-header.
 * @param pcapstream The FILE stream of the open PCAP file.
 * @param direction  The direction of the data (OASIS_PCAP_RX or OASIS_PCAP_TX).
 * @param data       Pointer to the raw OASIS message data.
 * @param len        Length of the OASIS message data in bytes.
 *
 * Writes a PCAP record header, an rtacser_hdr_t pseudo-header (timestamp, event_type,
 * control_line_state, footer), and the raw OASIS message data (masked to 7 bits)
 * to the specified file stream. Captures the current UTC time for the pcap record timestamp,
 * which is also used for the rtacser_hdr's timestamp fields.
 *
 * @return 0 on success, -1 on error.
 */
int oasis_pcap_add_record(FILE* pcapstream, int direction,
    const uint8_t* data, uint16_t len) {
    pcaprec_hdr_t pcap_rec;
    rtacser_hdr_t rtac_serial_header; /* Use the new rtacser_hdr structure */
    struct timespec ts; /* C11 standard */
    uint32_t ts_sec_val;
    uint32_t ts_usec_val;

    /* If pcap stream is not open, do nothing */
    if (pcapstream == NULL) {
        return 0;
    }

    /* Basic validation */
    if (data == NULL && len > 0) {
        fprintf(stderr, "%s: Error: data is NULL but len is %u.\n", __func__, len);
        return -1;
    }
    if (direction != OASIS_PCAP_RX && direction != OASIS_PCAP_TX) {
        fprintf(stderr, "%s: Error: Invalid direction %d.\n", __func__, direction);
        return -1;
    }

    /* Get current time using timespec_get (C11) */
#if defined(TIME_UTC) && __STDC_VERSION__ >= 201112L
    if (timespec_get(&ts, TIME_UTC) != TIME_UTC) {
        fprintf(stderr, "%s: Error: timespec_get failed.\n", __func__);
        return -1;
    }
    ts_sec_val = (uint32_t)ts.tv_sec;
    ts_usec_val = (uint32_t)(ts.tv_nsec / 1000); /* Convert nanoseconds to microseconds */
#else
    /* Fallback for older compilers/systems or if TIME_UTC is not defined */
    time_t current_time_fallback;
    current_time_fallback = time(NULL);
    if (current_time_fallback == (time_t)-1) {
        fprintf(stderr, "%s: Error: time(NULL) failed for fallback timestamp.\n", __func__);
        return -1;
    }
    ts_sec_val = (uint32_t)current_time_fallback;
    ts_usec_val = 0; /* Microseconds not available with this simple fallback */
#endif

    /* Prepare the PCAP record header */
    memset(&pcap_rec, 0, sizeof(pcap_rec));
    pcap_rec.ts_sec = ts_sec_val;
    pcap_rec.ts_usec = ts_usec_val;
    /* Length includes the rtacser_hdr_t pseudo-header + actual OASIS data length */
    pcap_rec.incl_len = (uint32_t)len + sizeof(rtacser_hdr_t);
    pcap_rec.orig_len = pcap_rec.incl_len; /* Assuming full packet is captured */

    /* Write PCAP record header */
    if (fwrite(&pcap_rec, sizeof(pcaprec_hdr_t), 1, pcapstream) != 1) {
        fprintf(stderr, "%s: Error writing PCAP record header.\n", __func__);
        return -1;
    }

    /* Prepare the rtacser_hdr_t pseudo-header */
    memset(&rtac_serial_header, 0, sizeof(rtacser_hdr_t));
    rtac_serial_header.ts_sec = htobe32(ts_sec_val);   /* Use the same timestamp as pcap record (Big Endian) */
    rtac_serial_header.ts_usec = htobe32(ts_usec_val); /* Use the same timestamp as pcap record (Big Endian) */

    /* Map OASIS_PCAP_TX/RX to RTACSER event types */
    /* For simplicity, mapping TX to DATA_TX_START and RX to DATA_RX_START */
    /* A more complex implementation might differentiate START/END if that info is available */
    if (direction == OASIS_PCAP_TX) {
        rtac_serial_header.event_type = 0x01; /* DATA_TX_START */
    }
    else { /* OASIS_PCAP_RX */
        rtac_serial_header.event_type = 0x02; /* DATA_RX_START */
    }

    rtac_serial_header.control_line_state = 0x00; /* Not currently tracked/logged */
    /* rtac_serial_header.footer is already zeroed by memset */


    /* Write the rtacser_hdr_t pseudo-header */
    if (fwrite(&rtac_serial_header, sizeof(rtacser_hdr_t), 1, pcapstream) != 1) {
        fprintf(stderr, "%s: Error writing RTAC_SERIAL pseudo-header.\n", __func__);
        return -1;
    }

    /* Write the actual OASIS message data payload, masked to 7 bits */
    if (len > 0) {
        uint8_t masked_data_payload_buf[MAX_MASK_BUFFER_SIZE]; /* Temporary buffer on stack */
        uint16_t k; /* C-style loop iterator */

        /* Check if data exceeds our temporary buffer for masking */
        if (len > MAX_MASK_BUFFER_SIZE) {
            fprintf(stderr, "%s: Error: Data length (%u) exceeds internal mask buffer size (%d).\n",
                __func__, len, MAX_MASK_BUFFER_SIZE);
            return -1;
        }

        /* Copy data to temporary buffer, applying 7-bit mask */
        for (k = 0; k < len; k++) {
            masked_data_payload_buf[k] = data[k] & 0x7F;
        }

        /* Write the masked data */
        if (fwrite(masked_data_payload_buf, (size_t)len, 1, pcapstream) != 1) {
            fprintf(stderr, "%s: Error writing masked OASIS message data payload.\n", __func__);
            return -1;
        }
    }

    /* Flush the stream buffer */
    fflush(pcapstream);

    return 0; /* Success */
}

/**
 * @brief Closes the PCAP file stream.
 * @param pcapstream The FILE stream to close. Can be NULL.
 *
 * Closes the file stream associated with the PCAP file.
 *
 * @return 0 on success, EOF on error (as returned by fclose).
 */
int oasis_pcap_close(FILE* pcapstream) {
    int ret = 0;

    /* Close .pcap file if the stream is valid */
    if (pcapstream != NULL) {
        if (fclose(pcapstream) == EOF) {
            int saved_errno = errno;
            fprintf(stderr, "%s: Error closing pcap file stream: %s\n",
                __func__, strerror(saved_errno));
            ret = EOF; /* Return EOF on error */
        }
    }
    return ret; /* 0 on success, EOF on failure */
}
