// Host-side coverage for firmware/Src/filesystem/DX7SysexFile.cpp —
// DX7 .syx bank listing/loading + the persisted folder-picker cursor.
//
// CHARACTERIZATION suite (spec-test-coverage-phase4). FatFs via the shim.
#include "gtest/gtest.h"

#include "DX7SysexFile.h"
#include "FileSystemUtils.h"

#include "fatfs.h"

#include <cstring>
#include <string>

// Expose protected surface for direct characterization.
class TestDX7SysexFile : public DX7SysexFile {
public:
    using DX7SysexFile::isCorrectFile;
    const char* folder() { return getFolderName(); }
};

static std::string MakeSyx(int patchToStamp) {
    // 4104 bytes: 6-byte header + 32 * 128-byte packed patches + checksum tail
    std::string s(4104, '\0');
    s[0] = (char)0xF0;
    for (int p = 0; p < 32; p++) {
        s[6 + p * 128 + 0] = (char)('A' + p);
        s[6 + p * 128 + 127] = (char)('a' + p);
    }
    if (patchToStamp >= 0) {
        s[6 + patchToStamp * 128 + 10] = 'X';  // marker inside the patch
    }
    return s;
}

class DX7SysexFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        fatfsShimReset();
        fatfsShimMkdir("0:/pfm3/dx7");
        fsu_ = new FileSystemUtils;
        dx7_.setFileSystemUtils(fsu_);
    }
    void TearDown() override { delete fsu_; }
    TestDX7SysexFile dx7_;
    FileSystemUtils* fsu_;
};

TEST_F(DX7SysexFileTest, DefaultsAreDx7DirAndZeroCursor) {
    EXPECT_STREQ(dx7_.folder(), "0:/pfm3/dx7");
    EXPECT_STREQ(dx7_.getRoot(), "0:/pfm3/dx7");
    EXPECT_EQ(dx7_.getLastBank(), 0);
    EXPECT_EQ(dx7_.getLastPreset(), 0);
    EXPECT_EQ(dx7_.getSubDirCount(), 0);
}

TEST_F(DX7SysexFileTest, IsCorrectFileSizeAndExtChecks) {
    char name1[] = "bank1.syx";
    EXPECT_TRUE(dx7_.isCorrectFile(name1, 4104));
    char name2[] = "BANK1.SYX";
    EXPECT_TRUE(dx7_.isCorrectFile(name2, 4104));
    char name3[] = "bank1.syx";
    EXPECT_FALSE(dx7_.isCorrectFile(name3, 4103));
    EXPECT_FALSE(dx7_.isCorrectFile(name3, 4105));
    char name4[] = "bank1.mid";
    EXPECT_FALSE(dx7_.isCorrectFile(name4, 4104));
    char name5[] = "nosuffix";
    EXPECT_FALSE(dx7_.isCorrectFile(name5, 4104));
}

TEST_F(DX7SysexFileTest, InitFilesListsOnly4104ByteSyxSorted) {
    fatfsShimInjectBytes("0:/pfm3/dx7/bank2.syx", MakeSyx(0).data(), 4104);
    fatfsShimInjectBytes("0:/pfm3/dx7/bank10.syx", MakeSyx(1).data(), 4104);
    fatfsShimInjectString("0:/pfm3/dx7/notes.txt", "nope");   // wrong ext
    fatfsShimInjectString("0:/pfm3/dx2/bank3.syx", "nope");   // other dir
    const PFM3File* f0 = dx7_.getFile(0);
    EXPECT_STREQ(f0->name, "bank10.syx");
    const PFM3File* f1 = dx7_.getFile(1);
    EXPECT_STREQ(f1->name, "bank2.syx");
    EXPECT_EQ(dx7_.getFile(2)->fileType, FILE_EMPTY);  // errorFile_
    EXPECT_STREQ(dx7_.getFile(2)->name, "<Empty>");
    EXPECT_EQ(dx7_.getFileIndex("bank2.syx"), 1);
    EXPECT_EQ(dx7_.getFileIndex("missing"), -1);
}

TEST_F(DX7SysexFileTest, Dx7LoadPatchReads128BytesAtPatchOffset) {
    std::string syx = MakeSyx(3);
    fatfsShimInjectBytes("0:/pfm3/dx7/bank.syx", syx.data(), syx.size());
    PFM3File bank;
    strcpy(bank.name, "bank.syx");
    bank.fileType = FILE_OK;
    bank.version = 0;
    uint8_t* patch = dx7_.dx7LoadPatch(&bank, 3);
    ASSERT_NE(patch, nullptr);
    EXPECT_EQ(patch[0], 'D');       // 'A' + 3
    EXPECT_EQ(patch[10], 'X');      // the marker
    EXPECT_EQ(patch[127], 'd');
    EXPECT_EQ(dx7_.dx7LoadPatch(&bank, 31)[0], 'A' + 31);
    EXPECT_EQ(dx7_.dx7LoadPatch(&bank, 32), nullptr);  // past the last patch
}

TEST_F(DX7SysexFileTest, Dx7LoadPatchFailsOnShortBank) {
    std::string syx = MakeSyx(-1);
    syx.resize(3000);  // truncated: patch 23's 128 bytes cross EOF
    fatfsShimInjectBytes("0:/pfm3/dx7/bank.syx", syx.data(), syx.size());
    PFM3File bank;
    strcpy(bank.name, "bank.syx");
    bank.fileType = FILE_OK;
    EXPECT_EQ(dx7_.dx7LoadPatch(&bank, 23), nullptr);
    EXPECT_NE(dx7_.dx7LoadPatch(&bank, 0), nullptr);  // patch 0 fits
}

// --- folder picker (E-picker beta) -----------------------------------------

TEST_F(DX7SysexFileTest, SetRootRebuildsCurrentAndInvalidatesListing) {
    fatfsShimInjectBytes("0:/pfm3/dx7lib/a.syx", MakeSyx(0).data(), 4104);
    fatfsShimMkdir("0:/pfm3/dx7lib");
    dx7_.setRoot("0:/pfm3/dx7lib");
    EXPECT_STREQ(dx7_.getRoot(), "0:/pfm3/dx7lib");
    EXPECT_STREQ(dx7_.folder(), "0:/pfm3/dx7lib");
    EXPECT_STREQ(dx7_.getFile(0)->name, "a.syx");
}

TEST_F(DX7SysexFileTest, SetRootRejectsEmptyAndNull) {
    dx7_.setRoot("");
    EXPECT_STREQ(dx7_.getRoot(), "0:/pfm3/dx7");  // unchanged
    dx7_.setRoot(nullptr);
    EXPECT_STREQ(dx7_.getRoot(), "0:/pfm3/dx7");
}

TEST_F(DX7SysexFileTest, SelectSubDirAppendsToCurrentDir) {
    fatfsShimMkdir("0:/pfm3/dx7lib");
    fatfsShimMkdir("0:/pfm3/dx7lib/subA");
    dx7_.setRoot("0:/pfm3/dx7lib");
    dx7_.applySelectedSubDir("subA");
    EXPECT_STREQ(dx7_.folder(), "0:/pfm3/dx7lib/subA");
    EXPECT_TRUE(dx7_.selectRoot());          // changed: subA -> root
    EXPECT_STREQ(dx7_.folder(), "0:/pfm3/dx7lib");
    EXPECT_FALSE(dx7_.selectRoot());         // already at root
}

TEST_F(DX7SysexFileTest, InitSubDirsEnumeratesSortedSubfolders) {
    fatfsShimMkdir("0:/pfm3/dx7");
    fatfsShimMkdir("0:/pfm3/dx7/zzz");
    fatfsShimMkdir("0:/pfm3/dx7/aaa");
    fatfsShimInjectString("0:/pfm3/dx7/file.txt", "not a dir");
    EXPECT_EQ(dx7_.initSubDirs(), 2);
    EXPECT_STREQ(dx7_.getSubDir(0)->name, "aaa");
    EXPECT_STREQ(dx7_.getSubDir(1)->name, "zzz");
    EXPECT_EQ(dx7_.getSubDir(2)->fileType, FILE_EMPTY);  // out of range
    EXPECT_EQ(dx7_.getSubDir(-1)->fileType, FILE_EMPTY);
}

TEST_F(DX7SysexFileTest, SelectSubDirByIndexDetectsChange) {
    fatfsShimMkdir("0:/pfm3/dx7");
    fatfsShimMkdir("0:/pfm3/dx7/aaa");
    fatfsShimMkdir("0:/pfm3/dx7/bbb");
    ASSERT_EQ(dx7_.initSubDirs(), 2);
    EXPECT_TRUE(dx7_.selectSubDir(0));   // root -> aaa
    EXPECT_STREQ(dx7_.folder(), "0:/pfm3/dx7/aaa");
    EXPECT_FALSE(dx7_.selectSubDir(0));  // aaa -> aaa (same)
    EXPECT_TRUE(dx7_.selectSubDir(1));   // aaa -> bbb
    EXPECT_TRUE(dx7_.selectSubDir(99));  // out of range -> root (changed)
    EXPECT_STREQ(dx7_.folder(), "0:/pfm3/dx7");
}

TEST_F(DX7SysexFileTest, CursorSettersRoundTrip) {
    dx7_.setLastBank(300);
    dx7_.setLastPreset(128);
    EXPECT_EQ(dx7_.getLastBank(), 300);
    EXPECT_EQ(dx7_.getLastPreset(), 128);
}
