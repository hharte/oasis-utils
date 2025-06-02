-- Wireshark Lua Dissector for OASIS Protocol
-- Intended to be used as a subdissector for rtacser.data
-- www.github.com/hharte/oasis-utils
--
-- Copyright (c) 2021-2025, Howard M. Harte
-- SPDX-License-Identifier: MIT

-- Protocol Definition
local oasis_proto = Proto("OASIS", "OASIS File Transfer Protocol")


-- File Format Types
local FILE_FORMAT_RELOCATABLE     = 0x01
local FILE_FORMAT_ABSOLUTE        = 0x02
local FILE_FORMAT_SEQUENTIAL      = 0x04
local FILE_FORMAT_DIRECT          = 0x08
local FILE_FORMAT_INDEXED         = 0x10
local FILE_FORMAT_KEYED           = 0x18 -- (Effectively Direct | Indexed based on usage)
local FILE_FORMAT_DELETED         = 0xFF
local FILE_FORMAT_EMPTY           = 0x00

-- File Attributes
local FILE_ATTRIBUTE_READ_PROTECTED   = 0x20
local FILE_ATTRIBUTE_WRITE_PROTECTED  = 0x40
local FILE_ATTRIBUTE_DELETE_PROTECTED = 0x80

-- Masks
local FILE_FORMAT_MASK = 0x1F
local FILE_ATTRIBUTE_MASK = 0xE0

-- DEB String Lengths
local FNAME_LEN = 8
local FTYPE_LEN = 8

-- Control Character Values
local STX = 0x02
local ETX = 0x03
local EOT = 0x04
local ENQ = 0x05
local VT  = 0x0B -- Used for RLE count
local SO  = 0x0E
local SI  = 0x0F
local DLE = 0x10
local NAK = 0x15 -- Explicit NAK character
local CAN = 0x18 -- DLE+CAN represents ESC
local SUB = 0x1A -- Substitute (Used as EOF marker in ASCII mode text files)
local ESC = 0x1B
local RUB = 0x7F -- Expected final byte after LRC

-- Helper table for command names
local command_names = {
    [string.byte('O')] = "OPEN",
    [string.byte('W')] = "WRITE",
    [string.byte('C')] = "CLOSE"
}

-- Define names for source/destination entities
local SENDER_ENTITY_NAME = "oasis_send"
local RECEIVER_ENTITY_NAME = "oasis_recv"

-- Global state for the currently active file transfer
local current_file_state = {
    file_name_str = nil,
    is_sequential = false,
    -- For sequential files: current_seq_write_segment is the last ACKED segment number. Starts at 0.
    current_seq_write_segment = 0,
    total_records_in_file_seq = 0,
    -- For non-sequential files: current_non_seq_record_num is the NEXT record number to be written. Starts at 1.
    current_non_seq_record_num = 1,
    -- Common for data packets (OPEN, WRITE, CLOSE)
    is_awaiting_ack_for_data_write = false,
    expected_ack_toggle_for_current_data_write = 1, -- Default for first data packet (OPEN)
    bytes_from_pending_data_write = 0,
    -- Common file summary
    total_records_in_file = 0,
    total_payload_bytes_written_this_file = 0,
    -- Context for the last data command awaiting an ACK/NAK
    last_data_command_awaiting_ack = nil -- Will store { cmd_char, file_name, is_seq [, rec_seg_num] }
}

-- Global cache for storing calculated display details per frame
local frame_display_details_cache = {}

-- Function to reset the current file state
local function reset_current_file_state(frame_num_for_debug)
    current_file_state.file_name_str = nil
    current_file_state.is_sequential = false
    current_file_state.current_seq_write_segment = 0 -- Last ACKed sequential segment
    current_file_state.total_records_in_file_seq = 0
    current_file_state.current_non_seq_record_num = 1 -- Next non-sequential record to write
    current_file_state.is_awaiting_ack_for_data_write = false
    current_file_state.expected_ack_toggle_for_current_data_write = 1 -- Initial expectation for OPEN
    current_file_state.bytes_from_pending_data_write = 0
    current_file_state.total_records_in_file = 0
    current_file_state.total_payload_bytes_written_this_file = 0
    current_file_state.last_data_command_awaiting_ack = nil -- Reset context for ACK/NAK info
    local debug_frame_info = frame_num_for_debug and ("Frame " .. tostring(frame_num_for_debug)) or "INIT"
    print(string.format("[OASIS_DEBUG] %s: reset_current_file_state: next_non_seq_rec_num=%d, last_acked_seq_seg=%d, is_awaiting_ack_data=%s, expected_ack_data_toggle=%d, pending_bytes=%d, total_written=%d, last_cmd_ctx_cleared",
        debug_frame_info,
        current_file_state.current_non_seq_record_num,
        current_file_state.current_seq_write_segment,
        tostring(current_file_state.is_awaiting_ack_for_data_write),
        current_file_state.expected_ack_toggle_for_current_data_write,
        current_file_state.bytes_from_pending_data_write,
        current_file_state.total_payload_bytes_written_this_file
        ))
end

-- Dissector's init function, called when script is loaded/reloaded
function oasis_proto.init()
    print("[OASIS_DEBUG] oasis_proto.init() called.")
    frame_display_details_cache = {}
    reset_current_file_state(nil)
end


-- Protocol Fields Definition
local pf = {
    msg_type = ProtoField.string("oasis.type", "Message Type"),

    enq = ProtoField.bytes("oasis.enq", "ENQ Signal", base.NONE),
    eot = ProtoField.bytes("oasis.eot", "EOT Signal", base.NONE),
    nak = ProtoField.bytes("oasis.nak", "NAK Signal", base.NONE),

    ack_raw = ProtoField.uint8("oasis.ack.raw", "ACK Signal", base.HEX),
    ack_toggle = ProtoField.uint8("oasis.ack.toggle", "ACK Toggle", base.DEC_HEX),

    packet_raw_data = ProtoField.bytes("oasis.packet.raw_data", "Raw Full Packet Data", base.NONE),
    packet_cmd = ProtoField.uint8("oasis.packet.cmd", "Command", base.ASCII, command_names),
    packet_payload_raw_stuffed = ProtoField.bytes("oasis.packet.payload_raw_stuffed", "Payload (Raw/Stuffed)", base.NONE),
    packet_payload_decoded_bytes = ProtoField.bytes("oasis.packet.payload_decoded.bytes", "Payload (Decoded Bytes)", base.NONE),
    packet_payload_seq_link = ProtoField.uint16("oasis.packet.payload.seq_link", "Sequential Next Sector Link", base.DEC_HEX, nil, nil, "Link in WRITE payload", ftypes.LITTLE_ENDIAN),
    packet_trailer_dle_etx = ProtoField.bytes("oasis.packet.trailer_dle_etx", "Trailer (DLE ETX)", base.NONE),
    packet_lrc_received = ProtoField.uint8("oasis.packet.lrc.received", "LRC (Received)", base.HEX),
    packet_lrc_calculated = ProtoField.uint8("oasis.packet.lrc.calculated", "LRC (Calculated)", base.HEX),
    packet_final_rub_byte = ProtoField.uint8("oasis.packet.final_rub_byte", "Final Byte (Padding)", base.HEX),

    packet_current_filename = ProtoField.string("oasis.packet.current_file", "Current File"),
    packet_write_segment = ProtoField.uint32("oasis.packet.write_segment", "Write Segment/Record #"),
    packet_total_bytes_transferred = ProtoField.uint32("oasis.packet.total_bytes_transferred", "Total Payload Bytes Transferred for File", base.DEC),

    deb_file_format_raw = ProtoField.uint8("oasis.deb.file_format.raw", "File Format (Raw Byte)", base.HEX),
    deb_file_format_type = ProtoField.uint8("oasis.deb.file_format.type", "File Format Type", base.HEX, {
        [FILE_FORMAT_RELOCATABLE] = "Relocatable", [FILE_FORMAT_ABSOLUTE] = "Absolute",
        [FILE_FORMAT_SEQUENTIAL] = "Sequential", [FILE_FORMAT_DIRECT] = "Direct",
        [FILE_FORMAT_INDEXED] = "Indexed", [FILE_FORMAT_KEYED] = "Keyed (Direct+Indexed)",
        [FILE_FORMAT_EMPTY] = "Empty", [FILE_FORMAT_DELETED] = "Deleted"
    }),
    deb_file_attributes_str = ProtoField.string("oasis.deb.file_format.attributes_str", "File Attributes (String)"),
    deb_file_name_str = ProtoField.string("oasis.deb.file_name_str", "File Name", base.ASCII),
    deb_file_type_str = ProtoField.string("oasis.deb.file_type_str", "File Type", base.ASCII),
    deb_record_count_val = ProtoField.uint16("oasis.deb.record_count_val", "Record Count", base.DEC, nil, nil, nil, ftypes.LITTLE_ENDIAN),
    deb_block_count_val = ProtoField.uint16("oasis.deb.block_count_val", "Block Count (1K units)", base.DEC, nil, nil, nil, ftypes.LITTLE_ENDIAN),
    deb_start_sector_val = ProtoField.uint16("oasis.deb.start_sector_val", "Start Sector (LBA)", base.DEC_HEX, nil, nil, nil, ftypes.LITTLE_ENDIAN),
    deb_ffd1_raw = ProtoField.uint16("oasis.deb.ffd1.raw", "Format Dependent 1 (Raw)", base.HEX, nil, nil, nil, ftypes.LITTLE_ENDIAN),
    deb_ffd1_seq_rec_len_val = ProtoField.uint16("oasis.deb.ffd1.seq_rec_len_val", "Seq: Longest Record Length", base.DEC, nil, nil, nil, ftypes.LITTLE_ENDIAN),
    deb_ffd1_dir_rec_len_val = ProtoField.uint16("oasis.deb.ffd1.dir_rec_len_val", "Dir: Allocated Record Length", base.DEC, nil, nil, nil, ftypes.LITTLE_ENDIAN),
    deb_ffd1_absrel_rec_len_val = ProtoField.uint16("oasis.deb.ffd1.absrel_rec_len_val", "Abs/Rel: Record Length", base.DEC, nil, nil, nil, ftypes.LITTLE_ENDIAN),
    deb_ffd1_idxkey_rec_len_val = ProtoField.uint16("oasis.deb.ffd1.idxkey_rec_len_val", "Idx/Key: Record Length", base.DEC, nil, nil, nil, ftypes.LITTLE_ENDIAN),
    deb_ffd1_idxkey_key_len_val = ProtoField.uint16("oasis.deb.ffd1.idxkey_key_len_val", "Idx/Key: Key Length", base.DEC, nil, nil, nil, ftypes.LITTLE_ENDIAN),
    deb_timestamp_raw_bytes = ProtoField.bytes("oasis.deb.timestamp.raw_bytes", "Timestamp (Raw Bytes)", base.NONE),
    deb_timestamp_decoded_str = ProtoField.string("oasis.deb.timestamp.decoded_str", "Timestamp (Decoded String)"),
    deb_owner_id_val = ProtoField.uint8("oasis.deb.owner_id_val", "Owner ID", base.DEC),
    deb_shared_from_owner_id_val = ProtoField.uint8("oasis.deb.shared_from_owner_id_val", "Shared From Owner ID", base.DEC),
    deb_ffd2_raw = ProtoField.uint16("oasis.deb.ffd2.raw", "Format Dependent 2 (Raw)", base.HEX, nil, nil, nil, ftypes.LITTLE_ENDIAN),
    deb_ffd2_seq_last_sector_val = ProtoField.uint16("oasis.deb.ffd2.seq_last_sector_val", "Seq: Last Sector LBA", base.DEC_HEX, nil, nil, nil, ftypes.LITTLE_ENDIAN),
    deb_ffd2_abs_load_addr_val = ProtoField.uint16("oasis.deb.ffd2.abs_load_addr_val", "Abs: Load Address", base.HEX, nil, nil, nil, ftypes.LITTLE_ENDIAN),
    deb_ffd2_rel_prog_len_val = ProtoField.uint16("oasis.deb.ffd2.rel_prog_len_val", "Rel: Program Length", base.DEC, nil, nil, nil, ftypes.LITTLE_ENDIAN),
    deb_ffd2_idxkey_alloc_size_val = ProtoField.uint16("oasis.deb.ffd2.idxkey_alloc_size_val", "Idx/Key: Allocated File Size", base.DEC, nil, nil, nil, ftypes.LITTLE_ENDIAN),

    trailing_enq_signal = ProtoField.uint8("oasis.trailing_enq", "Trailing ENQ Signal", base.HEX),
    trailing_rub_signal = ProtoField.uint8("oasis.trailing_rub", "Trailing RUB Signal", base.HEX),
    trailing_ack0_signal = ProtoField.uint8("oasis.trailing_ack0", "ACK0 Signal", base.HEX),
    trailing_ack1_signal = ProtoField.uint8("oasis.trailing_ack1", "ACK1 Signal", base.HEX),
    unexpected_trailing_data = ProtoField.bytes("oasis.unexpected_trailing_data", "Unexpected Trailing Data", base.NONE)
}
oasis_proto.fields = pf

-- Expert Info Fields
local expert_info = {
    lrc_mismatch = ProtoExpert.new("oasis.lrc.mismatch", "LRC Checksum Mismatch", expert.group.CHECKSUM, expert.severity.ERROR),
    open_payload_too_short = ProtoExpert.new("oasis.deb.payload_too_short", "OPEN packet payload is too short for a full DEB", expert.group.MALFORMED, expert.severity.WARN),
    invalid_deb_file_format = ProtoExpert.new("oasis.deb.invalid_file_format", "DEB file format type is unknown or invalid for the context", expert.group.MALFORMED, expert.severity.WARN),
    packet_trailer_not_found = ProtoExpert.new("oasis.packet.trailer_not_found", "Packet trailer (DLE ETX) was not found as expected", expert.group.MALFORMED, expert.severity.WARN),
    lrc_byte_missing = ProtoExpert.new("oasis.packet.lrc_byte_missing", "LRC byte is missing after DLE ETX trailer", expert.group.MALFORMED, expert.severity.WARN),
    payload_decode_error = ProtoExpert.new("oasis.packet.payload_decode_error", "An error occurred during payload decoding (e.g., DLE stuffing or RLE)", expert.group.MALFORMED, expert.severity.ERROR),
    rle_vt_error = ProtoExpert.new("oasis.packet.rle_vt_error", "DLE VT (Run-Length Encoding) compression error", expert.group.MALFORMED, expert.severity.WARN),
    final_rub_byte_missing = ProtoExpert.new("oasis.packet.final_rub_byte_missing", "Required final padding byte (0x7F RUB) is missing after LRC", expert.group.MALFORMED, expert.severity.WARN),
    final_rub_byte_incorrect = ProtoExpert.new("oasis.packet.final_rub_byte_incorrect", "Incorrect final padding byte value (expected 0x7F RUB)", expert.group.MALFORMED, expert.severity.WARN),
    unexpected_trailing_bytes = ProtoExpert.new("oasis.unexpected_trailing_bytes", "Unexpected trailing bytes found after recognized message and signals", expert.group.MALFORMED, expert.severity.WARN),
}
oasis_proto.experts = expert_info

--------------------------------------------------------------------------------
-- Helper Functions
--------------------------------------------------------------------------------
local function calculate_oasis_lrc(tvb, offset, len)
    local lrc = 0
    if len > 0 then
        for i = offset, offset + len - 1 do
            lrc = lrc + tvb(i, 1):uint()
        end
    end
    lrc = bit.bor(lrc, 0xC0)
    lrc = bit.band(lrc, 0x7F)
    return bit.band(lrc, 0xFF)
end

local function decode_oasis_timestamp_to_str(tvb_range)
    if not tvb_range or tvb_range:len() < 3 then return "Invalid Timestamp (too short)" end
    local b0, b1, b2
    local status_b0, val_b0 = pcall(function() return tvb_range(0,1):uint() end)
    local status_b1, val_b1 = pcall(function() return tvb_range(1,1):uint() end)
    local status_b2, val_b2 = pcall(function() return tvb_range(2,1):uint() end)

    if not (status_b0 and status_b1 and status_b2) then
        return "Invalid Timestamp (read error)"
    end
    b0, b1, b2 = val_b0, val_b1, val_b2

    local mon = bit.rshift(b0, 4)
    local day = bit.bor(bit.lshift(bit.band(b0, 0x0F), 1), bit.rshift(b1, 7))
    local year_offset = bit.band(bit.rshift(b1, 3), 0x0F)
    local hour = bit.bor(bit.lshift(bit.band(b1, 0x07), 2), bit.rshift(b2, 6))
    local min = bit.band(b2, 0x3F)

    if mon < 1 or mon > 12 or day < 1 or day > 31 or hour > 23 or min > 59 or year_offset > 15 then
        return string.format("Invalid Date/Time (Raw: %02X %02X %02X -> M:%d D:%d Y:%d H:%d M:%d)",
                             b0, b1, b2, mon, day, year_offset + 77, hour, min)
    end
    local year = year_offset + 1977
    return string.format("%02d/%02d/%02d %02d:%02d", mon, day, year % 100, hour, min)
end

local function tvb_to_printable_string_full(tvb_range)
    if not tvb_range then return "" end
    local str_tbl = {}
    for i = 0, tvb_range:len() - 1 do
        local byte = tvb_range(i, 1):uint()
        table.insert(str_tbl, string.char(byte))
    end
    return table.concat(str_tbl)
end

local function decode_oasis_payload_to_tvb(raw_payload_tvb, tree_node_for_experts)
    local byte_accumulator = {}
    local current_shift_state = 0
    local last_written_char_code = nil
    local i = 0
    local raw_payload_len = raw_payload_tvb:len()

    while i < raw_payload_len do
        local current_byte
        local pcall_status, byte_or_err = pcall(function() return raw_payload_tvb(i, 1):uint() end)
        if not pcall_status then
            return nil, "Error reading byte at offset " .. i .. " from raw payload: " .. tostring(byte_or_err)
        end
        current_byte = byte_or_err
        i = i + 1

        if current_byte == DLE then
            if i >= raw_payload_len then
                if tree_node_for_experts then tree_node_for_experts:add_tvb_expert_info(expert_info.payload_decode_error, raw_payload_tvb:range(i-1,1), "Unexpected end after DLE in payload") end
                return nil, "Unexpected end after DLE in payload"
            end
            local pcall_next_status, next_byte_or_err = pcall(function() return raw_payload_tvb(i, 1):uint() end)
            if not pcall_next_status then
                return nil, "Error reading byte following DLE at offset " .. i .. ": " .. tostring(next_byte_or_err)
            end
            local dle_arg = next_byte_or_err
            i = i + 1

            if dle_arg == DLE then
                last_written_char_code = DLE
                table.insert(byte_accumulator, DLE + current_shift_state)
            elseif dle_arg == SI then
                current_shift_state = 0x80
            elseif dle_arg == SO then
                current_shift_state = 0x00
            elseif dle_arg == CAN then
                last_written_char_code = ESC
                table.insert(byte_accumulator, ESC + current_shift_state)
            elseif dle_arg == VT then
                if i >= raw_payload_len then
                    if tree_node_for_experts then tree_node_for_experts:add_tvb_expert_info(expert_info.rle_vt_error, raw_payload_tvb:range(i-2,2),"Unexpected end after DLE VT") end
                    return nil, "Unexpected end after DLE VT"
                end
                if last_written_char_code == nil then
                     if tree_node_for_experts then tree_node_for_experts:add_tvb_expert_info(expert_info.rle_vt_error, raw_payload_tvb:range(i-2,2),"DLE VT used before any character outputted") end
                    return nil, "DLE VT used before any character was outputted"
                end

                local pcall_count_status, count_byte_or_err = pcall(function() return raw_payload_tvb(i, 1):uint() end)
                if not pcall_count_status then return nil, "Error reading RLE count byte: " .. tostring(count_byte_or_err) end
                local rle_count_val = count_byte_or_err
                i = i + 1

                if rle_count_val == DLE then
                    if i >= raw_payload_len then
                        if tree_node_for_experts then tree_node_for_experts:add_tvb_expert_info(expert_info.rle_vt_error, raw_payload_tvb:range(i-3,3), "Unexpected end after DLE VT DLE (for escaped count)") end
                        return nil, "Unexpected end after DLE VT DLE (for escaped count)"
                    end
                    local pcall_esc_count_status, esc_count_byte_or_err = pcall(function() return raw_payload_tvb(i, 1):uint() end)
                    if not pcall_esc_count_status then return nil, "Error reading escaped RLE count byte: " .. tostring(esc_count_byte_or_err) end
                    local esc_count_val = esc_count_byte_or_err
                    i = i + 1
                    if esc_count_val == DLE then rle_count_val = DLE
                    elseif esc_count_val == CAN then rle_count_val = ESC
                    else
                        if tree_node_for_experts then tree_node_for_experts:add_tvb_expert_info(expert_info.rle_vt_error, raw_payload_tvb:range(i-4,4), "Invalid escaped RLE count value: " .. esc_count_val) end
                        return nil, "Invalid DLE-escaped RLE count value: " .. esc_count_val
                    end
                end
                for _ = 1, rle_count_val do
                    table.insert(byte_accumulator, last_written_char_code + current_shift_state)
                end
            else
                if tree_node_for_experts then tree_node_for_experts:add_tvb_expert_info(expert_info.payload_decode_error, raw_payload_tvb:range(i-2,2), "Unknown DLE sequence: DLE + 0x" .. string.format("%02X", dle_arg)) end
                return nil, "Unknown DLE sequence in payload: DLE + 0x" .. string.format("%02X", dle_arg)
            end
        else
            last_written_char_code = current_byte
            table.insert(byte_accumulator, current_byte + current_shift_state)
        end
    end

    if #byte_accumulator > 0 then
        local byte_array_obj = ByteArray.new()
        local ok_set_size, err_set_size = pcall(function() byte_array_obj:set_size(#byte_accumulator) end)
        if not ok_set_size then return nil, "Failed to set ByteArray size: " .. tostring(err_set_size) end

        for idx, val in ipairs(byte_accumulator) do
            local ok_set_idx, err_set_idx = pcall(function() byte_array_obj:set_index(idx - 1, val) end)
            if not ok_set_idx then return nil, "Failed to set byte in ByteArray at index " .. (idx-1) .. ": " .. tostring(err_set_idx) end
        end
        return ByteArray.tvb(byte_array_obj, "Decoded OASIS Payload"), nil
    else
        return ByteArray.tvb(ByteArray.new(), "Empty Decoded OASIS Payload"), nil
    end
end

-- Helper to get command display details (handles caching)
local function get_command_display_details(pinfo, cmd_val, current_s, cache)
    local details = {}
    if not pinfo.visited then -- First Pass: Calculate and cache
        details.file_name = current_s.file_name_str
        details.is_seq = current_s.is_sequential

        if cmd_val == string.byte('W') then
            print(string.format("[OASIS_DEBUG] Frame %d: WRITE (get_details 1st pass) BEFORE process: file='%s', is_seq=%s, current_non_seq_rec_num=%d, current_seq_seg=%d (last_acked_seg), is_awaiting_ack_data=%s, expected_ack_data_toggle=%d",
                pinfo.number,
                tostring(current_s.file_name_str),
                tostring(current_s.is_sequential),
                current_s.current_non_seq_record_num,
                current_s.current_seq_write_segment, -- This is now the last ACKed segment
                tostring(current_s.is_awaiting_ack_for_data_write),
                current_s.expected_ack_toggle_for_current_data_write))

            details.total_recs_context = current_s.total_records_in_file
            if current_s.is_sequential then
                -- DON'T increment current_s.current_seq_write_segment here.
                -- Calculate the segment number for *this specific write* packet.
                details.display_rec_num = current_s.current_seq_write_segment + 1
                details.total_recs_context = current_s.total_records_in_file_seq
            else -- Non-sequential
                details.display_rec_num = current_s.current_non_seq_record_num -- This is the record number for the current write
            end
            current_s.is_awaiting_ack_for_data_write = true -- Set general flag for any data write
            print(string.format("[OASIS_DEBUG] Frame %d: WRITE (get_details 1st pass) AFTER process: display_rec_num_for_this_packet=%d, is_awaiting_ack_data_now=%s",
                pinfo.number,
                details.display_rec_num, -- This is the calculated segment/record for this packet
                tostring(current_s.is_awaiting_ack_for_data_write)))

        elseif cmd_val == string.byte('C') then
             details.total_bytes_close = current_s.total_payload_bytes_written_this_file
             if not pinfo.visited then current_s.is_awaiting_ack_for_data_write = true end
        elseif cmd_val == string.byte('O') then
             details.fname = current_s.file_name_str -- This will be stale for the current OPEN, parse_deb will fix current_s.file_name_str
             if not pinfo.visited then current_s.is_awaiting_ack_for_data_write = true end
        end
        cache[pinfo.number] = details
    else -- Second Pass: Read from cache
        local cached = cache[pinfo.number]
        if cached then
            details = cached
        else
            details = { file_name = "[CacheErr]", display_rec_num = 0, total_recs_context = 0, is_seq = false, total_bytes_close = 0 }
            if cmd_val == string.byte('O') then details.fname = "[CacheErrOPEN]" end
        end
    end
    return details
end


-- Helper to parse DEB and update current_file_state
local function parse_deb_and_update_state(deb_tvbr, deb_node, current_state_param, pinfo_param)
    if not pinfo_param.visited then
        reset_current_file_state(pinfo_param.number)
        print(string.format("[OASIS_DEBUG] Frame %d: Called reset_current_file_state during OPEN processing.", pinfo_param.number))
    end

    deb_node:add(pf.deb_file_format_raw, deb_tvbr:range(0,1))
    local ff_raw = deb_tvbr(0,1):uint()
    local ff_type_val = bit.band(ff_raw, FILE_FORMAT_MASK)

    current_state_param.is_sequential = (ff_type_val == FILE_FORMAT_SEQUENTIAL)

    local ff_attr_raw = bit.band(ff_raw, FILE_ATTRIBUTE_MASK)
    local attr_str = ""
    if bit.band(ff_attr_raw, FILE_ATTRIBUTE_READ_PROTECTED)   ~= 0 then attr_str = attr_str .. "R" end
    if bit.band(ff_attr_raw, FILE_ATTRIBUTE_WRITE_PROTECTED)  ~= 0 then attr_str = attr_str .. "W" end
    if bit.band(ff_attr_raw, FILE_ATTRIBUTE_DELETE_PROTECTED) ~= 0 then attr_str = attr_str .. "D" end
    if attr_str == "" then attr_str = "None" end

    deb_node:add(pf.deb_file_format_type, deb_tvbr:range(0,1), ff_type_val)
    deb_node:add(pf.deb_file_attributes_str, deb_tvbr:range(0,0)):set_text("Attributes: " .. attr_str)

    local known_types = {
        [FILE_FORMAT_RELOCATABLE]=true, [FILE_FORMAT_ABSOLUTE]=true, [FILE_FORMAT_SEQUENTIAL]=true,
        [FILE_FORMAT_DIRECT]=true, [FILE_FORMAT_INDEXED]=true, [FILE_FORMAT_KEYED]=true,
        [FILE_FORMAT_EMPTY]=true, [FILE_FORMAT_DELETED]=true
    }
    if not known_types[ff_type_val] then
        deb_node:add_tvb_expert_info(expert_info.invalid_deb_file_format, deb_tvbr:range(0,1))
    end

    local deb_fname_tvb = deb_tvbr:range(1, FNAME_LEN)
    local deb_ftype_tvb = deb_tvbr:range(1 + FNAME_LEN, FTYPE_LEN)
    deb_node:add(pf.deb_file_name_str, deb_fname_tvb)
    deb_node:add(pf.deb_file_type_str, deb_ftype_tvb)

    local fname_part = string.gsub(deb_fname_tvb:string(), "%s*$", "")
    local ftype_part = string.gsub(deb_ftype_tvb:string(), "%s*$", "")
    if ftype_part == "" then
        current_state_param.file_name_str = fname_part
    else
        current_state_param.file_name_str = fname_part .. "." .. ftype_part
    end

    local pcall_status_rec_count, rec_count_val = pcall(function() return deb_tvbr(17,2):le_uint() end)
    if pcall_status_rec_count then
        current_state_param.total_records_in_file = rec_count_val
        if current_state_param.is_sequential then
            current_state_param.total_records_in_file_seq = rec_count_val
        end
    else
        current_state_param.total_records_in_file = 0
        current_state_param.total_records_in_file_seq = 0
    end
    deb_node:add_le(pf.deb_record_count_val, deb_tvbr:range(17, 2))
    deb_node:add_le(pf.deb_block_count_val, deb_tvbr:range(19, 2))
    deb_node:add_le(pf.deb_start_sector_val, deb_tvbr:range(21, 2))

    local ffd1_node = deb_node:add_le(pf.deb_ffd1_raw, deb_tvbr:range(23,2))
    local ffd1_le_val = deb_tvbr(23,2):le_uint()
    if ff_type_val == FILE_FORMAT_SEQUENTIAL then
        ffd1_node:add_le(pf.deb_ffd1_seq_rec_len_val, deb_tvbr:range(23,2), ffd1_le_val)
    elseif ff_type_val == FILE_FORMAT_DIRECT then
        ffd1_node:add_le(pf.deb_ffd1_dir_rec_len_val, deb_tvbr:range(23,2), ffd1_le_val)
    elseif ff_type_val == FILE_FORMAT_ABSOLUTE or ff_type_val == FILE_FORMAT_RELOCATABLE then
        ffd1_node:add_le(pf.deb_ffd1_absrel_rec_len_val, deb_tvbr:range(23,2), ffd1_le_val)
    elseif ff_type_val == FILE_FORMAT_INDEXED or ff_type_val == FILE_FORMAT_KEYED then
        ffd1_node:add_le(pf.deb_ffd1_idxkey_rec_len_val, deb_tvbr:range(23,2), bit.band(ffd1_le_val, 0x01FF))
        ffd1_node:add_le(pf.deb_ffd1_idxkey_key_len_val, deb_tvbr:range(23,2), bit.rshift(ffd1_le_val, 9))
    end

    local ts_tvbr = deb_tvbr:range(25,3)
    local ts_str_val = decode_oasis_timestamp_to_str(ts_tvbr)
    deb_node:add(pf.deb_timestamp_raw_bytes, ts_tvbr)
    deb_node:add(pf.deb_timestamp_decoded_str, deb_tvbr:range(25,0)):set_text("Timestamp: " .. ts_str_val)

    deb_node:add(pf.deb_owner_id_val, deb_tvbr:range(28,1))
    deb_node:add(pf.deb_shared_from_owner_id_val, deb_tvbr:range(29,1))

    local ffd2_node = deb_node:add_le(pf.deb_ffd2_raw, deb_tvbr:range(30,2))
    local ffd2_le_val = deb_tvbr(30,2):le_uint()
    if ff_type_val == FILE_FORMAT_SEQUENTIAL then
        ffd2_node:add_le(pf.deb_ffd2_seq_last_sector_val, deb_tvbr:range(30,2), ffd2_le_val)
    elseif ff_type_val == FILE_FORMAT_ABSOLUTE then
        ffd2_node:add_le(pf.deb_ffd2_abs_load_addr_val, deb_tvbr:range(30,2), ffd2_le_val)
    elseif ff_type_val == FILE_FORMAT_RELOCATABLE then
        ffd2_node:add_le(pf.deb_ffd2_rel_prog_len_val, deb_tvbr:range(30,2), ffd2_le_val)
    elseif ff_type_val == FILE_FORMAT_INDEXED or ff_type_val == FILE_FORMAT_KEYED then
            ffd2_node:add_le(pf.deb_ffd2_idxkey_alloc_size_val, deb_tvbr:range(30,2), ffd2_le_val)
    end

    if not pinfo_param.visited then
        -- Update frame_display_details_cache for OPEN command, as its filename is now known
        if not frame_display_details_cache[pinfo_param.number] then frame_display_details_cache[pinfo_param.number] = {} end
        frame_display_details_cache[pinfo_param.number].fname = current_state_param.file_name_str

        print(string.format("[OASIS_DEBUG] Frame %d: OPEN processed (parse_deb): file_name='%s', is_sequential=%s, next_non_seq_rec_num=%d, last_acked_seq_seg=%d, total_recs_in_file=%d, total_recs_in_file_seq=%d, expected_ack_data_toggle=%d",
            pinfo_param.number,
            tostring(current_state_param.file_name_str),
            tostring(current_state_param.is_sequential),
            current_state_param.current_non_seq_record_num,
            current_state_param.current_seq_write_segment,
            current_state_param.total_records_in_file,
            current_state_param.total_records_in_file_seq,
            current_state_param.expected_ack_toggle_for_current_data_write))
    end
end

--------------------------------------------------------------------------------
-- Main Dissector Function
--------------------------------------------------------------------------------
function oasis_proto.dissector(tvb, pinfo, tree)
    print(string.format("[OASIS_DEBUG] Frame %d: Dissector START. Visited: %s", pinfo.number, tostring(pinfo.visited)))
    pinfo.cols.protocol = oasis_proto.name

    -- The actual OASIS message data starts after the pseudo-header
    local data_len = tvb:len()

    -- Main subtree for the OASIS protocol data itself
    local subtree = tree:add(oasis_proto, tvb)
    subtree:set_text("OASIS Protocol Message (" .. data_len .. " bytes)")

    local first_byte_val = tvb(0, 1):uint()
    local consumed_len_in_tvb = 0 -- Relative to tvb

    pinfo.cols.src = "?"
    pinfo.cols.dst = "?"

    if first_byte_val == ENQ and data_len >= 1 then
        pinfo.cols.info:set("ENQ Signal")
        pinfo.cols.src = SENDER_ENTITY_NAME; pinfo.cols.dst = RECEIVER_ENTITY_NAME
        subtree:add(pf.msg_type, tvb(0,1)):set_text("Type: ENQ")
        subtree:add(pf.enq, tvb(0, 1))
        consumed_len_in_tvb = 1
    elseif first_byte_val == NAK and data_len >= 1 then
        local nak_msg_base = "NAK Signal"
        local detailed_info_suffix = ""
        pinfo.cols.src = RECEIVER_ENTITY_NAME; pinfo.cols.dst = SENDER_ENTITY_NAME
        subtree:add(pf.msg_type, tvb(0,1)):set_text("Type: NAK")
        subtree:add(pf.nak, tvb(0, 1))
        consumed_len_in_tvb = 1

        if not pinfo.visited then
            print(string.format("[OASIS_DEBUG] Frame %d: NAK received. Current state before processing NAK: file='%s', is_seq=%s, is_awaiting_ack_data=%s, next_non_seq_rec=%d, last_acked_seq_seg=%d, expected_ack_data_toggle=%d, pending_bytes=%d",
                pinfo.number, tostring(current_file_state.file_name_str), tostring(current_file_state.is_sequential),
                tostring(current_file_state.is_awaiting_ack_for_data_write), current_file_state.current_non_seq_record_num,
                current_file_state.current_seq_write_segment, current_file_state.expected_ack_toggle_for_current_data_write, current_file_state.bytes_from_pending_data_write))

            if current_file_state.is_awaiting_ack_for_data_write and -- Check before it's cleared by NAK processing
               current_file_state.last_data_command_awaiting_ack then

                local context = current_file_state.last_data_command_awaiting_ack
                local fn = context.file_name or (current_file_state.file_name_str or "file")

                if context.cmd_char == string.byte('W') then
                    local type_str = context.is_seq and "Seq" or "Rec"
                    detailed_info_suffix = string.format(" for %s %s #%s", fn, type_str, tostring(context.rec_seg_num or "?"))
                elseif context.cmd_char == string.byte('O') then
                    detailed_info_suffix = string.format(" for OPEN of %s", fn)
                elseif context.cmd_char == string.byte('C') then
                    detailed_info_suffix = string.format(" for CLOSE of %s", fn)
                end
            end

            -- Perform NAK state updates
            if current_file_state.file_name_str and
               current_file_state.is_awaiting_ack_for_data_write then
                print(string.format("[OASIS_DEBUG] Frame %d: NAK - Discarding %d pending bytes. Counters not advanced. is_awaiting_ack_data cleared.",
                    pinfo.number, current_file_state.bytes_from_pending_data_write))
                current_file_state.bytes_from_pending_data_write = 0
                current_file_state.is_awaiting_ack_for_data_write = false
            else
                 print(string.format("[OASIS_DEBUG] Frame %d: NAK - Conditions to process for data_write NOT MET or not relevant.", pinfo.number))
            end
            if not frame_display_details_cache[pinfo.number] then frame_display_details_cache[pinfo.number] = {} end
            frame_display_details_cache[pinfo.number].ack_nak_suffix = detailed_info_suffix
        else -- 2nd pass
            if frame_display_details_cache[pinfo.number] and frame_display_details_cache[pinfo.number].ack_nak_suffix then
                detailed_info_suffix = frame_display_details_cache[pinfo.number].ack_nak_suffix
            end
        end
        pinfo.cols.info:set(nak_msg_base .. detailed_info_suffix)

    elseif first_byte_val == DLE and data_len >= 2 then
        local second_byte_val = tvb(1, 1):uint()
        if second_byte_val == string.byte('0') or second_byte_val == string.byte('1') then -- ACK
            local toggle_val = second_byte_val - string.byte('0') -- Integer 0 or 1
            local ack_msg_base = "ACK" .. toggle_val
            local detailed_info_suffix = ""
            pinfo.cols.src = RECEIVER_ENTITY_NAME; pinfo.cols.dst = SENDER_ENTITY_NAME
            subtree:add(pf.msg_type, tvb(0,2)):set_text("Type: ACK")
            
            -- Updated Main ACK display
            local ack_raw_item_text = "ACK" .. toggle_val .. " Signal"
            local ack_node = subtree:add(pf.ack_raw, tvb:range(0, 2))
            ack_node:set_text(ack_raw_item_text) -- Set text to "ACK0 Signal" or "ACK1 Signal"
            ack_node:add(pf.ack_toggle, tvb:range(1, 1), toggle_val) -- Display integer toggle value
            
            consumed_len_in_tvb = 2

            if not pinfo.visited then
                print(string.format("[OASIS_DEBUG] Frame %d: ACK (Toggle %d) received. Current state BEFORE ACK processing: file='%s', is_seq=%s, is_awaiting_ack_data=%s, next_non_seq_rec=%d, last_acked_seq_seg=%d, expected_ack_data_toggle=%d, pending_bytes=%d",
                    pinfo.number, toggle_val, tostring(current_file_state.file_name_str), tostring(current_file_state.is_sequential),
                    tostring(current_file_state.is_awaiting_ack_for_data_write), current_file_state.current_non_seq_record_num,
                    current_file_state.current_seq_write_segment, current_file_state.expected_ack_toggle_for_current_data_write, current_file_state.bytes_from_pending_data_write))

                if current_file_state.is_awaiting_ack_for_data_write and -- Check before it's cleared by ACK processing
                   current_file_state.last_data_command_awaiting_ack then

                    local context = current_file_state.last_data_command_awaiting_ack
                    local fn = context.file_name or (current_file_state.file_name_str or "file")

                    if context.cmd_char == string.byte('W') then
                        local type_str = context.is_seq and "Seq" or "Rec"
                        detailed_info_suffix = string.format(" for %s %s #%s", fn, type_str, tostring(context.rec_seg_num or "?"))
                    elseif context.cmd_char == string.byte('O') then
                        detailed_info_suffix = string.format(" for OPEN of %s", fn)
                    elseif context.cmd_char == string.byte('C') then
                        detailed_info_suffix = string.format(" for CLOSE of %s", fn)
                    end
                end

                -- Perform ACK state updates
                if current_file_state.file_name_str and
                   current_file_state.is_awaiting_ack_for_data_write then
                    if toggle_val == current_file_state.expected_ack_toggle_for_current_data_write then
                        print(string.format("[OASIS_DEBUG] Frame %d: ACK - CORRECT TOGGLE for data write. Adding %d bytes to total. Total before: %d",
                            pinfo.number, current_file_state.bytes_from_pending_data_write, current_file_state.total_payload_bytes_written_this_file))
                        current_file_state.total_payload_bytes_written_this_file = current_file_state.total_payload_bytes_written_this_file + current_file_state.bytes_from_pending_data_write;
                        current_file_state.expected_ack_toggle_for_current_data_write = current_file_state.expected_ack_toggle_for_current_data_write == 0 and 1 or 0

                        local acked_command_context = current_file_state.last_data_command_awaiting_ack
                        if acked_command_context then
                            if acked_command_context.cmd_char == string.byte('W') then
                                if acked_command_context.is_seq then
                                    current_file_state.current_seq_write_segment = acked_command_context.rec_seg_num
                                    print(string.format("[OASIS_DEBUG] Frame %d: ACK - (Seq WRITE) Successful. current_seq_write_segment updated to %d.", pinfo.number, current_file_state.current_seq_write_segment))
                                else -- Non-sequential WRITE
                                    current_file_state.current_non_seq_record_num = current_file_state.current_non_seq_record_num + 1
                                    print(string.format("[OASIS_DEBUG] Frame %d: ACK - (Non-seq WRITE) Successful. current_non_seq_record_num incremented to %d.", pinfo.number, current_file_state.current_non_seq_record_num))
                                end
                            elseif acked_command_context.cmd_char == string.byte('O') then
                                print(string.format("[OASIS_DEBUG] Frame %d: ACK - (OPEN) Successful. File: %s. Seq status: %s. Initial state: current_seq_seg=%d, current_non_seq_rec=%d",
                                    pinfo.number, acked_command_context.file_name or "N/A", tostring(acked_command_context.is_seq),
                                    current_file_state.current_seq_write_segment, current_file_state.current_non_seq_record_num))
                            elseif acked_command_context.cmd_char == string.byte('C') then
                                print(string.format("[OASIS_DEBUG] Frame %d: ACK - (CLOSE) Successful. File: %s", pinfo.number, acked_command_context.file_name or "N/A"))
                            end
                        end

                        current_file_state.is_awaiting_ack_for_data_write = false
                        current_file_state.bytes_from_pending_data_write = 0;
                        print(string.format("[OASIS_DEBUG] Frame %d: ACK - Processed Correct ACK. Total bytes for file %s: %d. is_awaiting_ack_data cleared. Next expected_ack_data_toggle: %d",
                            pinfo.number, tostring(current_file_state.file_name_str), current_file_state.total_payload_bytes_written_this_file, current_file_state.expected_ack_toggle_for_current_data_write))
                    else
                        print(string.format("[OASIS_DEBUG] Frame %d: ACK - INCORRECT TOGGLE for data write. Expected: %d, Got: %d. NOT adding %d bytes. is_awaiting_ack_data remains true. expected_ack_data_toggle remains %d. No counters updated.",
                            pinfo.number, current_file_state.expected_ack_toggle_for_current_data_write, toggle_val, current_file_state.bytes_from_pending_data_write, current_file_state.expected_ack_toggle_for_current_data_write))
                    end
                else
                    print(string.format("[OASIS_DEBUG] Frame %d: ACK - Conditions to process for data_write NOT MET (e.g. not awaiting data ACK, or no file context).", pinfo.number))
                end
                if not frame_display_details_cache[pinfo.number] then frame_display_details_cache[pinfo.number] = {} end
                frame_display_details_cache[pinfo.number].ack_nak_suffix = detailed_info_suffix
            else -- 2nd pass
                if frame_display_details_cache[pinfo.number] and frame_display_details_cache[pinfo.number].ack_nak_suffix then
                    detailed_info_suffix = frame_display_details_cache[pinfo.number].ack_nak_suffix
                end
            end
            pinfo.cols.info:set(ack_msg_base .. detailed_info_suffix)

        elseif second_byte_val == EOT then
            pinfo.cols.info:set("EOT Signal")
            pinfo.cols.src = SENDER_ENTITY_NAME; pinfo.cols.dst = RECEIVER_ENTITY_NAME
            subtree:add(pf.msg_type, tvb(0,2)):set_text("Type: EOT")
            subtree:add(pf.eot, tvb(0, 2))
            consumed_len_in_tvb = 2
            if not pinfo.visited then
                print(string.format("[OASIS_DEBUG] Frame %d: EOT processed.", pinfo.number))
                reset_current_file_state(pinfo.number)
            end
        elseif second_byte_val == STX then -- Data Packet
            pinfo.cols.src = SENDER_ENTITY_NAME; pinfo.cols.dst = RECEIVER_ENTITY_NAME
            if data_len < 7 then -- DLE STX CMD (DLE ETX LRC RUB) -> Min 1 byte payload for this check
                pinfo.cols.info:set("Data Packet (Too Short)")
                subtree:add(pf.msg_type, tvb:range(0,0)):set_text("Type: Data Packet (Malformed - Too Short for basic structure)")
                subtree:add(pf.packet_raw_data, tvb)
                consumed_len_in_tvb = data_len
            else
                subtree:add(pf.msg_type, tvb:range(0,0)):set_text("Type: Data Packet")

                local cmd_val = tvb(2, 1):uint()
                local cmd_char = string.char(cmd_val)
                local cmd_name = command_names[cmd_val] or "Unknown"
                subtree:add(pf.packet_cmd, tvb(2, 1))

                local cmd_info_str_built = "Cmd: " .. cmd_name .." (" .. cmd_char .. ")"

                local display_details = get_command_display_details(pinfo, cmd_val, current_file_state, frame_display_details_cache)

                if not pinfo.visited then
                    if cmd_val == string.byte('W') then
                        current_file_state.bytes_from_pending_data_write = 0
                        current_file_state.last_data_command_awaiting_ack = {
                            cmd_char = cmd_val,
                            file_name = display_details.file_name,
                            rec_seg_num = display_details.display_rec_num,
                            is_seq = display_details.is_seq
                        }
                    elseif cmd_val == string.byte('C') then
                        current_file_state.bytes_from_pending_data_write = 0
                        current_file_state.last_data_command_awaiting_ack = {
                            cmd_char = cmd_val,
                            file_name = display_details.file_name,
                            is_seq = current_file_state.is_sequential -- Use current state for is_seq for CLOSE
                        }
                    elseif cmd_val == string.byte('O') then
                         current_file_state.bytes_from_pending_data_write = 0
                         -- last_data_command_awaiting_ack for OPEN is set after DEB parsing
                    end
                end

                if cmd_val == string.byte('O') then
                    -- Filename for OPEN is determined after DEB parsing.
                    -- The 'fname' in display_details might be from a previous file if not careful.
                    -- It will be updated by parse_deb_and_update_state and then used.
                    -- For initial info string, it might be generic or use cached value if available.
                    if display_details.fname and display_details.fname ~= "" and display_details.fname ~= "[CacheErrOPEN]" then
                         cmd_info_str_built = "Cmd: OPEN (O), File: " .. display_details.fname
                         subtree:add(pf.packet_current_filename, display_details.fname)
                    else
                         cmd_info_str_built = "Cmd: OPEN (O)"
                    end
                elseif cmd_val == string.byte('W') then
                    if display_details.file_name and display_details.file_name ~= "" then
                        subtree:add(pf.packet_current_filename, display_details.file_name)
                        cmd_info_str_built = cmd_info_str_built .. ", File: " .. display_details.file_name
                    end
                    if display_details.display_rec_num then
                        subtree:add(pf.packet_write_segment, display_details.display_rec_num)
                        local write_label = display_details.is_seq and "Seq Write" or "Record"
                        cmd_info_str_built = cmd_info_str_built .. ", " .. write_label .. " #: " .. tostring(display_details.display_rec_num)
                    end
                elseif cmd_val == string.byte('C') then
                    if display_details.file_name and display_details.file_name ~= "" then
                        cmd_info_str_built = cmd_info_str_built .. ", File: " .. display_details.file_name
                        subtree:add(pf.packet_current_filename, display_details.file_name)
                    end
                end

                local trailer_start_offset = -1
                -- Search for DLE ETX, tvb relative offsets
                -- DLE STX <CMD> <Payload> DLE ETX <LRC> <RUB>
                --  0   1    2     3...k   k+1 k+2  k+3   k+4 (example payload len k-3)
                -- trailer_start_offset would be 'k' (index of DLE in DLE ETX)
                for k = 3, data_len - 2 do -- k is offset in tvb for DLE of DLE ETX
                    local pcall_k_status, k_byte_val = pcall(function() return tvb(k,1):uint() end)
                    local pcall_k1_status, k1_byte_val = pcall(function() return tvb(k+1,1):uint() end)

                    if pcall_k_status and pcall_k1_status and k_byte_val == DLE and k1_byte_val == ETX then
                        local is_actual_trailer = true
                        -- Check if this DLE ETX is escaped (preceded by an odd number of DLEs)
                        if k > 0 then -- Ensure there's a byte before DLE of potential DLE ETX
                            local pcall_prev_status, prev_byte_val_check = pcall(function() return tvb(k-1,1):uint() end)
                            if pcall_prev_status and prev_byte_val_check == DLE then
                                local dle_count = 0
                                local temp_idx = k - 1
                                while temp_idx >= 0 do
                                    local pcall_temp_status, temp_byte_val_inner = pcall(function() return tvb(temp_idx,1):uint() end)
                                    if pcall_temp_status and temp_byte_val_inner == DLE then
                                        dle_count = dle_count + 1
                                        temp_idx = temp_idx - 1
                                    else
                                        break
                                    end
                                end
                                if dle_count % 2 == 1 then -- Odd number of DLEs means the DLE ETX's DLE is escaped
                                    is_actual_trailer = false
                                end
                            end
                        end
                        if is_actual_trailer then
                            trailer_start_offset = k
                            break
                        end
                    elseif not (pcall_k_status and pcall_k1_status) then
                        -- This case indicates an issue reading from TVB, which is unlikely if length checks are okay.
                        -- It might mean data_len is too small for the loop's upper bound if not handled by prior length check.
                        subtree:add_tvb_expert_info(expert_info.packet_trailer_not_found, tvb, "Error accessing TVB while searching for DLE ETX trailer")
                        trailer_start_offset = -1 -- Explicitly mark as not found
                        break
                    end
                end

                if trailer_start_offset ~= -1 then
                    -- DLE STX <CMD> ...payload... DLE ETX <LRC> <RUB>
                    -- 0   1    2      3 to k-1      k  k+1   k+2   k+3
                    -- Raw payload is from offset 3 up to trailer_start_offset-1
                    local raw_payload_len = trailer_start_offset - 3
                    local payload_node

                    if cmd_val == string.byte('C') then
                        payload_node = subtree:add(oasis_proto, tvb:range(3,0), "Payload Details") -- Placeholder for structure
                        if raw_payload_len == 0 then
                            payload_node:add(pf.packet_payload_raw_stuffed, tvb:range(3,0)):set_text("Payload: (Empty - Normal for CLOSE)")
                        else
                            local unexpected_payload_tvbr = tvb:range(3, raw_payload_len)
                            payload_node:add(pf.packet_payload_raw_stuffed, unexpected_payload_tvbr):append_text(" (ERROR: Unexpected payload for CLOSE command)")
                            payload_node:add_tvb_expert_info(expert_info.payload_decode_error, unexpected_payload_tvbr, "CLOSE packet has unexpected payload data")
                        end

                        if display_details.total_bytes_close and display_details.total_bytes_close > 0 then
                             subtree:add(pf.packet_total_bytes_transferred, display_details.total_bytes_close):set_text(string.format("File Summary: %d bytes transferred", display_details.total_bytes_close))
                             cmd_info_str_built = cmd_info_str_built .. ", " .. (display_details.total_bytes_close or 0) .. " bytes transferred"
                        elseif display_details.total_bytes_close == 0 then
                             cmd_info_str_built = cmd_info_str_built .. ", 0 bytes transferred"
                        end

                        if not pinfo.visited then
                           print(string.format("[OASIS_DEBUG] Frame %d: CLOSE processed. File: %s", pinfo.number, tostring(current_file_state.file_name_str)))
                        end

                    elseif raw_payload_len >= 0 then -- Can be 0 for OPEN/WRITE if no data
                        local raw_payload_tvbr = tvb:range(3, raw_payload_len)
                        payload_node = subtree:add(oasis_proto, raw_payload_tvbr, "Payload Details")

                        if raw_payload_len > 0 then
                            payload_node:add(pf.packet_payload_raw_stuffed, raw_payload_tvbr)
                            local decoded_payload_tvbr, decode_err = decode_oasis_payload_to_tvb(raw_payload_tvbr, payload_node)

                            if decoded_payload_tvbr then
                                local decoded_len = decoded_payload_tvbr:len()
                                if decoded_len > 0 then
                                    payload_node:add(pf.packet_payload_decoded_bytes, decoded_payload_tvbr())
                                elseif cmd_val ~= string.byte('O') then -- OPEN can have empty decoded if raw was just RLE/DLE sequences resolving to nothing
                                     payload_node:add(pf.packet_payload_decoded_bytes, decoded_payload_tvbr()):set_text("Payload (Decoded Bytes): (Empty after RLE/DLE processing)")
                                end

                                if not pinfo.visited and cmd_val == string.byte('W') then
                                    local data_part_len_for_sum = decoded_len
                                    if current_file_state.is_sequential and decoded_len >= 2 then
                                        data_part_len_for_sum = decoded_len - 2
                                    end
                                    current_file_state.bytes_from_pending_data_write = data_part_len_for_sum
                                    print(string.format("[OASIS_DEBUG] Frame %d: WRITE payload processed, file='%s', is_seq=%s, pending_payload_bytes=%d for seg/rec %d",
                                        pinfo.number, tostring(current_file_state.file_name_str), tostring(current_file_state.is_sequential),
                                        current_file_state.bytes_from_pending_data_write, display_details.display_rec_num))
                                end

                                if cmd_val == string.byte('O') then
                                    if decoded_len >= 32 then
                                        local deb_tvbr = decoded_payload_tvbr:range(0, 32)
                                        local deb_node = payload_node:add(oasis_proto, deb_tvbr, "Directory Entry Block (DEB)")
                                        parse_deb_and_update_state(deb_tvbr, deb_node, current_file_state, pinfo)
                                        if not pinfo.visited then
                                            -- Update last_data_command_awaiting_ack for OPEN *after* DEB is parsed and current_file_state is updated
                                            current_file_state.last_data_command_awaiting_ack = {
                                                cmd_char = cmd_val,
                                                file_name = current_file_state.file_name_str, -- Use newly parsed name
                                                is_seq = current_file_state.is_sequential    -- Use newly parsed seq status
                                            }
                                            if current_file_state.file_name_str then
                                                 cmd_info_str_built = "Cmd: OPEN (O), File: " .. current_file_state.file_name_str
                                                 -- Update filename in subtree if it exists, or add it
                                                 local fn_field_item = subtree:get_child_by_name("oasis.packet.current_file")
                                                 if fn_field_item then fn_field_item:replace(current_file_state.file_name_str) else subtree:add(pf.packet_current_filename, current_file_state.file_name_str) end
                                            end
                                        end
                                    else
                                        payload_node:add_tvb_expert_info(expert_info.open_payload_too_short, decoded_payload_tvbr())
                                        cmd_info_str_built = cmd_info_str_built .. " [DEB Too Short]"
                                    end
                                elseif cmd_val == string.byte('W') then
                                    if current_file_state.is_sequential and decoded_len >= 2 then
                                        local link_offset = decoded_len - 2
                                        payload_node:add_le(pf.packet_payload_seq_link, decoded_payload_tvbr:range(link_offset, 2))
                                        cmd_info_str_built = cmd_info_str_built .. ", SeqLink: " .. tostring(decoded_payload_tvbr(link_offset, 2):le_uint())
                                    end
                                end
                            else -- decode_err is not nil
                                payload_node:add_tvb_expert_info(expert_info.payload_decode_error, raw_payload_tvbr, decode_err or "Unknown payload decode error")
                                cmd_info_str_built = cmd_info_str_built .. " [Payload Decode Error]"
                            end
                        elseif cmd_val == string.byte('O') then -- Raw payload was empty
                            payload_node:add(pf.packet_payload_raw_stuffed, raw_payload_tvbr):set_text("Payload: (Empty - Invalid for OPEN)")
                            payload_node:add_tvb_expert_info(expert_info.open_payload_too_short, raw_payload_tvbr)
                            cmd_info_str_built = cmd_info_str_built .. " [Empty Payload for OPEN]"
                        else -- Raw payload empty, not OPEN (e.g. WRITE with no data, though unusual)
                            payload_node:add(pf.packet_payload_raw_stuffed, raw_payload_tvbr):set_text("Payload: (Empty)")
                        end
                    end

                    subtree:add(pf.packet_trailer_dle_etx, tvb:range(trailer_start_offset, 2))
                    local lrc_byte_offset = trailer_start_offset + 2
                    if lrc_byte_offset < data_len then
                        local received_lrc_val = tvb(lrc_byte_offset, 1):uint()
                        local lrc_node = subtree:add(pf.packet_lrc_received, tvb:range(lrc_byte_offset, 1))
                        -- LRC calculation: from DLE of DLE STX up to ETX of DLE ETX.
                        -- In tvb, this is from offset 0 up to trailer_start_offset + 1 (inclusive of ETX)
                        local lrc_calc_val = calculate_oasis_lrc(tvb, 0, trailer_start_offset + 2) -- Length is (offset_of_ETX_inclusive - offset_of_DLE_STX_DLE) + 1
                        subtree:add(pf.packet_lrc_calculated, lrc_calc_val)
                        if lrc_calc_val ~= received_lrc_val then
                            lrc_node:add_tvb_expert_info(expert_info.lrc_mismatch, tvb:range(lrc_byte_offset, 1))
                            lrc_node:append_text(string.format(" [Mismatch! Calculated: 0x%02X]", lrc_calc_val))
                            cmd_info_str_built = cmd_info_str_built .. " [LRC Mismatch]"
                        else
                            lrc_node:append_text(" [Correct]")
                        end

                        local final_byte_offset_val = lrc_byte_offset + 1
                        if final_byte_offset_val < data_len then
                            local final_byte_read = tvb(final_byte_offset_val, 1):uint()
                            local final_byte_node = subtree:add(pf.packet_final_rub_byte, tvb:range(final_byte_offset_val, 1))
                            if final_byte_read ~= RUB then
                                final_byte_node:add_tvb_expert_info(expert_info.final_rub_byte_incorrect, tvb:range(final_byte_offset_val, 1))
                                final_byte_node:append_text(string.format(" [Expected 0x%02X]", RUB))
                                cmd_info_str_built = cmd_info_str_built .. " [Wrong Final Byte]"
                            else
                                final_byte_node:append_text(" [Correct]")
                            end
                            consumed_len_in_tvb = final_byte_offset_val + 1
                        else
                            subtree:add_tvb_expert_info(expert_info.final_rub_byte_missing, tvb:range(lrc_byte_offset,1))
                            cmd_info_str_built = cmd_info_str_built .. " [Final Byte Missing]"
                            consumed_len_in_tvb = lrc_byte_offset + 1
                        end
                    else
                        subtree:add_tvb_expert_info(expert_info.lrc_byte_missing, tvb:range(trailer_start_offset, 2))
                        cmd_info_str_built = cmd_info_str_built .. " [LRC Missing]"
                        consumed_len_in_tvb = trailer_start_offset + 2
                    end
                else -- Trailer (DLE ETX) not found
                    subtree:add_tvb_expert_info(expert_info.packet_trailer_not_found, tvb)
                    cmd_info_str_built = cmd_info_str_built .. " (Malformed - No DLE ETX Trailer)"
                    if data_len > 3 then subtree:add(pf.packet_payload_raw_stuffed, tvb:range(3)) end
                    consumed_len_in_tvb = data_len -- Consume rest if malformed
                end
                pinfo.cols.info:set("Data Packet, " .. cmd_info_str_built)
            end
        else -- DLE followed by something other than '0', '1', EOT, STX
            pinfo.cols.info:set("Unknown DLE Sequence")
            subtree:add(pf.msg_type, tvb:range(0,0)):set_text("Type: Unknown DLE Sequence")
            subtree:add(pf.packet_raw_data, tvb); consumed_len_in_tvb = data_len
        end
    else -- First byte of message was not ENQ, NAK, or DLE
        pinfo.cols.info:set("Unknown/Invalid Data (Not ENQ, NAK, or DLE-prefixed)")
        subtree:add(pf.msg_type, tvb:range(0,0)):set_text("Type: Unknown/Invalid")
        subtree:add(pf.packet_raw_data, tvb); consumed_len_in_tvb = data_len
    end

    -- Trailing data handling: after the recognized OASIS message part (within tvb)
    local current_trailing_offset_in_tvb = consumed_len_in_tvb
    local unexpected_data_start_offset_in_tvb = -1
    local unexpected_data_accumulator = {}

    local function finalize_pending_unexpected_data_in_tvb(current_pos_in_tvb)
        if unexpected_data_start_offset_in_tvb ~= -1 then
            local len = current_pos_in_tvb - unexpected_data_start_offset_in_tvb
            if len > 0 then
                table.insert(unexpected_data_accumulator, tvb:range(unexpected_data_start_offset_in_tvb, len))
            end
            unexpected_data_start_offset_in_tvb = -1
        end
    end

    while current_trailing_offset_in_tvb < data_len do
        local pcall_status_curr, byte_val = pcall(function() return tvb(current_trailing_offset_in_tvb, 1):uint() end)
        if not pcall_status_curr then break end

        local is_known_signal = false

        if byte_val == ENQ then
            finalize_pending_unexpected_data_in_tvb(current_trailing_offset_in_tvb)
            subtree:add(pf.trailing_enq_signal, tvb:range(current_trailing_offset_in_tvb, 1))
            current_trailing_offset_in_tvb = current_trailing_offset_in_tvb + 1
            is_known_signal = true
        elseif byte_val == RUB then
            finalize_pending_unexpected_data_in_tvb(current_trailing_offset_in_tvb)
            subtree:add(pf.trailing_rub_signal, tvb:range(current_trailing_offset_in_tvb, 1))
            current_trailing_offset_in_tvb = current_trailing_offset_in_tvb + 1
            is_known_signal = true
        elseif byte_val == DLE then
            if current_trailing_offset_in_tvb + 1 < data_len then
                local pcall_status_next, next_byte_val = pcall(function() return tvb(current_trailing_offset_in_tvb + 1, 1):uint() end)
                if pcall_status_next then
                    local toggle_char_val = next_byte_val
                    if toggle_char_val == string.byte('0') then
                        finalize_pending_unexpected_data_in_tvb(current_trailing_offset_in_tvb)
                        local signal_node = subtree:add(pf.trailing_ack0_signal, tvb:range(current_trailing_offset_in_tvb, 2))
                        signal_node:add(pf.ack_toggle, tvb:range(current_trailing_offset_in_tvb + 1, 1), 0) -- Add toggle 0
                        current_trailing_offset_in_tvb = current_trailing_offset_in_tvb + 2
                        is_known_signal = true
                    elseif toggle_char_val == string.byte('1') then
                        finalize_pending_unexpected_data_in_tvb(current_trailing_offset_in_tvb)
                        local signal_node = subtree:add(pf.trailing_ack1_signal, tvb:range(current_trailing_offset_in_tvb, 2))
                        signal_node:add(pf.ack_toggle, tvb:range(current_trailing_offset_in_tvb + 1, 1), 1) -- Add toggle 1
                        current_trailing_offset_in_tvb = current_trailing_offset_in_tvb + 2
                        is_known_signal = true
                    end
                end
            end
        end

        if not is_known_signal then
            if unexpected_data_start_offset_in_tvb == -1 then
                unexpected_data_start_offset_in_tvb = current_trailing_offset_in_tvb
            end
            current_trailing_offset_in_tvb = current_trailing_offset_in_tvb + 1
        end
    end

    finalize_pending_unexpected_data_in_tvb(data_len)

    if #unexpected_data_accumulator > 0 then
        for _, tvbr in ipairs(unexpected_data_accumulator) do
            local unexp_node = subtree:add(pf.unexpected_trailing_data, tvbr)
            unexp_node:add_tvb_expert_info(expert_info.unexpected_trailing_bytes, tvbr)
        end
    end

    -- The dissector should consume the entire TVB it was given,
    -- which includes the pseudo-header and the OASIS message.
    return total_len
end

local rtacser_data_table = DissectorTable.get("rtacser.data")
if rtacser_data_table then
    rtacser_data_table:add_for_decode_as(oasis_proto)
    print("OASIS Wireshark Dissector registered as a subdissector for rtacser.data")
else
    print("OASIS Wireshark Dissector: Could not find DissectorTable 'rtacser.data'. Subdissector registration failed.")
end
