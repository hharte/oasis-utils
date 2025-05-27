/* tests/test_oasis_file_rename.cpp */
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
#include "oasis.h"
#include "oasis_file_rename.h"
}

class OasisFileRenameTest : public ::testing::Test {
protected:
    std::filesystem::path temp_image_path;
    sector_io_stream_t* sio_stream;
    oasis_disk_layout_t disk_layout;
    std::vector<uint8_t> alloc_map_buffer_rename;
    cli_options_t rename_options;

    void InitializeAllocMapRename(size_t num_1k_blocks_in_map, bool pre_allocate_system_blocks = true) {
        size_t map_bytes = (num_1k_blocks_in_map + 7) / 8;
        if (map_bytes == 0 && num_1k_blocks_in_map > 0) map_bytes = 1;
        alloc_map_buffer_rename.assign(map_bytes, 0x00);
        disk_layout.alloc_map.map_data = alloc_map_buffer_rename.data();
        disk_layout.alloc_map.map_size_bytes = alloc_map_buffer_rename.size();
        if (pre_allocate_system_blocks && num_1k_blocks_in_map > 0) {
            set_block_state(&disk_layout.alloc_map, 0, 1);
        }
        disk_layout.fsblock.free_blocks = (uint16_t)count_total_free_blocks(&disk_layout.alloc_map);
    }

    void PopulateMockDirectoryRename(const std::vector<directory_entry_block_t>& debs) {
        if (disk_layout.directory) { free(disk_layout.directory); disk_layout.directory = NULL; }
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

    void WriteDirectoryAndSystemBlocksToImageRename() {
        if (!sio_stream) return;
        uint8_t sector1_buffer[SECTOR_SIZE];
        memcpy(sector1_buffer, &disk_layout.fsblock, sizeof(filesystem_block_t));
        if (disk_layout.alloc_map.map_data && disk_layout.alloc_map.map_size_bytes > 0) {
             size_t map_in_s1 = SECTOR_SIZE - sizeof(filesystem_block_t);
             if(map_in_s1 > disk_layout.alloc_map.map_size_bytes) map_in_s1 = disk_layout.alloc_map.map_size_bytes;
            memcpy(sector1_buffer + sizeof(filesystem_block_t), disk_layout.alloc_map.map_data, map_in_s1);
        }
        ASSERT_EQ(sector_io_write(sio_stream, 1, 1, sector1_buffer), 1);
        /* Write additional AM sectors */
        uint32_t add_am_sec = disk_layout.fsblock.fs_flags & ADDITIONAL_AM_SECTORS_MASK;
        if (add_am_sec > 0 && disk_layout.alloc_map.map_data) {
            size_t map_in_s1 = SECTOR_SIZE - sizeof(filesystem_block_t);
            if (disk_layout.alloc_map.map_size_bytes > map_in_s1) {
                ASSERT_EQ(sector_io_write(sio_stream, 2, add_am_sec, disk_layout.alloc_map.map_data + map_in_s1), (ssize_t)add_am_sec);
            }
        }
        if (disk_layout.directory && disk_layout.fsblock.dir_sectors_max > 0) {
            uint32_t dir_start = 1 + 1 + add_am_sec;
            ASSERT_EQ(sector_io_write(sio_stream, dir_start, disk_layout.fsblock.dir_sectors_max, (uint8_t*)disk_layout.directory->directory), disk_layout.fsblock.dir_sectors_max);
        }
    }

    void SetUp() override {
        sio_stream = nullptr;
        memset(&disk_layout, 0, sizeof(disk_layout));
        alloc_map_buffer_rename.clear();
        memset(&rename_options, 0, sizeof(rename_options));

        const ::testing::TestInfo* const test_info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        std::string unique_filename = std::string("oasis_rename_test_") + test_info->test_suite_name() + "_" + test_info->name() + ".img";
        temp_image_path = std::filesystem::temp_directory_path() / unique_filename;
        std::filesystem::remove(temp_image_path);

        sio_stream = sector_io_open(temp_image_path.string().c_str(), "w+b");
        ASSERT_NE(nullptr, sio_stream);

        InitializeAllocMapRename(20, true);
        disk_layout.fsblock.dir_sectors_max = 1;
        std::vector<directory_entry_block_t> empty_debs_rn;
        PopulateMockDirectoryRename(empty_debs_rn);
        WriteDirectoryAndSystemBlocksToImageRename();

        rename_options.owner_id_filter = 0; /* Default */
    }

    void TearDown() override {
        if (sio_stream) sector_io_close(sio_stream);
        if (disk_layout.directory) free(disk_layout.directory);
        std::filesystem::remove(temp_image_path);
    }

    /* Helper to find a DEB by OASIS name and type in the current disk_layout */
    const directory_entry_block_t* FindDebInLayout(const char* fname, const char* ftype, uint8_t owner_id) {
        char padded_fname[FNAME_LEN]; /* Not null-terminated for DEB comparison. */
        char padded_ftype[FTYPE_LEN]; /* Not null-terminated for DEB comparison. */
        size_t len_to_copy;       /* Length of string to copy. */

        /* Initialize with spaces. */
        memset(padded_fname, ' ', FNAME_LEN);
        memset(padded_ftype, ' ', FTYPE_LEN);

        if (fname) { /* Check if fname is not NULL. */
            len_to_copy = strlen(fname);
            if (len_to_copy > FNAME_LEN) { /* Truncate if source is longer. */
                len_to_copy = FNAME_LEN;
            }
            memcpy(padded_fname, fname, len_to_copy);
        }

        if (ftype) { /* Check if ftype is not NULL. */
            len_to_copy = strlen(ftype);
            if (len_to_copy > FTYPE_LEN) { /* Truncate if source is longer. */
                len_to_copy = FTYPE_LEN;
            }
            memcpy(padded_ftype, ftype, len_to_copy);
        }

        if (!disk_layout.directory) return nullptr;
        size_t num_entries = disk_layout.directory->directory_size_bytes / sizeof(directory_entry_block_t);
        for (size_t i = 0; i < num_entries; ++i) {
            const directory_entry_block_t* current_deb = &disk_layout.directory->directory[i];
            if (oasis_deb_is_valid(current_deb) &&
                (owner_id == (uint8_t)OWNER_ID_WILDCARD || current_deb->owner_id == owner_id) &&
                memcmp(current_deb->file_name, padded_fname, FNAME_LEN) == 0 &&
                memcmp(current_deb->file_type, padded_ftype, FTYPE_LEN) == 0) {
                return current_deb;
            }
        }
        return nullptr;
    }
};

TEST_F(OasisFileRenameTest, RenameSingleFileSuccess) {
    std::vector<directory_entry_block_t> debs_rn;
    directory_entry_block_t deb_orig;
    tests_common::populate_deb(&deb_orig, "OLDFILE", "TXT", FILE_FORMAT_SEQUENTIAL, 10, 1, 1, 0, 10, 0);
    debs_rn.push_back(deb_orig);
    PopulateMockDirectoryRename(debs_rn);
    WriteDirectoryAndSystemBlocksToImageRename();

    rename_options.pattern = "OLDFILE.TXT";
    const char* new_name_str = "NEWFILE.DAT";
    ASSERT_TRUE(oasis_rename_file_by_pattern_and_name(sio_stream, &disk_layout, &rename_options, new_name_str));

    /* Verify in-memory DEB */
    EXPECT_EQ(strncmp(disk_layout.directory->directory[0].file_name, "NEWFILE ", FNAME_LEN), 0);
    EXPECT_EQ(strncmp(disk_layout.directory->directory[0].file_type, "DAT     ", FTYPE_LEN), 0);
    EXPECT_EQ(disk_layout.directory->directory[0].start_sector, 10); /* Should not change */

    /* Verify by reloading from disk */
    oasis_disk_layout_t reloaded_layout_rn{}; /* Modern C++ way to zero-initialize */
    sector_io_stream_t* temp_sio_rn = sector_io_open(temp_image_path.string().c_str(), "rb");
    ASSERT_NE(nullptr, temp_sio_rn);
    ASSERT_TRUE(load_oasis_disk(temp_sio_rn, &reloaded_layout_rn));
    ASSERT_NE(nullptr, reloaded_layout_rn.directory);
    ASSERT_GT(reloaded_layout_rn.directory->directory_size_bytes, static_cast<size_t>(0));
    EXPECT_EQ(strncmp(reloaded_layout_rn.directory->directory[0].file_name, "NEWFILE ", FNAME_LEN), 0);
    EXPECT_EQ(strncmp(reloaded_layout_rn.directory->directory[0].file_type, "DAT     ", FTYPE_LEN), 0);
    cleanup_oasis_disk(&reloaded_layout_rn);
    sector_io_close(temp_sio_rn);
}

TEST_F(OasisFileRenameTest, RenameFailNewNameExists) {
    std::vector<directory_entry_block_t> debs_rn_exist;
    directory_entry_block_t deb_e1, deb_e2;
    tests_common::populate_deb(&deb_e1, "ORIGIN", "DAT", FILE_FORMAT_DIRECT, 40,1,0,0,0,0);
    tests_common::populate_deb(&deb_e2, "EXISTS", "DAT", FILE_FORMAT_DIRECT, 50,1,0,0,0,0);
    debs_rn_exist.push_back(deb_e1);
    debs_rn_exist.push_back(deb_e2);
    PopulateMockDirectoryRename(debs_rn_exist);
    WriteDirectoryAndSystemBlocksToImageRename();

    rename_options.pattern = "ORIGIN.DAT";
    ASSERT_FALSE(oasis_rename_file_by_pattern_and_name(sio_stream, &disk_layout, &rename_options, "EXISTS.DAT"));
    EXPECT_EQ(strncmp(disk_layout.directory->directory[0].file_name, "ORIGIN  ", FNAME_LEN), 0); /* Should not have changed */
}

TEST_F(OasisFileRenameTest, RenameNoMatchOldName) {
    rename_options.pattern = "NOSUCH.FIL";
    ASSERT_TRUE(oasis_rename_file_by_pattern_and_name(sio_stream, &disk_layout, &rename_options, "ANYWAY.DAT"));
    /* No files were actually changed, directory remains as is. */
}

TEST_F(OasisFileRenameTest, RenameWithUserFilter) {
    std::vector<directory_entry_block_t> debs_rn_usr;
    directory_entry_block_t deb_u0_rn, deb_u1_rn, deb_u1_other_rn;
    tests_common::populate_deb(&deb_u0_rn, "TARGET", "FIL", FILE_FORMAT_DIRECT, 60,1,0,0,0,0); /* Owner 0 */
    tests_common::populate_deb(&deb_u1_rn, "TARGET", "FIL", FILE_FORMAT_DIRECT, 70,1,0,0,0,1); /* Owner 1 */
    tests_common::populate_deb(&deb_u1_other_rn, "NEWNAMEX", "FIL", FILE_FORMAT_DIRECT, 80,1,0,0,0,1); /* Owner 1, different name */

    debs_rn_usr.push_back(deb_u0_rn);
    debs_rn_usr.push_back(deb_u1_rn);
    debs_rn_usr.push_back(deb_u1_other_rn);
    PopulateMockDirectoryRename(debs_rn_usr);
    WriteDirectoryAndSystemBlocksToImageRename();

    rename_options.pattern = "TARGET.FIL";
    rename_options.owner_id_filter = 1; /* Only rename user 1's file */
    ASSERT_TRUE(oasis_rename_file_by_pattern_and_name(sio_stream, &disk_layout, &rename_options, "RENAMED.YES"));

    /* Check User 0's file - should be unchanged */
    const directory_entry_block_t* found_deb_u0 = FindDebInLayout("TARGET", "FIL", 0);
    ASSERT_NE(nullptr, found_deb_u0);
    EXPECT_EQ(strncmp(found_deb_u0->file_name, "TARGET  ", FNAME_LEN),0);

    /* Check User 1's original "TARGET.FIL" - should now be "RENAMED.YES" */
    const directory_entry_block_t* original_u1_target_gone = FindDebInLayout("TARGET", "FIL", 1);
    EXPECT_EQ(nullptr, original_u1_target_gone); /* Original name should be gone for user 1*/

    const directory_entry_block_t* found_deb_u1_renamed = FindDebInLayout("RENAMED", "YES", 1);
    ASSERT_NE(nullptr, found_deb_u1_renamed);
    EXPECT_EQ(strncmp(found_deb_u1_renamed->file_name, "RENAMED ", FNAME_LEN),0);
    EXPECT_EQ(strncmp(found_deb_u1_renamed->file_type, "YES     ", FTYPE_LEN),0);
    EXPECT_EQ(found_deb_u1_renamed->owner_id, 1);

    /* Check User 1's other file - should be unchanged */
     const directory_entry_block_t* found_deb_u1_other = FindDebInLayout("NEWNAMEX", "FIL", 1);
    ASSERT_NE(nullptr, found_deb_u1_other);
     EXPECT_EQ(strncmp(found_deb_u1_other->file_name, "NEWNAMEX", FNAME_LEN),0);


    /* Attempt to rename to a name that exists for *another* user, should be fine */
    rename_options.pattern = "RENAMED.YES"; /* The one we just renamed for user 1 */
    rename_options.owner_id_filter = 1;
    ASSERT_TRUE(oasis_rename_file_by_pattern_and_name(sio_stream, &disk_layout, &rename_options, "TARGET.FIL"))
     << "Should be able to rename user 1's file to TARGET.FIL, even if user 0 has one, if owner filter is active.";

    const directory_entry_block_t* found_deb_u1_reverted = FindDebInLayout("TARGET", "FIL", 1);
    ASSERT_NE(nullptr, found_deb_u1_reverted);

}

TEST_F(OasisFileRenameTest, RenameNewNameTooLong) {
    std::vector<directory_entry_block_t> debs_rn_long;
    directory_entry_block_t deb_long1;
    tests_common::populate_deb(&deb_long1, "ORIG", "EXT", FILE_FORMAT_DIRECT, 10,1,0,0,0,0);
    debs_rn_long.push_back(deb_long1);
    PopulateMockDirectoryRename(debs_rn_long);
    WriteDirectoryAndSystemBlocksToImageRename();

    rename_options.pattern = "ORIG.EXT";
    EXPECT_FALSE(oasis_rename_file_by_pattern_and_name(sio_stream, &disk_layout, &rename_options, "VERYLONGNAMEINDEED.EXT"));
    EXPECT_FALSE(oasis_rename_file_by_pattern_and_name(sio_stream, &disk_layout, &rename_options, "SHORTN.VERYLONGEXT"));
}
