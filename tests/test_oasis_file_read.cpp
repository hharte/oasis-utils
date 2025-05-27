/* tests/test_oasis_file_read.cpp */
#include "gtest/gtest.h"
#include "oasis.h"
#include <vector>
#include <string>
#include <cstring> /* For memcpy, memset */
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <cstdlib> /* For getenv */

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


/* Global variable to store the disk image path provided on the command line */
char g_oasis_disk_image_path[1024] = { 0 };

/* This will be defined by CMake via target_compile_definitions */
#ifndef OASIS_DISK_IMAGE_PATH_FROM_CMAKE
#define OASIS_DISK_IMAGE_PATH_FROM_CMAKE ""
#endif

class OasisReadFileTest : public ::testing::Test {
protected:
    directory_entry_block_t deb;
    std::vector<uint8_t> buffer;
    sector_io_stream_t* mock_sector_io_stream;
    std::filesystem::path temp_filepath;


    void SetUp() override {
        /* Initialize members */
        memset(&deb, 0, sizeof(deb));
        buffer.resize(1024);
        mock_sector_io_stream = nullptr;

        const ::testing::TestInfo* const test_info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        std::string unique_filename = std::string("oasis_file_read_test_") + test_info->test_suite_name() + "_" + test_info->name() + ".img";
        temp_filepath = std::filesystem::temp_directory_path() / unique_filename;
        std::filesystem::remove(temp_filepath); /* Clean before test */
    }

    void TearDown() override {
        if (mock_sector_io_stream != nullptr) {
            sector_io_close(mock_sector_io_stream);
            mock_sector_io_stream = nullptr;
        }
        if (!temp_filepath.empty()) {
            std::filesystem::remove(temp_filepath);
        }
    }
};

TEST_F(OasisReadFileTest, ReadInvalidArgs) {
    /* Create an empty dummy file for sector_io_open to succeed on. */
    ASSERT_TRUE(tests_common::create_dummy_image_file(temp_filepath, 0))
        << "Could not create temp file for ReadInvalidArgs test.";

    mock_sector_io_stream = sector_io_open(temp_filepath.string().c_str(), "rb");
    /* This was GTEST_SKIP before, changed to ASSERT for clarity in test flow. */
    ASSERT_NE(mock_sector_io_stream, nullptr)
        << "Could not open temp file with sector_io_open for ReadInvalidArgs test.";


    EXPECT_EQ(oasis_read_sequential_file(nullptr, mock_sector_io_stream, buffer.data(), buffer.size()), -1);
    EXPECT_EQ(errno, EINVAL);

    deb.file_format = FILE_FORMAT_SEQUENTIAL;
    EXPECT_EQ(oasis_read_sequential_file(&deb, mock_sector_io_stream, nullptr, 100), -1);
    EXPECT_EQ(errno, EINVAL);

    EXPECT_EQ(oasis_read_sequential_file(&deb, nullptr, buffer.data(), buffer.size()), -1);
    EXPECT_EQ(errno, EINVAL);
}

TEST_F(OasisReadFileTest, ReadNonSequentialDeb) {
    ASSERT_TRUE(tests_common::create_dummy_image_file(temp_filepath, 0))
        << "Could not create temp file for ReadNonSequentialDeb test.";

    mock_sector_io_stream = sector_io_open(temp_filepath.string().c_str(), "rb");
    ASSERT_NE(mock_sector_io_stream, nullptr)
        << "Could not open temp file with sector_io_open for ReadNonSequentialDeb test.";

    deb.file_format = FILE_FORMAT_DIRECT; /* Not sequential */
    memcpy(deb.file_name, "TEST    ", FNAME_LEN);
    memcpy(deb.file_type, "DAT     ", FTYPE_LEN);
    deb.start_sector = 10;
    deb.block_count = 1;

    EXPECT_EQ(oasis_read_sequential_file(&deb, mock_sector_io_stream, buffer.data(), buffer.size()), -1);
    EXPECT_EQ(errno, EINVAL);
}

TEST_F(OasisReadFileTest, ReadEmptyFileDeb) {
    ASSERT_TRUE(tests_common::create_dummy_image_file(temp_filepath, 0))
        << "Could not create empty temp file for ReadEmptyFileDeb test.";

    mock_sector_io_stream = sector_io_open(temp_filepath.string().c_str(), "rb");
    ASSERT_NE(mock_sector_io_stream, nullptr)
        << "Could not open temp file with sector_io_open for ReadEmptyFileDeb test.";

    deb.file_format = FILE_FORMAT_SEQUENTIAL;
    memcpy(deb.file_name, "EMPTY   ", FNAME_LEN);
    memcpy(deb.file_type, "FIL     ", FTYPE_LEN);
    deb.start_sector = 0;
    deb.file_format_dependent2 = 0;
    deb.block_count = 0;
    deb.record_count = 0;

    EXPECT_EQ(oasis_read_sequential_file(&deb, mock_sector_io_stream, buffer.data(), buffer.size()), 0);
}

TEST_F(OasisReadFileTest, ReadEmptyFileDebWarningMismatchLastSector) {
    ASSERT_TRUE(tests_common::create_dummy_image_file(temp_filepath, 0))
        << "Could not create empty temp file for test.";

    mock_sector_io_stream = sector_io_open(temp_filepath.string().c_str(), "rb");
    ASSERT_NE(mock_sector_io_stream, nullptr)
        << "Could not open temp file with sector_io_open for test.";

    deb.file_format = FILE_FORMAT_SEQUENTIAL;
    deb.start_sector = 0;
    deb.file_format_dependent2 = 100; /* Mismatch for empty file */
    EXPECT_EQ(oasis_read_sequential_file(&deb, mock_sector_io_stream, buffer.data(), buffer.size()), 0);
}

TEST_F(OasisReadFileTest, ReadEmptyFileDebWarningMismatchRecordCount) {
    ASSERT_TRUE(tests_common::create_dummy_image_file(temp_filepath, 0))
        << "Could not create empty temp file for test.";

    mock_sector_io_stream = sector_io_open(temp_filepath.string().c_str(), "rb");
    ASSERT_NE(mock_sector_io_stream, nullptr)
        << "Could not open temp file with sector_io_open for test.";

    deb.file_format = FILE_FORMAT_SEQUENTIAL;
    deb.start_sector = 0;
    deb.file_format_dependent2 = 0;
    deb.record_count = 5; /* Mismatch for empty file */
    EXPECT_EQ(oasis_read_sequential_file(&deb, mock_sector_io_stream, buffer.data(), buffer.size()), 0);
}


/* Test Fixture for Real Disk Image I/O */
class OasisReadFileDiskImageTest : public ::testing::Test {
protected:
    sector_io_stream_t* disk_image_sector_stream;
    std::vector<uint8_t> read_buffer;
    directory_entry_block_t current_deb;

    OasisReadFileDiskImageTest() : disk_image_sector_stream(nullptr) {}

    void SetUp() override {
        read_buffer.resize(65536);
        memset(&current_deb, 0, sizeof(current_deb));

        if (g_oasis_disk_image_path[0] == '\0') {
            GTEST_SKIP() << "Disk image path not provided. Skipping I/O tests. Provide with --disk_image=<path>";
            return;
        }
        disk_image_sector_stream = sector_io_open(g_oasis_disk_image_path, "rb");
        if (disk_image_sector_stream == nullptr) {
            FAIL() << "Failed to open disk image with sector_io_open: " << g_oasis_disk_image_path;
        }
    }

    void TearDown() override {
        if (disk_image_sector_stream != nullptr) {
            sector_io_close(disk_image_sector_stream);
            disk_image_sector_stream = nullptr;
        }
    }

    void PrepareDebForKnownFile(const char* name, const char* type,
        uint16_t start_sector, uint16_t block_count,
        uint16_t last_sector, uint16_t record_count) {
        tests_common::populate_deb(&current_deb, name, type, FILE_FORMAT_SEQUENTIAL,
            start_sector, block_count, record_count, 0, last_sector);
    }
};


TEST_F(OasisReadFileDiskImageTest, ReadKnownSingleSectorFile) {
    if (!disk_image_sector_stream) GTEST_SKIP();

    const char* test_filename = "REPEAT  ";
    const char* test_filetype = "EXEC    ";
    uint16_t start_sec = 744;
    uint16_t num_1k_blocks = 1;
    uint16_t last_sec_in_chain = 746;
    uint16_t rec_count = 26;
    /* Based on start=744, last=746, this implies 3 sectors: 744, 745, 746. */
    size_t expected_bytes = OASIS_SEQ_DATA_PER_SECTOR * 3;


    PrepareDebForKnownFile(test_filename, test_filetype, start_sec, num_1k_blocks, last_sec_in_chain, rec_count);

    ssize_t bytes_read = oasis_read_sequential_file(&current_deb, disk_image_sector_stream, read_buffer.data(), read_buffer.size());

    ASSERT_EQ(bytes_read, (ssize_t)expected_bytes) << "Number of bytes read does not match expected for "
        << test_filename << "." << test_filetype;

    if (bytes_read > 0) {
        printf("Read %zd bytes from %.8s.%.8s. First few bytes: ", bytes_read, current_deb.file_name, current_deb.file_type);
        for (int k = 0; k < std::min((ssize_t)16, bytes_read); ++k) {
            printf("%02X ", read_buffer[k]);
        }
        printf("\n");
    }
}


TEST_F(OasisReadFileDiskImageTest, ReadKnownMultiSectorFile) {
    if (!disk_image_sector_stream) GTEST_SKIP();


    const char* test_filename = "VOL1    ";
    const char* test_filetype = "EXEC    ";
    uint16_t start_sec = 312;
    uint16_t num_1k_blocks = 13;
    uint16_t last_sec_in_chain = 362;
    uint16_t rec_count = 416;
    /* Sectors in chain = 362 - 312 + 1 = 51 sectors */
    size_t expected_data_sectors = 51;
    size_t expected_bytes = OASIS_SEQ_DATA_PER_SECTOR * expected_data_sectors;

    PrepareDebForKnownFile(test_filename, test_filetype, start_sec, num_1k_blocks, last_sec_in_chain, rec_count);

    ssize_t bytes_read = oasis_read_sequential_file(&current_deb, disk_image_sector_stream, read_buffer.data(), read_buffer.size());

    ASSERT_EQ(bytes_read, (ssize_t)expected_bytes) << "Number of bytes read does not match expected for "
        << test_filename << "." << test_filetype;
    if (bytes_read > 0) {
        printf("Read %zd bytes from %.8s.%.8s. First few bytes: ", bytes_read, current_deb.file_name, current_deb.file_type);
        for (int k = 0; k < std::min((ssize_t)16, bytes_read); ++k) {
            printf("%02X ", read_buffer[k]);
        }
        printf("\n");
    }
}


/*
 * Test oasis_read_sequential_file with a buffer_size of 0.
 */
TEST_F(OasisReadFileTest, ReadSequentialWithZeroBufferSize) {
    /* Setup a minimal valid DEB and a 1-sector dummy image file */
    uint8_t sector_content[SECTOR_SIZE];
    memset(sector_content, 'A', OASIS_SEQ_DATA_PER_SECTOR);
    uint16_t next_lba = 0; /* End of chain */
    memcpy(sector_content + OASIS_SEQ_DATA_PER_SECTOR, &next_lba, sizeof(uint16_t));

    ASSERT_TRUE(tests_common::create_dummy_image_file(temp_filepath, 1));
    /* MODIFICATION: Open in "r+b" mode to allow writing for setup */
    mock_sector_io_stream = sector_io_open(temp_filepath.string().c_str(), "r+b");
    ASSERT_NE(mock_sector_io_stream, nullptr);

    /* Write content to LBA 10. This now should succeed. */
    ASSERT_EQ(sector_io_write(mock_sector_io_stream, 10, 1, sector_content), 1)
        << "Failed to write setup sector to LBA 10. Check stream mode and path.";

    tests_common::populate_deb(&deb, "ZEROBUF", "SEQ", FILE_FORMAT_SEQUENTIAL,
        10 /* start_sector */, 1 /* block_count */, 1 /* record_count */,
        0, 10 /* last_sector_lba (ffd2) */);

    /* Test with a non-NULL buffer but zero size */
    ssize_t bytes_read = oasis_read_sequential_file(&deb, mock_sector_io_stream, buffer.data(), 0);
    EXPECT_EQ(bytes_read, 0);

    /* Test with NULL buffer and zero size */
    bytes_read = oasis_read_sequential_file(&deb, mock_sector_io_stream, nullptr, 0);
    EXPECT_EQ(bytes_read, 0);
}

/*
 * Test reading a sequential file where the chain length (number of sectors)
 * exceeds the number of blocks allocated in the DEB (deb->block_count).
 * Expected: Error (-1) and errno set to EIO (or similar for inconsistency).
 */
TEST_F(OasisReadFileTest, ReadSequentialExceedsAllocatedBlocks) {
    /* Setup: Create a 2-sector chain (LBA 10 -> LBA 11 -> 0) */
    uint8_t sector_data[SECTOR_SIZE];
    memset(sector_data, 'A', OASIS_SEQ_DATA_PER_SECTOR);
    uint16_t lba10 = 10, lba11 = 11, lba_end = 0;
    memcpy(sector_data + OASIS_SEQ_DATA_PER_SECTOR, &lba11, sizeof(uint16_t)); /* LBA 10 links to 11 */

    ASSERT_TRUE(tests_common::create_dummy_image_file(temp_filepath, 2)); /* Create a 2-sector image */
    mock_sector_io_stream = sector_io_open(temp_filepath.string().c_str(), "r+b");
    ASSERT_NE(mock_sector_io_stream, nullptr);
    ASSERT_EQ(sector_io_write(mock_sector_io_stream, lba10, 1, sector_data), 1);

    memset(sector_data, 'B', OASIS_SEQ_DATA_PER_SECTOR);
    memcpy(sector_data + OASIS_SEQ_DATA_PER_SECTOR, &lba_end, sizeof(uint16_t)); /* LBA 11 links to 0 */
    ASSERT_EQ(sector_io_write(mock_sector_io_stream, lba11, 1, sector_data), 1);

    tests_common::populate_deb(&deb, "BADCOUNT", "SEQ", FILE_FORMAT_SEQUENTIAL,
        lba10, 0 /* block_count = 0, implying 0 sectors allocated */,
        2 /* record_count (actual sectors in chain) */,
        0, lba11 /* last_sector_lba (ffd2) */);

    ssize_t bytes_read = oasis_read_sequential_file(&deb, mock_sector_io_stream, buffer.data(), buffer.size());
    EXPECT_EQ(bytes_read, -1);
    EXPECT_EQ(errno, EIO); /* Expected error for inconsistency */
}

/*
 * Test reading a sequential file where the buffer is just large enough for the file.
 */
TEST_F(OasisReadFileTest, ReadSequentialBufferFillsExactly) {
    /* Setup a 2-sector file. Total data = 2 * OASIS_SEQ_DATA_PER_SECTOR */
    uint16_t lba1 = 20, lba2 = 21, lba_end = 0;
    size_t file_data_size = 2 * OASIS_SEQ_DATA_PER_SECTOR;
    std::vector<uint8_t> file_content(file_data_size);
    uint8_t sector_buffer_write[SECTOR_SIZE];

    /* Populate content and write sectors */
    for (size_t i = 0; i < file_data_size; ++i) file_content[i] = static_cast<uint8_t>(i % 256);

    ASSERT_TRUE(tests_common::create_dummy_image_file(temp_filepath, 2));
    mock_sector_io_stream = sector_io_open(temp_filepath.string().c_str(), "r+b");
    ASSERT_NE(mock_sector_io_stream, nullptr);

    memcpy(sector_buffer_write, file_content.data(), OASIS_SEQ_DATA_PER_SECTOR);
    memcpy(sector_buffer_write + OASIS_SEQ_DATA_PER_SECTOR, &lba2, sizeof(uint16_t));
    ASSERT_EQ(sector_io_write(mock_sector_io_stream, lba1, 1, sector_buffer_write), 1);

    memcpy(sector_buffer_write, file_content.data() + OASIS_SEQ_DATA_PER_SECTOR, OASIS_SEQ_DATA_PER_SECTOR);
    memcpy(sector_buffer_write + OASIS_SEQ_DATA_PER_SECTOR, &lba_end, sizeof(uint16_t));
    ASSERT_EQ(sector_io_write(mock_sector_io_stream, lba2, 1, sector_buffer_write), 1);

    tests_common::populate_deb(&deb, "EXACTBUF", "SEQ", FILE_FORMAT_SEQUENTIAL,
        lba1, 1 /* block_count for 2 sectors */, 2 /* record_count (sectors) */,
        0, lba2 /* last_sector_lba */);

    buffer.resize(file_data_size); /* Resize test buffer to exact size */
    ssize_t bytes_read = oasis_read_sequential_file(&deb, mock_sector_io_stream, buffer.data(), buffer.size());
    EXPECT_EQ(bytes_read, (ssize_t)file_data_size);
    EXPECT_EQ(memcmp(buffer.data(), file_content.data(), file_data_size), 0);
}

/*
 * Test reading a sequential file where the buffer is smaller than the total file content.
 * Expect a partial read and a warning (implicitly, not directly testable from C++ without output capture).
 */
TEST_F(OasisReadFileTest, ReadSequentialBufferFillsPartially) {
    /* Setup a 3-sector file. */
    uint16_t lba1 = 30, lba2 = 31, lba3 = 32, lba_end = 0;
    size_t total_file_data_size = 3 * OASIS_SEQ_DATA_PER_SECTOR;
    std::vector<uint8_t> full_file_content(total_file_data_size);
    uint8_t sector_buffer_write[SECTOR_SIZE];

    for (size_t i = 0; i < total_file_data_size; ++i) full_file_content[i] = static_cast<uint8_t>(i % 250);

    ASSERT_TRUE(tests_common::create_dummy_image_file(temp_filepath, 3));
    mock_sector_io_stream = sector_io_open(temp_filepath.string().c_str(), "r+b");
    ASSERT_NE(mock_sector_io_stream, nullptr);

    /* Write sector 1 (LBA 30) -> links to LBA 31 */
    memcpy(sector_buffer_write, full_file_content.data(), OASIS_SEQ_DATA_PER_SECTOR);
    memcpy(sector_buffer_write + OASIS_SEQ_DATA_PER_SECTOR, &lba2, sizeof(uint16_t));
    ASSERT_EQ(sector_io_write(mock_sector_io_stream, lba1, 1, sector_buffer_write), 1);

    /* Write sector 2 (LBA 31) -> links to LBA 32 */
    memcpy(sector_buffer_write, full_file_content.data() + OASIS_SEQ_DATA_PER_SECTOR, OASIS_SEQ_DATA_PER_SECTOR);
    memcpy(sector_buffer_write + OASIS_SEQ_DATA_PER_SECTOR, &lba3, sizeof(uint16_t));
    ASSERT_EQ(sector_io_write(mock_sector_io_stream, lba2, 1, sector_buffer_write), 1);

    /* Write sector 3 (LBA 32) -> links to 0 (end) */
    memcpy(sector_buffer_write, full_file_content.data() + 2 * OASIS_SEQ_DATA_PER_SECTOR, OASIS_SEQ_DATA_PER_SECTOR);
    memcpy(sector_buffer_write + OASIS_SEQ_DATA_PER_SECTOR, &lba_end, sizeof(uint16_t));
    ASSERT_EQ(sector_io_write(mock_sector_io_stream, lba3, 1, sector_buffer_write), 1);


    tests_common::populate_deb(&deb, "PARTBUF", "SEQ", FILE_FORMAT_SEQUENTIAL,
        lba1, 1 /* block_count for 3 sectors */, 3 /* record_count (sectors) */,
        0, lba3 /* last_sector_lba */);

    /* Buffer size for 1.5 sectors of data */
    size_t partial_buffer_size = OASIS_SEQ_DATA_PER_SECTOR + OASIS_SEQ_DATA_PER_SECTOR / 2;
    buffer.resize(partial_buffer_size);

    ssize_t bytes_read = oasis_read_sequential_file(&deb, mock_sector_io_stream, buffer.data(), buffer.size());
    EXPECT_EQ(bytes_read, (ssize_t)partial_buffer_size);
    EXPECT_EQ(memcmp(buffer.data(), full_file_content.data(), partial_buffer_size), 0);
    /* A warning should be printed by the C code, not easily verifiable here without output capture. */
}

/*
 * Test reading a sequential file where the chain correctly ends, but the
 * last sector LBA recorded in the DEB (ffd2) does not match the actual last sector.
 */
TEST_F(OasisReadFileTest, ReadSequentialChainLastSectorMismatch) {
    uint16_t lba1 = 40, lba2 = 41, lba_end = 0;
    uint8_t sector_data[SECTOR_SIZE];
    memset(sector_data, 'C', OASIS_SEQ_DATA_PER_SECTOR);

    ASSERT_TRUE(tests_common::create_dummy_image_file(temp_filepath, 2));
    mock_sector_io_stream = sector_io_open(temp_filepath.string().c_str(), "r+b");
    ASSERT_NE(mock_sector_io_stream, nullptr);

    memcpy(sector_data + OASIS_SEQ_DATA_PER_SECTOR, &lba2, sizeof(uint16_t)); /* LBA 40 links to 41 */
    ASSERT_EQ(sector_io_write(mock_sector_io_stream, lba1, 1, sector_data), 1);

    memcpy(sector_data + OASIS_SEQ_DATA_PER_SECTOR, &lba_end, sizeof(uint16_t)); /* LBA 41 links to 0 */
    ASSERT_EQ(sector_io_write(mock_sector_io_stream, lba2, 1, sector_data), 1);

    /* DEB expects last sector to be LBA 40, but actual chain ends at LBA 41 */
    tests_common::populate_deb(&deb, "LASTMISS", "SEQ", FILE_FORMAT_SEQUENTIAL,
        lba1, 1, 2, 0, lba1 /* Incorrect FFD2 */);

    buffer.resize(2 * OASIS_SEQ_DATA_PER_SECTOR); /* Ensure buffer is large enough */
    ssize_t bytes_read = oasis_read_sequential_file(&deb, mock_sector_io_stream, buffer.data(), buffer.size());
    EXPECT_EQ(bytes_read, -1);
    EXPECT_EQ(errno, EIO);
}


TEST_F(OasisReadFileTest, ReadDataNullOutputParams) {
    uint8_t* file_buf = nullptr;
    ssize_t bytes_rd = -1;

    /* Create a minimal valid DEB and stream */
    ASSERT_TRUE(tests_common::create_dummy_image_file(temp_filepath, 0));
    mock_sector_io_stream = sector_io_open(temp_filepath.string().c_str(), "rb");
    ASSERT_NE(mock_sector_io_stream, nullptr);
    tests_common::populate_deb(&deb, "ANY", "DAT", FILE_FORMAT_DIRECT, 0, 0, 0, 0, 0);

    EXPECT_FALSE(oasis_file_read_data(mock_sector_io_stream, &deb, nullptr, &bytes_rd));
    EXPECT_FALSE(oasis_file_read_data(mock_sector_io_stream, &deb, &file_buf, nullptr));
    /* Note: The function expects non-NULL for file_buffer_ptr and bytes_read_ptr */
}

TEST_F(OasisReadFileTest, ReadDataInvalidDebEmptyOrDeleted) {
    uint8_t* file_buf = nullptr;
    ssize_t bytes_rd = -1;

    ASSERT_TRUE(tests_common::create_dummy_image_file(temp_filepath, 0));
    mock_sector_io_stream = sector_io_open(temp_filepath.string().c_str(), "rb");
    ASSERT_NE(mock_sector_io_stream, nullptr);

    /* Test with FILE_FORMAT_EMPTY */
    tests_common::populate_deb(&deb, "EMPTY", "DEL", FILE_FORMAT_EMPTY, 0, 0, 0, 0, 0);
    EXPECT_TRUE(oasis_file_read_data(mock_sector_io_stream, &deb, &file_buf, &bytes_rd));
    EXPECT_EQ(bytes_rd, 0);
    EXPECT_EQ(file_buf, nullptr); /* No buffer should be allocated for an invalid/empty DEB */

    /* Test with FILE_FORMAT_DELETED */
    tests_common::populate_deb(&deb, "DELETED", "FIL", FILE_FORMAT_DELETED, 0, 0, 0, 0, 0);
    EXPECT_TRUE(oasis_file_read_data(mock_sector_io_stream, &deb, &file_buf, &bytes_rd));
    EXPECT_EQ(bytes_rd, 0);
    EXPECT_EQ(file_buf, nullptr);
}

TEST_F(OasisReadFileTest, ReadDataEmptyFileBlockCountZero) {
    uint8_t* file_buf = nullptr;
    ssize_t bytes_rd = -1;

    ASSERT_TRUE(tests_common::create_dummy_image_file(temp_filepath, 0));
    mock_sector_io_stream = sector_io_open(temp_filepath.string().c_str(), "rb");
    ASSERT_NE(mock_sector_io_stream, nullptr);

    /* Direct file, 0 blocks */
    tests_common::populate_deb(&deb, "EMPTYDIR", "DAT", FILE_FORMAT_DIRECT,
        0 /* start_sector */, 0 /* block_count */, 0, 0, 0);
    EXPECT_TRUE(oasis_file_read_data(mock_sector_io_stream, &deb, &file_buf, &bytes_rd));
    EXPECT_EQ(bytes_rd, 0);
    EXPECT_EQ(file_buf, nullptr);

    /* Sequential file, 0 blocks */
    tests_common::populate_deb(&deb, "EMPTYSEQ", "DAT", FILE_FORMAT_SEQUENTIAL,
        0 /* start_sector */, 0 /* block_count */, 0, 0, 0 /* ffd2 */);
    EXPECT_TRUE(oasis_file_read_data(mock_sector_io_stream, &deb, &file_buf, &bytes_rd));
    EXPECT_EQ(bytes_rd, 0);
    EXPECT_EQ(file_buf, nullptr);
}

TEST_F(OasisReadFileTest, ReadDataSeqEmptyFileNonZeroStartSectorWarning) {
    uint8_t* file_buf = nullptr;
    ssize_t bytes_rd = -1;

    ASSERT_TRUE(tests_common::create_dummy_image_file(temp_filepath, 0));
    mock_sector_io_stream = sector_io_open(temp_filepath.string().c_str(), "rb");
    ASSERT_NE(mock_sector_io_stream, nullptr);

    /*
     * Sequential DEB, block_count=0, but start_sector is non-zero.
     * This is a warning case; function should still report 0 bytes.
     */
    tests_common::populate_deb(&deb, "WARNSEQ", "DAT", FILE_FORMAT_SEQUENTIAL,
        10 /* start_sector */, 0 /* block_count */, 0, 0, 10 /* ffd2 */);
    EXPECT_TRUE(oasis_file_read_data(mock_sector_io_stream, &deb, &file_buf, &bytes_rd));
    EXPECT_EQ(bytes_rd, 0);
    EXPECT_EQ(file_buf, nullptr);
    /* A warning should be printed by the C code. */
}

TEST_F(OasisReadFileTest, ReadDataContiguousSingleBlock) {
    uint8_t* file_buf = nullptr;
    ssize_t bytes_rd = -1;
    uint16_t lba_start = 50;
    uint16_t num_blocks = 1;
    std::vector<uint8_t> expected_content(num_blocks * BLOCK_SIZE);
    for (size_t i = 0; i < expected_content.size(); ++i) expected_content[i] = static_cast<uint8_t>(i % 128);

    /* Create a dummy image with enough sectors */
    ASSERT_TRUE(tests_common::create_dummy_image_file(temp_filepath, lba_start + (num_blocks * (BLOCK_SIZE / SECTOR_SIZE))));
    mock_sector_io_stream = sector_io_open(temp_filepath.string().c_str(), "r+b");
    ASSERT_NE(mock_sector_io_stream, nullptr);

    /* Write the expected content to the mock image */
    ASSERT_EQ(sector_io_write(mock_sector_io_stream, lba_start, (uint32_t)(num_blocks * (BLOCK_SIZE / SECTOR_SIZE)), expected_content.data()),
        (ssize_t)(num_blocks * (BLOCK_SIZE / SECTOR_SIZE)));

    tests_common::populate_deb(&deb, "CONTIG1", "BIN", FILE_FORMAT_DIRECT,
        lba_start, num_blocks, (uint16_t)(BLOCK_SIZE / 128) /* record_count */, 128 /* ffd1=record_length */, 0 /* ffd2 */);


    EXPECT_TRUE(oasis_file_read_data(mock_sector_io_stream, &deb, &file_buf, &bytes_rd));
    ASSERT_NE(file_buf, nullptr);
    EXPECT_EQ(bytes_rd, (ssize_t)(num_blocks * BLOCK_SIZE));
    EXPECT_EQ(memcmp(file_buf, expected_content.data(), bytes_rd), 0);

    if (file_buf) free(file_buf);
}

TEST_F(OasisReadFileTest, ReadDataContiguousMultiBlock) {
    uint8_t* file_buf = nullptr;
    ssize_t bytes_rd = -1;
    uint16_t lba_start = 60;
    uint16_t num_blocks = 3; /* Requires 3 * 4 = 12 sectors */
    std::vector<uint8_t> expected_content(num_blocks * BLOCK_SIZE);
    for (size_t i = 0; i < expected_content.size(); ++i) expected_content[i] = static_cast<uint8_t>((i + 50) % 200);

    ASSERT_TRUE(tests_common::create_dummy_image_file(temp_filepath, lba_start + (num_blocks * (BLOCK_SIZE / SECTOR_SIZE))));
    mock_sector_io_stream = sector_io_open(temp_filepath.string().c_str(), "r+b");
    ASSERT_NE(mock_sector_io_stream, nullptr);
    ASSERT_EQ(sector_io_write(mock_sector_io_stream, lba_start, (uint32_t)(num_blocks * (BLOCK_SIZE / SECTOR_SIZE)), expected_content.data()),
        (ssize_t)(num_blocks * (BLOCK_SIZE / SECTOR_SIZE)));

    tests_common::populate_deb(&deb, "CONTIGM", "BIN", FILE_FORMAT_ABSOLUTE, /* Any contiguous type */
        lba_start, num_blocks, 0, 0, 0);

    EXPECT_TRUE(oasis_file_read_data(mock_sector_io_stream, &deb, &file_buf, &bytes_rd));
    ASSERT_NE(file_buf, nullptr);
    EXPECT_EQ(bytes_rd, (ssize_t)(num_blocks * BLOCK_SIZE));
    EXPECT_EQ(memcmp(file_buf, expected_content.data(), bytes_rd), 0);

    if (file_buf) free(file_buf);
}

/*
 * Test successful read of a sequential file through oasis_file_read_data.
 * This leverages the same setup as ReadSequentialBufferFillsExactly.
 */
TEST_F(OasisReadFileTest, ReadDataSequentialSuccess) {
    uint16_t lba1 = 20, lba2 = 21, lba_end = 0;
    size_t file_data_size = 2 * OASIS_SEQ_DATA_PER_SECTOR;
    std::vector<uint8_t> expected_file_content(file_data_size);
    uint8_t sector_buffer_write[SECTOR_SIZE];
    for (size_t i = 0; i < file_data_size; ++i) expected_file_content[i] = static_cast<uint8_t>(i % 256);

    ASSERT_TRUE(tests_common::create_dummy_image_file(temp_filepath, 2));
    mock_sector_io_stream = sector_io_open(temp_filepath.string().c_str(), "r+b");
    ASSERT_NE(mock_sector_io_stream, nullptr);
    memcpy(sector_buffer_write, expected_file_content.data(), OASIS_SEQ_DATA_PER_SECTOR);
    memcpy(sector_buffer_write + OASIS_SEQ_DATA_PER_SECTOR, &lba2, sizeof(uint16_t));
    ASSERT_EQ(sector_io_write(mock_sector_io_stream, lba1, 1, sector_buffer_write), 1);
    memcpy(sector_buffer_write, expected_file_content.data() + OASIS_SEQ_DATA_PER_SECTOR, OASIS_SEQ_DATA_PER_SECTOR);
    memcpy(sector_buffer_write + OASIS_SEQ_DATA_PER_SECTOR, &lba_end, sizeof(uint16_t));
    ASSERT_EQ(sector_io_write(mock_sector_io_stream, lba2, 1, sector_buffer_write), 1);

    tests_common::populate_deb(&deb, "SEQREAD", "DAT", FILE_FORMAT_SEQUENTIAL,
        lba1, 1, 2, 0, lba2);

    uint8_t* actual_file_buf = nullptr;
    ssize_t actual_bytes_read = -1;
    EXPECT_TRUE(oasis_file_read_data(mock_sector_io_stream, &deb, &actual_file_buf, &actual_bytes_read));
    ASSERT_NE(actual_file_buf, nullptr);
    EXPECT_EQ(actual_bytes_read, (ssize_t)file_data_size);
    EXPECT_EQ(memcmp(actual_file_buf, expected_file_content.data(), file_data_size), 0);

    if (actual_file_buf) free(actual_file_buf);
}

/*
 * Test error propagation if internal call to oasis_read_sequential_file fails.
 * We can trigger this by setting up a DEB that would cause an error in oasis_read_sequential_file,
 * for example, a chain that points to a non-existent sector if the image is small.
 */
TEST_F(OasisReadFileTest, ReadDataSequentialErrorPropagation) {
    /* Create a very small image (e.g., 1 sector at LBA 0) */
    ASSERT_TRUE(tests_common::create_dummy_image_file(temp_filepath, 1));
    mock_sector_io_stream = sector_io_open(temp_filepath.string().c_str(), "r+b");
    ASSERT_NE(mock_sector_io_stream, nullptr);
    uint8_t dummy_sector_data[SECTOR_SIZE] = { 0 };
    ASSERT_EQ(sector_io_write(mock_sector_io_stream, 0, 1, dummy_sector_data), 1);


    /* DEB for a sequential file starting at LBA 10 (which doesn't exist in our 1-sector image) */
    tests_common::populate_deb(&deb, "BADSEQ", "DAT", FILE_FORMAT_SEQUENTIAL,
        10 /* Non-existent start_sector */, 1, 1, 0, 10);

    uint8_t* file_buf = nullptr;
    ssize_t bytes_rd = 0; /* Initialize to a non -1 value */

    /*
     * This call to oasis_file_read_data should return false because the internal
     * call to oasis_read_sequential_file will fail (trying to read LBA 10).
     */
    EXPECT_FALSE(oasis_file_read_data(mock_sector_io_stream, &deb, &file_buf, &bytes_rd));
    EXPECT_EQ(bytes_rd, -1); /* Should be set to -1 on error by oasis_file_read_data */
    EXPECT_EQ(file_buf, nullptr); /* Buffer should not be (or remain) allocated on error */
}

/*
 * Test fixture for focused logical file size tests of oasis_file_read_data.
 * This can extend OasisReadFileTest or be a new standalone fixture if preferred.
 * For simplicity, we'll assume it can use helpers from OasisReadFileTest or test_oasis_common.
 */
class OasisReadFileDataLogicalSizeTest : public OasisReadFileTest {
protected:
    void SetUp() override {
        OasisReadFileTest::SetUp(); /* Call base class setup */
        /*
         * FIX: Open the mock_sector_io_stream here.
         * The base class SetUp prepares temp_filepath and sets mock_sector_io_stream to nullptr.
         * We need an open stream for SetupContiguousFileContent.
         */
        mock_sector_io_stream = sector_io_open(temp_filepath.string().c_str(), "w+b"); /* Open for writing and reading to setup mock image */
        ASSERT_NE(mock_sector_io_stream, nullptr)
            << "Failed to open temp_filepath for w+b in OasisReadFileDataLogicalSizeTest::SetUp. Path: "
            << temp_filepath.string();
    }

    void TearDown() override {
        /* Base class TearDown will close and remove the file. */
        OasisReadFileTest::TearDown();
    }

    /*
     * Helper to set up a mock contiguous file in the temp_image_path.
     * Writes patterned data for the specified number of allocated blocks.
     *
     * @param lba_start The starting LBA for the file content.
     * @param num_1k_blocks The number of 1K blocks allocated to the file.
     * @param content_pattern_base Base value for generating patterned data.
     */
    void SetupContiguousFileContent(uint16_t lba_start, uint16_t num_1k_blocks, uint8_t content_pattern_base) {
        ASSERT_NE(mock_sector_io_stream, nullptr) << "mock_sector_io_stream is NULL in SetupContiguousFileContent. Ensure it's opened in SetUp.";
        size_t total_bytes_to_write = (size_t)num_1k_blocks * BLOCK_SIZE;
        std::vector<uint8_t> file_image_data = tests_common::generate_patterned_data(total_bytes_to_write, content_pattern_base);

        uint32_t num_sectors_to_write = num_1k_blocks * (BLOCK_SIZE / SECTOR_SIZE);
        if (num_sectors_to_write == 0 && total_bytes_to_write > 0) {
            num_sectors_to_write = 1;
        }

        if (total_bytes_to_write > 0) {
            ssize_t sectors_written = sector_io_write(mock_sector_io_stream, lba_start, num_sectors_to_write, file_image_data.data());
            ASSERT_EQ(sectors_written, (ssize_t)num_sectors_to_write)
                << "Failed to write setup content for contiguous file at LBA " << lba_start;
        }
    }
};

/*
 * Test DIRECT file where logical size (record_count * record_length)
 * is less than allocated disk size (block_count * BLOCK_SIZE).
 * Expected: bytes_read_ptr should be the smaller logical size.
 */
TEST_F(OasisReadFileDataLogicalSizeTest, ReadDataDirectLogicalSizeLessThanAllocated) {
    uint8_t* file_buf = nullptr;
    ssize_t bytes_rd = -1;
    uint16_t lba_start = 70;
    uint16_t num_blocks = 1; /* Allocated: BLOCK_SIZE bytes */
    uint16_t record_count = 2;
    uint16_t record_length = 100;
    size_t expected_logical_size = (size_t)record_count * record_length; /* 2 * 100 = 200 bytes */

    /* Setup file content on mock image */
    SetupContiguousFileContent(lba_start, num_blocks, 0x30);

    /* Populate DEB */
    tests_common::populate_deb(&deb, "DIRECT1", "DAT", FILE_FORMAT_DIRECT,
        lba_start, num_blocks, record_count, record_length /* ffd1 */, 0 /* ffd2 */);

    /* Perform read */
    ASSERT_TRUE(oasis_file_read_data(mock_sector_io_stream, &deb, &file_buf, &bytes_rd));
    ASSERT_NE(file_buf, nullptr);

    /* Verify bytes read and content */
    EXPECT_EQ(bytes_rd, (ssize_t)expected_logical_size);

    /*
     * Verify that the content of file_buf matches the first 'expected_logical_size'
     * bytes of what was written to the mock image.
     */
    std::vector<uint8_t> full_block_content = tests_common::generate_patterned_data(BLOCK_SIZE, 0x30);
    EXPECT_EQ(0, memcmp(file_buf, full_block_content.data(), expected_logical_size))
        << "Content mismatch for DIRECT file with logical size < allocated.";

    if (file_buf) free(file_buf);
}

/*
 * Test DIRECT file where logical size calculates to 0 (e.g., record_count = 0),
 * but blocks are allocated.
 * Expected: bytes_read_ptr should default to the allocated disk size.
 */
TEST_F(OasisReadFileDataLogicalSizeTest, ReadDataDirectLogicalSizeZeroWithAllocatedBlocks) {
    uint8_t* file_buf = nullptr;
    ssize_t bytes_rd = -1;
    uint16_t lba_start = 80;
    uint16_t num_blocks = 1; /* Allocated: BLOCK_SIZE bytes */
    uint16_t record_count = 0; /* This will make calculated logical size zero */
    uint16_t record_length = 100;
    size_t expected_logical_size = BLOCK_SIZE; /* Should default to allocated size */

    SetupContiguousFileContent(lba_start, num_blocks, 0x40);

    tests_common::populate_deb(&deb, "DIRECT0", "DAT", FILE_FORMAT_DIRECT,
        lba_start, num_blocks, record_count, record_length, 0);

    ASSERT_TRUE(oasis_file_read_data(mock_sector_io_stream, &deb, &file_buf, &bytes_rd));
    ASSERT_NE(file_buf, nullptr);

    EXPECT_EQ(bytes_rd, (ssize_t)expected_logical_size);
    std::vector<uint8_t> full_block_content = tests_common::generate_patterned_data(BLOCK_SIZE, 0x40);
    EXPECT_EQ(0, memcmp(file_buf, full_block_content.data(), expected_logical_size))
        << "Content mismatch for DIRECT file defaulting to allocated size.";

    if (file_buf) free(file_buf);
}

/*
 * Test DIRECT file where calculated logical size is greater than allocated disk size.
 * Expected: bytes_read_ptr should be capped at the allocated disk size.
 */
TEST_F(OasisReadFileDataLogicalSizeTest, ReadDataDirectLogicalSizeGreaterThanAllocated) {
    uint8_t* file_buf = nullptr;
    ssize_t bytes_rd = -1;
    uint16_t lba_start = 90;
    uint16_t num_blocks = 1; /* Allocated: BLOCK_SIZE bytes */
    uint16_t record_length = 10;
    /* Make record_count such that record_count * record_length > BLOCK_SIZE */
    uint16_t record_count = (BLOCK_SIZE / record_length) + 5;

    size_t expected_logical_size = BLOCK_SIZE; /* Capped at allocated size */

    SetupContiguousFileContent(lba_start, num_blocks, 0x50);

    tests_common::populate_deb(&deb, "DIRECTCAP", "DAT", FILE_FORMAT_DIRECT,
        lba_start, num_blocks, record_count, record_length, 0);

    ASSERT_TRUE(oasis_file_read_data(mock_sector_io_stream, &deb, &file_buf, &bytes_rd));
    ASSERT_NE(file_buf, nullptr);

    EXPECT_EQ(bytes_rd, (ssize_t)expected_logical_size);
    std::vector<uint8_t> full_block_content = tests_common::generate_patterned_data(BLOCK_SIZE, 0x50);
    EXPECT_EQ(0, memcmp(file_buf, full_block_content.data(), expected_logical_size))
        << "Content mismatch for DIRECT file with logical size capped at allocated.";

    if (file_buf) free(file_buf);
}

/*
 * Test RELOCATABLE file where logical size is determined by ffd2 (Program Length).
 */
TEST_F(OasisReadFileDataLogicalSizeTest, ReadDataRelocatableLogicalSize) {
    uint8_t* file_buf = nullptr;
    ssize_t bytes_rd = -1;
    uint16_t lba_start = 100;
    uint16_t num_blocks = 2; /* Allocated: 2 * BLOCK_SIZE */
    uint16_t program_length = BLOCK_SIZE + 50; /* Logical size < allocated */
    size_t expected_logical_size = program_length;

    SetupContiguousFileContent(lba_start, num_blocks, 0x60);

    tests_common::populate_deb(&deb, "RELPROG", "COM", FILE_FORMAT_RELOCATABLE,
        lba_start, num_blocks, 0 /* rec_count unused for size */,
        0 /* ffd1 unused for size */, program_length /* ffd2 is Program Length */);

    ASSERT_TRUE(oasis_file_read_data(mock_sector_io_stream, &deb, &file_buf, &bytes_rd));
    ASSERT_NE(file_buf, nullptr);

    EXPECT_EQ(bytes_rd, (ssize_t)expected_logical_size);
    std::vector<uint8_t> allocated_content = tests_common::generate_patterned_data(num_blocks * BLOCK_SIZE, 0x60);
    EXPECT_EQ(0, memcmp(file_buf, allocated_content.data(), expected_logical_size))
        << "Content mismatch for RELOCATABLE file.";

    if (file_buf) free(file_buf);
}

/*
 * Test INDEXED file logical size.
 * logical_file_size = deb->record_count * (deb->file_format_dependent1 & 0x1FF)
 */
TEST_F(OasisReadFileDataLogicalSizeTest, ReadDataIndexedLogicalSize) {
    uint8_t* file_buf = nullptr;
    ssize_t bytes_rd = -1;
    uint16_t lba_start = 110;
    uint16_t num_blocks = 1; /* Allocated: BLOCK_SIZE */
    uint16_t record_count = 5;
    uint16_t record_length_val = 64;
    uint16_t key_length_val = 10; /* Example key length */
    uint16_t ffd1 = (key_length_val << 9) | record_length_val;
    size_t expected_logical_size = (size_t)record_count * record_length_val; /* 5 * 64 = 320 */

    ASSERT_LT(expected_logical_size, (size_t)BLOCK_SIZE) << "Test setup error: logical size should be less than block size for this test.";
    SetupContiguousFileContent(lba_start, num_blocks, 0x70);

    tests_common::populate_deb(&deb, "IDXFILE", "IDX", FILE_FORMAT_INDEXED,
        lba_start, num_blocks, record_count, ffd1, 0 /* ffd2 not used for size */);

    ASSERT_TRUE(oasis_file_read_data(mock_sector_io_stream, &deb, &file_buf, &bytes_rd));
    ASSERT_NE(file_buf, nullptr);

    EXPECT_EQ(bytes_rd, (ssize_t)expected_logical_size);
    std::vector<uint8_t> allocated_content = tests_common::generate_patterned_data(BLOCK_SIZE, 0x70);
    EXPECT_EQ(0, memcmp(file_buf, allocated_content.data(), expected_logical_size))
        << "Content mismatch for INDEXED file.";

    if (file_buf) free(file_buf);
}

/*
 * Test KEYED file logical size.
 * logical_file_size = deb->record_count * (deb->file_format_dependent1 & 0x1FF)
 */
TEST_F(OasisReadFileDataLogicalSizeTest, ReadDataKeyedLogicalSize) {
    uint8_t* file_buf = nullptr;
    ssize_t bytes_rd = -1;
    uint16_t lba_start = 120;
    uint16_t num_blocks = 1; /* Allocated: BLOCK_SIZE */
    uint16_t record_count = 3;
    uint16_t record_length_val = 128;
    uint16_t key_length_val = 16; /* Example key length */
    uint16_t ffd1 = (key_length_val << 9) | record_length_val;
    size_t expected_logical_size = (size_t)record_count * record_length_val; /* 3 * 128 = 384 */

    ASSERT_LT(expected_logical_size, (size_t)BLOCK_SIZE) << "Test setup error: logical size should be less than block size for this test.";
    SetupContiguousFileContent(lba_start, num_blocks, 0x80);

    tests_common::populate_deb(&deb, "KEYFILE", "KEY", FILE_FORMAT_KEYED,
        lba_start, num_blocks, record_count, ffd1, 0 /* ffd2 not used for size */);

    ASSERT_TRUE(oasis_file_read_data(mock_sector_io_stream, &deb, &file_buf, &bytes_rd));
    ASSERT_NE(file_buf, nullptr);

    EXPECT_EQ(bytes_rd, (ssize_t)expected_logical_size);
    std::vector<uint8_t> allocated_content = tests_common::generate_patterned_data(BLOCK_SIZE, 0x80);
    EXPECT_EQ(0, memcmp(file_buf, allocated_content.data(), expected_logical_size))
        << "Content mismatch for KEYED file.";

    if (file_buf) free(file_buf);
}

/*
 * Test ABSOLUTE file always uses the full allocated size.
 */
TEST_F(OasisReadFileDataLogicalSizeTest, ReadDataAbsoluteUsesAllocatedSize) {
    uint8_t* file_buf = nullptr;
    ssize_t bytes_rd = -1;
    uint16_t lba_start = 130;
    uint16_t num_blocks = 1; /* Allocated: BLOCK_SIZE */
    size_t expected_logical_size = BLOCK_SIZE;

    SetupContiguousFileContent(lba_start, num_blocks, 0x90);

    /*
     * For ABSOLUTE, record_count, ffd1, ffd2 are often specific (e.g. ffd2=load address)
     * but don't determine the logical size read by oasis_file_read_data.
     */
    tests_common::populate_deb(&deb, "ABSPROG", "ABS", FILE_FORMAT_ABSOLUTE,
        lba_start, num_blocks, 0, 0, 0x1000 /* Example load address */);

    ASSERT_TRUE(oasis_file_read_data(mock_sector_io_stream, &deb, &file_buf, &bytes_rd));
    ASSERT_NE(file_buf, nullptr);

    EXPECT_EQ(bytes_rd, (ssize_t)expected_logical_size);
    std::vector<uint8_t> allocated_content = tests_common::generate_patterned_data(BLOCK_SIZE, 0x90);
    EXPECT_EQ(0, memcmp(file_buf, allocated_content.data(), expected_logical_size))
        << "Content mismatch for ABSOLUTE file.";

    if (file_buf) free(file_buf);
}

/*
 * Test case where logical_file_size for a contiguous file becomes 0,
 * but block_count > 0, and ffd2 for Relocatable is also 0.
 * This should make logical_file_size default to actual_data_read_from_disk.
 */
TEST_F(OasisReadFileDataLogicalSizeTest, ReadDataRelocatableLogicalZeroBlockCountNonZero) {
    uint8_t* file_buf = nullptr;
    ssize_t bytes_rd = -1;
    uint16_t lba_start = 140;
    uint16_t num_blocks = 1; /* 1 block allocated */
    uint16_t program_length = 0; /* Logical size is 0 */
    size_t expected_logical_size = BLOCK_SIZE; /* Should default to allocated size */

    SetupContiguousFileContent(lba_start, num_blocks, 0xA0);

    tests_common::populate_deb(&deb, "RELZERO", "COM", FILE_FORMAT_RELOCATABLE,
        lba_start, num_blocks, 0, 0, program_length /* ffd2 is 0 */);

    ASSERT_TRUE(oasis_file_read_data(mock_sector_io_stream, &deb, &file_buf, &bytes_rd));
    ASSERT_NE(file_buf, nullptr);

    EXPECT_EQ(bytes_rd, (ssize_t)expected_logical_size);
    std::vector<uint8_t> allocated_content = tests_common::generate_patterned_data(BLOCK_SIZE, 0xA0);
    EXPECT_EQ(0, memcmp(file_buf, allocated_content.data(), expected_logical_size))
        << "Content mismatch for RELOCATABLE file defaulting to allocated size.";

    if (file_buf) free(file_buf);
}


/* Main function to parse custom arguments for Google Test */
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    bool path_resolved = false;

    /* 1. Try command-line argument first (for manual overrides) */
    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "--disk_image=", strlen("--disk_image=")) == 0) {
            strncpy(g_oasis_disk_image_path, argv[i] + strlen("--disk_image="), sizeof(g_oasis_disk_image_path) - 1);
            g_oasis_disk_image_path[sizeof(g_oasis_disk_image_path) - 1] = '\0';
            if (g_oasis_disk_image_path[0] != '\0') {
                printf("[Test Main] Using disk image from command line --disk_image: %s\n", g_oasis_disk_image_path);
                path_resolved = true;
            }
            break;
        }
    }

    /* 2. If not from command line, try CMake compile definition */
    if (!path_resolved) {
        const char* cmake_def_path = OASIS_DISK_IMAGE_PATH_FROM_CMAKE;
        if (cmake_def_path != nullptr && cmake_def_path[0] != '\0') {
            strncpy(g_oasis_disk_image_path, cmake_def_path, sizeof(g_oasis_disk_image_path) - 1);
            g_oasis_disk_image_path[sizeof(g_oasis_disk_image_path) - 1] = '\0';
            printf("[Test Main] Using disk image from CMake definition OASIS_DISK_IMAGE_PATH_FROM_CMAKE: %s\n", g_oasis_disk_image_path);
            path_resolved = true;
        }
        else {
            printf("[Test Main] CMake definition OASIS_DISK_IMAGE_PATH_FROM_CMAKE is empty or not defined.\n");
        }
    }

    /* 3. As a last resort, try environment variable */
    if (!path_resolved) {
        const char* env_path = std::getenv("OASIS_TEST_DISK_IMAGE");
        if (env_path != nullptr && env_path[0] != '\0') {
            strncpy(g_oasis_disk_image_path, env_path, sizeof(g_oasis_disk_image_path) - 1);
            g_oasis_disk_image_path[sizeof(g_oasis_disk_image_path) - 1] = '\0';
            printf("[Test Main] Using disk image from environment variable OASIS_TEST_DISK_IMAGE: %s\n", g_oasis_disk_image_path);
            path_resolved = true;
        }
        else {
            printf("[Test Main] Environment variable OASIS_TEST_DISK_IMAGE not found or empty.\n");
        }
    }

    if (g_oasis_disk_image_path[0] != '\0') {
        printf("[Test Main] Final disk image path for I/O tests: %s\n", g_oasis_disk_image_path);
    }
    else {
        printf("[Test Main] No disk image path resolved. I/O specific tests (OasisReadFileDiskImageTest) will be skipped.\n");
    }

    return RUN_ALL_TESTS();
}
