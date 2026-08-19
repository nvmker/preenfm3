// Host-side unit tests for the Phase-4 FatFs SHIM itself
// (tests/host_shims/fatfs.h + fatfs_impl.cpp — see spec
// _bmad-output/implementation-artifacts/spec-test-coverage-phase4.md).
//
// These tests PIN the shim's own FR_* semantics before any firmware TU is
// allowed to sit on top of it: every filesystem-TU characterization golden
// below (banks, parsers, PPMImage) inherits its determinism from THIS layer.
// If a firmware test disagrees with reality, the bug must be in the firmware
// or in the fixture — never quietly absorbed into an under-specified shim.
//
// Semantics contract (mirrors tests/host_shims/fatfs.h "FIDELITY CONTRACT"):
//   * f_open(FA_READ) missing file                 -> FR_NO_FILE
//   * f_open(FA_WRITE|FA_OPEN_ALWAYS), missing
//     parent dir                                   -> FR_NO_PATH
//   * FA_OPEN_ALWAYS|FA_WRITE creates-or-opens at fptr=0 (no append-on-open);
//     firmware appends via explicit f_lseek (zero-extending)
//   * f_read clamps at EOF (short read: *br < btr)
//   * f_write at fptr<size overwrites in place; past EOF zero-extends
//   * f_opendir/f_readdir iterate SORTED keys; dirs get AM_DIR; end-of-dir
//     is fname[0]==0
//   * f_stat missing                               -> FR_NO_FILE
//   * f_write on a read-mode FIL                   -> FR_DENIED
//   * f_* on a closed/never-opened FIL             -> FR_INVALID_OBJECT
#include "gtest/gtest.h"

#include "fatfs.h"

#include <cstring>
#include <string>
#include <vector>

class FatfsShimTest : public ::testing::Test {
protected:
    void SetUp() override { fatfsShimReset(); }
};

TEST_F(FatfsShimTest, OpenReadCloseRoundTrip) {
    fatfsShimInjectString("0:/pfm3/Settings.txt", "hello world");
    FIL f;
    ASSERT_EQ(f_open(&f, "0:/pfm3/Settings.txt", FA_READ), FR_OK);
    EXPECT_EQ(f_size(&f), 11u);
    char buf[32] = {0};
    UINT br = 0;
    ASSERT_EQ(f_read(&f, buf, sizeof(buf), &br), FR_OK);
    EXPECT_EQ(br, 11u);
    EXPECT_STREQ(buf, "hello world");
    EXPECT_EQ(f_close(&f), FR_OK);
}

TEST_F(FatfsShimTest, OpenMissingFileForReadIsNoFile) {
    FIL f;
    EXPECT_EQ(f_open(&f, "0:/pfm3/nope.bin", FA_READ), FR_NO_FILE);
}

TEST_F(FatfsShimTest, OpenForWriteWithMissingParentIsNoPath) {
    FIL f;
    EXPECT_EQ(f_open(&f, "0:/nosuchdir/new.bin", FA_OPEN_ALWAYS | FA_WRITE), FR_NO_PATH);
}

TEST_F(FatfsShimTest, OpenAlwaysCreatesEmptyFileAndWrites) {
    fatfsShimMkdir("0:/pfm3");
    FIL f;
    ASSERT_EQ(f_open(&f, "0:/pfm3/new.bin", FA_OPEN_ALWAYS | FA_WRITE), FR_OK);
    EXPECT_EQ(f_size(&f), 0u);
    const uint8_t payload[4] = {1, 2, 3, 4};
    UINT bw = 0;
    ASSERT_EQ(f_write(&f, payload, 4, &bw), FR_OK);
    EXPECT_EQ(bw, 4u);
    ASSERT_EQ(f_close(&f), FR_OK);
    EXPECT_EQ(fatfsShimFileSize("0:/pfm3/new.bin"), 4u);
}

TEST_F(FatfsShimTest, ReopenDoesNotAppendButOverwritesFromZero) {
    // FA_OPEN_ALWAYS|FA_WRITE re-opens with fptr=0 — the firmware's
    // save-default flows rely on rewriting from the start.
    fatfsShimMkdir("0:/pfm3");
    fatfsShimInjectString("0:/pfm3/a.bin", "AAAA");
    FIL f;
    ASSERT_EQ(f_open(&f, "0:/pfm3/a.bin", FA_OPEN_ALWAYS | FA_WRITE), FR_OK);
    EXPECT_EQ(f.fptr, 0u);
    UINT bw = 0;
    ASSERT_EQ(f_write(&f, "BB", 2, &bw), FR_OK);
    ASSERT_EQ(f_close(&f), FR_OK);
    std::vector<uint8_t> out;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/a.bin", out));
    EXPECT_EQ(out, (std::vector<uint8_t>{'B', 'B', 'A', 'A'}));
}

TEST_F(FatfsShimTest, LseekPastEndZeroExtendsOnWrite) {
    fatfsShimMkdir("0:/pfm3");
    FIL f;
    ASSERT_EQ(f_open(&f, "0:/pfm3/patch.bin", FA_OPEN_ALWAYS | FA_WRITE), FR_OK);
    ASSERT_EQ(f_lseek(&f, 8), FR_OK);
    UINT bw = 0;
    ASSERT_EQ(f_write(&f, "XY", 2, &bw), FR_OK);
    ASSERT_EQ(f_close(&f), FR_OK);
    std::vector<uint8_t> out;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/patch.bin", out));
    EXPECT_EQ(out, (std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 0, 0, 'X', 'Y'}));
    EXPECT_EQ(fatfsShimFileSize("0:/pfm3/patch.bin"), 10u);
}

TEST_F(FatfsShimTest, ReadClampsAtEofWithShortRead) {
    fatfsShimInjectString("0:/f.bin", "abc");
    FIL f;
    ASSERT_EQ(f_open(&f, "0:/f.bin", FA_READ), FR_OK);
    ASSERT_EQ(f_lseek(&f, 2), FR_OK);
    char buf[16] = {0};
    UINT br = 99;
    ASSERT_EQ(f_read(&f, buf, 16, &br), FR_OK);
    EXPECT_EQ(br, 1u);              // only 1 byte left
    EXPECT_EQ(buf[0], 'c');
    EXPECT_EQ(f_close(&f), FR_OK);
}

TEST_F(FatfsShimTest, SeekPastEofThenReadIsEmpty) {
    fatfsShimInjectString("0:/f.bin", "abc");
    FIL f;
    ASSERT_EQ(f_open(&f, "0:/f.bin", FA_READ), FR_OK);
    ASSERT_EQ(f_lseek(&f, 100), FR_OK);
    UINT br = 5;
    ASSERT_EQ(f_read(&f, nullptr, 5, &br), FR_OK);
    EXPECT_EQ(br, 0u);
    EXPECT_EQ(f_close(&f), FR_OK);
}

TEST_F(FatfsShimTest, WriteToReadModeFileIsDenied) {
    fatfsShimInjectString("0:/f.bin", "abc");
    FIL f;
    ASSERT_EQ(f_open(&f, "0:/f.bin", FA_READ), FR_OK);
    UINT bw = 9;
    EXPECT_EQ(f_write(&f, "z", 1, &bw), FR_DENIED);
    EXPECT_EQ(bw, 0u);
    EXPECT_EQ(f_close(&f), FR_OK);
}

TEST_F(FatfsShimTest, ReadFromWriteModeFileIsDenied) {
    fatfsShimInjectString("0:/f.bin", "abc");
    FIL f;
    ASSERT_EQ(f_open(&f, "0:/f.bin", FA_WRITE), FR_OK);
    char value = 0;
    UINT br = 9;
    EXPECT_EQ(f_read(&f, &value, 1, &br), FR_DENIED);
    EXPECT_EQ(br, 0u);
    EXPECT_EQ(value, 0);
    EXPECT_EQ(f_close(&f), FR_OK);
}

TEST_F(FatfsShimTest, OperationsOnClosedFileAreInvalidObject) {
    fatfsShimInjectString("0:/f.bin", "abc");
    FIL f;
    ASSERT_EQ(f_open(&f, "0:/f.bin", FA_READ), FR_OK);
    ASSERT_EQ(f_close(&f), FR_OK);
    UINT br = 0;
    EXPECT_EQ(f_read(&f, nullptr, 1, &br), FR_INVALID_OBJECT);
    EXPECT_EQ(f_lseek(&f, 0), FR_INVALID_OBJECT);
    EXPECT_EQ(f_close(&f), FR_INVALID_OBJECT);
}

TEST_F(FatfsShimTest, FilCopyByValueKeepsWorkingLikePreenFMDisplayCreateFile) {
    // PreenFMFileType::createFile() returns the FIL BY VALUE; the copy must
    // still be usable for writes (shim identity lives inside the FIL).
    fatfsShimMkdir("0:/pfm3");
    FIL f;
    ASSERT_EQ(f_open(&f, "0:/pfm3/copy.bin", FA_OPEN_ALWAYS | FA_WRITE), FR_OK);
    FIL copy = f;  // by-value copy, like createFile()
    UINT bw = 0;
    ASSERT_EQ(f_write(&copy, "data", 4, &bw), FR_OK);
    ASSERT_EQ(f_close(&copy), FR_OK);
    EXPECT_EQ(fatfsShimFileSize("0:/pfm3/copy.bin"), 4u);
    // Original handle is now closed too (shared id) — matches the firmware's
    // single-static-FIL usage where only one is closed.
    EXPECT_EQ(f_write(&f, "x", 1, &bw), FR_INVALID_OBJECT);
}

TEST_F(FatfsShimTest, StatReportsSizeAndName) {
    fatfsShimInjectBytes("0:/pfm3/big.syx", "12345678", 8);
    FILINFO fi;
    ASSERT_EQ(f_stat("0:/pfm3/big.syx", &fi), FR_OK);
    EXPECT_EQ(fi.fsize, 8u);
    EXPECT_STREQ(fi.fname, "big.syx");
    EXPECT_EQ(fi.fattrib & AM_DIR, 0);
}

TEST_F(FatfsShimTest, StatMissingFileIsNoFile) {
    FILINFO fi;
    EXPECT_EQ(f_stat("0:/pfm3/none.ppm", &fi), FR_NO_FILE);
}

TEST_F(FatfsShimTest, UnlinkRemovesFile) {
    fatfsShimInjectString("0:/pfm3/x.tmp", "x");
    ASSERT_EQ(f_unlink("0:/pfm3/x.tmp"), FR_OK);
    EXPECT_FALSE(fatfsShimFileExists("0:/pfm3/x.tmp"));
    EXPECT_EQ(f_unlink("0:/pfm3/x.tmp"), FR_NO_FILE);
}

TEST_F(FatfsShimTest, RenameMovesAndChecksTarget) {
    fatfsShimInjectString("0:/pfm3/old.mix", "data");
    EXPECT_EQ(f_rename("0:/pfm3/old.mix", "0:/pfm3/old.mix"), FR_EXIST);
    EXPECT_EQ(f_rename("0:/pfm3/missing.mix", "0:/pfm3/new.mix"), FR_NO_FILE);
    ASSERT_EQ(f_rename("0:/pfm3/old.mix", "0:/pfm3/new.mix"), FR_OK);
    EXPECT_FALSE(fatfsShimFileExists("0:/pfm3/old.mix"));
    EXPECT_TRUE(fatfsShimFileExists("0:/pfm3/new.mix"));
    EXPECT_EQ(fatfsShimFileSize("0:/pfm3/new.mix"), 4u);
}

TEST_F(FatfsShimTest, RenameOverwritesExistingDestinationFile) {
    // Real-FatFs fidelity (fixed in bugfix-phase1 item 1.4): renaming onto an
    // existing destination FILE silently replaces it (same-volume atomic
    // swap — the primitive tmp-then-rename saves rely on). A destination
    // DIRECTORY stays FR_EXIST, and a same-path rename stays FR_EXIST.
    fatfsShimInjectString("0:/pfm3/a.tmp", "new-content");
    fatfsShimInjectString("0:/pfm3/a.bin", "stale-but-longer-content");
    fatfsShimMkdir("0:/pfm3/subdir");
    EXPECT_EQ(f_rename("0:/pfm3/a.tmp", "0:/pfm3/subdir"), FR_EXIST);
    ASSERT_EQ(f_rename("0:/pfm3/a.tmp", "0:/pfm3/a.bin"), FR_OK);
    EXPECT_FALSE(fatfsShimFileExists("0:/pfm3/a.tmp"));
    std::vector<uint8_t> out;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/a.bin", out));
    EXPECT_EQ(out, std::vector<uint8_t>({'n','e','w','-','c','o','n','t','e','n','t'}));
    EXPECT_EQ(fatfsShimFileSize("0:/pfm3/a.bin"), 11u);  // replaced, not appended
}

TEST_F(FatfsShimTest, FailNextIsOneShotAndResetClearsIt) {
    fatfsShimInjectString("0:/pfm3/f.bin", "x");
    fatfsShimFailNext("f_write", FR_DENIED);
    FIL f{};
    ASSERT_EQ(f_open(&f, "0:/pfm3/f.bin", FA_READ | FA_WRITE), FR_OK);
    UINT bw = 99;
    EXPECT_EQ(f_write(&f, "y", 1, &bw), FR_DENIED);
    EXPECT_EQ(bw, 0u);
    // one-shot: consumed above, the next write succeeds again
    EXPECT_EQ(f_write(&f, "y", 1, &bw), FR_OK);
    EXPECT_EQ(bw, 1u);
    EXPECT_EQ(f_close(&f), FR_OK);
    // reset clears pending injections
    fatfsShimFailNext("f_unlink", FR_WRITE_PROTECTED);
    fatfsShimReset();
    fatfsShimInjectString("0:/pfm3/f.bin", "x");
    EXPECT_EQ(f_unlink("0:/pfm3/f.bin"), FR_OK);
    // open-failure injection surfaces through PreenFMFileType-style flows
    fatfsShimFailNext("f_open", FR_NO_PATH);
    FIL g{};
    EXPECT_EQ(f_open(&g, "0:/pfm3/f.bin", FA_READ), FR_NO_PATH);
    EXPECT_EQ(f_open(&g, "0:/pfm3/f.bin", FA_READ), FR_NO_FILE);  // one-shot spent
}

TEST_F(FatfsShimTest, MkdirCreatesDirAndRejectsDuplicates) {
    ASSERT_EQ(f_mkdir("0:/PPM"), FR_OK);
    EXPECT_EQ(f_mkdir("0:/PPM"), FR_EXIST);
    EXPECT_EQ(f_mkdir("0:/nope/deep"), FR_NO_PATH);  // parent missing
}

TEST_F(FatfsShimTest, OpenDirMissingPathIsNoPath) {
    DIR d;
    EXPECT_EQ(f_opendir(&d, "0:/pfm3/dx7"), FR_NO_PATH);
}

TEST_F(FatfsShimTest, ReaddirIteratesSortedKeysWithEndMarker) {
    fatfsShimMkdir("0:/pfm3/dx7");
    fatfsShimInjectString("0:/pfm3/dx7/bank2.syx", "12");
    fatfsShimInjectString("0:/pfm3/dx7/bank10.syx", "12345678");
    fatfsShimMkdir("0:/pfm3/dx7/sub");

    DIR d;
    ASSERT_EQ(f_opendir(&d, "0:/pfm3/dx7"), FR_OK);
    FILINFO fi;
    // SORTED lexicographic order: bank10 < bank2 < sub ("1" < "2" < "s")
    ASSERT_EQ(f_readdir(&d, &fi), FR_OK);
    EXPECT_STREQ(fi.fname, "bank10.syx");
    EXPECT_EQ(fi.fsize, 8u);
    EXPECT_EQ(fi.fattrib & AM_DIR, 0);
    ASSERT_EQ(f_readdir(&d, &fi), FR_OK);
    EXPECT_STREQ(fi.fname, "bank2.syx");
    ASSERT_EQ(f_readdir(&d, &fi), FR_OK);
    EXPECT_STREQ(fi.fname, "sub");
    EXPECT_EQ(fi.fattrib & AM_DIR, AM_DIR);
    ASSERT_EQ(f_readdir(&d, &fi), FR_OK);
    EXPECT_EQ(fi.fname[0], 0);      // end-of-directory
    ASSERT_EQ(f_readdir(&d, &fi), FR_OK);
    EXPECT_EQ(fi.fname[0], 0);      // stays ended
    EXPECT_EQ(f_closedir(&d), FR_OK);
}

TEST_F(FatfsShimTest, ResetClearsEverything) {
    fatfsShimMkdir("0:/pfm3");
    fatfsShimInjectString("0:/pfm3/f.bin", "x");
    FIL f;
    ASSERT_EQ(f_open(&f, "0:/pfm3/f.bin", FA_READ), FR_OK);
    fatfsShimReset();
    EXPECT_EQ(fatfsShimFileCount(), 0u);
    EXPECT_FALSE(fatfsShimFileExists("0:/pfm3/f.bin"));
    DIR d;
    EXPECT_EQ(f_opendir(&d, "0:/pfm3"), FR_NO_PATH);
    UINT br = 0;
    EXPECT_EQ(f_read(&f, nullptr, 1, &br), FR_INVALID_OBJECT);  // table wiped
}

TEST_F(FatfsShimTest, ListDirHelperReturnsSortedChildren) {
    fatfsShimMkdir("0:/pfm3/waveform");
    fatfsShimInjectString("0:/pfm3/waveform/usr1.txt", "a");
    fatfsShimInjectString("0:/pfm3/waveform/usr2.bin", "ab");
    std::vector<std::string> kids = fatfsShimListDir("0:/pfm3/waveform");
    EXPECT_EQ(kids, (std::vector<std::string>{"usr1.txt", "usr2.bin"}));
}

TEST_F(FatfsShimTest, BareWriteModeOnMissingFileIsNoFile) {
    // MixerBank::saveMixer opens with FA_WRITE only (no create flag): a
    // missing file must NOT be silently created (real FatFs: FR_NO_FILE).
    FIL f;
    EXPECT_EQ(f_open(&f, "0:/pfm3/absent.mix", FA_WRITE), FR_NO_FILE);
}

// --- Review-hardened flag/error semantics (Phase 4 step-04 review) ----------

TEST_F(FatfsShimTest, CreateAlwaysTruncatesExistingFile) {
    fatfsShimInjectString("0:/pfm3/f.bin", "olddata");
    FIL f;
    ASSERT_EQ(f_open(&f, "0:/pfm3/f.bin", FA_WRITE | FA_CREATE_ALWAYS), FR_OK);
    EXPECT_EQ(fatfsShimFileSize("0:/pfm3/f.bin"), 0u);
    ASSERT_EQ(f_close(&f), FR_OK);
}

TEST_F(FatfsShimTest, CreateNewRefusesExistingFile) {
    fatfsShimInjectString("0:/pfm3/f.bin", "x");
    FIL f;
    EXPECT_EQ(f_open(&f, "0:/pfm3/f.bin", FA_WRITE | FA_CREATE_NEW), FR_EXIST);
}

TEST_F(FatfsShimTest, OpenAppendPositionsAtEnd) {
    fatfsShimInjectString("0:/pfm3/f.bin", "abc");
    FIL f;
    ASSERT_EQ(f_open(&f, "0:/pfm3/f.bin", FA_WRITE | FA_OPEN_APPEND), FR_OK);
    UINT bw = 0;
    ASSERT_EQ(f_write(&f, "d", 1, &bw), FR_OK);
    ASSERT_EQ(f_close(&f), FR_OK);
    std::vector<uint8_t> out;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/f.bin", out));
    EXPECT_EQ(out, (std::vector<uint8_t>{'a', 'b', 'c', 'd'}));
}

TEST_F(FatfsShimTest, OpenOnDirectoryIsDeniedNoShadowFile) {
    fatfsShimMkdir("0:/pfm3/dir");
    FIL f;
    EXPECT_EQ(f_open(&f, "0:/pfm3/dir", FA_WRITE | FA_OPEN_ALWAYS), FR_DENIED);
    EXPECT_FALSE(fatfsShimFileExists("0:/pfm3/dir"));
    EXPECT_EQ(f_open(&f, "0:", FA_READ), FR_DENIED);  // volume root
}

TEST_F(FatfsShimTest, NullPathArgumentsAreInvalidParameter) {
    FIL f;
    EXPECT_EQ(f_open(&f, nullptr, FA_READ), FR_INVALID_PARAMETER);
    EXPECT_EQ(f_stat(nullptr, nullptr), FR_INVALID_PARAMETER);
    EXPECT_EQ(f_unlink(nullptr), FR_INVALID_PARAMETER);
    EXPECT_EQ(f_rename(nullptr, nullptr), FR_INVALID_PARAMETER);
    EXPECT_EQ(f_mkdir(nullptr), FR_INVALID_PARAMETER);
    DIR d;
    EXPECT_EQ(f_opendir(&d, nullptr), FR_INVALID_PARAMETER);
}

TEST_F(FatfsShimTest, SeekAndWriteFailClosedPastSizeCeiling) {
    fatfsShimInjectString("0:/pfm3/f.bin", "x");
    FIL f;
    ASSERT_EQ(f_open(&f, "0:/pfm3/f.bin", FA_WRITE | FA_OPEN_ALWAYS), FR_OK);
    EXPECT_EQ(f_lseek(&f, 8u * 1024u * 1024u), FR_INVALID_PARAMETER);
    UINT bw = 99;
    ASSERT_EQ(f_lseek(&f, 4u * 1024u * 1024u - 2), FR_OK);
    EXPECT_EQ(f_write(&f, "toolong", 7, &bw), FR_DENIED);  // would pass ceiling
    EXPECT_EQ(bw, 0u);
    ASSERT_EQ(f_close(&f), FR_OK);
    EXPECT_LT(fatfsShimFileSize("0:/pfm3/f.bin"), 4u * 1024u * 1024u);
}

TEST_F(FatfsShimTest, UnlinkOfOpenFileIsDenied) {
    fatfsShimInjectString("0:/pfm3/f.bin", "x");
    FIL f;
    ASSERT_EQ(f_open(&f, "0:/pfm3/f.bin", FA_READ), FR_OK);
    EXPECT_EQ(f_unlink("0:/pfm3/f.bin"), FR_DENIED);
    ASSERT_EQ(f_close(&f), FR_OK);
    EXPECT_EQ(f_unlink("0:/pfm3/f.bin"), FR_OK);  // closed: now removable
}

TEST_F(FatfsShimTest, OpendirPathTooLongIsInvalidName) {
    std::string deep = "0:/";
    for (int i = 0; i < 60; i++) deep += "abcdefgh";  // 480+ chars
    fatfsShimMkdir(deep.c_str());
    DIR d;
    EXPECT_EQ(f_opendir(&d, deep.c_str()), FR_INVALID_NAME);
}

TEST_F(FatfsShimTest, MkdirHelperCreatesAllAncestors) {
    fatfsShimMkdir("a/b/c");  // no 0: prefix — ancestors must still appear
    DIR d;
    EXPECT_EQ(f_opendir(&d, "a"), FR_OK);
    EXPECT_EQ(f_opendir(&d, "a/b"), FR_OK);
    EXPECT_EQ(f_opendir(&d, "a/b/c"), FR_OK);
}

TEST_F(FatfsShimTest, InjectNullPayloadCreatesEmptyFile) {
    fatfsShimInjectBytes("0:/pfm3/empty.bin", nullptr, 16);  // len ignored
    EXPECT_TRUE(fatfsShimFileExists("0:/pfm3/empty.bin"));
    EXPECT_EQ(fatfsShimFileSize("0:/pfm3/empty.bin"), 0u);
}
