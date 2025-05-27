-- Wireshark Lua Dissector for OASIS Protocol (LINKTYPE_USER2 / 149)
-- www.github.com/hharte/oasis-utils
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
local DLE = 0x10
local SI  = 0x0F
local SO  = 0x0E
local VT  = 0x0B -- Used for RLE count
local CAN = 0x18 -- DLE+CAN represents ESC
local ESC = 0x1B
local SUB = 0x1A -- Substitute (Used as EOF marker in ASCII mode text files)
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

-- Global state for the currently active file transfer (for sequential processing)
local current_file_state = {
    file_name_str = nil,
    is_sequential = false,
    transient_write_count = 0, -- Used for calculating sequence during initial pass
    total_payload_bytes_written = 0,
    total_records_in_file = 0
}

-- Global cache for storing calculated write sequence details per frame
-- Key: frame_number, Value: {file_name = "...", seq_num = N, total_recs = T, is_seq = true/false}
local frame_write_details_cache = {}

-- Function to reset the current file state (and transient counters for cache building)
local function reset_current_file_state_and_transient_counters()
    current_file_state.file_name_str = nil
    current_file_state.is_sequential = false
    current_file_state.transient_write_count = 0 -- Reset for the new file context
    current_file_state.total_payload_bytes_written = 0
    current_file_state.total_records_in_file = 0
end

-- Dissector's init function, called when script is loaded/reloaded
function oasis_proto.init()
    frame_write_details_cache = {} -- Clear cache on script load/reload
    reset_current_file_state_and_transient_counters()
end


-- Protocol Fields Definition
local pf = {
    direction = ProtoField.uint8("oasis.direction", "Direction", base.HEX, { [0x00] = "RX", [0x01] = "TX" }),
    msg_type = ProtoField.string("oasis.type", "Message Type"),

    enq = ProtoField.bytes("oasis.enq", "ENQ Signal", base.NONE),
    eot = ProtoField.bytes("oasis.eot", "EOT Signal", base.NONE),

    ack_raw = ProtoField.bytes("oasis.ack.raw", "ACK Signal (Raw)", base.NONE),
    ack_toggle = ProtoField.uint8("oasis.ack.toggle", "ACK Toggle ('0' or '1')", base.DEC_HEX),

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

    extra_data_bytes = ProtoField.bytes("oasis.extra_data_bytes", "Extra Trailing Data Bytes", base.NONE)
}
oasis_proto.fields = pf

-- Expert Info Fields
local expert_info = {
    lrc_mismatch = ProtoExpert.new("oasis.lrc.mismatch", "LRC Checksum Mismatch", expert.group.CHECKSUM, expert.severity.ERROR),
    open_payload_too_short = ProtoExpert.new("oasis.deb.payload_too_short", "OPEN packet payload is too short for a full DEB", expert.group.MALFORMED, expert.severity.WARN),
    invalid_deb_file_format = ProtoExpert.new("oasis.deb.invalid_file_format", "DEB file format type is unknown or invalid for the context", expert.group.MALFORMED, expert.severity.WARN),
    extra_trailing_data = ProtoExpert.new("oasis.extra_trailing_data", "Extra trailing data found after the recognized message", expert.group.MALFORMED, expert.severity.NOTE),
    packet_trailer_not_found = ProtoExpert.new("oasis.packet.trailer_not_found", "Packet trailer (DLE ETX) was not found as expected", expert.group.MALFORMED, expert.severity.WARN),
    lrc_byte_missing = ProtoExpert.new("oasis.packet.lrc_byte_missing", "LRC byte is missing after DLE ETX trailer", expert.group.MALFORMED, expert.severity.WARN),
    payload_decode_error = ProtoExpert.new("oasis.packet.payload_decode_error", "An error occurred during payload decoding (e.g., DLE stuffing or RLE)", expert.group.MALFORMED, expert.severity.ERROR),
    rle_vt_error = ProtoExpert.new("oasis.packet.rle_vt_error", "DLE VT (Run-Length Encoding) compression error", expert.group.MALFORMED, expert.severity.WARN),
    final_rub_byte_missing = ProtoExpert.new("oasis.packet.final_rub_byte_missing", "Required final padding byte (0x7F RUB) is missing after LRC", expert.group.MALFORMED, expert.severity.WARN),
    final_rub_byte_incorrect = ProtoExpert.new("oasis.packet.final_rub_byte_incorrect", "Incorrect final padding byte value (expected 0x7F RUB)", expert.group.MALFORMED, expert.severity.WARN)
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
        table.insert(str_tbl, string.char(byte)) -- Directly convert byte to char
    end
    return table.concat(str_tbl)
end


local function decode_oasis_payload_to_tvb(raw_payload_tvb, pinfo, tree_node)
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
                if tree_node then pinfo:expert(expert_info.payload_decode_error, raw_payload_tvb:range(i-1,1),"Unexpected end after DLE in payload") end
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
                    if tree_node then pinfo:expert(expert_info.rle_vt_error, raw_payload_tvb:range(i-2,2),"Unexpected end after DLE VT") end
                    return nil, "Unexpected end after DLE VT"
                end
                if last_written_char_code == nil then
                     if tree_node then pinfo:expert(expert_info.rle_vt_error, raw_payload_tvb:range(i-2,2),"DLE VT used before any character outputted") end
                    return nil, "DLE VT used before any character was outputted"
                end

                local pcall_count_status, count_byte_or_err = pcall(function() return raw_payload_tvb(i, 1):uint() end)
                if not pcall_count_status then return nil, "Error reading RLE count byte: " .. tostring(count_byte_or_err) end
                local rle_count_val = count_byte_or_err
                i = i + 1

                if rle_count_val == DLE then
                    if i >= raw_payload_len then
                        if tree_node then pinfo:expert(expert_info.rle_vt_error, raw_payload_tvb:range(i-3,3), "Unexpected end after DLE VT DLE (for escaped count)") end
                        return nil, "Unexpected end after DLE VT DLE (for escaped count)"
                    end
                    local pcall_esc_count_status, esc_count_byte_or_err = pcall(function() return raw_payload_tvb(i, 1):uint() end)
                    if not pcall_esc_count_status then return nil, "Error reading escaped RLE count byte: " .. tostring(esc_count_byte_or_err) end
                    local esc_count_val = esc_count_byte_or_err
                    i = i + 1
                    if esc_count_val == DLE then rle_count_val = DLE
                    elseif esc_count_val == CAN then rle_count_val = ESC
                    else
                        if tree_node then pinfo:expert(expert_info.rle_vt_error, raw_payload_tvb:range(i-4,4), "Invalid escaped RLE count value: " .. esc_count_val) end
                        return nil, "Invalid DLE-escaped RLE count value: " .. esc_count_val
                    end
                end
                for _ = 1, rle_count_val do
                    table.insert(byte_accumulator, last_written_char_code + current_shift_state)
                end
            else
                if tree_node then pinfo:expert(expert_info.payload_decode_error, raw_payload_tvb:range(i-2,2), "Unknown DLE sequence: DLE + 0x" .. string.format("%02X", dle_arg)) end
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


--------------------------------------------------------------------------------
-- Main Dissector Function
--------------------------------------------------------------------------------
function oasis_proto.dissector(tvb, pinfo, tree)
    pinfo.cols.protocol = oasis_proto.name
    local total_len = tvb:len()

    if total_len < 1 then
        pinfo.cols.info:set("Frame Too Short (Missing Direction Byte)")
        return 0
    end

    tree:add(pf.direction, tvb(0, 1))
    if total_len < 2 then
        pinfo.cols.info:set("Direction Byte Only")
        return 1
    end

    local data_tvb = tvb:range(1)
    local data_len = data_tvb:len()
    local subtree = tree:add(oasis_proto, data_tvb)
    subtree:set_text("OASIS Protocol Data (" .. data_len .. " bytes)")

    local first_byte_val = data_tvb(0, 1):uint()
    local consumed_len = 0

    -- Set default Source/Destination, can be overridden by specific message types
    pinfo.cols.src = "?"
    pinfo.cols.dst = "?"

    if first_byte_val == ENQ and data_len >= 1 then
        pinfo.cols.info:set("ENQ Signal")
        pinfo.cols.src = SENDER_ENTITY_NAME
        pinfo.cols.dst = RECEIVER_ENTITY_NAME
        subtree:add(pf.msg_type, data_tvb(0,1)):set_text("Type: ENQ")
        subtree:add(pf.enq, data_tvb(0, 1))
        consumed_len = 1
    elseif first_byte_val == DLE and data_len >= 2 then
        local second_byte_val = data_tvb(1, 1):uint()
        if second_byte_val == string.byte('0') or second_byte_val == string.byte('1') then
            local toggle_val = second_byte_val - string.byte('0')
            pinfo.cols.info:set("ACK (Toggle " .. toggle_val .. ")")
            pinfo.cols.src = RECEIVER_ENTITY_NAME
            pinfo.cols.dst = SENDER_ENTITY_NAME
            subtree:add(pf.msg_type, data_tvb(0,2)):set_text("Type: ACK")
            local ack_node = subtree:add(pf.ack_raw, data_tvb(0, 2))
            ack_node:add(pf.ack_toggle, data_tvb(1, 1))
            consumed_len = 2
        elseif second_byte_val == EOT then
            pinfo.cols.info:set("EOT Signal")
            pinfo.cols.src = SENDER_ENTITY_NAME
            pinfo.cols.dst = RECEIVER_ENTITY_NAME
            subtree:add(pf.msg_type, data_tvb(0,2)):set_text("Type: EOT")
            subtree:add(pf.eot, data_tvb(0, 2))
            consumed_len = 2
            reset_current_file_state_and_transient_counters()
        elseif second_byte_val == STX then
             pinfo.cols.src = SENDER_ENTITY_NAME
             pinfo.cols.dst = RECEIVER_ENTITY_NAME
            if data_len < 7 then -- Minimum DLE STX CMD DLE ETX LRC RUB
                pinfo.cols.info:set("Data Packet (Too Short)")
                subtree:add(pf.msg_type, data_tvb:range(0,0)):set_text("Type: Data Packet (Malformed - Too Short)")
                subtree:add(pf.packet_raw_data, data_tvb)
                consumed_len = data_len
            else
                pinfo.cols.info:set("Data Packet") -- Base info, will be refined
                subtree:add(pf.msg_type, data_tvb:range(0,0)):set_text("Type: Data Packet")

                local packet_node = subtree:add(pf.packet_raw_data, data_tvb)
                packet_node:set_text("Packet Details")

                local cmd_val = data_tvb(2, 1):uint()
                local cmd_char = string.char(cmd_val)
                local cmd_name = command_names[cmd_val] or "Unknown"

                packet_node:add(pf.packet_cmd, data_tvb(2, 1))
                local cmd_info_str = "Cmd: " .. cmd_name .." (" .. cmd_char .. ")"

                if cmd_val == string.byte('O') then
                    reset_current_file_state_and_transient_counters()
                end

                -- File name is set during OPEN payload processing, so handle it there for info column
                -- For WRITE/CLOSE, current_file_state.file_name_str should already be set.
                if current_file_state.file_name_str and cmd_val ~= string.byte('O') then
                    cmd_info_str = cmd_info_str .. ", File: " .. current_file_state.file_name_str
                    packet_node:add(pf.packet_current_filename, current_file_state.file_name_str)
                end

                if cmd_val == string.byte('W') then
                    local write_display_label = "Record"
                    local write_display_count_str = "?"
                    local actual_seq_num_for_tree = 0

                    local cached_detail = frame_write_details_cache[pinfo.number]
                    local use_cache = false
                    if cached_detail and cached_detail.file_name == current_file_state.file_name_str then
                        use_cache = true
                    end

                    if not pinfo.visited or not use_cache then
                        current_file_state.transient_write_count = current_file_state.transient_write_count + 1
                        actual_seq_num_for_tree = current_file_state.transient_write_count

                        frame_write_details_cache[pinfo.number] = {
                            file_name = current_file_state.file_name_str,
                            seq_num = actual_seq_num_for_tree,
                            total_recs = current_file_state.total_records_in_file,
                            is_seq = current_file_state.is_sequential
                        }
                        if current_file_state.is_sequential then
                            write_display_label = "Seq Write"
                            write_display_count_str = tostring(actual_seq_num_for_tree)
                        else
                            write_display_label = "Record"
                            if current_file_state.total_records_in_file > 0 then
                                write_display_count_str = string.format("%d / %d", actual_seq_num_for_tree, current_file_state.total_records_in_file)
                            else
                                write_display_count_str = tostring(actual_seq_num_for_tree)
                            end
                        end
                    else
                        actual_seq_num_for_tree = cached_detail.seq_num
                        if cached_detail.is_seq then
                            write_display_label = "Seq Write"
                            write_display_count_str = tostring(cached_detail.seq_num)
                        else
                            write_display_label = "Record"
                            if cached_detail.total_recs > 0 then
                                write_display_count_str = string.format("%d / %d", cached_detail.seq_num, cached_detail.total_recs)
                            else
                                write_display_count_str = tostring(cached_detail.seq_num)
                            end
                        end
                    end

                    cmd_info_str = cmd_info_str .. ", " .. write_display_label .. " #: " .. write_display_count_str
                end
                
                pinfo.cols.info:set("Data Packet, " .. cmd_info_str)


                local trailer_start_offset = -1
                for k = 3, data_len - 2 do
                    local pcall_k_status, k_byte_val = pcall(function() return data_tvb(k,1):uint() end)
                    local pcall_k1_status, k1_byte_val = pcall(function() return data_tvb(k+1,1):uint() end)

                    if pcall_k_status and pcall_k1_status and k_byte_val == DLE and k1_byte_val == ETX then
                        local is_actual_trailer = true
                        if k > 0 then
                            local pcall_prev_status, prev_byte_val_check = pcall(function() return data_tvb(k-1,1):uint() end)
                            if pcall_prev_status and prev_byte_val_check == DLE then
                                local dle_count = 0
                                local temp_idx = k - 1
                                while temp_idx >= 0 do
                                    local pcall_temp_status, temp_byte_val_inner = pcall(function() return data_tvb(temp_idx,1):uint() end)
                                    if pcall_temp_status and temp_byte_val_inner == DLE then
                                        dle_count = dle_count + 1
                                        temp_idx = temp_idx - 1
                                    else
                                        break
                                    end
                                end
                                if dle_count % 2 == 1 then
                                    is_actual_trailer = false
                                end
                            end
                        end
                        if is_actual_trailer then
                            trailer_start_offset = k
                            break
                        end
                    elseif not (pcall_k_status and pcall_k1_status) then
                        pinfo:expert(expert_info.packet_trailer_not_found, data_tvb, "Error accessing TVB while searching for trailer")
                        trailer_start_offset = -1
                        break
                    end
                end

                if trailer_start_offset ~= -1 then
                    local raw_payload_len = trailer_start_offset - 3

                    if cmd_val == string.byte('C') then
                        if current_file_state.file_name_str then
                           pinfo.cols.info:set(string.format("Data Packet, Cmd: CLOSE (C), File: %s, length: %d bytes", current_file_state.file_name_str, current_file_state.total_payload_bytes_written))
                        else -- Should ideally not happen if OPEN was processed
                           pinfo.cols.info:set(string.format("Data Packet, Cmd: CLOSE (C), length: %d bytes", current_file_state.total_payload_bytes_written))
                        end

                        local payload_node = packet_node:add(oasis_proto, data_tvb:range(3,0), "Payload Details")
                        if raw_payload_len == 0 then
                            payload_node:add(pf.packet_payload_raw_stuffed, data_tvb:range(3,0))
                                :set_text("Payload: (Empty - Normal for CLOSE)")
                        else
                            local unexpected_payload_tvbr = data_tvb:range(3, raw_payload_len)
                            payload_node:add(pf.packet_payload_raw_stuffed, unexpected_payload_tvbr)
                                :append_text(" (ERROR: Unexpected payload for CLOSE command)")
                            pinfo:expert(expert_info.payload_decode_error, unexpected_payload_tvbr, "CLOSE packet has unexpected payload data")
                        end
                        
                        if current_file_state.file_name_str and current_file_state.total_payload_bytes_written > 0 then
                            local summary_item = packet_node:add(pf.packet_total_bytes_transferred, current_file_state.total_payload_bytes_written)
                            summary_item:set_text(string.format("File Summary: %d bytes transferred", current_file_state.total_payload_bytes_written))
                        end
                        reset_current_file_state_and_transient_counters()

                    elseif raw_payload_len >= 0 then -- For OPEN and WRITE
                        local raw_payload_tvbr = data_tvb:range(3, raw_payload_len)
                        local payload_node = packet_node:add(oasis_proto, raw_payload_tvbr, "Payload Details")

                        if raw_payload_len > 0 then
                            payload_node:add(pf.packet_payload_raw_stuffed, raw_payload_tvbr)
                            local decoded_payload_tvbr, decode_err = decode_oasis_payload_to_tvb(raw_payload_tvbr, pinfo, payload_node)

                            if decoded_payload_tvbr then
                                local decoded_len_for_sum = decoded_payload_tvbr:len()
                                if decoded_len_for_sum > 0 then
                                    payload_node:add(pf.packet_payload_decoded_bytes, decoded_payload_tvbr())
                                elseif cmd_val ~= string.byte('O') then
                                     payload_node:add(pf.packet_payload_decoded_bytes, decoded_payload_tvbr())
                                         :set_text("Payload (Decoded Bytes): (Empty after RLE/DLE processing)")
                                end

                                if cmd_val == string.byte('O') then
                                    if decoded_payload_tvbr:len() >= 32 then
                                        local deb_tvbr = decoded_payload_tvbr:range(0, 32)
                                        local deb_node = payload_node:add(oasis_proto, deb_tvbr, "Directory Entry Block (DEB)")
                                        deb_node:add(pf.deb_file_format_raw, deb_tvbr:range(0,1))
                                        local ff_raw = deb_tvbr(0,1):uint()
                                        local ff_type_val = bit.band(ff_raw, FILE_FORMAT_MASK)
                                        current_file_state.is_sequential = (ff_type_val == FILE_FORMAT_SEQUENTIAL)

                                        local ff_attr_raw = bit.band(ff_raw, FILE_ATTRIBUTE_MASK)
                                        local attr_str = ""
                                        if bit.band(ff_attr_raw, FILE_ATTRIBUTE_READ_PROTECTED)   ~= 0 then attr_str = attr_str .. "R" end
                                        if bit.band(ff_attr_raw, FILE_ATTRIBUTE_WRITE_PROTECTED)  ~= 0 then attr_str = attr_str .. "W" end
                                        if bit.band(ff_attr_raw, FILE_ATTRIBUTE_DELETE_PROTECTED) ~= 0 then attr_str = attr_str .. "D" end
                                        if attr_str == "" then attr_str = "None" end

                                        deb_node:add(pf.deb_file_format_type, deb_tvbr:range(0,1), ff_type_val)
                                        deb_node:add(pf.deb_file_attributes_str, deb_tvbr:range(0,0)):set_text("Attributes: " .. attr_str)

                                        local known_types = { [FILE_FORMAT_RELOCATABLE]=true, [FILE_FORMAT_ABSOLUTE]=true, [FILE_FORMAT_SEQUENTIAL]=true,
                                                              [FILE_FORMAT_DIRECT]=true, [FILE_FORMAT_INDEXED]=true, [FILE_FORMAT_KEYED]=true,
                                                              [FILE_FORMAT_EMPTY]=true, [FILE_FORMAT_DELETED]=true }
                                        if not known_types[ff_type_val] then
                                            pinfo:expert(expert_info.invalid_deb_file_format, deb_tvbr:range(0,1))
                                        end

                                        local deb_fname_tvb = deb_tvbr:range(1, FNAME_LEN)
                                        local deb_ftype_tvb = deb_tvbr:range(1 + FNAME_LEN, FTYPE_LEN)
                                        deb_node:add(pf.deb_file_name_str, deb_fname_tvb)
                                        deb_node:add(pf.deb_file_type_str, deb_ftype_tvb)

                                        local fname_part = string.gsub(deb_fname_tvb:string(), "%s*$", "")
                                        local ftype_part = string.gsub(deb_ftype_tvb:string(), "%s*$", "")
                                        if ftype_part == "" then
                                            current_file_state.file_name_str = fname_part
                                        else
                                            current_file_state.file_name_str = fname_part .. "." .. ftype_part
                                        end

                                        -- Update info column for OPEN command now that filename is known
                                        local open_info_str = "Cmd: OPEN (O)"
                                        if current_file_state.file_name_str then
                                             open_info_str = open_info_str .. ", File: " .. current_file_state.file_name_str
                                             packet_node:add(pf.packet_current_filename, current_file_state.file_name_str)
                                        end
                                        pinfo.cols.info:set("Data Packet, " .. open_info_str)


                                        local pcall_status_rec_count, rec_count_val = pcall(function() return deb_tvbr(17,2):le_uint() end)
                                        if pcall_status_rec_count then
                                            current_file_state.total_records_in_file = rec_count_val
                                        else
                                            current_file_state.total_records_in_file = 0
                                        end
                                        deb_node:add(pf.deb_record_count_val, deb_tvbr:range(17, 2))

                                        deb_node:add(pf.deb_block_count_val, deb_tvbr:range(19, 2))
                                        deb_node:add(pf.deb_start_sector_val, deb_tvbr:range(21, 2))

                                        local ffd1_node = deb_node:add(pf.deb_ffd1_raw, deb_tvbr:range(23,2))
                                        local ffd1_le_val = deb_tvbr(23,2):le_uint()
                                        if ff_type_val == FILE_FORMAT_SEQUENTIAL then
                                            ffd1_node:add(pf.deb_ffd1_seq_rec_len_val, deb_tvbr:range(23,2), ffd1_le_val)
                                        elseif ff_type_val == FILE_FORMAT_DIRECT then
                                            ffd1_node:add(pf.deb_ffd1_dir_rec_len_val, deb_tvbr:range(23,2), ffd1_le_val)
                                        elseif ff_type_val == FILE_FORMAT_ABSOLUTE or ff_type_val == FILE_FORMAT_RELOCATABLE then
                                            ffd1_node:add(pf.deb_ffd1_absrel_rec_len_val, deb_tvbr:range(23,2), ffd1_le_val)
                                        elseif ff_type_val == FILE_FORMAT_INDEXED or ff_type_val == FILE_FORMAT_KEYED then
                                            ffd1_node:add(pf.deb_ffd1_idxkey_rec_len_val, deb_tvbr:range(23,2), bit.band(ffd1_le_val, 0x01FF))
                                            ffd1_node:add(pf.deb_ffd1_idxkey_key_len_val, deb_tvbr:range(23,2), bit.rshift(ffd1_le_val, 9))
                                        end

                                        local ts_tvbr = deb_tvbr:range(25,3)
                                        local ts_str_val = decode_oasis_timestamp_to_str(ts_tvbr)
                                        deb_node:add(pf.deb_timestamp_raw_bytes, ts_tvbr)
                                        deb_node:add(pf.deb_timestamp_decoded_str, deb_tvbr:range(25,0)):set_text("Timestamp: " .. ts_str_val)

                                        deb_node:add(pf.deb_owner_id_val, deb_tvbr:range(28,1))
                                        deb_node:add(pf.deb_shared_from_owner_id_val, deb_tvbr:range(29,1))

                                        local ffd2_node = deb_node:add(pf.deb_ffd2_raw, deb_tvbr:range(30,2))
                                        local ffd2_le_val = deb_tvbr(30,2):le_uint()
                                        if ff_type_val == FILE_FORMAT_SEQUENTIAL then
                                            ffd2_node:add(pf.deb_ffd2_seq_last_sector_val, deb_tvbr:range(30,2), ffd2_le_val)
                                        elseif ff_type_val == FILE_FORMAT_ABSOLUTE then
                                            ffd2_node:add(pf.deb_ffd2_abs_load_addr_val, deb_tvbr:range(30,2), ffd2_le_val)
                                        elseif ff_type_val == FILE_FORMAT_RELOCATABLE then
                                            ffd2_node:add(pf.deb_ffd2_rel_prog_len_val, deb_tvbr:range(30,2), ffd2_le_val)
                                        elseif ff_type_val == FILE_FORMAT_INDEXED or ff_type_val == FILE_FORMAT_KEYED then
                                             ffd2_node:add(pf.deb_ffd2_idxkey_alloc_size_val, deb_tvbr:range(30,2), ffd2_le_val)
                                        end
                                    else
                                        pinfo:expert(expert_info.open_payload_too_short, decoded_payload_tvbr())
                                    end
                                elseif cmd_val == string.byte('W') then
                                    local data_part_len_for_sum = decoded_len_for_sum
                                    if current_file_state.is_sequential then
                                        if decoded_len_for_sum >= 2 then
                                            data_part_len_for_sum = decoded_len_for_sum - 2
                                            local link_offset = decoded_payload_tvbr:len() - 2
                                            payload_node:add(pf.packet_payload_seq_link, decoded_payload_tvbr:range(link_offset, 2))
                                            -- Append SeqLink to info column only if it's a sequential WRITE
                                            -- This is now handled by the main cmd_info_str logic for WRITE
                                            -- but we need to append the value if it's a sequential write with link.
                                            pinfo.cols.info:append(", SeqLink: " .. decoded_payload_tvbr(link_offset, 2):le_uint())
                                        else
                                            data_part_len_for_sum = 0
                                        end
                                    end
                                    current_file_state.total_payload_bytes_written = current_file_state.total_payload_bytes_written + data_part_len_for_sum
                                end
                            else
                                pinfo:expert(expert_info.payload_decode_error, raw_payload_tvbr, (decode_err or "Unknown decode error"))
                                pinfo.cols.info:append(" [Payload Decode Error]")
                            end
                        elseif cmd_val == string.byte('O') then -- Empty payload for OPEN
                             payload_node:add(pf.packet_payload_raw_stuffed, raw_payload_tvbr)
                                :set_text("Payload: (Empty - Invalid for OPEN)")
                            pinfo:expert(expert_info.open_payload_too_short, raw_payload_tvbr)
                            pinfo.cols.info:append(" [Empty Payload for OPEN]")
                        else -- Empty payload for WRITE (if that's possible and not an error) or other commands
                             payload_node:add(pf.packet_payload_raw_stuffed, raw_payload_tvbr)
                                :set_text("Payload: (Empty)")
                        end
                    end

                    packet_node:add(pf.packet_trailer_dle_etx, data_tvb:range(trailer_start_offset, 2))
                    local lrc_byte_offset = trailer_start_offset + 2
                    if lrc_byte_offset < data_len then
                        local received_lrc_val = data_tvb(lrc_byte_offset, 1):uint()
                        local lrc_node = packet_node:add(pf.packet_lrc_received, data_tvb:range(lrc_byte_offset, 1))
                        local lrc_calc_val = calculate_oasis_lrc(data_tvb, 0, lrc_byte_offset)
                        packet_node:add(pf.packet_lrc_calculated, lrc_calc_val)
                        if lrc_calc_val ~= received_lrc_val then
                            pinfo:expert(expert_info.lrc_mismatch, data_tvb:range(lrc_byte_offset, 1))
                            lrc_node:append_text(string.format(" [Mismatch! Calculated: 0x%02X]", lrc_calc_val))
                            pinfo.cols.info:append(" [LRC Mismatch]")
                        else
                            lrc_node:append_text(" [Correct]")
                        end

                        local final_byte_offset_val = lrc_byte_offset + 1
                        if final_byte_offset_val < data_len then
                            local final_byte_read = data_tvb(final_byte_offset_val, 1):uint()
                            local final_byte_node = packet_node:add(pf.packet_final_rub_byte, data_tvb:range(final_byte_offset_val, 1))
                            if final_byte_read ~= RUB then
                                pinfo:expert(expert_info.final_rub_byte_incorrect, data_tvb:range(final_byte_offset_val, 1))
                                final_byte_node:append_text(string.format(" [Expected 0x%02X]", RUB))
                                pinfo.cols.info:append(" [Wrong Final Byte]")
                            else
                                final_byte_node:append_text(" [Correct]")
                            end
                            consumed_len = final_byte_offset_val + 1
                        else
                            pinfo:expert(expert_info.final_rub_byte_missing, data_tvb:range(lrc_byte_offset,1))
                            pinfo.cols.info:append(" [Final Byte Missing]")
                            consumed_len = lrc_byte_offset + 1
                        end
                    else
                        pinfo:expert(expert_info.lrc_byte_missing, data_tvb:range(trailer_start_offset, 2))
                        pinfo.cols.info:append(" [LRC Missing]")
                        consumed_len = trailer_start_offset + 2
                    end
                else
                    pinfo:expert(expert_info.packet_trailer_not_found, data_tvb)
                    pinfo.cols.info:append(" (Malformed - No DLE ETX Trailer)")
                    if data_len > 3 then packet_node:add(pf.packet_payload_raw_stuffed, data_tvb:range(3)) end
                    consumed_len = data_len
                end
            end
        else
            pinfo.cols.info:set("Unknown DLE Sequence")
            pinfo.cols.src = "?"
            pinfo.cols.dst = "?"
            subtree:add(pf.msg_type, data_tvb:range(0,0)):set_text("Type: Unknown DLE Sequence")
            subtree:add(pf.packet_raw_data, data_tvb)
            consumed_len = data_len
        end
    else
        pinfo.cols.info:set("Unknown/Invalid Data (Not ENQ or DLE-prefixed)")
        pinfo.cols.src = "?"
        pinfo.cols.dst = "?"
        subtree:add(pf.msg_type, data_tvb:range(0,0)):set_text("Type: Unknown/Invalid")
        subtree:add(pf.packet_raw_data, data_tvb)
        consumed_len = data_len
    end

    if data_len > consumed_len then
        local extra_len = data_len - consumed_len
        local extra_data_tvbr = data_tvb:range(consumed_len, extra_len)
        tree:add(pf.extra_data_bytes, extra_data_tvbr):append_text(" (After recognized message part)")
        pinfo:expert(expert_info.extra_trailing_data, extra_data_tvbr)
    end

    return total_len
end

local wtap_encap_table = DissectorTable.get("wtap_encap")
wtap_encap_table:add(149, oasis_proto)

print("OASIS Wireshark Dissector Loaded for LINKTYPE_USER2 (149) -- Production Version")
