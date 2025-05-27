/* tests/test_oasis_deb.cpp */
#include "gtest/gtest.h"
#include "oasis.h"     /* For DEB structure and constants */
#include <string.h>    /* For memcpy, strcmp, memset, strncmp */

/* Common Test Utilities */
#include "test_oasis_common.h"

class OasisDebTest : public ::testing::Test {
protected:
    directory_entry_block_t deb;
    char host_filename_buffer[MAX_HOST_FILENAME_LEN];
    char fname_ftype_buffer[MAX_FNAME_FTYPE_LEN];

    void SetUp() override {
        memset(&deb, 0, sizeof(deb));
        memset(host_filename_buffer, 0, sizeof(host_filename_buffer));
        memset(fname_ftype_buffer, 0, sizeof(fname_ftype_buffer));
    }
};

TEST_F(OasisDebTest, IsValidDeb) {
    /* Valid Sequential */
    tests_common::populate_deb(&deb, "VALID", "SEQ", FILE_FORMAT_SEQUENTIAL, 10, 1, 1, 0, 0);
    EXPECT_TRUE(oasis_deb_is_valid(&deb));

    /* Empty */
    deb.file_format = FILE_FORMAT_EMPTY; /* Directly set for this specific invalid case */
    EXPECT_FALSE(oasis_deb_is_valid(&deb));

    /* Deleted */
    deb.file_format = FILE_FORMAT_DELETED; /* Directly set for this specific invalid case */
    EXPECT_FALSE(oasis_deb_is_valid(&deb));

    /* Invalid type (e.g., type 0 with attributes) */
    tests_common::populate_deb(&deb, "ATTR", "INV", FILE_FORMAT_READ_PROTECTED, 10, 1, 1, 0, 0);
    EXPECT_FALSE(oasis_deb_is_valid(&deb));

    /* Unknown type bits */
    tests_common::populate_deb(&deb, "UNKNOWN", "TYP", 0x07, 10, 1, 1, 0, 0); /* 0x07 is undefined type */
    EXPECT_FALSE(oasis_deb_is_valid(&deb));

    EXPECT_FALSE(oasis_deb_is_valid(nullptr));
}

TEST_F(OasisDebTest, GetFnameFtype) {
    tests_common::populate_deb(&deb, "TESTPROG", "BAS", FILE_FORMAT_SEQUENTIAL, 10, 1, 1, 0, 0);
    ASSERT_TRUE(oasis_deb_get_fname_ftype(&deb, fname_ftype_buffer, sizeof(fname_ftype_buffer)));
    EXPECT_STREQ(fname_ftype_buffer, "TESTPROG.BAS");

    tests_common::populate_deb(&deb, "FILE", "DAT", FILE_FORMAT_DIRECT, 10, 1, 1, 0, 0);
    ASSERT_TRUE(oasis_deb_get_fname_ftype(&deb, fname_ftype_buffer, sizeof(fname_ftype_buffer)));
    EXPECT_STREQ(fname_ftype_buffer, "FILE.DAT");

    tests_common::populate_deb(&deb, " SPACY  ", " EXT  ", FILE_FORMAT_INDEXED, 10, 1, 1, 0, 0);
    ASSERT_TRUE(oasis_deb_get_fname_ftype(&deb, fname_ftype_buffer, sizeof(fname_ftype_buffer)));
    EXPECT_STREQ(fname_ftype_buffer, "SPACY.EXT");

    /* Test buffer too small */
    tests_common::populate_deb(&deb, "LONGFNAM", "LONGFTYP", FILE_FORMAT_SEQUENTIAL, 10, 1, 1, 0, 0); /* Max lengths */
    /* "LONGFNAM.LONGFTYP" is 8 + 1 + 8 = 17 chars. Buffer of 10 is too small. */
    EXPECT_FALSE(oasis_deb_get_fname_ftype(&deb, fname_ftype_buffer, 10));

    /* Test with invalid DEB */
    deb.file_format = FILE_FORMAT_EMPTY;
    EXPECT_FALSE(oasis_deb_get_fname_ftype(&deb, fname_ftype_buffer, sizeof(fname_ftype_buffer)));
}

/* --- DebToHostFilename Tests --- */
TEST_F(OasisDebTest, DebToHostFilenameSequential) {
    /* Case 1: Sequential, Read-Protected, ffd1=128 (longest record) */
    tests_common::populate_deb(&deb, "SEQFILE", "DAT",
        FILE_FORMAT_SEQUENTIAL | FILE_FORMAT_READ_PROTECTED,
        100, 5, 10, /*ffd1*/128, /*ffd2-last_sector*/200);
    ASSERT_TRUE(oasis_deb_to_host_filename(&deb, host_filename_buffer, sizeof(host_filename_buffer)));
    EXPECT_STREQ(host_filename_buffer, "SEQFILE.DAT_SR_128");

    /* Case 2: Sequential, no attributes, ffd1=0 (calculated record length) */
    tests_common::populate_deb(&deb, "SEQCALC", "TXT", FILE_FORMAT_SEQUENTIAL, 150, 3, 0, /*ffd1*/0, /*ffd2-last_sector*/0);
    ASSERT_TRUE(oasis_deb_to_host_filename(&deb, host_filename_buffer, sizeof(host_filename_buffer)));
    EXPECT_STREQ(host_filename_buffer, "SEQCALC.TXT_S");

    /* Case 3: Sequential, Write-Protected, ffd1=0 */
    tests_common::populate_deb(&deb, "SEQWRP0", "TMP", FILE_FORMAT_SEQUENTIAL | FILE_FORMAT_WRITE_PROTECTED, 160, 2, 0, /*ffd1*/0, /*ffd2-last_sector*/0);
    ASSERT_TRUE(oasis_deb_to_host_filename(&deb, host_filename_buffer, sizeof(host_filename_buffer)));
    EXPECT_STREQ(host_filename_buffer, "SEQWRP0.TMP_SW_0");

    /* Case 4: Sequential, no attributes, ffd1=50 */
    tests_common::populate_deb(&deb, "SEQREC", "DAT", FILE_FORMAT_SEQUENTIAL, 100, 5, 0, /*ffd1*/50, /*ffd2-last_sector*/0);
    ASSERT_TRUE(oasis_deb_to_host_filename(&deb, host_filename_buffer, sizeof(host_filename_buffer)));
    EXPECT_STREQ(host_filename_buffer, "SEQREC.DAT_S_50");
}


TEST_F(OasisDebTest, DebToHostFilenameDirect) {
    tests_common::populate_deb(&deb, "DIRFILE", "REC",
        FILE_FORMAT_DIRECT | FILE_FORMAT_WRITE_PROTECTED | FILE_FORMAT_DELETE_PROTECTED,
        150, 10, 20, /*ffd1*/256, /*ffd2 should be 0 for direct*/0);
    ASSERT_TRUE(oasis_deb_to_host_filename(&deb, host_filename_buffer, sizeof(host_filename_buffer)));
    EXPECT_STREQ(host_filename_buffer, "DIRFILE.REC_DWD_256");
}

TEST_F(OasisDebTest, DebToHostFilenameRelocatable) {
    tests_common::populate_deb(&deb, "RELPROG", "COM",
        FILE_FORMAT_RELOCATABLE,
        200, 15, 0, /*ffd1=SECTOR_LEN*/256, /*ffd2=prog len, not in name*/1024);
    ASSERT_TRUE(oasis_deb_to_host_filename(&deb, host_filename_buffer, sizeof(host_filename_buffer)));
    EXPECT_STREQ(host_filename_buffer, "RELPROG.COM_R_256");
}

TEST_F(OasisDebTest, DebToHostFilenameAbsolute) {
    tests_common::populate_deb(&deb, "ABSPROG", "ABS",
        FILE_FORMAT_ABSOLUTE,
        300, 20, 0, /*ffd1=SECTOR_LEN*/256, /*ffd2=load_addr*/0x1000);
    ASSERT_TRUE(oasis_deb_to_host_filename(&deb, host_filename_buffer, sizeof(host_filename_buffer)));
    EXPECT_STREQ(host_filename_buffer, "ABSPROG.ABS_A_256_1000");
}

TEST_F(OasisDebTest, DebToHostFilenameIndexed) {
    tests_common::populate_deb(&deb, "IDXFILE", "IDX",
        FILE_FORMAT_INDEXED,
        400, 25, 50, /*ffd1=(keylen<<9)|reclen*/(10 << 9) | 80, /*ffd2=alloc size, not in name*/2048);
    ASSERT_TRUE(oasis_deb_to_host_filename(&deb, host_filename_buffer, sizeof(host_filename_buffer)));
    EXPECT_STREQ(host_filename_buffer, "IDXFILE.IDX_I_80_10");
}

TEST_F(OasisDebTest, DebToHostFilenameKeyed) {
    tests_common::populate_deb(&deb, "KEYFILE", "KEY",
        FILE_FORMAT_KEYED | FILE_FORMAT_READ_PROTECTED | FILE_FORMAT_WRITE_PROTECTED,
        500, 30, 60, /*ffd1=(keylen<<9)|reclen*/(8 << 9) | 64, /*ffd2=alloc size, not in name*/4096);
    ASSERT_TRUE(oasis_deb_to_host_filename(&deb, host_filename_buffer, sizeof(host_filename_buffer)));
    EXPECT_STREQ(host_filename_buffer, "KEYFILE.KEY_KRW_64_8");
}

/* --- HostToDebFilename Tests --- */
TEST_F(OasisDebTest, HostToDebFilenameDefaultSequentialCases) {
    /* Case 1: FNAME.FTYPE */
    ASSERT_TRUE(host_filename_to_oasis_deb("MYFILE.TXT", &deb));
    EXPECT_EQ(strncmp(deb.file_name, "MYFILE  ", FNAME_LEN), 0);
    EXPECT_EQ(strncmp(deb.file_type, "TXT     ", FTYPE_LEN), 0);
    EXPECT_EQ(deb.file_format, FILE_FORMAT_SEQUENTIAL);
    EXPECT_EQ(deb.file_format_dependent1, 0); /* Record length 0, to be calculated */

    /* Case 2: FNAME. (empty FTYPE) */
    memset(&deb, 0, sizeof(deb)); /* Clear DEB for next sub-test */
    ASSERT_TRUE(host_filename_to_oasis_deb("PROGRAM.", &deb));
    EXPECT_EQ(strncmp(deb.file_name, "PROGRAM ", FNAME_LEN), 0);
    EXPECT_EQ(strncmp(deb.file_type, "        ", FTYPE_LEN), 0); /* FTYPE should be blank */
    EXPECT_EQ(deb.file_format, FILE_FORMAT_SEQUENTIAL);
    EXPECT_EQ(deb.file_format_dependent1, 0);

    /* Case 3: FNAME (no dot, no FTYPE) */
    memset(&deb, 0, sizeof(deb));
    ASSERT_TRUE(host_filename_to_oasis_deb("EXECUTE", &deb));
    EXPECT_EQ(strncmp(deb.file_name, "EXECUTE ", FNAME_LEN), 0);
    EXPECT_EQ(strncmp(deb.file_type, "        ", FTYPE_LEN), 0); /* FTYPE should be blank */
    EXPECT_EQ(deb.file_format, FILE_FORMAT_SEQUENTIAL);
    EXPECT_EQ(deb.file_format_dependent1, 0);

    /* Case 4: FNAME too long */
    memset(&deb, 0, sizeof(deb));
    EXPECT_FALSE(host_filename_to_oasis_deb("LONGFILENAMEXXX.OK", &deb))
        << "Expected parsing to fail for FNAME longer than FNAME_LEN";

    /* Case 4b: FTYPE too long */
    memset(&deb, 0, sizeof(deb));
    EXPECT_FALSE(host_filename_to_oasis_deb("NORMALFN.WAYTOOLONGTYPE", &deb))
        << "Expected parsing to fail for FTYPE longer than FTYPE_LEN";

    /* Case 5: Short FNAME.FTYPE */
    memset(&deb, 0, sizeof(deb));
    ASSERT_TRUE(host_filename_to_oasis_deb("S.T", &deb));
    EXPECT_EQ(strncmp(deb.file_name, "S       ", FNAME_LEN), 0);
    EXPECT_EQ(strncmp(deb.file_type, "T       ", FTYPE_LEN), 0);
    EXPECT_EQ(deb.file_format, FILE_FORMAT_SEQUENTIAL);
    EXPECT_EQ(deb.file_format_dependent1, 0);
}


TEST_F(OasisDebTest, HostToDebFilenameSequentialExplicit) {
    const char* hn1 = "SEQFILE.DAT_S"; /* No record length specified -> ffd1=0 */
    ASSERT_TRUE(host_filename_to_oasis_deb(hn1, &deb));
    EXPECT_EQ(strncmp(deb.file_name, "SEQFILE ", FNAME_LEN), 0);
    EXPECT_EQ(strncmp(deb.file_type, "DAT     ", FTYPE_LEN), 0);
    EXPECT_EQ(deb.file_format, FILE_FORMAT_SEQUENTIAL);
    EXPECT_EQ(deb.file_format_dependent1, 0);

    memset(&deb, 0, sizeof(deb)); /* Clear for next */
    const char* hn2 = "SEQFILE.DAT_S_128";
    ASSERT_TRUE(host_filename_to_oasis_deb(hn2, &deb));
    EXPECT_EQ(strncmp(deb.file_name, "SEQFILE ", FNAME_LEN), 0);
    EXPECT_EQ(strncmp(deb.file_type, "DAT     ", FTYPE_LEN), 0);
    EXPECT_EQ(deb.file_format, FILE_FORMAT_SEQUENTIAL);
    EXPECT_EQ(deb.file_format_dependent1, 128);

    memset(&deb, 0, sizeof(deb));
    const char* hn3 = "SEQFILE.DAT_SR_0"; /* Attributes, ffd1=0 */
    ASSERT_TRUE(host_filename_to_oasis_deb(hn3, &deb));
    EXPECT_EQ(deb.file_format, FILE_FORMAT_SEQUENTIAL | FILE_FORMAT_READ_PROTECTED);
    EXPECT_EQ(deb.file_format_dependent1, 0);
}

TEST_F(OasisDebTest, HostToDebFilenameDirectNoAttrs) {
    const char* hn = "MYDATA.REC_D_64";
    ASSERT_TRUE(host_filename_to_oasis_deb(hn, &deb));
    EXPECT_EQ(strncmp(deb.file_name, "MYDATA  ", FNAME_LEN), 0);
    EXPECT_EQ(strncmp(deb.file_type, "REC     ", FTYPE_LEN), 0);
    EXPECT_EQ(deb.file_format, FILE_FORMAT_DIRECT);
    EXPECT_EQ(deb.file_format_dependent1, 64);
    EXPECT_EQ(deb.file_format_dependent2, 0); /* Should be zero for Direct */
}


TEST_F(OasisDebTest, HostToDebFilenameRelocatableOnlyRecLen) {
    const char* hn = "RELONLY.REC_R_256";
    ASSERT_TRUE(host_filename_to_oasis_deb(hn, &deb));
    EXPECT_EQ(strncmp(deb.file_name, "RELONLY ", FNAME_LEN), 0);
    EXPECT_EQ(strncmp(deb.file_type, "REC     ", FTYPE_LEN), 0);
    EXPECT_EQ(deb.file_format, FILE_FORMAT_RELOCATABLE);
    EXPECT_EQ(deb.file_format_dependent1, 256);
    EXPECT_EQ(deb.file_format_dependent2, 0); /* Program length should be set by caller */
}

TEST_F(OasisDebTest, HostToDebFilenameAbsolute) {
    const char* hn = "ABSPROG.ABS_A_256_1A00";
    ASSERT_TRUE(host_filename_to_oasis_deb(hn, &deb));
    EXPECT_EQ(strncmp(deb.file_name, "ABSPROG ", FNAME_LEN), 0);
    EXPECT_EQ(strncmp(deb.file_type, "ABS     ", FTYPE_LEN), 0);
    EXPECT_EQ(deb.file_format, FILE_FORMAT_ABSOLUTE);
    EXPECT_EQ(deb.file_format_dependent1, 256);
    EXPECT_EQ(deb.file_format_dependent2, 0x1A00); /* Load address */
}

TEST_F(OasisDebTest, HostToDebFilenameIndexedOnlyRecKeyLen) {
    const char* hn = "IDXONLY.DAT_IR_128_12"; /* Read-protected attribute added */
    ASSERT_TRUE(host_filename_to_oasis_deb(hn, &deb));
    EXPECT_EQ(strncmp(deb.file_name, "IDXONLY ", FNAME_LEN), 0);
    EXPECT_EQ(strncmp(deb.file_type, "DAT     ", FTYPE_LEN), 0);
    EXPECT_EQ(deb.file_format, FILE_FORMAT_INDEXED | FILE_FORMAT_READ_PROTECTED);
    EXPECT_EQ(deb.file_format_dependent1, (12 << 9) | 128);
    EXPECT_EQ(deb.file_format_dependent2, 0); /* Allocated size should be set by caller */
}

TEST_F(OasisDebTest, HostToDebFilenameKeyedOnlyRecKeyLen) {
    const char* hn = "KEYONLY.KEY_KWD_64_8";
    ASSERT_TRUE(host_filename_to_oasis_deb(hn, &deb));
    EXPECT_EQ(strncmp(deb.file_name, "KEYONLY ", FNAME_LEN), 0);
    EXPECT_EQ(strncmp(deb.file_type, "KEY     ", FTYPE_LEN), 0);
    EXPECT_EQ(deb.file_format, FILE_FORMAT_KEYED | FILE_FORMAT_WRITE_PROTECTED | FILE_FORMAT_DELETE_PROTECTED);
    EXPECT_EQ(deb.file_format_dependent1, (8 << 9) | 64);
    EXPECT_EQ(deb.file_format_dependent2, 0);
}

TEST_F(OasisDebTest, InvalidHostFilenames) {
    /* Case 1: "INVALID" (no dot, no type) -> Should now parse as Sequential */
    ASSERT_TRUE(host_filename_to_oasis_deb("INVALID", &deb)) << "Filename 'INVALID' should now parse as Sequential.";
    EXPECT_EQ(strncmp(deb.file_name, "INVALID ", FNAME_LEN), 0);
    EXPECT_EQ(strncmp(deb.file_type, "        ", FTYPE_LEN), 0);
    EXPECT_EQ(deb.file_format, FILE_FORMAT_SEQUENTIAL);
    EXPECT_EQ(deb.file_format_dependent1, 0);

    /* Case 2: "FNAMEONLY" (9 chars) -> Fails because FNAME_LEN is 8 */
    memset(&deb, 0, sizeof(deb));
    EXPECT_FALSE(host_filename_to_oasis_deb("FNAMEONLY", &deb)) << "'FNAMEONLY' (9 chars) should fail due to FNAME length.";

    /* Case 3: "FNAMEONL" (8 chars) -> Should parse as Sequential */
    memset(&deb, 0, sizeof(deb));
    ASSERT_TRUE(host_filename_to_oasis_deb("FNAMEONL", &deb));
    EXPECT_EQ(strncmp(deb.file_name, "FNAMEONL", FNAME_LEN), 0);
    EXPECT_EQ(strncmp(deb.file_type, "        ", FTYPE_LEN), 0); /* No type specified */
    EXPECT_EQ(deb.file_format, FILE_FORMAT_SEQUENTIAL);
    EXPECT_EQ(deb.file_format_dependent1, 0);

    /* Case 4: "FNAME.TYPEONLY" -> Should parse as Sequential */
    memset(&deb, 0, sizeof(deb));
    ASSERT_TRUE(host_filename_to_oasis_deb("FNAME.TYPEONLY", &deb));
    EXPECT_EQ(strncmp(deb.file_name, "FNAME   ", FNAME_LEN), 0);
    EXPECT_EQ(strncmp(deb.file_type, "TYPEONLY", FTYPE_LEN), 0); /* TYPEONLY is 8 chars, fits */
    EXPECT_EQ(deb.file_format, FILE_FORMAT_SEQUENTIAL);
    EXPECT_EQ(deb.file_format_dependent1, 0);


    /* These should still be invalid based on their metadata suffixes */
    EXPECT_FALSE(host_filename_to_oasis_deb("FNAME.TYPE_X_128", &deb)); /* Invalid type char */
    EXPECT_FALSE(host_filename_to_oasis_deb("FNAME.TYPE_S_ABC", &deb)); /* Invalid number for sequential */
    EXPECT_FALSE(host_filename_to_oasis_deb("FNAME.TYPE_SX_128", &deb)); /* Invalid attribute char */
    EXPECT_FALSE(host_filename_to_oasis_deb("FNAME.TYPE_S_128_EXTRA", &deb)); /* Extra part after sequential's record length */
    EXPECT_FALSE(host_filename_to_oasis_deb("FNAME.TYPE_A_256_NOTHEX", &deb)); /* Non-hex load address for Absolute */
    EXPECT_FALSE(host_filename_to_oasis_deb("RELPROG.COM_R_256_1024_EXTRA", &deb));
    EXPECT_FALSE(host_filename_to_oasis_deb("IDXFILE.IDX_I_80_10_2048_EXTRA", &deb));
    EXPECT_FALSE(host_filename_to_oasis_deb(nullptr, &deb));
    EXPECT_FALSE(host_filename_to_oasis_deb("FNAME.TYPE_S_128", nullptr));

    /* Case 5: Filename with only a dot -> should fail due to invalid FNAME part */
    memset(&deb, 0, sizeof(deb));
    EXPECT_FALSE(host_filename_to_oasis_deb(".", &deb)) << "Filename '.' should be invalid (empty FNAME part).";

    /* Case 6: Filename starting with a dot, like ".PROFILE" */
    memset(&deb, 0, sizeof(deb));
    ASSERT_TRUE(host_filename_to_oasis_deb(".PROFILE", &deb)) << "Filename '.PROFILE' should parse with empty FNAME.";
    EXPECT_EQ(strncmp(deb.file_name, "        ", FNAME_LEN), 0); /* FNAME becomes empty */
    EXPECT_EQ(strncmp(deb.file_type, "PROFILE ", FTYPE_LEN), 0); /* FTYPE becomes PROFILE */
    EXPECT_EQ(deb.file_format, FILE_FORMAT_SEQUENTIAL);
    EXPECT_EQ(deb.file_format_dependent1, 0);
}
