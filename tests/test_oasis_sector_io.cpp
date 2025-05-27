/* tests/test_oasis_sector_io.cpp */
/* GTest unit tests for functions in oasis_sector_io.c */

#include "gtest/gtest.h"
#include <vector>
#include <string>
#include <fstream>      /* For std::ifstream, std::ofstream */
#include <cstdio>       /* For FILE, remove, L_tmpnam, tmpnam */
#include <cstring>      /* For memcpy, memset */
#include <cerrno>       /* For errno constants */
#include <filesystem>   /* For temporary directory management (C++17) */
#include <algorithm>    /* For std::fill, std::equal */

/* Common Test Utilities */
#include "test_oasis_common.h"

/* DUT headers */
#include "oasis.h"

class OasisSectorIoTest : public ::testing::Test {
protected:
    std::filesystem::path temp_image_path;
    sector_io_stream_t* sio_stream;
    std::vector<uint8_t> sector_buffer;

    /*
     * Helper to generate predictable sector data is now in tests_common::generate_patterned_data.
     * Helper to create a dummy image file is now tests_common::create_dummy_image_file.
     */

    void SetUp() override {
        sio_stream = nullptr;
        sector_buffer.resize(SECTOR_SIZE * 10); /* Buffer for up to 10 sectors */
        std::fill(sector_buffer.begin(), sector_buffer.end(), 0xDD); /* Fill with a pattern */

        /* Create a unique temporary filename for each test */
        const ::testing::TestInfo* const test_info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        std::string unique_filename = std::string("oasis_sio_test_") + test_info->test_suite_name() + "_" + test_info->name() + ".img";
        temp_image_path = std::filesystem::temp_directory_path() / unique_filename;

        /* Ensure it's clean before each test, though TearDown should handle it */
        std::filesystem::remove(temp_image_path);
    }

    void TearDown() override {
        if (sio_stream) {
            sector_io_close(sio_stream);
            sio_stream = nullptr;
        }
        std::filesystem::remove(temp_image_path);
    }
};

/* --- Tests for sector_io_open --- */

TEST_F(OasisSectorIoTest, OpenNullArgs) {
    EXPECT_EQ(nullptr, sector_io_open(nullptr, "rb"));
    EXPECT_EQ(nullptr, sector_io_open(temp_image_path.string().c_str(), nullptr));
}

TEST_F(OasisSectorIoTest, OpenNonExistentFileReadMode) {
    sio_stream = sector_io_open(temp_image_path.string().c_str(), "rb");
    EXPECT_EQ(nullptr, sio_stream) << "Opening non-existent file in 'rb' mode should fail.";
}

TEST_F(OasisSectorIoTest, OpenAndCreateWriteMode) {
    sio_stream = sector_io_open(temp_image_path.string().c_str(), "wb"); /* Should create */
    ASSERT_NE(nullptr, sio_stream) << "Failed to open/create file in 'wb' mode.";
    EXPECT_TRUE(std::filesystem::exists(temp_image_path));
    EXPECT_EQ(static_cast<uint32_t>(0), sector_io_get_total_sectors(sio_stream)) << "New file in 'wb' mode should have 0 sectors initially.";
}

TEST_F(OasisSectorIoTest, OpenAndCreateReadWritePlusMode) {
    sio_stream = sector_io_open(temp_image_path.string().c_str(), "w+b"); /* Should create or truncate */
    ASSERT_NE(nullptr, sio_stream) << "Failed to open/create file in 'w+b' mode.";
    EXPECT_TRUE(std::filesystem::exists(temp_image_path));
    EXPECT_EQ(static_cast<uint32_t>(0), sector_io_get_total_sectors(sio_stream)) << "New/truncated file in 'w+b' mode should have 0 sectors.";
}

TEST_F(OasisSectorIoTest, OpenExistingFileReadMode) {
    ASSERT_TRUE(tests_common::create_dummy_image_file(temp_image_path, 5)); /* Create a 5-sector image */
    sio_stream = sector_io_open(temp_image_path.string().c_str(), "rb");
    ASSERT_NE(nullptr, sio_stream) << "Failed to open existing file in 'rb' mode.";
    EXPECT_EQ(static_cast<uint32_t>(5), sector_io_get_total_sectors(sio_stream));
}

TEST_F(OasisSectorIoTest, OpenExistingFileReadWriteMode) {
    ASSERT_TRUE(tests_common::create_dummy_image_file(temp_image_path, 3)); /* Create a 3-sector image */
    sio_stream = sector_io_open(temp_image_path.string().c_str(), "r+b"); /* Should open existing, not truncate */
    ASSERT_NE(nullptr, sio_stream) << "Failed to open existing file in 'r+b' mode.";
    EXPECT_EQ(static_cast<uint32_t>(3), sector_io_get_total_sectors(sio_stream));
}

TEST_F(OasisSectorIoTest, OpenFileWithNonSectorMultipleSize) {
    std::ofstream outfile(temp_image_path, std::ios::binary);
    ASSERT_TRUE(outfile.is_open());
    outfile.write("abc", 3); /* 3 bytes, not a multiple of SECTOR_SIZE */
    outfile.close();

    sio_stream = sector_io_open(temp_image_path.string().c_str(), "rb");
    ASSERT_NE(nullptr, sio_stream);
    /* total_sectors should be 0 because file_size / SECTOR_SIZE truncates */
    EXPECT_EQ(static_cast<uint32_t>(0), sector_io_get_total_sectors(sio_stream));
}


/* --- Tests for sector_io_close --- */

TEST_F(OasisSectorIoTest, CloseValidStream) {
    ASSERT_TRUE(tests_common::create_dummy_image_file(temp_image_path, 1));
    sio_stream = sector_io_open(temp_image_path.string().c_str(), "rb");
    ASSERT_NE(nullptr, sio_stream);
    EXPECT_EQ(0, sector_io_close(sio_stream));
    sio_stream = nullptr; /* Prevent TearDown from closing again */
}

TEST_F(OasisSectorIoTest, CloseNullStream) {
    EXPECT_EQ(0, sector_io_close(nullptr));
}

/* --- Tests for sector_io_get_total_sectors --- */

TEST_F(OasisSectorIoTest, GetTotalSectorsEmptyFile) {
    sio_stream = sector_io_open(temp_image_path.string().c_str(), "w+b"); /* Creates empty */
    ASSERT_NE(nullptr, sio_stream);
    EXPECT_EQ(static_cast<uint32_t>(0), sector_io_get_total_sectors(sio_stream));
}

TEST_F(OasisSectorIoTest, GetTotalSectorsNullStream) {
    EXPECT_EQ(static_cast<uint32_t>(0), sector_io_get_total_sectors(nullptr));
}


/* --- Tests for sector_io_read --- */

TEST_F(OasisSectorIoTest, ReadNullArgs) {
    /* Need a valid stream to test other null args */
    ASSERT_TRUE(tests_common::create_dummy_image_file(temp_image_path, 1));
    sio_stream = sector_io_open(temp_image_path.string().c_str(), "rb");
    ASSERT_NE(nullptr, sio_stream);

    EXPECT_EQ(-1, sector_io_read(nullptr, 0, 1, sector_buffer.data()));
    EXPECT_EQ(-1, sector_io_read(sio_stream, 0, 1, nullptr));
    /* Reading 0 sectors should succeed and return 0 */
    EXPECT_EQ(0, sector_io_read(sio_stream, 0, 0, sector_buffer.data()));
}

TEST_F(OasisSectorIoTest, ReadFromEmptyImage) {
    sio_stream = sector_io_open(temp_image_path.string().c_str(), "w+b"); /* Creates empty file */
    ASSERT_NE(nullptr, sio_stream);
    EXPECT_EQ(static_cast<uint32_t>(0), sector_io_get_total_sectors(sio_stream));
    EXPECT_EQ(0, sector_io_read(sio_stream, 0, 1, sector_buffer.data()))
        << "Reading from empty image should return 0 sectors read.";
}

TEST_F(OasisSectorIoTest, ReadSingleSector) {
    ASSERT_TRUE(tests_common::create_dummy_image_file(temp_image_path, 1));
    std::vector<uint8_t> expected_data = tests_common::generate_patterned_data(SECTOR_SIZE, 0);

    sio_stream = sector_io_open(temp_image_path.string().c_str(), "rb");
    ASSERT_NE(nullptr, sio_stream);
    ASSERT_EQ(static_cast<uint32_t>(1), sector_io_get_total_sectors(sio_stream));

    ssize_t sectors_read = sector_io_read(sio_stream, 0, 1, sector_buffer.data());
    ASSERT_EQ(1, sectors_read);

    EXPECT_TRUE(std::equal(expected_data.begin(), expected_data.end(), sector_buffer.begin()));
}

TEST_F(OasisSectorIoTest, ReadMultipleSectors) {
    uint16_t num_to_test = 3;
    ASSERT_TRUE(tests_common::create_dummy_image_file(temp_image_path, num_to_test));
    sio_stream = sector_io_open(temp_image_path.string().c_str(), "rb");
    ASSERT_NE(nullptr, sio_stream);
    ASSERT_EQ(static_cast<uint32_t>(num_to_test), sector_io_get_total_sectors(sio_stream));

    ssize_t sectors_read = sector_io_read(sio_stream, 0, num_to_test, sector_buffer.data());
    ASSERT_EQ(num_to_test, sectors_read);

    for (uint16_t i = 0; i < num_to_test; ++i) {
        std::vector<uint8_t> expected_data = tests_common::generate_patterned_data(SECTOR_SIZE, static_cast<uint8_t>(i));
        EXPECT_TRUE(std::equal(expected_data.begin(), expected_data.end(),
            sector_buffer.begin() + (i * SECTOR_SIZE)));
    }
}

TEST_F(OasisSectorIoTest, ReadPastEndOfFile) {
    ASSERT_TRUE(tests_common::create_dummy_image_file(temp_image_path, 2)); /* Image has sectors 0, 1 */
    sio_stream = sector_io_open(temp_image_path.string().c_str(), "rb");
    ASSERT_NE(nullptr, sio_stream);

    /* Try to read 3 sectors starting at 0 (request goes 1 sector beyond EOF) */
    ssize_t sectors_read = sector_io_read(sio_stream, 0, 3, sector_buffer.data());
    EXPECT_EQ(2, sectors_read) << "Should read only available sectors up to EOF.";

    /* Try to read starting at EOF */
    sectors_read = sector_io_read(sio_stream, 2, 1, sector_buffer.data());
    EXPECT_EQ(0, sectors_read) << "Reading at or after EOF should return 0.";

    /* Try to read starting way past EOF */
    sectors_read = sector_io_read(sio_stream, 10, 1, sector_buffer.data());
    EXPECT_EQ(0, sectors_read) << "Reading far past EOF should return 0.";
}


/* --- Tests for sector_io_write --- */

TEST_F(OasisSectorIoTest, WriteNullArgs) {
    /* Need a valid stream to test other null args */
    sio_stream = sector_io_open(temp_image_path.string().c_str(), "w+b");
    ASSERT_NE(nullptr, sio_stream);
    std::vector<uint8_t> data_to_write = tests_common::generate_patterned_data(SECTOR_SIZE, 0);

    EXPECT_EQ(-1, sector_io_write(nullptr, 0, 1, data_to_write.data()));
    EXPECT_EQ(-1, sector_io_write(sio_stream, 0, 1, nullptr));
    /* Writing 0 sectors should succeed and return 0 */
    EXPECT_EQ(0, sector_io_write(sio_stream, 0, 0, data_to_write.data()));
}

TEST_F(OasisSectorIoTest, WriteSingleSectorToNewFile) {
    sio_stream = sector_io_open(temp_image_path.string().c_str(), "w+b");
    ASSERT_NE(nullptr, sio_stream);
    EXPECT_EQ(static_cast<uint32_t>(0), sector_io_get_total_sectors(sio_stream));

    std::vector<uint8_t> data_to_write = tests_common::generate_patterned_data(SECTOR_SIZE, 30); /* Pattern base 30 */
    ssize_t sectors_written = sector_io_write(sio_stream, 0, 1, data_to_write.data());
    ASSERT_EQ(1, sectors_written);
    EXPECT_EQ(static_cast<uint32_t>(1), sector_io_get_total_sectors(sio_stream)) << "Total sectors should update after write.";

    /* Verify by reading back */
    ssize_t sectors_read = sector_io_read(sio_stream, 0, 1, sector_buffer.data());
    ASSERT_EQ(1, sectors_read);
    EXPECT_TRUE(std::equal(data_to_write.begin(), data_to_write.end(), sector_buffer.begin()));
}

TEST_F(OasisSectorIoTest, WriteMultipleSectorsToNewFile) {
    sio_stream = sector_io_open(temp_image_path.string().c_str(), "w+b");
    ASSERT_NE(nullptr, sio_stream);
    uint16_t num_to_write = 3;
    std::vector<uint8_t> multi_sector_data_to_write;
    multi_sector_data_to_write.reserve(num_to_write * SECTOR_SIZE);
    for (uint16_t i = 0; i < num_to_write; ++i) {
        std::vector<uint8_t> s_data = tests_common::generate_patterned_data(SECTOR_SIZE, (uint8_t)(20 + i)); /* Pattern base 20, increment for each sector */
        multi_sector_data_to_write.insert(multi_sector_data_to_write.end(), s_data.begin(), s_data.end());
    }

    ssize_t sectors_written = sector_io_write(sio_stream, 0, num_to_write, multi_sector_data_to_write.data());
    ASSERT_EQ(num_to_write, sectors_written);
    EXPECT_EQ(static_cast<uint32_t>(num_to_write), sector_io_get_total_sectors(sio_stream));

    /* Verify by reading back */
    ssize_t sectors_read = sector_io_read(sio_stream, 0, num_to_write, sector_buffer.data());
    ASSERT_EQ(num_to_write, sectors_read);
    EXPECT_TRUE(std::equal(multi_sector_data_to_write.begin(), multi_sector_data_to_write.end(),
        sector_buffer.begin()));
}

TEST_F(OasisSectorIoTest, WriteToExtendFile) {
    ASSERT_TRUE(tests_common::create_dummy_image_file(temp_image_path, 1)); /* Sector 0 exists */
    sio_stream = sector_io_open(temp_image_path.string().c_str(), "r+b");
    ASSERT_NE(nullptr, sio_stream);
    ASSERT_EQ(static_cast<uint32_t>(1), sector_io_get_total_sectors(sio_stream));

    /* Write to sector 1 (which doesn't exist yet) */
    std::vector<uint8_t> data_s1 = tests_common::generate_patterned_data(SECTOR_SIZE, 30);
    ssize_t sectors_written = sector_io_write(sio_stream, 1, 1, data_s1.data());
    ASSERT_EQ(1, sectors_written);
    EXPECT_EQ(static_cast<uint32_t>(2), sector_io_get_total_sectors(sio_stream)) << "File should extend to 2 sectors.";

    /* Verify sector 1 */
    ssize_t sectors_read = sector_io_read(sio_stream, 1, 1, sector_buffer.data());
    ASSERT_EQ(1, sectors_read);
    EXPECT_TRUE(std::equal(data_s1.begin(), data_s1.end(), sector_buffer.begin()));

    /* Verify original sector 0 is untouched */
    std::vector<uint8_t> expected_s0 = tests_common::generate_patterned_data(SECTOR_SIZE, 0); /* Default pattern for CreateDummyImage */
    sectors_read = sector_io_read(sio_stream, 0, 1, sector_buffer.data());
    ASSERT_EQ(1, sectors_read);
    EXPECT_TRUE(std::equal(expected_s0.begin(), expected_s0.end(), sector_buffer.begin()));
}


TEST_F(OasisSectorIoTest, OverwriteExistingSectors) {
    ASSERT_TRUE(tests_common::create_dummy_image_file(temp_image_path, 2)); /* Sectors 0, 1 with default pattern */
    sio_stream = sector_io_open(temp_image_path.string().c_str(), "r+b");
    ASSERT_NE(nullptr, sio_stream);
    ASSERT_EQ(static_cast<uint32_t>(2), sector_io_get_total_sectors(sio_stream));

    /* Overwrite sector 0 */
    std::vector<uint8_t> new_s0_data = tests_common::generate_patterned_data(SECTOR_SIZE, 50); /* New pattern */
    ssize_t sectors_written = sector_io_write(sio_stream, 0, 1, new_s0_data.data());
    ASSERT_EQ(1, sectors_written);
    EXPECT_EQ(static_cast<uint32_t>(2), sector_io_get_total_sectors(sio_stream)); /* Size shouldn't change */

    /* Verify sector 0 is overwritten */
    ssize_t sectors_read = sector_io_read(sio_stream, 0, 1, sector_buffer.data());
    ASSERT_EQ(1, sectors_read);
    EXPECT_TRUE(std::equal(new_s0_data.begin(), new_s0_data.end(), sector_buffer.begin()));

    /* Verify sector 1 is untouched */
    std::vector<uint8_t> expected_s1_data = tests_common::generate_patterned_data(SECTOR_SIZE, 1); /* Original pattern for sector 1 */
    sectors_read = sector_io_read(sio_stream, 1, 1, sector_buffer.data());
    ASSERT_EQ(1, sectors_read);
    EXPECT_TRUE(std::equal(expected_s1_data.begin(), expected_s1_data.end(), sector_buffer.begin()));
}
