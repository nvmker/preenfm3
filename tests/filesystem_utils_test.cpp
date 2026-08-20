// Host-side coverage for firmware/Src/filesystem/FileSystemUtils.cpp.
//
// Pure string/number helpers shared by every filesystem parser. This is a
// CHARACTERIZATION suite: it locks current behavior (including the quirks
// flagged in spec-test-coverage-phase4) rather than intended behavior.
//
// Known quirks pinned here (see deferred-work.md):
//   * stof() scans FORWARD past non-numeric bytes WITHOUT stopping at NUL —
//     the "unbounded scan". Fixtures therefore always place a digit later in
//     the same buffer (mirroring the firmware's lineBuffer usage, where the
//     whole 1024/512-byte buffer is in scope).
//   * str_cmp/strcmp treat '_' as a terminator-equivalent (strcmp returns 0
//     for "ab_" vs "ab") — the bank-name padding interaction.
#include "gtest/gtest.h"

#include "FileSystemUtils.h"

#include <cmath>
#include <cstring>

class FileSystemUtilsTest : public ::testing::Test {
protected:
    FileSystemUtils fsu;
    char lineBuffer[1024];  // mirrors the firmware's shared parse buffer
};

TEST_F(FileSystemUtilsTest, CopyCopiesExactLength) {
    char dst[8] = {0};
    fsu.copy(dst, "abcdef", 4);
    EXPECT_STREQ(dst, "abcd");
}

TEST_F(FileSystemUtilsTest, AddNumberWritesThreeDigits) {
    char name[8] = "img";
    fsu.addNumber(name, 3, 7);
    EXPECT_STREQ(name, "img007");
    fsu.addNumber(name, 3, 42);
    EXPECT_STREQ(name, "img042");
}

TEST_F(FileSystemUtilsTest, StrlenCountsUntilNul) {
    EXPECT_EQ(fsu.strlen(""), 0);
    EXPECT_EQ(fsu.strlen("abc"), 3);
}

TEST_F(FileSystemUtilsTest, CopyStringReturnsCountAndTerminates) {
    char target[16];
    EXPECT_EQ(fsu.copy_string(target, "hello"), 5);
    EXPECT_STREQ(target, "hello");
}

TEST_F(FileSystemUtilsTest, PrintIntWidths) {
    // printInt does NOT NUL-terminate (firmware writes into storageBuffer);
    // assert by return count + exact bytes.
    char buf[8] = {0};
    EXPECT_EQ(fsu.printInt(buf, 7), 1);
    EXPECT_EQ(std::memcmp(buf, "7", 1), 0);
    EXPECT_EQ(fsu.printInt(buf, 42), 2);
    EXPECT_EQ(std::memcmp(buf, "42", 2), 0);
    EXPECT_EQ(fsu.printInt(buf, 421), 3);
    EXPECT_EQ(std::memcmp(buf, "421", 3), 0);
    EXPECT_EQ(fsu.printInt(buf, 4213), 4);
    EXPECT_EQ(std::memcmp(buf, "4213", 4), 0);
    EXPECT_EQ(fsu.printInt(buf, 0), 1);
    EXPECT_EQ(std::memcmp(buf, "0", 1), 0);
    // >= 10000: the else-branch computes i/1000 = 12 -> '0'+12 = '<'
    // (characterization: overflow past 9999 emits garbage digits)
    EXPECT_EQ(fsu.printInt(buf, 12345), 4);
    EXPECT_EQ(std::memcmp(buf, "<345", 4), 0);
}

TEST_F(FileSystemUtilsTest, PrintFloatOneDecimalTruncating) {
    char buf[8] = {0};  // no NUL written by printFloat either
    EXPECT_EQ(fsu.printFloat(buf, 0.5f), 3);
    EXPECT_EQ(std::memcmp(buf, "0.5", 3), 0);
    EXPECT_EQ(fsu.printFloat(buf, 3.146f), 3);
    EXPECT_EQ(std::memcmp(buf, "3.1", 3), 0);  // truncation, not rounding
}

TEST_F(FileSystemUtilsTest, GetPositionOfEqualFindsFirstWithin20) {
    char line[32];
    strcpy(line, "key=value=2");
    EXPECT_EQ(fsu.getPositionOfEqual(line), 3);
    memset(line, 'x', 21); line[21] = 0;
    EXPECT_EQ(fsu.getPositionOfEqual(line), -1);  // beyond the 20-char window
}

TEST_F(FileSystemUtilsTest, GetKeyStopsAtSpaceOrEqual) {
    char key[24];
    fsu.getKey("midiChannel=3", key);
    EXPECT_STREQ(key, "midiChannel");
    fsu.getKey("short ", key);
    EXPECT_STREQ(key, "short");
}

TEST_F(FileSystemUtilsTest, ToIntStopsAtFirstNonDigit) {
    EXPECT_EQ(fsu.toInt("123"), 123);
    EXPECT_EQ(fsu.toInt("12x9"), 12);
    EXPECT_EQ(fsu.toInt("x12"), 0);
}

TEST_F(FileSystemUtilsTest, ToFloatParsesFractionTruncating) {
    EXPECT_FLOAT_EQ(fsu.toFloat("2"), 2.0f);
    EXPECT_FLOAT_EQ(fsu.toFloat("2.5"), 2.5f);
    EXPECT_FLOAT_EQ(fsu.toFloat("0.25"), 0.25f);
    EXPECT_FLOAT_EQ(fsu.toFloat("x"), 0.0f);
}

TEST_F(FileSystemUtilsTest, GetValueExtractsNumericRun) {
    // NOTE: only ' '/'\t' are skipped; '=' is NOT a separator (firmware call
    // sites pass line+equalPos+1, so the '=' is already gone).
    char v[24];
    fsu.getValue("  42 rest", v);
    EXPECT_STREQ(v, "42");
    fsu.getValue("abc", v);
    EXPECT_STREQ(v, "");
}

TEST_F(FileSystemUtilsTest, GetFloatValueKeepsDot) {
    char v[24];
    fsu.getFloatValue("  4.75x", v);
    EXPECT_STREQ(v, "4.75");
}

TEST_F(FileSystemUtilsTest, GetTextValueRangeIs43To126) {
    char v[24];
    fsu.getTextValue("  Sub1_dir", v);
    EXPECT_STREQ(v, "Sub1_dir");
    fsu.getTextValue("  space", v);  // leading space is skipped as separator
    EXPECT_STREQ(v, "space");
    fsu.getTextValue("  a'b", v);    // apostrophe (39) < 43 -> stops
    EXPECT_STREQ(v, "a");
}

TEST_F(FileSystemUtilsTest, StrCmpIsStandardByteOrder) {
    EXPECT_EQ(fsu.str_cmp("abc", "abc"), 0);
    EXPECT_EQ(fsu.str_cmp("abc", "abd"), -1);
    EXPECT_EQ(fsu.str_cmp("abd", "abc"), 1);
}

TEST_F(FileSystemUtilsTest, StrcmpTreatsUnderscoreAsTerminator) {
    // Quirk golden: initFiles pads dotless names with '~'... but bank names
    // from save flows can carry '_'; strcmp treats '_' as 0, so "ab_" and
    // "ab" compare EQUAL.
    EXPECT_EQ(fsu.strcmp("ab_", "ab"), 0);
    EXPECT_EQ(fsu.strcmp("ab", "ab"), 0);
    EXPECT_NE(fsu.strcmp("a_b", "ab"), 0);  // '_' vs NUL-ish mismatch path
}

TEST_F(FileSystemUtilsTest, GetLineSplitsOnNewlineAndSkipsSeparators) {
    char line[160];
    strcpy(lineBuffer, "first line\nsecond\r\nthird");
    int consumed = fsu.getLine(lineBuffer, line);
    EXPECT_STREQ(line, "first line");
    EXPECT_EQ(consumed, 11);  // skip loop consumes the '\n'; "second" @11
    consumed = fsu.getLine(lineBuffer + consumed, line);
    EXPECT_STREQ(line, "second");
    EXPECT_EQ(consumed, 8);   // "\r\nthird" -> "third" at rel offset 8
}

TEST_F(FileSystemUtilsTest, GetLineReturnsMinusOneAtBufferEnd) {
    // FIXED (spec 2.5): getLine stops at NUL, so a short final line scans
    // only its own bytes before returning the end-of-buffer -1.
    char line[160];
    memset(lineBuffer, 0, 160);
    strcpy(lineBuffer, "tail");
    EXPECT_EQ(fsu.getLine(lineBuffer, line), -1);
    EXPECT_STREQ(line, "tail");
}

TEST_F(FileSystemUtilsTest, GetLineExactly128CharLine) {
    char line[160];
    memset(lineBuffer, 'A', 128);
    lineBuffer[128] = '\n';
    lineBuffer[129] = 0;
    int consumed = fsu.getLine(lineBuffer, line);
    EXPECT_EQ(consumed, -1);           // only the newline left, then NUL
    EXPECT_EQ(fsu.strlen(line), 128);  // full 128 chars copied
    EXPECT_EQ(line[127], 'A');
}

TEST_F(FileSystemUtilsTest, CopyFloatCopiesNFloats) {
    float src[4] = {1.5f, -2.5f, 3.25f, 4.0f};
    float dst[4] = {0};
    fsu.copyFloat(src, dst, 3);
    EXPECT_FLOAT_EQ(dst[2], 3.25f);
    EXPECT_FLOAT_EQ(dst[3], 0.0f);  // not copied
}

TEST_F(FileSystemUtilsTest, GetPositionOfSlashAndPeriod) {
    EXPECT_EQ(fsu.getPositionOfSlash("3/2"), 1);
    EXPECT_EQ(fsu.getPositionOfSlash("32"), -1);
    EXPECT_EQ(fsu.getPositionOfPeriod("100.5"), 3);
    EXPECT_EQ(fsu.getPositionOfPeriod("1005"), -1);
}

TEST_F(FileSystemUtilsTest, StofParsesPositiveNegativeAndDecimal) {
    int read = 0;
    EXPECT_FLOAT_EQ(fsu.stof("3.5", read), 3.5f);
    EXPECT_EQ(read, 3);
    EXPECT_FLOAT_EQ(fsu.stof("-2.25", read), -2.25f);
    EXPECT_EQ(read, 5);
    EXPECT_FLOAT_EQ(fsu.stof("12", read), 12.0f);
    EXPECT_FLOAT_EQ(fsu.stof("0.125", read), 0.125f);
}

TEST_F(FileSystemUtilsTest, StofSecondDotIsIgnoredAsDigit) {
    // isNumber() includes '.', so "1.2.3" parses: second '.' yields
    // d = '.'-'0' < 0 -> skipped without advancing fact. Characterization.
    int read = 0;
    // '1' '2' accumulate with fact=0.1 then 0.01 -> 123 * 0.01 = 1.23
    EXPECT_FLOAT_EQ(fsu.stof("1.2.3", read), 1.23f);
    EXPECT_EQ(read, 5);
}

TEST_F(FileSystemUtilsTest, StofStopsAtNul) {
    // FIXED (spec 2.5): the skip loop `while (*s != 0 && !isNumber(*s))`
    // stops at NUL. "abc\0...42" inside the 1024-byte parse buffer yields
    // 0.0f with charRead at the NUL — it no longer scans past the terminator
    // and picks up unrelated bytes further into the buffer.
    memset(lineBuffer, 0, sizeof(lineBuffer));
    strcpy(lineBuffer, "abc");
    lineBuffer[100] = '4';
    lineBuffer[101] = '2';
    int read = 0;
    EXPECT_FLOAT_EQ(fsu.stof(lineBuffer, read), 0.0f);
    EXPECT_EQ(read, 3);
    // A NUL-terminated valid number still parses normally.
    strcpy(lineBuffer, "42");
    EXPECT_FLOAT_EQ(fsu.stof(lineBuffer, read), 42.0f);
    EXPECT_EQ(read, 2);
}

TEST_F(FileSystemUtilsTest, IsNumberAndIsSeparatorRanges) {
    EXPECT_TRUE(fsu.isNumber('0'));
    EXPECT_TRUE(fsu.isNumber('.'));
    EXPECT_TRUE(fsu.isNumber('-'));
    EXPECT_FALSE(fsu.isNumber(' '));
    EXPECT_FALSE(fsu.isNumber(0));
    EXPECT_TRUE(fsu.isSeparator(' '));
    EXPECT_TRUE(fsu.isSeparator('\t'));
    EXPECT_TRUE(fsu.isSeparator('\r'));
    EXPECT_TRUE(fsu.isSeparator('\n'));
    EXPECT_FALSE(fsu.isSeparator('x'));
}
