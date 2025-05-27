/* tests/test_oasis_disk_util_integration.cpp */
#include "gtest/gtest.h"
#include <vector>
#include <string>
#include <fstream>
#include <sstream> // Required for std::istringstream
#include <cstdio>  // For popen, pclose, fgets, system
#include <cstdlib> // For std::system
#include <cstring>
#include <cerrno>
#include <filesystem> // For temporary directory management and file system operations (C++17)
#include <algorithm>
#include <stdexcept> // For std::runtime_error

/* Common Test Utilities */
/* #include "test_oasis_common.h" */

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

/* Global variable to store the disk image path */
#ifndef OASIS_DISK_IMAGE_PATH_FROM_CMAKE
#define OASIS_DISK_IMAGE_PATH_FROM_CMAKE ""
#endif
char g_test_disk_image_path[1024] = OASIS_DISK_IMAGE_PATH_FROM_CMAKE;

/* Global variable for the oasis_disk_util executable path */
#ifndef OASIS_DISK_UTIL_EXECUTABLE_PATH_FROM_CMAKE
#define OASIS_DISK_UTIL_EXECUTABLE_PATH_FROM_CMAKE ""
#endif
char g_oasis_disk_util_executable_path[1024] = OASIS_DISK_UTIL_EXECUTABLE_PATH_FROM_CMAKE;

// Enhanced helper function to execute a command and capture its standard output
std::string execute_command_and_capture_output(const std::string& command, int& command_exit_code) {
    std::string output;
    char buffer[256];
    FILE* pipe = nullptr;

    command_exit_code = -1; // Default to error

#ifdef _WIN32
    pipe = _popen(command.c_str(), "r");
#else
    pipe = popen(command.c_str(), "r");
#endif

    if (!pipe) {
        // perror("popen() failed"); // perror prints to stderr, might be good for CI logs
        return "TEST_ERROR_POPEN_FAILED: popen() failed for command: " + command + " (errno: " + std::to_string(errno) + ")";
    }

    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }

    int pclose_ret;
#ifdef _WIN32
    pclose_ret = _pclose(pipe);
#else
    pclose_ret = pclose(pipe);
#endif

    if (pclose_ret == -1) {
        // pclose failed
        // perror("pclose() failed");
        command_exit_code = -1; // Indicate pclose error
        // Append to output for test visibility, as pclose doesn't affect stdout directly
        output += "\nTEST_ERROR_PCLOSE_FAILED: pclose() failed (errno: " + std::to_string(errno) + ")";
    }
    else {
        // On POSIX, pclose returns wait status. WEXITSTATUS gives actual exit code.
        // On Windows, _pclose returns the exit code directly.
#ifdef _WIN32
        command_exit_code = pclose_ret;
#else
        if (WIFEXITED(pclose_ret)) {
            command_exit_code = WEXITSTATUS(pclose_ret);
        }
        else {
            command_exit_code = -1; // Indicate non-normal termination
            output += "\nTEST_ERROR_PCLOSE_NON_NORMAL_EXIT: Process did not terminate normally. Status: " + std::to_string(pclose_ret);
        }
#endif
    }
    return output;
}


class OasisDiskUtilIntegrationTest : public ::testing::Test {
protected:
    std::filesystem::path temp_output_dir;
    sector_io_stream_t* sio_stream;
    oasis_disk_layout_t disk_layout;
    cli_options_t extract_options;

    int count_files_in_directory(const std::filesystem::path& dir_path, const std::string& exclude_filename = "") {
        int count = 0;
        if (!std::filesystem::exists(dir_path) || !std::filesystem::is_directory(dir_path)) {
            return 0;
        }
        for (const auto& entry : std::filesystem::directory_iterator(dir_path)) {
            if (entry.is_regular_file()) {
                if (exclude_filename.empty() || entry.path().filename().string() != exclude_filename) {
                    count++;
                }
            }
        }
        return count;
    }

    std::string native_executable_path_str;
    std::string native_test_disk_image_path_str;


    void SetUp() override {
        sio_stream = nullptr;
        memset(&disk_layout, 0, sizeof(disk_layout));
        disk_layout.directory = nullptr;
        memset(&extract_options, 0, sizeof(extract_options));

        if (strlen(g_test_disk_image_path) == 0) {
            GTEST_SKIP() << "Test disk image path not set. Provide via --disk_image, CMake def, or env var.";
            return;
        }
        if (!std::filesystem::exists(g_test_disk_image_path)) {
            GTEST_SKIP() << "Test disk image not found: " << g_test_disk_image_path;
            return;
        }
        native_test_disk_image_path_str = std::filesystem::path(g_test_disk_image_path).make_preferred().string();


        if (strlen(g_oasis_disk_util_executable_path) == 0) {
            GTEST_SKIP() << "Path to oasis_disk_util executable not set. Provide via --disk_util_path, CMake def, or env var.";
            return;
        }
        if (!std::filesystem::exists(g_oasis_disk_util_executable_path)) {
            GTEST_SKIP() << "oasis_disk_util executable not found at: " << g_oasis_disk_util_executable_path;
            return;
        }
        native_executable_path_str = std::filesystem::path(g_oasis_disk_util_executable_path).make_preferred().string();


        const ::testing::TestInfo* const test_info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        std::filesystem::path base_temp_dir = std::filesystem::temp_directory_path();
        std::string unique_subdir_name = std::string("OasisInteg_") +
            (test_info ? test_info->test_suite_name() : "UnknownSuite") + "_" +
            (test_info ? test_info->name() : "UnknownTest");
        temp_output_dir = base_temp_dir / unique_subdir_name;
        std::filesystem::remove_all(temp_output_dir); // Clean slate
        ASSERT_TRUE(std::filesystem::create_directories(temp_output_dir))
            << "Failed to create temp output dir: " << temp_output_dir.string();

        sio_stream = sector_io_open(native_test_disk_image_path_str.c_str(), "rb");
        ASSERT_NE(nullptr, sio_stream) << "Failed to open disk image: " << native_test_disk_image_path_str;

        ASSERT_TRUE(load_oasis_disk(sio_stream, &disk_layout))
            << "Failed to load disk layout from image: " << native_test_disk_image_path_str;
        ASSERT_NE(nullptr, disk_layout.directory) << "Disk directory was not loaded.";
    }

    void TearDown() override {
        if (sio_stream) {
            sector_io_close(sio_stream);
            sio_stream = nullptr;
        }
        cleanup_oasis_disk(&disk_layout);

        if (std::filesystem::exists(temp_output_dir)) {
            std::error_code ec;
            std::filesystem::remove_all(temp_output_dir, ec); // Add error code handling
            if (ec) {
                 // Optionally log the error, but don't fail the test for teardown issues.
                 // For example:
                 // std::cerr << "Warning: Could not remove temp directory " << temp_output_dir << ": " << ec.message() << std::endl;
            }
        }
    }
};

TEST_F(OasisDiskUtilIntegrationTest, ExtractAllFilesViaFunctionCall) {
    if (!sio_stream) GTEST_SKIP();

    extract_options.image_path = native_test_disk_image_path_str.c_str();
    extract_options.operation = "extract";
    extract_options.pattern = nullptr;
    extract_options.ascii_conversion = false;
    extract_options.owner_id_filter = OWNER_ID_WILDCARD;

    ASSERT_TRUE(extract_files_matching_pattern(sio_stream, &disk_layout, temp_output_dir.string().c_str(), &extract_options))
        << "extract_files_matching_pattern failed.";

    int expected_file_count = 0;
    if (disk_layout.directory->directory_size_bytes > 0) {
        size_t num_entries = disk_layout.directory->directory_size_bytes / sizeof(directory_entry_block_t);
        for (size_t i = 0; i < num_entries; ++i) {
            const directory_entry_block_t* deb_entry = &disk_layout.directory->directory[i];
            if (oasis_deb_is_valid(deb_entry) &&
                (extract_options.owner_id_filter == OWNER_ID_WILDCARD || deb_entry->owner_id == extract_options.owner_id_filter)) {
                expected_file_count++;
            }
        }
    }

    int actual_extracted_files = count_files_in_directory(temp_output_dir);
    EXPECT_EQ(expected_file_count, actual_extracted_files)
        << "Number of extracted files does not match expected valid DEBs.";

    if (expected_file_count > 0 && expected_file_count == actual_extracted_files) {
        if (disk_layout.directory->directory_size_bytes > 0) {
            size_t num_entries = disk_layout.directory->directory_size_bytes / sizeof(directory_entry_block_t);
            for (size_t i = 0; i < num_entries; ++i) {
                const directory_entry_block_t* deb_entry = &disk_layout.directory->directory[i];
                if (oasis_deb_is_valid(deb_entry) &&
                    (extract_options.owner_id_filter == OWNER_ID_WILDCARD || deb_entry->owner_id == extract_options.owner_id_filter)) {
                    char host_filename_buf[MAX_HOST_FILENAME_LEN];
                    ASSERT_TRUE(oasis_deb_to_host_filename(deb_entry, host_filename_buf, sizeof(host_filename_buf)));
                    std::filesystem::path expected_file = temp_output_dir / host_filename_buf;
                    EXPECT_TRUE(std::filesystem::exists(expected_file))
                        << "Expected extracted file " << expected_file.string() << " not found.";
                }
            }
        }
    }
    std::cout << "[INFO] Function call test extracted " << actual_extracted_files << " files to " << temp_output_dir.string() << std::endl;
}

TEST_F(OasisDiskUtilIntegrationTest, ExtractAllFilesViaCommandLine) {
    if (!sio_stream) GTEST_SKIP();
    if (native_executable_path_str.empty() || !std::filesystem::exists(native_executable_path_str)) {
        GTEST_SKIP() << "oasis_disk_util executable path not valid or not found: " << native_executable_path_str;
        return;
    }
    std::string temp_output_dir_native_str = temp_output_dir.make_preferred().string();

    std::string command = native_executable_path_str + " \"" +
        native_test_disk_image_path_str + "\" extract \"" +
        temp_output_dir_native_str + "\" -u \"*\"";

    std::cout << "[INFO] Executing command: " << command << std::endl;
    int system_ret = std::system(command.c_str());
    // On Windows, system() returns the exit code of the command processor if it could start,
    // or of the command itself. 0 usually means success for the command.
    // If cmd.exe itself fails to launch or parse, it might return non-zero distinct from app's error.
    ASSERT_EQ(0, system_ret) << "oasis_disk_util extract command failed. Exit code: " << system_ret
        << ". Command: " << command;


    int expected_file_count = 0;
    if (disk_layout.directory->directory_size_bytes > 0) {
        size_t num_entries = disk_layout.directory->directory_size_bytes / sizeof(directory_entry_block_t);
        for (size_t i = 0; i < num_entries; ++i) {
            const directory_entry_block_t* deb_entry = &disk_layout.directory->directory[i];
            if (oasis_deb_is_valid(deb_entry)) {
                expected_file_count++;
            }
        }
    }

    int actual_extracted_files = count_files_in_directory(temp_output_dir);
    EXPECT_EQ(expected_file_count, actual_extracted_files)
        << "Number of extracted files via CLI does not match expected valid DEBs.";

    if (expected_file_count > 0 && expected_file_count == actual_extracted_files) {
        if (disk_layout.directory->directory_size_bytes > 0) {
            size_t num_entries = disk_layout.directory->directory_size_bytes / sizeof(directory_entry_block_t);
            for (size_t i = 0; i < num_entries; ++i) {
                const directory_entry_block_t* deb_entry = &disk_layout.directory->directory[i];
                if (oasis_deb_is_valid(deb_entry)) {
                    char host_filename_buf[MAX_HOST_FILENAME_LEN];
                    ASSERT_TRUE(oasis_deb_to_host_filename(deb_entry, host_filename_buf, sizeof(host_filename_buf)));
                    std::filesystem::path expected_file = temp_output_dir / host_filename_buf;
                    EXPECT_TRUE(std::filesystem::exists(expected_file))
                        << "Expected CLI extracted file " << expected_file.string() << " not found.";
                }
            }
        }
    }
    std::cout << "[INFO] CLI extract test extracted " << actual_extracted_files << " files to " << temp_output_dir.string() << std::endl;
}


TEST_F(OasisDiskUtilIntegrationTest, ListAllFilesViaCommandLine) {
    if (!sio_stream) GTEST_SKIP();
    if (native_executable_path_str.empty() || !std::filesystem::exists(native_executable_path_str)) {
        GTEST_SKIP() << "oasis_disk_util executable path not valid or not found: " << native_executable_path_str;
        return;
    }

    std::string command = native_executable_path_str + " \"" +
        native_test_disk_image_path_str + "\" list -u \"*\"";

    std::cout << "[INFO] Executing command for list: " << command << std::endl;

    int command_exit_code = -1;
    std::string command_output = execute_command_and_capture_output(command, command_exit_code);

    ASSERT_NE(command_output.rfind("TEST_ERROR_POPEN_FAILED", 0), static_cast<std::string::size_type>(0))
        << "popen failed to execute command. Raw output: " << command_output;
    ASSERT_EQ(command_exit_code, 0)
        << "Command '" << command << "' failed with exit code: " << command_exit_code
        << "\nOutput:\n" << command_output;
    ASSERT_FALSE(command_output.empty()) << "Command output was empty.";
    ASSERT_EQ(command_output.find("Error:"), std::string::npos)
        << "Command output seems to contain an error message from oasis_disk_util: "
        << command_output.substr(0, command_output.find('\n'));


    int expected_file_count = 0;
    if (disk_layout.directory->directory_size_bytes > 0) {
        size_t num_entries = disk_layout.directory->directory_size_bytes / sizeof(directory_entry_block_t);
        for (size_t i = 0; i < num_entries; ++i) {
            const directory_entry_block_t* deb_entry = &disk_layout.directory->directory[i];
            if (oasis_deb_is_valid(deb_entry)) {
                expected_file_count++;
            }
        }
    }

    int actual_listed_files = 0;
    std::string line;
    std::istringstream output_stream(command_output);
    bool in_file_list_section = false;

    while (std::getline(output_stream, line)) {
        if (line.find("----------------------------------------------------------------------------------------------------") != std::string::npos) {
            if (!in_file_list_section) { // Entering the list section
                in_file_list_section = true;
            }
            else { // Exiting the list section
                in_file_list_section = false;
                break; // No need to parse further after the list
            }
            continue;
        }
        if (in_file_list_section && !line.empty() &&
            line.find("Host Filename") == std::string::npos) {
            actual_listed_files++;
        }
    }

    EXPECT_EQ(expected_file_count, actual_listed_files)
        << "Number of files listed by CLI (" << actual_listed_files
        << ") does not match expected valid DEBs (" << expected_file_count
        << ").\nCaptured Output:\n" << command_output;

    if (expected_file_count > 0 && expected_file_count == actual_listed_files) {
        if (disk_layout.directory->directory_size_bytes > 0) {
            size_t num_entries = disk_layout.directory->directory_size_bytes / sizeof(directory_entry_block_t);
            for (size_t i = 0; i < num_entries; ++i) {
                const directory_entry_block_t* deb_entry = &disk_layout.directory->directory[i];
                if (oasis_deb_is_valid(deb_entry)) {
                    char host_filename_buf[MAX_HOST_FILENAME_LEN];
                    ASSERT_TRUE(oasis_deb_to_host_filename(deb_entry, host_filename_buf, sizeof(host_filename_buf)));
                    EXPECT_NE(command_output.find(host_filename_buf), std::string::npos)
                        << "Expected filename " << host_filename_buf << " not found in 'list' command output.";
                }
            }
        }
    }
    std::cout << "[INFO] CLI list test processed output. Listed " << actual_listed_files << " files." << std::endl;
}


int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);

    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "--disk_image=", strlen("--disk_image=")) == 0) {
            strncpy(g_test_disk_image_path, argv[i] + strlen("--disk_image="), sizeof(g_test_disk_image_path) - 1);
            g_test_disk_image_path[sizeof(g_test_disk_image_path) - 1] = '\0';
            for (int j = i; j < argc - 1; ++j) argv[j] = argv[j + 1];
            argc--; i--;
        }
        else if (strncmp(argv[i], "--disk_util_path=", strlen("--disk_util_path=")) == 0) {
            strncpy(g_oasis_disk_util_executable_path, argv[i] + strlen("--disk_util_path="), sizeof(g_oasis_disk_util_executable_path) - 1);
            g_oasis_disk_util_executable_path[sizeof(g_oasis_disk_util_executable_path) - 1] = '\0';
            for (int j = i; j < argc - 1; ++j) argv[j] = argv[j + 1];
            argc--; i--;
        }
    }

    if (strlen(g_test_disk_image_path) == 0) {
        if (strlen(OASIS_DISK_IMAGE_PATH_FROM_CMAKE) > 0) {
            strncpy(g_test_disk_image_path, OASIS_DISK_IMAGE_PATH_FROM_CMAKE, sizeof(g_test_disk_image_path) - 1);
            g_test_disk_image_path[sizeof(g_test_disk_image_path) - 1] = '\0';
        }
        else {
            const char* env_path = std::getenv("OASIS_TEST_DISK_IMAGE");
            if (env_path != nullptr && strlen(env_path) > 0) {
                strncpy(g_test_disk_image_path, env_path, sizeof(g_test_disk_image_path) - 1);
                g_test_disk_image_path[sizeof(g_test_disk_image_path) - 1] = '\0';
            }
        }
    }
    if (strlen(g_test_disk_image_path) > 0) {
        printf("[Test Main] Using disk image: %s\n", g_test_disk_image_path);
    }
    else {
        printf("[Test Main] WARNING: No disk image path resolved. Integration tests needing it will be skipped.\n");
    }

    if (strlen(g_oasis_disk_util_executable_path) == 0) {
        if (strlen(OASIS_DISK_UTIL_EXECUTABLE_PATH_FROM_CMAKE) > 0) {
            strncpy(g_oasis_disk_util_executable_path, OASIS_DISK_UTIL_EXECUTABLE_PATH_FROM_CMAKE, sizeof(g_oasis_disk_util_executable_path) - 1);
            g_oasis_disk_util_executable_path[sizeof(g_oasis_disk_util_executable_path) - 1] = '\0';
        }
        else {
            const char* env_path_exec = std::getenv("OASIS_DISK_UTIL_EXEC_PATH");
            if (env_path_exec != nullptr && strlen(env_path_exec) > 0) {
                strncpy(g_oasis_disk_util_executable_path, env_path_exec, sizeof(g_oasis_disk_util_executable_path) - 1);
                g_oasis_disk_util_executable_path[sizeof(g_oasis_disk_util_executable_path) - 1] = '\0';
            }
        }
    }
    if (strlen(g_oasis_disk_util_executable_path) > 0) {
        printf("[Test Main] Using oasis_disk_util path: %s\n", g_oasis_disk_util_executable_path);
    }
    else {
        printf("[Test Main] WARNING: No oasis_disk_util executable path resolved. CLI tests will be skipped.\n");
    }

    return RUN_ALL_TESTS();
}
