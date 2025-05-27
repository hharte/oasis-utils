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

/* Common Test Utilities */
#include "test_oasis_common.h"

/* DUT headers */
#include "oasis.h"


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
            test_info->test_suite_name() + "_" +
            test_info->name() + ".pcap";
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
        EXPECT_EQ(header.network, static_cast<uint32_t>(149)); /* LINKTYPE_USER2 */
    }

    void VerifyPcapRecord(const std::vector<uint8_t>& pcap_buffer, size_t offset,
        uint32_t expected_data_len_after_dir_byte, /* Original data len */
        int expected_direction,
        const std::vector<uint8_t>& expected_payload_data_original) {
        ASSERT_GT(pcap_buffer.size(), offset + sizeof(pcaprec_hdr_t));
        pcaprec_hdr_t rec_hdr;
        memcpy(&rec_hdr, pcap_buffer.data() + offset, sizeof(pcaprec_hdr_t));

        uint32_t expected_incl_len = expected_data_len_after_dir_byte + 1; /* +1 for direction byte */
        EXPECT_EQ(rec_hdr.incl_len, expected_incl_len);
        EXPECT_EQ(rec_hdr.orig_len, expected_incl_len);
        EXPECT_GE(rec_hdr.ts_sec, 0U);
        EXPECT_LT(rec_hdr.ts_usec, 1000000U);


        size_t record_data_offset = offset + sizeof(pcaprec_hdr_t);
        ASSERT_GT(pcap_buffer.size(), record_data_offset); /* Check for direction byte */
        uint8_t actual_direction = pcap_buffer[record_data_offset];
        EXPECT_EQ(actual_direction, expected_direction);

        ASSERT_EQ(pcap_buffer.size(), record_data_offset + 1 + expected_data_len_after_dir_byte);

        std::vector<uint8_t> actual_payload_data(expected_data_len_after_dir_byte);
        if (expected_data_len_after_dir_byte > 0) {
            memcpy(actual_payload_data.data(), pcap_buffer.data() + record_data_offset + 1, expected_data_len_after_dir_byte);
        }

        std::vector<uint8_t> expected_masked_payload(expected_payload_data_original.size());
        for (size_t i = 0; i < expected_payload_data_original.size(); ++i) {
            expected_masked_payload[i] = expected_payload_data_original[i] & 0x7F;
        }
        EXPECT_EQ(actual_payload_data, expected_masked_payload);
    }
};

/* --- Tests for oasis_pcap_create --- */

TEST_F(OasisPcapTest, CreateValidFile) {
    ASSERT_EQ(0, oasis_pcap_create(temp_pcap_filename_str.c_str(), &pcap_stream));
    ASSERT_NE(nullptr, pcap_stream);
    oasis_pcap_close(pcap_stream);
    pcap_stream = nullptr;

    std::vector<uint8_t> content = tests_common::read_file_to_bytes(temp_pcap_filepath);
    ASSERT_FALSE(content.empty());
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
    std::string invalid_path_filename = "non_existent_dir_for_pcap_test/test_file.pcap";
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
    ASSERT_GE(content.size(), sizeof(pcap_hdr_t) + sizeof(pcaprec_hdr_t) + 1 + data_to_log.size());
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
    ASSERT_GE(content.size(), sizeof(pcap_hdr_t) + sizeof(pcaprec_hdr_t) + 1 + data_to_log.size());
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
    ASSERT_GE(content.size(), sizeof(pcap_hdr_t) + sizeof(pcaprec_hdr_t) + 1); /* +1 for direction byte */
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
    const uint16_t data_len_too_long = 1025; /* MAX_MASK_BUFFER_SIZE in oasis_pcap.c is 1024 */
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
