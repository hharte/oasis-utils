/*
 * oasis_sendrecv.c - OASIS Send/Receive packet processing implementation.
 *
 * Reference: OASIS Communications Reference Manual, Mar 1980
 * http://bitsavers.org/pdf/phaseOneSystems/oasis/Communications_Reference_Manual_Mar80.pdf
 * pp. 8 (Packet Format, DLE stuffing, LRC)
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>  /* For fprintf, stderr, perror */
#include <stdint.h>
#include <string.h>
#include <errno.h>  /* For error codes like ETIMEDOUT, EIO, EINVAL, EBADMSG, ENOBUFS */

 /* Local Headers */
#include "oasis.h" /* For DLE, STX, etc., and DEB structure for OPEN */
#include "mm_serial.h"  /* For read_serial, write_serial */

/* Define ssize_t for MSVC if not already defined (e.g., via mm_serial.h) */
#if defined(_MSC_VER) && !defined(_SSIZE_T_DEFINED)
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#define _SSIZE_T_DEFINED 1
#endif

/* Enable debug prints by defining DEBUG_SENDRECV */
/* #define DEBUG_SENDRECV */

/**
 * oasis_packet_decode() - Decode DLE stuffing from received data.
 * @inbuf:  Pointer to the raw input buffer (received packet data, including header).
 * @inlen:  Length of the input buffer.
 * @outbuf: Pointer to the output buffer for decoded data (payload only).
 * @outlen: Pointer to store the length of the decoded data payload.
 *
 * Return: Calculated LRC checksum on success, 0 on checksum mismatch,
 * negative errno code on other errors.
 * Note: Assumes inbuf contains at least header (3 bytes) + LRC (1 byte).
 */
int oasis_packet_decode(uint8_t* inbuf, uint16_t inlen,
    uint8_t* outbuf, uint16_t* outlen)
{
    uint8_t     shift = 0; /* 0x80 if Shift-In (SI) active, 0 otherwise */
    uint16_t    j = 0; /* Index for outbuf */
    uint8_t     dle_arg;
    uint8_t     last_processed_char = 0;
    uint8_t     calc_cksum = 0;
    uint8_t     recv_cksum = 0;
    uint16_t    i;
    const uint16_t max_outbuf_len = 512; /* Max payload size */

    /* Basic validation */
    if (inbuf == NULL || outbuf == NULL || outlen == NULL || inlen < 5) { /* Min: DLE+STX+CMD + DLE+ETX + LRC */
        if (outlen) *outlen = 0;
        return -EINVAL;
    }

    /* Initialize output length */
    *outlen = 0;

    /* Check header */
    if (inbuf[0] != DLE || inbuf[1] != STX) {
        fprintf(stderr, "Error: Invalid packet header 0x%02X 0x%02X (Expected DLE STX).\n", inbuf[0], inbuf[1]);
        return -EBADMSG;
    }

    /* Decode payload starting after the 3-byte header */
    for (i = 3; i < inlen; i++) {
        uint8_t src_data = inbuf[i];

        /* Process DLE (Data Link Escape) sequences */
        if (src_data == DLE) {
            i++;
            if (i >= inlen) { /* Check bounds after increment */
                fprintf(stderr, "Error: Packet ends unexpectedly after DLE at index %u.\n", i - 1);
                return -EBADMSG; /* Bad message */
            }
            dle_arg = inbuf[i];

            switch (dle_arg) {
            case SI: /* Shift in (add 0x80 to the following data) */
                shift = 0x80;
                break;
            case SO: /* Shift out (don't add 0x80 to the following data) */
                shift = 0x00;
                break;
            case DLE: /* DLE+DLE represents a single DLE character */
                last_processed_char = DLE | shift;
                if (j < max_outbuf_len) outbuf[j++] = last_processed_char; else goto overflow;
                break;
            case VT: /* Compression: Repeat last character 'n' times */
            {
                int tablen;
                i++;
                if (i >= inlen) {
                    fprintf(stderr, "Error: Packet ends unexpectedly during VT sequence count at index %u.\n", i);
                    return -EBADMSG;
                }
                tablen = inbuf[i];

                /* Handle escaped tablen */
                if (tablen == DLE) {
                    i++;
                    if (i >= inlen) {
                        fprintf(stderr, "Error: Packet ends unexpectedly during escaped VT sequence at index %u.\n", i);
                        return -EBADMSG;
                    }
                    switch (inbuf[i]) {
                    case DLE: tablen = DLE; break;
                    case CAN: tablen = ESC; break; /* DLE+CAN represents ESC */
                    default:
                        fprintf(stderr, "Error: Unknown escaped tablen 0x%02x after DLE+VT+DLE at index %d.\n", inbuf[i], i);
                        return -EBADMSG;
                    }
                }

                /* Repeat the last character */
                if (j == 0 && tablen > 0) {
                    fprintf(stderr, "Error: VT compression used before any character was output.\n");
                    return -EBADMSG;
                }
                for (int k = 0; k < tablen; k++) {
                    if (j < max_outbuf_len) outbuf[j++] = last_processed_char; else goto overflow;
                }
                break;
            }
            case ETX: /* End of Text marker */
                /* Next byte should be the LRC checksum */
                i++;
                if (i >= inlen) {
                    fprintf(stderr, "Error: Packet ends unexpectedly after DLE+ETX, missing LRC at index %u.\n", i);
                    return -EBADMSG;
                }
                recv_cksum = inbuf[i];

                /* Calculate checksum on the received packet up to DLE+ETX */
                calc_cksum = oasis_lrcc(inbuf, i); /* Pass length including DLE+ETX */

                if (calc_cksum != recv_cksum) {
                    fprintf(stderr, "Error: Checksum mismatch! Calculated=0x%02x, Received=0x%02x\n", calc_cksum, recv_cksum);
#ifdef DEBUG_SENDRECV
                    dump_hex(inbuf, inlen);
#endif
                    * outlen = 0; /* Indicate error */
                    return 0; /* Return 0 specifically for checksum error */
                }
                else {
                    /* Checksum matches, decoding successful */
                    *outlen = j; /* Set the decoded payload length */

                    /* Consume the 0xFF padding that OASIS sends. */
                    i++;

                    /* Check if there's extra data after LRC */
                    if (i + 1 < inlen) {
#ifdef DEBUG_SENDRECV
                        fprintf(stderr, "DEBUG: Extra data found after LRC at index %u.\n", i + 1);
                        dump_hex(inbuf + i + 1, inlen - (i + 1));
#endif
                    }
                    return calc_cksum; /* Return checksum on success */
                }
                /* break; Unreachable after return */
            case CAN: /* DLE+CAN represents a single ESC character */
                last_processed_char = ESC | shift;
                if (j < max_outbuf_len) outbuf[j++] = last_processed_char; else goto overflow;
                break;
            default:
                fprintf(stderr, "Warning: Unknown DLE sequence: DLE + 0x%02x at index %d\n", dle_arg, i);
                return -EBADMSG;
            }
        }
        else { /* Process normal data character */
            last_processed_char = src_data | shift;
            if (j < max_outbuf_len) outbuf[j++] = last_processed_char; else goto overflow;
        }
    } /* End for loop */

    fprintf(stderr, "Error: Reached end of input buffer (%d bytes) without finding DLE+ETX trailer.\n", inlen);
    *outlen = 0;
    return -EBADMSG;

overflow:
    fprintf(stderr, "Error: Decoded output buffer overflow (max %u bytes).\n", max_outbuf_len);
    *outlen = 0;
    return -ENOBUFS;
}

/* Modified function */
int oasis_send_packet(int fd, uint8_t* buf, uint16_t len, uint8_t cmd, FILE* pcapstream)
{
    uint8_t     encoded_buf[1024];
    uint8_t     packet_data[512 + 3] = { 0 }; /* Header (3) + Max Payload (512) */
    uint16_t    encoded_len = 0;
    ssize_t     bytes_written;
    int         saved_errno;

    if (len > 512) {
        fprintf(stderr, "Error: Payload length %u exceeds maximum 512.\n", len);
        return -EINVAL;
    }

#ifdef DEBUG_SENDRECV
    fprintf(stderr, "DEBUG: -> Sending Packet: Cmd='%c', Payload Len=%u\n", cmd, len);
    if (len > 0 && buf != NULL) dump_hex(buf, len);
#endif

    /* Construct the unencoded packet data (DLE STX CMD + payload) */
    packet_data[0] = DLE;
    packet_data[1] = STX;
    packet_data[2] = cmd;

    if (len > 0 && buf != NULL) {
        if (cmd == OPEN && len == sizeof(directory_entry_block_t)) {
            /* Convert DEB to little-endian before putting in packet_data */
            directory_entry_block_t* deb_src = (directory_entry_block_t*)buf;
            directory_entry_block_t deb_le_target;

            deb_le_target.file_format = deb_src->file_format;
            memcpy(deb_le_target.file_name, deb_src->file_name, FNAME_LEN);
            memcpy(deb_le_target.file_type, deb_src->file_type, FTYPE_LEN);
            memcpy(&deb_le_target.timestamp, &deb_src->timestamp, sizeof(oasis_tm_t));
            deb_le_target.owner_id = deb_src->owner_id;
            deb_le_target.shared_from_owner_id = deb_src->shared_from_owner_id;

            /* Correctly convert uint16_t fields to little-endian */
            deb_le_target.record_count = htole16(deb_src->record_count);
            deb_le_target.block_count = htole16(deb_src->block_count);
            deb_le_target.start_sector = htole16(deb_src->start_sector);
            deb_le_target.file_format_dependent1 = htole16(deb_src->file_format_dependent1);
            deb_le_target.file_format_dependent2 = htole16(deb_src->file_format_dependent2);

            memcpy(&packet_data[3], &deb_le_target, sizeof(directory_entry_block_t));
        }
        else {
            memcpy(&packet_data[3], buf, len);
        }
    }

    /* Encode the packet */
#ifdef DEBUG_SENDRECV
    uint8_t cksum =
#endif /* DEBUG_SENDRECV */
        oasis_packet_encode(packet_data, (uint16_t)(len + 3), encoded_buf, &encoded_len);

    if (encoded_len == 0 || encoded_len > sizeof(encoded_buf)) {
        fprintf(stderr, "Error: Packet encoding failed or resulting length %u exceeds buffer %zu.\n",
            encoded_len, sizeof(encoded_buf));
        return 0; /* Return 0 for encoding error, not -1 or errno */
    }

#ifdef DEBUG_SENDRECV
    fprintf(stderr, "DEBUG:    Encoded Len=%u, LRCC=0x%02x\n", encoded_len, cksum);
    dump_hex(encoded_buf, encoded_len);
#endif /* DEBUG_SENDRECV */

    /* Write the encoded packet to serial port */
    bytes_written = write_serial(fd, encoded_buf, encoded_len);

    if (bytes_written < 0) {
        saved_errno = (int)(-bytes_written); /* write_serial returns negative errno */
        errno = saved_errno; /* Set global errno for perror */
        perror("Error: write_serial failed");
        return -saved_errno;
    }
    else if ((size_t)bytes_written != encoded_len) {
        fprintf(stderr, "Error: Partial write to serial port (%zd / %u bytes).\n",
            bytes_written, encoded_len);
        return -EIO; /* Indicate an I/O error for partial write */
    }

    /* Log to PCAP if stream is provided */
    if (pcapstream != NULL) {
        oasis_pcap_add_record(pcapstream, OASIS_PCAP_TX, encoded_buf, encoded_len);
    }

    return (int)encoded_len; /* Success, return bytes written */
}

/* Modified function */
int oasis_send_ack(int fd, int toggle, FILE* pcapstream) {
    uint8_t ack_buf[2];
    ssize_t bytes_written;
    int saved_errno;

    ack_buf[0] = DLE;
    ack_buf[1] = '0' + (toggle & 1);

#ifdef DEBUG_SENDRECV
    fprintf(stderr, "DEBUG: -> Sending ACK %c\n", ack_buf[1]);
#endif

    bytes_written = write_serial(fd, ack_buf, 2);

    if (bytes_written < 0) {
        saved_errno = (int)(-bytes_written);
        errno = saved_errno;
        perror("Error writing ACK to serial port");
        return -saved_errno;
    }
    else if (bytes_written != 2) {
        fprintf(stderr, "Error: Partial ACK write (%zd / 2 bytes).\n", bytes_written);
        return -EIO;
    }

    /* Log to PCAP if stream is provided */
    if (pcapstream != NULL) {
        oasis_pcap_add_record(pcapstream, OASIS_PCAP_TX, ack_buf, 2);
    }
    return 0; /* Success */
}

/* Modified function */
int oasis_receive_ack(int fd, int expected_toggle, FILE* pcapstream) {
    uint8_t buf[2];
    int retries = 5; /* Or some configurable value */
    ssize_t bytes_read;
    int received_toggle;
    int saved_errno;
    int result = ACK_TIMEOUT; /* Default to timeout */

    while (retries-- > 0) {
        bytes_read = read_serial(fd, buf, 2);

        if (bytes_read == 2) {
            if (pcapstream != NULL) {
                oasis_pcap_add_record(pcapstream, OASIS_PCAP_RX, buf, 2);
            }
            if (buf[0] == DLE && (buf[1] == '0' || buf[1] == '1')) {
                received_toggle = buf[1] & 1;
                if (received_toggle == (expected_toggle & 1)) {
                    result = ACK_OK;
                }
                else {
                    result = ACK_WRONG_TOGGLE;
                }
                goto exit_receive_ack; /* Break from loop and function */
            }
            else {
                result = ACK_INVALID;
                goto exit_receive_ack; /* Break from loop and function */
            }
        }
        else if (bytes_read == 0 || (bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT))) {
            /* Timeout or non-blocking read with no data, continue retry */
            result = ACK_TIMEOUT; /* Keep reflecting timeout unless a read error occurs */
#ifdef DEBUG_SENDRECV
            if (bytes_read == 0) fprintf(stderr, "DEBUG: <- ACK Wait: Read 0 bytes (Retry %d).\n", retries);
            else fprintf(stderr, "DEBUG: <- ACK Wait: Read timeout/would block (errno=%d, Retry %d).\n", errno, retries);
#endif
        }
        else { /* bytes_read < 0 and not a timeout/EAGAIN */
            saved_errno = (int)(-bytes_read); /* read_serial returns negative errno */
            errno = saved_errno;
            perror("Error reading ACK from serial port");
            result = -saved_errno; /* Propagate the actual error code */
            goto exit_receive_ack; /* Break from loop and function */
        }
    }

exit_receive_ack:
#ifdef DEBUG_SENDRECV
    if (result == ACK_OK) fprintf(stderr, "DEBUG: <- ACK Received OK (Toggle=%c, Expected=%c)\n", '0' + (expected_toggle & 1), '0' + (expected_toggle & 1));
    else if (result == ACK_WRONG_TOGGLE) fprintf(stderr, "DEBUG: <- ACK Received WRONG TOGGLE (Toggle=%c, Expected=%c)\n", '0' + ((expected_toggle ^ 1) & 1), '0' + (expected_toggle & 1));
    else if (result == ACK_TIMEOUT) fprintf(stderr, "DEBUG: <- ACK Timeout.\n");
    else if (result == ACK_INVALID) fprintf(stderr, "DEBUG: <- Received invalid ACK sequence.\n");
    else fprintf(stderr, "DEBUG: <- ACK Read Error (%d).\n", result);
#endif
    return result;
}

uint8_t oasis_packet_encode(uint8_t* inbuf, uint16_t inlen,
    uint8_t* outbuf, uint16_t* outlen)
{
    uint8_t     shift = 0;
    uint16_t    j = 0;
    uint8_t     cksum = 0;
    uint16_t    i;
    const uint16_t max_outbuf_len = 1024;

    if (!inbuf || !outbuf || !outlen || inlen < 3) {
        if (outlen) *outlen = 0;
        return 0;
    }
    if (inbuf[0] != DLE || inbuf[1] != STX) {
        fprintf(stderr, "Warning: Encoding packet with unexpected header 0x%02X 0x%02X.\n", inbuf[0], inbuf[1]);
    }

    outbuf[0] = inbuf[0];
    outbuf[1] = inbuf[1];
    outbuf[2] = inbuf[2];
    j = 3;

    for (i = 3; i < inlen; i++) {
        uint8_t src_data = inbuf[i];
        uint8_t data_to_write;

        if (i + 3 < inlen &&
            src_data == inbuf[i + 1] &&
            src_data == inbuf[i + 2] &&
            src_data == inbuf[i + 3]) {
            uint8_t run_char = src_data;
            int run_length = 0;

            while (i < inlen && inbuf[i] == run_char) {
                run_length++;
                i++;
            }
            i--;

            data_to_write = run_char;
            if ((data_to_write & 0x80) != shift) {
                if (j + 2 > max_outbuf_len) goto encode_overflow;
                outbuf[j++] = DLE;
                shift = (shift == 0) ? 0x80 : 0x00;
                outbuf[j++] = (shift == 0x80) ? SI : SO;
            }
            uint8_t char_masked = data_to_write & 0x7F;
            if (char_masked == DLE) {
                if (j + 2 > max_outbuf_len) goto encode_overflow;
                outbuf[j++] = DLE;
                outbuf[j++] = DLE;
            }
            else if (char_masked == ESC) {
                if (j + 2 > max_outbuf_len) goto encode_overflow;
                outbuf[j++] = DLE;
                outbuf[j++] = CAN;
            }
            else {
                if (j + 1 > max_outbuf_len) goto encode_overflow;
                outbuf[j++] = char_masked;
            }

            run_length--;
            while (run_length > 0) {
                int current_run = (run_length > RUN_LENGTH_MAX) ? RUN_LENGTH_MAX : run_length;
                if (j + 2 > max_outbuf_len) goto encode_overflow;
                outbuf[j++] = DLE;
                outbuf[j++] = VT;

                if (current_run == DLE) {
                    if (j + 2 > max_outbuf_len) goto encode_overflow;
                    outbuf[j++] = DLE;
                    outbuf[j++] = DLE;
                }
                else if (current_run == ESC) {
                    if (j + 2 > max_outbuf_len) goto encode_overflow;
                    outbuf[j++] = DLE;
                    outbuf[j++] = CAN;
                }
                else {
                    if (j + 1 > max_outbuf_len) goto encode_overflow;
                    outbuf[j++] = (uint8_t)current_run;
                }
                run_length -= current_run;
            }
            continue;
        }

        data_to_write = src_data;
        if ((data_to_write & 0x80) != shift) {
            if (j + 2 > max_outbuf_len) goto encode_overflow;
            outbuf[j++] = DLE;
            shift = (shift == 0) ? 0x80 : 0x00;
            outbuf[j++] = (shift == 0x80) ? SI : SO;
        }
        uint8_t char_masked = data_to_write & 0x7F;
        if (char_masked == DLE) {
            if (j + 2 > max_outbuf_len) goto encode_overflow;
            outbuf[j++] = DLE;
            outbuf[j++] = DLE;
        }
        else if (char_masked == ESC) {
            if (j + 2 > max_outbuf_len) goto encode_overflow;
            outbuf[j++] = DLE;
            outbuf[j++] = CAN;
        }
        else {
            if (j + 1 > max_outbuf_len) goto encode_overflow;
            outbuf[j++] = char_masked;
        }
    }

    if (j + 2 > max_outbuf_len) goto encode_overflow;
    outbuf[j++] = DLE;
    outbuf[j++] = ETX;

    cksum = oasis_lrcc(outbuf, j);
    if (j + 1 > max_outbuf_len) goto encode_overflow;
    outbuf[j++] = cksum;

    if (j + 1 > max_outbuf_len) goto encode_overflow;
    outbuf[j++] = RUB;

    *outlen = j;
    return cksum;

encode_overflow:
    fprintf(stderr, "Error: Encoded output buffer overflow (max %u bytes).\n", max_outbuf_len);
    *outlen = 0;
    return 0;
}

uint8_t oasis_lrcc(uint8_t* buf, uint16_t len)
{
    uint8_t lrcc = 0;
    uint16_t i;

    if (!buf && len > 0) return 0; /* Guard against null buf with positive length */
    /* If buf is NULL and len is 0, it will proceed to return (0 | 0xC0) & 0x7F = 0x40 */

    for (i = 0; i < len; i++) {
        lrcc += buf[i];
    }
    lrcc |= 0xC0;
    lrcc &= 0x7F;
    return lrcc;
}
