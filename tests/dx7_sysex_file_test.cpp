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

// Phase 8.5 (A2): stamp a complete, valid Yamaha DX7 32-voice bulk framing
// onto a 4104-byte buffer: F0 43 0n 09 20 00 | 4096 data bytes | checksum | F7.
// The channel nibble defaults to 0x0C (the common real-world dump); the
// checksum is recomputed over the final data bytes, so tests mutate data
// first and stamp last.
static void StampDx7Framing(std::string& s) {
    ASSERT_EQ(s.size(), 4104u);
    s[0] = (char)0xF0;   // SysEx start
    s[1] = (char)0x43;   // Yamaha manufacturer ID
    s[2] = (char)0x0C;   // sub-status 0 + channel 12 (any nibble is legal)
    s[3] = (char)0x09;   // format: 32-voice bulk dump
    s[4] = (char)0x20;   // byte count MSB
    s[5] = (char)0x00;   // byte count LSB ((0x20<<7)|0x00 = 4096, 7-bit MSB/LSB pair)
    s[4103] = (char)0xF7;  // SysEx end
    uint32_t sum = 0;
    for (int k = 6; k < 4102; k++) {
        sum += (uint8_t)s[k];
    }
    s[4102] = (char)((-sum) & 0x7F);
}

static std::string MakeSyx(int patchToStamp, bool stampFraming = true) {
    // 4104 bytes: 6-byte header + 32 * 128-byte packed patches + checksum tail
    std::string s(4104, '\0');
    for (int p = 0; p < 32; p++) {
        s[6 + p * 128 + 0] = (char)('A' + p);
        // End marker clamped to 7 bits: 'a'+p reaches 0x80 at p=31, which is
        // not a legal SysEx data byte (and the checksum is blind to bit 7 —
        // only the independent 7-bit rule rejects such a file).
        s[6 + p * 128 + 127] = (char)(('a' + p) & 0x7F);
    }
    if (patchToStamp >= 0) {
        s[6 + patchToStamp * 128 + 10] = 'X';  // marker inside the patch
    }
    if (stampFraming) {
        StampDx7Framing(s);
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
    uint8_t* lastPatch = dx7_.dx7LoadPatch(&bank, 31);
    ASSERT_NE(lastPatch, nullptr);
    EXPECT_EQ(lastPatch[0], 'A' + 31);
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

// --- Phase 8.5 (A2): DX7 bulk framing/checksum validation ------------------

TEST_F(DX7SysexFileTest, ValidatorAcceptsWellFormedBulkBanks) {
    std::string s = MakeSyx(0);
    EXPECT_TRUE(DX7SysexFile::isValidDx7BulkBank((const uint8_t*)s.data(), 4104));
    // Any channel nibble 0-15 is valid framing (real dumps vary; 0x0C common).
    for (int ch = 0; ch < 16; ch++) {
        s[2] = (char)ch;
        EXPECT_TRUE(DX7SysexFile::isValidDx7BulkBank((const uint8_t*)s.data(), 4104)) << "ch=" << ch;
    }
    // Wrong size / null rejected.
    EXPECT_FALSE(DX7SysexFile::isValidDx7BulkBank((const uint8_t*)s.data(), 4103));
    EXPECT_FALSE(DX7SysexFile::isValidDx7BulkBank(nullptr, 4104));
}

TEST_F(DX7SysexFileTest, ValidatorRejectsEachFramingDefect) {
    struct Case { const char* what; int idx; char value; };
    const Case cases[] = {
        {"not SysEx start", 0, (char)0xF1},
        {"not Yamaha ID", 1, (char)0x42},
        {"sub-status high nibble", 2, (char)0x10},
        {"wrong format (single voice)", 3, (char)0x00},
        {"byte count MSB", 4, (char)0x21},
        {"byte count LSB", 5, (char)0x01},
        {"missing F7 tail", 4103, (char)0x00},
    };
    for (const auto& c : cases) {
        std::string s = MakeSyx(0);
        s[c.idx] = c.value;
        EXPECT_FALSE(DX7SysexFile::isValidDx7BulkBank((const uint8_t*)s.data(), 4104)) << c.what;
    }
    // Bit-flipped data byte with the original checksum → checksum mismatch.
    std::string flipped = MakeSyx(0);
    flipped[6 + 5 * 128 + 40] ^= 0x01;
    EXPECT_FALSE(DX7SysexFile::isValidDx7BulkBank((const uint8_t*)flipped.data(), 4104));
    // Direct checksum corruption.
    std::string badSum = MakeSyx(0);
    badSum[4102] = (char)(badSum[4102] ^ 0x01);
    EXPECT_FALSE(DX7SysexFile::isValidDx7BulkBank((const uint8_t*)badSum.data(), 4104));
    // 8-bit data byte with a still-matching checksum: (-sum)&0x7F is blind
    // to bit 7, so the independent 7-bit data check must reject it.
    std::string eightBit = MakeSyx(0);
    eightBit[6 + 9 * 128 + 3] = (char)((uint8_t)eightBit[6 + 9 * 128 + 3] | 0x80);
    EXPECT_FALSE(DX7SysexFile::isValidDx7BulkBank((const uint8_t*)eightBit.data(), 4104));
    // Checksum byte with bit 7 set but correct low seven bits: the compare
    // is unmasked — pin that it stays that way (a masked &0x7F compare here
    // would accept a non-conforming framing byte).
    std::string bit7Checksum = MakeSyx(0);
    bit7Checksum[4102] = (char)((uint8_t)bit7Checksum[4102] | 0x80);
    EXPECT_FALSE(DX7SysexFile::isValidDx7BulkBank((const uint8_t*)bit7Checksum.data(), 4104));
}

TEST_F(DX7SysexFileTest, ValidatorCoversTheFinalDataByte) {
    // Off-by-one guard: the fixture's last data byte (index 4101) is normally
    // zero, so a checksum loop ending one byte early would sum identically.
    // Make the final byte non-zero and prove it is inside both the sum and
    // the 7-bit check.
    std::string s = MakeSyx(0);
    s[4101] = 0x55;
    StampDx7Framing(s);  // recompute checksum over the mutated data
    EXPECT_TRUE(DX7SysexFile::isValidDx7BulkBank((const uint8_t*)s.data(), 4104));
    // Same mutation WITHOUT restamping: checksum now stale -> rejected.
    // (An off-by-one loop excluding 4101 would compute the OLD sum and
    // wrongly accept this file.)
    std::string stale = MakeSyx(0);
    stale[4101] = 0x55;
    EXPECT_FALSE(DX7SysexFile::isValidDx7BulkBank((const uint8_t*)stale.data(), 4104));
}

TEST_F(DX7SysexFileTest, AllZeroDataWithZeroChecksumIsValidAndLoads) {
    // Plan §8.5 boundary: all-zero data (checksum 0) is VALID DX7 data.
    std::string s(4104, '\0');
    StampDx7Framing(s);
    EXPECT_EQ((uint8_t)s[4102], 0);
    EXPECT_TRUE(DX7SysexFile::isValidDx7BulkBank((const uint8_t*)s.data(), 4104));

    fatfsShimInjectBytes("0:/pfm3/dx7/zeros.syx", s.data(), s.size());
    const PFM3File* f0 = dx7_.getFile(0);
    EXPECT_STREQ(f0->name, "zeros.syx");
    // Load through the ENUMERATED entry, not a manufactured PFM3File — pins
    // that the stored name round-trips through getFullName on the load path.
    uint8_t* patch = dx7_.dx7LoadPatch(f0, 0);
    ASSERT_NE(patch, nullptr);
    EXPECT_EQ(patch[0], 0);
}

TEST_F(DX7SysexFileTest, EnumerationHidesContentInvalidBanks) {
    std::string good = MakeSyx(0);
    std::string badChecksum = MakeSyx(0);
    badChecksum[4102] = (char)(badChecksum[4102] ^ 0x01);
    std::string eightBit = MakeSyx(0);
    eightBit[500] = (char)((uint8_t)eightBit[500] | 0x80);  // checksum still matches
    std::string wrongFormat = MakeSyx(0);
    wrongFormat[3] = (char)0x00;  // single-voice format, not 32-voice bulk
    std::string noTail = MakeSyx(0);
    noTail[4103] = 0;
    std::string noFraming = MakeSyx(0, false);  // legacy fixture: no framing at all

    fatfsShimInjectBytes("0:/pfm3/dx7/valid.syx", good.data(), good.size());
    fatfsShimInjectBytes("0:/pfm3/dx7/badsum.syx", badChecksum.data(), badChecksum.size());
    fatfsShimInjectBytes("0:/pfm3/dx7/bit8.syx", eightBit.data(), eightBit.size());
    fatfsShimInjectBytes("0:/pfm3/dx7/format.syx", wrongFormat.data(), wrongFormat.size());
    fatfsShimInjectBytes("0:/pfm3/dx7/notail.syx", noTail.data(), noTail.size());
    fatfsShimInjectBytes("0:/pfm3/dx7/raw.syx", noFraming.data(), noFraming.size());

    // Only the well-formed bank is listed; invalid ones are invisible,
    // exactly like a wrong extension or truncated file.
    EXPECT_EQ(dx7_.getFile(1)->fileType, FILE_EMPTY);
    EXPECT_STREQ(dx7_.getFile(0)->name, "valid.syx");
    EXPECT_EQ(dx7_.getFileIndex("valid.syx"), 0);
    EXPECT_EQ(dx7_.getFileIndex("badsum.syx"), -1);

    // And the surviving bank actually loads — via the enumerated entry.
    EXPECT_NE(dx7_.dx7LoadPatch(dx7_.getFile(0), 0), nullptr);
}

TEST_F(DX7SysexFileTest, EnumerationUnreadableBankIsInvisible) {
    // A file whose validation read fails is treated as invalid — for every
    // distinct failure path through PreenFMFileType::load(): f_open error,
    // f_read error, successful-but-short read, and f_close error.
    struct Case { const char* what; const char* fn; FRESULT err; int shortRead; };
    const Case cases[] = {
        {"f_open error", "f_open", FR_DISK_ERR, 0},
        {"f_read error", "f_read", FR_DISK_ERR, 0},
        {"short read", "f_read", FR_OK, 100},
        {"f_close error", "f_close", FR_DISK_ERR, 0},
    };
    for (const auto& c : cases) {
        fatfsShimReset();
        fatfsShimMkdir("0:/pfm3/dx7");
        fatfsShimInjectBytes("0:/pfm3/dx7/valid.syx", MakeSyx(0).data(), 4104);
        fatfsShimInjectBytes("0:/pfm3/dx7/wedged.syx", MakeSyx(0).data(), 4104);
        // Fresh instance per case (PR #48 Copilot finding): the gtest fixture
        // builds dx7_ once per TEST, and reattaching fsu_ does NOT clear
        // isInitialized_ — reusing dx7_ would keep iteration 1's cached
        // listing and silently skip re-enumeration, leaving the armed
        // failure injections unconsumed (assertions pass vacuously).
        TestDX7SysexFile fresh;
        fresh.setFileSystemUtils(fsu_);
        if (c.shortRead > 0) {
            fatfsShimShortReadNextNth(c.fn, c.shortRead, 2);
        } else {
            fatfsShimFailNextNth(c.fn, c.err, 2);
        }
        EXPECT_STREQ(fresh.getFile(0)->name, "valid.syx") << c.what;
        EXPECT_EQ(fresh.getFile(1)->fileType, FILE_EMPTY) << c.what;
    }
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

TEST_F(DX7SysexFileTest, SubFolderBankEnumeratesValidatesAndLoads) {
    // Review round 1: pin the E-picker integration — a bank inside a selected
    // subfolder of a custom root is content-validated (read via currentDir_)
    // and loadable through the enumerated entry. Path stays within
    // getFullName's 24-char folder budget (see deferred-work: longer
    // currentDir_ paths truncate).
    fatfsShimMkdir("0:/pfm3/dx7lib/subA");
    fatfsShimInjectBytes("0:/pfm3/dx7lib/subA/good.syx", MakeSyx(0).data(), 4104);
    std::string bad = MakeSyx(0);
    bad[4102] = (char)(bad[4102] ^ 0x01);  // stale checksum
    fatfsShimInjectBytes("0:/pfm3/dx7lib/subA/bad.syx", bad.data(), bad.size());
    dx7_.setRoot("0:/pfm3/dx7lib");
    ASSERT_EQ(dx7_.initSubDirs(), 1);
    ASSERT_TRUE(dx7_.selectSubDir(0));
    EXPECT_STREQ(dx7_.folder(), "0:/pfm3/dx7lib/subA");
    EXPECT_STREQ(dx7_.getFile(0)->name, "good.syx");
    EXPECT_EQ(dx7_.getFile(1)->fileType, FILE_EMPTY);  // bad.syx invisible
    EXPECT_NE(dx7_.dx7LoadPatch(dx7_.getFile(0), 0), nullptr);
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
