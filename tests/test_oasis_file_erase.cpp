/* tests/test_oasis_file_erase.cpp */
#include "gtest/gtest.h"
#include <vector>
#include <string>
#include <fstream>
#include <filesystem>
#include <cstring>

/* Common Test Utilities */
#include "test_oasis_common.h"

/* DUT headers */
extern "C" {
#include "oasis.h" /* For oasis_disk_layout_t, etc. */
#include "oasis_file_erase.h" /* The functions to test */
}

class OasisFileEraseTest : public ::testing::Test {
protected:
    std::filesystem::path temp_image_path;
    sector_io_stream_t* sio_stream;
    oasis_disk_layout_t disk_layout;
    std::vector<uint8_t> alloc_map_buffer; /* To back disk_layout.alloc_map.map_data */
    cli_options_t erase_options;

    void InitializeAllocMap(size_t num_1k_blocks_in_map, bool pre_allocate_system_blocks = true) {
        size_t map_bytes = (num_1k_blocks_in_map + 7) / 8;
        if (map_bytes == 0 && num_1k_blocks_in_map > 0) map_bytes = 1;

        alloc_map_buffer.assign(map_bytes, 0x00);
        disk_layout.alloc_map.map_data = alloc_map_buffer.data();
        disk_layout.alloc_map.map_size_bytes = alloc_map_buffer.size();

        if (pre_allocate_system_blocks && num_1k_blocks_in_map > 0) {
            if (set_block_state(&disk_layout.alloc_map, 0, 1) != 0) {
                GTEST_LOG_(WARNING) << "InitializeAllocMap: Failed to pre-allocate system block 0.";
            }
        }
        disk_layout.fsblock.fs_flags = 0;
        disk_layout.fsblock.free_blocks = (uint16_t)count_total_free_blocks(&disk_layout.alloc_map);
    }

    void PopulateMockDirectory(const std::vector<directory_entry_block_t>& debs) {
        if (disk_layout.directory) {
            free(disk_layout.directory);
            disk_layout.directory = NULL;
        }
        if (debs.empty()) {
            disk_layout.directory = (oasis_directory_t*)malloc(sizeof(oasis_directory_t));
            ASSERT_NE(nullptr, disk_layout.directory);
            disk_layout.directory->directory_size_bytes = 0;
            disk_layout.fsblock.dir_sectors_max = 0;
        } else {
            size_t dir_data_size = debs.size() * sizeof(directory_entry_block_t);
            disk_layout.directory = (oasis_directory_t*)malloc(sizeof(oasis_directory_t) + dir_data_size);
            ASSERT_NE(nullptr, disk_layout.directory);
            disk_layout.directory->directory_size_bytes = dir_data_size;
            memcpy(disk_layout.directory->directory, debs.data(), dir_data_size);
            disk_layout.fsblock.dir_sectors_max = (uint8_t)((dir_data_size + SECTOR_SIZE -1) / SECTOR_SIZE);
        }
    }

    void WriteDirectoryAndSystemBlocksToImage() {
        if (!sio_stream) return;
        /* Write FS block and initial AM (Sector 1) */
        uint8_t sector1_buffer[SECTOR_SIZE];
        memset(sector1_buffer, 0, SECTOR_SIZE); /* Ensure clean buffer */
        memcpy(sector1_buffer, &disk_layout.fsblock, sizeof(filesystem_block_t));
        if (disk_layout.alloc_map.map_data && disk_layout.alloc_map.map_size_bytes > 0) {
             size_t map_in_sector1 = SECTOR_SIZE - sizeof(filesystem_block_t);
             if (map_in_sector1 > disk_layout.alloc_map.map_size_bytes) map_in_sector1 = disk_layout.alloc_map.map_size_bytes;
            memcpy(sector1_buffer + sizeof(filesystem_block_t), disk_layout.alloc_map.map_data, map_in_sector1);
        }
        ASSERT_EQ(sector_io_write(sio_stream, 1, 1, sector1_buffer), 1);

        /* Write additional AM sectors (starting at Sector 2) */
        uint32_t additional_am_sectors_count = disk_layout.fsblock.fs_flags & ADDITIONAL_AM_SECTORS_MASK;
        if (additional_am_sectors_count > 0 && disk_layout.alloc_map.map_data) {
            size_t map_in_sector1 = SECTOR_SIZE - sizeof(filesystem_block_t);
            if (disk_layout.alloc_map.map_size_bytes > map_in_sector1) {
                 ASSERT_EQ(sector_io_write(sio_stream, 2, additional_am_sectors_count, disk_layout.alloc_map.map_data + map_in_sector1), (ssize_t)additional_am_sectors_count);
            }
        }
        /* Write Directory sectors */
        if (disk_layout.directory && disk_layout.fsblock.dir_sectors_max > 0) {
            uint32_t dir_start_lba = 1 + 1 + additional_am_sectors_count;
            ASSERT_EQ(sector_io_write(sio_stream, dir_start_lba, disk_layout.fsblock.dir_sectors_max, (uint8_t*)disk_layout.directory->directory), disk_layout.fsblock.dir_sectors_max);
        }
    }


    void SetUp() override {
        sio_stream = nullptr;
        memset(&disk_layout, 0, sizeof(disk_layout));
        alloc_map_buffer.clear();
        memset(&erase_options, 0, sizeof(erase_options));

        const ::testing::TestInfo* const test_info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        std::string unique_filename = std::string("oasis_erase_test_") + test_info->test_suite_name() + "_" + test_info->name() + ".img";
        temp_image_path = std::filesystem::temp_directory_path() / unique_filename;

        std::filesystem::remove(temp_image_path);

        sio_stream = sector_io_open(temp_image_path.string().c_str(), "w+b");
        ASSERT_NE(nullptr, sio_stream) << "Failed to create/open mock disk image: " << temp_image_path.string();

        /* Initialize a small disk for tests */
        InitializeAllocMap(100, true); /* 100 1K blocks -> 104 actual map blocks, sys block 0 pre-allocated -> 103 free */
        disk_layout.fsblock.dir_sectors_max = 2; /* Enough for 16 DEBs */
        std::vector<directory_entry_block_t> empty_debs;
        PopulateMockDirectory(empty_debs); /* Create an empty directory initially */
        WriteDirectoryAndSystemBlocksToImage(); /* Write initial empty state */

        erase_options.owner_id_filter = 0; /* Default */
    }

    void TearDown() override {
        if (sio_stream) {
            sector_io_close(sio_stream);
            sio_stream = nullptr;
        }
        if (disk_layout.directory) {
            free(disk_layout.directory);
            disk_layout.directory = nullptr;
        }
        std::filesystem::remove(temp_image_path);
    }
};

TEST_F(OasisFileEraseTest, EraseSingleContiguousFile) {
    std::vector<directory_entry_block_t> debs;
    directory_entry_block_t deb1;
    memset(&deb1, 0, sizeof(deb1)); /* Ensure it's zeroed before populate */

    /* Create a file */
    tests_common::populate_deb(&deb1, "CONTIGF", "DAT", FILE_FORMAT_DIRECT, 0, 0, 0, 0, 0, 0);
    std::vector<uint8_t> data_to_write = tests_common::generate_patterned_data(BLOCK_SIZE * 2, 0xAA); /* Needs 2 blocks */
    /* Before write: disk_layout.fsblock.free_blocks should be 103 */
    ASSERT_EQ(disk_layout.fsblock.free_blocks, 103);

    ASSERT_TRUE(oasis_file_write_data(sio_stream, &disk_layout, &deb1, data_to_write.data(), data_to_write.size()));
    /* After write: disk_layout.fsblock.free_blocks should be 103 - 2 = 101 */
    ASSERT_EQ(disk_layout.fsblock.free_blocks, 101);

    debs.push_back(deb1);
    PopulateMockDirectory(debs);
    WriteDirectoryAndSystemBlocksToImage(); /* Save state after write, fsblock.free_blocks = 101 */

    uint16_t initial_free_blocks = disk_layout.fsblock.free_blocks; /* Should be 101 */
    ASSERT_EQ(initial_free_blocks, 101);
    uint16_t blocks_of_file_to_erase = deb1.block_count; /* Should be 2 */
    ASSERT_EQ(blocks_of_file_to_erase, 2);

    erase_options.pattern = "CONTIGF.DAT";
    ASSERT_TRUE(oasis_erase_files_by_pattern(sio_stream, &disk_layout, &erase_options));
    /* After erase: disk_layout.fsblock.free_blocks should be 101 + 2 = 103 */
    ASSERT_EQ(disk_layout.fsblock.free_blocks, 103);


    /* Verify DEB is marked deleted */
    EXPECT_EQ(disk_layout.directory->directory[0].file_format, FILE_FORMAT_DELETED);
    EXPECT_EQ(disk_layout.directory->directory[0].block_count, 0);
    EXPECT_EQ(disk_layout.directory->directory[0].start_sector, 0);


    /* Verify blocks are freed in alloc map */
    int alloc_state;
    uint16_t original_start_1k_block = deb1.start_sector / (BLOCK_SIZE/SECTOR_SIZE);
    for (uint16_t i = 0; i < blocks_of_file_to_erase; ++i) {
        ASSERT_EQ(0, get_block_state(&disk_layout.alloc_map, original_start_1k_block + i, &alloc_state));
        EXPECT_EQ(0, alloc_state) << "Block " << (original_start_1k_block + i) << " should be free.";
    }
    EXPECT_EQ(disk_layout.fsblock.free_blocks, initial_free_blocks + blocks_of_file_to_erase); /* 101 + 2 = 103 */

    /* Reload and verify from disk */
    oasis_disk_layout_t reloaded_layout;
    memset(&reloaded_layout, 0, sizeof(reloaded_layout));
    sector_io_stream_t* temp_sio = sector_io_open(temp_image_path.string().c_str(), "rb");
    ASSERT_NE(nullptr, temp_sio);
    ASSERT_TRUE(load_oasis_disk(temp_sio, &reloaded_layout));
    EXPECT_EQ(reloaded_layout.directory->directory[0].file_format, FILE_FORMAT_DELETED);
    EXPECT_EQ(reloaded_layout.fsblock.free_blocks, initial_free_blocks + blocks_of_file_to_erase); /* Expected 103 */
    cleanup_oasis_disk(&reloaded_layout);
    sector_io_close(temp_sio);
}

TEST_F(OasisFileEraseTest, EraseSingleSequentialFile) {
    std::vector<directory_entry_block_t> debs;
    directory_entry_block_t deb_seq;
    memset(&deb_seq, 0, sizeof(deb_seq));

    tests_common::populate_deb(&deb_seq, "SEQFILE", "TXT", FILE_FORMAT_SEQUENTIAL, 0, 0, 0, 0, 0, 0);
    std::vector<uint8_t> data_seq = tests_common::generate_patterned_data(OASIS_SEQ_DATA_PER_SECTOR * 5, 0xBB); /* Uses 2 1K-blocks */
    uint16_t free_blocks_before_write = disk_layout.fsblock.free_blocks;
    ASSERT_TRUE(oasis_file_write_data(sio_stream, &disk_layout, &deb_seq, data_seq.data(), data_seq.size()));
    uint16_t blocks_written = deb_seq.block_count;
    ASSERT_EQ(blocks_written, 2);
    ASSERT_EQ(disk_layout.fsblock.free_blocks, free_blocks_before_write - blocks_written);

    debs.push_back(deb_seq);
    PopulateMockDirectory(debs);
    WriteDirectoryAndSystemBlocksToImage();

    uint16_t initial_free_blocks_seq = disk_layout.fsblock.free_blocks;
    uint16_t blocks_of_file_to_erase_seq = deb_seq.block_count;
    ASSERT_EQ(blocks_of_file_to_erase_seq, 2);

    std::vector<uint16_t> original_used_1k_blocks;
    uint16_t current_lba = deb_seq.start_sector;
    uint32_t sectors_walked = 0;
    uint8_t sector_buffer[SECTOR_SIZE];
    while(current_lba != 0 && sectors_walked < 100) {
        sectors_walked++;
        uint16_t k_block = current_lba / (BLOCK_SIZE / SECTOR_SIZE);
        bool found = false;
        for(uint16_t b : original_used_1k_blocks) if(b == k_block) found = true;
        if(!found) original_used_1k_blocks.push_back(k_block);
        ASSERT_EQ(sector_io_read(sio_stream, current_lba, 1, sector_buffer), 1);
        memcpy(&current_lba, sector_buffer + OASIS_SEQ_DATA_PER_SECTOR, sizeof(uint16_t));
    }
    ASSERT_EQ(original_used_1k_blocks.size(), (size_t)blocks_of_file_to_erase_seq);

    erase_options.pattern = "SEQFILE.TXT";
    ASSERT_TRUE(oasis_erase_files_by_pattern(sio_stream, &disk_layout, &erase_options));

    EXPECT_EQ(disk_layout.directory->directory[0].file_format, FILE_FORMAT_DELETED);
    EXPECT_EQ(disk_layout.fsblock.free_blocks, initial_free_blocks_seq + blocks_of_file_to_erase_seq);

    int alloc_state_seq;
    for (uint16_t block_idx : original_used_1k_blocks) {
        ASSERT_EQ(0, get_block_state(&disk_layout.alloc_map, block_idx, &alloc_state_seq));
        EXPECT_EQ(0, alloc_state_seq) << "1K Block " << block_idx << " from erased sequential file should be free.";
    }
}

TEST_F(OasisFileEraseTest, EraseWithWildcard) {
    std::vector<directory_entry_block_t> debs_wc;
    directory_entry_block_t deb_wc1, deb_wc2, deb_wc3;
    memset(&deb_wc1, 0, sizeof(deb_wc1));
    memset(&deb_wc2, 0, sizeof(deb_wc2));
    memset(&deb_wc3, 0, sizeof(deb_wc3));

    tests_common::populate_deb(&deb_wc1, "TEST1", "DAT", FILE_FORMAT_DIRECT, 0,0,0,0,0,0);
    std::vector<uint8_t> data_wc1 = tests_common::generate_patterned_data(100, 0xCC);
    ASSERT_TRUE(oasis_file_write_data(sio_stream, &disk_layout, &deb_wc1, data_wc1.data(), data_wc1.size()));
    debs_wc.push_back(deb_wc1);

    tests_common::populate_deb(&deb_wc2, "TEST2", "TXT", FILE_FORMAT_SEQUENTIAL, 0,0,0,0,0,0);
    std::vector<uint8_t> data_wc2 = tests_common::generate_patterned_data(200, 0xDD);
    ASSERT_TRUE(oasis_file_write_data(sio_stream, &disk_layout, &deb_wc2, data_wc2.data(), data_wc2.size()));
    debs_wc.push_back(deb_wc2);

    tests_common::populate_deb(&deb_wc3, "OTHER", "BAS", FILE_FORMAT_DIRECT, 0,0,0,0,0,0);
    std::vector<uint8_t> data_wc3 = tests_common::generate_patterned_data(50, 0xEE);
    ASSERT_TRUE(oasis_file_write_data(sio_stream, &disk_layout, &deb_wc3, data_wc3.data(), data_wc3.size()));
    debs_wc.push_back(deb_wc3);

    PopulateMockDirectory(debs_wc);
    WriteDirectoryAndSystemBlocksToImage();

    erase_options.pattern = "TEST*.*"; /* Erase TEST1.DAT and TEST2.TXT */
    ASSERT_TRUE(oasis_erase_files_by_pattern(sio_stream, &disk_layout, &erase_options));

    EXPECT_EQ(disk_layout.directory->directory[0].file_format, FILE_FORMAT_DELETED); /* TEST1.DAT */
    EXPECT_EQ(disk_layout.directory->directory[1].file_format, FILE_FORMAT_DELETED); /* TEST2.TXT */
    EXPECT_NE(disk_layout.directory->directory[2].file_format, FILE_FORMAT_DELETED); /* OTHER.BAS */
}

TEST_F(OasisFileEraseTest, EraseNonExistentFile) {
    erase_options.pattern = "NOSUCH.FIL";
    ASSERT_TRUE(oasis_erase_files_by_pattern(sio_stream, &disk_layout, &erase_options));
}

TEST_F(OasisFileEraseTest, EraseWithUserFilter) {
    std::vector<directory_entry_block_t> debs_uf;
    directory_entry_block_t deb_uf_u0, deb_uf_u1;
    memset(&deb_uf_u0, 0, sizeof(deb_uf_u0));
    memset(&deb_uf_u1, 0, sizeof(deb_uf_u1));

    tests_common::populate_deb(&deb_uf_u0, "USER0", "FIL", FILE_FORMAT_DIRECT, 0,0,0,0,0,0); /* Owner 0 */
    std::vector<uint8_t> data_uf_u0 = tests_common::generate_patterned_data(10, 0xF0);
    ASSERT_TRUE(oasis_file_write_data(sio_stream, &disk_layout, &deb_uf_u0, data_uf_u0.data(), data_uf_u0.size()));
    debs_uf.push_back(deb_uf_u0);

    tests_common::populate_deb(&deb_uf_u1, "USER1", "FIL", FILE_FORMAT_DIRECT, 0,0,0,0,0,1); /* Owner 1 */
    std::vector<uint8_t> data_uf_u1 = tests_common::generate_patterned_data(20, 0xF1);
    ASSERT_TRUE(oasis_file_write_data(sio_stream, &disk_layout, &deb_uf_u1, data_uf_u1.data(), data_uf_u1.size()));
    debs_uf.push_back(deb_uf_u1);

    PopulateMockDirectory(debs_uf);
    WriteDirectoryAndSystemBlocksToImage();

    erase_options.pattern = "*.FIL";
    erase_options.owner_id_filter = 1; /* Only erase for user 1 */
    ASSERT_TRUE(oasis_erase_files_by_pattern(sio_stream, &disk_layout, &erase_options));

    EXPECT_NE(disk_layout.directory->directory[0].file_format, FILE_FORMAT_DELETED); /* USER0.FIL */
    EXPECT_EQ(disk_layout.directory->directory[1].file_format, FILE_FORMAT_DELETED); /* USER1.FIL */
}
