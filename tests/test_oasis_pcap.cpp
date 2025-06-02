/* tests/test_oasis_pcap.cpp */
/* GTest unit tests for functions in oasis_pcap.c */

#include "gtest/gtest.h"
#include <vector>
#include <string>
#include <fstream>      /* For std::ifstream */
#include <cstdio>       /* For FILE, fopen, fclose, remove */
#include <cstring>      /* For memcpy, memset */
#include <cerrno>       /* For errno constants */
#include <algorithm>    /* For std::equal */
#include <cstdint>      /* For uint32_t, etc. */
#include <filesystem>   /* For std::filesystem::path, temp_directory_path, remove */
#include <ctime>        /* For time_t, time for timestamp comparison */

/* Common Test Utilities */
#include "test_oasis_common.h"

/* DUT headers */
#include "oasis_pcap.h"
#include "oasis_endian.h" /* Added for be32toh and other endian utilities */


class OasisPcapTest : public ::testing::Test {
protected:
    std::string temp_pcap_filename_str; /* Store as string for existing function calls */
    std::filesystem::path temp_pcap_filepath; /* Store as path object for filesystem operations */
    FILE* pcap_stream;

    void SetUp() override {
        pcap_stream = nullptr;

        /* Create a unique temporary filename for each test using std::filesystem */
        const ::testing::TestInfo* const test_info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        std::string unique_filename = std::string("oasis_pcap_test_") +
            (test_info ? test_info->test_suite_name() : "DefaultSuite") + "_" +
            (test_info ? test_info->name() : "DefaultTest") + ".pcap";
        temp_pcap_filepath = std::filesystem::temp_directory_path() / unique_filename;
        temp_pcap_filename_str = temp_pcap_filepath.string();

        /* Ensure the file does not exist from a previous failed run */
        std::error_code ec;
        std::filesystem::remove(temp_pcap_filepath, ec);
        /* Not asserting on ec, as file might not exist, which is fine */
    }

    void TearDown() override {
        if (pcap_stream) {
            oasis_pcap_close(pcap_stream); /* Ensure stream is closed if test didn't do it */
            pcap_stream = nullptr;
        }
        std::error_code ec;
        std::filesystem::remove(temp_pcap_filepath, ec);
        /* Not asserting on ec, as file might have been removed by test or not created */
    }

    void VerifyPcapGlobalHeader(const std::vector<uint8_t>& buffer) {
        ASSERT_GE(buffer.size(), sizeof(pcap_hdr_t));
        pcap_hdr_t header;
        memcpy(&header, buffer.data(), sizeof(pcap_hdr_t));

        EXPECT_EQ(header.magic_number, 0xa1b2c3d4);
        EXPECT_EQ(header.version_major, 2);
        EXPECT_EQ(header.version_minor, 4);
        EXPECT_EQ(header.thiszone, 0);
        EXPECT_EQ(header.sigfigs, static_cast<uint32_t>(0));
        EXPECT_EQ(header.snaplen, static_cast<uint32_t>(65535));
        EXPECT_EQ(header.network, static_cast<uint32_t>(LINKTYPE_RTAC_SERIAL));
    }

    /*
     * Verifies a single PCAP record, including the rtacser_hdr_t pseudo-header.
     * @param pcap_buffer The complete buffer containing the PCAP data (global header + records).
     * @param offset The offset within pcap_buffer where this specific record (pcaprec_hdr_t) starts.
     * @param expected_oasis_payload_len The length of the original OASIS message data.
     * @param expected_direction The original direction (OASIS_PCAP_RX or OASIS_PCAP_TX).
     * @param expected_oasis_payload_original The original OASIS message data (before 7-bit masking).
     */
    void VerifyPcapRecord(const std::vector<uint8_t>& pcap_buffer, size_t offset,
        uint32_t expected_oasis_payload_len,
        int expected_direction, /* OASIS_PCAP_RX or OASIS_PCAP_TX */
        const std::vector<uint8_t>& expected_oasis_payload_original) {

        /* Check if there's enough data for the pcap record header */
        ASSERT_GT(pcap_buffer.size(), offset + sizeof(pcaprec_hdr_t) - 1)
            << "Buffer too small for pcaprec_hdr_t at offset " << offset;
        pcaprec_hdr_t rec_hdr;
        memcpy(&rec_hdr, pcap_buffer.data() + offset, sizeof(pcaprec_hdr_t));

        /* The included length in PCAP record is rtacser_hdr_t + OASIS payload */
        uint32_t expected_incl_len = expected_oasis_payload_len + sizeof(rtacser_hdr_t);
        EXPECT_EQ(rec_hdr.incl_len, expected_incl_len);
        EXPECT_EQ(rec_hdr.orig_len, expected_incl_len); /* Assuming orig_len is same as incl_len */

        time_t now = time(NULL);
        EXPECT_GE(rec_hdr.ts_sec, now - 60) << "PCAP record timestamp seconds seem too old.";
        EXPECT_LE(rec_hdr.ts_sec, now + 60) << "PCAP record timestamp seconds seem too far in future.";
        EXPECT_LT(rec_hdr.ts_usec, 1000000U) << "PCAP record timestamp microseconds out of range.";

        size_t rtac_header_offset = offset + sizeof(pcaprec_hdr_t);
        ASSERT_GT(pcap_buffer.size(), rtac_header_offset + sizeof(rtacser_hdr_t) - 1)
            << "Buffer too small for rtacser_hdr_t at offset " << rtac_header_offset;
        rtacser_hdr_t actual_rtac_hdr;
        memcpy(&actual_rtac_hdr, pcap_buffer.data() + rtac_header_offset, sizeof(rtacser_hdr_t));

        /* Verify rtacser_hdr_t fields */
        /* Timestamps in rtacser_hdr are Big Endian, pcaprec_hdr timestamps are in file/host order */
        EXPECT_EQ(be32toh(actual_rtac_hdr.ts_sec), rec_hdr.ts_sec) << "RTAC ts_sec (BE->Host) should match PCAP record ts_sec.";
        EXPECT_EQ(be32toh(actual_rtac_hdr.ts_usec), rec_hdr.ts_usec) << "RTAC ts_usec (BE->Host) should match PCAP record ts_usec.";

        if (expected_direction == OASIS_PCAP_TX) {
            EXPECT_EQ(actual_rtac_hdr.event_type, 0x01) << "RTAC event_type should be 0x01 (DATA_TX_START) for TX.";
        }
        else { /* OASIS_PCAP_RX */
            EXPECT_EQ(actual_rtac_hdr.event_type, 0x02) << "RTAC event_type should be 0x02 (DATA_RX_START) for RX.";
        }
        EXPECT_EQ(actual_rtac_hdr.control_line_state, 0x00) << "RTAC control_line_state should be 0x00.";
        EXPECT_EQ(actual_rtac_hdr.footer[0], 0x00) << "RTAC footer[0] should be 0x00.";
        EXPECT_EQ(actual_rtac_hdr.footer[1], 0x00) << "RTAC footer[1] should be 0x00.";

        size_t expected_record_end_offset = rtac_header_offset + sizeof(rtacser_hdr_t) + expected_oasis_payload_len;
        ASSERT_GE(pcap_buffer.size(), expected_record_end_offset)
            << "Buffer too small for the complete record data. Expected end: " << expected_record_end_offset << ", Buffer size: " << pcap_buffer.size();

        std::vector<uint8_t> actual_oasis_payload(expected_oasis_payload_len);
        if (expected_oasis_payload_len > 0) {
            memcpy(actual_oasis_payload.data(), pcap_buffer.data() + rtac_header_offset + sizeof(rtacser_hdr_t), expected_oasis_payload_len);
        }

        std::vector<uint8_t> expected_masked_payload(expected_oasis_payload_original.size());
        for (size_t i = 0; i < expected_oasis_payload_original.size(); ++i) {
            expected_masked_payload[i] = expected_oasis_payload_original[i] & 0x7F;
        }
        EXPECT_EQ(actual_oasis_payload, expected_masked_payload) << "Masked OASIS payload data mismatch.";
    }
};

/* --- Tests for oasis_pcap_create --- */

TEST_F(OasisPcapTest, CreateValidFile) {
    ASSERT_EQ(0, oasis_pcap_create(temp_pcap_filename_str.c_str(), &pcap_stream));
    ASSERT_NE(nullptr, pcap_stream);
    oasis_pcap_close(pcap_stream);
    pcap_stream = nullptr;

    std::vector<uint8_t> content = tests_common::read_file_to_bytes(temp_pcap_filepath);
    ASSERT_FALSE(content.empty()) << "PCAP file is empty after create and close.";
    VerifyPcapGlobalHeader(content);
}

TEST_F(OasisPcapTest, CreateNullFilename) {
    EXPECT_EQ(-1, oasis_pcap_create(nullptr, &pcap_stream));
    EXPECT_EQ(nullptr, pcap_stream);
}

TEST_F(OasisPcapTest, CreateNullStreamPtr) {
    EXPECT_EQ(-1, oasis_pcap_create(temp_pcap_filename_str.c_str(), nullptr));
}

TEST_F(OasisPcapTest, CreateFileInvalidPath) {
    std::filesystem::path non_existent_dir = temp_pcap_filepath.parent_path() / "non_existent_pcap_subdir_test";
    std::filesystem::path invalid_path_obj = non_existent_dir / "test_file.pcap";
    std::string invalid_path_filename = invalid_path_obj.string();

    EXPECT_EQ(-1, oasis_pcap_create(invalid_path_filename.c_str(), &pcap_stream));
    EXPECT_EQ(nullptr, pcap_stream);
}

/* --- Tests for oasis_pcap_add_record --- */

TEST_F(OasisPcapTest, AddRecordRx) {
    ASSERT_EQ(0, oasis_pcap_create(temp_pcap_filename_str.c_str(), &pcap_stream));
    ASSERT_NE(nullptr, pcap_stream);

    std::vector<uint8_t> data_to_log = { 0x01, 0x82, 0x03, 0xFF };
    ASSERT_EQ(0, oasis_pcap_add_record(pcap_stream, OASIS_PCAP_RX, data_to_log.data(), (uint16_t)data_to_log.size()));

    oasis_pcap_close(pcap_stream);
    pcap_stream = nullptr;

    std::vector<uint8_t> content = tests_common::read_file_to_bytes(temp_pcap_filepath);
    ASSERT_GE(content.size(), sizeof(pcap_hdr_t) + sizeof(pcaprec_hdr_t) + sizeof(rtacser_hdr_t) + data_to_log.size());
    VerifyPcapGlobalHeader(content);
    VerifyPcapRecord(content, sizeof(pcap_hdr_t), (uint32_t)data_to_log.size(), OASIS_PCAP_RX, data_to_log);
}

TEST_F(OasisPcapTest, AddRecordTx) {
    ASSERT_EQ(0, oasis_pcap_create(temp_pcap_filename_str.c_str(), &pcap_stream));
    ASSERT_NE(nullptr, pcap_stream);

    std::vector<uint8_t> data_to_log = { 0x1A, 0x2B, 0xCF };
    ASSERT_EQ(0, oasis_pcap_add_record(pcap_stream, OASIS_PCAP_TX, data_to_log.data(), (uint16_t)data_to_log.size()));

    oasis_pcap_close(pcap_stream);
    pcap_stream = nullptr;

    std::vector<uint8_t> content = tests_common::read_file_to_bytes(temp_pcap_filepath);
    ASSERT_GE(content.size(), sizeof(pcap_hdr_t) + sizeof(pcaprec_hdr_t) + sizeof(rtacser_hdr_t) + data_to_log.size());
    VerifyPcapGlobalHeader(content);
    VerifyPcapRecord(content, sizeof(pcap_hdr_t), (uint32_t)data_to_log.size(), OASIS_PCAP_TX, data_to_log);
}

TEST_F(OasisPcapTest, AddRecordEmptyData) {
    ASSERT_EQ(0, oasis_pcap_create(temp_pcap_filename_str.c_str(), &pcap_stream));
    ASSERT_NE(nullptr, pcap_stream);

    std::vector<uint8_t> empty_data = {};
    ASSERT_EQ(0, oasis_pcap_add_record(pcap_stream, OASIS_PCAP_RX, empty_data.data(), 0));

    oasis_pcap_close(pcap_stream);
    pcap_stream = nullptr;

    std::vector<uint8_t> content = tests_common::read_file_to_bytes(temp_pcap_filepath);
    ASSERT_GE(content.size(), sizeof(pcap_hdr_t) + sizeof(pcaprec_hdr_t) + sizeof(rtacser_hdr_t));
    VerifyPcapGlobalHeader(content);
    VerifyPcapRecord(content, sizeof(pcap_hdr_t), 0, OASIS_PCAP_RX, empty_data);
}

TEST_F(OasisPcapTest, AddRecordNullStream) {
    std::vector<uint8_t> data_to_log = { 0x01, 0x02 };
    EXPECT_EQ(0, oasis_pcap_add_record(nullptr, OASIS_PCAP_RX, data_to_log.data(), (uint16_t)data_to_log.size()));
}

TEST_F(OasisPcapTest, AddRecordNullDataWithLen) {
    ASSERT_EQ(0, oasis_pcap_create(temp_pcap_filename_str.c_str(), &pcap_stream));
    ASSERT_NE(nullptr, pcap_stream);
    EXPECT_EQ(-1, oasis_pcap_add_record(pcap_stream, OASIS_PCAP_RX, nullptr, 5));
}

TEST_F(OasisPcapTest, AddRecordInvalidDirection) {
    ASSERT_EQ(0, oasis_pcap_create(temp_pcap_filename_str.c_str(), &pcap_stream));
    ASSERT_NE(nullptr, pcap_stream);
    std::vector<uint8_t> data_to_log = { 0x01 };
    EXPECT_EQ(-1, oasis_pcap_add_record(pcap_stream, 0x05, data_to_log.data(), (uint16_t)data_to_log.size()));
}

TEST_F(OasisPcapTest, AddRecordDataTooLongForMaskBuffer) {
    const uint16_t data_len_too_long = MAX_MASK_BUFFER_SIZE + 1;
    std::vector<uint8_t> long_data(data_len_too_long, 0xAA);

    ASSERT_EQ(0, oasis_pcap_create(temp_pcap_filename_str.c_str(), &pcap_stream));
    ASSERT_NE(nullptr, pcap_stream);

    EXPECT_EQ(-1, oasis_pcap_add_record(pcap_stream, OASIS_PCAP_TX, long_data.data(), data_len_too_long));
}

/* --- Tests for oasis_pcap_close --- */

TEST_F(OasisPcapTest, CloseValidStream) {
    ASSERT_EQ(0, oasis_pcap_create(temp_pcap_filename_str.c_str(), &pcap_stream));
    ASSERT_NE(nullptr, pcap_stream);
    EXPECT_EQ(0, oasis_pcap_close(pcap_stream));
    pcap_stream = nullptr;
}

TEST_F(OasisPcapTest, CloseNullStream) {
    EXPECT_EQ(0, oasis_pcap_close(nullptr));
}
