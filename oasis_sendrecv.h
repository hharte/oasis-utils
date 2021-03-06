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

#ifndef OASIS_SENDRECV_H_
#define OASIS_SENDRECV_H_

#include <stdio.h>
#include <stdint.h>

#define STX     0x02
#define ETX     0x03
#define EOT     0x04
#define ENQ     0x05
#define VT      0x0B
#define SO      0x0E
#define SI      0x0F
#define DLE     0x10
#define CAN     0x18
#define ESC     0x1B
#define RUB     0x7F /* DEL */

#define OPEN    'O'
#define WRITE   'W'
#define CLOSE   'C'

#pragma pack(push, 1)
typedef struct OASIS_PACKET_HEADER {
    uint8_t sync;
    uint8_t ctrl;
    uint8_t rectype;
} OASIS_PACKET_HEADER_T;

typedef struct OASIS_PACKET_TRAILER {
    uint8_t sync;
    uint8_t ctrl;
    uint8_t rectype;
} OASIS_PACKET_TRAILER_T;
#pragma pack(pop)

extern int     oasis_packet_decode(uint8_t *inbuf, uint16_t  inlen, uint8_t  *outbuf, uint16_t *outlen);
extern int     oasis_send_packet(int fd, uint8_t *buf, uint16_t len, uint8_t  cmd);
extern int     oasis_wait_for_ack(int fd);
extern int     oasis_packet_encode(uint8_t *inbuf, uint16_t  inlen, uint8_t  *outbuf, uint16_t *outlen);
extern uint8_t oasis_lrcc(uint8_t *buf, uint16_t len);

#endif  // OASIS_SENDRECV_H_
