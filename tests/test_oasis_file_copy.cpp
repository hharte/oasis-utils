/* tests/test_oasis_file_copy.cpp */
#include "gtest/gtest.h"
#include <vector>
#include <string>
#include <fstream>
#include <filesystem>
#include <cstring>
#include <cstdio> /* For remove() */
#include <sstream> /* For std::stringstream */
#include <iomanip> /* For std::hex, std::setw, std::setfill */
#include <iostream> /* For GTEST_LOG_ */


/* Common Test Utilities */
#include "test_oasis_common.h"

/* DUT headers */
extern "C" {
#include "oasis.h"
#include "oasis_file_copy.h"
#include "oasis_file_erase.h" /* Needed if copy overwrites via erase */
}

/* Platform-specific line endings for test verification */
#ifdef _WIN32
#define HOST_TEST_LINE_ENDING "\r\n"
#else
#define HOST_TEST_LINE_ENDING "\n"
#endif
#define OASIS_TEST_LINE_ENDING "\r"
#define OASIS_TEST_SUB_CHAR (0x1A)


class OasisFileCopyTest : public ::testing::Test {
protected:
    std::filesystem::path temp_image_path_fc;
    std::filesystem::path temp_host_files_dir_fc;
    sector_io_stream_t* sio_stream_fc;
    oasis_disk_layout_t disk_layout_fc;
    std::vector<uint8_t> alloc_map_buffer_fc;
    cli_options_t copy_options_fc;

    void InitializeAllocMapFc(size_t num_1k_blocks_in_map, bool pre_allocate_system_blocks = true) {
        size_t map_bytes = (num_1k_blocks_in_map + 7) / 8;
        if (map_bytes == 0 && num_1k_blocks_in_map > 0) map_bytes = 1;
        alloc_map_buffer_fc.assign(map_bytes, 0x00);
        disk_layout_fc.alloc_map.map_data = alloc_map_buffer_fc.data();
        disk_layout_fc.alloc_map.map_size_bytes = alloc_map_buffer_fc.size();
        if (pre_allocate_system_blocks && num_1k_blocks_in_map > 0) {
            set_block_state(&disk_layout_fc.alloc_map, 0, 1);
        }
        disk_layout_fc.fsblock.free_blocks = (uint16_t)count_total_free_blocks(&disk_layout_fc.alloc_map);
    }

    void PopulateMockDirectoryFc(const std::vector<directory_entry_block_t>& debs) {
        if (disk_layout_fc.directory) { free(disk_layout_fc.directory); disk_layout_fc.directory = NULL; }

        size_t num_debs_to_populate = debs.size();
        /* Determine the directory size based on fsblock.dir_sectors_max if debs is empty, */
        /* or based on debs.size() if debs is not empty, ensuring it doesn't exceed dir_sectors_max capacity. */
        size_t actual_dir_capacity_debs = (size_t)disk_layout_fc.fsblock.dir_sectors_max * (SECTOR_SIZE / sizeof(directory_entry_block_t));

        if (debs.empty()) {
            /* If debs is empty, the directory data buffer should reflect the capacity defined by dir_sectors_max, */
            /* filled with FILE_FORMAT_EMPTY entries. */
            size_t dir_data_size_for_capacity = actual_dir_capacity_debs * sizeof(directory_entry_block_t);
            disk_layout_fc.directory = (oasis_directory_t*)malloc(sizeof(oasis_directory_t) + dir_data_size_for_capacity);
            ASSERT_NE(nullptr, disk_layout_fc.directory);
            disk_layout_fc.directory->directory_size_bytes = dir_data_size_for_capacity;
            /* Initialize all these potential DEB slots to EMPTY */
            if (dir_data_size_for_capacity > 0) {
                for (size_t i = 0; i < actual_dir_capacity_debs; ++i) {
                    memset(&disk_layout_fc.directory->directory[i], 0, sizeof(directory_entry_block_t));
                    disk_layout_fc.directory->directory[i].file_format = FILE_FORMAT_EMPTY;
                }
            }
        } else {
            /* If debs is not empty, populate with provided debs, but ensure it doesn't exceed actual_dir_capacity_debs */
            if (num_debs_to_populate > actual_dir_capacity_debs) {
                /* This case should ideally be an error or warning in test setup, */
                /* as we're trying to populate more DEBs than the directory can hold. */
                num_debs_to_populate = actual_dir_capacity_debs; /* Truncate for safety */
                GTEST_LOG_(WARNING) << "PopulateMockDirectoryFc: Number of provided DEBs (" << debs.size()
                                   << ") exceeds directory capacity (" << actual_dir_capacity_debs
                                   << ") based on dir_sectors_max. Truncating.";
            }
            size_t dir_data_size_populated = num_debs_to_populate * sizeof(directory_entry_block_t);
            /* The allocated buffer should still be for the full capacity, partly filled */
            size_t dir_alloc_size_for_capacity = actual_dir_capacity_debs * sizeof(directory_entry_block_t);

            disk_layout_fc.directory = (oasis_directory_t*)malloc(sizeof(oasis_directory_t) + dir_alloc_size_for_capacity);
            ASSERT_NE(nullptr, disk_layout_fc.directory);
            disk_layout_fc.directory->directory_size_bytes = dir_alloc_size_for_capacity; /* Reflects potential capacity */

            /* Initialize all to EMPTY first */
            if (dir_alloc_size_for_capacity > 0) {
                for (size_t i = 0; i < actual_dir_capacity_debs; ++i) {
                    memset(&disk_layout_fc.directory->directory[i], 0, sizeof(directory_entry_block_t));
                    disk_layout_fc.directory->directory[i].file_format = FILE_FORMAT_EMPTY;
                }
            }
            /* Then copy the provided DEBs */
            if (dir_data_size_populated > 0) {
                memcpy(disk_layout_fc.directory->directory, debs.data(), dir_data_size_populated);
            }
            /* fsblock.dir_sectors_max is already set in SetUp and should remain unchanged. */
        }
    }

    void WriteDirectoryAndSystemBlocksToImageFc() {
        if (!sio_stream_fc) return;
        uint8_t s1_buf[SECTOR_SIZE];
        memcpy(s1_buf, &disk_layout_fc.fsblock, sizeof(filesystem_block_t));
        if (disk_layout_fc.alloc_map.map_data && disk_layout_fc.alloc_map.map_size_bytes > 0) {
             size_t map_s1 = SECTOR_SIZE - sizeof(filesystem_block_t);
             if(map_s1 > disk_layout_fc.alloc_map.map_size_bytes) map_s1 = disk_layout_fc.alloc_map.map_size_bytes;
            memcpy(s1_buf + sizeof(filesystem_block_t), disk_layout_fc.alloc_map.map_data, map_s1);
        }
        ASSERT_EQ(sector_io_write(sio_stream_fc, 1, 1, s1_buf), 1);
        uint32_t add_am = disk_layout_fc.fsblock.fs_flags & ADDITIONAL_AM_SECTORS_MASK;
        if (add_am > 0 && disk_layout_fc.alloc_map.map_data) {
            size_t map_s1 = SECTOR_SIZE - sizeof(filesystem_block_t);
            if (disk_layout_fc.alloc_map.map_size_bytes > map_s1) {
                 ASSERT_EQ(sector_io_write(sio_stream_fc, 2, add_am, disk_layout_fc.alloc_map.map_data + map_s1), (ssize_t)add_am);
            }
        }
        if (disk_layout_fc.directory && disk_layout_fc.fsblock.dir_sectors_max > 0) {
            uint32_t dir_start = 1 + 1 + add_am;
            ASSERT_EQ(sector_io_write(sio_stream_fc, dir_start, disk_layout_fc.fsblock.dir_sectors_max, (uint8_t*)disk_layout_fc.directory->directory), disk_layout_fc.fsblock.dir_sectors_max);
        }
    }

    std::string CreateHostFile(const std::string& name, const std::string& content) {
        std::filesystem::path file_path = temp_host_files_dir_fc / name;
        std::ofstream outfile(file_path, std::ios::binary);
        outfile.write(content.data(), content.length());
        outfile.close();
        return file_path.string();
    }
     std::string CreateHostFileBinary(const std::string& name, const std::vector<uint8_t>& content) {
        std::filesystem::path file_path = temp_host_files_dir_fc / name;
        std::ofstream outfile(file_path, std::ios::binary);
        outfile.write(reinterpret_cast<const char*>(content.data()), content.size());
        outfile.close();
        return file_path.string();
    }


    void SetUp() override {
        sio_stream_fc = nullptr;
        memset(&disk_layout_fc, 0, sizeof(disk_layout_fc));
        alloc_map_buffer_fc.clear();
        memset(&copy_options_fc, 0, sizeof(copy_options_fc));

        const ::testing::TestInfo* const test_info = ::testing::UnitTest::GetInstance()->current_test_info();
        std::string base_name = std::string("oasis_copy_") + test_info->test_suite_name() + "_" + test_info->name();
        temp_image_path_fc = std::filesystem::temp_directory_path() / (base_name + ".img");
        temp_host_files_dir_fc = std::filesystem::temp_directory_path() / (base_name + "_host_files");

        std::filesystem::remove(temp_image_path_fc);
        std::filesystem::remove_all(temp_host_files_dir_fc);
        std::filesystem::create_directories(temp_host_files_dir_fc);


        sio_stream_fc = sector_io_open(temp_image_path_fc.string().c_str(), "w+b");
        ASSERT_NE(nullptr, sio_stream_fc);

        InitializeAllocMapFc(100, true); /* 100 1K blocks */
        disk_layout_fc.fsblock.dir_sectors_max = 2; /* 16 DEBs */
        std::vector<directory_entry_block_t> empty_debs_fc;
        PopulateMockDirectoryFc(empty_debs_fc);
        WriteDirectoryAndSystemBlocksToImageFc();

        copy_options_fc.owner_id_filter = 0; /* Default */
        copy_options_fc.ascii_conversion = false;
    }

    void TearDown() override {
        if (sio_stream_fc) sector_io_close(sio_stream_fc);
        if (disk_layout_fc.directory) free(disk_layout_fc.directory);
        std::filesystem::remove(temp_image_path_fc);
        std::filesystem::remove_all(temp_host_files_dir_fc);
    }

    const directory_entry_block_t* FindDebInLayoutFc(const char* fname, const char* ftype, uint8_t owner_id) {
        char padded_fname_search[FNAME_LEN]; /* Not null-terminated for comparison */
        char padded_ftype_search[FTYPE_LEN];
        memset(padded_fname_search, ' ', FNAME_LEN);
        memset(padded_ftype_search, ' ', FTYPE_LEN);

        if (fname) {
            size_t len_to_copy_fname = strlen(fname);
            if (len_to_copy_fname > FNAME_LEN) len_to_copy_fname = FNAME_LEN;
            memcpy(padded_fname_search, fname, len_to_copy_fname);
        }
        if (ftype) {
            size_t len_to_copy_ftype = strlen(ftype);
            if (len_to_copy_ftype > FTYPE_LEN) len_to_copy_ftype = FTYPE_LEN;
            memcpy(padded_ftype_search, ftype, len_to_copy_ftype);
        }

        if (!disk_layout_fc.directory) return nullptr;
        size_t num_entries = disk_layout_fc.directory->directory_size_bytes / sizeof(directory_entry_block_t);
        for (size_t i = 0; i < num_entries; ++i) {
            const directory_entry_block_t* current_deb = &disk_layout_fc.directory->directory[i];
            if (oasis_deb_is_valid(current_deb) &&
                (owner_id == (uint8_t)OWNER_ID_WILDCARD || current_deb->owner_id == owner_id) &&
                memcmp(current_deb->file_name, padded_fname_search, FNAME_LEN) == 0 &&
                memcmp(current_deb->file_type, padded_ftype_search, FTYPE_LEN) == 0) {
                return current_deb;
            }
        }
        return nullptr;
    }
};

/* Helper function to convert a vector of bytes to a hex string for debugging */
/* This can be a static member of your test fixture or a free function */
static std::string VectorToHexString(const std::vector<uint8_t>& vec) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (size_t i = 0; i < vec.size(); ++i) {
        ss << std::setw(2) << static_cast<unsigned>(vec[i]);
        if (i < vec.size() - 1) {
            ss << " "; /* Add space between hex values */
        }
    }
    return ss.str();
}

/* Helper function to convert raw bytes to a hex string for debugging */
static std::string RawBytesToHexString(const uint8_t* data, size_t len) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    if (data == nullptr && len > 0) {
        ss << "[NULL data pointer with len > 0]";
        return ss.str();
    }
    if (data == nullptr && len == 0) {
        ss << "[NULL data pointer with len == 0]";
        return ss.str();
    }
     if (len == 0) {
        ss << "[Empty buffer (len == 0)]";
        return ss.str();
    }
    for (size_t i = 0; i < len; ++i) {
        ss << std::setw(2) << static_cast<unsigned>(data[i]);
        if (i < len - 1) {
            ss << " "; /* Add space between hex values */
        }
    }
    return ss.str();
}


TEST_F(OasisFileCopyTest, CopyNewBinaryFile) {
    std::string host_content_str = "This is a binary test file.";
    std::string host_file = CreateHostFile("binary.dat", host_content_str);
    copy_options_fc.ascii_conversion = false;

    ASSERT_TRUE(oasis_copy_host_file_to_disk(sio_stream_fc, &disk_layout_fc, host_file.c_str(), NULL, &copy_options_fc));

    const directory_entry_block_t* copied_deb = FindDebInLayoutFc("BINARY", "DAT", 0);
    ASSERT_NE(nullptr, copied_deb);
    EXPECT_EQ(copied_deb->file_format & FILE_FORMAT_MASK, FILE_FORMAT_SEQUENTIAL); // Default if not specified
    EXPECT_EQ(copied_deb->block_count, 1);

    uint8_t* read_back_data = NULL;
    ssize_t bytes_read_oasis;
    ASSERT_TRUE(oasis_file_read_data(sio_stream_fc, copied_deb, &read_back_data, &bytes_read_oasis));
    ASSERT_NE(nullptr, read_back_data);
    ASSERT_EQ((size_t)bytes_read_oasis, OASIS_SEQ_DATA_PER_SECTOR);
    EXPECT_EQ(0, memcmp(read_back_data, host_content_str.data(), host_content_str.length()));
    if(read_back_data) free(read_back_data);
}

TEST_F(OasisFileCopyTest, CopyNewAsciiFile) {
    std::string host_content_str = "Line 1" HOST_TEST_LINE_ENDING "Line 2" HOST_TEST_LINE_ENDING "Line 3";
    std::string host_file = CreateHostFile("ascii.txt", host_content_str);
    copy_options_fc.ascii_conversion = true;

    GTEST_LOG_(INFO) << "Host content to write: \n---\n" << host_content_str << "\n---";
    GTEST_LOG_(INFO) << "Host file created at: " << host_file;

    /* This will derive name ASCIINEW.TXT and type _S from the override */
    ASSERT_TRUE(oasis_copy_host_file_to_disk(sio_stream_fc, &disk_layout_fc, host_file.c_str(), "ASCIINEW.TXT_S", &copy_options_fc));

    const directory_entry_block_t* copied_deb_ascii = FindDebInLayoutFc("ASCIINEW", "TXT", 0);
    ASSERT_NE(nullptr, copied_deb_ascii) << "Could not find DEB for ASCIINEW.TXT";

    /* Log DEB details */
    GTEST_LOG_(INFO) << "DEB for ASCIINEW.TXT found.";
    GTEST_LOG_(INFO) << "  DEB File Format (raw): 0x" << std::hex << static_cast<int>(copied_deb_ascii->file_format);
    GTEST_LOG_(INFO) << "  DEB Format Type (masked): 0x" << std::hex << static_cast<int>(copied_deb_ascii->file_format & FILE_FORMAT_MASK);
    GTEST_LOG_(INFO) << "  DEB Attributes (masked): 0x" << std::hex << static_cast<int>(copied_deb_ascii->file_format & FILE_ATTRIBUTE_MASK);
    GTEST_LOG_(INFO) << "  DEB Block Count: " << std::dec << copied_deb_ascii->block_count;
    GTEST_LOG_(INFO) << "  DEB Record Count: " << std::dec << copied_deb_ascii->record_count;
    GTEST_LOG_(INFO) << "  DEB Start Sector: " << std::dec << copied_deb_ascii->start_sector;
    GTEST_LOG_(INFO) << "  DEB FFD1 (Seq Rec Len): " << std::dec << copied_deb_ascii->file_format_dependent1;
    GTEST_LOG_(INFO) << "  DEB FFD2 (Seq Last Sector): " << std::dec << copied_deb_ascii->file_format_dependent2;


    EXPECT_EQ(copied_deb_ascii->file_format & FILE_FORMAT_MASK, FILE_FORMAT_SEQUENTIAL);
    EXPECT_EQ(copied_deb_ascii->block_count, 1) << "This small ASCII file should fit in 1 block (1024 bytes total, 4 sectors)";

    /* This is the content ascii_host_to_oasis would produce (CRs) */
    std::string expected_oasis_payload_before_sub = "Line 1" OASIS_TEST_LINE_ENDING "Line 2" OASIS_TEST_LINE_ENDING "Line 3";
    /* The copy function should append SUB for ASCII sequential */
    std::string expected_data_on_disk_str = expected_oasis_payload_before_sub;
    expected_data_on_disk_str += (char)OASIS_TEST_SUB_CHAR;

    std::vector<uint8_t> expected_data_on_disk(expected_data_on_disk_str.begin(), expected_data_on_disk_str.end());

    GTEST_LOG_(INFO) << "Expected OASIS payload (before SUB): \n---\n" << expected_oasis_payload_before_sub << "\n---";
    GTEST_LOG_(INFO) << "Expected data on disk (with SUB, size " << expected_data_on_disk.size() << " bytes): " << VectorToHexString(expected_data_on_disk);

    uint8_t* read_back_data_ascii = NULL;
    ssize_t bytes_read_oasis_ascii;
    ASSERT_TRUE(oasis_file_read_data(sio_stream_fc, copied_deb_ascii, &read_back_data_ascii, &bytes_read_oasis_ascii)) << "oasis_file_read_data failed";
    ASSERT_NE(nullptr, read_back_data_ascii) << "read_back_data_ascii is NULL after read";

    GTEST_LOG_(INFO) << "Bytes read by oasis_file_read_data: " << bytes_read_oasis_ascii;
    GTEST_LOG_(INFO) << "Read back data (raw, " << bytes_read_oasis_ascii << " bytes): " << RawBytesToHexString(read_back_data_ascii, bytes_read_oasis_ascii);

    /* For a single-sector sequential file, oasis_file_read_data reads OASIS_SEQ_DATA_PER_SECTOR bytes */
    /* This test assumes the file fits in one Oasis sector's data portion (254 bytes) */
    /* If the file is larger and spans multiple sectors, bytes_read_oasis_ascii will be larger. */
    /* The current expectation is that the test file is small. */
    /* If the test data + SUB is larger than OASIS_SEQ_DATA_PER_SECTOR, this ASSERT will fail */
    /* and logic for multi-sector read needs to be considered for verification. */
    /* However, current `expected_data_on_disk` should be small. */
    /* Line1\rLine2\rLine3 + SUB = 6+1+6+1+6+1 = 21 bytes. */
    /* This fits well within OASIS_SEQ_DATA_PER_SECTOR (254). */
    /* oasis_file_read_data for sequential files returns content up to OASIS_SEQ_DATA_PER_SECTOR * number of sectors in chain. */
    /* If it's one sector, it will read 254 bytes. */
    ASSERT_EQ((size_t)bytes_read_oasis_ascii, (size_t)OASIS_SEQ_DATA_PER_SECTOR) << "Should read one full data portion of a sector for a small sequential file.";

    /* Compare the actual written data part (payload + SUB) */
    ASSERT_GE((size_t)bytes_read_oasis_ascii, expected_data_on_disk.size());

    /* Add detailed logging before memcmp */
    GTEST_LOG_(INFO) << "Comparing expected_data_on_disk (size " << expected_data_on_disk.size() << "): " << VectorToHexString(expected_data_on_disk);
    GTEST_LOG_(INFO) << "With read_back_data_ascii (first " << expected_data_on_disk.size() << " of " << bytes_read_oasis_ascii << " bytes): " << RawBytesToHexString(read_back_data_ascii, expected_data_on_disk.size());

    EXPECT_EQ(0, memcmp(read_back_data_ascii, expected_data_on_disk.data(), expected_data_on_disk.size())) << "Memory comparison of expected data and read back data failed.";

    /* Verify padding after SUB is zeros, up to OASIS_SEQ_DATA_PER_SECTOR */
    GTEST_LOG_(INFO) << "Verifying padding bytes after SUB character.";
    for (size_t i = expected_data_on_disk.size(); i < (size_t)OASIS_SEQ_DATA_PER_SECTOR; ++i) {
        if (i < (size_t)bytes_read_oasis_ascii) { /* Check bounds */
            EXPECT_EQ(read_back_data_ascii[i], 0x00) << "Padding byte at index " << i << " (relative to start of read_back_data_ascii) should be 0x00, but found 0x" << std::hex << static_cast<int>(read_back_data_ascii[i]);
        } else {
            /* This case should not be reached if bytes_read_oasis_ascii is OASIS_SEQ_DATA_PER_SECTOR */
            GTEST_LOG_(INFO) << "Skipping padding check at index " << i << " as it's beyond bytes_read_oasis_ascii (" << bytes_read_oasis_ascii << ")";
        }
    }

    if(read_back_data_ascii) free(read_back_data_ascii);
}


TEST_F(OasisFileCopyTest, OverwriteExistingFile) {
    std::string host_content_orig = "Original file content.";
    std::string host_file_orig = CreateHostFile("myfile.dat", host_content_orig);
    ASSERT_TRUE(oasis_copy_host_file_to_disk(sio_stream_fc, &disk_layout_fc, host_file_orig.c_str(), "MYFILE.DAT_D_32", &copy_options_fc));
    const directory_entry_block_t* deb_before_overwrite = FindDebInLayoutFc("MYFILE", "DAT", 0);
    ASSERT_NE(nullptr, deb_before_overwrite);
    /* uint16_t blocks_before = disk_layout_fc.fsblock.free_blocks; // Commented out as not directly used in assertions below */

    std::string host_content_new = "New overwritten content, much longer to potentially use more blocks and test everything.";
    std::string host_file_new = CreateHostFile("newcont.dat", host_content_new);
    copy_options_fc.ascii_conversion = false;
    /* Use the same OASIS name to trigger overwrite */
    ASSERT_TRUE(oasis_copy_host_file_to_disk(sio_stream_fc, &disk_layout_fc, host_file_new.c_str(), "MYFILE.DAT_D_64", &copy_options_fc));

    const directory_entry_block_t* deb_after_overwrite = FindDebInLayoutFc("MYFILE", "DAT", 0);
    ASSERT_NE(nullptr, deb_after_overwrite);
    EXPECT_EQ(deb_after_overwrite->file_format_dependent1, 64); /* Check if new FFD1 from override took effect */
    EXPECT_EQ(deb_after_overwrite->file_format & FILE_FORMAT_MASK, FILE_FORMAT_DIRECT);

    uint8_t* read_back_overwrite = NULL;
    ssize_t bytes_read_overwrite;
    ASSERT_TRUE(oasis_file_read_data(sio_stream_fc, deb_after_overwrite, &read_back_overwrite, &bytes_read_overwrite));
    ASSERT_NE(nullptr, read_back_overwrite);
    /* Logical size for DIRECT is rec_count * FFD1. write_data calculates rec_count */
    /* For this test, just check the actual content matches */
    size_t expected_read_len = (host_content_new.length() > (size_t)bytes_read_overwrite && (size_t)bytes_read_overwrite > 0) ? (size_t)bytes_read_overwrite : host_content_new.length();
    if ((size_t)bytes_read_overwrite < host_content_new.length()) {
        /* This can happen if logical size (rec_count * rec_len) is smaller than full content */
        /* or if data was truncated to fit allocated blocks. */
        /* For a direct file, it's about how many records of FFD1 length fit. */
    }

    ASSERT_EQ(0, memcmp(read_back_overwrite, host_content_new.data(), expected_read_len));
    if(read_back_overwrite) free(read_back_overwrite);

    /* Check free blocks: (blocks_before + old_file_blocks) - new_file_blocks */
    /* This test isn't precise enough without tracking exact block changes of the overwrite */
}

TEST_F(OasisFileCopyTest, NotEnoughSpace) {
    /* Fill up most of the disk */
    InitializeAllocMapFc(10, true); /* Small disk: ~9KB free after sys block */
    uint16_t initial_free = disk_layout_fc.fsblock.free_blocks;
    ASSERT_GT(initial_free, 0);

    std::vector<uint8_t> large_data( (size_t)initial_free * BLOCK_SIZE + 1024 ); /* Make it larger than free space */
    std::string host_large_file = CreateHostFileBinary("large.bin", large_data);

    copy_options_fc.ascii_conversion = false;
    ASSERT_FALSE(oasis_copy_host_file_to_disk(sio_stream_fc, &disk_layout_fc, host_large_file.c_str(), NULL, &copy_options_fc));

    /* Verify no new file was created */
    EXPECT_EQ(nullptr, FindDebInLayoutFc("LARGE", "BIN", 0));
    /* Verify free blocks count is unchanged */
    EXPECT_EQ(disk_layout_fc.fsblock.free_blocks, initial_free);
}

TEST_F(OasisFileCopyTest, DirectoryFull) {
    /* Make directory small (e.g. 1 sector = 8 DEBs), fill it up */
    disk_layout_fc.fsblock.dir_sectors_max = 1;
    std::vector<directory_entry_block_t> debs_full_dir(8);
    for(int i=0; i < 8; ++i) {
        char fname[FNAME_LEN+1]; snprintf(fname, sizeof(fname), "FILE%d", i);
        tests_common::populate_deb(&debs_full_dir[i], fname, "EXT", FILE_FORMAT_DIRECT, (uint16_t)(100+i*4), 1,1,128,0,0);
    }
    PopulateMockDirectoryFc(debs_full_dir);
    WriteDirectoryAndSystemBlocksToImageFc(); /* Write full directory */

    std::string host_content_str_df = "content for full dir test";
    std::string host_file_df = CreateHostFile("newfile.dat", host_content_str_df);
    copy_options_fc.ascii_conversion = false;

    /* Attempt to add one more file */
    ASSERT_FALSE(oasis_copy_host_file_to_disk(sio_stream_fc, &disk_layout_fc, host_file_df.c_str(), "NEWFILE.DAT", &copy_options_fc));
    EXPECT_EQ(nullptr, FindDebInLayoutFc("NEWFILE", "DAT", 0));
}
