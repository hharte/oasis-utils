/* tests_oasis_common.h */
#ifndef TESTS_OASIS_COMMON_H_
#define TESTS_OASIS_COMMON_H_

#include "oasis.h" /* For directory_entry_block_t, FNAME_LEN, FTYPE_LEN, SECTOR_SIZE */
#include <vector>
#include <string>
#include <cstdint>    /* For uint8_t, uint16_t, uint32_t */
#include <filesystem> /* For std::filesystem::path */

namespace tests_common {

    /*
     * Populates a directory_entry_block_t structure with given values.
     * FNAME and FTYPE are space-padded according to OASIS conventions.
     * A default timestamp is also set.
     *
     * @param deb Pointer to the directory_entry_block_t structure to populate.
     * @param fname The file name (up to FNAME_LEN characters).
     * @param ftype The file type/extension (up to FTYPE_LEN characters).
     * @param format The file format and attributes byte.
     * @param start_sector The starting sector number (LBA) of the file.
     * @param block_count The number of 1K blocks allocated to the file.
     * @param record_count The number of records in the file.
     * @param ffd1 File Format Dependent field 1 (e.g., record length).
     * @param ffd2 File Format Dependent field 2 (e.g., load address, program length).
     * @param owner_id The owner ID for the file. Defaults to 1 if not specified in older test calls.
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
        uint8_t owner_id = 0);

/*
 * Generates a vector of bytes with predictable patterned data.
 * The pattern generated is (pattern_base + index) % 256 for each byte.
 * This is useful for creating consistent test sector content or file data.
 *
 * @param data_size The number of bytes to generate.
 * @param pattern_base An initial value to offset the pattern.
 * @return A std::vector<uint8_t> containing the generated data.
 */
std::vector<uint8_t> generate_patterned_data(
    size_t data_size,
    uint8_t pattern_base);

/*
 * Creates a dummy disk image file at the specified path.
 * The file is filled with a specified number of sectors, where each sector
 * contains patterned data. The pattern for each sector is based on its
 * logical block address (LBA) to ensure unique content per sector.
 * This function is useful for setting up mock disk images for testing.
 *
 * @param filepath The path where the dummy disk image file will be created.
 * If the file exists, it will be overwritten.
 * @param num_sectors The total number of sectors the dummy image file should contain.
 * If 0, an empty file is created.
 * @return True on successful creation and writing of all sectors, false otherwise.
 * An error message will be printed to stderr in case of failure.
 */
bool create_dummy_image_file(
    const std::filesystem::path& filepath,
    uint32_t num_sectors);

/*
 * Helper function to read the entire content of a file into a vector of bytes.
 * Useful for verifying the content of extracted or generated files.
 *
 * @param filepath Path to the file to read.
 * @return A std::vector<uint8_t> containing the file content, or an empty
 * vector if the file cannot be opened or read.
 */
std::vector<uint8_t> read_file_to_bytes(
    const std::filesystem::path& filepath);

} /* namespace tests_common */

#endif /* TESTS_OASIS_COMMON_H_ */
