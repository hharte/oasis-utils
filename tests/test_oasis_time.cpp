/* test_oasis_time.cpp */
#include "gtest/gtest.h"
#include "oasis.h"      /* For oasis_tm_t */
#include <string.h>     /* For memset, strcmp */
#include <time.h>       /* For struct tm */

class OasisTimeTest : public ::testing::Test {
protected:
    oasis_tm_t o_time;
    struct tm c_time;
    char time_str_buffer[32];

    void SetUp() override {
        /* Initialize o_time with zeros. */
        memset(&o_time, 0, sizeof(o_time));
        /* Initialize c_time with zeros. This ensures all fields,
           including any non-standard ones like tm_gmtoff, are zeroed. */
        memset(&c_time, 0, sizeof(c_time));
        /* Initialize time_str_buffer with zeros. */
        memset(time_str_buffer, 0, sizeof(time_str_buffer));
    }

    /* Helper to compare two oasis_tm_t structures. */
    bool AreOasisTmEqual(const oasis_tm_t* t1, const oasis_tm_t* t2) {
        return memcmp(t1->raw, t2->raw, sizeof(t1->raw)) == 0;
    }
};

TEST_F(OasisTimeTest, OasisToTmBasic) {
    /* Example: 04/23/85 14:30
     * Month = 4 (0100)
     * Day   = 23 (10111) -> day_hi = 1011 (B), day_lo = 1
     * Year  = 1985 -> 85-77 = 8 (1000)
     * Hour  = 14 (01110) -> hour_hi = 011 (3), hour_lo = 10 (2)
     * Min   = 30 (011110)
     *
     * raw[0] = MMMM DDDD_hi = 0100 1011 = 0x4B
     * raw[1] = DDDD_loYYYYHHH_hi = 1 1000 011 = 0xC3
     * raw[2] = HH_lo MMMMMM = 10 011110 = 0x9E
     */
    o_time.raw[0] = 0x4B;
    o_time.raw[1] = 0xC3;
    o_time.raw[2] = 0x9E;

    oasis_convert_timestamp_to_tm(&o_time, &c_time);

    EXPECT_EQ(c_time.tm_year, 1985 - 1900);
    EXPECT_EQ(c_time.tm_mon, 4 - 1);
    EXPECT_EQ(c_time.tm_mday, 23);
    EXPECT_EQ(c_time.tm_hour, 14);
    EXPECT_EQ(c_time.tm_min, 30);
    EXPECT_EQ(c_time.tm_sec, 0);
    EXPECT_EQ(c_time.tm_isdst, -1);
}

TEST_F(OasisTimeTest, TmToOasisBasic) {
    c_time.tm_year = 1985 - 1900;
    c_time.tm_mon  = 4 - 1;
    c_time.tm_mday = 23;
    c_time.tm_hour = 14;
    c_time.tm_min  = 30;
    /* Other fields like tm_sec, tm_wday, tm_yday, tm_isdst
       are already 0 or -1 due to memset in SetUp, or their values
       are not used by oasis_convert_tm_to_timestamp beyond the main ones. */

    oasis_convert_tm_to_timestamp(&c_time, &o_time);

    EXPECT_EQ(o_time.raw[0], 0x4B);
    EXPECT_EQ(o_time.raw[1], 0xC3);
    EXPECT_EQ(o_time.raw[2], 0x9E);
}

TEST_F(OasisTimeTest, OasisToTmBoundariesAndClamping) {
    /* Min OASIS date: 01/01/77 00:00 */
    /* M=1 (0001), D=1 (00001), Y=0 (0000), H=0 (00000), M=0 (000000) */
    /* raw[0]=00010000=0x10, raw[1]=10000000=0x80, raw[2]=00000000=0x00 */
    o_time.raw[0] = 0x10; o_time.raw[1] = 0x80; o_time.raw[2] = 0x00;
    oasis_convert_timestamp_to_tm(&o_time, &c_time);
    EXPECT_EQ(c_time.tm_year, 1977 - 1900); EXPECT_EQ(c_time.tm_mon, 0); EXPECT_EQ(c_time.tm_mday, 1);
    EXPECT_EQ(c_time.tm_hour, 0); EXPECT_EQ(c_time.tm_min, 0);

    /* Max OASIS date: 12/31/92 23:59 */
    /* M=12(1100), D=31(11111), Y=15(1111), H=23(10111), M=59(111011) */
    /* raw[0]=11001111=0xCF, raw[1]=11111101=0xFD, raw[2]=11111011=0xFB */
    o_time.raw[0] = 0xCF; o_time.raw[1] = 0xFD; o_time.raw[2] = 0xFB;
    oasis_convert_timestamp_to_tm(&o_time, &c_time);
    EXPECT_EQ(c_time.tm_year, 1992 - 1900); EXPECT_EQ(c_time.tm_mon, 11); EXPECT_EQ(c_time.tm_mday, 31);
    EXPECT_EQ(c_time.tm_hour, 23); EXPECT_EQ(c_time.tm_min, 59);

    /* Test clamping (e.g. month 15 -> 12, day 0 -> 1, day 35 -> 31) */
    /* Month 15 (1111), Day 0 (00000), Year 5 (0101), Hour 25 (11001 -> 23), Min 70 (1000110 -> 59) */
    /* Hour was 25 (11001). H_hi=110 (from raw[1]=0x2E). H_lo should be 01. */
    /* raw[0]=11110000=0xF0, raw[1]=00101110=0x2E, raw[2]=01000110=0x46 (using raw values before packing)
       Month=15 -> 12. Day=0 -> 1. Year=5(1982). Hour=25 -> 23. Min=70 -> 59
       Re-pack for test with clamped values:
       M=12(1100), D=1(00001), Y=5(0101), H=23(10111), M=59(111011)
       raw[0]=11000000=0xC0, raw[1]=10101101=0xAD, raw[2]=11111011=0xFB
    */
    o_time.raw[0] = 0xF0;
    o_time.raw[1] = 0x2E; /* D_lo=0, Y=5 (0101), H_hi=6 (110 for Hour 25) */
    o_time.raw[2] = 0x7C; /* H_lo=1 (01 for Hour 25), Min_raw=60 (111100) */
    /* So raw[2] = (01 << 6) | 111100 = 01111100 = 0x7C */
    oasis_convert_timestamp_to_tm(&o_time, &c_time);
    EXPECT_EQ(c_time.tm_year, (1977 + 5) - 1900); /* Year 5 -> 1982 */
    EXPECT_EQ(c_time.tm_mon, 11);  /* Clamped from M=15 (raw[0]>>4) */
    EXPECT_EQ(c_time.tm_mday, 1);  /* Clamped from D=0 (raw[0]&0xF, raw[1]>>7) */
    EXPECT_EQ(c_time.tm_hour, 23); /* Clamped from H=25 */
    EXPECT_EQ(c_time.tm_min, 59);  /* Clamped from Min=60 (raw[2]&0x3F = 0x3C = 60) */
}

TEST_F(OasisTimeTest, TmToOasisClamping) {
    /* Year before range. */
    /* c_time = {0, 0, 0, 1, 0, 1970 - 1900, 0, 0, -1}; 01/01/1970 00:00:00 */
    /* memset in SetUp has already zeroed c_time. Assign standard fields directly. */
    c_time.tm_sec = 0;
    c_time.tm_min = 0;
    c_time.tm_hour = 0;
    c_time.tm_mday = 1;
    c_time.tm_mon = 0; /* January. */
    c_time.tm_year = 1970 - 1900;
    c_time.tm_wday = 0; /* Not used by converter, but initializing. */
    c_time.tm_yday = 0; /* Not used by converter. */
    c_time.tm_isdst = -1;
    oasis_convert_tm_to_timestamp(&c_time, &o_time);
    /* Expect it to be clamped to 01/01/77 00:00. */
    EXPECT_EQ(o_time.raw[0], 0x10); EXPECT_EQ(o_time.raw[1], 0x80); EXPECT_EQ(o_time.raw[2], 0x00);

    /* Year after range. */
    /* c_time = {0, 59, 23, 31, 11, 2000 - 1900, 0, 0, -1}; 12/31/2000 23:59:00 */
    c_time.tm_sec = 0;
    c_time.tm_min = 59;
    c_time.tm_hour = 23;
    c_time.tm_mday = 31;
    c_time.tm_mon = 11; /* December. */
    c_time.tm_year = 2000 - 1900;
    c_time.tm_wday = 0;
    c_time.tm_yday = 0;
    c_time.tm_isdst = -1;
    oasis_convert_tm_to_timestamp(&c_time, &o_time);
    /* Expect it to be clamped to 12/31/92 23:59. */
    EXPECT_EQ(o_time.raw[0], 0xCF); EXPECT_EQ(o_time.raw[1], 0xFD); EXPECT_EQ(o_time.raw[2], 0xFB);

    /* Invalid month/day (should be clamped). */
    /* c_time = {0, 30, 14, 35, 13, 1985 - 1900, 0, 0, -1}; M=14(clamp->12), D=35(clamp->31) */
    c_time.tm_sec = 0;
    c_time.tm_min = 30;
    c_time.tm_hour = 14;
    c_time.tm_mday = 35; /* Day 35 (to be clamped). */
    c_time.tm_mon = 13;  /* Month 14 (0-indexed, so 13 means 14th month, to be clamped). */
    c_time.tm_year = 1985 - 1900;
    c_time.tm_wday = 0;
    c_time.tm_yday = 0;
    c_time.tm_isdst = -1;
    oasis_convert_tm_to_timestamp(&c_time, &o_time);
    /* Expect 12/31/85 14:30. */
    /* M=12(1100), D=31(11111), Y=8(1000), H=14(01110), M=30(011110) */
    /* raw[0]=11001111=0xCF, raw[1]=11000011=0xC3, raw[2]=10011110=0x9E */
    EXPECT_EQ(o_time.raw[0], 0xCF);
    EXPECT_EQ(o_time.raw[1], 0xC3);
    EXPECT_EQ(o_time.raw[2], 0x9E);
}

TEST_F(OasisTimeTest, OasisTimeStrBasic) {
    o_time.raw[0] = 0x4B; o_time.raw[1] = 0xC3; o_time.raw[2] = 0x9E; /* 04/23/85 14:30 */
    size_t len = oasis_time_str(time_str_buffer, sizeof(time_str_buffer), &o_time);
    EXPECT_STREQ(time_str_buffer, "04/23/85 14:30");
    EXPECT_EQ(len, strlen("04/23/85 14:30"));
}

TEST_F(OasisTimeTest, OasisTimeStrBufferTooSmall) {
    o_time.raw[0] = 0x4B; o_time.raw[1] = 0xC3; o_time.raw[2] = 0x9E; /* 04/23/85 14:30 */
    size_t len = oasis_time_str(time_str_buffer, 10, &o_time); /* "04/23/85 14:30" is 14 chars */
    EXPECT_EQ(len, static_cast<size_t>(0)); /* strftime returns 0 if buffer too small */
    EXPECT_STREQ(time_str_buffer, ""); /* Should be empty string on error */

    /* Test with null dest or zero size */
    EXPECT_EQ(oasis_time_str(nullptr, 10, &o_time), static_cast<size_t>(0));
    EXPECT_EQ(oasis_time_str(time_str_buffer, 0, &o_time), static_cast<size_t>(0));
}

TEST_F(OasisTimeTest, RoundTripTmToOasisToTm) {
    struct tm original_tm, final_tm;
    oasis_tm_t intermediate_oasis_tm;

    /* memset in SetUp has already zeroed original_tm via c_time,
       but we'll be explicit for original_tm and final_tm if they are different instances.
       However, this test uses local struct tm instances, so they need init. */
    memset(&original_tm, 0, sizeof(original_tm));
    memset(&final_tm, 0, sizeof(final_tm));


    original_tm.tm_year = 1980 - 1900;
    original_tm.tm_mon  = 6 - 1; /* June */
    original_tm.tm_mday = 15;
    original_tm.tm_hour = 10;
    original_tm.tm_min  = 45;
    original_tm.tm_sec  = 0; /* OASIS doesn't store seconds */
    original_tm.tm_isdst = -1;

    oasis_convert_tm_to_timestamp(&original_tm, &intermediate_oasis_tm);
    oasis_convert_timestamp_to_tm(&intermediate_oasis_tm, &final_tm);

    EXPECT_EQ(final_tm.tm_year, original_tm.tm_year);
    EXPECT_EQ(final_tm.tm_mon,  original_tm.tm_mon);
    EXPECT_EQ(final_tm.tm_mday, original_tm.tm_mday);
    EXPECT_EQ(final_tm.tm_hour, original_tm.tm_hour);
    EXPECT_EQ(final_tm.tm_min,  original_tm.tm_min);
    EXPECT_EQ(final_tm.tm_sec,  0); /* Seconds are always 0 after conversion from OASIS */
}

TEST_F(OasisTimeTest, RoundTripOasisToTmToOasis) {
    oasis_tm_t original_oasis_tm, final_oasis_tm;
    struct tm intermediate_tm;

    /* Initialize local struct instances. */
    memset(&original_oasis_tm, 0, sizeof(original_oasis_tm));
    memset(&final_oasis_tm, 0, sizeof(final_oasis_tm));
    memset(&intermediate_tm, 0, sizeof(intermediate_tm));


    /* 09/10/88 08:20 */
    /* M=9 (1001), D=10(01010), Y=11(1011), H=8(01000), M=20(010100) */
    original_oasis_tm.raw[0] = 0b10010101; /* 0x95 */
    original_oasis_tm.raw[1] = 0b01011010; /* 0x5A */
    original_oasis_tm.raw[2] = 0b00010100; /* 0x14 */

    oasis_convert_timestamp_to_tm(&original_oasis_tm, &intermediate_tm);
    oasis_convert_tm_to_timestamp(&intermediate_tm, &final_oasis_tm);

    ASSERT_TRUE(AreOasisTmEqual(&original_oasis_tm, &final_oasis_tm));
}
