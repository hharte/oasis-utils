/* tests/test_oasis_ascii.cpp */
#include "gtest/gtest.h"
#include "oasis.h"       /* Functions to be tested */
#include <vector>        /* For buffer management */
#include <string>        /* For std::string */
#include <cstring>       /* For memcmp, memset, strlen */
#include <fstream>       /* For std::ofstream, std::ifstream */
#include <filesystem>    /* For std::filesystem paths and operations */
#include <cstdio>        /* For remove, tmpnam related constants if needed */

/* Platform-specific line endings for test verification */
#ifdef _WIN32
#define NATIVE_HOST_LINE_ENDING "\r\n"
#define NATIVE_HOST_LINE_ENDING_LEN 2
#else
#define NATIVE_HOST_LINE_ENDING "\n"
#define NATIVE_HOST_LINE_ENDING_LEN 1
#endif
#define OASIS_SUB_CHAR_TEST (0x1A) /* SUB character */
#define OASIS_LINE_ENDING_CHAR '\r'
#define OASIS_LINE_ENDING_STR "\r"
#define OASIS_LINE_ENDING_STR_LEN 1


class OasisAsciiTest : public ::testing::Test {
protected:
    std::vector<uint8_t> input_buf;
    std::vector<uint8_t> output_buf;
    conversion_result_t conv_result;
    static const size_t DEFAULT_OUTPUT_BUF_SIZE = 256; /* Define a constant for clarity */

    void SetUp() override {
        output_buf.resize(DEFAULT_OUTPUT_BUF_SIZE);
        memset(output_buf.data(), 0xAA, output_buf.size()); /* Fill for overflow check */
        memset(&conv_result, 0, sizeof(conversion_result_t));
    }

    /* Helper to set input buffer from a C-string */
    void SetInput(const char* str) {
        if (str) {
            input_buf.assign(str, str + strlen(str));
        }
        else {
            input_buf.clear();
        }
    }

    /* Helper to set input buffer from a std::vector<uint8_t> */
    void SetInput(const std::vector<uint8_t>& vec) {
        input_buf = vec;
    }
};

/* --- Tests for ascii_oasis_to_host (buffer to buffer) --- */

TEST_F(OasisAsciiTest, OasisToHostBasic) {
    SetInput("Line 1\rLine 2\rLast Line"); /* OASIS format (CR) */
    int32_t written = ascii_oasis_to_host(input_buf.data(), input_buf.size(),
        output_buf.data(), output_buf.size(), &conv_result);

    std::string expected_host_str_val = std::string("Line 1") + NATIVE_HOST_LINE_ENDING + "Line 2" + NATIVE_HOST_LINE_ENDING + "Last Line";
    ASSERT_EQ(written, static_cast<int32_t>(expected_host_str_val.length()));
    EXPECT_EQ(conv_result.output_chars, expected_host_str_val.length());
    ASSERT_GE(output_buf.size(), expected_host_str_val.length());
    EXPECT_EQ(memcmp(output_buf.data(), expected_host_str_val.c_str(), expected_host_str_val.length()), 0);
    EXPECT_EQ(conv_result.output_lines, static_cast<size_t>(3));
    EXPECT_EQ(conv_result.max_line_len, static_cast<size_t>(9)); /* "Last Line" */
}

TEST_F(OasisAsciiTest, OasisToHostWithSUB) {
    std::string oasis_content_str = "First part\rBeforeSUB";
    oasis_content_str += OASIS_SUB_CHAR_TEST;
    oasis_content_str += "AfterSUB\rThis part ignored";
    SetInput(oasis_content_str.c_str());

    int32_t written = ascii_oasis_to_host(input_buf.data(), input_buf.size(),
        output_buf.data(), output_buf.size(), &conv_result);

    std::string expected_host_str_val = std::string("First part") + NATIVE_HOST_LINE_ENDING + "BeforeSUB";
    ASSERT_EQ(written, static_cast<int32_t>(expected_host_str_val.length()));
    EXPECT_EQ(conv_result.output_chars, expected_host_str_val.length());
    ASSERT_GE(output_buf.size(), expected_host_str_val.length());
    EXPECT_EQ(memcmp(output_buf.data(), expected_host_str_val.c_str(), expected_host_str_val.length()), 0);
    EXPECT_EQ(conv_result.output_lines, static_cast<size_t>(2));
    EXPECT_EQ(conv_result.max_line_len, static_cast<size_t>(10)); /* "First part" or "BeforeSUB" */
}

TEST_F(OasisAsciiTest, OasisToHostSubAtStart) {
    std::string oasis_content_str;
    oasis_content_str += OASIS_SUB_CHAR_TEST;
    oasis_content_str += "This should be ignored\rLine2";
    SetInput(oasis_content_str.c_str());

    int32_t written = ascii_oasis_to_host(input_buf.data(), input_buf.size(),
        output_buf.data(), output_buf.size(), &conv_result);
    ASSERT_EQ(written, 0);
    EXPECT_EQ(conv_result.output_chars, static_cast<size_t>(0));
    EXPECT_EQ(conv_result.output_lines, static_cast<size_t>(0));
    EXPECT_EQ(conv_result.max_line_len, static_cast<size_t>(0));
}

TEST_F(OasisAsciiTest, OasisToHostOnlySub) {
    std::string oasis_content_str;
    oasis_content_str += OASIS_SUB_CHAR_TEST;
    SetInput(oasis_content_str.c_str());
    int32_t written = ascii_oasis_to_host(input_buf.data(), input_buf.size(),
        output_buf.data(), output_buf.size(), &conv_result);
    ASSERT_EQ(written, 0);
}


/* --- Tests for ascii_host_to_oasis (buffer to buffer) --- */

TEST_F(OasisAsciiTest, HostToOasisBasic) {
    std::string host_content_str = std::string("Line 1") + NATIVE_HOST_LINE_ENDING + "Line 2" + NATIVE_HOST_LINE_ENDING + "Last Line";
    SetInput(host_content_str.c_str());

    const char* expected_oasis_str = "Line 1\rLine 2\rLast Line";
    int32_t written = ascii_host_to_oasis(input_buf.data(), input_buf.size(),
        output_buf.data(), output_buf.size(), &conv_result);

    ASSERT_EQ(written, static_cast<int32_t>(strlen(expected_oasis_str)));
    EXPECT_EQ(conv_result.output_chars, strlen(expected_oasis_str));
    ASSERT_GE(output_buf.size(), strlen(expected_oasis_str));
    EXPECT_EQ(memcmp(output_buf.data(), expected_oasis_str, strlen(expected_oasis_str)), 0);
    EXPECT_EQ(conv_result.output_lines, static_cast<size_t>(3));
    EXPECT_EQ(conv_result.max_line_len, static_cast<size_t>(9)); /* "Last Line" */
}

#ifdef _WIN32
TEST_F(OasisAsciiTest, HostToOasisWinSpecificLoneLF) {
    SetInput("LineA\nLineB"); /* Lone LF on Windows */
    const char* expected_oasis_str = "LineA\rLineB";
    int32_t written = ascii_host_to_oasis(input_buf.data(), input_buf.size(),
        output_buf.data(), output_buf.size(), &conv_result);
    ASSERT_EQ(written, static_cast<int32_t>(strlen(expected_oasis_str)));
    EXPECT_EQ(memcmp(output_buf.data(), expected_oasis_str, strlen(expected_oasis_str)), 0);
    EXPECT_EQ(conv_result.output_lines, 2);
}

TEST_F(OasisAsciiTest, HostToOasisWinSpecificLoneCR) {
    SetInput("LineC\rLineD"); /* Lone CR on Windows */
    const char* expected_oasis_str = "LineC\rLineD";
    int32_t written = ascii_host_to_oasis(input_buf.data(), input_buf.size(),
        output_buf.data(), output_buf.size(), &conv_result);
    ASSERT_EQ(written, static_cast<int32_t>(strlen(expected_oasis_str)));
    EXPECT_EQ(memcmp(output_buf.data(), expected_oasis_str, strlen(expected_oasis_str)), 0);
    EXPECT_EQ(conv_result.output_lines, 2);
}
#endif

/* --- Common Buffer Tests (Empty, BufferTooSmall, NullPointer, TrailingChars, OnlyNewlines) --- */
/* These are mostly the same as before, just verifying they still pass. */

TEST_F(OasisAsciiTest, EmptyInput) {
    SetInput("");
    int32_t written_o2h = ascii_oasis_to_host(input_buf.data(), input_buf.size(),
        output_buf.data(), output_buf.size(), &conv_result);
    EXPECT_EQ(written_o2h, 0);
    EXPECT_EQ(conv_result.output_chars, static_cast<size_t>(0));
    EXPECT_EQ(conv_result.output_lines, static_cast<size_t>(0));
    EXPECT_EQ(conv_result.max_line_len, static_cast<size_t>(0));

    memset(&conv_result, 0, sizeof(conversion_result_t));
    int32_t written_h2o = ascii_host_to_oasis(input_buf.data(), input_buf.size(),
        output_buf.data(), output_buf.size(), &conv_result);
    EXPECT_EQ(written_h2o, 0);
    EXPECT_EQ(conv_result.output_chars, static_cast<size_t>(0));
    EXPECT_EQ(conv_result.output_lines, static_cast<size_t>(0));
    EXPECT_EQ(conv_result.max_line_len, static_cast<size_t>(0));
}

TEST_F(OasisAsciiTest, BufferTooSmallOasisToHost) {
    SetInput("Line 1\rLine 2");
    const size_t small_buffer_size = 7;
    std::vector<uint8_t> small_out_buf(small_buffer_size);

    int32_t written = ascii_oasis_to_host(input_buf.data(), input_buf.size(),
        small_out_buf.data(), small_out_buf.size(),
        &conv_result);
    EXPECT_EQ(written, OASIS_ERR_BUFFER_TOO_SMALL);

#ifdef _WIN32 /* Expects CRLF (2 bytes) */
    /* "Line 1" (6) + CRLF (2) = 8. Capacity 7. Writes "Line 1". output_chars=6. */
    EXPECT_EQ(conv_result.output_chars, 6);
    if (conv_result.output_chars == 6) {
        EXPECT_EQ(memcmp(small_out_buf.data(), "Line 1", 6), 0);
    }
#else /* Expects LF (1 byte) */
    /* "Line 1" (6) + LF (1) = 7. Capacity 7. Writes "Line 1\n". output_chars=7. */
    EXPECT_EQ(conv_result.output_chars, static_cast<size_t>(7));
    if (conv_result.output_chars == 7) {
        EXPECT_EQ(memcmp(small_out_buf.data(), "Line 1\n", static_cast<size_t>(7)), 0);
    }
#endif
}

TEST_F(OasisAsciiTest, BufferTooSmallHostToOasis) {
    std::string host_input_str = std::string("Line 1") + NATIVE_HOST_LINE_ENDING + "Line 2";
    SetInput(host_input_str.c_str());

    const size_t small_buffer_size = 7;
    std::vector<uint8_t> small_out_buf(small_buffer_size);

    int32_t written = ascii_host_to_oasis(input_buf.data(), input_buf.size(),
        small_out_buf.data(), small_out_buf.size(),
        &conv_result);
    EXPECT_EQ(written, OASIS_ERR_BUFFER_TOO_SMALL);
    /* "Line 1" (6) + CR (1) = 7. Capacity 7. Writes "Line 1\r". output_chars=7. */
    EXPECT_EQ(conv_result.output_chars, static_cast<size_t>(7));
    if (conv_result.output_chars == 7) {
        EXPECT_EQ(memcmp(small_out_buf.data(), "Line 1\r", static_cast<size_t>(7)), 0);
    }
}


TEST_F(OasisAsciiTest, IsAsciiFunction) {
    SetInput("Hello World 123!@#");
    EXPECT_EQ(is_ascii(input_buf.data(), input_buf.size()), 1);

    std::vector<uint8_t> non_ascii_input = { 'H', 0x80, 'i' };
    EXPECT_EQ(is_ascii(non_ascii_input.data(), non_ascii_input.size()), 0);

    SetInput("");
    EXPECT_EQ(is_ascii(input_buf.data(), input_buf.size()), 1);

    EXPECT_EQ(is_ascii(nullptr, 0), 1);
    EXPECT_EQ(is_ascii(nullptr, 5), 0);
}

TEST_F(OasisAsciiTest, NullPointerArgs) {
    SetInput("test");
    EXPECT_EQ(ascii_oasis_to_host(input_buf.data(), 1, nullptr, 10, &conv_result), OASIS_ERR_NULL_POINTER);
    EXPECT_EQ(ascii_host_to_oasis(input_buf.data(), 1, nullptr, 10, &conv_result), OASIS_ERR_NULL_POINTER);

    EXPECT_EQ(ascii_oasis_to_host(input_buf.data(), 1, output_buf.data(), 10, nullptr), OASIS_ERR_NULL_POINTER);
    EXPECT_EQ(ascii_host_to_oasis(input_buf.data(), 1, output_buf.data(), 10, nullptr), OASIS_ERR_NULL_POINTER);

    EXPECT_EQ(ascii_oasis_to_host(nullptr, 1, output_buf.data(), 10, &conv_result), OASIS_ERR_NULL_POINTER);
    EXPECT_EQ(ascii_host_to_oasis(nullptr, 1, output_buf.data(), 10, &conv_result), OASIS_ERR_NULL_POINTER);
}


TEST_F(OasisAsciiTest, OasisToHostTrailingCharsNoNewline) {
    SetInput("Last Line");
    int32_t written = ascii_oasis_to_host(input_buf.data(), input_buf.size(),
        output_buf.data(), output_buf.size(), &conv_result);
    const char* expected_host_str = "Last Line";
    ASSERT_EQ(written, static_cast<int32_t>(strlen(expected_host_str)));
    EXPECT_EQ(conv_result.output_chars, strlen(expected_host_str));
    ASSERT_GE(output_buf.size(), strlen(expected_host_str));
    EXPECT_EQ(memcmp(output_buf.data(), expected_host_str, strlen(expected_host_str)), 0);
    EXPECT_EQ(conv_result.output_lines, static_cast<size_t>(1));
    EXPECT_EQ(conv_result.max_line_len, static_cast<size_t>(9));
}

TEST_F(OasisAsciiTest, HostToOasisTrailingCharsNoNewline) {
    SetInput("Last Line");
    const char* expected_oasis_str = "Last Line";
    int32_t written = ascii_host_to_oasis(input_buf.data(), input_buf.size(),
        output_buf.data(), output_buf.size(), &conv_result);

    ASSERT_EQ(written, static_cast<int32_t>(strlen(expected_oasis_str)));
    EXPECT_EQ(conv_result.output_chars, strlen(expected_oasis_str));
    ASSERT_GE(output_buf.size(), strlen(expected_oasis_str));
    EXPECT_EQ(memcmp(output_buf.data(), expected_oasis_str, strlen(expected_oasis_str)), 0);
    EXPECT_EQ(conv_result.output_lines, static_cast<size_t>(1));
    EXPECT_EQ(conv_result.max_line_len, static_cast<size_t>(9));
}

TEST_F(OasisAsciiTest, OnlyNewlinesOasisToHost) {
    SetInput("\r\r\r");
    int32_t written = ascii_oasis_to_host(input_buf.data(), input_buf.size(),
        output_buf.data(), output_buf.size(), &conv_result);
    std::string expected_host_str_val = std::string(NATIVE_HOST_LINE_ENDING) + NATIVE_HOST_LINE_ENDING + NATIVE_HOST_LINE_ENDING;
    ASSERT_EQ(written, static_cast<int32_t>(expected_host_str_val.length()));
    ASSERT_GE(output_buf.size(), expected_host_str_val.length());
    EXPECT_EQ(memcmp(output_buf.data(), expected_host_str_val.c_str(), expected_host_str_val.length()), 0);
    EXPECT_EQ(conv_result.output_lines, static_cast<size_t>(3));
    EXPECT_EQ(conv_result.max_line_len, static_cast<size_t>(0));
}

TEST_F(OasisAsciiTest, OnlyNewlinesHostToOasis) {
    std::string host_newlines = std::string(NATIVE_HOST_LINE_ENDING) + NATIVE_HOST_LINE_ENDING + NATIVE_HOST_LINE_ENDING;
    SetInput(host_newlines.c_str());

    const char* expected_oasis_str = "\r\r\r";
    int32_t written = ascii_host_to_oasis(input_buf.data(), input_buf.size(),
        output_buf.data(), output_buf.size(), &conv_result);

    ASSERT_EQ(written, static_cast<int32_t>(strlen(expected_oasis_str)));
    ASSERT_GE(output_buf.size(), strlen(expected_oasis_str));
    EXPECT_EQ(memcmp(output_buf.data(), expected_oasis_str, strlen(expected_oasis_str)), 0);
    EXPECT_EQ(conv_result.output_lines, static_cast<size_t>(3));
    EXPECT_EQ(conv_result.max_line_len, static_cast<size_t>(0));
}


/* --- Test Fixture and Tests for oasis_ascii_file_to_host_file --- */
class OasisAsciiFileTest : public ::testing::Test {
protected:
    std::filesystem::path temp_dir;
    std::string input_filename_str;
    std::string output_filename_str;

    void SetUp() override {
        /* Create a unique temporary directory for test files */
        temp_dir = std::filesystem::temp_directory_path() / "oasis_ascii_tests";
        std::filesystem::remove_all(temp_dir); /* Clean up from previous runs */
        std::filesystem::create_directory(temp_dir);

        input_filename_str = (temp_dir / "input.txt").string();
        output_filename_str = (temp_dir / "output.txt").string();
    }

    void TearDown() override {
        std::filesystem::remove_all(temp_dir); /* Clean up all files and the directory */
    }

    /* Helper to create a file with specific content */
    bool CreateFileWithContent(const std::string& filename, const std::string& content) {
        std::ofstream outfile(filename, std::ios::binary);
        if (!outfile) return false;
        outfile.write(content.data(), content.length());
        return outfile.good();
    }
    /* Overload for vector<uint8_t> */
    bool CreateFileWithContent(const std::string& filename, const std::vector<uint8_t>& content) {
        std::ofstream outfile(filename, std::ios::binary);
        if (!outfile) return false;
        outfile.write(reinterpret_cast<const char*>(content.data()), content.size());
        return outfile.good();
    }


    /* Helper to read file content into a string */
    std::string ReadFileContent(const std::string& filename) {
        std::ifstream infile(filename, std::ios::binary | std::ios::ate);
        if (!infile) return "";
        std::streamsize size = infile.tellg();
        infile.seekg(0, std::ios::beg);
        std::string buffer(size, '\0');
        if (infile.read(&buffer[0], size)) {
            return buffer;
        }
        return "";
    }
};

TEST_F(OasisAsciiFileTest, ConvertToNewFile) {
    std::string oasis_content = "Line1\rLine2\rEnd";
    ASSERT_TRUE(CreateFileWithContent(input_filename_str, oasis_content));

    int result = oasis_ascii_file_to_host_file(input_filename_str.c_str(), output_filename_str.c_str());
    ASSERT_EQ(result, 0);

    std::string expected_host_content = std::string("Line1") + NATIVE_HOST_LINE_ENDING + "Line2" + NATIVE_HOST_LINE_ENDING + "End";
    std::string actual_output_content = ReadFileContent(output_filename_str);
    EXPECT_EQ(actual_output_content, expected_host_content);
}

TEST_F(OasisAsciiFileTest, ConvertInPlace) {
    std::string oasis_content = "InPlace1\rInPlace2";
    ASSERT_TRUE(CreateFileWithContent(input_filename_str, oasis_content));

    int result = oasis_ascii_file_to_host_file(input_filename_str.c_str(), nullptr);
    ASSERT_EQ(result, 0);

    std::string expected_host_content = std::string("InPlace1") + NATIVE_HOST_LINE_ENDING + "InPlace2";
    std::string actual_input_content_after = ReadFileContent(input_filename_str);
    EXPECT_EQ(actual_input_content_after, expected_host_content);
}

TEST_F(OasisAsciiFileTest, EmptyInputFileToNew) {
    ASSERT_TRUE(CreateFileWithContent(input_filename_str, ""));
    int result = oasis_ascii_file_to_host_file(input_filename_str.c_str(), output_filename_str.c_str());
    ASSERT_EQ(result, 0);
    EXPECT_EQ(ReadFileContent(output_filename_str), "");
    EXPECT_TRUE(std::filesystem::exists(output_filename_str));
}

TEST_F(OasisAsciiFileTest, EmptyInputFileInPlace) {
    ASSERT_TRUE(CreateFileWithContent(input_filename_str, ""));
    int result = oasis_ascii_file_to_host_file(input_filename_str.c_str(), nullptr);
    ASSERT_EQ(result, 0);
    EXPECT_EQ(ReadFileContent(input_filename_str), "");
}

TEST_F(OasisAsciiFileTest, FileWithOnlyCRs) {
    std::string oasis_content = "\r\r\r";
    ASSERT_TRUE(CreateFileWithContent(input_filename_str, oasis_content));
    int result = oasis_ascii_file_to_host_file(input_filename_str.c_str(), output_filename_str.c_str());
    ASSERT_EQ(result, 0);
    std::string expected_host_content = std::string(NATIVE_HOST_LINE_ENDING) + NATIVE_HOST_LINE_ENDING + NATIVE_HOST_LINE_ENDING;
    EXPECT_EQ(ReadFileContent(output_filename_str), expected_host_content);
}

TEST_F(OasisAsciiFileTest, FileWithNoCRs) {
    std::string oasis_content = "Single long line without CR.";
    ASSERT_TRUE(CreateFileWithContent(input_filename_str, oasis_content));
    int result = oasis_ascii_file_to_host_file(input_filename_str.c_str(), output_filename_str.c_str());
    ASSERT_EQ(result, 0);
    EXPECT_EQ(ReadFileContent(output_filename_str), oasis_content); /* Should remain unchanged */
}

TEST_F(OasisAsciiFileTest, FileWithSUBCharacter) {
    std::string oasis_content_str = "Valid part\rDataBeforeSUB";
    oasis_content_str += OASIS_SUB_CHAR_TEST; /* SUB */
    oasis_content_str += "This part is ignored\rMore ignored.";
    ASSERT_TRUE(CreateFileWithContent(input_filename_str, oasis_content_str));

    int result = oasis_ascii_file_to_host_file(input_filename_str.c_str(), output_filename_str.c_str());
    ASSERT_EQ(result, 0);

    std::string expected_host_content = std::string("Valid part") + NATIVE_HOST_LINE_ENDING + "DataBeforeSUB";
    EXPECT_EQ(ReadFileContent(output_filename_str), expected_host_content);
}

TEST_F(OasisAsciiFileTest, FileWithOnlySUBCharacter) {
    std::string oasis_content_str;
    oasis_content_str += OASIS_SUB_CHAR_TEST;
    ASSERT_TRUE(CreateFileWithContent(input_filename_str, oasis_content_str));
    int result = oasis_ascii_file_to_host_file(input_filename_str.c_str(), output_filename_str.c_str());
    ASSERT_EQ(result, 0);
    EXPECT_EQ(ReadFileContent(output_filename_str), "");
}

TEST_F(OasisAsciiFileTest, InputFileNonASCII) {
    std::vector<uint8_t> non_ascii_content = { 'H', 'e', 'l', 'l', 0x80, 'o' };
    ASSERT_TRUE(CreateFileWithContent(input_filename_str, non_ascii_content));

    int result = oasis_ascii_file_to_host_file(input_filename_str.c_str(), output_filename_str.c_str());
    EXPECT_EQ(result, OASIS_ERR_INVALID_INPUT);
    EXPECT_FALSE(std::filesystem::exists(output_filename_str)); /* Output file should not be created */
}

TEST_F(OasisAsciiFileTest, InputFileNonASCIIInPlace) {
    std::vector<uint8_t> non_ascii_content = { 'B', 'a', 0xE4, 'd' };
    ASSERT_TRUE(CreateFileWithContent(input_filename_str, non_ascii_content));
    std::string original_content_str(non_ascii_content.begin(), non_ascii_content.end());

    int result = oasis_ascii_file_to_host_file(input_filename_str.c_str(), nullptr);
    EXPECT_EQ(result, OASIS_ERR_INVALID_INPUT);
    /* Input file should remain untouched on error before write */
    EXPECT_EQ(ReadFileContent(input_filename_str), original_content_str);
}


TEST_F(OasisAsciiFileTest, InputFileDoesNotExist) {
    int result = oasis_ascii_file_to_host_file("non_existent_input.txt", output_filename_str.c_str());
    EXPECT_EQ(result, OASIS_ERR_FILE_IO);
}

TEST_F(OasisAsciiFileTest, OutputFileCannotBeCreated) {
    std::string oasis_content = "Test data.";
    ASSERT_TRUE(CreateFileWithContent(input_filename_str, oasis_content));
    /* Assuming temp_dir is writable, make a subdirectory non-writable or a file path that's invalid */
    std::string invalid_output_path = (temp_dir / "restricted_dir/output.txt").string();
    /* For simplicity, just use a path that's unlikely to be creatable by fopen in "wb"
       if the directory doesn't exist. fopen won't create intermediate dirs.
    */
    int result = oasis_ascii_file_to_host_file(input_filename_str.c_str(), invalid_output_path.c_str());
    EXPECT_EQ(result, OASIS_ERR_FILE_IO);
}

TEST_F(OasisAsciiFileTest, InputFileNameIsNull) {
    int result = oasis_ascii_file_to_host_file(nullptr, output_filename_str.c_str());
    EXPECT_EQ(result, OASIS_ERR_NULL_POINTER);
}

TEST_F(OasisAsciiFileTest, InPlaceConversionShortensContent) {
    std::string initial_oasis_content = "Line 1 is very long to ensure it is longer\rLine 2 also long";
    std::string final_oasis_content_after_sub = "Short\rDone";
    std::string full_oasis_content = initial_oasis_content;
    full_oasis_content += OASIS_SUB_CHAR_TEST;
    full_oasis_content += "This is ignored after SUB";

    ASSERT_TRUE(CreateFileWithContent(input_filename_str, full_oasis_content));
    long original_size = (long)std::filesystem::file_size(input_filename_str);

    int result = oasis_ascii_file_to_host_file(input_filename_str.c_str(), nullptr);
    ASSERT_EQ(result, 0);

    std::string expected_host_content = std::string("Line 1 is very long to ensure it is longer") + NATIVE_HOST_LINE_ENDING + "Line 2 also long";
    std::string actual_content = ReadFileContent(input_filename_str);
    EXPECT_EQ(actual_content, expected_host_content);
    EXPECT_LT(std::filesystem::file_size(input_filename_str), static_cast<uintmax_t>(original_size));
    EXPECT_EQ(std::filesystem::file_size(input_filename_str), expected_host_content.length());
}

TEST_F(OasisAsciiFileTest, InPlaceConversionLengthensContent) {
    std::string oasis_content = "L1\rL2\rL3"; /* Short OASIS lines */
    ASSERT_TRUE(CreateFileWithContent(input_filename_str, oasis_content));
    long original_size = (long)std::filesystem::file_size(input_filename_str);
    /* Original size for "L1\rL2\rL3" is 2+1+2+1+2 = 8 bytes. */
    ASSERT_EQ(original_size, (long)(2 + OASIS_LINE_ENDING_STR_LEN + 2 + OASIS_LINE_ENDING_STR_LEN + 2));


    int result = oasis_ascii_file_to_host_file(input_filename_str.c_str(), nullptr);
    ASSERT_EQ(result, 0);

    std::string expected_host_content = std::string("L1") + NATIVE_HOST_LINE_ENDING +
        std::string("L2") + NATIVE_HOST_LINE_ENDING +
        std::string("L3");
    std::string actual_content = ReadFileContent(input_filename_str);
    EXPECT_EQ(actual_content, expected_host_content);

    /*
     * The file size should only be greater if the native host line ending
     * is longer than the OASIS line ending.
     * NATIVE_HOST_LINE_ENDING_LEN is 2 for Windows ("\r\n"), 1 for Linux/macOS ("\n").
     * OASIS_LINE_ENDING_STR_LEN is 1 for "\r".
     */
    if constexpr(NATIVE_HOST_LINE_ENDING_LEN > OASIS_LINE_ENDING_STR_LEN) {
        EXPECT_GT(std::filesystem::file_size(input_filename_str), static_cast<uintmax_t>(original_size))
            << "File size should increase when host line endings (" << NATIVE_HOST_LINE_ENDING_LEN
            << " bytes) are longer than OASIS endings (" << OASIS_LINE_ENDING_STR_LEN << " byte).";
    }
    else if constexpr(NATIVE_HOST_LINE_ENDING_LEN == OASIS_LINE_ENDING_STR_LEN) {
        EXPECT_EQ(std::filesystem::file_size(input_filename_str), static_cast<uintmax_t>(original_size))
            << "File size should remain the same when host line endings (" << NATIVE_HOST_LINE_ENDING_LEN
            << " byte) are the same length as OASIS endings (" << OASIS_LINE_ENDING_STR_LEN << " byte).";
    }
    else { /* NATIVE_HOST_LINE_ENDING_LEN < OASIS_LINE_ENDING_STR_LEN */
        /* This case is not expected with current definitions but included for completeness. */
        EXPECT_LT(std::filesystem::file_size(input_filename_str), static_cast<uintmax_t>(original_size))
            << "File size should decrease if host line endings were shorter than OASIS endings.";
    }

    EXPECT_EQ(std::filesystem::file_size(input_filename_str), expected_host_content.length());
}
