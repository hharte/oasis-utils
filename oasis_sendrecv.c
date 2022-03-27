/*
 * OASIS Send/Receive Data Structures and Definitions
 *
 * www.github.com/hharte/oasis-utils
 *
 * Copyright (c) 2021-2022, Howard M. Harte
 *
 * Reference:
 * http://bitsavers.org/pdf/phaseOneSystems/oasis/Communications_Reference_Manual_Mar80.pdf
 * pp. 8
 *
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "./oasis_sendrecv.h"
#include "./mm_serial.h"
#include "./oasis_utils.h"

#if defined(_MSC_VER)
# include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif /* if defined(_MSC_VER) */

int oasis_packet_decode(uint8_t *inbuf, uint16_t inlen, uint8_t *outbuf, uint16_t *outlen) {
    uint8_t  shift = 0;
    uint16_t j     = 0;
    uint8_t  dle_arg;
    uint8_t  last_processed_char = 0;
    uint8_t  cksum               = 0;

    j = 0;

    for (uint16_t i = 3; i < inlen; i++) {
        uint8_t src_data = inbuf[i];

        /* Process DLE (Data Link Escape) */
        if (src_data == DLE) {
            i++;
            dle_arg = inbuf[i];

            switch (dle_arg) {
                case SI:  /* Shift in (add 0x80 to the following data) */
                    shift = 0x80;
                    break;
                case SO:  /* Shift out (don't add 0x80 to the following data) */
                    shift = 0x00;
                    break;
                case DLE: /* DLE+DLE emits DLE (0x10) into the output buffer. */
                    last_processed_char = DLE | shift;
                    outbuf[j]           = last_processed_char;
                    j++;
                    break;
                case VT: /* VT: repeat last_processed_char tablen times. */
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
                                printf("\n\nERROR: Unknown Tablen 0x%x after DLE.\n", inbuf[i - 1]);
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
                        *outlen = 0;
                        return 0;
                    } else {
                        *outlen = j;
                        return cksum;
                    }
                    break;
                case CAN:
                    last_processed_char = ESC | shift;
                    outbuf[j]           = last_processed_char;
                    j++;
                    break;
                default:
                    printf("At %d: Unknown DLE=0x%2x\n", i, inbuf[i]);
                    break;
            }
        } else { /* Process normal character */
            last_processed_char = inbuf[i] + shift;
            outbuf[j]           = last_processed_char;
            j++;
        }
    }

    printf("Decoded %d input bytes to %d output bytes, without reaching <ETX>.\n", inlen, j);
    *outlen = 0;

    return 0;
}

int oasis_send_packet(int fd, uint8_t *buf, uint16_t len, uint8_t cmd) {
    uint8_t  commBuffer[1024];
    uint8_t  decoded_buf[512] = { 0 };
    uint16_t encoded_len;
#ifdef DEBUG_SEND
    uint8_t  cksum;
    printf("Sending %d bytes, cmd='%c'\n", len, cmd);
#endif /* DEBUG_SEND */

    decoded_buf[0] = DLE;
    decoded_buf[1] = STX;
    decoded_buf[2] = cmd;

    if ((len > 0) && (buf != NULL)) {
        memcpy(&decoded_buf[3], buf, len);
    }

#ifdef DEBUG_SEND
    dump_hex(decoded_buf, len + 3);
    cksum =
#endif /* DEBUG_SEND */
    oasis_packet_encode(decoded_buf, len + 3, commBuffer, &encoded_len);

#ifdef DEBUG_SEND
    printf("LRCC: 0x%02x\n", cksum);
    dump_hex(commBuffer, encoded_len);
#endif /* DEBUG_SEND */

    for (uint16_t i = 0; i < len; i++) {
        commBuffer[i] |= 0x80;
    }

    if (write_serial(fd, commBuffer, encoded_len) != encoded_len) {
        return -1;
    }

    return 0;
}

int oasis_wait_for_ack(int fd) {
    uint8_t buf[2];
    int8_t  toggle = -1;

    for (int retry = 0; retry < 5; retry++) {
        if (read_serial(fd, buf, 2) != 2) {
            continue;
        }

        if ((buf[0] != DLE) && ((buf[1] != '0') && (buf[1] != '1'))) {
            //            printf("Retrying...\n");
        } else {
            toggle = buf[1] & 1;

            //            printf("Got ACK, toggle=%d", toggle);
            break;
        }
    }

    return toggle;
}

int oasis_packet_encode(uint8_t *inbuf, uint16_t inlen, uint8_t *outbuf, uint16_t *outlen) {
    uint8_t  shift = 0;
    uint16_t j     = 0;
    uint8_t  cksum = 0;

    outbuf[0] = inbuf[0];
    outbuf[1] = inbuf[1];
    outbuf[2] = inbuf[2];
    j         = 3;

    for (uint16_t i = 3; i < inlen; i++) {
        uint8_t src_data = inbuf[i];

        if ((src_data & 0x80) == shift) {
            outbuf[j] = src_data;

            if (shift) {
                outbuf[j] -= shift;
            }
            j++;
        } else {
            outbuf[j++] = DLE;

            if (shift) {
                outbuf[j++] = SO;
                shift       = 0;
                outbuf[j++] = src_data;
            } else {
                outbuf[j++] = SI;
                shift       = 0x80;
                outbuf[j++] = src_data - shift;
            }
        }

        /* Send DLE as DLE,DKE */
        if (outbuf[j - 1] == DLE) {
            outbuf[j++] = DLE;
        } else if (outbuf[j - 1] == ESC) { /* Handle ESC as DLE,CAN */
            outbuf[j - 1] = DLE;
            outbuf[j++]   = CAN;
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
                } else if (run_length > 3) {
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
                } else {
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

    cksum       = oasis_lrcc(&outbuf[0], j);
    outbuf[j++] = cksum;
    outbuf[j++] = RUB;
    *outlen     = j;

    return cksum;
}

/*
 * Text messages include a Longitudinal Redundancy Check Code (LRCC) which is
 * the
 * 8-bit sum of all characters transmitted in the record, including all
 * control characters. The sum is logically ORed with 0xC0 and transmitted at
 * the
 * end of the message
 */
uint8_t oasis_lrcc(uint8_t *buf, uint16_t len) {
    uint8_t lrcc = 0;

    for (uint16_t i = 0; i < len; i++) {
        lrcc += buf[i];
    }

    /* Not sure why OASIS didn't specify the LRCC as being ORed with 0x40, since
       the
     * high bit will be masked on transmit anyway.
     */
    lrcc |= 0xC0;
    lrcc &= 0x7F;

    return lrcc;
}
