# OASIS Send/Receive Protocol Specification

**Status of this Memo**

This document specifies an OASIS Send/Receive protocol for reliable, transparent binary file transfer between two computer systems, typically over a serial communication link. Distribution of this memo is unlimited.

**Abstract**

The OASIS Send/Receive protocol is a initiator/responder, ACK/NAK-based protocol designed for transferring files between OASIS operating systems. It employs packet-based communication with DLE (Data Link Escape) stuffing for transparency, a toggle-bit acknowledgment scheme for reliable delivery, run-length encoding for data compression, and an LRC (Longitudinal Redundancy Check) for error detection. This document details the protocol's packet structures, control characters, data encoding mechanisms, and operational flow.

---

### 1. Introduction

The OASIS Send/Receive protocol facilitates the transfer of various file types supported by the OASIS operating system. The protocol operates with a sender (initiator) and a receiver (responder), ensuring data integrity through checksums and acknowledgments. It supports transparent data transmission, allowing control characters to be embedded within the data through DLE stuffing.

---

### 2. Terminology

* **DLE**: Data Link Escape (0x10)
* **STX**: Start of Text (0x02)
* **ETX**: End of Text (0x03)
* **EOT**: End of Transmission (0x04)
* **ENQ**: Enquiry (0x05)
* **ACK0**: Acknowledge 0 (DLE followed by '0' (0x30))
* **ACK1**: Acknowledge 1 (DLE followed by '1' (0x31))
* **NAK**: Negative Acknowledge (Implicitly handled by resending the previous ACK or by sender timing out after ENQ)
* **SI**: Shift In (0x0F) - DLE SI indicates subsequent 7-bit data bytes should have their MSB set upon receipt.
* **SO**: Shift Out (0x0E) - DLE SO indicates subsequent 7-bit data bytes should have their MSB cleared upon receipt.
* **VT**: Vertical Tab (0x0B) - DLE VT `count` `char` is used for Run-Length Encoding.
* **CAN**: Cancel (0x18) - DLE CAN represents an ESC character in the data stream.
* **ESC**: Escape (0x1B) - If present in user data, it's transmitted as DLE CAN.
* **SUB**: Substitute (0x1A) - Used as an EOF marker in ASCII mode text files.
* **RUB**: Rubout / Delete (0x7F) - Used as a padding/terminator character after the LRC in sent data packets.
* **LRC**: Longitudinal Redundancy Check.
* **DEB**: Directory Entry Block - A 32-byte structure describing file metadata.
* **Initiator**: The sending system.
* **Responder**: The receiving system.

---

### 3. Protocol Overview

The communication is a initiator/responder relationship where the sender is the initiator. The initiator initiates the transfer and sends data packets. The responder acknowledges received packets. A toggle-bit ACK mechanism (ACK0, ACK1) ensures sequential packet reception.

**General Flow:**
1.  Sender (Initiator) sends ENQ to Receiver (Responder).
2.  Receiver sends ACK0 (DLE '0').
3.  Sender sends Data Packet 1 (e.g., OPEN command with DEB).
4.  Receiver, if packet is valid, sends ACK1 (DLE '1').
5.  Sender sends Data Packet 2 (e.g., WRITE command with file data).
6.  Receiver, if packet is valid, sends ACK0 (DLE '0').
7.  This continues, alternating ACKs, until the file transfer is complete.
8.  If the sender receives an incorrect ACK (e.g., expecting ACK1 but receives ACK0 again) or no ACK after a timeout, it may send an ENQ to solicit a resend of the last ACK from the receiver. This can be repeated up to five times before the initiator disconnects.
9.  After the last data packet (e.g., CLOSE command), the sender may transmit EOT to terminate the session. (This can be suppressed with the NOEOT option in the SEND command).
10. The receiver sends a final ACK for the EOT.

---

### 4. Packet Structure

#### 4.1. Data Packet Format
All data packets (OPEN, WRITE, CLOSE) follow this general structure:

`DLE STX <CMD> <Payload> DLE ETX <LRC> RUB`

* **DLE STX** (2 bytes): Start of packet frame.
* **CMD** (1 byte): Command byte ('O', 'W', or 'C'). This byte is part of the data payload for LRC calculation and DLE stuffing.
* **Payload** (Variable length, up to 512 bytes for user data in WRITE): The actual data or metadata being transferred (e.g., DEB for OPEN, file content for WRITE). The payload undergoes DLE stuffing and RLE (see Section 6).
* **DLE ETX** (2 bytes): End of packet frame (before checksum).
* **LRC** (1 byte): Longitudinal Redundancy Check (see Section 7).
* **RUB** (1 byte): A Rubout character (0x7F) is appended by the sender after the LRC as padding or a terminator.

#### 4.2. Control Signals
* **ENQ** (1 byte): 0x05. Used by sender to initiate or re-query, and by receiver if timeout occurs (though the manual indicates NAK from responder in some contexts).
* **ACK0/ACK1** (2 bytes): DLE + '0' (0x10 0x30) or DLE + '1' (0x10 0x31).
* **EOT** (2 bytes): DLE + EOT (0x10 0x04).

---

### 5. Message Types

#### 5.1. Sender (Initiator) Messages
* **ENQ**: Sent to initiate communication or to re-synchronize if an expected ACK is not received.
* **Data Packet**: Contains commands and data.
    * **OPEN ('O') Packet**: The payload is a 32-byte Directory Entry Block (DEB) for the file being transferred.
        * **Directory Entry Block (DEB) Structure (32 bytes, all multi-byte fields are little-endian, filenames/types are uppercase):**
            * `file_format` (1 byte): Type and attributes of the file.
                * Bits 0-4: File Format Type (FILE\_FORMAT\_RELOCATABLE (0x01), FILE\_FORMAT\_ABSOLUTE (0x02), FILE\_FORMAT\_SEQUENTIAL (0x04), FILE\_FORMAT\_DIRECT (0x08), FILE\_FORMAT\_INDEXED (0x10), FILE\_FORMAT\_KEYED (0x18))
                * Bits 5-7: Attributes (FILE\_FORMAT\_READ\_PROTECTED (0x20), FILE\_FORMAT\_WRITE\_PROTECTED (0x40), FILE\_FORMAT\_DELETE\_PROTECTED (0x80))
            * `file_name` (8 bytes): File name, uppercase, space-padded.
            * `file_type` (8 bytes): File extension/type, uppercase, space-padded.
            * `record_count` (uint16\_t, little-endian): Number of records in the file.
            * `block_count` (uint16\_t, little-endian): Number of 1K blocks allocated.
            * `start_sector` (uint16\_t, little-endian): Logical sector number (LBA) of the first sector.
            * `file_format_dependent1` (uint16\_t, little-endian): Meaning depends on `file_format`.
                * Sequential: Longest record length.
                * Direct: Allocated Record Length.
                * Absolute/Relocatable: Record Length (typically SECTOR\_SIZE, e.g., 256).
                * Indexed/Keyed: Bits 0-8 are record length, Bits 9-15 are key length.
            * `timestamp` (3 bytes, `oasis_tm_t`): Packed OASIS timestamp.
            * `owner_id` (1 byte): ID of the file owner.
            * `shared_from_owner_id` (1 byte): (Meaning context-dependent, often 0).
            * `file_format_dependent2` (uint16\_t, little-endian): Meaning depends on `file_format`.
                * Sequential: Disk address (LBA) of the last sector in the file.
                * Direct: Always zero.
                * Relocatable: Program length in bytes.
                * Absolute: Origin (load) address.
                * Indexed/Keyed: Allocated file size (in some interpretations, or related to total records).
    * **WRITE ('W') Packet**: The payload contains a segment of the file's data.
        * Max data payload is typically 256 bytes (`XFR_BLOCK_SIZE`).
        * For **Sequential** files, the last 2 bytes of this 256-byte block are a `uint16_t` (little-endian) link to the next sector's LBA, or 0 for the last sector of the file. The actual file data in such a packet is therefore `XFR_BLOCK_SIZE - 2` bytes.
        * For other (contiguous) file types, the payload is purely file data.
    * **CLOSE ('C') Packet**: Indicates the end of the current file transfer. The payload is typically empty.
* **EOT**: Sent to terminate the entire communication session.

#### 5.2. Receiver (Responder) Messages
* **ACK0 / ACK1**: Sent to acknowledge the successful and correct receipt of a data packet. The toggle bit alternates for each new packet received correctly.
* **ENQ**: (As per manual, though less common for responder to initiate in documented initiator/responder flow) Sent if the responder has not received anything for a while. In practice, if the initiator times out waiting for an ACK, it sends an ENQ, and the responder responds by resending its last ACK.
* **NAK**: (As per manual) Negative acknowledge, "didn't receive properly." In the implemented protocol and dissector, this seems to be handled by the responder *not* toggling its ACK and resending its previous ACK state when the initiator sends an ENQ after a timeout or garbled reception.

---

### 6. Data Transparency, Encoding, and Compression

The protocol ensures transparent data transmission, allowing any byte value to be part of the payload.

#### 6.1. DLE Stuffing
To allow control characters (like DLE, STX, ETX) to be part of the actual data payload, DLE stuffing is used:
* Any DLE (0x10) byte within the payload (CMD + data) is transmitted as two DLE bytes (DLE DLE).
* Any ESC (0x1B) byte within the payload is transmitted as DLE CAN (0x10 0x18).

#### 6.2. Shift States (7-bit Transmission with 8th Bit Indication)
Message characters are always transmitted as 7-bit ASCII characters. To represent characters that originally had their 8th bit set:
* **DLE SI** (0x10 0x0F): Indicates that subsequent data bytes received should have their most significant bit (MSB) set (i.e., `byte | 0x80` is performed on the received 7-bit byte).
* **DLE SO** (0x10 0x0E): Indicates that subsequent data bytes received should have their MSB cleared (i.e., the received 7-bit byte is used as is, effectively `byte & 0x7F`).
The actual bytes on the wire remain 7-bit; the DLE SI/SO sequences instruct the receiver on how to reconstruct the original 8-bit data. Example: 81H, 0FEH, 8FH, 23H becomes DLE, SI, 01H, 7EH, 0FH, DLE, SO, 23H.

#### 6.3. Run-Length Encoding (RLE)
Repetitive character sequences in the payload (after any 8th-bit processing, but before DLE stuffing of DLE/ESC themselves) can be compressed if a character is repeated four or more times:
* The sequence is transmitted as: `<character> DLE VT count`.
* `<character>`: The character that is repeated (sent once).
* **DLE VT**: The RLE introducer sequence (0x10 0x0B).
* **`count`** (1 byte): The number of *additional* times the character is repeated (actual run length - 1). The `count` byte itself is subject to DLE stuffing if it is DLE (sent as DLE DLE) or ESC (sent as DLE CAN). The maximum value for `count` is 127 (`RUN_LENGTH_MAX`).

Example: Six consecutive spaces ("      ") would be sent as: SP DLE VT 0x05 (where 0x05 is the character with value 5).

---

### 7. Checksum (LRC)

A Longitudinal Redundancy Check (LRC) is used for error detection in data packets.
* **Calculation**: The LRC is an 8-bit sum of all characters in the packet from the initial DLE of `DLE STX` up to and including the ETX of `DLE ETX`.
* **Transformation**: After summing, the 8-bit result is logically ORed with 0xC0, and then logically ANDed with 0x7F.
    `LRC_final = (sum_of_bytes | 0xC0) & 0x7F`
* **Transmission**: This final 7-bit LRC value is transmitted after the DLE ETX sequence.

---

### 8. Handshake and Flow Control

* **Initiation**: The sender (initiator) starts by sending ENQ. The receiver (responder) must respond with ACK0 (DLE '0').
* **Packet Acknowledgment**: Each data packet sent by the initiator must be acknowledged by the responder. The responder alternates between ACK0 and ACK1 for consecutively received valid packets.
* **Error Handling / Retries**:
    * If the initiator receives the wrong ACK (e.g., expects ACK1 but gets ACK0 again), it retransmits an ENQ. This can be repeated up to five times. If the response remains incorrect, the initiator will disconnect.
    * If the initiator times out waiting for an ACK, it also sends an ENQ.
    * If the responder receives a packet with an LRC mismatch or other format error, it implicitly NAKs by *not* toggling its ACK state and resending its *previous* ACK upon the initiator's ENQ. (The manual states responder sends NAK, but practical implementations often use the ACK resend mechanism).
* Hardware flow control (RTS/CTS) may be used at the serial port level but is not part of this packet protocol layer.

---

### 9. Termination

The sender (initiator) signals the end of the entire transmission session by sending DLE EOT. The receiver (responder) should respond with a final ACK (with its current toggle state). The NOEOT option in the SEND command can suppress the EOT after a single file if more files are to follow.

---

### 10. Wireshark Dissection Notes (LINKTYPE\_USER2)

For analysis with tools like Wireshark, PCAP files generated by utilities implementing this protocol (e.g., `oasis-utils`) typically use `LINKTYPE_USER2 (149)`.
A custom Lua dissector (like `oasis_wireshark_dissector.lua`) can interpret these captures.

A key aspect for such dissectors is that the capturing utility (`oasis_send` or `oasis_recv`) prepends a **direction byte** to the actual serial data for each frame in the PCAP file:
* `0x00`: Indicates data **received** (RX) by the capturing utility from the serial port.
* `0x01`: Indicates data **transmitted** (TX) by the capturing utility to the serial port.

The dissector uses this first byte to correctly label the direction of the encapsulated OASIS protocol message. The actual OASIS packet (e.g., starting DLE STX or ENQ) follows this direction byte.

---

### 11. Security Considerations

This protocol was designed for direct serial connections and does not include any intrinsic security features such as encryption or authentication. It is vulnerable to eavesdropping and data manipulation if used over insecure channels.

---

### 12. References

* Phase One Systems, Inc. ["OASIS Communications Reference Manual", Second Edition, March 1980](http://www.bitsavers.org/pdf/phaseOneSystems/oasis/Communications_Reference_Manual_2ed.pdf). (Provided as `Communications_Reference_Manual_Mar80.pdf`)
* [`oasis-utils` project source code](https://github.com/hharte/oasis-utils/tree/main/)
    * [`src/oasis_sendrecv.c`](https://raw.githubusercontent.com/hharte/oasis-utils/refs/heads/main/src/oasis_sendrecv.c)
    * [`src/oasis.h`](https://raw.githubusercontent.com/hharte/oasis-utils/refs/heads/main/src/oasis.h)
    * [`src/oasis_sendrecv.h`](https://raw.githubusercontent.com/hharte/oasis-utils/refs/heads/main/src/oasis_sendrecv.h)
* `oasis-utils` project Wireshark dissector: [`wireshark/oasis_wireshark_dissector.lua`](https://raw.githubusercontent.com/hharte/oasis-utils/refs/heads/main/wireshark/oasis_wireshark_dissector.lua).
