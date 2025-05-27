/* test_oasis_sendrecv.cpp */
/* GTest unit tests for functions in oasis_sendrecv.c */

#include "gtest/gtest.h"
#include <vector>
#include <string>
#include <cstring> /* For memcpy, memset */
#include <cerrno>  /* For errno constants */
#include <iomanip> /* For std::hex */
#include <iostream> /* For debug printing in VerifyOutput */

/* DUT headers */
#include "oasis.h"
#include "oasis_sendrecv.h"

/* Helper to create a raw "packet" (DLE STX CMD + payload) for encoding tests */
/* This is the format oasis_packet_encode expects as input */
static std::vector<uint8_t> create_unencoded_packet_data(uint8_t cmd, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> data;
    data.push_back(DLE);
    data.push_back(STX);
    data.push_back(cmd);
    data.insert(data.end(), payload.begin(), payload.end());
    return data;
}

class OasisSendRecvTest : public ::testing::Test {
protected:
    /* Buffers for encoding/decoding */
    std::vector<uint8_t> in_buf;
    std::vector<uint8_t> out_buf;
    std::vector<uint8_t> expected_buf;
    uint16_t out_len;

    /* Max buffer sizes based on typical usage in oasis_sendrecv.c */
    /* Max payload for OPEN/WRITE/CLOSE is 512 bytes. */
    static const size_t MAX_PAYLOAD_SIZE = 512;
    static const size_t UNENCODED_HEADER_SIZE = 3; /* DLE STX CMD */
    /* Encoded can be up to 2x payload due to DLE stuffing, plus header, plus DLE ETX LRC RUB trailer (4). */
    static const size_t ENCODED_TRAILER_SIZE = 4; /* DLE ETX LRC RUB */
    /* Max size for out_buf in encode tests and in_buf for decode tests (test harness buffers) */
    static const size_t MAX_ENCODED_BUF_SIZE_FOR_TEST = UNENCODED_HEADER_SIZE + (MAX_PAYLOAD_SIZE * 2) + ENCODED_TRAILER_SIZE + 200; /* Extra safety for test harness */
    /* Max size for decoded payload (test harness buffer) */
    static const size_t MAX_DECODED_PAYLOAD_BUF_SIZE_FOR_TEST = MAX_PAYLOAD_SIZE + 100; /* Extra safety for test harness */


    void SetUp() override {
        out_len = 0;
        in_buf.reserve(MAX_ENCODED_BUF_SIZE_FOR_TEST);
        out_buf.assign(MAX_ENCODED_BUF_SIZE_FOR_TEST, 0xAA); /* Fill with a pattern to check overwrite */
        expected_buf.reserve(MAX_ENCODED_BUF_SIZE_FOR_TEST);
    }

    void TearDown() override {
        in_buf.clear();
        out_buf.clear();
        expected_buf.clear();
    }

    /* Helper to compare buffers */
    void VerifyOutput(const std::vector<uint8_t>& expected, const std::vector<uint8_t>& actual_full_buffer, uint16_t actual_len, const std::string& message = "") {
        ASSERT_EQ(expected.size(), actual_len) << message << " - Length mismatch.";
        if (expected.size() == actual_len) { /* Only compare if lengths match assertion */
            EXPECT_EQ(0, memcmp(expected.data(), actual_full_buffer.data(), actual_len)) << message << " - Content mismatch.";
        }
        /* Optional: Print buffers if different for easier debugging, even if ASSERT_EQ fails first */
        if (expected.size() != actual_len || (actual_len > 0 && memcmp(expected.data(), actual_full_buffer.data(), actual_len) != 0)) {
            std::cout << "Debug Info for: " << message << std::endl;
            std::cout << "Expected (len " << expected.size() << "): ";
            for (size_t i = 0; i < expected.size(); ++i) {
                std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)expected[i] << " ";
            }
            std::cout << std::dec << std::endl;
            std::cout << "Actual   (len " << actual_len << "): ";
            for (size_t i = 0; i < actual_len; ++i) {
                std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)actual_full_buffer[i] << " ";
            }
            std::cout << std::dec << std::endl;
        }
    }

    /* Helper for round-trip encode/decode test */
    void TestRoundTrip(const std::vector<uint8_t>& original_payload, uint8_t cmd, const std::string& test_name) {
        std::vector<uint8_t> packet_to_encode = create_unencoded_packet_data(cmd, original_payload);
        std::vector<uint8_t> encoded_buffer(MAX_ENCODED_BUF_SIZE_FOR_TEST);
        uint16_t encoded_len_val = 0;

        uint8_t encode_lrc = oasis_packet_encode(packet_to_encode.data(), (uint16_t)packet_to_encode.size(),
            encoded_buffer.data(), &encoded_len_val);

        ASSERT_GT(encoded_len_val, 0) << test_name << " - Encoding failed or produced zero length.";
        ASSERT_LE(encoded_len_val, encoded_buffer.size()) << test_name << " - Encoded output exceeded buffer.";

        std::vector<uint8_t> decoded_payload_buffer(MAX_DECODED_PAYLOAD_BUF_SIZE_FOR_TEST);
        uint16_t decoded_payload_len = 0;

        int decode_ret = oasis_packet_decode(encoded_buffer.data(), encoded_len_val,
            decoded_payload_buffer.data(), &decoded_payload_len);

        ASSERT_GT(decode_ret, 0) << test_name << " - Decoding failed or checksum error (ret: " << decode_ret << ").";
        EXPECT_EQ((uint8_t)decode_ret, encode_lrc) << test_name << " - Encode/Decode LRC mismatch.";

        ASSERT_EQ(original_payload.size(), decoded_payload_len) << test_name << " - Decoded payload length mismatch.";
        if (original_payload.size() == decoded_payload_len) {
            EXPECT_EQ(0, memcmp(original_payload.data(), decoded_payload_buffer.data(), original_payload.size()))
                << test_name << " - Decoded payload content mismatch.";
        }
    }
};

/* --- Tests for oasis_lrcc --- */
TEST_F(OasisSendRecvTest, LrccBasic) {
    in_buf = { DLE, STX, 'O', 'A', 'B', 'C', DLE, ETX }; /* Data up to DLE ETX */
    uint8_t lrc = oasis_lrcc(in_buf.data(), (uint16_t)in_buf.size());
    EXPECT_EQ(0x7A, lrc);
}

TEST_F(OasisSendRecvTest, LrccWithZeroes) {
    in_buf = { 0x00, 0x00, 0x00 };
    uint8_t lrc = oasis_lrcc(in_buf.data(), (uint16_t)in_buf.size());
    EXPECT_EQ(0x40, lrc);
}

TEST_F(OasisSendRecvTest, LrccEmpty) {
    in_buf = {};
    uint8_t lrc = oasis_lrcc(in_buf.data(), (uint16_t)in_buf.size());
    EXPECT_EQ(0x40, lrc);
}

TEST_F(OasisSendRecvTest, LrccNullBuffer) {
    uint8_t lrc = oasis_lrcc(nullptr, 5);
    EXPECT_EQ(0, lrc); /* Function returns 0 if buf is NULL */
}

TEST_F(OasisSendRecvTest, LrccSingleByte0xFF) {
    in_buf = { 0xFF };
    uint8_t lrc = oasis_lrcc(in_buf.data(), (uint16_t)in_buf.size());
    /* Sum = 0xFF. (0xFF | 0xC0) & 0x7F = 0xFF & 0x7F = 0x7F */
    EXPECT_EQ(0x7F, lrc);
}

TEST_F(OasisSendRecvTest, LrccSingleByte0x40) {
    in_buf = { 0x40 };
    uint8_t lrc = oasis_lrcc(in_buf.data(), (uint16_t)in_buf.size());
    /* Sum = 0x40. (0x40 | 0xC0) & 0x7F = 0xC0 & 0x7F = 0x40 */
    EXPECT_EQ(0x40, lrc);
}

TEST_F(OasisSendRecvTest, LrccControlCodesOnly) {
    in_buf = { DLE, STX, OPEN, DLE, ETX }; /* Example sequence for LRC calc */
    uint8_t lrc = oasis_lrcc(in_buf.data(), (uint16_t)in_buf.size());
    /* DLE(10)+STX(02)+OPEN(4F)+DLE(10)+ETX(03) = 10+02+4F+10+03 = 0x74
       (0x74 | 0xC0) & 0x7F = 0xF4 & 0x7F = 0x74
    */
    EXPECT_EQ(0x74, lrc);
}

/* --- Tests for oasis_packet_encode --- */

TEST_F(OasisSendRecvTest, EncodeBasic) {
    std::vector<uint8_t> payload = { 'H', 'e', 'l', 'l', 'o' };
    in_buf = create_unencoded_packet_data('W', payload);

    std::vector<uint8_t> lrc_calc_data = { DLE, STX, 'W', 'H', 'e', 'l', 'l', 'o', DLE, ETX };
    uint8_t expected_lrc_val = oasis_lrcc(lrc_calc_data.data(), (uint16_t)lrc_calc_data.size());
    expected_buf = { DLE, STX, 'W', 'H', 'e', 'l', 'l', 'o', DLE, ETX, expected_lrc_val, RUB };

    uint8_t lrc = oasis_packet_encode(in_buf.data(), (uint16_t)in_buf.size(), out_buf.data(), &out_len);
    EXPECT_EQ(expected_lrc_val, lrc);
    VerifyOutput(expected_buf, out_buf, out_len, "EncodeBasic");
}

TEST_F(OasisSendRecvTest, EncodeDleStuffing) {
    std::vector<uint8_t> payload = { 'A', DLE, 'B' };
    TestRoundTrip(payload, 'W', "EncodeDecodeDleStuffing");
}

TEST_F(OasisSendRecvTest, EncodeEscStuffing) {
    std::vector<uint8_t> payload = { 'X', ESC, 'Y' };
    TestRoundTrip(payload, 'W', "EncodeDecodeEscStuffing");
}

TEST_F(OasisSendRecvTest, EncodeShiftStates) {
    std::vector<uint8_t> payload = { 'A', 0x81, 'B', 0x02, 'C', 0x83 };
    TestRoundTrip(payload, 'W', "EncodeDecodeShiftStates");
}

TEST_F(OasisSendRecvTest, EncodeEmptyPayload) {
    std::vector<uint8_t> payload = {};
    in_buf = create_unencoded_packet_data('O', payload);

    std::vector<uint8_t> lrc_calc_data = { DLE, STX, 'O', DLE, ETX };
    uint8_t expected_lrc_val = oasis_lrcc(lrc_calc_data.data(), (uint16_t)lrc_calc_data.size());
    expected_buf = { DLE, STX, 'O', DLE, ETX, expected_lrc_val, RUB };

    uint8_t lrc = oasis_packet_encode(in_buf.data(), (uint16_t)in_buf.size(), out_buf.data(), &out_len);
    EXPECT_EQ(expected_lrc_val, lrc);
    VerifyOutput(expected_buf, out_buf, out_len, "EncodeEmptyPayload");
    TestRoundTrip(payload, 'O', "EncodeDecodeEmptyPayload");
}

TEST_F(OasisSendRecvTest, EncodeRLEBasic) {
    std::vector<uint8_t> payload = { 'A', 'A', 'A', 'A', 'A', 'B', 'C', 'C', 'C', 'C' };
    TestRoundTrip(payload, 'W', "EncodeDecodeRLEBasic");
}

TEST_F(OasisSendRecvTest, EncodeRLEWithDLECharRun) {
    std::vector<uint8_t> payload = { DLE, DLE, DLE, DLE };
    TestRoundTrip(payload, 'W', "EncodeDecodeRLEWithDLECharRun");
}

TEST_F(OasisSendRecvTest, EncodeRLEWithESCCharRun) {
    std::vector<uint8_t> payload = { ESC, ESC, ESC, ESC, ESC };
    TestRoundTrip(payload, 'W', "EncodeDecodeRLEWithESCCharRun");
}


TEST_F(OasisSendRecvTest, EncodeDecodeComplexRLE) {
    std::vector<uint8_t> payload = { 'A', 'A', 'A', /* No RLE */
                                  'B', 'B', 'B', 'B', /* RLE */
                                  'C', 'C', 'C', 'C', 'C', 'C', /* RLE */
                                  DLE, DLE, DLE, DLE, DLE,   /* RLE of DLE */
                                  ESC, ESC, ESC, ESC, ESC, ESC, ESC, /* RLE of ESC */
                                  'D' }; /* Single char */
    /* Add a long run of spaces */
    for (int i = 0; i < 150; ++i) payload.push_back(' ');
    payload.push_back('E');

    TestRoundTrip(payload, 'W', "EncodeDecodeComplexRLE");
}

TEST_F(OasisSendRecvTest, EncodeDecodeRLEMaxAndMultiSegment) {
    std::vector<uint8_t> payload;
    /* RUN_LENGTH_MAX is 127. RLE encodes count of *additional* chars. */
    /* So a run of (1 + RUN_LENGTH_MAX) chars will use DLE VT RUN_LENGTH_MAX */
    /* A run of (1 + RUN_LENGTH_MAX + 1) chars will use DLE VT RUN_LENGTH_MAX then DLE VT 1 */

    /* Test 1: Exactly 1 + RUN_LENGTH_MAX characters */
    for (int i = 0; i < (1 + RUN_LENGTH_MAX); ++i) payload.push_back('X');
    /* Test 2: More than 1 + RUN_LENGTH_MAX, requiring two VT segments */
    for (int i = 0; i < (1 + RUN_LENGTH_MAX + 5); ++i) payload.push_back('Y');

    TestRoundTrip(payload, 'W', "EncodeDecodeRLEMaxAndMultiSegment");
}

TEST_F(OasisSendRecvTest, EncodeDecodeRLE_VTCountEscaping) {
    std::vector<uint8_t> payload;
    /* Test 1: Run length where count byte for VT is DLE (0x10) */
    /* Need 0x10 (DLE) + 1 = 17 'D's for the RLE count to be DLE */
    for (int i = 0; i < (DLE + 1); ++i) payload.push_back('D');

    /* Test 2: Run length where count byte for VT is ESC (0x1B) */
    /* Need 0x1B (ESC) + 1 = 28 'E's for the RLE count to be ESC */
    for (int i = 0; i < (ESC + 1); ++i) payload.push_back('E');

    TestRoundTrip(payload, 'W', "EncodeDecodeRLE_VTCountEscaping");
}


TEST_F(OasisSendRecvTest, EncodeInvalidInput) {
    /* Null inbuf */
    uint8_t lrc = oasis_packet_encode(nullptr, 10, out_buf.data(), &out_len);
    EXPECT_EQ(0, lrc);
    EXPECT_EQ(0, out_len);

    /* inlen too short */
    in_buf = { DLE, STX }; /* Length 2, min is 3 for header */
    lrc = oasis_packet_encode(in_buf.data(), (uint16_t)in_buf.size(), out_buf.data(), &out_len);
    EXPECT_EQ(0, lrc);
    EXPECT_EQ(0, out_len);
}

TEST_F(OasisSendRecvTest, EncodeErrorOutputBufferOverflowSimulated) {
    std::vector<uint8_t> large_payload;
    large_payload.reserve(700);
    /* Create a payload that expands significantly and doesn't compress well with RLE.
     * Each (DLE, 'A') pair (2 input bytes) becomes (DLE, DLE, 'A') (3 encoded bytes for payload part).
     * The 'A' ensures RLE is not triggered for the DLEs.
     * We want the total encoded size to exceed oasis_packet_encode's internal max_outbuf_len of 1024.
     * Total encoded size = 3 (header) + payload_encoded_len + 4 (trailer: DLE ETX LRC RUB).
     * Let payload_encoded_len be just over 1024 - 3 - 4 = 1017.
     * If 1.5 * original_payload_len ~ 1020, then original_payload_len ~ 1020 / 1.5 = 680.
     * Let's use 340 pairs of (DLE, 'A'), so original payload length is 680 bytes.
     * Encoded payload part will be 340 * 3 = 1020 bytes.
     * Total encoded length before overflow check: 3 + 1020 + 4 = 1027 bytes.
     */
    for (int k = 0; k < 340; ++k) { /* 340 pairs = 680 bytes payload */
        large_payload.push_back(DLE);
        large_payload.push_back('A');
    }

    std::vector<uint8_t> unencoded_packet = create_unencoded_packet_data('W', large_payload);
    /* unencoded_packet size will be 3 + 680 = 683. */

    std::vector<uint8_t> local_out_buf(2048); /* This buffer given to the function is large enough. */
    /* The overflow we are testing is *internal* to oasis_packet_encode's max_outbuf_len. */
    uint16_t local_out_len = 0;

    uint8_t lrc = oasis_packet_encode(unencoded_packet.data(), (uint16_t)unencoded_packet.size(),
        local_out_buf.data(), &local_out_len);

    EXPECT_EQ(0, lrc) << "LRC should be 0 on encode overflow.";
    EXPECT_EQ(0, local_out_len) << "Output length should be 0 on encode overflow.";
}


/* --- Tests for oasis_packet_decode --- */

TEST_F(OasisSendRecvTest, DecodeBasic) {
    std::vector<uint8_t> lrc_calc_data = { DLE, STX, 'W', 'H', 'i', DLE, ETX };
    uint8_t lrc_val = oasis_lrcc(lrc_calc_data.data(), (uint16_t)lrc_calc_data.size());
    in_buf = { DLE, STX, 'W', 'H', 'i', DLE, ETX, lrc_val, RUB };
    expected_buf = { 'H', 'i' };

    int ret = oasis_packet_decode(in_buf.data(), (uint16_t)in_buf.size(), out_buf.data(), &out_len);
    EXPECT_EQ(lrc_val, ret);
    VerifyOutput(expected_buf, out_buf, out_len, "DecodeBasic");
}

TEST_F(OasisSendRecvTest, DecodeChecksumMismatch) {
    std::vector<uint8_t> lrc_calc_data = { DLE, STX, 'W', 'H', 'i', DLE, ETX };
    uint8_t lrc_val = oasis_lrcc(lrc_calc_data.data(), (uint16_t)lrc_calc_data.size());
    uint8_t bad_lrc = (lrc_val == 0x12 ? 0x13 : 0x12); /* Pick a different lrc */

    in_buf = { DLE, STX, 'W', 'H', 'i', DLE, ETX, bad_lrc, RUB };

    int ret = oasis_packet_decode(in_buf.data(), (uint16_t)in_buf.size(), out_buf.data(), &out_len);
    EXPECT_EQ(0, ret); /* Expect 0 for checksum mismatch */
    EXPECT_EQ(0, out_len);
}

TEST_F(OasisSendRecvTest, DecodeInvalidHeader) {
    in_buf = { DLE, 'X', 'W', 'H', 'i', DLE, ETX, 0x12, RUB }; /* Invalid STX */
    int ret = oasis_packet_decode(in_buf.data(), (uint16_t)in_buf.size(), out_buf.data(), &out_len);
    EXPECT_EQ(-EBADMSG, ret);
    EXPECT_EQ(0, out_len);

    in_buf = { 'Y', STX, 'W', 'H', 'i', DLE, ETX, 0x12, RUB }; /* Invalid DLE */
    ret = oasis_packet_decode(in_buf.data(), (uint16_t)in_buf.size(), out_buf.data(), &out_len);
    EXPECT_EQ(-EBADMSG, ret);
    EXPECT_EQ(0, out_len);
}

TEST_F(OasisSendRecvTest, DecodeMissingTrailer) {
    in_buf = { DLE, STX, 'W', 'H', 'i' }; /* Missing DLE ETX LRC RUB */
    int ret = oasis_packet_decode(in_buf.data(), (uint16_t)in_buf.size(), out_buf.data(), &out_len);
    EXPECT_EQ(-EBADMSG, ret);
    EXPECT_EQ(0, out_len);
}

TEST_F(OasisSendRecvTest, DecodeEndsAfterDle) {
    in_buf = { DLE, STX, 'W', 'A', DLE };
    int ret = oasis_packet_decode(in_buf.data(), (uint16_t)in_buf.size(), out_buf.data(), &out_len);
    EXPECT_EQ(-EBADMSG, ret);
}

TEST_F(OasisSendRecvTest, DecodeEndsAfterDleEtx) {
    in_buf = { DLE, STX, 'W', 'A', DLE, ETX };
    int ret = oasis_packet_decode(in_buf.data(), (uint16_t)in_buf.size(), out_buf.data(), &out_len);
    EXPECT_EQ(-EBADMSG, ret);
}

TEST_F(OasisSendRecvTest, DecodeVtEndsEarly) {
    in_buf = { DLE, STX, 'W', 'A', DLE, VT };
    int ret = oasis_packet_decode(in_buf.data(), (uint16_t)in_buf.size(), out_buf.data(), &out_len);
    EXPECT_EQ(-EBADMSG, ret);

    in_buf = { DLE, STX, 'W', 'A', DLE, VT, DLE };
    ret = oasis_packet_decode(in_buf.data(), (uint16_t)in_buf.size(), out_buf.data(), &out_len);
    EXPECT_EQ(-EBADMSG, ret);
}

TEST_F(OasisSendRecvTest, DecodeErrorVtBeforeChar) {
    std::vector<uint8_t> lrc_calc_data = { DLE, STX, 'W', DLE, VT, 5, DLE, ETX };
    uint8_t lrc_val = oasis_lrcc(lrc_calc_data.data(), (uint16_t)lrc_calc_data.size());
    in_buf = { DLE, STX, 'W', DLE, VT, 5, DLE, ETX, lrc_val, RUB };

    int ret = oasis_packet_decode(in_buf.data(), (uint16_t)in_buf.size(), out_buf.data(), &out_len);
    EXPECT_EQ(-EBADMSG, ret);
}

TEST_F(OasisSendRecvTest, DecodeErrorUnknownDleSequence) {
    std::vector<uint8_t> lrc_calc_data = { DLE, STX, 'W', DLE, 0x01, DLE, ETX }; /* DLE 0x01 is invalid seq */
    uint8_t lrc_val = oasis_lrcc(lrc_calc_data.data(), (uint16_t)lrc_calc_data.size());
    in_buf = { DLE, STX, 'W', DLE, 0x01, DLE, ETX, lrc_val, RUB };

    int ret = oasis_packet_decode(in_buf.data(), (uint16_t)in_buf.size(), out_buf.data(), &out_len);
    EXPECT_EQ(-EBADMSG, ret);
}

TEST_F(OasisSendRecvTest, DecodeErrorUnknownEscapedVtCount) {
    std::vector<uint8_t> lrc_calc_data = { DLE, STX, 'W', 'A', DLE, VT, DLE, 'X', DLE, ETX }; /* DLE 'X' invalid count */
    uint8_t lrc_val = oasis_lrcc(lrc_calc_data.data(), (uint16_t)lrc_calc_data.size());
    in_buf = { DLE, STX, 'W', 'A', DLE, VT, DLE, 'X', DLE, ETX, lrc_val, RUB };

    int ret = oasis_packet_decode(in_buf.data(), (uint16_t)in_buf.size(), out_buf.data(), &out_len);
    EXPECT_EQ(-EBADMSG, ret);
}


TEST_F(OasisSendRecvTest, DecodeInvalidArgs) {
    uint16_t len;
    int ret = oasis_packet_decode(nullptr, 10, out_buf.data(), &len);
    EXPECT_EQ(-EINVAL, ret);

    in_buf = { DLE, STX, 'O', DLE, ETX, 0x12, RUB };
    ret = oasis_packet_decode(in_buf.data(), (uint16_t)in_buf.size(), nullptr, &len);
    EXPECT_EQ(-EINVAL, ret);

    ret = oasis_packet_decode(in_buf.data(), (uint16_t)in_buf.size(), out_buf.data(), nullptr);
    EXPECT_EQ(-EINVAL, ret);

    in_buf = { DLE, STX, 'O', DLE }; /* Len 4. Decode expects in_len < 5 for this specific EINVAL. */
    ret = oasis_packet_decode(in_buf.data(), (uint16_t)in_buf.size(), out_buf.data(), &len);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(OasisSendRecvTest, DecodeErrorOutputBufferOverflowInternalLimit) {
    /* Create a valid encoded packet whose *decoded* payload is > 512 bytes */
    std::vector<uint8_t> large_payload(MAX_PAYLOAD_SIZE + 1, 'A'); /* e.g., 513 'A's */
    std::vector<uint8_t> unencoded_packet = create_unencoded_packet_data('W', large_payload);

    std::vector<uint8_t> encoded_buffer_for_test(MAX_ENCODED_BUF_SIZE_FOR_TEST);
    uint16_t encoded_len_val = 0;

    /* Encode it (this should succeed as encoded_buffer_for_test is large) */
    oasis_packet_encode(unencoded_packet.data(), (uint16_t)unencoded_packet.size(),
        encoded_buffer_for_test.data(), &encoded_len_val);
    ASSERT_GT(encoded_len_val, 0U);

    /* Now try to decode. out_buf from the fixture is large enough.
     * The test is for oasis_packet_decode's internal limit (max_outbuf_len = 512) for the *decoded* payload.
     */
    uint16_t decoded_len_val = 0;
    int ret = oasis_packet_decode(encoded_buffer_for_test.data(), encoded_len_val,
        out_buf.data(),
        &decoded_len_val);

    EXPECT_EQ(-ENOBUFS, ret) << "Decode should return -ENOBUFS due to internal payload limit.";
}
