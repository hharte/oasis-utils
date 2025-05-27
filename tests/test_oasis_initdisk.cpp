/* tests/test_oasis_initdisk.cpp */
#include "gtest/gtest.h"
#include <vector>
#include <string>
#include <fstream>
#include <filesystem>
#include <cstring>
#include <cstdio> /* For remove() */
#include <cerrno>

/* Common Test Utilities */
#include "test_oasis_common.h"

/* DUT headers */
extern "C" {
#include "oasis.h"
#include "oasis_initdisk.h"
/* Required for initdisk_create_empty_imd_file, imd_write_file_header, imd_write_comment_block */
#include "libimdf.h"
}

class OasisInitDiskTest : public ::testing::Test {
protected:
    std::filesystem::path temp_image_path_id;
    std::filesystem::path temp_imd_image_path_id;
    sector_io_stream_t* sio_id;
    oasis_disk_layout_t disk_layout_id;
    initdisk_options_lib_t options_id;

    /* Helper to set default options */
    void SetDefaultOptions() {
        memset(&options_id, 0, sizeof(options_id));
        strncpy(options_id.image_path, temp_image_path_id.string().c_str(), sizeof(options_id.image_path) -1);
        options_id.image_path[sizeof(options_id.image_path) -1] = '\0';
        options_id.drive_letter = 'A';
        options_id.num_heads = DEFAULT_NUM_HEADS_LIB;
        options_id.tracks_per_surface = DEFAULT_TRACKS_PER_SURFACE_LIB;
        options_id.sectors_per_track = DEFAULT_SECTORS_PER_TRACK_LIB;
        options_id.sector_increment = DEFAULT_SECTOR_INCREMENT_LIB;
        options_id.track_skew = DEFAULT_TRACK_SKEW_LIB;
        options_id.dir_size = DEFAULT_DIR_SIZE_LIB;
        strncpy(options_id.disk_label_str, "TESTDISK", FNAME_LEN);
        options_id.disk_label_str[FNAME_LEN] = '\0'; /* Ensure null termination */
        options_id.label_specified = 1; /* Assume label is always "specified" for tests */
    }


    void SetUp() override {
        sio_id = nullptr;
        memset(&disk_layout_id, 0, sizeof(disk_layout_id));

        const ::testing::TestInfo* const test_info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        std::string base_name = std::string("oasis_initdisk_") + test_info->test_suite_name() + "_" + test_info->name();
        temp_image_path_id = std::filesystem::temp_directory_path() / (base_name + ".img");
        temp_imd_image_path_id = std::filesystem::temp_directory_path() / (base_name + ".imd");

        std::filesystem::remove(temp_image_path_id);
        std::filesystem::remove(temp_imd_image_path_id);

        SetDefaultOptions(); /* Initialize options with default values */
    }

    void TearDown() override {
        if (sio_id) {
            sector_io_close(sio_id);
            sio_id = nullptr;
        }
        cleanup_oasis_disk(&disk_layout_id);
        std::filesystem::remove(temp_image_path_id);
        std::filesystem::remove(temp_imd_image_path_id);
    }

    /* Helper to verify the basic filesystem structure written to disk */
    void VerifyFilesystemStructuresOnDisk(const std::filesystem::path& image_path_to_verify, const char* expected_label) {
        sector_io_stream_t* verify_sio = sector_io_open(image_path_to_verify.string().c_str(), "rb");
        ASSERT_NE(nullptr, verify_sio) << "Failed to open image for verification: " << image_path_to_verify.string();

        oasis_disk_layout_t loaded_layout;
        memset(&loaded_layout, 0, sizeof(loaded_layout));
        ASSERT_TRUE(load_oasis_disk(verify_sio, &loaded_layout)) << "Failed to load disk layout for verification.";

        /* Verify FS Block details */
        char expected_padded_label[FNAME_LEN + 1];
        memset(expected_padded_label, ' ', FNAME_LEN); /* Fill with spaces */
        /* strncpy(expected_padded_label, expected_label, strlen(expected_label) > FNAME_LEN ? FNAME_LEN : strlen(expected_label)); // Old line */
        /* New approach: */
        size_t len_to_copy = strlen(expected_label);
        if (len_to_copy > FNAME_LEN) {
            len_to_copy = FNAME_LEN; /* Truncate if source is longer than FNAME_LEN */
        }
        memcpy(expected_padded_label, expected_label, len_to_copy);
        /* The rest of expected_padded_label (from len_to_copy to FNAME_LEN-1) remains spaces due to memset. */
        expected_padded_label[FNAME_LEN] = '\0'; /* Ensure null termination */


        EXPECT_EQ(0, strncmp(loaded_layout.fsblock.label, expected_padded_label, FNAME_LEN));
        EXPECT_EQ(loaded_layout.fsblock.num_heads >> 4, options_id.num_heads);
        EXPECT_EQ(loaded_layout.fsblock.num_cyl, options_id.tracks_per_surface);
        EXPECT_EQ(loaded_layout.fsblock.num_sectors, options_id.sectors_per_track);
        uint32_t expected_dir_sectors = (options_id.dir_size + DIR_ENTRIES_PER_SECTOR - 1) / DIR_ENTRIES_PER_SECTOR;
        if (expected_dir_sectors > 255) expected_dir_sectors = 255;
        EXPECT_EQ(loaded_layout.fsblock.dir_sectors_max, expected_dir_sectors);

        uint32_t total_1k_blocks = (uint32_t)options_id.num_heads * options_id.tracks_per_surface * options_id.sectors_per_track / (BLOCK_SIZE / SECTOR_SIZE);
        uint32_t am_bytes_in_sector1_u32 = SECTOR_SIZE - sizeof(filesystem_block_t); /* Use uint32_t for calculation consistency */
        uint32_t additional_am_sectors = 0;
        size_t actual_map_bytes_needed_sz = (total_1k_blocks + 7) / 8;
        if (actual_map_bytes_needed_sz > am_bytes_in_sector1_u32) {
            additional_am_sectors = (uint32_t)((actual_map_bytes_needed_sz - am_bytes_in_sector1_u32 + SECTOR_SIZE - 1) / SECTOR_SIZE);
        }
        EXPECT_EQ(static_cast<uint32_t>(loaded_layout.fsblock.fs_flags & ADDITIONAL_AM_SECTORS_MASK), additional_am_sectors);

        if (options_id.wp_op && !options_id.nowp_op) {
            EXPECT_TRUE(loaded_layout.fsblock.fs_flags & FS_FLAGS_WP);
        } else if (options_id.nowp_op) {
            EXPECT_FALSE(loaded_layout.fsblock.fs_flags & FS_FLAGS_WP);
        }


        /* Verify Allocation Map (basic check: system blocks are allocated) */
        ASSERT_NE(nullptr, loaded_layout.alloc_map.map_data);
        int state;
        ASSERT_EQ(0, get_block_state(&loaded_layout.alloc_map, 0, &state)); /* Boot/FSB/AM1 */
        EXPECT_EQ(1, state);

        for(uint32_t i=0; i < additional_am_sectors; ++i) {
            uint32_t lba = 2 + i;
            uint32_t k_block = lba / SECTORS_PER_BLOCK;
            if (k_block < total_1k_blocks) {
                ASSERT_EQ(0, get_block_state(&loaded_layout.alloc_map, k_block, &state));
            }
        }
        uint32_t dir_start_lba = 2 + additional_am_sectors;
        for(uint32_t i=0; i < expected_dir_sectors; ++i) {
            uint32_t lba = dir_start_lba + i;
            uint32_t k_block = lba / SECTORS_PER_BLOCK;
            if (k_block < total_1k_blocks) {
                 ASSERT_EQ(0, get_block_state(&loaded_layout.alloc_map, k_block, &state));
            }
        }
        if (total_1k_blocks > 0) {
             EXPECT_EQ(loaded_layout.fsblock.free_blocks, total_1k_blocks - count_total_allocated_blocks(&loaded_layout.alloc_map));
        } else {
             EXPECT_EQ(loaded_layout.fsblock.free_blocks, 0);
        }


        /* Verify Directory (all entries should be empty) */
        ASSERT_NE(nullptr, loaded_layout.directory);
        for (size_t i = 0; i < expected_dir_sectors * DIR_ENTRIES_PER_SECTOR; ++i) {
            EXPECT_EQ(loaded_layout.directory->directory[i].file_format, FILE_FORMAT_EMPTY);
        }

        cleanup_oasis_disk(&loaded_layout);
        sector_io_close(verify_sio);
    }

    size_t count_total_allocated_blocks(const oasis_alloc_map_t* map) {
        if (!map || !map->map_data) return 0;
        size_t allocated_count = 0;
        size_t total_map_bits = map->map_size_bytes * 8;
        for (size_t i = 0; i < total_map_bits; ++i) {
            int state;
            if (get_block_state(map, i, &state) == 0 && state == 1) {
                allocated_count++;
            }
        }
        return allocated_count;
    }
};


TEST_F(OasisInitDiskTest, CreateEmptyImdFileSuccess) {
    ASSERT_TRUE(initdisk_create_empty_imd_file(temp_imd_image_path_id.string().c_str()))
        << "Failed to create empty IMD file.";
    ASSERT_TRUE(std::filesystem::exists(temp_imd_image_path_id));

    /* Minimal verification: check if file is non-empty (should contain header + comment terminator) */
    std::ifstream imdfile(temp_imd_image_path_id, std::ios::binary | std::ios::ate);
    ASSERT_TRUE(imdfile.is_open());
    EXPECT_GT(imdfile.tellg(), 0);
    imdfile.close();
}

TEST_F(OasisInitDiskTest, CreateEmptyImdFileInvalidPath) {
    std::filesystem::path invalid_dir = temp_imd_image_path_id.parent_path() / "non_existent_subdir_id";
    std::filesystem::path invalid_path = invalid_dir / "test.imd";
    ASSERT_FALSE(initdisk_create_empty_imd_file(invalid_path.string().c_str()));
    ASSERT_FALSE(std::filesystem::exists(invalid_path));
}


TEST_F(OasisInitDiskTest, InitFilesystemStructuresSuccess) {
    sio_id = sector_io_open(temp_image_path_id.string().c_str(), "w+b");
    ASSERT_NE(nullptr, sio_id);
    options_id.format_op = 1; /* Simulate FORMAT context for geometry usage */

    ASSERT_EQ(EXIT_SUCCESS, initdisk_initialize_filesystem_structures(sio_id, &options_id, &disk_layout_id));
    VerifyFilesystemStructuresOnDisk(temp_image_path_id, options_id.disk_label_str);
}

TEST_F(OasisInitDiskTest, InitFilesystemStructuresInvalidGeometry) {
    sio_id = sector_io_open(temp_image_path_id.string().c_str(), "w+b");
    ASSERT_NE(nullptr, sio_id);
    options_id.format_op = 1;
    options_id.num_heads = 0; /* Invalid geometry */
    ASSERT_EQ(EXIT_FAILURE, initdisk_initialize_filesystem_structures(sio_id, &options_id, &disk_layout_id));
}

TEST_F(OasisInitDiskTest, InitFilesystemStructuresDiskTooLargeForAm) {
    sio_id = sector_io_open(temp_image_path_id.string().c_str(), "w+b");
    ASSERT_NE(nullptr, sio_id);
    options_id.format_op = 1;
    options_id.num_heads = 16;             /* Example large geometry */
    options_id.tracks_per_surface = 200;
    options_id.sectors_per_track = 32;    /* Leads to > 7 additional AM sectors */
    ASSERT_EQ(EXIT_FAILURE, initdisk_initialize_filesystem_structures(sio_id, &options_id, &disk_layout_id));
}


TEST_F(OasisInitDiskTest, HandleFormatOperationRawSuccess) {
    options_id.format_op = 1;
    strncpy(options_id.image_path, temp_image_path_id.string().c_str(), sizeof(options_id.image_path) -1);
    options_id.image_path[sizeof(options_id.image_path)-1] = '\0';

    ASSERT_EQ(EXIT_SUCCESS, initdisk_perform_operation(&options_id));
    VerifyFilesystemStructuresOnDisk(temp_image_path_id, options_id.disk_label_str);

    sector_io_stream_t* verify_sio = sector_io_open(temp_image_path_id.string().c_str(), "rb");
    ASSERT_NE(nullptr, verify_sio);
    uint32_t total_sectors_val = sector_io_get_total_sectors(verify_sio);
    ASSERT_GT(total_sectors_val, (uint32_t)options_id.dir_size / DIR_ENTRIES_PER_SECTOR + 2 );
    uint8_t sector_data[SECTOR_SIZE];
    uint32_t data_sector_to_check = 2 + (options_id.num_heads * options_id.tracks_per_surface * options_id.sectors_per_track / (BLOCK_SIZE / SECTOR_SIZE) > 0 ?
                                       (disk_layout_id.fsblock.fs_flags & ADDITIONAL_AM_SECTORS_MASK) : 0) +
                                  ((options_id.dir_size + DIR_ENTRIES_PER_SECTOR -1) / DIR_ENTRIES_PER_SECTOR) + 1;

    if (data_sector_to_check < total_sectors_val) {
        ASSERT_EQ(sector_io_read(verify_sio, data_sector_to_check, 1, sector_data), 1);
        /* For RAW format, sectors beyond the OS-managed area should be 0xE5 */
        /* This check is simplified; a more robust check would iterate several data sectors. */
        bool all_e5 = true;
        for(size_t i = 0; i < SECTOR_SIZE; ++i) {
            if (sector_data[i] != 0xE5) {
                all_e5 = false;
                break;
            }
        }
        EXPECT_TRUE(all_e5) << "Expected data sector " << data_sector_to_check << " to be filled with 0xE5 after RAW format.";
    }
    sector_io_close(verify_sio);
}

TEST_F(OasisInitDiskTest, HandleFormatOperationImdSuccess) {
    options_id.format_op = 1;
    strncpy(options_id.image_path, temp_imd_image_path_id.string().c_str(), sizeof(options_id.image_path)-1);
    options_id.image_path[sizeof(options_id.image_path)-1] = '\0';

    ASSERT_EQ(EXIT_SUCCESS, initdisk_perform_operation(&options_id));
    VerifyFilesystemStructuresOnDisk(temp_imd_image_path_id, options_id.disk_label_str);

    ImdImageFile* imdf_verify;
    ASSERT_EQ(IMDF_ERR_OK, imdf_open(temp_imd_image_path_id.string().c_str(), 1 /*read-only*/, &imdf_verify));
    size_t num_tracks_val;
    imdf_get_num_tracks(imdf_verify, &num_tracks_val);
    EXPECT_EQ(num_tracks_val, (size_t)options_id.num_heads * options_id.tracks_per_surface);
    if (num_tracks_val > 0) {
        const ImdTrackInfo* track0 = imdf_get_track_info(imdf_verify, 0);
        ASSERT_NE(nullptr, track0);
        EXPECT_EQ(track0->num_sectors, options_id.sectors_per_track);
        EXPECT_EQ(track0->sector_size, static_cast<unsigned int>(SECTOR_SIZE));
    }
    imdf_close(imdf_verify);
}


TEST_F(OasisInitDiskTest, HandleBuildOperationSuccess) {
    options_id.format_op = 1;
    options_id.num_heads = 1; options_id.tracks_per_surface = 10; options_id.sectors_per_track = 10;
    options_id.dir_size = 16;
    strncpy(options_id.disk_label_str, "OLDLABEL", FNAME_LEN);
    options_id.disk_label_str[FNAME_LEN] = '\0'; /* Ensure null termination */
    ASSERT_EQ(EXIT_SUCCESS, initdisk_perform_operation(&options_id));

    initdisk_options_lib_t build_opts;
    memset(&build_opts, 0, sizeof(build_opts));
    strncpy(build_opts.image_path, temp_image_path_id.string().c_str(), sizeof(build_opts.image_path)-1);
    build_opts.build_op = 1;
    build_opts.dir_size = 32;
    build_opts.label_specified = 1;
    strncpy(build_opts.disk_label_str, "NEWLABEL", FNAME_LEN);
    build_opts.disk_label_str[FNAME_LEN] = '\0'; /* Ensure null termination */
    build_opts.num_heads = DEFAULT_NUM_HEADS_LIB;
    build_opts.tracks_per_surface = DEFAULT_TRACKS_PER_SURFACE_LIB;
    build_opts.sectors_per_track = DEFAULT_SECTORS_PER_TRACK_LIB;

    ASSERT_EQ(EXIT_SUCCESS, initdisk_perform_operation(&build_opts));

    sector_io_stream_t* verify_sio_build = sector_io_open(temp_image_path_id.string().c_str(), "rb");
    ASSERT_NE(nullptr, verify_sio_build);
    oasis_disk_layout_t loaded_layout_build;
    memset(&loaded_layout_build, 0, sizeof(loaded_layout_build));
    ASSERT_TRUE(load_oasis_disk(verify_sio_build, &loaded_layout_build));

    EXPECT_EQ(loaded_layout_build.fsblock.num_heads >> 4, 1);
    EXPECT_EQ(loaded_layout_build.fsblock.num_cyl, 10);
    EXPECT_EQ(loaded_layout_build.fsblock.num_sectors, 10);
    EXPECT_EQ(0, strncmp(loaded_layout_build.fsblock.label, "NEWLABEL", FNAME_LEN));
    uint32_t expected_dir_sectors_build = (32 + DIR_ENTRIES_PER_SECTOR - 1) / DIR_ENTRIES_PER_SECTOR;
    if (expected_dir_sectors_build > 255) expected_dir_sectors_build = 255;
    EXPECT_EQ(loaded_layout_build.fsblock.dir_sectors_max, expected_dir_sectors_build);

    cleanup_oasis_disk(&loaded_layout_build);
    sector_io_close(verify_sio_build);
}


TEST_F(OasisInitDiskTest, HandleClearOperationSuccess) {
    options_id.format_op = 1;
    ASSERT_EQ(EXIT_SUCCESS, initdisk_perform_operation(&options_id));

    sio_id = sector_io_open(temp_image_path_id.string().c_str(), "r+b");
    ASSERT_NE(nullptr, sio_id);
    cleanup_oasis_disk(&disk_layout_id);
    ASSERT_TRUE(load_oasis_disk(sio_id, &disk_layout_id));

    directory_entry_block_t temp_deb;
    tests_common::populate_deb(&temp_deb, "TEMPFILE", "DAT", FILE_FORMAT_DIRECT, 0,0,0,0,0,0);
    std::vector<uint8_t> data = tests_common::generate_patterned_data(100, 0x11);
    ASSERT_TRUE(oasis_file_write_data(sio_id, &disk_layout_id, &temp_deb, data.data(), data.size()));

    uint32_t add_am_clear = disk_layout_id.fsblock.fs_flags & ADDITIONAL_AM_SECTORS_MASK;
    uint32_t dir_start_lba_clear = 2 + add_am_clear; /* Sector 1 is FSB/AM, Sector 2+ is additional AM */
    ASSERT_EQ(sector_io_write(sio_id, dir_start_lba_clear, disk_layout_id.fsblock.dir_sectors_max, (uint8_t*)disk_layout_id.directory->directory), disk_layout_id.fsblock.dir_sectors_max);

    /* Write FS Block and initial AM (Sector 1) back as well, since free_blocks changed */
    uint8_t sector1_buffer_after_write[SECTOR_SIZE];
    memcpy(sector1_buffer_after_write, &disk_layout_id.fsblock, sizeof(filesystem_block_t));
    size_t map_in_s1 = SECTOR_SIZE - sizeof(filesystem_block_t);
    if (disk_layout_id.alloc_map.map_data) {
        size_t bytes_to_copy_s1 = disk_layout_id.alloc_map.map_size_bytes < map_in_s1 ? disk_layout_id.alloc_map.map_size_bytes : map_in_s1;
        memcpy(sector1_buffer_after_write + sizeof(filesystem_block_t), disk_layout_id.alloc_map.map_data, bytes_to_copy_s1);
    }
    ASSERT_EQ(sector_io_write(sio_id, 1, 1, sector1_buffer_after_write), 1);

    sector_io_close(sio_id); sio_id = nullptr;
    cleanup_oasis_disk(&disk_layout_id);

    options_id.format_op = 0;
    options_id.clear_op = 1;
    options_id.label_specified = 1;
    strncpy(options_id.disk_label_str, "CLEARED ", FNAME_LEN);
    options_id.disk_label_str[FNAME_LEN] = '\0'; /* Ensure null termination */
    ASSERT_EQ(EXIT_SUCCESS, initdisk_perform_operation(&options_id));

    VerifyFilesystemStructuresOnDisk(temp_image_path_id, "CLEARED");
}


TEST_F(OasisInitDiskTest, HandleLabelOperationSuccess) {
    options_id.format_op = 1;
    ASSERT_EQ(EXIT_SUCCESS, initdisk_perform_operation(&options_id));

    options_id.format_op = 0;
    options_id.label_op = 1;
    strncpy(options_id.disk_label_str, "NEWLBL", FNAME_LEN);
    options_id.disk_label_str[FNAME_LEN] = '\0'; /* Ensure null termination */
    ASSERT_EQ(EXIT_SUCCESS, initdisk_perform_operation(&options_id));

    sector_io_stream_t* verify_sio_lbl = sector_io_open(temp_image_path_id.string().c_str(), "rb");
    ASSERT_NE(nullptr, verify_sio_lbl);
    oasis_disk_layout_t loaded_layout_lbl;
    memset(&loaded_layout_lbl, 0, sizeof(loaded_layout_lbl));
    ASSERT_TRUE(load_oasis_disk(verify_sio_lbl, &loaded_layout_lbl));
    EXPECT_EQ(0, strncmp(loaded_layout_lbl.fsblock.label, "NEWLBL  ", FNAME_LEN));
    cleanup_oasis_disk(&loaded_layout_lbl);
    sector_io_close(verify_sio_lbl);
}

TEST_F(OasisInitDiskTest, HandleWpOperationSuccess) {
    options_id.format_op = 1;
    ASSERT_EQ(EXIT_SUCCESS, initdisk_perform_operation(&options_id));

    options_id.format_op = 0;
    options_id.wp_op = 1;
    ASSERT_EQ(EXIT_SUCCESS, initdisk_perform_operation(&options_id));

    sector_io_stream_t* verify_sio_wp = sector_io_open(temp_image_path_id.string().c_str(), "rb");
    ASSERT_NE(nullptr, verify_sio_wp);
    oasis_disk_layout_t loaded_layout_wp;
    memset(&loaded_layout_wp, 0, sizeof(loaded_layout_wp));
    ASSERT_TRUE(load_oasis_disk(verify_sio_wp, &loaded_layout_wp));
    EXPECT_TRUE(loaded_layout_wp.fsblock.fs_flags & FS_FLAGS_WP);
    cleanup_oasis_disk(&loaded_layout_wp);
    sector_io_close(verify_sio_wp);

    options_id.wp_op = 0;
    options_id.nowp_op = 1;
    ASSERT_EQ(EXIT_SUCCESS, initdisk_perform_operation(&options_id));

    verify_sio_wp = sector_io_open(temp_image_path_id.string().c_str(), "rb");
    ASSERT_NE(nullptr, verify_sio_wp);
    memset(&loaded_layout_wp, 0, sizeof(loaded_layout_wp));
    ASSERT_TRUE(load_oasis_disk(verify_sio_wp, &loaded_layout_wp));
    EXPECT_FALSE(loaded_layout_wp.fsblock.fs_flags & FS_FLAGS_WP);
    cleanup_oasis_disk(&loaded_layout_wp);
    sector_io_close(verify_sio_wp);
}

TEST_F(OasisInitDiskTest, PerformOperationNoPrimaryOp) {
    /* This test sets up default options which do not set any primary operation flag (format_op, clear_op, etc.) to 1.
     * initdisk_perform_operation should then return EXIT_FAILURE as no action is specified.
     */
    EXPECT_EQ(EXIT_FAILURE, initdisk_perform_operation(&options_id));
}
