/*
 * oasis_sendrecv.h - OASIS Serial Transfer Protocol Definitions and Utilities Interface
 *
 * This header file defines constants, structures, and function prototypes
 * related to the OASIS serial communication protocol used for file transfer.
 * It covers packet encoding/decoding, LRC calculation, and ACK/NAK handling.
 * Part of the oasis-utils library.
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 *
 * Reference: OASIS Communications Reference Manual, Mar 1980, pp. 8 (Packet Format)
 */

#ifndef OASIS_SENDRECV_H_
#define OASIS_SENDRECV_H_

#include <stdint.h> /* For uint8_t, uint16_t */
#include <stdio.h>  /* For FILE (used in PCAP wrapper declarations) */
#include <errno.h>  /* For error constants like ETIMEDOUT, EIO, EBADMSG */

#ifdef __cplusplus
extern "C" {
#endif

    /* OASIS Send/Receive Command Types (used in the Record Type field of a packet) */
#define OPEN  'O' /**< Command to open a file for transfer. Payload typically contains DEB. */
#define WRITE 'W' /**< Command to write a data segment. Payload contains file data. */
#define CLOSE 'C' /**< Command to close the currently open file. Payload is usually empty. */

/** @brief Maximum number of characters to repeat in a single Run-Length Encoding (RLE) sequence (DLE VT count). */
#define RUN_LENGTH_MAX 127

/* Return codes for oasis_receive_ack and its PCAP wrapper */
#define ACK_OK            0         /**< Correct ACK (DLE + '0' or '1' matching expected toggle) received. */
#define ACK_WRONG_TOGGLE -1         /**< ACK received, but with the wrong toggle bit. */
#define ACK_TIMEOUT      -ETIMEDOUT /**< Timeout occurred while waiting for an ACK. */
#define ACK_READ_ERROR   -EIO       /**< An error occurred while reading from the serial port. */
#define ACK_INVALID      -EBADMSG   /**< An invalid or unexpected sequence was received instead of an ACK. */

/** @brief Standard transfer block size for data in WRITE packets.
 * For sequential files, the last 2 bytes of this block are used for the next sector link.
 */
#define XFR_BLOCK_SIZE  (256)


 /*
  * Note: The original `oasis_packet_header_t` and `oasis_packet_trailer_t` structs
  * are not strictly necessary for the library's public interface as packet
  * construction/deconstruction is handled internally by the functions.
  * They are omitted here for brevity as they are implementation details of
  * oasis_sendrecv.c rather than part of the user-facing API of this header.
  */

  /* --- Function Prototypes --- */

  /**
   * @brief Decodes a raw received OASIS packet, handling DLE stuffing and RLE compression.
   *
   * Takes a raw input buffer (`inbuf`) containing a DLE STX framed packet,
   * decodes its payload by removing DLE stuffing and expanding RLE sequences,
   * and places the result into `outbuf`. It also verifies the LRC checksum.
   *
   * @param inbuf Pointer to the raw input buffer (received packet data, including DLE STX header
   * and DLE ETX LRC RUB trailer).
   * @param inlen Length of the data in `inbuf`. Must be at least 7 bytes for a minimal valid data packet.
   * @param outbuf Pointer to the output buffer where the decoded payload data will be stored.
   * The buffer should be large enough for the maximum expected decoded payload (typically 512 bytes).
   * @param outlen Pointer to a `uint16_t` where the length of the decoded data in `outbuf` will be stored.
   * Set to 0 on checksum mismatch or other errors.
   *
   * @return The calculated LRC checksum (as a positive integer, typically uint8_t range) on successful
   * decoding and checksum match.
   * 0 if the checksum mismatched (specific return for this error).
   * A negative errno code on other errors:
   * -EINVAL for NULL pointers or `inlen` too short.
   * -EBADMSG for malformed packets (e.g., invalid header, unexpected end, bad DLE sequence).
   * -ENOBUFS if the decoded payload would exceed internal limits (max 512 bytes for `outbuf`).
   */
    extern int oasis_packet_decode(uint8_t* inbuf, uint16_t inlen,
        uint8_t* outbuf, uint16_t* outlen);

    /**
     * @brief Encodes a data payload into an OASIS packet and sends it over the serial port.
     *
     * Constructs a full OASIS packet: DLE STX CMD [payload with DLE stuffing/RLE] DLE ETX LRC RUB.
     * The payload is processed for DLE stuffing and RLE compression.
     * The LRC is calculated over the packet data up to and including DLE ETX.
     * If `pcapstream` is not NULL, the sent packet is logged.
     *
     * @param fd File descriptor for the open serial port.
     * @param buf Pointer to the data payload to send. Can be NULL if `len` is 0.
     * @param len Length of the data payload in `buf`. Maximum 512 bytes.
     * @param cmd Command/Record Type character (e.g., OPEN, WRITE, CLOSE).
     * @param pcapstream Optional `FILE` stream for PCAP logging. If NULL, no logging occurs.
     *
     * @return The total number of bytes written to the serial port (i.e., the length of the
     * fully encoded packet including headers, trailer, LRC, and RUB) on success.
     * 0 if packet encoding fails (e.g., payload too long, internal buffer overflow during encoding).
     * A negative errno code on serial write error (e.g., -EIO).
     */
    extern int oasis_send_packet(int fd, uint8_t* buf, uint16_t len, uint8_t cmd, FILE* pcapstream);

    /**
     * @brief Sends an ACK (DLE + '0' or DLE + '1') packet over the serial port.
     * If `pcapstream` is not NULL, the sent ACK is logged.
     *
     * @param fd File descriptor for the open serial port.
     * @param toggle The current toggle state (0 or 1). The ACK sent will be DLE + character '0' or '1'
     * corresponding to this toggle state.
     * @param pcapstream Optional `FILE` stream for PCAP logging. If NULL, no logging occurs.
     * @return 0 on success.
     * A negative errno code on serial write error (e.g., -EIO).
     */
    extern int oasis_send_ack(int fd, int toggle, FILE* pcapstream);

    /**
     * @brief Waits to receive an ACK (DLE + '0' or DLE + '1') response from the serial port.
     * If `pcapstream` is not NULL, the received ACK (or invalid sequence) is logged.
     *
     * Attempts to read a 2-byte ACK sequence. Retries internally on timeouts for a
     * limited number of times.
     *
     * @param fd File descriptor for the open serial port.
     * @param expected_toggle The expected toggle bit (0 or 1) in the received ACK.
     * The function compares the received ACK's toggle with this value.
     * @param pcapstream Optional `FILE` stream for PCAP logging. If NULL, no logging occurs.
     *
     * @return `ACK_OK` (0) if an ACK with the `expected_toggle` is received.
     * `ACK_WRONG_TOGGLE` (-1) if an ACK is received but with the incorrect toggle bit.
     * `ACK_TIMEOUT` (-ETIMEDOUT) if no valid ACK is received after retries.
     * `ACK_READ_ERROR` (-EIO or other negative errno) on a serial read error.
     * `ACK_INVALID` (-EBADMSG) if an invalid or unexpected sequence is received
     * instead of a DLE-prefixed ACK.
     */
    extern int oasis_receive_ack(int fd, int expected_toggle, FILE* pcapstream);

    /**
     * @brief Encodes raw data (header + payload) into a fully formed OASIS packet buffer.
     *
     * This function takes the initial packet parts (DLE STX CMD + payload), performs
     * DLE stuffing and RLE compression on the payload, and appends the DLE ETX trailer,
     * calculated LRC checksum, and final RUB padding byte.
     *
     * @param inbuf Pointer to the raw data to encode. This buffer should contain the
     * 3-byte header (DLE, STX, CMD) followed by the payload data.
     * @param inlen Length of the data in `inbuf` (i.e., 3 + payload_length).
     * @param outbuf Pointer to the output buffer where the fully encoded packet will be stored.
     * This buffer should be large enough (e.g., 1024 bytes) to accommodate
     * the expansion due to DLE stuffing.
     * @param outlen Pointer to a `uint16_t` where the total length of the encoded data
     * in `outbuf` (including header, stuffed payload, trailer, LRC, RUB)
     * will be stored. Set to 0 on encoding failure.
     *
     * @return The calculated 8-bit LRC checksum (masked to 7 bits).
     * Returns 0 and sets `*outlen` to 0 if `inbuf`, `outbuf`, or `outlen` is NULL,
     * if `inlen` is too short (less than 3 for the header), or if an internal
     * buffer overflow occurs during encoding.
     */
    extern uint8_t oasis_packet_encode(uint8_t* inbuf, uint16_t inlen,
        uint8_t* outbuf, uint16_t* outlen);

    /**
     * @brief Calculates the OASIS Longitudinal Redundancy Check (LRC) checksum.
     *
     * The LRC is an 8-bit sum of all bytes in the provided buffer. After summing,
     * the result is ORed with 0xC0 and then ANDed with 0x7F to produce the final
     * 7-bit LRC value.
     *
     * @param buf Pointer to the data buffer over which the LRC is to be calculated.
     * Typically, this includes the packet from DLE STX up to and including DLE ETX.
     * @param len Length of the data in `buf` to be included in the checksum.
     *
     * @return The calculated 8-bit LRC value (effectively 7-bit due to masking).
     * Returns 0 if `buf` is NULL.
     */
    extern uint8_t oasis_lrcc(uint8_t* buf, uint16_t len);

#ifdef __cplusplus
} /* extern "C" */
#endif


#endif /* OASIS_SENDRECV_H_ */
