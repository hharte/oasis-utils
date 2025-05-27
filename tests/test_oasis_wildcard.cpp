/* tests/test_oasis_wildcard.cpp */
/* GTest unit tests for the oasis_filename_wildcard_match function in oasis_wildcard.c */

#include "gtest/gtest.h"
#include <cstring> /* For strncpy, memset, strlen, memcpy */
#include <cctype>  /* For toupper */


/* DUT header */
#include "oasis.h"

class OasisWildcardMatchTest : public ::testing::Test {
protected:
    char fname_deb[FNAME_LEN + 1]; /* Buffer for DEB filename + null */
    char ftype_deb[FTYPE_LEN + 1]; /* Buffer for DEB filetype + null */

    /* Helper to set fname_deb and ftype_deb simulating raw DEB fields (space-padded) */
    void SetDebName(const char* name_part, const char* type_part) {
        size_t len;

        memset(fname_deb, ' ', FNAME_LEN);
        fname_deb[FNAME_LEN] = '\0'; /* Null-terminate C-string buffer */
        if (name_part && *name_part) { /* Check for NULL or empty string */
            len = strlen(name_part);
            if (len > FNAME_LEN) len = FNAME_LEN;
            memcpy(fname_deb, name_part, len);
        }

        memset(ftype_deb, ' ', FTYPE_LEN);
        ftype_deb[FTYPE_LEN] = '\0'; /* Null-terminate C-string buffer */
        if (type_part && *type_part) { /* Check for NULL or empty string */
            len = strlen(type_part);
            if (len > FTYPE_LEN) len = FTYPE_LEN;
            memcpy(ftype_deb, type_part, len);
        }
    }
};

/* --- Exact Matches (No Wildcards) --- */
TEST_F(OasisWildcardMatchTest, ExactMatch) {
    SetDebName("TESTPROG", "BAS");
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "TESTPROG.BAS"));
}

TEST_F(OasisWildcardMatchTest, ExactMatchShortName) {
    SetDebName("FILE", "TXT");
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "FILE.TXT"));
}

TEST_F(OasisWildcardMatchTest, ExactMatchNoExtensionDeb) {
    SetDebName("EXECUTE", NULL); /* Simulate no extension effectively */
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "EXECUTE."));
}

TEST_F(OasisWildcardMatchTest, ExactMatchNoExtensionPattern) {
    SetDebName("MYPROG", "   ");
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "MYPROG."));
}


TEST_F(OasisWildcardMatchTest, ExactMatchFailName) {
    SetDebName("TESTPROG", "BAS");
    EXPECT_FALSE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "WRONG.BAS"));
}

TEST_F(OasisWildcardMatchTest, ExactMatchFailExtension) {
    SetDebName("TESTPROG", "BAS");
    EXPECT_FALSE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "TESTPROG.COM"));
}

TEST_F(OasisWildcardMatchTest, ExactMatchCaseInsensitive) {
    SetDebName("TestProg", "bas");
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "TESTPROG.BAS"));
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "testprog.bas"));
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "TeStPrOg.BaS"));
}

/* --- Star Wildcard Tests --- */
TEST_F(OasisWildcardMatchTest, StarAtEnd) {
    SetDebName("AUTOEXEC", "BAT");
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "AUTOEXEC.*"));
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "AUTO*"));
    EXPECT_FALSE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "AUTOEXED.*"));
}

TEST_F(OasisWildcardMatchTest, StarAtBeginning) {
    SetDebName("MYFILE", "TXT");
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "*.TXT"));
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "*FILE.TXT"));
    EXPECT_FALSE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "*.DAT"));
}

TEST_F(OasisWildcardMatchTest, StarInMiddle) {
    SetDebName("TEXTEDIT", "COM"); /* Effective: TEXTEDIT.COM */
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "TEXT*COM"));
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "T*TEDIT.COM"));
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "TEX*EDIT.COM"));
}

TEST_F(OasisWildcardMatchTest, MultipleStars) {
    SetDebName("PROGNAME", "EXT");
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "*PROG*.EXT"));
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "P*N*E.E*T"));
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "*.*"));
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "**PROG**NAME.**E**X**T**"));
}

TEST_F(OasisWildcardMatchTest, StarMatchesZeroChars) {
    SetDebName("FILENAME", "TYP");
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "FILENAME*.TYP"));
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "*FILENAME.TYP"));
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "FILE*NAME.TYP"));
}

TEST_F(OasisWildcardMatchTest, StarOnlyPattern) {
    SetDebName("ANYTHING", "EXT");
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "*"));
}

TEST_F(OasisWildcardMatchTest, StarWithDot) {
    SetDebName("HELLO", "WORLD"); /* HELLO.WORLD */
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "HELLO.*"));
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "*.WORLD"));
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "HELLO*WORLD")); /* '*' should match '.' */
}

/* --- Question Mark Wildcard Tests --- */
TEST_F(OasisWildcardMatchTest, QuestionMarkSimple) {
    SetDebName("FILE1", "DAT"); /* FILE1.DAT */
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "FILE?.DAT"));
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "FILE1.DA?"));
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "FIL??.DAT"));
}

TEST_F(OasisWildcardMatchTest, MultipleQuestionMarks) {
    SetDebName("REPORT", "TXT"); /* REPORT.TXT */
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "REP??T.T?T"));
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "REP???.TXT"));
}

TEST_F(OasisWildcardMatchTest, QuestionMarkAtExtremes) {
    SetDebName("TEST", "EXE");
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "?EST.EXE"));
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "TEST.?XE"));
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "TES?.EX?"));
}

/* --- Mixed Wildcard Tests --- */
TEST_F(OasisWildcardMatchTest, StarAndQuestionMark) {
    SetDebName("MYPROG1", "ASM");
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "MYPROG?.A*M"));
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "M*PROG?.?S?"));
    EXPECT_FALSE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "MYPROG??.A*M"));
}

#if 1 /* Failing test */
TEST_F(OasisWildcardMatchTest, ComplexPattern) {
    SetDebName("BACKUP_V1", "AUG"); /* BACKUP_V1.AUG */
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "B?CK*V.*G"));
    EXPECT_FALSE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "B?CK*V.F*G"));
}
#endif /* 0 */

/* --- Edge Cases --- */
TEST_F(OasisWildcardMatchTest, EmptyPattern) {
    SetDebName("NONEMPTY", "FIL");
    EXPECT_FALSE(oasis_filename_wildcard_match(fname_deb, ftype_deb, ""));
}

TEST_F(OasisWildcardMatchTest, EmptyDebNameAndType) {
    SetDebName("", ""); /* Effective filename "." */
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "."));
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "*.*"));
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "?"));
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "*"));
    EXPECT_FALSE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "A.B"));
}

TEST_F(OasisWildcardMatchTest, PatternLongerThanFilename) {
    SetDebName("SHORT", "S"); /* SHORT.S */
    EXPECT_FALSE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "SHORTNAME.S"));
    EXPECT_FALSE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "SHORT.SUPER"));
}

TEST_F(OasisWildcardMatchTest, PatternShorterThanFilenameWithNoWildcards) {
    SetDebName("LONGFILENAME", "EXT");
    EXPECT_FALSE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "LONG.EXT"));
}

TEST_F(OasisWildcardMatchTest, SpacePaddingEffect) {
    SetDebName("TEST", "BAS");
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "TEST.BAS"));
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "TEST*.BAS"));

    SetDebName("SPACES  ", "IN ");
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "SPACES.IN"));
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "SPACES.*"));
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "*.IN"));
}

TEST_F(OasisWildcardMatchTest, PatternWithSpaces) {
    SetDebName("TEST", "BAS");
    EXPECT_FALSE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "TEST .BAS"));
    EXPECT_FALSE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "TEST. BAS"));
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "TEST.BAS"));
    EXPECT_FALSE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "TEST.BAS  "));
}


TEST_F(OasisWildcardMatchTest, NoDotInPattern) {
    SetDebName("FILENAME", "EXT");
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "FILENAME*"));
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "*EXT"));
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "F*E*T"));
    EXPECT_FALSE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "FILENAMETYPE"));
}

TEST_F(OasisWildcardMatchTest, OnlyDotInPattern) {
    SetDebName("", "");
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "."));
    SetDebName("F", NULL);
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "F."));
    SetDebName(NULL, "E");
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, ".E"));
    EXPECT_FALSE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "."));
}

TEST_F(OasisWildcardMatchTest, DebWithNoExtensionVersusPatternWithExtension) {
    SetDebName("NOEXT", NULL);
    EXPECT_FALSE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "NOEXT.TXT"));
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "NOEXT.*"));
    EXPECT_FALSE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "NOEXT.?"));
}

TEST_F(OasisWildcardMatchTest, StarFollowedByMismatch) {
    SetDebName("ABC", "DEF"); /* ABC.DEF */
    EXPECT_FALSE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "AB*X")); /* '*' matches C.D, but then E != X */
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "AB*F"));  /* '*' matches C.DE */
    EXPECT_TRUE(oasis_filename_wildcard_match(fname_deb, ftype_deb, "*C.D*"));
}
