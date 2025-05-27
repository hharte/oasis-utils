/* tests/test_oasis_file_write.cpp */
/* GTest unit tests for functions in oasis_file_write.c */

#include "gtest/gtest.h"
#include <vector>
#include <string>
#include <fstream>      /* For std::ifstream, std::ofstream */
#include <cstdio>       /* For FILE, remove */
#include <cstring>      /* For memcpy, memset, strncpy */
#include <cerrno>       /* For errno constants */
#include <algorithm>    /* For std::equal, std::sort, std::unique */
#include <cstdint>      /* For uint32_t, etc. */
#include <filesystem>   /* For temporary directory management (C++17) */
#include <limits.h>     /* For UINT16_MAX */


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

class OasisWriteFileTest : public ::testing::Test {
protected:
    std::filesystem::path temp_image_path;
    sector_io_stream_t* mock_sio_stream;
    oasis_disk_layout_t disk_layout;
    std::vector<uint8_t> alloc_map_buffer; /* To back disk_layout.alloc_map.map_data */

    /*
     * Helper to initialize the allocation map
     * @param num_1k_blocks_in_map Total 1K blocks the map can represent.
     * @param pre_allocate_system_blocks If true, block 0 will be marked as allocated.
     */
    void InitializeAllocMap(size_t num_1k_blocks_in_map, bool pre_allocate_system_blocks = true) {
        size_t map_bytes = (num_1k_blocks_in_map + 7) / 8;
        if (map_bytes == 0 && num_1k_blocks_in_map > 0) {
            map_bytes = 1; /* Ensure at least 1 byte if blocks > 0 */
        }

        alloc_map_buffer.assign(map_bytes, 0x00); /* All blocks free initially */

        disk_layout.alloc_map.map_data = alloc_map_buffer.data();
        disk_layout.alloc_map.map_size_bytes = alloc_map_buffer.size();

        if (pre_allocate_system_blocks && num_1k_blocks_in_map > 0) {
            /* Simulate block 0 (containing boot, fsblock, initial AM, dir) being allocated */
            if (set_block_state(&disk_layout.alloc_map, 0, 1) != 0) {
                GTEST_LOG_(WARNING) << "InitializeAllocMap: Failed to pre-allocate system block 0.";
            }
        }

        /* Simulate fsblock fields that might be relevant for alloc map checks */
        disk_layout.fsblock.fs_flags = 0; /* Simplified for tests */
        /* This calculation for fs_flags should align with how load_oasis_disk would set it,
           but for isolated write tests, direct control over map_size_bytes is often sufficient.
           If tests were to fully simulate load_oasis_disk behavior before write, this would be more complex.
        */
    }

    /* Helper to create a DEB for tests */
    directory_entry_block_t CreateDeb(const char* fname, const char* ftype, uint8_t format) {
        directory_entry_block_t deb;
        size_t len_to_copy; /* For storing length of strings to copy. */
        /* Initialize the structure to zeros. */
        memset(&deb, 0, sizeof(deb));

        /* Fill file_name with spaces. */
        memset(deb.file_name, ' ', FNAME_LEN);
        if (fname) { /* Check if fname is not NULL. */
            len_to_copy = strlen(fname);
            if (len_to_copy > FNAME_LEN) { /* Truncate if source is longer. */
                len_to_copy = FNAME_LEN;
            }
            /* Copy the actual content. */
            memcpy(deb.file_name, fname, len_to_copy);
        }

        /* Fill file_type with spaces. */
        memset(deb.file_type, ' ', FTYPE_LEN);
        if (ftype) { /* Check if ftype is not NULL. */
            len_to_copy = strlen(ftype);
            if (len_to_copy > FTYPE_LEN) { /* Truncate if source is longer. */
                len_to_copy = FTYPE_LEN;
            }
            /* Copy the actual content. */
            memcpy(deb.file_type, ftype, len_to_copy);
        }

        deb.file_format = format;
        /* Default other fields that might be checked or used. */
        deb.owner_id = 1;
        deb.record_count = 0; /* Will be set by caller or remain 0 for contiguous. */
        /* Default timestamp (e.g., 04/23/85 14:30). */
        deb.timestamp.raw[0] = 0x4B;
        deb.timestamp.raw[1] = 0xC3;
        deb.timestamp.raw[2] = 0x9E;
        return deb;
    }

    /* Helper to generate predictable data */
    std::vector<uint8_t> GenerateData(size_t data_size, uint8_t start_val = 0) {
        std::vector<uint8_t> data(data_size);
        for (size_t i = 0; i < data_size; ++i) {
            data[i] = (uint8_t)((start_val + i) % 256);
        }
        return data;
    }

    void SetUp() override {
        mock_sio_stream = nullptr;
        memset(&disk_layout, 0, sizeof(disk_layout));
        alloc_map_buffer.clear();

        const ::testing::TestInfo* const test_info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        std::string unique_filename = std::string("oasis_write_test_") + test_info->test_suite_name() + "_" + test_info->name() + ".img";
        temp_image_path = std::filesystem::temp_directory_path() / unique_filename;

        std::filesystem::remove(temp_image_path); /* Clean before test */

        /* Create an empty file that can be written to */
        mock_sio_stream = sector_io_open(temp_image_path.string().c_str(), "w+b");
        ASSERT_NE(nullptr, mock_sio_stream) << "Failed to create/open mock disk image: " << temp_image_path.string();

        /* Initialize a reasonably sized allocation map for most tests (e.g., 100 1K blocks) */
        /* Pre-allocate system block 0 by default */
        InitializeAllocMap(100, true);
    }

    void TearDown() override {
        if (mock_sio_stream) {
            sector_io_close(mock_sio_stream);
            mock_sio_stream = nullptr;
        }

        if (std::filesystem::exists(temp_image_path)) {
            std::error_code ec;
            std::filesystem::remove(temp_image_path, ec);
            if (ec) {
                GTEST_LOG_(WARNING) << "Could not remove temp image file " << temp_image_path << ": " << ec.message();
            }
        }
    }

    /* Helper to verify sequential file content and DEB */
    void VerifySequentialFile(const directory_entry_block_t* written_deb,
        const std::vector<uint8_t>& original_data) {
        ASSERT_NE(written_deb, nullptr);

        /* Calculate expected bytes read based on sectors in DEB. */
        /* This is what oasis_read_sequential_file will return if the buffer is sufficient. */
        ssize_t expected_bytes_read_from_sectors = 0;
        if (written_deb->record_count > 0) {
            expected_bytes_read_from_sectors = (ssize_t)written_deb->record_count * OASIS_SEQ_DATA_PER_SECTOR;
        }

        /* Size the read_back_buffer appropriately. */
        /* If expected_bytes_read_from_sectors is 0 (e.g., empty file DEB), use a minimal buffer for the read call. */
        std::vector<uint8_t> read_back_buffer(expected_bytes_read_from_sectors > 0 ? expected_bytes_read_from_sectors : 1);

        ssize_t bytes_read_seq = oasis_read_sequential_file(written_deb, mock_sio_stream, read_back_buffer.data(), read_back_buffer.size());

        /*
         * Check 1: oasis_read_sequential_file should return the total bytes from data portions
         * of sectors listed in DEB, if the buffer was large enough. Our buffer is sized for this.
         * For an empty file (record_count=0), bytes_read_seq should be 0.
         */
        ASSERT_EQ(bytes_read_seq, expected_bytes_read_from_sectors)
            << "Bytes read back by oasis_read_sequential_file (" << bytes_read_seq
            << ") does not match expected bytes based on DEB record_count * OASIS_SEQ_DATA_PER_SECTOR ("
            << expected_bytes_read_from_sectors << "). Original data size: " << original_data.size();

        /*
         * Check 2: The actual original data should match the beginning of what was read.
         */
        if (original_data.size() > 0) {
            ASSERT_LE((ssize_t)original_data.size(), bytes_read_seq)
                << "Original data size (" << original_data.size()
                << ") is larger than total bytes read from sectors (" << bytes_read_seq << ").";
            ASSERT_EQ(0, memcmp(read_back_buffer.data(), original_data.data(), original_data.size()))
                << "Sequential file content mismatch after writing and reading back (first " << original_data.size() << " bytes).";
        }
        else { /* original_data.size() == 0, implies it's an empty file test */
            ASSERT_EQ(bytes_read_seq, (ssize_t)0)
                << "Expected 0 bytes read for empty original data, but got " << bytes_read_seq;
        }

        /*
         * Check 3: Padding bytes in the last sector (if any) should be zero,
         * as oasis_file_write_data zeros the sector buffer before copying data.
         */
        if (original_data.size() > 0 && written_deb->record_count > 0) {
            /* Determine the start of padding within the total bytes read from sectors. */
            /* The padding only exists in the last sector's data portion. */
            size_t data_in_last_sector_portion = original_data.size() % OASIS_SEQ_DATA_PER_SECTOR;
            if (data_in_last_sector_portion == 0 && original_data.size() > 0) { /* If original_data perfectly filled sectors (and wasn't empty). */
                data_in_last_sector_portion = OASIS_SEQ_DATA_PER_SECTOR;
            }

            size_t padding_start_offset_in_buffer;
            if (written_deb->record_count > 1) {
                padding_start_offset_in_buffer = ((size_t)written_deb->record_count - 1) * OASIS_SEQ_DATA_PER_SECTOR + data_in_last_sector_portion;
            }
            else { /* Only one sector in the file. */
                padding_start_offset_in_buffer = data_in_last_sector_portion;
            }

            for (size_t k = padding_start_offset_in_buffer; k < (size_t)bytes_read_seq; ++k) {
                ASSERT_EQ(read_back_buffer[k], 0x00)
                    << "Padding byte at overall index " << k << " (within read_back_buffer) should be 0x00."
                    << " (Original data size: " << original_data.size()
                    << ", Total bytes read from sectors: " << bytes_read_seq
                    << ", Effective data in last sector: " << data_in_last_sector_portion
                    << ", Padding check start index in buffer: " << padding_start_offset_in_buffer << ")";
            }
        }

        /* Verify DEB fields specific to sequential files (original logic for these seems okay) */
        uint16_t expected_deb_record_count = 0;
        if (original_data.size() > 0) {
            expected_deb_record_count = (uint16_t)((original_data.size() + OASIS_SEQ_DATA_PER_SECTOR - 1) / OASIS_SEQ_DATA_PER_SECTOR);
        }
        EXPECT_EQ(written_deb->record_count, expected_deb_record_count) << "Sequential DEB record_count (actual number of sectors) is incorrect.";

        if (original_data.size() == 0) {
            EXPECT_EQ(written_deb->start_sector, (uint16_t)0) << "Empty sequential file start_sector should be 0.";
            EXPECT_EQ(written_deb->file_format_dependent2, (uint16_t)0) << "Empty sequential file FFD2 (last sector) should be 0.";
            EXPECT_EQ(written_deb->block_count, (uint16_t)0) << "Empty sequential file block_count should be 0.";
        }
        else {
            EXPECT_GT(written_deb->start_sector, (uint16_t)0) << "Non-empty sequential file start_sector should be > 0.";
            EXPECT_GT(written_deb->file_format_dependent2, (uint16_t)0) << "Non-empty sequential file FFD2 (last sector) should be > 0.";
            EXPECT_GT(written_deb->block_count, (uint16_t)0) << "Non-empty sequential file block_count should be > 0.";

            /* Further verify allocation map state for sequential files */
            std::vector<uint16_t> actual_used_1k_blocks;
            uint16_t current_lba_verify = written_deb->start_sector;
            uint32_t sectors_processed_verify = 0;
            const uint32_t MAX_SECTORS_VERIFY_LOOP = 66000;

            while (current_lba_verify != 0 && sectors_processed_verify < MAX_SECTORS_VERIFY_LOOP) {
                uint16_t block_1k_of_current_lba = current_lba_verify / (BLOCK_SIZE / SECTOR_SIZE);
                bool found = false;
                for (uint16_t b : actual_used_1k_blocks) {
                    if (b == block_1k_of_current_lba) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    actual_used_1k_blocks.push_back(block_1k_of_current_lba);
                }

                int alloc_state;
                ASSERT_EQ(0, get_block_state(&disk_layout.alloc_map, block_1k_of_current_lba, &alloc_state));
                EXPECT_EQ(1, alloc_state) << "1K Block " << block_1k_of_current_lba << " (containing sector " << current_lba_verify << ") for sequential file should be allocated.";

                uint8_t temp_sector_buf[SECTOR_SIZE];
                ASSERT_EQ(sector_io_read(mock_sio_stream, current_lba_verify, 1, temp_sector_buf), 1)
                    << "Failed to read sector " << current_lba_verify << " during verification chain walk.";
                memcpy(&current_lba_verify, temp_sector_buf + OASIS_SEQ_DATA_PER_SECTOR, sizeof(uint16_t));
                sectors_processed_verify++;
            }
            ASSERT_LT(sectors_processed_verify, MAX_SECTORS_VERIFY_LOOP) << "Sequential file chain verification loop seems broken or too long.";
            EXPECT_EQ(actual_used_1k_blocks.size(), (size_t)written_deb->block_count)
                << "Number of unique 1K blocks used by sequential file does not match DEB block_count.";
        }
    }
};
/* The rest of the test file remains unchanged. */
/* ... (existing test cases) ... */

/* Test case: Successfully write a small contiguous file */
TEST_F(OasisWriteFileTest, WriteContiguousFileDirectSuccess) {
    directory_entry_block_t deb = CreateDeb("TESTFILE", "DAT", FILE_FORMAT_DIRECT);
    std::vector<uint8_t> data_to_write = GenerateData(500, 0xAA); /* Less than 1K block */
    uint16_t expected_blocks = 1;

    bool result = oasis_file_write_data(mock_sio_stream, &disk_layout, &deb,
        data_to_write.data(), data_to_write.size());
    ASSERT_TRUE(result);

    /* Verify DEB fields */
    EXPECT_EQ(deb.block_count, expected_blocks);
    /* After pre-allocating block 0, the next allocated block should be 1 (if free).
     * Block 1 corresponds to sector 1 * (1024/256) = 4.
     */
    uint16_t expected_start_sector = (1 * (BLOCK_SIZE / SECTOR_SIZE)); /* Expecting allocation to start at 1K block 1 */
    EXPECT_EQ(deb.start_sector, expected_start_sector);

    /* Verify allocation map */
    int alloc_state;
    size_t start_1k_block_from_deb = deb.start_sector / (BLOCK_SIZE / SECTOR_SIZE);
    EXPECT_EQ(start_1k_block_from_deb, (size_t)1) << "File should have been allocated starting at 1K block 1.";

    for (uint16_t i = 0; i < expected_blocks; ++i) {
        ASSERT_EQ(0, get_block_state(&disk_layout.alloc_map, start_1k_block_from_deb + i, &alloc_state));
        EXPECT_EQ(1, alloc_state) << "Block " << (start_1k_block_from_deb + i) << " should be allocated.";
    }
    /* Ensure block 0 is still allocated (system block) */
    ASSERT_EQ(0, get_block_state(&disk_layout.alloc_map, 0, &alloc_state));
    EXPECT_EQ(1, alloc_state) << "System Block 0 should remain allocated.";


    /* Verify data written to disk image by reading it back */
    uint8_t* read_back_buffer_ptr = nullptr;
    ssize_t bytes_read_from_disk = -1;
    ASSERT_TRUE(oasis_file_read_data(mock_sio_stream, &deb, &read_back_buffer_ptr, &bytes_read_from_disk));
    ASSERT_NE(nullptr, read_back_buffer_ptr);
    ASSERT_EQ(bytes_read_from_disk, (ssize_t)(expected_blocks * BLOCK_SIZE));

    EXPECT_EQ(0, memcmp(read_back_buffer_ptr, data_to_write.data(), data_to_write.size()));

    if (data_to_write.size() % BLOCK_SIZE != 0) {
        for (size_t i = data_to_write.size(); i < (size_t)bytes_read_from_disk; ++i) {
            EXPECT_EQ(0x00, read_back_buffer_ptr[i]) << "Padding byte at offset " << i << " should be 0x00.";
        }
    }
    if (read_back_buffer_ptr) free(read_back_buffer_ptr);
}

/* Test case: Write an empty file (both contiguous and sequential should behave similarly) */
TEST_F(OasisWriteFileTest, WriteEmptyFileContiguous) {
    directory_entry_block_t deb = CreateDeb("EMPTYCNT", "DAT", FILE_FORMAT_DIRECT);
    std::vector<uint8_t> data_to_write; /* Zero size */
    size_t free_blocks_before = count_total_free_blocks(&disk_layout.alloc_map);

    bool result = oasis_file_write_data(mock_sio_stream, &disk_layout, &deb,
        data_to_write.data(), data_to_write.size());
    ASSERT_TRUE(result);

    EXPECT_EQ(deb.start_sector, 0);
    EXPECT_EQ(deb.block_count, 0);
    EXPECT_EQ(deb.record_count, 0); /* Explicitly check, writer sets to 0 for empty */
    /* No new blocks should be allocated */
    EXPECT_EQ(free_blocks_before, count_total_free_blocks(&disk_layout.alloc_map));
}

TEST_F(OasisWriteFileTest, WriteEmptyFileSequential) {
    directory_entry_block_t deb = CreateDeb("EMPTYSEQ", "DAT", FILE_FORMAT_SEQUENTIAL);
    std::vector<uint8_t> data_to_write; /* Zero size */
    size_t free_blocks_before = count_total_free_blocks(&disk_layout.alloc_map);

    bool result = oasis_file_write_data(mock_sio_stream, &disk_layout, &deb,
        data_to_write.data(), data_to_write.size());
    ASSERT_TRUE(result);

    EXPECT_EQ(deb.start_sector, 0);
    EXPECT_EQ(deb.block_count, 0);
    EXPECT_EQ(deb.record_count, 0);
    EXPECT_EQ(deb.file_format_dependent2, 0); /* Last sector LBA */
    /* No new blocks should be allocated */
    EXPECT_EQ(free_blocks_before, count_total_free_blocks(&disk_layout.alloc_map));
}


/* Test case: Write a file requiring multiple 1K blocks (Contiguous) */
TEST_F(OasisWriteFileTest, WriteMultiBlockContiguousFile) {
    directory_entry_block_t deb = CreateDeb("BIGFILE", "DAT", FILE_FORMAT_ABSOLUTE);
    std::vector<uint8_t> data_to_write = GenerateData(BLOCK_SIZE * 2 + 100, 0xBB); /* > 2 blocks */
    uint16_t expected_blocks = 3;

    bool result = oasis_file_write_data(mock_sio_stream, &disk_layout, &deb,
        data_to_write.data(), data_to_write.size());
    ASSERT_TRUE(result);

    EXPECT_EQ(deb.block_count, expected_blocks);
    /* Expect allocation to start at 1K block 1 (sector 4) because block 0 is pre-allocated */
    uint16_t expected_start_sector = (1 * (BLOCK_SIZE / SECTOR_SIZE));
    EXPECT_EQ(deb.start_sector, expected_start_sector);


    int alloc_state;
    size_t start_1k_block_from_deb = deb.start_sector / (BLOCK_SIZE / SECTOR_SIZE);
    EXPECT_EQ(start_1k_block_from_deb, (size_t)1);

    for (uint16_t i = 0; i < expected_blocks; ++i) {
        ASSERT_EQ(0, get_block_state(&disk_layout.alloc_map, start_1k_block_from_deb + i, &alloc_state));
        EXPECT_EQ(1, alloc_state) << "Block " << (start_1k_block_from_deb + i) << " should be allocated.";
    }
    /* Ensure block 0 is still allocated (system block) */
    ASSERT_EQ(0, get_block_state(&disk_layout.alloc_map, 0, &alloc_state));
    EXPECT_EQ(1, alloc_state) << "System Block 0 should remain allocated.";


    uint8_t* read_back_buffer_ptr = nullptr;
    ssize_t bytes_read_from_disk = -1;
    ASSERT_TRUE(oasis_file_read_data(mock_sio_stream, &deb, &read_back_buffer_ptr, &bytes_read_from_disk));
    ASSERT_NE(nullptr, read_back_buffer_ptr);
    ASSERT_EQ(bytes_read_from_disk, (ssize_t)(expected_blocks * BLOCK_SIZE));
    EXPECT_EQ(0, memcmp(read_back_buffer_ptr, data_to_write.data(), data_to_write.size()));
    if (read_back_buffer_ptr) free(read_back_buffer_ptr);
}

/* tests/test_oasis_file_write.cpp */
TEST_F(OasisWriteFileTest, ErrorNoContiguousSpace) {
    InitializeAllocMap(10, true); /* Actual capacity 16 blocks (0-15). Block 0 is 'A'. Blocks 1-15 are 'F'. */
    directory_entry_block_t deb = CreateDeb("NOSPACE", "DAT", FILE_FORMAT_DIRECT);

    /* Initial Map: [A F F F F F F F F F F F F F F F] */

    /* Allocate blocks 1, 2, 3. allocate_blocks returns the start block (1). */
    ASSERT_EQ(1, allocate_blocks(&disk_layout.alloc_map, 3));
    /* Map now:     [A A A A F F F F F F F F F F F F] (Blocks 0-3 'A'. Blocks 4-15 'F'. Largest free is 12 blocks from 4-15) */

    /* To make the largest free chunk exactly 6 blocks, we need to consume 12 - 6 = 6 blocks from the current free chunk.
     * We allocate 6 more blocks. These will be blocks 4, 5, 6, 7, 8, 9.
     * allocate_blocks will return the start block (4).
     */
    ASSERT_EQ(4, allocate_blocks(&disk_layout.alloc_map, 6));
    /* Map now:     [A A A A A A A A A A F F F F F F] (Blocks 0-9 'A'. Blocks 10-15 'F'. Largest free is 6 blocks from 10-15) */

    /* Verify the map state: count_total_free_blocks should be 6. */
    size_t free_blocks_before_test = count_total_free_blocks(&disk_layout.alloc_map);
    ASSERT_EQ((size_t)6, free_blocks_before_test) << "Setup error: Expected 6 free blocks before the test allocation.";

    /* Verify find_largest_free_contiguous_blocks also reports 6 */
    ASSERT_EQ((size_t)6, find_largest_free_contiguous_blocks(&disk_layout.alloc_map)) << "Setup error: Expected largest free chunk of 6 blocks.";

    /* Now, try to allocate 7 blocks. The largest free chunk is 6. This should fail. */
    std::vector<uint8_t> data_to_write = GenerateData(BLOCK_SIZE * 7);

    uint16_t original_block_count = deb.block_count;
    uint16_t original_start_sector = deb.start_sector;

    bool result = oasis_file_write_data(mock_sio_stream, &disk_layout, &deb,
        data_to_write.data(), data_to_write.size());

    /* The allocation of 7 blocks should fail, so oasis_file_write_data should return false. */
    ASSERT_FALSE(result) << "Allocation of 7 blocks should fail when largest free is 6.";

    /* Verify DEB is not modified since the write failed */
    EXPECT_EQ(deb.block_count, original_block_count);
    EXPECT_EQ(deb.start_sector, original_start_sector);

    /* Verify allocation map state is unchanged from *before the failed write attempt* */
    EXPECT_EQ(count_total_free_blocks(&disk_layout.alloc_map), free_blocks_before_test);

    int state;
    /* Check some specific blocks to ensure they weren't inadvertently changed by the failed allocation */
    ASSERT_EQ(0, get_block_state(&disk_layout.alloc_map, 9, &state)); EXPECT_EQ(state, 1);  /* Should still be allocated from setup */
    ASSERT_EQ(0, get_block_state(&disk_layout.alloc_map, 10, &state)); EXPECT_EQ(state, 0); /* Should still be free */
    ASSERT_EQ(0, get_block_state(&disk_layout.alloc_map, 15, &state)); EXPECT_EQ(state, 0); /* Should still be free */
}

/* Test case: Invalid arguments to oasis_file_write_data */
TEST_F(OasisWriteFileTest, InvalidArguments) {
    directory_entry_block_t deb = CreateDeb("ANYFILE", "DAT", FILE_FORMAT_DIRECT);
    std::vector<uint8_t> data_to_write = GenerateData(100);

    EXPECT_FALSE(oasis_file_write_data(nullptr, &disk_layout, &deb, data_to_write.data(), data_to_write.size()));
    EXPECT_FALSE(oasis_file_write_data(mock_sio_stream, nullptr, &deb, data_to_write.data(), data_to_write.size()));
    EXPECT_FALSE(oasis_file_write_data(mock_sio_stream, &disk_layout, nullptr, data_to_write.data(), data_to_write.size()));

    /* Test: data_size > 0, but data_buffer is NULL */
    EXPECT_FALSE(oasis_file_write_data(mock_sio_stream, &disk_layout, &deb, nullptr, data_to_write.size()));

    /* Test: data_size is 0, and data_buffer is NULL (should be true for empty file) */
    EXPECT_TRUE(oasis_file_write_data(mock_sio_stream, &disk_layout, &deb, nullptr, 0));
}

/* Test case: Attempt to write a file that would exceed DEB's block_count (uint16_t) capacity */
TEST_F(OasisWriteFileTest, ErrorExceedsBlockCountCapacityContiguous) {
    InitializeAllocMap(70000, true); /* Large enough map for many blocks, block 0 pre-allocated */
    directory_entry_block_t deb = CreateDeb("HUGEFIL", "DAT", FILE_FORMAT_DIRECT);

    size_t data_size_too_large = (size_t)(UINT16_MAX + 10) * BLOCK_SIZE; /* More than max uint16_t blocks */
    /* We don't actually need to create this huge vector, just pass the size.
       The function should fail before trying to read from data_buffer if data_size is the problem.
       However, to avoid issues with data_buffer being NULL when data_size > 0, pass a dummy buffer.
    */
    std::vector<uint8_t> dummy_data(1);

    bool result = oasis_file_write_data(mock_sio_stream, &disk_layout, &deb,
        dummy_data.data(), data_size_too_large);
    ASSERT_FALSE(result) << "Writing a file requiring more than UINT16_MAX blocks should fail.";
}

/* Test case: Writing a file when the allocation map itself is not initialized (map_data is NULL) */
TEST_F(OasisWriteFileTest, ErrorNullAllocMapData) {
    directory_entry_block_t deb = CreateDeb("TESTFILE", "DAT", FILE_FORMAT_DIRECT);
    std::vector<uint8_t> data_to_write = GenerateData(100);

    /* Ensure the map_data pointer in disk_layout is NULL */
    disk_layout.alloc_map.map_data = nullptr;
    disk_layout.alloc_map.map_size_bytes = 0; /* Also set size to 0 for consistency */

    bool result = oasis_file_write_data(mock_sio_stream, &disk_layout, &deb,
        data_to_write.data(), data_to_write.size());
    ASSERT_FALSE(result);
}


/* --- Tests for Sequential File Writing --- */

/* Test: Write a very small sequential file (less than OASIS_SEQ_DATA_PER_SECTOR) */
TEST_F(OasisWriteFileTest, WriteSequentialFileTiny) {
    directory_entry_block_t deb = CreateDeb("TINYSEQ", "DAT", FILE_FORMAT_SEQUENTIAL);
    std::vector<uint8_t> data_to_write = GenerateData(10, 0xC0); /* 10 bytes */

    bool result = oasis_file_write_data(mock_sio_stream, &disk_layout, &deb,
        data_to_write.data(), data_to_write.size());
    ASSERT_TRUE(result);
    VerifySequentialFile(&deb, data_to_write);
}

/* Test: Write a sequential file that exactly fills one sector's data portion */
TEST_F(OasisWriteFileTest, WriteSequentialFileOneSectorFull) {
    directory_entry_block_t deb = CreateDeb("ONESECT", "DAT", FILE_FORMAT_SEQUENTIAL);
    std::vector<uint8_t> data_to_write = GenerateData(OASIS_SEQ_DATA_PER_SECTOR, 0xD0);

    bool result = oasis_file_write_data(mock_sio_stream, &disk_layout, &deb,
        data_to_write.data(), data_to_write.size());
    ASSERT_TRUE(result);
    VerifySequentialFile(&deb, data_to_write);
}

/* Test: Write a sequential file that uses multiple sectors but fits in one 1K block */
TEST_F(OasisWriteFileTest, WriteSequentialFileMultiSectorOneBlock) {
    directory_entry_block_t deb = CreateDeb("MULTIS1K", "DAT", FILE_FORMAT_SEQUENTIAL);
    /* Data for 2 full sectors + part of a 3rd, all within one 1K block (4 sectors total capacity) */
    std::vector<uint8_t> data_to_write = GenerateData(OASIS_SEQ_DATA_PER_SECTOR * 2 + 50, 0xE0);

    bool result = oasis_file_write_data(mock_sio_stream, &disk_layout, &deb,
        data_to_write.data(), data_to_write.size());
    ASSERT_TRUE(result);
    VerifySequentialFile(&deb, data_to_write);
}

/* Test: Write a sequential file that spans multiple 1K blocks */
TEST_F(OasisWriteFileTest, WriteSequentialFileMultiBlock) {
    directory_entry_block_t deb = CreateDeb("MULTIBLK", "DAT", FILE_FORMAT_SEQUENTIAL);
    /* Data for 5 full sectors - will require two 1K blocks.
       (1K block = 4 * 256B sectors)
    */
    std::vector<uint8_t> data_to_write = GenerateData(OASIS_SEQ_DATA_PER_SECTOR * 5, 0xF0);

    bool result = oasis_file_write_data(mock_sio_stream, &disk_layout, &deb,
        data_to_write.data(), data_to_write.size());
    ASSERT_TRUE(result);
    VerifySequentialFile(&deb, data_to_write);
}

/* Test: Sequential write when disk runs out of space during allocation */
TEST_F(OasisWriteFileTest, WriteSequentialFileNoSpaceMidWrite) {
    /*
     * Goal: Create a scenario where only 3 free 1K blocks are available.
     * Then, attempt to write a sequential file that requires 4 blocks.
     * This write operation should fail.
     */

     /* * InitializeAllocMap(4, true) results in a 1-byte map (8 blocks capacity: 0-7).
      * Block 0 is marked 'Allocated' (system).
      * Blocks 1-7 are initially 'Free'. Total 7 free blocks.
      */
    InitializeAllocMap(4, true);
    directory_entry_block_t deb = CreateDeb("NOSPACE", "SEQ", FILE_FORMAT_SEQUENTIAL);

    /* * We want 3 free blocks remaining for the test. Currently, 7 blocks (1-7) are free.
     * To achieve 3 free blocks, we need to allocate 7 - 3 = 4 additional blocks.
     * allocate_blocks will likely allocate blocks 1, 2, 3, and 4.
     * The start block returned by allocate_blocks(..., 4) when blocks 1-7 are free should be 1.
     */
    ASSERT_EQ(1, allocate_blocks(&disk_layout.alloc_map, 4))
        << "Failed to allocate initial blocks to set up the desired free block count.";
    /* * Map state after this:
     * Block 0: 'A' (system)
     * Blocks 1, 2, 3, 4: 'A' (allocated by the line above)
     * Blocks 5, 6, 7: 'F' (these are the 3 remaining free blocks)
     */

     /* * The data to write requires 15 sectors.
      * Each 1K block holds (1024 / 256) = 4 sectors.
      * So, 15 sectors require ceil(15 / 4) = 4 blocks.
      */
    std::vector<uint8_t> data_to_write = GenerateData(15 * OASIS_SEQ_DATA_PER_SECTOR);

    /* Check that our setup is correct: there should be 3 free blocks now. */
    size_t free_blocks_before_attempt = count_total_free_blocks(&disk_layout.alloc_map);
    ASSERT_EQ((size_t)3, free_blocks_before_attempt)
        << "Test setup error: Expected 3 free blocks before attempting the main write operation.";

    /* Attempt to write the file (which needs 4 blocks) when only 3 are free. */
    bool result = oasis_file_write_data(mock_sio_stream, &disk_layout, &deb,
        data_to_write.data(), data_to_write.size());

    /* The write should fail because there isn't enough space. */
    ASSERT_FALSE(result) << "Sequential write should fail if disk runs out of space mid-operation. "
        << "Free blocks available: " << free_blocks_before_attempt
        << ", Blocks needed: " << ((data_to_write.size() + BLOCK_SIZE - 1) / BLOCK_SIZE);

    /* * Check if allocated blocks were rolled back by oasis_file_write_data.
     * The number of free blocks should be the same as before the failed write attempt.
     */
    EXPECT_EQ(count_total_free_blocks(&disk_layout.alloc_map), free_blocks_before_attempt)
        << "Allocation map should be restored to its pre-failure state.";
}

TEST_F(OasisWriteFileTest, WriteUnsupportedFileFormat) {
    directory_entry_block_t deb = CreateDeb("BADFORM", "DAT", 0x03 /* Invalid combined type */);
    std::vector<uint8_t> data_to_write = GenerateData(100);
    bool result = oasis_file_write_data(mock_sio_stream, &disk_layout, &deb,
        data_to_write.data(), data_to_write.size());
    ASSERT_FALSE(result);
}
