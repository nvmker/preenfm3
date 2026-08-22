// Host-side coverage for firmware/Src/filesystem/PreenFMFileType.cpp — the
// shared base of every bank/file class: enumeration, name mangling, load/
// save plumbing, rename, and the FlashSynthParams <-> OneSynthParams
// conversions.
//
// CHARACTERIZATION suite (spec-test-coverage-phase4). FatFs via the shim.
// Quirks pinned (deferred-work.md):
//   * initFiles pads DOTLESS names with '~' at the NUL position and rewrites
//     ' ' to '_'; names carrying a '.' pass through verbatim (13-char cap).
//   * nameExists() compares only up to the '.' (case-insensitive) — a save
//     under "hello.bin" collides with an existing "hello.mix".
#include "gtest/gtest.h"

#include "Common.h"
#include "FileSystemUtils.h"
#include "fatfs.h"
#define private public
#include "PreenFMFileType.h"
#undef private

#include "fatfs.h"

#include <cstring>
#include <string>
#include <vector>

extern const struct OneSynthParams preenMainPreset;

// Concrete subclass: enumerate "0:/pfm3/test" for names ending ".tst" of
// any size; lets each test control the folder contents precisely.
class TestFileType : public PreenFMFileType {
public:
    using PreenFMFileType::remove;
    using PreenFMFileType::getFileName;
    using PreenFMFileType::load;
    using PreenFMFileType::save;
    using PreenFMFileType::checkSize;
    using PreenFMFileType::createFile;
    using PreenFMFileType::closeFile;
    using PreenFMFileType::saveData;
    using PreenFMFileType::initFiles;
    using PreenFMFileType::sortFiles;
    using PreenFMFileType::swapFiles;
    using PreenFMFileType::convertParamsToFlash;
    using PreenFMFileType::convertFlashToParams;
    using PreenFMFileType::bankBaseLength;
    using PreenFMFileType::enumerateSubDirs;
    using PreenFMFileType::readNextFile;
    const char* folder() { return getFolderName(); }
    bool correct(char* n, int s) { return isCorrectFile(n, s); }
    const char* fullName(const char* f) { return getFullName(f); }
    bool initialized() const { return isInitialized_; }
    void setListing(PFM3File* files, int max) {
        myFiles_ = files;
        numberOfFilesMax_ = max;
    }

protected:
    const char* getFolderName() { return "0:/pfm3/test"; }
    bool isCorrectFile(char* name, int size) {
        (void)size;
        int len = 0;
        while (name[len] && len < 12) len++;
        return len >= 4 && name[len - 4] == '.' && name[len - 3] == 't' &&
               name[len - 2] == 's' && name[len - 1] == 't';
    }
};

class PreenFMFileTypeTest : public ::testing::Test {
protected:
    void SetUp() override {
        fatfsShimReset();
        fatfsShimMkdir("0:/pfm3/test");
        fsu_ = new FileSystemUtils;
        tft_.setFileSystemUtils(fsu_);
        tft_.setListing(files_, 16);
        memset(files_, 0, sizeof(files_));
    }
    void TearDown() override { delete fsu_; }
    TestFileType tft_;
    FileSystemUtils* fsu_;
    struct PFM3File files_[16];
};

TEST_F(PreenFMFileTypeTest, EmptyFolderYieldsErrorFile) {
    EXPECT_EQ(tft_.initFiles(), 0);
    const PFM3File* f = tft_.getFile(0);
    EXPECT_EQ(f->fileType, FILE_EMPTY);
    EXPECT_STREQ(f->name, "<Empty>");
    EXPECT_EQ(tft_.getFile(-1), f);
    EXPECT_EQ(tft_.getFile(99), f);
}

TEST_F(PreenFMFileTypeTest, MissingFolderAlsoEmpty) {
    fatfsShimReset();  // no dirs at all
    EXPECT_EQ(tft_.initFiles(), 0);
    EXPECT_EQ(tft_.getFile(0)->fileType, FILE_EMPTY);
}

TEST_F(PreenFMFileTypeTest, InitFilesManglesNamesAndSorts) {
    fatfsShimInjectString("0:/pfm3/test/beta.tst", "x");
    fatfsShimInjectString("0:/pfm3/test/Alpha.tst", "x");
    fatfsShimInjectString("0:/pfm3/test/nodot.tst", "x");      // has '.'
    fatfsShimInjectString("0:/pfm3/test/wi th.tst", "x");
    fatfsShimInjectString("0:/pfm3/test/other.bin", "x");      // wrong ext
    fatfsShimInjectString("0:/pfm3/test/a very long name.tst", "x"); // point > 8
    EXPECT_EQ(tft_.initFiles(), 4);
    EXPECT_STREQ(tft_.getFile(0)->name, "Alpha.tst");
    EXPECT_STREQ(tft_.getFile(1)->name, "beta.tst");
    EXPECT_STREQ(tft_.getFile(2)->name, "nodot.tst");
    EXPECT_STREQ(tft_.getFile(3)->name, "wi_th.tst");  // every ' ' -> '_'
    EXPECT_EQ(tft_.getFile(2)->fileType, FILE_OK);
}

TEST_F(PreenFMFileTypeTest, DotlessTstNamesGetTildePadding) {
    // isCorrectFile requires ".tst", so a dotless name can never reach the
    // pad path through THIS subclass — use a permissive subclass instead.
    struct Permissive : TestFileType {
    protected:
        bool isCorrectFile(char* name, int size) { (void)name; (void)size; return true; }
    } p;
    p.setFileSystemUtils(fsu_);
    struct PFM3File pf[4];
    memset(pf, 0, sizeof(pf));
    p.setListing(pf, 4);
    fatfsShimInjectString("0:/pfm3/test/plainname", "x");
    fatfsShimInjectString("0:/pfm3/test/_readonly", "x");
    fatfsShimInjectString("0:/pfm3/test/twelvechar12", "x");  // exactly 12 chars
    EXPECT_EQ(p.initFiles(), 3);
    // FIXED (5.2): dotless names get '~' padding only in [len, 12) and the
    // name is always NUL-terminated at [12] — no read past the 13-byte
    // array. Leading '_' -> FILE_READ_ONLY.
    EXPECT_STREQ(p.getFile(0)->name, "_readonly~~~");
    EXPECT_EQ(p.getFile(0)->fileType, FILE_READ_ONLY);
    EXPECT_STREQ(p.getFile(1)->name, "plainname~~~");
    EXPECT_EQ(p.getFile(1)->fileType, FILE_OK);
    // Full-length name: no tilde at all, NUL still lands at [12].
    const char fullLen[13] = {'t','w','e','l','v','e','c','h','a','r','1','2','\0'};
    EXPECT_EQ(memcmp(p.getFile(2)->name, fullLen, 13), 0);
    EXPECT_EQ(p.getFile(2)->fileType, FILE_OK);
}

TEST_F(PreenFMFileTypeTest, GetFullNameJoinsFolderAndFile) {
    EXPECT_STREQ(tft_.fullName("ab.tst"), "0:/pfm3/test/ab.tst");
    // 14-char cap on the file name part
    EXPECT_STREQ(tft_.fullName("abcdefghijklmnop.tst"),
                 "0:/pfm3/test/abcdefghijklmn");
}

TEST_F(PreenFMFileTypeTest, GetFileNameEnumMapping) {
    EXPECT_STREQ(tft_.getFileName(DEFAULT_MIXER), "0:/pfm3/mix.dfl");
    EXPECT_STREQ(tft_.getFileName(DEFAULT_SEQUENCE), "0:/pfm3/seq.dfl");
    EXPECT_STREQ(tft_.getFileName(PROPERTIES), "0:/pfm3/Settings.txt");
    EXPECT_STREQ(tft_.getFileName(MIDI_CONTROLLER_STATE), "0:/pfm3/MidiCtl1.bin");
    EXPECT_STREQ(tft_.getFileName(FIRMWARE), "0:/pfm3");  // default arm
}

TEST_F(PreenFMFileTypeTest, SaveLoadCheckSizeRoundTrip) {
    const char payload[] = "0123456789ABCDEF";
    EXPECT_EQ(tft_.save("data.tst", 0, (void*)payload, 16), 16);
    EXPECT_EQ(tft_.checkSize("data.tst"), 16);
    char buf[32] = {0};
    EXPECT_EQ(tft_.load("data.tst", 0, buf, 16), 16);
    EXPECT_EQ(memcmp(buf, payload, 16), 0);
    // seek save/load
    EXPECT_EQ(tft_.save("data.tst", 4, (void*)"XY", 2), 2);
    EXPECT_EQ(tft_.load("data.tst", 4, buf, 2), 2);
    EXPECT_EQ(buf[0], 'X');
    EXPECT_EQ(buf[1], 'Y');
    // in-place overwrite never grows the file past its size: still 16
    memset(buf, 0, sizeof(buf));
    EXPECT_EQ(tft_.load("data.tst", 0, buf, 0), 16);
    EXPECT_EQ(tft_.checkSize("data.tst"), 16);
    // missing file: load 0, checkSize -1
    EXPECT_EQ(tft_.load("nothere.tst", 0, buf, 4), 0);
    EXPECT_EQ(tft_.checkSize("nothere.tst"), -1);
    // short read (ask past EOF) -> 0
    EXPECT_EQ(tft_.load("data.tst", 0, buf, 100), 0);
}

TEST_F(PreenFMFileTypeTest, EnumSaveLoadUsesCanonicalNames) {
    fatfsShimMkdir("0:/pfm3");
    uint32_t four = 0x11223344;
    EXPECT_EQ(tft_.save(PROPERTIES, 0, &four, 4), 4);
    uint32_t back = 0;
    EXPECT_EQ(tft_.load(PROPERTIES, 0, &back, 4), 4);
    EXPECT_EQ(back, 0x11223344u);
    EXPECT_EQ(tft_.checkSize(PROPERTIES), 4);
}

TEST_F(PreenFMFileTypeTest, RemoveDeletesTheEnumFile) {
    fatfsShimMkdir("0:/pfm3");
    uint32_t x = 1;
    tft_.save(PROPERTIES, 0, &x, 4);
    EXPECT_EQ((FRESULT)tft_.remove(PROPERTIES), FR_OK);
    EXPECT_FALSE(fatfsShimFileExists("0:/pfm3/Settings.txt"));
}

TEST_F(PreenFMFileTypeTest, CreateSaveDataCloseLowLevel) {
    // NOTE: createFile/saveData take the FULL path (no getFullName join).
    FIL f = tft_.createFile("0:/pfm3/test/made.tst");
    EXPECT_EQ(f.err, 0);
    EXPECT_TRUE(tft_.closeFile(f));
    EXPECT_TRUE(fatfsShimFileExists("0:/pfm3/test/made.tst"));
    FIL f2 = tft_.createFile("0:/pfm3/test/made.tst");
    EXPECT_EQ(tft_.saveData(f2, (void*)"hello", 5), 5);
    EXPECT_TRUE(tft_.closeFile(f2));
    std::vector<uint8_t> out;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/test/made.tst", out));
    EXPECT_EQ(out, (std::vector<uint8_t>{'h', 'e', 'l', 'l', 'o'}));
}

TEST_F(PreenFMFileTypeTest, CreateFileInMissingParentSetsErrMarker) {
    fatfsShimReset();
    FIL f = tft_.createFile("0:/no/such/dir/gone.tst");
    EXPECT_EQ(f.err, 1);  // PatchBank::createPatchBank checks this
}

TEST_F(PreenFMFileTypeTest, RenameFileMovesShimEntryAndInvalidates) {
    fatfsShimInjectString("0:/pfm3/test/oldname.tst", "x");
    PFM3File bank;
    strcpy(bank.name, "oldname.tst");
    bank.fileType = FILE_OK;
    bank.version = 0;
    EXPECT_EQ((FRESULT)tft_.renameFile(&bank, "newname.tst"), FR_OK);
    EXPECT_FALSE(fatfsShimFileExists("0:/pfm3/test/oldname.tst"));
    EXPECT_TRUE(fatfsShimFileExists("0:/pfm3/test/newname.tst"));
    EXPECT_FALSE(tft_.initialized());
}

TEST_F(PreenFMFileTypeTest, NameExistsComparesBaseCaseInsensitive) {
    fatfsShimInjectString("0:/pfm3/test/hello.tst", "x");
    tft_.initFiles();
    EXPECT_TRUE(tft_.nameExists("HELLO.new"));
    EXPECT_TRUE(tft_.nameExists("hello.tst"));
    EXPECT_FALSE(tft_.nameExists("hell"));
}

TEST_F(PreenFMFileTypeTest, AddEmptyFileMarksListingStale) {
    EXPECT_EQ(tft_.getFileIndex("notlisted"), -1);  // triggers initFiles
    const PFM3File* added = tft_.addEmptyFile("short");  // < 12 chars: copy must NUL-pad, not over-read
    ASSERT_NE(added, nullptr);
    EXPECT_STREQ(added->name, "short");
    EXPECT_EQ(added->fileType, FILE_OK);
    EXPECT_FALSE(tft_.initialized());
    // QUIRK ADJACENT: any lazy re-init (getFile/getFileIndex) re-enumerates
    // from the folder and DROPS the just-added empty entry until the caller
    // saves a real file under that name.
    EXPECT_EQ(tft_.getFileIndex("short"), -1);
}

TEST_F(PreenFMFileTypeTest, AddEmptyFileExact12NameIsCopiedFullAndTerminated) {
    // Review patch (P4): the exact-12 full-copy case was retired with the
    // padded fixture — keep it pinned. name is char[13]: all 12 bytes copied,
    // 13th byte NUL-terminated (previously the stale byte from the listing
    // stayed), so the stored name is always a valid C string.
    EXPECT_EQ(tft_.getFileIndex("notlisted"), -1);  // triggers initFiles
    // Dirty the first free slot's 13th byte so a missing termination shows.
    for (int k = 0; k < 4; k++) {
        files_[k].name[12] = 'X';
        files_[k].fileType = FILE_EMPTY;
    }
    const PFM3File* added = tft_.addEmptyFile("brandnew1234");  // exactly 12 chars
    ASSERT_NE(added, nullptr);
    EXPECT_EQ(memcmp(added->name, "brandnew1234", 12), 0);
    EXPECT_EQ(added->name[12], '\0');
}

TEST_F(PreenFMFileTypeTest, AddEmptyFileNullNameIsRejected) {
    // Review patch (P2): strnlen(NULL) is UB; the add fails cleanly instead.
    EXPECT_EQ(tft_.getFileIndex("notlisted"), -1);  // triggers initFiles
    EXPECT_EQ(tft_.addEmptyFile(nullptr), nullptr);
}

TEST_F(PreenFMFileTypeTest, SortAndSwapHelpers) {
    for (int k = 0; k < 4; k++) {
        memset(&files_[k], 0, sizeof(files_[k]));
        files_[k].name[0] = (char)('d' - k);  // d,c,b,a
        files_[k].name[1] = 0;
        files_[k].fileType = FILE_OK;
    }
    tft_.sortFiles(files_, 4);
    EXPECT_EQ(files_[0].name[0], 'a');
    EXPECT_EQ(files_[3].name[0], 'd');
    tft_.swapFiles(files_, 0, 0);  // no-op path
    tft_.swapFiles(files_, 0, 3);
    EXPECT_EQ(files_[0].name[0], 'd');
}

TEST_F(PreenFMFileTypeTest, BankBaseLengthStopsAtDotOrEight) {
    EXPECT_EQ(tft_.bankBaseLength("abcd.tst"), 4);
    EXPECT_EQ(tft_.bankBaseLength("abcdefgh"), 8);
    EXPECT_EQ(tft_.bankBaseLength("abcdefghi"), 8);  // capped at 8
}

TEST_F(PreenFMFileTypeTest, EnumerateSubDirsSortedDepth1) {
    fatfsShimMkdir("0:/pfm3/test/aaa");
    fatfsShimMkdir("0:/pfm3/test/bbb");
    fatfsShimMkdir("0:/pfm3/test/bbb/ccc");   // depth 2: NOT enumerated
    fatfsShimInjectString("0:/pfm3/test/file.tst", "x");
    struct PFM3File out[8];
    memset(out, 0, sizeof(out));
    EXPECT_EQ(tft_.enumerateSubDirs("0:/pfm3/test", out, 8), 2);
    EXPECT_STREQ(out[0].name, "aaa");
    EXPECT_STREQ(out[1].name, "bbb");
    EXPECT_EQ(out[2].fileType, FILE_EMPTY);
    // missing path -> 0
    EXPECT_EQ(tft_.enumerateSubDirs("0:/pfm3/nothere", out, 8), 0);
}

TEST_F(PreenFMFileTypeTest, AddEmptyFileWithFullListingReturnsNull) {
    // Fixed: the find loop checks k < numberOfFilesMax_ BEFORE reading
    // myFiles_[k].fileType, so a full listing no longer reads one past the
    // array — no padding slot needed (ASAN would catch the regression).
    struct PFM3File files[16];
    memset(files, 0, sizeof(files));
    tft_.setListing(files, 16);
    tft_.initFiles();
    for (int k = 0; k < 16; k++) files[k].fileType = FILE_OK;
    EXPECT_EQ(tft_.addEmptyFile("full"), nullptr);
}

TEST_F(PreenFMFileTypeTest, ReadNextFileIsAStubReturningZero) {
    PFM3File b;
    memset(&b, 0, sizeof(b));
    // readNextFile is protected + a stub; reached via getFile()'s lazy init
    // paths above. Direct call through the subclass public using-decl:
    struct TestFileTypeUsingRead : TestFileType {
        using TestFileType::readNextFile;
    } tr;
    tr.setFileSystemUtils(fsu_);
    EXPECT_EQ(tr.readNextFile(&b), 0);
}

// --- FlashSynthParams <-> OneSynthParams conversions -----------------------

TEST_F(PreenFMFileTypeTest, ParamsFlashRoundTripPreservesCoreFields) {
    OneSynthParams src = preenMainPreset;
    FlashSynthParams flash;
    memset(&flash, 0, sizeof(flash));
    tft_.convertParamsToFlash(&src, &flash, true);
    EXPECT_FLOAT_EQ(flash.engine2.pfm3Version, 1.0f);  // stamped on save

    OneSynthParams dst;
    memset(&dst, 0, sizeof(dst));
    tft_.convertFlashToParams(&flash, &dst, true);
    EXPECT_FLOAT_EQ(dst.engine1.playMode, src.engine1.playMode);
    EXPECT_EQ(memcmp(dst.presetName, src.presetName, 13), 0);
    EXPECT_FLOAT_EQ(dst.osc1.shape, src.osc1.shape);
    EXPECT_FLOAT_EQ((&dst.env1Time)[0].attackTime,
                    (&src.env1Time)[0].attackTime);
    EXPECT_FLOAT_EQ(dst.matrixRowState1.source, src.matrixRowState1.source);
}

TEST_F(PreenFMFileTypeTest, SaveWithoutArpWritesArpDefaults) {
    FlashSynthParams flash;
    memset(&flash, 0, sizeof(flash));
    OneSynthParams src = preenMainPreset;
    src.engineArp1.clock = 4;
    tft_.convertParamsToFlash(&src, &flash, false);
    EXPECT_EQ(flash.engineArp1.clock, 0);
    EXPECT_EQ(flash.engineArp1.BPM, 90);
    EXPECT_EQ(flash.engineArp1.octave, 1);
    EXPECT_EQ(flash.engineArp2.pattern, 2);
    EXPECT_EQ(flash.engineArp2.division, 12);
    EXPECT_EQ(flash.engineArp2.duration, 14);
    EXPECT_EQ(flash.engineArp2.latche, 0);
}

TEST_F(PreenFMFileTypeTest, LoadZeroFlashInjectsDefaults) {
    FlashSynthParams flash;
    memset(&flash, 0, sizeof(flash));
    OneSynthParams p;
    memset(&p, 0, sizeof(p));
    tft_.convertFlashToParams(&flash, &p, false);
    // zeroed flash -> default injections
    EXPECT_FLOAT_EQ(p.env1Curve.attackCurve, 1.0f);
    EXPECT_FLOAT_EQ(p.effect1.param1, 0.5f);
    EXPECT_FLOAT_EQ(p.effect2.param3, 1.0f);
    EXPECT_FLOAT_EQ(p.midiNote1Curve.curveAfter, 1.0f);
    EXPECT_FLOAT_EQ(p.midiNote2Curve.curveBefore, 4.0f);
    EXPECT_FLOAT_EQ(p.midiNote2Curve.breakNote, 60.0f);
    // zero version -> pfm2 compatibility mapping: unison defaults + glide
    // typing. playMode stays 0 unless it was a pfm2 voice-count (> 2) —
    // covered in the next test.
    EXPECT_FLOAT_EQ(p.engine1.playMode, 0.0f);
    EXPECT_FLOAT_EQ(p.engine2.unisonSpread, 0.5f);
    EXPECT_FLOAT_EQ(p.engine2.unisonDetune, 0.12f);
    EXPECT_EQ((int)p.engine2.glideType, GLIDE_TYPE_OFF);
    EXPECT_FLOAT_EQ(p.performance1.perf1, 0.0f);
}

TEST_F(PreenFMFileTypeTest, Pfm2VoiceCountMapsToPoly) {
    FlashSynthParams flash;
    memset(&flash, 0, sizeof(flash));
    OneSynthParams p;
    memset(&p, 0, sizeof(p));
    p.engine1.playMode = 4.0f;  // pfm2: playMode was the voice count
    tft_.convertFlashToParams(&flash, &p, false);
    // NOTE: convertFlashToParams COPIES engine1 from flash FIRST, so the
    // caller's preset playMode is overwritten by the flash content (0).
    // The >2 mapping therefore acts on the FLASH value, not the live one.
    EXPECT_FLOAT_EQ(p.engine1.playMode, 0.0f);
}

TEST_F(PreenFMFileTypeTest, Pfm2VoiceCountInFlashMapsToPoly) {
    FlashSynthParams flash;
    memset(&flash, 0, sizeof(flash));
    float* eng = (float*)&flash.engine1;
    // playMode is a field of Engine1Params; find it by writing the whole
    // engine1 block as floats: use the params->flash copy instead.
    OneSynthParams src;
    memset(&src, 0, sizeof(src));
    src.engine1.playMode = 4.0f;
    tft_.convertParamsToFlash(&src, &flash, false);
    flash.engine2.pfm3Version = 0.0f;  // pretend pfm2 bank
    OneSynthParams p;
    memset(&p, 0, sizeof(p));
    tft_.convertFlashToParams(&flash, &p, false);
    EXPECT_EQ((int)p.engine1.playMode, PLAY_MODE_POLY);
    EXPECT_EQ((int)p.engine2.glideType, GLIDE_TYPE_OFF);
}

TEST_F(PreenFMFileTypeTest, LoadClampsEnvLevelsAboveOne) {
    FlashSynthParams flash;
    memset(&flash, 0, sizeof(flash));
    float* envFlashFloat = (float*)&flash.env1a;
    envFlashFloat[1] = 2.5f;   // attackLevel of env 0
    envFlashFloat[3] = 1.7f;   // decayLevel
    OneSynthParams p;
    memset(&p, 0, sizeof(p));
    tft_.convertFlashToParams(&flash, &p, false);
    EXPECT_FLOAT_EQ((&p.env1Level)[0].attackLevel, 1.0f);
    EXPECT_FLOAT_EQ((&p.env1Level)[0].decayLevel, 1.0f);
}

TEST_F(PreenFMFileTypeTest, ZeroEnvCurveFlashFallsBackToDefaultCurves) {
    FlashSynthParams flash;
    memset(&flash, 0, sizeof(flash));
    OneSynthParams p;
    memset(&p, 0, sizeof(p));
    tft_.convertFlashToParams(&flash, &p, false);
    EXPECT_FLOAT_EQ(p.env6Curve.releaseCurve, 0.0f);
    EXPECT_FLOAT_EQ(p.env6Curve.attackCurve, 1.0f);
}


// 3.8: a failed f_lseek must fail the save/load closed — no write ever lands
// at offset 0 (silent wrong-slot clobber) and load returns 0.
TEST_F(PreenFMFileTypeTest, SaveWithFailedLseekReturnsZeroAndDoesNotClobberOffsetZero) {
    const uint8_t original[8] = {'O', 'O', 'O', 'O', 'O', 'O', 'O', 'O'};
    fatfsShimInjectBytes("0:/pfm3/test/data.tst", original, sizeof(original));
    fatfsShimFailNext("f_lseek", FR_DISK_ERR);
    uint32_t payload = 0xAABBCCDD;
    EXPECT_EQ(tft_.save("data.tst", 4, &payload, 4), 0);
    std::vector<uint8_t> out;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/test/data.tst", out));
    EXPECT_EQ(out, std::vector<uint8_t>(original, original + 8))
        << "offset 0 must stay intact when the seek fails";
}

TEST_F(PreenFMFileTypeTest, LoadWithFailedLseekReturnsZero) {
    const uint8_t content[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    fatfsShimInjectBytes("0:/pfm3/test/data.tst", content, sizeof(content));
    fatfsShimFailNext("f_lseek", FR_DISK_ERR);
    uint8_t buf[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    EXPECT_EQ(tft_.load("data.tst", 4, buf, 4), 0);
    // No read happened at offset 0 either.
    EXPECT_EQ(buf[0], 0xFF);
}
