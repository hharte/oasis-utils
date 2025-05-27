/*
 * OASIS Packet Capture (PCAP) Implementation
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdint.h>
#include <time.h>   /* For timespec_get, TIME_UTC (Requires C11) */
#include <stdlib.h>
#include <string.h> /* For memset, strerror */
#include <errno.h>

#include "oasis.h"

/* Check if timespec_get is available (C11 standard) */
#ifndef TIME_UTC
#warning "TIME_UTC or timespec_get potentially not supported by this compiler/library."
/* Provide a fallback or error if C11 time functions are unavailable */
#endif

/* Define a maximum size for the temporary buffer used for masking */
/* Should be large enough to hold the largest expected raw packet fragment */
#define MAX_MASK_BUFFER_SIZE 1024

/**
 * oasis_pcap_create() - Creates and initializes a PCAP file for OASIS comms.
 * @filename: Path to the PCAP file to be created.
 * @pcapstream_ptr: Pointer to a FILE* which will be set to the opened stream.
 *
 * Opens the specified file in binary write mode, writes the global PCAP
 * header with LINKTYPE_USER2, and returns a file stream pointer.
 *
 * Return: 0 on success, -1 on error (e.g., file cannot be opened or
 * header write fails). On error, *pcapstream_ptr is set to NULL.
 */
	int oasis_pcap_create(const char* filename, FILE** pcapstream_ptr)
{
	pcap_hdr_t pcap_hdr;
	FILE* stream = NULL;

	/* Defensive checks */
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
	pcap_hdr.magic_number = 0xa1b2c3d4; /* Standard magic for big-endian */
	pcap_hdr.version_major = 2;
	pcap_hdr.version_minor = 4;
	pcap_hdr.thiszone = 0;     /* GMT timezone */
	pcap_hdr.sigfigs = 0;     /* No specification on timestamp accuracy */
	pcap_hdr.snaplen = 65535; /* Max packet length */
	pcap_hdr.network = 149;   /* LINKTYPE_USER2 */

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
 * oasis_pcap_add_record() - Adds a raw data record to the PCAP file.
 * @pcapstream: The FILE stream of the open PCAP file. Can be NULL (noop).
 * @direction:  The direction of the data (OASIS_PCAP_RX or OASIS_PCAP_TX).
 * @data:       Pointer to the raw data buffer (bytes sent/received).
 * @len:        Length of the data in bytes.
 *
 * Writes a PCAP record header, a direction byte, and the raw data (masked
 * to 7 bits) to the specified file stream. Captures the current UTC time
 * for the timestamp.
 *
 * Return: 0 on success, -1 on error (e.g., write failure, time error,
 * data too long for internal buffer). Returns 0 immediately if pcapstream is NULL.
 */
int oasis_pcap_add_record(FILE* pcapstream, int direction,
	const uint8_t* data, uint16_t len)
{
	pcaprec_hdr_t pcap_rec;
	struct timespec ts;
	uint32_t ts_sec;
	uint32_t ts_usec;
	uint8_t dir_byte;
	uint16_t i; /* Loop counter */

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

	/* Get current time */
#ifdef TIME_UTC
	if (timespec_get(&ts, TIME_UTC) != TIME_UTC) {
		/* timespec_get is C11. Check compiler support if this fails. */
		fputs("timespec_get failed!\n", stderr);
		return -1;
	}
	ts_sec = (uint32_t)ts.tv_sec;
	ts_usec = (uint32_t)(ts.tv_nsec / 1000); /* Convert nanoseconds to microseconds */
#else
	/* Fallback if timespec_get is not available */
	fprintf(stderr, "%s: Error: Cannot get timestamp (timespec_get unavailable).\n", __func__);
	/* Consider using platform-specific time functions if needed */
	/* For simplicity, use a zero timestamp as fallback */
	ts_sec = 0;
	ts_usec = 0;
	/* return -1; // Or return error if timestamp is critical */
#endif

	/* Prepare the PCAP record header */
	memset(&pcap_rec, 0, sizeof(pcap_rec));
	pcap_rec.ts_sec = ts_sec;
	pcap_rec.ts_usec = ts_usec;
	/* Length includes the direction byte + actual data length */
	pcap_rec.incl_len = (uint32_t)len + 1;
	pcap_rec.orig_len = (uint32_t)len + 1; /* Assuming we capture the full "packet" */

	/* Write PCAP record header */
	if (fwrite(&pcap_rec, sizeof(pcaprec_hdr_t), 1, pcapstream) != 1) {
		fprintf(stderr, "%s: Error writing PCAP record header.\n", __func__);
		return -1;
	}

	/* Write the direction byte */
	dir_byte = (uint8_t)direction;
	if (fwrite(&dir_byte, sizeof(uint8_t), 1, pcapstream) != 1) {
		fprintf(stderr, "%s: Error writing direction byte.\n", __func__);
		return -1;
	}

	/* Write the actual data payload, masked to 7 bits */
	if (len > 0) {
		uint8_t masked_data[MAX_MASK_BUFFER_SIZE]; /* Temp buffer on stack */

		/* Check if data exceeds our temporary buffer */
		if (len > MAX_MASK_BUFFER_SIZE) {
			fprintf(stderr, "%s: Error: Data length (%u) exceeds internal mask buffer size (%d).\n",
				__func__, len, MAX_MASK_BUFFER_SIZE);
			/* File is likely corrupt now as header/direction were written */
			return -1;
		}

		/* Copy data to temporary buffer, applying 7-bit mask */
		for (i = 0; i < len; i++) {
			masked_data[i] = data[i] & 0x7F;
		}

		/* Write the masked data */
		if (fwrite(masked_data, (size_t)len, 1, pcapstream) != 1) {
			fprintf(stderr, "%s: Error writing masked packet data payload.\n", __func__);
			/* Note: Record header and direction byte are already written. */
			/* File might be left in an inconsistent state. */
			return -1;
		}
	}

	/* Flush the stream buffer (optional, but good for real-time capture) */
	/* fflush(pcapstream); */

	return 0; /* Success */
}

/**
 * oasis_pcap_close() - Closes the PCAP file stream.
 * @pcapstream: The FILE stream to close. Can be NULL.
 *
 * Closes the file stream associated with the PCAP file.
 *
 * Return: 0 on success, EOF on error (as returned by fclose).
 */
int oasis_pcap_close(FILE* pcapstream)
{
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
