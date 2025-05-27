#include "gtest/gtest.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <cstring> // For strcpy, memset, strncpy
#include <cstdio>  // For remove
#include <cerrno>  // For errno

// Ensure C linkage for C headers when included in C++
extern "C" {
#include "oasis_transfer_utils.h"
#include "mm_serial.h"  // For serial port function declarations (will be stubbed)
#include "oasis_pcap.h" // For PCAP function declarations (will be stubbed)
#include "oasis.h"      // For kPathSeparator (though not directly tested here, good include)
}

// --- Global Stubs/Mocks for External Dependencies ---
// These allow us to control the behavior of external functions for testing.
int g_mock_open_serial_return = -1;
char g_mock_open_serial_device_called[256];
int g_mock_init_serial_return = -1;
int g_mock_init_serial_fd_called = -1;
int g_mock_init_serial_baudrate_called = 0;
bool g_mock_init_serial_flow_control_called = false;
int g_mock_close_serial_return = 0;
int g_mock_close_serial_fd_called = -1;

FILE* g_mock_pcap_create_file_ptr_return = nullptr;
char g_mock_pcap_create_filename_called[256];
int g_mock_pcap_create_return_code = -1;
int g_mock_pcap_close_return_code = 0;
FILE* g_mock_pcap_close_stream_val_called = nullptr;

// Stub implementations of external C functions
extern "C" {
int open_serial(const char* modem_dev) {
    if (modem_dev) {
        strncpy(g_mock_open_serial_device_called, modem_dev, sizeof(g_mock_open_serial_device_called) - 1);
        g_mock_open_serial_device_called[sizeof(g_mock_open_serial_device_called) - 1] = '\0';
    } else {
        g_mock_open_serial_device_called[0] = '\0';
    }
    return g_mock_open_serial_return;
}

int init_serial(int fd, int baudrate, bool enable_flow_control) {
    g_mock_init_serial_fd_called = fd;
    g_mock_init_serial_baudrate_called = baudrate;
    g_mock_init_serial_flow_control_called = enable_flow_control;
    return g_mock_init_serial_return;
}

int close_serial(int fd) {
    g_mock_close_serial_fd_called = fd;
    return g_mock_close_serial_return;
}

int oasis_pcap_create(const char* filename, FILE** pcapstream_ptr) {
    if (filename) {
        strncpy(g_mock_pcap_create_filename_called, filename, sizeof(g_mock_pcap_create_filename_called) - 1);
         g_mock_pcap_create_filename_called[sizeof(g_mock_pcap_create_filename_called) - 1] = '\0';
    } else {
        g_mock_pcap_create_filename_called[0] = '\0';
    }

    if (g_mock_pcap_create_return_code == 0) {
        *pcapstream_ptr = g_mock_pcap_create_file_ptr_return; // Use the pre-set mock file pointer
        return 0;
    }
    *pcapstream_ptr = nullptr;
    return -1;
}

int oasis_pcap_close(FILE* pcapstream) {
    g_mock_pcap_close_stream_val_called = pcapstream;
    return g_mock_pcap_close_return_code;
}
} // extern "C"

// Helper to manage C-style argv for testing parsing functions
class ArgvManager {
public:
    ArgvManager(const std::vector<std::string>& args) : argv_(nullptr), argc_(0) {
        if (args.empty()) return;
        argc_ = static_cast<int>(args.size());
        argv_ = new char*[argc_];
        for (int i = 0; i < argc_; ++i) {
            argv_[i] = new char[args[i].length() + 1];
            strcpy(argv_[i], args[i].c_str());
        }
    }

    ~ArgvManager() {
        if (!argv_) return;
        for (int i = 0; i < argc_; ++i) {
            delete[] argv_[i];
        }
        delete[] argv_;
    }

    char** argv() const { return argv_; }
    int argc() const { return argc_; }

private:
    char** argv_;
    int argc_;

    // Disable copy and assign
    ArgvManager(const ArgvManager&) = delete;
    ArgvManager& operator=(const ArgvManager&) = delete;
};


class OasisTransferUtilsTest : public ::testing::Test {
protected:
    oasis_transfer_common_args_t common_args;
    oasis_transfer_session_t session;
    std::filesystem::path temp_dir_path;
    std::string temp_pcap_file_full_path;

    void SetUp() override {
        // Initialize common_args to defaults before each test
        strcpy(common_args.port_path, ""); // Explicitly clear
        common_args.quiet = 0;
        common_args.debug = 0;
        common_args.baud_rate = DEFAULT_BAUD_RATE;
        common_args.pacing_packet_ms = 0;
        common_args.pcap_filename[0] = '\0'; // Explicitly clear
        common_args.ascii_conversion = 0;
        common_args.flow_control = 1; // Default is enabled

        memset(&session, 0, sizeof(session));
        session.serial_fd = -1; // Indicate not open

        // Reset mock global variables
        g_mock_open_serial_return = -1;
        g_mock_open_serial_device_called[0] = '\0';
        g_mock_init_serial_return = -1;
        g_mock_init_serial_fd_called = -1;
        g_mock_init_serial_baudrate_called = 0;
        g_mock_init_serial_flow_control_called = false;
        g_mock_close_serial_return = 0;
        g_mock_close_serial_fd_called = -1;
        g_mock_pcap_create_file_ptr_return = nullptr;
        g_mock_pcap_create_filename_called[0] = '\0';
        g_mock_pcap_create_return_code = -1;
        g_mock_pcap_close_return_code = 0;
        g_mock_pcap_close_stream_val_called = nullptr;

        // Create a unique temporary directory for pcap files for this test run
        const ::testing::TestInfo* const test_info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        std::string unique_dir_name = std::string("OasisTransferUtilsTest_") +
                                       test_info->test_suite_name() + "_" +
                                       test_info->name();
        temp_dir_path = std::filesystem::temp_directory_path() / unique_dir_name;
        std::filesystem::create_directories(temp_dir_path);
        temp_pcap_file_full_path = (temp_dir_path / "test_temp.pcap").string();
    }

    void TearDown() override {
        // Clean up session if it was initialized (mainly for pcap_stream if real files were used by mocks)
        if (session.pcap_stream && session.pcap_stream != (FILE*)0x1 /* not dummy */) {
            fclose(session.pcap_stream);
            session.pcap_stream = nullptr;
        }
         std::error_code ec;
        std::filesystem::remove_all(temp_dir_path, ec);
        // Optionally log if ec indicates an error, but don't fail test for teardown issues
    }
};

// --- Tests for parse_one_common_option ---

TEST_F(OasisTransferUtilsTest, ParseOptionQuietShort) {
    ArgvManager argv_manager({"test_program", "-q"});
    int idx = 1;
    int result = parse_one_common_option(argv_manager.argc(), argv_manager.argv(), &idx, &common_args);
    EXPECT_EQ(1, result);
    EXPECT_TRUE(common_args.quiet);
    EXPECT_EQ(2, idx);
}

TEST_F(OasisTransferUtilsTest, ParseOptionDebugShort) {
    ArgvManager argv_manager({"test_program", "-d"});
    int idx = 1;
    int result = parse_one_common_option(argv_manager.argc(), argv_manager.argv(), &idx, &common_args);
    EXPECT_EQ(1, result);
    EXPECT_TRUE(common_args.debug);
    EXPECT_EQ(2, idx);
}

TEST_F(OasisTransferUtilsTest, ParseOptionAsciiShort) {
    ArgvManager argv_manager({"test_program", "-a"});
    int idx = 1;
    int result = parse_one_common_option(argv_manager.argc(), argv_manager.argv(), &idx, &common_args);
    EXPECT_EQ(1, result);
    EXPECT_TRUE(common_args.ascii_conversion);
    EXPECT_EQ(2, idx);
}

TEST_F(OasisTransferUtilsTest, ParseOptionAsciiLong) {
    ArgvManager argv_manager({"test_program", "--ascii"});
    int idx = 1;
    int result = parse_one_common_option(argv_manager.argc(), argv_manager.argv(), &idx, &common_args);
    EXPECT_EQ(1, result);
    EXPECT_TRUE(common_args.ascii_conversion);
    EXPECT_EQ(2, idx);
}

TEST_F(OasisTransferUtilsTest, ParseOptionFlowControlShort) {
    ArgvManager argv_manager({"test_program", "-f"}); // -f disables flow control
    int idx = 1;
    int result = parse_one_common_option(argv_manager.argc(), argv_manager.argv(), &idx, &common_args);
    EXPECT_EQ(1, result);
    EXPECT_FALSE(common_args.flow_control);
    EXPECT_EQ(2, idx);
}

TEST_F(OasisTransferUtilsTest, ParseOptionFlowControlLong) {
    ArgvManager argv_manager({"test_program", "--flow-control"}); // --flow-control disables it
    int idx = 1;
    int result = parse_one_common_option(argv_manager.argc(), argv_manager.argv(), &idx, &common_args);
    EXPECT_EQ(1, result);
    EXPECT_FALSE(common_args.flow_control);
    EXPECT_EQ(2, idx);
}


TEST_F(OasisTransferUtilsTest, ParseOptionBaudShortAttached) {
    ArgvManager argv_manager({"test_program", "-b9600"});
    int idx = 1;
    int result = parse_one_common_option(argv_manager.argc(), argv_manager.argv(), &idx, &common_args);
    EXPECT_EQ(1, result);
    EXPECT_EQ(9600, common_args.baud_rate);
    EXPECT_EQ(2, idx);
}

TEST_F(OasisTransferUtilsTest, ParseOptionBaudShortSeparate) {
    ArgvManager argv_manager({"test_program", "-b", "19200"});
    int idx = 1;
    int result = parse_one_common_option(argv_manager.argc(), argv_manager.argv(), &idx, &common_args);
    EXPECT_EQ(1, result);
    EXPECT_EQ(19200, common_args.baud_rate);
    EXPECT_EQ(3, idx);
}

TEST_F(OasisTransferUtilsTest, ParseOptionBaudMissingValue) {
    ArgvManager argv_manager({"test_program", "-b"});
    int idx = 1;
    int result = parse_one_common_option(argv_manager.argc(), argv_manager.argv(), &idx, &common_args);
    EXPECT_EQ(-1, result); // Error
}

TEST_F(OasisTransferUtilsTest, ParseOptionBaudInvalidValue) {
    ArgvManager argv_manager({"test_program", "-b", "xyz"});
    int idx = 1;
    int result = parse_one_common_option(argv_manager.argc(), argv_manager.argv(), &idx, &common_args);
    EXPECT_EQ(-1, result); // Error
}

TEST_F(OasisTransferUtilsTest, ParseOptionPcapLongAttached) {
    ArgvManager argv_manager({"test_program", "--pcap=out.pcap"});
    int idx = 1;
    int result = parse_one_common_option(argv_manager.argc(), argv_manager.argv(), &idx, &common_args);
    EXPECT_EQ(1, result);
    EXPECT_STREQ("out.pcap", common_args.pcap_filename);
    EXPECT_EQ(2, idx);
}

TEST_F(OasisTransferUtilsTest, ParseOptionPcapLongSeparate) {
    ArgvManager argv_manager({"test_program", "--pcap", "mycapture.pcap"});
    int idx = 1;
    int result = parse_one_common_option(argv_manager.argc(), argv_manager.argv(), &idx, &common_args);
    EXPECT_EQ(1, result);
    EXPECT_STREQ("mycapture.pcap", common_args.pcap_filename);
    EXPECT_EQ(3, idx);
}

TEST_F(OasisTransferUtilsTest, ParseOptionPacingPacketLongAttached) {
    ArgvManager argv_manager({"test_program", "--pacing-packet=10"});
    int idx = 1;
    int result = parse_one_common_option(argv_manager.argc(), argv_manager.argv(), &idx, &common_args);
    EXPECT_EQ(1, result);
    EXPECT_EQ(10, common_args.pacing_packet_ms);
    EXPECT_EQ(2, idx);
}

TEST_F(OasisTransferUtilsTest, ParseOptionPacingPacketLongSeparate) {
    ArgvManager argv_manager({"test_program", "--pacing-packet", "20"});
    int idx = 1;
    int result = parse_one_common_option(argv_manager.argc(), argv_manager.argv(), &idx, &common_args);
    EXPECT_EQ(1, result);
    EXPECT_EQ(20, common_args.pacing_packet_ms);
    EXPECT_EQ(3, idx);
}

TEST_F(OasisTransferUtilsTest, ParseOptionHelpShort) {
    ArgvManager argv_manager({"test_program", "-h"});
    int idx = 1;
    int result = parse_one_common_option(argv_manager.argc(), argv_manager.argv(), &idx, &common_args);
    EXPECT_EQ(-2, result); // Help requested
    EXPECT_EQ(2, idx);
}

TEST_F(OasisTransferUtilsTest, ParseOptionHelpLong) {
    ArgvManager argv_manager({"test_program", "--help"});
    int idx = 1;
    int result = parse_one_common_option(argv_manager.argc(), argv_manager.argv(), &idx, &common_args);
    EXPECT_EQ(-2, result); // Help requested
    EXPECT_EQ(2, idx);
}

TEST_F(OasisTransferUtilsTest, ParseNonOptionArgument) {
    ArgvManager argv_manager({"test_program", "filename.txt"});
    int idx = 1;
    int result = parse_one_common_option(argv_manager.argc(), argv_manager.argv(), &idx, &common_args);
    EXPECT_EQ(0, result); // Not a common option
    EXPECT_EQ(1, idx);    // Index unchanged
}

TEST_F(OasisTransferUtilsTest, ParseUnknownOption) {
    ArgvManager argv_manager({"test_program", "--unknown"});
    int idx = 1;
    int result = parse_one_common_option(argv_manager.argc(), argv_manager.argv(), &idx, &common_args);
    EXPECT_EQ(0, result); // Unknown long option returns 0
    EXPECT_EQ(1, idx);    // Index unchanged
}


// --- Tests for initialize_transfer_session & cleanup_transfer_session ---

TEST_F(OasisTransferUtilsTest, InitializeSessionSuccessNoPcap) {
    strcpy(common_args.port_path, "/dev/ttyMock1");
    common_args.baud_rate = 19200;
    common_args.flow_control = 1; // enabled
    common_args.pcap_filename[0] = '\0';

    g_mock_open_serial_return = 10; // Mock fd
    g_mock_init_serial_return = 0;  // Success

    ASSERT_EQ(0, initialize_transfer_session(&common_args, &session));
    EXPECT_EQ(10, session.serial_fd);
    EXPECT_STREQ("/dev/ttyMock1", g_mock_open_serial_device_called);
    EXPECT_EQ(10, g_mock_init_serial_fd_called);
    EXPECT_EQ(19200, g_mock_init_serial_baudrate_called);
    EXPECT_TRUE(g_mock_init_serial_flow_control_called);
    EXPECT_EQ(nullptr, session.pcap_stream);

    cleanup_transfer_session(&session);
    EXPECT_EQ(10, g_mock_close_serial_fd_called); // Check close_serial was called with correct fd
    EXPECT_EQ(-1, session.serial_fd); // fd should be reset
}

TEST_F(OasisTransferUtilsTest, InitializeSessionSuccessWithPcap) {
    strcpy(common_args.port_path, "COM1");
    strcpy(common_args.pcap_filename, temp_pcap_file_full_path.c_str());

    g_mock_open_serial_return = 12;
    g_mock_init_serial_return = 0;
    g_mock_pcap_create_return_code = 0;
    g_mock_pcap_create_file_ptr_return = (FILE*)0x1; // Dummy non-NULL FILE*

    ASSERT_EQ(0, initialize_transfer_session(&common_args, &session));
    EXPECT_EQ(12, session.serial_fd);
    EXPECT_NE(nullptr, session.pcap_stream);
    EXPECT_EQ((FILE*)0x1, session.pcap_stream);
    EXPECT_STREQ(temp_pcap_file_full_path.c_str(), g_mock_pcap_create_filename_called);

    cleanup_transfer_session(&session);
    EXPECT_EQ(12, g_mock_close_serial_fd_called);
    EXPECT_EQ((FILE*)0x1, g_mock_pcap_close_stream_val_called);
    EXPECT_EQ(-1, session.serial_fd);
    EXPECT_EQ(nullptr, session.pcap_stream);
}

TEST_F(OasisTransferUtilsTest, InitializeSessionOpenSerialFails) {
    strcpy(common_args.port_path, "/dev/ttyFail");
    g_mock_open_serial_return = -1; // Simulate failure

    EXPECT_EQ(-1, initialize_transfer_session(&common_args, &session));
    EXPECT_EQ(-1, session.serial_fd);
    EXPECT_EQ(nullptr, session.pcap_stream);
}

TEST_F(OasisTransferUtilsTest, InitializeSessionInitSerialFails) {
    strcpy(common_args.port_path, "/dev/ttyGoodOpen");
    g_mock_open_serial_return = 15;    // open_serial succeeds
    g_mock_init_serial_return = -1;   // init_serial fails

    EXPECT_EQ(-1, initialize_transfer_session(&common_args, &session));
    EXPECT_EQ(-1, session.serial_fd); // fd should be reset
    EXPECT_EQ(nullptr, session.pcap_stream);
    EXPECT_EQ(15, g_mock_close_serial_fd_called); // Ensure close_serial was called on the opened fd
}

TEST_F(OasisTransferUtilsTest, InitializeSessionPcapCreateFailsButSessionInitializes) {
    strcpy(common_args.port_path, "COM_OK");
    strcpy(common_args.pcap_filename, temp_pcap_file_full_path.c_str());
    g_mock_pcap_create_return_code = -1; // pcap_create fails

    g_mock_open_serial_return = 18; // Serial parts succeed
    g_mock_init_serial_return = 0;

    // Function prints a warning but does not return -1 for PCAP creation failure alone
    ASSERT_EQ(0, initialize_transfer_session(&common_args, &session));
    EXPECT_EQ(18, session.serial_fd);
    EXPECT_EQ(nullptr, session.pcap_stream); // pcap_stream should be NULL
    EXPECT_STREQ(temp_pcap_file_full_path.c_str(), g_mock_pcap_create_filename_called);

    cleanup_transfer_session(&session);
}

TEST_F(OasisTransferUtilsTest, CleanupSessionNull) {
    EXPECT_NO_THROW(cleanup_transfer_session(nullptr));
}

TEST_F(OasisTransferUtilsTest, CleanupUninitializedSession) {
    // session is zero-initialized by default in fixture, serial_fd is -1
    EXPECT_NO_THROW(cleanup_transfer_session(&session));
    EXPECT_EQ(-1, g_mock_close_serial_fd_called); // close_serial should not be called if fd is -1
    EXPECT_EQ(nullptr, g_mock_pcap_close_stream_val_called);
}


// --- Tests for sleep_ms_util ---
TEST_F(OasisTransferUtilsTest, SleepMsUtilZero) {
    // Primarily a "does not crash" test.
    EXPECT_NO_THROW(sleep_ms_util(0));
}

TEST_F(OasisTransferUtilsTest, SleepMsUtilNegative) {
    EXPECT_NO_THROW(sleep_ms_util(-100));
}

TEST_F(OasisTransferUtilsTest, SleepMsUtilPositive) {
    // Verifying actual sleep duration is complex and flaky in unit tests.
    // This just ensures it can be called.
    EXPECT_NO_THROW(sleep_ms_util(1));
}
