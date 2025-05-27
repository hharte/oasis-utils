/* tests_oasis_common.cpp */
#include "oasis.h"
#include "test_oasis_common.h"
#include <cstring>   /* For memset, strncpy, memcpy, strlen */
#include <fstream>   /* For std::ofstream, std::ifstream */
#include <cstdio>    /* For fprintf, stderr, strerror */
#include <cerrno>    /* For errno */


namespace tests_common {

/*
 * Populates a directory_entry_block_t structure with given values.
 */
void populate_deb(
    directory_entry_block_t* deb,
    const char* fname,
    const char* ftype,
    uint8_t format,
    uint16_t start_sector,
    uint16_t block_count,
    uint16_t record_count,
    uint16_t ffd1,
    uint16_t ffd2,
    uint8_t owner_id) { /* Added owner_id parameter */

    if (!deb) {
        return;
    }
    std::memset(deb, 0, sizeof(directory_entry_block_t));

    deb->file_format = format;

    /* Initialize with spaces, then copy actual name/type */
    std::memset(deb->file_name, ' ', FNAME_LEN);
    if (fname) {
        size_t len_fname = std::strlen(fname);
        if (len_fname > FNAME_LEN) {
            len_fname = FNAME_LEN; /* Truncate if longer */
        }
        std::memcpy(deb->file_name, fname, len_fname);
    }

    std::memset(deb->file_type, ' ', FTYPE_LEN);
    if (ftype) {
        size_t len_ftype = std::strlen(ftype);
        if (len_ftype > FTYPE_LEN) {
            len_ftype = FTYPE_LEN; /* Truncate if longer */
        }
        std::memcpy(deb->file_type, ftype, len_ftype);
    }

    deb->start_sector = start_sector;
    deb->block_count = block_count;
    deb->record_count = record_count;
    deb->file_format_dependent1 = ffd1;
    deb->file_format_dependent2 = ffd2;

    /* Default timestamp, e.g., 04/23/85 14:30 */
    /* Corresponds to: raw[0]=0x4B, raw[1]=0xC3, raw[2]=0x9E */
    deb->timestamp.raw[0] = 0x4B;
    deb->timestamp.raw[1] = 0xC3;
    deb->timestamp.raw[2] = 0x9E;
    deb->owner_id = owner_id; /* Use provided owner_id */
    deb->shared_from_owner_id = 0; /* Default */
}

/*
 * Generates a vector of bytes with predictable patterned data.
 */
std::vector<uint8_t> generate_patterned_data(
    size_t data_size,
    uint8_t pattern_base) {
    std::vector<uint8_t> data(data_size);
    for (size_t i = 0; i < data_size; ++i) {
        data[i] = static_cast<uint8_t>((pattern_base + i) % 256);
    }
    return data;
}

/*
 * Creates a dummy disk image file with patterned sectors.
 */
bool create_dummy_image_file(
    const std::filesystem::path& filepath,
    uint32_t num_sectors) {

    std::ofstream outfile(filepath, std::ios::binary | std::ios::trunc);
    if (!outfile.is_open()) {
        #ifdef _WIN32
            char err_buf[256];
            strerror_s(err_buf, sizeof(err_buf), errno);
            fprintf(stderr, "tests_common::create_dummy_image_file: failed to open file %s: %s\n", filepath.string().c_str(), err_buf);
        #else
            fprintf(stderr, "tests_common::create_dummy_image_file: failed to open file %s: %s\n", filepath.string().c_str(), strerror(errno));
        #endif
        return false;
    }

    if (num_sectors > 0) {
        std::vector<uint8_t> sector_data_vec; /* Re-use vector inside loop */
        sector_data_vec.reserve(SECTOR_SIZE); /* Reserve once */
        for (uint32_t i = 0; i < num_sectors; ++i) {
            sector_data_vec = generate_patterned_data(SECTOR_SIZE, static_cast<uint8_t>(i));
            outfile.write(reinterpret_cast<const char*>(sector_data_vec.data()), SECTOR_SIZE);
            if (!outfile.good()) {
                fprintf(stderr, "tests_common::create_dummy_image_file: fwrite failed for sector %u in file %s\n", i, filepath.string().c_str());
                outfile.close();
                std::filesystem::remove(filepath); /* Attempt to clean up */
                return false;
            }
        }
    }

    outfile.close();
    return !outfile.fail(); /* True if close succeeded (no error flags set) */
}

/*
 * Reads the entire content of a file into a vector of bytes.
 */
std::vector<uint8_t> read_file_to_bytes(
    const std::filesystem::path& filepath) {
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return {};
    }
    std::streamsize size = file.tellg();
    if (size < 0) { /* Error or empty file might yield -1 for tellg */
        return {};
    }
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    if (size == 0) { /* Handle empty file case explicitly */
        return buffer;
    }
    if (file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        return buffer;
    }
    return {}; /* Read failed */
}

} /* namespace tests_common */
