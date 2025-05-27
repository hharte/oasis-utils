/* tests/test_oasis_extract.cpp */
/* GTest unit tests for file extraction logic, primarily in oasis_extract.c */

#include "gtest/gtest.h"
#include <vector>
#include <string>
#include <fstream>      /* For std::ifstream, std::ofstream */
#include <cstdio>       /* For FILE, remove, fopen, fclose, fwrite, fseek, feof */
#include <cstring>      /* For memcpy, memset, strncpy, strcmp */
#include <cerrno>       /* For errno constants */
#include <algorithm>    /* For std::equal, std::min */
#include <cstdint>      /* For uint32_t, etc. */
#include <filesystem>   /* For temporary directory management (C++17) */
#include <sys/stat.h>   /* For stat to check timestamps */
#include <time.h>       /* For time_t, struct tm, mktime */
#ifdef _WIN32
#include <windows.h> /* For Sleep */
#else
#include <unistd.h> /* For usleep */
#endif

/* Common Test Utilities */
#include "test_oasis_common.h"

/* Suppress pedantic warning for flexible array members in C headers included by C++ */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

/* DUT headers */
#include "oasis.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

/* Platform-specific line endings for test verification */
#ifdef _WIN32
#define HOST_LINE_ENDING "\r\n"
#else
#define HOST_LINE_ENDING "\n"
#endif

/* Define OASIS_SUB_CHAR_TEST for tests if not available from headers easily */
#ifndef OASIS_SUB_CHAR_TEST
#define OASIS_SUB_CHAR_TEST (0x1A)
#endif


class ExtractFilesTest : public ::testing::Test {
protected:
    std::filesystem::path temp_output_dir_base;
    std::filesystem::path temp_output_dir; /* Per-test unique output dir */
    std::filesystem::path temp_image_path;
    sector_io_stream_t* mock_sector_io_stream;
    oasis_disk_layout_t disk_layout;
    cli_options_t options;
    std::vector<directory_entry_block_t> debs_vector;

    void SetUp() override {
        const ::testing::TestInfo* const test_info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        /* Create a base directory for all tests if it doesn't exist */
        temp_output_dir_base = std::filesystem::temp_directory_path() / "oasis_gtests_root";
        std::filesystem::create_directories(temp_output_dir_base);

        /* Create a unique subdirectory for each test to avoid file collisions and locking issues */
        std::string unique_test_subdir_name = std::string(test_info->test_suite_name()) + "_" + test_info->name();
        temp_output_dir = temp_output_dir_base / unique_test_subdir_name;
        std::filesystem::remove_all(temp_output_dir); /* Clean from previous run if any */
        std::filesystem::create_directories(temp_output_dir);


        temp_image_path = temp_output_dir / "mock_disk.img";

        /* Create an empty file for sector_io_open to work with */
        std::ofstream temp_file_creator(temp_image_path, std::ios::binary | std::ios::trunc);
        temp_file_creator.close();
        ASSERT_TRUE(std::filesystem::exists(temp_image_path)) << "Failed to create initial empty mock disk image file.";


        mock_sector_io_stream = sector_io_open(temp_image_path.string().c_str(), "r+b");
        ASSERT_NE(nullptr, mock_sector_io_stream) << "Failed to open mock disk image with sector_io_open.";
        ASSERT_EQ(sector_io_get_total_sectors(mock_sector_io_stream), static_cast<uint32_t>(0))
            << "Mock image initial total sectors should be 0.";

        memset(&disk_layout, 0, sizeof(disk_layout));
        disk_layout.directory = nullptr;
        memset(&options, 0, sizeof(options));
        options.owner_id_filter = 0; /* Default to user 0 for tests unless overridden */
        debs_vector.clear();
    }

    void TearDown() override {
        if (mock_sector_io_stream) {
            sector_io_close(mock_sector_io_stream);
            mock_sector_io_stream = nullptr;
        }
        if (disk_layout.directory) {
            free(disk_layout.directory);
            disk_layout.directory = nullptr;
        }
        /* Attempt to remove the per-test unique directory */
        if (std::filesystem::exists(temp_output_dir)) {
            std::error_code ec;
            std::filesystem::remove_all(temp_output_dir, ec);
            if (ec) {
                GTEST_LOG_(WARNING) << "Could not remove temp test directory " << temp_output_dir << ": " << ec.message();
            }
        }
    }

    /*
     * Wrapper to use the common DEB population utility.
     */
    directory_entry_block_t CreateDeb(
        const char* fname, const char* ftype, uint8_t format,
        uint16_t start_sector, uint16_t block_count,
        uint8_t owner_id, /* Added owner_id */
        uint16_t record_count = 1,
        uint16_t ffd1 = 0, uint16_t ffd2 = 0) {
        directory_entry_block_t deb;
        tests_common::populate_deb(&deb, fname, ftype, format, start_sector, block_count, record_count, ffd1, ffd2, owner_id);
        return deb;
    }

    void AddDebToDebVector(const directory_entry_block_t& deb) { debs_vector.push_back(deb); }
    void PopulateMockDirectory() {
        if (disk_layout.directory) { free(disk_layout.directory); disk_layout.directory = nullptr; }
        if (debs_vector.empty()) {
            disk_layout.directory = (oasis_directory_t*)malloc(sizeof(oasis_directory_t));
            ASSERT_NE(nullptr, disk_layout.directory);
            disk_layout.directory->directory_size_bytes = 0;
        }
        else {
            size_t dir_data_size = debs_vector.size() * sizeof(directory_entry_block_t);
            disk_layout.directory = (oasis_directory_t*)malloc(sizeof(oasis_directory_t) + dir_data_size);
            ASSERT_NE(nullptr, disk_layout.directory);
            disk_layout.directory->directory_size_bytes = dir_data_size;
            memcpy(disk_layout.directory->directory, debs_vector.data(), dir_data_size);
        }
    }
    void WriteSectorToMockImage(sector_io_stream_t* sio_stream, uint16_t lba, const uint8_t* data_for_sector_content, size_t data_content_len, uint16_t next_lba_if_seq, bool is_sequential) {
        ASSERT_NE(nullptr, sio_stream); uint8_t sector_buffer[SECTOR_SIZE];
        memset(sector_buffer, 0, SECTOR_SIZE); size_t copy_len = data_content_len;
        if (is_sequential) { ASSERT_LE(copy_len, OASIS_SEQ_DATA_PER_SECTOR); }
        else { ASSERT_LE(copy_len, static_cast<size_t>(SECTOR_SIZE)); }
        if (data_for_sector_content != nullptr && copy_len > 0) memcpy(sector_buffer, data_for_sector_content, copy_len);
        if (is_sequential) memcpy(sector_buffer + OASIS_SEQ_DATA_PER_SECTOR, &next_lba_if_seq, sizeof(uint16_t));
        ssize_t sectors_written = sector_io_write(sio_stream, lba, 1, sector_buffer);
        ASSERT_EQ(sectors_written, 1) << "Failed to write to mock image LBA " << lba;
    }
    void WriteSequentialSectorToMockImage(sector_io_stream_t* sio_stream, uint16_t lba, const uint8_t* data_for_data_portion, size_t actual_data_portion_len, uint16_t next_lba_if_seq) {
        uint8_t full_sector_payload[SECTOR_SIZE];
        memset(full_sector_payload, 0, SECTOR_SIZE);
#undef min /* Make sure to use std::min if min is defined as a macro */
        size_t len_to_copy_to_payload = std::min(actual_data_portion_len, (size_t)OASIS_SEQ_DATA_PER_SECTOR);
        if (data_for_data_portion != nullptr && len_to_copy_to_payload > 0) {
            memcpy(full_sector_payload, data_for_data_portion, len_to_copy_to_payload);
        }
        memcpy(full_sector_payload + OASIS_SEQ_DATA_PER_SECTOR, &next_lba_if_seq, sizeof(uint16_t));
        ssize_t sectors_written = sector_io_write(sio_stream, lba, 1, full_sector_payload);
        ASSERT_EQ(sectors_written, 1) << "Failed to write sequential sector to mock image LBA " << lba;
    }

    std::string GetHostFilename(const directory_entry_block_t* deb) {
        char host_filename_buf[MAX_HOST_FILENAME_LEN];
        if (!oasis_deb_to_host_filename(deb, host_filename_buf, sizeof(host_filename_buf))) return "FILENAME_ERROR";
        return std::string(host_filename_buf);
    }
    int CountExtractedFiles() {
        int count = 0;
        if (!std::filesystem::exists(temp_output_dir)) return 0;
        for (const auto& entry : std::filesystem::directory_iterator(temp_output_dir)) {
            if (entry.path().filename() != "mock_disk.img") { count++; }
        }
        return count;
    }
};

TEST_F(ExtractFilesTest, ExtractEmptyDirectory) {
    PopulateMockDirectory(); /* Empty directory */
    options.pattern = NULL;
    options.ascii_conversion = false;
    options.owner_id_filter = 0;
    ASSERT_TRUE(extract_files(mock_sector_io_stream, &disk_layout, temp_output_dir.string().c_str(), &options));
    EXPECT_EQ(0, CountExtractedFiles());
}

TEST_F(ExtractFilesTest, ExtractSingleSequentialFileBinary_User0_Match) {
    options.pattern = NULL;
    options.ascii_conversion = false;
    options.owner_id_filter = 0;

    std::vector<uint8_t> file_content_sector1_data(OASIS_SEQ_DATA_PER_SECTOR);
    for (size_t i = 0; i < file_content_sector1_data.size(); ++i) file_content_sector1_data[i] = (uint8_t)('A' + (i % 26));
    std::vector<uint8_t> file_content_sector2_data_actual(100);
    for (size_t i = 0; i < file_content_sector2_data_actual.size(); ++i) file_content_sector2_data_actual[i] = (uint8_t)('S' + (i % 5));
    std::vector<uint8_t> sector2_data_portion_buffer(OASIS_SEQ_DATA_PER_SECTOR, 0);
    memcpy(sector2_data_portion_buffer.data(), file_content_sector2_data_actual.data(), file_content_sector2_data_actual.size());

    uint16_t lba1_u0 = 10, lba2_u0 = 11;
    uint16_t lba1_u1 = 20, lba2_u1 = 21;

    WriteSequentialSectorToMockImage(mock_sector_io_stream, lba1_u0, file_content_sector1_data.data(), file_content_sector1_data.size(), lba2_u0);
    WriteSequentialSectorToMockImage(mock_sector_io_stream, lba2_u0, sector2_data_portion_buffer.data(), sector2_data_portion_buffer.size(), 0);
    std::vector<uint8_t> u1_s1_data(OASIS_SEQ_DATA_PER_SECTOR, 'X');
    std::vector<uint8_t> u1_s2_data(OASIS_SEQ_DATA_PER_SECTOR, 'Y');
    WriteSequentialSectorToMockImage(mock_sector_io_stream, lba1_u1, u1_s1_data.data(), u1_s1_data.size(), lba2_u1);
    WriteSequentialSectorToMockImage(mock_sector_io_stream, lba2_u1, u1_s2_data.data(), u1_s2_data.size(), 0);


    directory_entry_block_t deb_user0 = CreateDeb("SEQFILE0", "BIN", FILE_FORMAT_SEQUENTIAL, lba1_u0, 1, /*owner_id*/ 0, /*rec_cnt*/ 2, 0, lba2_u0);
    directory_entry_block_t deb_user1 = CreateDeb("SEQFILE1", "BIN", FILE_FORMAT_SEQUENTIAL, lba1_u1, 1, /*owner_id*/ 1, /*rec_cnt*/ 2, 0, lba2_u1);

    AddDebToDebVector(deb_user0);
    AddDebToDebVector(deb_user1);
    PopulateMockDirectory();

    ASSERT_TRUE(extract_files(mock_sector_io_stream, &disk_layout, temp_output_dir.string().c_str(), &options));

    std::filesystem::path expected_filepath_user0 = temp_output_dir / GetHostFilename(&deb_user0);
    std::filesystem::path expected_filepath_user1 = temp_output_dir / GetHostFilename(&deb_user1);

    ASSERT_TRUE(std::filesystem::exists(expected_filepath_user0));
    ASSERT_FALSE(std::filesystem::exists(expected_filepath_user1));

    std::vector<uint8_t> expected_full_content = file_content_sector1_data;
    expected_full_content.insert(expected_full_content.end(), sector2_data_portion_buffer.begin(), sector2_data_portion_buffer.end());
    std::vector<uint8_t> actual_content = tests_common::read_file_to_bytes(expected_filepath_user0);
    EXPECT_EQ(expected_full_content.size(), actual_content.size());
    EXPECT_EQ(expected_full_content, actual_content);
}

TEST_F(ExtractFilesTest, ExtractSingleSequentialFileBinary_User1_Match) {
    options.pattern = NULL;
    options.ascii_conversion = false;
    options.owner_id_filter = 1;

    std::vector<uint8_t> file_content_sector1_data(OASIS_SEQ_DATA_PER_SECTOR, 'A');
    std::vector<uint8_t> sector2_data_portion_buffer(OASIS_SEQ_DATA_PER_SECTOR, 'B');
    uint16_t lba1_u0 = 10, lba2_u0 = 11;
    uint16_t lba1_u1 = 20, lba2_u1 = 21;

    WriteSequentialSectorToMockImage(mock_sector_io_stream, lba1_u0, file_content_sector1_data.data(), file_content_sector1_data.size(), lba2_u0);
    WriteSequentialSectorToMockImage(mock_sector_io_stream, lba2_u0, sector2_data_portion_buffer.data(), sector2_data_portion_buffer.size(), 0);
    std::vector<uint8_t> u1_s1_data(OASIS_SEQ_DATA_PER_SECTOR, 'X');
    std::vector<uint8_t> u1_s2_data_actual(50, 'Z');
    std::vector<uint8_t> u1_s2_buffer(OASIS_SEQ_DATA_PER_SECTOR, 0);
    memcpy(u1_s2_buffer.data(), u1_s2_data_actual.data(), u1_s2_data_actual.size());
    WriteSequentialSectorToMockImage(mock_sector_io_stream, lba1_u1, u1_s1_data.data(), u1_s1_data.size(), lba2_u1);
    WriteSequentialSectorToMockImage(mock_sector_io_stream, lba2_u1, u1_s2_buffer.data(), u1_s2_buffer.size(), 0);

    directory_entry_block_t deb_user0 = CreateDeb("SEQFILE0", "BIN", FILE_FORMAT_SEQUENTIAL, lba1_u0, 1, /*owner_id*/ 0, 2, 0, lba2_u0);
    directory_entry_block_t deb_user1 = CreateDeb("SEQFILE1", "BIN", FILE_FORMAT_SEQUENTIAL, lba1_u1, 1, /*owner_id*/ 1, 2, 0, lba2_u1);

    AddDebToDebVector(deb_user0);
    AddDebToDebVector(deb_user1);
    PopulateMockDirectory();

    ASSERT_TRUE(extract_files(mock_sector_io_stream, &disk_layout, temp_output_dir.string().c_str(), &options));

    std::filesystem::path expected_filepath_user0 = temp_output_dir / GetHostFilename(&deb_user0);
    std::filesystem::path expected_filepath_user1 = temp_output_dir / GetHostFilename(&deb_user1);

    ASSERT_FALSE(std::filesystem::exists(expected_filepath_user0));
    ASSERT_TRUE(std::filesystem::exists(expected_filepath_user1));

    std::vector<uint8_t> expected_u1_content = u1_s1_data;
    expected_u1_content.insert(expected_u1_content.end(), u1_s2_buffer.begin(), u1_s2_buffer.end());
    std::vector<uint8_t> actual_u1_content = tests_common::read_file_to_bytes(expected_filepath_user1);
    EXPECT_EQ(expected_u1_content.size(), actual_u1_content.size());
    EXPECT_EQ(expected_u1_content, actual_u1_content);
}


TEST_F(ExtractFilesTest, ExtractSingleSequentialFileAscii_User0_Match) {
    options.pattern = NULL;
    options.ascii_conversion = true;
    options.owner_id_filter = 0;

    std::string oasis_payload_u0 = "User0 Line1\rUser0 Line2\r";
    std::vector<uint8_t> disk_data_u0(OASIS_SEQ_DATA_PER_SECTOR, 0);
    memcpy(disk_data_u0.data(), oasis_payload_u0.data(), oasis_payload_u0.length());
    if (oasis_payload_u0.length() < OASIS_SEQ_DATA_PER_SECTOR) {
        disk_data_u0[oasis_payload_u0.length()] = OASIS_SUB_CHAR_TEST;
    }

    std::string oasis_payload_u1 = "User1 Data\r";
    std::vector<uint8_t> disk_data_u1(OASIS_SEQ_DATA_PER_SECTOR, 0);
    memcpy(disk_data_u1.data(), oasis_payload_u1.data(), oasis_payload_u1.length());
    if (oasis_payload_u1.length() < OASIS_SEQ_DATA_PER_SECTOR) {
        disk_data_u1[oasis_payload_u1.length()] = OASIS_SUB_CHAR_TEST;
    }

    uint16_t lba_u0 = 15;
    uint16_t lba_u1 = 16;
    WriteSequentialSectorToMockImage(mock_sector_io_stream, lba_u0, disk_data_u0.data(), OASIS_SEQ_DATA_PER_SECTOR, 0);
    WriteSequentialSectorToMockImage(mock_sector_io_stream, lba_u1, disk_data_u1.data(), OASIS_SEQ_DATA_PER_SECTOR, 0);

    directory_entry_block_t deb_u0 = CreateDeb("TEXT_U0 ", "TXT", FILE_FORMAT_SEQUENTIAL, lba_u0, 1, /*owner_id*/0, 1, 0, lba_u0);
    directory_entry_block_t deb_u1 = CreateDeb("TEXT_U1 ", "TXT", FILE_FORMAT_SEQUENTIAL, lba_u1, 1, /*owner_id*/1, 1, 0, lba_u1);
    AddDebToDebVector(deb_u0);
    AddDebToDebVector(deb_u1);
    PopulateMockDirectory();

    ASSERT_TRUE(extract_files(mock_sector_io_stream, &disk_layout, temp_output_dir.string().c_str(), &options));
    std::filesystem::path filepath_u0 = temp_output_dir / GetHostFilename(&deb_u0);
    std::filesystem::path filepath_u1 = temp_output_dir / GetHostFilename(&deb_u1);

    ASSERT_TRUE(std::filesystem::exists(filepath_u0));
    ASSERT_FALSE(std::filesystem::exists(filepath_u1));

    std::string expected_host_text_u0 = std::string("User0 Line1") + HOST_LINE_ENDING + "User0 Line2" + HOST_LINE_ENDING;
    std::vector<uint8_t> expected_bytes_u0(expected_host_text_u0.begin(), expected_host_text_u0.end());
    std::vector<uint8_t> actual_content_u0 = tests_common::read_file_to_bytes(filepath_u0);
    EXPECT_EQ(actual_content_u0, expected_bytes_u0);
}


TEST_F(ExtractFilesTest, ExtractWithNullOptionsExtractsUser0) {
    /* Test extract_files with options = nullptr. Should extract files for user 0. */
    std::vector<uint8_t> content_u0_actual(10, 'N');
    std::vector<uint8_t> content_u1_actual(5, 'O');

    std::vector<uint8_t> sector_data_u0(OASIS_SEQ_DATA_PER_SECTOR, 0);
    memcpy(sector_data_u0.data(), content_u0_actual.data(), content_u0_actual.size());
    std::vector<uint8_t> sector_data_u1(OASIS_SEQ_DATA_PER_SECTOR, 0);
    memcpy(sector_data_u1.data(), content_u1_actual.data(), content_u1_actual.size());

    uint16_t lba_u0 = 5, lba_u1 = 6;
    directory_entry_block_t deb_u0 = CreateDeb("USER0FIL", "EXT", FILE_FORMAT_SEQUENTIAL, lba_u0, 1, /*owner_id*/0, 1, 0, lba_u0);
    directory_entry_block_t deb_u1 = CreateDeb("USER1FIL", "DAT", FILE_FORMAT_SEQUENTIAL, lba_u1, 1, /*owner_id*/1, 1, 0, lba_u1);

    WriteSequentialSectorToMockImage(mock_sector_io_stream, lba_u0, sector_data_u0.data(), OASIS_SEQ_DATA_PER_SECTOR, 0);
    WriteSequentialSectorToMockImage(mock_sector_io_stream, lba_u1, sector_data_u1.data(), OASIS_SEQ_DATA_PER_SECTOR, 0);

    AddDebToDebVector(deb_u0);
    AddDebToDebVector(deb_u1);
    PopulateMockDirectory();

    ASSERT_TRUE(extract_files(mock_sector_io_stream, &disk_layout, temp_output_dir.string().c_str(), nullptr))
        << "extract_files with nullptr options should succeed.";

    EXPECT_EQ(1, CountExtractedFiles()) << "Only user 0 files should be extracted when options is nullptr.";
    EXPECT_TRUE(std::filesystem::exists(temp_output_dir / GetHostFilename(&deb_u0)));
    EXPECT_FALSE(std::filesystem::exists(temp_output_dir / GetHostFilename(&deb_u1)));

    std::vector<uint8_t> actual_content_u0 = tests_common::read_file_to_bytes(temp_output_dir / GetHostFilename(&deb_u0));
    EXPECT_EQ(sector_data_u0, actual_content_u0);


    debs_vector.clear();
    std::error_code ec;
    std::filesystem::remove_all(temp_output_dir, ec);
    if (ec) {
        GTEST_LOG_(ERROR) << "Could not remove temp test directory " << temp_output_dir << ": " << ec.message();
    }
    std::filesystem::create_directories(temp_output_dir);
    PopulateMockDirectory();

    cli_options_t valid_options_for_efmp;
    memset(&valid_options_for_efmp, 0, sizeof(cli_options_t));
    valid_options_for_efmp.pattern = "*.*";
    valid_options_for_efmp.owner_id_filter = 0;


    EXPECT_FALSE(extract_files_matching_pattern(nullptr, &disk_layout, temp_output_dir.string().c_str(), &valid_options_for_efmp));
    EXPECT_FALSE(extract_files_matching_pattern(mock_sector_io_stream, nullptr, temp_output_dir.string().c_str(), &valid_options_for_efmp));
    EXPECT_FALSE(extract_files_matching_pattern(mock_sector_io_stream, &disk_layout, temp_output_dir.string().c_str(), nullptr));
    EXPECT_FALSE(extract_files_matching_pattern(mock_sector_io_stream, &disk_layout, nullptr, &valid_options_for_efmp));

    cli_options_t dummy_options_for_extract_files = { nullptr, nullptr, nullptr, false, 0 };
    EXPECT_FALSE(extract_files(nullptr, &disk_layout, temp_output_dir.string().c_str(), &dummy_options_for_extract_files));
    EXPECT_FALSE(extract_files(mock_sector_io_stream, nullptr, temp_output_dir.string().c_str(), &dummy_options_for_extract_files));
    EXPECT_FALSE(extract_files(mock_sector_io_stream, &disk_layout, nullptr, &dummy_options_for_extract_files));
}


TEST_F(ExtractFilesTest, ExtractPatternStarDotStarExtractsUserSpecificFiles) {
    std::vector<uint8_t> content_u0_f1_data(8, 'A');
    std::vector<uint8_t> content_u0_f2_data(6, 'B');
    std::vector<uint8_t> content_u1_f1_data(7, 'C');

    std::vector<uint8_t> sector_u0_f1(OASIS_SEQ_DATA_PER_SECTOR, 0); memcpy(sector_u0_f1.data(), content_u0_f1_data.data(), content_u0_f1_data.size());
    std::vector<uint8_t> sector_u0_f2(OASIS_SEQ_DATA_PER_SECTOR, 0); memcpy(sector_u0_f2.data(), content_u0_f2_data.data(), content_u0_f2_data.size());
    std::vector<uint8_t> sector_u1_f1(OASIS_SEQ_DATA_PER_SECTOR, 0); memcpy(sector_u1_f1.data(), content_u1_f1_data.data(), content_u1_f1_data.size());

    uint16_t lba_u0_f1 = 20, lba_u0_f2 = 21, lba_u1_f1 = 22;
    WriteSequentialSectorToMockImage(mock_sector_io_stream, lba_u0_f1, sector_u0_f1.data(), OASIS_SEQ_DATA_PER_SECTOR, 0);
    WriteSequentialSectorToMockImage(mock_sector_io_stream, lba_u0_f2, sector_u0_f2.data(), OASIS_SEQ_DATA_PER_SECTOR, 0);
    WriteSequentialSectorToMockImage(mock_sector_io_stream, lba_u1_f1, sector_u1_f1.data(), OASIS_SEQ_DATA_PER_SECTOR, 0);

    directory_entry_block_t deb_u0_f1 = CreateDeb("USER0F1 ", "DOC", FILE_FORMAT_SEQUENTIAL, lba_u0_f1, 1, /*owner_id*/0, 1, 0, lba_u0_f1);
    directory_entry_block_t deb_u0_f2 = CreateDeb("USER0F2 ", "TXT", FILE_FORMAT_SEQUENTIAL, lba_u0_f2, 1, /*owner_id*/0, 1, 0, lba_u0_f2);
    directory_entry_block_t deb_u1_f1 = CreateDeb("USER1F1 ", "DAT", FILE_FORMAT_SEQUENTIAL, lba_u1_f1, 1, /*owner_id*/1, 1, 0, lba_u1_f1);

    AddDebToDebVector(deb_u0_f1); AddDebToDebVector(deb_u0_f2); AddDebToDebVector(deb_u1_f1);
    PopulateMockDirectory();

    options.pattern = "*.*";
    options.ascii_conversion = false;

    /* Test Case 1: Filter for User 0 */
    options.owner_id_filter = 0;
    ASSERT_TRUE(extract_files_matching_pattern(mock_sector_io_stream, &disk_layout, temp_output_dir.string().c_str(), &options));
    EXPECT_EQ(2, CountExtractedFiles());
    EXPECT_TRUE(std::filesystem::exists(temp_output_dir / GetHostFilename(&deb_u0_f1)));
    EXPECT_TRUE(std::filesystem::exists(temp_output_dir / GetHostFilename(&deb_u0_f2)));
    EXPECT_FALSE(std::filesystem::exists(temp_output_dir / GetHostFilename(&deb_u1_f1)));

    /* Clean up extracted files for the next part of the test */
    if (std::filesystem::exists(temp_output_dir / GetHostFilename(&deb_u0_f1))) {
        std::filesystem::remove(temp_output_dir / GetHostFilename(&deb_u0_f1));
    }
    if (std::filesystem::exists(temp_output_dir / GetHostFilename(&deb_u0_f2))) {
        std::filesystem::remove(temp_output_dir / GetHostFilename(&deb_u0_f2));
    }
    if (std::filesystem::exists(temp_output_dir / GetHostFilename(&deb_u1_f1))) { /* Should not exist, but clean if it does */
        std::filesystem::remove(temp_output_dir / GetHostFilename(&deb_u1_f1));
    }


    /* Test Case 2: Filter for User 1 */
    options.owner_id_filter = 1;
    ASSERT_TRUE(extract_files_matching_pattern(mock_sector_io_stream, &disk_layout, temp_output_dir.string().c_str(), &options));
    EXPECT_EQ(1, CountExtractedFiles());
    EXPECT_FALSE(std::filesystem::exists(temp_output_dir / GetHostFilename(&deb_u0_f1)));
    EXPECT_FALSE(std::filesystem::exists(temp_output_dir / GetHostFilename(&deb_u0_f2)));
    EXPECT_TRUE(std::filesystem::exists(temp_output_dir / GetHostFilename(&deb_u1_f1)));
}


TEST_F(ExtractFilesTest, ExtractPatternSpecificMatchesOne_FilteredOutByUser) {
    options.owner_id_filter = 0;
    options.pattern = "MATCH.TXT";
    options.ascii_conversion = false;

    std::vector<uint8_t> content_txt_data_actual(12, 'T');
    std::vector<uint8_t> sector_data_txt(OASIS_SEQ_DATA_PER_SECTOR, 0);
    memcpy(sector_data_txt.data(), content_txt_data_actual.data(), content_txt_data_actual.size());

    uint16_t lba_txt = 30;
    WriteSequentialSectorToMockImage(mock_sector_io_stream, lba_txt, sector_data_txt.data(), OASIS_SEQ_DATA_PER_SECTOR, 0);

    directory_entry_block_t deb_txt_user1 = CreateDeb("MATCH   ", "TXT", FILE_FORMAT_SEQUENTIAL, lba_txt, 1, /*owner_id*/1, 1, 0, lba_txt);
    AddDebToDebVector(deb_txt_user1);
    PopulateMockDirectory();

    ASSERT_TRUE(extract_files_matching_pattern(mock_sector_io_stream, &disk_layout, temp_output_dir.string().c_str(), &options));
    EXPECT_EQ(0, CountExtractedFiles());
    ASSERT_FALSE(std::filesystem::exists(temp_output_dir / GetHostFilename(&deb_txt_user1)));
}

TEST_F(ExtractFilesTest, ExtractPatternSpecificMatchesOne_FilteredInByUser) {
    options.owner_id_filter = 5;
    options.pattern = "TARGET.DAT";
    options.ascii_conversion = false;

    std::vector<uint8_t> content_target_data(128, 'Q');
    std::vector<uint8_t> sector_data_target(OASIS_SEQ_DATA_PER_SECTOR, 0);
    memcpy(sector_data_target.data(), content_target_data.data(), content_target_data.size());

    std::vector<uint8_t> content_other_data(10, 'Z');
    std::vector<uint8_t> sector_data_other(OASIS_SEQ_DATA_PER_SECTOR, 0);
    memcpy(sector_data_other.data(), content_other_data.data(), content_other_data.size());


    uint16_t lba_target = 30;
    uint16_t lba_other_u5 = 31;
    uint16_t lba_other_u0 = 32;

    WriteSequentialSectorToMockImage(mock_sector_io_stream, lba_target, sector_data_target.data(), OASIS_SEQ_DATA_PER_SECTOR, 0);
    WriteSequentialSectorToMockImage(mock_sector_io_stream, lba_other_u5, sector_data_other.data(), OASIS_SEQ_DATA_PER_SECTOR, 0);
    WriteSequentialSectorToMockImage(mock_sector_io_stream, lba_other_u0, sector_data_target.data(), OASIS_SEQ_DATA_PER_SECTOR, 0);

    directory_entry_block_t deb_target_u5 = CreateDeb("TARGET  ", "DAT", FILE_FORMAT_SEQUENTIAL, lba_target, 1, /*owner_id*/5, 1, 0, lba_target);
    directory_entry_block_t deb_other_u5 = CreateDeb("OTHERFIL", "DAT", FILE_FORMAT_SEQUENTIAL, lba_other_u5, 1, /*owner_id*/5, 1, 0, lba_other_u5);
    /* Changed name of user 0's file to avoid host filename collision for assertion */
    directory_entry_block_t deb_target_u0 = CreateDeb("TARGET0 ", "DAT", FILE_FORMAT_SEQUENTIAL, lba_other_u0, 1, /*owner_id*/0, 1, 0, lba_other_u0);

    AddDebToDebVector(deb_target_u5);
    AddDebToDebVector(deb_other_u5);
    AddDebToDebVector(deb_target_u0);
    PopulateMockDirectory();

    ASSERT_TRUE(extract_files_matching_pattern(mock_sector_io_stream, &disk_layout, temp_output_dir.string().c_str(), &options));
    EXPECT_EQ(1, CountExtractedFiles());

    EXPECT_TRUE(std::filesystem::exists(temp_output_dir / GetHostFilename(&deb_target_u5)));
    EXPECT_FALSE(std::filesystem::exists(temp_output_dir / GetHostFilename(&deb_other_u5)));
    EXPECT_FALSE(std::filesystem::exists(temp_output_dir / GetHostFilename(&deb_target_u0)));

    std::vector<uint8_t> actual_target_content = tests_common::read_file_to_bytes(temp_output_dir / GetHostFilename(&deb_target_u5));
    EXPECT_EQ(sector_data_target, actual_target_content);
}


TEST_F(ExtractFilesTest, ExtractEmptySequentialFile_CorrectUser) {
    options.pattern = NULL;
    options.ascii_conversion = false;
    options.owner_id_filter = 0;

    uint16_t lba_start = 0;
    directory_entry_block_t deb = CreateDeb("EMPTY   ", "SEQ", FILE_FORMAT_SEQUENTIAL, lba_start, 0, /*owner_id*/0, 0, 0, 0);
    AddDebToDebVector(deb);
    PopulateMockDirectory();
    ASSERT_TRUE(extract_files(mock_sector_io_stream, &disk_layout, temp_output_dir.string().c_str(), &options));
    std::filesystem::path expected_filepath = temp_output_dir / GetHostFilename(&deb);
    ASSERT_TRUE(std::filesystem::exists(expected_filepath));
    std::vector<uint8_t> actual_content = tests_common::read_file_to_bytes(expected_filepath);
    EXPECT_TRUE(actual_content.empty());
}

TEST_F(ExtractFilesTest, ExtractEmptySequentialFile_WrongUser) {
    options.pattern = NULL;
    options.ascii_conversion = false;
    options.owner_id_filter = 0;

    uint16_t lba_start = 0;
    directory_entry_block_t deb = CreateDeb("EMPTY   ", "SEQ", FILE_FORMAT_SEQUENTIAL, lba_start, 0, /*owner_id*/1, 0, 0, 0);
    AddDebToDebVector(deb);
    PopulateMockDirectory();
    ASSERT_TRUE(extract_files(mock_sector_io_stream, &disk_layout, temp_output_dir.string().c_str(), &options));
    EXPECT_EQ(0, CountExtractedFiles());
}


TEST_F(ExtractFilesTest, ErrorDebIsInvalid) {
    options.pattern = NULL;
    options.ascii_conversion = false;
    options.owner_id_filter = 0;

    directory_entry_block_t deb = CreateDeb("BADDEB  ", "XXX", FILE_FORMAT_DELETED, 100, 1, /*owner_id*/0);
    AddDebToDebVector(deb);
    PopulateMockDirectory();
    bool result = extract_files(mock_sector_io_stream, &disk_layout, temp_output_dir.string().c_str(), &options);
    EXPECT_TRUE(result);
    EXPECT_EQ(0, CountExtractedFiles());
}

TEST_F(ExtractFilesTest, ExtractSequentialFileReadError_CorrectUser) {
    options.owner_id_filter = 7;
    uint32_t current_total_sectors = sector_io_get_total_sectors(mock_sector_io_stream);
    uint16_t lba1 = (uint16_t)(current_total_sectors + 5);
    directory_entry_block_t deb = CreateDeb("READERR ", "SEQ", FILE_FORMAT_SEQUENTIAL, lba1, 1, /*owner_id*/7, 1, 0, lba1);
    AddDebToDebVector(deb);
    PopulateMockDirectory();
    options.ascii_conversion = false;
    options.pattern = NULL;

    bool result = extract_files(mock_sector_io_stream, &disk_layout, temp_output_dir.string().c_str(), &options);
    EXPECT_FALSE(result) << "extract_files should return false if an underlying file read error causes an individual file extraction to fail.";
    std::filesystem::path expected_filepath = temp_output_dir / GetHostFilename(&deb);
    EXPECT_FALSE(std::filesystem::exists(expected_filepath)) << "File should not be created on read error.";
}

/* Specifically test the extract_files() wrapper with a non-default user filter */
/* New test case to specifically test the extract_files() wrapper with a non-default user filter */
TEST_F(ExtractFilesTest, ExtractFilesWrapper_FiltersByUser) {
    /* This test uses the extract_files() wrapper directly, not extract_files_matching_pattern() */
    /* Setup cli_options_t with a specific user filter */
    options.pattern = NULL; /* Indicates all files by pattern */
    options.ascii_conversion = false;
    options.owner_id_filter = 12; /* Filter for user 12 */

    /* Create DEBs for different users */
    uint16_t lba_u12_seq = 10;
    uint16_t lba_u13_contig = 20; /* Start LBA for user 13's contiguous file */
    uint16_t lba_u12_contig = 30; /* Start LBA for user 12's contiguous file */


    directory_entry_block_t deb_user12_seq = CreateDeb("USER12S ", "SEQ", FILE_FORMAT_SEQUENTIAL, lba_u12_seq, 1, /*owner_id*/12, /*record_count*/1, /*ffd1*/0, /*ffd2 for seq is last sector*/ lba_u12_seq);
    /* For contiguous file, block_count=1 means 1KB = 4 sectors. record_count and ffd1/ffd2 depend on type. */
    directory_entry_block_t deb_user13_contig = CreateDeb("USER13C ", "DAT", FILE_FORMAT_DIRECT, lba_u13_contig, /*block_count*/1, /*owner_id*/13, /*record_count*/1, /*ffd1 for DIRECT is rec_len*/128, /*ffd2 for DIRECT is 0*/0);
    directory_entry_block_t deb_user12_contig = CreateDeb("USER12C ", "BIN", FILE_FORMAT_ABSOLUTE, lba_u12_contig, /*block_count*/1, /*owner_id*/12, /*record_count (often 0 for ABS)*/0, /*ffd1 for ABS (sector_len)*/SECTOR_SIZE, /*ffd2 for ABS (load_addr)*/0x1000);

    /* Setup mock file content on disk */
    std::vector<uint8_t> data_u12_seq(OASIS_SEQ_DATA_PER_SECTOR, 'S');
    WriteSequentialSectorToMockImage(mock_sector_io_stream, lba_u12_seq, data_u12_seq.data(), data_u12_seq.size(), 0);

    std::vector<uint8_t> data_u13_contig(BLOCK_SIZE, 'T'); /* 1KB of data */
    uint32_t num_sectors_u13_contig = BLOCK_SIZE / SECTOR_SIZE;
    ASSERT_EQ(sector_io_write(mock_sector_io_stream, lba_u13_contig, num_sectors_u13_contig, data_u13_contig.data()), (ssize_t)num_sectors_u13_contig);

    std::vector<uint8_t> data_u12_contig(BLOCK_SIZE, 'R'); /* 1KB of data */
    uint32_t num_sectors_u12_contig = BLOCK_SIZE / SECTOR_SIZE;
    ASSERT_EQ(sector_io_write(mock_sector_io_stream, lba_u12_contig, num_sectors_u12_contig, data_u12_contig.data()), (ssize_t)num_sectors_u12_contig);


    AddDebToDebVector(deb_user12_seq);
    AddDebToDebVector(deb_user13_contig);
    AddDebToDebVector(deb_user12_contig);
    PopulateMockDirectory();

    /* Call extract_files with the configured options */
    ASSERT_TRUE(extract_files(mock_sector_io_stream, &disk_layout, temp_output_dir.string().c_str(), &options));

    /* Verify that only files for user 12 were extracted */
    EXPECT_EQ(2, CountExtractedFiles());
    EXPECT_TRUE(std::filesystem::exists(temp_output_dir / GetHostFilename(&deb_user12_seq)));
    EXPECT_TRUE(std::filesystem::exists(temp_output_dir / GetHostFilename(&deb_user12_contig)));
    EXPECT_FALSE(std::filesystem::exists(temp_output_dir / GetHostFilename(&deb_user13_contig)));

    /* Verify content of one of the extracted files */
    std::vector<uint8_t> actual_content_u12_seq = tests_common::read_file_to_bytes(temp_output_dir / GetHostFilename(&deb_user12_seq));
    EXPECT_EQ(data_u12_seq, actual_content_u12_seq);

    std::vector<uint8_t> actual_content_u12_contig = tests_common::read_file_to_bytes(temp_output_dir / GetHostFilename(&deb_user12_contig));
    /* For ABSOLUTE files, oasis_file_read_data reads the full block_count * BLOCK_SIZE.
       So data_u12_contig (which is BLOCK_SIZE) is the expected content.
    */
    EXPECT_EQ(data_u12_contig, actual_content_u12_contig);
}

/* --- New Test Cases for Wildcard Owner Functionality --- */

TEST_F(ExtractFilesTest, ExtractWildcardOwner_NoPattern_ExtractsAllUsers) {
    options.pattern = NULL;
    options.ascii_conversion = false;
    options.owner_id_filter = OWNER_ID_WILDCARD; /* Use the defined wildcard value */

    /* Create files for different owners */
    std::vector<uint8_t> content_u0_data(10, 'A');
    std::vector<uint8_t> content_u1_data(10, 'B');
    std::vector<uint8_t> content_u255_data(10, 'C');

    uint16_t lba_u0 = 40, lba_u1 = 41, lba_u255 = 42;
    WriteSequentialSectorToMockImage(mock_sector_io_stream, lba_u0, content_u0_data.data(), content_u0_data.size(), 0);
    WriteSequentialSectorToMockImage(mock_sector_io_stream, lba_u1, content_u1_data.data(), content_u1_data.size(), 0);
    WriteSequentialSectorToMockImage(mock_sector_io_stream, lba_u255, content_u255_data.data(), content_u255_data.size(), 0);

    directory_entry_block_t deb_u0 = CreateDeb("FILE_U0 ", "DAT", FILE_FORMAT_SEQUENTIAL, lba_u0, 1, /*owner_id*/0, 1, 0, lba_u0);
    directory_entry_block_t deb_u1 = CreateDeb("FILE_U1 ", "DAT", FILE_FORMAT_SEQUENTIAL, lba_u1, 1, /*owner_id*/1, 1, 0, lba_u1);
    directory_entry_block_t deb_u255 = CreateDeb("FILE_PUB", "DAT", FILE_FORMAT_SEQUENTIAL, lba_u255, 1, /*owner_id*/255, 1, 0, lba_u255);

    AddDebToDebVector(deb_u0);
    AddDebToDebVector(deb_u1);
    AddDebToDebVector(deb_u255);
    PopulateMockDirectory();

    ASSERT_TRUE(extract_files(mock_sector_io_stream, &disk_layout, temp_output_dir.string().c_str(), &options));

    EXPECT_EQ(3, CountExtractedFiles());
    EXPECT_TRUE(std::filesystem::exists(temp_output_dir / GetHostFilename(&deb_u0)));
    EXPECT_TRUE(std::filesystem::exists(temp_output_dir / GetHostFilename(&deb_u1)));
    EXPECT_TRUE(std::filesystem::exists(temp_output_dir / GetHostFilename(&deb_u255)));

    /* Verify content (optional, but good practice) */
    std::vector<uint8_t> expected_content_u0_padded(OASIS_SEQ_DATA_PER_SECTOR, 0);
    memcpy(expected_content_u0_padded.data(), content_u0_data.data(), content_u0_data.size());
    EXPECT_EQ(expected_content_u0_padded, tests_common::read_file_to_bytes(temp_output_dir / GetHostFilename(&deb_u0)));
}

TEST_F(ExtractFilesTest, ExtractWildcardOwner_WithPattern_ExtractsMatchingFromAllUsers) {
    options.pattern = "*.TXT";
    options.ascii_conversion = false;
    options.owner_id_filter = OWNER_ID_WILDCARD;

    /* Files for owner 0 */
    std::vector<uint8_t> content_u0_txt_data(10, 'A');
    std::vector<uint8_t> content_u0_dat_data(10, 'B');
    uint16_t lba_u0_txt = 50, lba_u0_dat = 51;
    WriteSequentialSectorToMockImage(mock_sector_io_stream, lba_u0_txt, content_u0_txt_data.data(), content_u0_txt_data.size(), 0);
    WriteSequentialSectorToMockImage(mock_sector_io_stream, lba_u0_dat, content_u0_dat_data.data(), content_u0_dat_data.size(), 0);
    directory_entry_block_t deb_u0_txt = CreateDeb("U0FILE  ", "TXT", FILE_FORMAT_SEQUENTIAL, lba_u0_txt, 1, /*owner_id*/0, 1, 0, lba_u0_txt);
    directory_entry_block_t deb_u0_dat = CreateDeb("U0FILE  ", "DAT", FILE_FORMAT_SEQUENTIAL, lba_u0_dat, 1, /*owner_id*/0, 1, 0, lba_u0_dat);

    /* Files for owner 10 */
    std::vector<uint8_t> content_u10_txt_data(10, 'C');
    std::vector<uint8_t> content_u10_doc_data(10, 'D');
    uint16_t lba_u10_txt = 52, lba_u10_doc = 53;
    WriteSequentialSectorToMockImage(mock_sector_io_stream, lba_u10_txt, content_u10_txt_data.data(), content_u10_txt_data.size(), 0);
    WriteSequentialSectorToMockImage(mock_sector_io_stream, lba_u10_doc, content_u10_doc_data.data(), content_u10_doc_data.size(), 0);
    directory_entry_block_t deb_u10_txt = CreateDeb("U10FILE ", "TXT", FILE_FORMAT_SEQUENTIAL, lba_u10_txt, 1, /*owner_id*/10, 1, 0, lba_u10_txt);
    directory_entry_block_t deb_u10_doc = CreateDeb("U10FILE ", "DOC", FILE_FORMAT_SEQUENTIAL, lba_u10_doc, 1, /*owner_id*/10, 1, 0, lba_u10_doc);


    AddDebToDebVector(deb_u0_txt);
    AddDebToDebVector(deb_u0_dat);
    AddDebToDebVector(deb_u10_txt);
    AddDebToDebVector(deb_u10_doc);
    PopulateMockDirectory();

    ASSERT_TRUE(extract_files_matching_pattern(mock_sector_io_stream, &disk_layout, temp_output_dir.string().c_str(), &options));

    EXPECT_EQ(2, CountExtractedFiles()); /* U0FILE.TXT and U10FILE.TXT */
    EXPECT_TRUE(std::filesystem::exists(temp_output_dir / GetHostFilename(&deb_u0_txt)));
    EXPECT_FALSE(std::filesystem::exists(temp_output_dir / GetHostFilename(&deb_u0_dat)));
    EXPECT_TRUE(std::filesystem::exists(temp_output_dir / GetHostFilename(&deb_u10_txt)));
    EXPECT_FALSE(std::filesystem::exists(temp_output_dir / GetHostFilename(&deb_u10_doc)));
}

TEST_F(ExtractFilesTest, ExtractWildcardOwner_EmptyDirectory) {
    options.pattern = NULL;
    options.ascii_conversion = false;
    options.owner_id_filter = OWNER_ID_WILDCARD;

    PopulateMockDirectory(); /* Empty directory */

    ASSERT_TRUE(extract_files(mock_sector_io_stream, &disk_layout, temp_output_dir.string().c_str(), &options));
    EXPECT_EQ(0, CountExtractedFiles());
}

/* --- Tests for create_and_open_oasis_file --- */
class CreateAndOpenTest : public ::testing::Test {
protected:
    std::filesystem::path temp_base_dir;
    std::filesystem::path output_dir;
    directory_entry_block_t dummy_deb;

    void SetUp() override {
        temp_base_dir = std::filesystem::temp_directory_path() / "oasis_extract_tests_co";
        std::filesystem::create_directories(temp_base_dir);
        const ::testing::TestInfo* const test_info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        output_dir = temp_base_dir / test_info->name();
        std::filesystem::create_directories(output_dir);
        tests_common::populate_deb(&dummy_deb, "TEST", "DAT", FILE_FORMAT_SEQUENTIAL, 0, 0, 0, 0, 0, /*owner_id*/0);
    }

    void TearDown() override {
        if (std::filesystem::exists(temp_base_dir)) {
            std::filesystem::remove_all(temp_base_dir);
        }
    }
};

TEST_F(CreateAndOpenTest, NullArgs) {
    FILE* ostream = nullptr;
    EXPECT_EQ(-EINVAL, create_and_open_oasis_file(nullptr, "file.txt", &ostream, &dummy_deb, 0, 0));
    EXPECT_EQ(-EINVAL, create_and_open_oasis_file(output_dir.string().c_str(), nullptr, &ostream, &dummy_deb, 0, 0));
    EXPECT_EQ(-EINVAL, create_and_open_oasis_file(output_dir.string().c_str(), "file.txt", nullptr, &dummy_deb, 0, 0));
    EXPECT_EQ(-EINVAL, create_and_open_oasis_file(output_dir.string().c_str(), "file.txt", &ostream, nullptr, 0, 0));
}

TEST_F(CreateAndOpenTest, DirExistsFileOpens) {
    FILE* ostream = nullptr;
    std::string base_filename = "testfile1.txt";
    std::filesystem::path full_path = output_dir / base_filename;

    EXPECT_EQ(0, create_and_open_oasis_file(output_dir.string().c_str(), base_filename.c_str(), &ostream, &dummy_deb, 1, 0));
    ASSERT_NE(nullptr, ostream);
    fclose(ostream);
    EXPECT_TRUE(std::filesystem::exists(full_path));
}

TEST_F(CreateAndOpenTest, DirNotExistsFileOpens) {
    FILE* ostream = nullptr;
    std::filesystem::path new_dir = output_dir / "newly_created_dir";
    std::string base_filename = "testfile2.txt";
    std::filesystem::path full_path = new_dir / base_filename;

    ASSERT_FALSE(std::filesystem::exists(new_dir));

    EXPECT_EQ(0, create_and_open_oasis_file(new_dir.string().c_str(), base_filename.c_str(), &ostream, &dummy_deb, 1, 0));
    ASSERT_NE(nullptr, ostream);
    fclose(ostream);
    EXPECT_TRUE(std::filesystem::exists(new_dir));
    EXPECT_TRUE(std::filesystem::is_directory(new_dir));
    EXPECT_TRUE(std::filesystem::exists(full_path));
}

TEST_F(CreateAndOpenTest, OutputPathIsAFile) {
    FILE* ostream = nullptr;
    std::filesystem::path path_as_file = output_dir / "iam_a_file.txt";
    std::string base_filename = "testfile3.txt";

    std::ofstream file_creator(path_as_file);
    file_creator << "hello";
    file_creator.close();
    ASSERT_TRUE(std::filesystem::exists(path_as_file) && !std::filesystem::is_directory(path_as_file));

    EXPECT_EQ(-ENOTDIR, create_and_open_oasis_file(path_as_file.string().c_str(), base_filename.c_str(), &ostream, &dummy_deb, 1, 0));
    EXPECT_EQ(nullptr, ostream);
}

TEST_F(CreateAndOpenTest, DebugAndQuietFlags) {
    FILE* ostream = nullptr;
    std::string base_filename = "test_debug.txt";
    std::filesystem::path full_path = output_dir / base_filename;

    EXPECT_EQ(0, create_and_open_oasis_file(output_dir.string().c_str(), base_filename.c_str(), &ostream, &dummy_deb, 0, 1));
    ASSERT_NE(nullptr, ostream);
    fclose(ostream);
    ostream = nullptr;
    std::filesystem::remove(full_path);

    EXPECT_EQ(0, create_and_open_oasis_file(output_dir.string().c_str(), base_filename.c_str(), &ostream, &dummy_deb, 1, 0));
    ASSERT_NE(nullptr, ostream);
    fclose(ostream);
    ostream = nullptr;
    std::filesystem::remove(full_path);

    EXPECT_EQ(0, create_and_open_oasis_file(output_dir.string().c_str(), base_filename.c_str(), &ostream, &dummy_deb, 1, 1));
    ASSERT_NE(nullptr, ostream);
    fclose(ostream);
    ostream = nullptr;
}


/* --- Test Fixture for set_file_timestamp (moved from test_oasis_utils.cpp) --- */
class SetFileTimestampTest : public ::testing::Test {
protected:
    std::filesystem::path temp_file_path;

    void SetUp() override {
        const ::testing::TestInfo* const test_info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        std::string unique_filename = std::string("oasis_ts_test_") + test_info->test_suite_name() + "_" + test_info->name() + ".tmp";
        temp_file_path = std::filesystem::temp_directory_path() / unique_filename;

        std::ofstream outfile(temp_file_path);
        outfile.close();
        ASSERT_TRUE(std::filesystem::exists(temp_file_path));
    }

    void TearDown() override {
        if (std::filesystem::exists(temp_file_path)) {
            std::filesystem::remove(temp_file_path);
        }
    }

    time_t get_file_mtime(const std::filesystem::path& filepath) {
#ifdef _MSC_VER
        struct _stat64i32 result;
        if (_stat64i32(filepath.string().c_str(), &result) == 0) {
            return result.st_mtime;
        }
#else
        struct stat result;
        if (stat(filepath.string().c_str(), &result) == 0) {
            return result.st_mtime;
        }
#endif
        return (time_t)-1;
    }
};

TEST_F(SetFileTimestampTest, SetValidTimestamp) {
    oasis_tm_t o_ts; /* 04/23/85 14:30 */
    o_ts.raw[0] = 0x4B;
    o_ts.raw[1] = 0xC3;
    o_ts.raw[2] = 0x9E;

    bool success = set_file_timestamp(temp_file_path.string().c_str(), &o_ts);

#if defined(HAVE_SYS_UTIME_H) || defined(HAVE_UTIME_H) || defined(_WIN32)
    EXPECT_TRUE(success);
    if (success) {
        time_t mtime = get_file_mtime(temp_file_path);
        ASSERT_NE(mtime, (time_t)-1);

        struct tm expected_tm_val;
        oasis_convert_timestamp_to_tm(&o_ts, &expected_tm_val);
        struct tm* actual_tm_val_local = localtime(&mtime);
        ASSERT_NE(nullptr, actual_tm_val_local);
        struct tm actual_tm_val = *actual_tm_val_local;

        EXPECT_EQ(actual_tm_val.tm_year, expected_tm_val.tm_year);
        EXPECT_EQ(actual_tm_val.tm_mon, expected_tm_val.tm_mon);
        EXPECT_EQ(actual_tm_val.tm_mday, expected_tm_val.tm_mday);
        EXPECT_EQ(actual_tm_val.tm_hour, expected_tm_val.tm_hour);
        EXPECT_EQ(actual_tm_val.tm_min, expected_tm_val.tm_min);
    }
#else
    EXPECT_TRUE(success) << "Timestamp setting should be a 'success' (no-op with warning) if utime/SetFileTime not available.";
#endif
}

TEST_F(SetFileTimestampTest, InvalidFilepath) {
    oasis_tm_t o_ts = { {0x4B, 0xC3, 0x9E} };
    bool success = set_file_timestamp("non_existent_dir/non_existent_file.txt", &o_ts);
    EXPECT_FALSE(success);
}
