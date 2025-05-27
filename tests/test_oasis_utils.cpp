/* tests/test_oasis_utils.cpp */
/* GTest unit tests for utility functions in oasis_utils.c */

#include "gtest/gtest.h"
#include "gtest/gtest-spi.h" /* For output capturing */
#include <vector>
#include <string>
#include <fstream>      /* For std::ifstream, std::ofstream */
#include <cstdio>       /* For FILE, remove */
#include <cstring>      /* For memcpy, memset */
#include <cerrno>       /* For errno constants */
#include <algorithm>    /* For std::equal */
#include <cstdint>      /* For uint32_t, etc. */


/* DUT headers */
#include "oasis.h"

/* --- Test Fixture for cleanup_oasis_disk --- */
class OasisCleanupTest : public ::testing::Test {
protected:
    oasis_disk_layout_t disk_layout;

    void SetUp() override {
        memset(&disk_layout, 0, sizeof(disk_layout));
    }

    void TearDown() override {
        /* Ensure cleanup in case a test forgets or fails before its own cleanup */
        if (disk_layout.alloc_map.map_data) {
            free(disk_layout.alloc_map.map_data);
            disk_layout.alloc_map.map_data = nullptr;
        }
        if (disk_layout.directory) {
            free(disk_layout.directory);
            disk_layout.directory = nullptr;
        }
    }
};

TEST_F(OasisCleanupTest, CleanupNullLayout) {
    EXPECT_NO_THROW(cleanup_oasis_disk(nullptr));
}

TEST_F(OasisCleanupTest, CleanupWithNullMembers) {
    disk_layout.alloc_map.map_data = nullptr;
    disk_layout.directory = nullptr;
    EXPECT_NO_THROW(cleanup_oasis_disk(&disk_layout));
    EXPECT_EQ(nullptr, disk_layout.alloc_map.map_data);
    EXPECT_EQ(nullptr, disk_layout.directory);
}

TEST_F(OasisCleanupTest, CleanupWithAllocatedMembers) {
    disk_layout.alloc_map.map_data = (uint8_t*)malloc(10);
    ASSERT_NE(nullptr, disk_layout.alloc_map.map_data);
    disk_layout.alloc_map.map_size_bytes = 10;

    /* Allocate for the directory structure itself, not just the flexible array part for this test */
    disk_layout.directory = (oasis_directory_t*)malloc(sizeof(oasis_directory_t) + 5 * sizeof(directory_entry_block_t));
    ASSERT_NE(nullptr, disk_layout.directory);
    disk_layout.directory->directory_size_bytes = 5 * sizeof(directory_entry_block_t);

    EXPECT_NO_THROW(cleanup_oasis_disk(&disk_layout));

    EXPECT_EQ(nullptr, disk_layout.alloc_map.map_data) << "map_data should be nullified.";
    EXPECT_EQ(static_cast<size_t>(0), disk_layout.alloc_map.map_size_bytes) << "map_size_bytes should be zeroed.";
    EXPECT_EQ(nullptr, disk_layout.directory) << "directory should be nullified.";
}


/* --- Test Fixture for dump_hex --- */
class DumpHexTest : public ::testing::Test {};

TEST_F(DumpHexTest, NullDataZeroLen) {
    ::testing::internal::CaptureStdout();
    dump_hex(nullptr, 0);
    std::string output = ::testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("(No data to dump)"), std::string::npos);
}

TEST_F(DumpHexTest, ValidDataZeroLen) {
    uint8_t data[] = { 1, 2, 3 };
    ::testing::internal::CaptureStdout();
    dump_hex(data, 0);
    std::string output = ::testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("(No data to dump)"), std::string::npos);
}

TEST_F(DumpHexTest, ShortData) {
    uint8_t data[] = { 0x01, 0x02, 0x03, 0xAA, 0xBB };
    ::testing::internal::CaptureStdout();
    dump_hex(data, sizeof(data));
    std::string output = ::testing::internal::GetCapturedStdout();
    /* Expected: \n\t0000: 01 02 03 aa bb                                       |.....|\n\n */
    EXPECT_NE(output.find("01 02 03 aa bb"), std::string::npos);
    EXPECT_NE(output.find("|.....|"), std::string::npos);
}

TEST_F(DumpHexTest, ExactLineData) {
    uint8_t data[16];
    for (int i = 0; i < 16; ++i) data[i] = (uint8_t)i;
    data[0] = 'A'; data[1] = 'B'; /* Make some printable */
    ::testing::internal::CaptureStdout();
    dump_hex(data, sizeof(data));
    std::string output = ::testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("41 42 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f"), std::string::npos);
    EXPECT_NE(output.find("|AB..............|"), std::string::npos);
    EXPECT_EQ(std::count(output.begin(), output.end(), '\n'), 3); /* Intro, content, outro */
}

TEST_F(DumpHexTest, MultiLineData) {
    uint8_t data[20];
    for (int i = 0; i < 20; ++i) data[i] = (uint8_t)('a' + i);
    ::testing::internal::CaptureStdout();
    dump_hex(data, sizeof(data));
    std::string output = ::testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("0000: 61 62 63 64 65 66 67 68 69 6a 6b 6c 6d 6e 6f 70"), std::string::npos); /* Line 1 hex */
    EXPECT_NE(output.find("|abcdefghijklmnop|"), std::string::npos); /* Line 1 ASCII */
    EXPECT_NE(output.find("0010: 71 72 73 74"), std::string::npos); /* Line 2 hex */
    EXPECT_NE(output.find("|qrst|"), std::string::npos); /* Line 2 ASCII */
    EXPECT_EQ(std::count(output.begin(), output.end(), '\n'), 4); /* Intro, line1, line2, outro */
}

/* --- Tests for get_total_blocks (now in oasis_utils.c) --- */
TEST(OasisUtilsTest, GetTotalBlocks) {
    filesystem_block_t fs_block;
    memset(&fs_block, 0, sizeof(fs_block));

    // Test case 1: Typical values
    // Heads: 2 (0x20 >> 4), Cyls: 77, Sectors/Track: 26
    // Total sectors = 2 * 77 * 26 = 4004
    // Total 1K blocks = 4004 / 4 = 1001
    fs_block.num_heads = 0x2F; // High nibble for heads, low for drive type (ignored by func)
    fs_block.num_cyl = 77;
    fs_block.num_sectors = 26;
    EXPECT_EQ(get_total_blocks(&fs_block), static_cast<size_t>(1001));

    // Test case 2: Single head, minimal values
    // Heads: 1 (0x10 >> 4), Cyls: 1, Sectors/Track: 4 (min for one 1K block)
    // Total sectors = 1 * 1 * 4 = 4
    // Total 1K blocks = 4 / 4 = 1
    fs_block.num_heads = 0x10;
    fs_block.num_cyl = 1;
    fs_block.num_sectors = 4;
    EXPECT_EQ(get_total_blocks(&fs_block), static_cast<size_t>(1));

    // Test case 3: Zero heads
    fs_block.num_heads = 0x00;
    fs_block.num_cyl = 77;
    fs_block.num_sectors = 26;
    EXPECT_EQ(get_total_blocks(&fs_block), static_cast<size_t>(0));

    // Test case 4: Zero cylinders
    fs_block.num_heads = 0x20;
    fs_block.num_cyl = 0;
    fs_block.num_sectors = 26;
    EXPECT_EQ(get_total_blocks(&fs_block), static_cast<size_t>(0));

    // Test case 5: Zero sectors per track
    fs_block.num_heads = 0x20;
    fs_block.num_cyl = 77;
    fs_block.num_sectors = 0;
    EXPECT_EQ(get_total_blocks(&fs_block), static_cast<size_t>(0));

    // Test case 6: Sectors not a multiple of (BLOCK_SIZE / SECTOR_SIZE)
    // Heads: 1, Cyls: 1, Sectors/Track: 7
    // Total sectors = 7
    // Total 1K blocks = 7 / 4 = 1 (integer division)
    fs_block.num_heads = 0x10;
    fs_block.num_cyl = 1;
    fs_block.num_sectors = 7;
    EXPECT_EQ(get_total_blocks(&fs_block), static_cast<size_t>(1));

    // Test case 7: Null fs_block
    EXPECT_EQ(get_total_blocks(nullptr), static_cast<size_t>(0));
}
