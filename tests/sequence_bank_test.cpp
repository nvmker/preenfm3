// Host-side coverage for firmware/Src/filesystem/SequenceBank.cpp —
// sequence bank create/save/load round-trips (v1 + v2 layouts).
//
// CHARACTERIZATION suite (spec-test-coverage-phase4). FatFs via the shim.
// Quirk pinned (deferred-work.md):
//   * isReadOnly() reads `bankVersion` UNINITIALIZED when f_open fails —
//     the return value is stack garbage (the sequence banks on a real card
//     always open, so the path is only reachable on I/O failure).
// Round-trips: saveDefaultSequence -> loadDefaultSequence byte-identical
// (fullstate + actions[] + stepNotes[]), and createSequenceFile produces the
// documented v2 slot layout (4 + 32*(1024+16384+24576) bytes).
#include "gtest/gtest.h"

#include "Sequencer.h"
#include "FileSystemUtils.h"
#include "Common.h"
#include "fatfs.h"
#define private public
#include "SequenceBank.h"
#undef private

#include <cstring>
#include <memory>
#include <vector>

extern SeqMidiAction actions[SEQ_ACTION_SIZE];
extern StepSeqValue stepNotes[NUMBER_OF_STEP_SEQUENCES][256];

// Expose protected surface.
class TestSequenceBank : public SequenceBank {
public:
    using SequenceBank::isReadOnly;
    const char* folder() { return getFolderName(); }
    bool correct(char* n) { return isCorrectFile(n, 0); }
    void setListing(PFM3File* files, int cap) {
        myFiles_ = files;
        numberOfFilesMax_ = cap;
        isInitialized_ = true;
    }
};

static std::unique_ptr<Sequencer> MakeSequencer() {
    auto s = std::make_unique<Sequencer>();
    s->setSynth(nullptr);
    s->setDisplaySequencer(nullptr);
    return s;
}

class SequenceBankTest : public ::testing::Test {
protected:
    void SetUp() override {
        fatfsShimReset();
        fatfsShimMkdir("0:/pfm3");
        fsu_ = new FileSystemUtils;
        bank_.setFileSystemUtils(fsu_);
        seq_ = MakeSequencer();
        bank_.setSequencer(seq_.get());
        memset(actions, 0, sizeof(actions));
        memset(stepNotes, 0, sizeof(stepNotes));
    }
    void TearDown() override { delete fsu_; }
    void StampState() {
        // make the sequencer + tables content unique so round-trips are real
        seq_->setSequenceName("MYSEQ      ");
        actions[0].when = 0x1234;
        actions[0].actionType = 0x55;
        stepNotes[3][100].full = 0xAABBCCDD;
    }
    TestSequenceBank bank_;
    FileSystemUtils* fsu_;
    std::unique_ptr<Sequencer> seq_;
};

TEST_F(SequenceBankTest, DefaultSequenceSaveLoadRoundTripIsByteIdentical) {
    StampState();
    ASSERT_TRUE(bank_.saveDefaultSequence());
    ASSERT_TRUE(fatfsShimFileExists("0:/pfm3/seq.dfl"));

    // snapshot what was saved
    std::vector<uint8_t> before;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/seq.dfl", before));

    // wipe everything (NOTE: getFullState -> setFullState is NOT byte-stable
    // for derived counters, so the round-trip golden is the FILE bytes plus
    // the observable actions/stepNotes/name, not the raw state buffer)
    memset(actions, 0, sizeof(actions));
    memset(stepNotes, 0, sizeof(stepNotes));
    std::unique_ptr<Sequencer> seq2 = MakeSequencer();
    TestSequenceBank bank2;
    bank2.setFileSystemUtils(fsu_);
    bank2.setSequencer(seq2.get());

    ASSERT_TRUE(bank2.loadDefaultSequence());
    EXPECT_EQ(actions[0].when, 0x1234);
    EXPECT_EQ(actions[0].actionType, 0x55);
    EXPECT_EQ(stepNotes[3][100].full, 0xAABBCCDDu);
    EXPECT_EQ(strncmp(seq2->getSequenceName(), "MYSEQ      ", 11), 0);

    // Re-serialize the loaded state. Comparing the second save with the first
    // covers the complete sequencer state plus every action and step byte.
    ASSERT_TRUE(bank2.saveDefaultSequence());
    std::vector<uint8_t> after;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/seq.dfl", after));
    EXPECT_EQ(before, after);
}

TEST_F(SequenceBankTest, SaveDefaultSequenceOpenFailReturnsFalse) {
    fatfsShimReset();  // no "0:/pfm3" dir -> FR_NO_PATH
    EXPECT_FALSE(bank_.saveDefaultSequence());
}

TEST_F(SequenceBankTest, CreateSequenceFileWritesVersionedSlots) {
    bank_.createSequenceFile("mybank123456"); // 12 chars: addEmptyFile copies 12
    EXPECT_TRUE(fatfsShimFileExists("0:/pfm3/mybank123456"));
    // header (4) + 32 slots * (1024 state + 16384 actions + 24576 steps)
    EXPECT_EQ(fatfsShimFileSize("0:/pfm3/mybank123456"),
              4u + 32u * (1024u + 16384u + 24576u));
    std::vector<uint8_t> data;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/mybank123456", data));
    uint32_t version = 0;
    memcpy(&version, data.data(), 4);
    EXPECT_EQ(version, (uint32_t)SEQUENCE_BANK_CURRENT_VERSION);
}

TEST_F(SequenceBankTest, SaveSequenceWritesNamedSlotAndLoadSequenceName) {
    bank_.createSequenceFile("mybank123456"); // 12 chars: addEmptyFile copies 12
    PFM3File bank;
    strcpy(bank.name, "mybank123456");
    bank.fileType = FILE_OK;
    bank.version = 0;

    StampState();
    char sequenceName[] = "FIFTHSEQ   ";
    bank_.saveSequence(&bank, 5, sequenceName);
    EXPECT_STREQ(bank_.loadSequenceName(&bank, 5), "FIFTHSEQ   ");

    // load the slot back into a fresh sequencer
    std::unique_ptr<Sequencer> seq2 = MakeSequencer();
    TestSequenceBank bank2;
    bank2.setFileSystemUtils(fsu_);
    bank2.setSequencer(seq2.get());
    bank2.loadSequence(&bank, 5);
    EXPECT_EQ(actions[0].when, 0x1234);
    EXPECT_EQ(stepNotes[3][100].full, 0xAABBCCDDu);
    char nm[13];
    strncpy(nm, seq2->getSequenceName(), 12);
    nm[12] = 0;
    EXPECT_STREQ(nm, "FIFTHSEQ   ");
}

TEST_F(SequenceBankTest, LoadSequenceNameVersion1Layout) {
    // v1 slot: 1024 + 16384 + 12336. The name lives in the state's first
    // bytes; build via a real sequencer save of a v2-shaped state into a
    // v1-versioned container (the loader only trusts the version word).
    std::vector<uint8_t> data(4 + 1024 + 16384 + 12336, 0);
    uint32_t v1 = SEQUENCE_BANK_VERSION1;
    memcpy(data.data(), &v1, 4);
    uint8_t state[1024];
    uint32_t sz = 0;
    seq_->setSequenceName("OLDV1SEQ    ");
    seq_->getFullState(state, &sz);
    memcpy(data.data() + 4, state, 1024);
    fatfsShimInjectBytes("0:/pfm3/v1bank", data.data(), data.size());
    PFM3File bank;
    strcpy(bank.name, "v1bank");
    bank.fileType = FILE_OK;
    EXPECT_STREQ(bank_.loadSequenceName(&bank, 0), "OLDV1SEQ    ");

    // and the v1 loader path (reads 12336 into the 24576-byte stepNotes)
    memset(actions, 0, sizeof(actions));
    bank_.loadSequence(&bank, 0);
    char nm[13];
    strncpy(nm, seq_->getSequenceName(), 12);
    nm[12] = 0;
    EXPECT_STREQ(nm, "OLDV1SEQ    ");
}

TEST_F(SequenceBankTest, LoadSequenceNameMissingOrUnknownVersion) {
    PFM3File bank;
    strcpy(bank.name, "nothing");
    bank.fileType = FILE_OK;
    EXPECT_STREQ(bank_.loadSequenceName(&bank, 0), "##");  // open fails

    std::vector<uint8_t> data(4096, 0);
    uint32_t bogus = 99;
    memcpy(data.data(), &bogus, 4);
    fatfsShimInjectBytes("0:/pfm3/bogus", data.data(), data.size());
    PFM3File b2;
    strcpy(b2.name, "bogus");
    b2.fileType = FILE_OK;
    EXPECT_STREQ(bank_.loadSequenceName(&b2, 0), "##");    // no version arm
    EXPECT_EQ(fatfsShimOpenFileCount(), 0u)
        << "unknown-version fallback must close the opened bank";
}

TEST_F(SequenceBankTest, IsReadOnlyFollowsStoredVersion) {
    bank_.createSequenceFile("mybank123456"); // 12 chars: addEmptyFile copies 12
    PFM3File bank;
    strcpy(bank.name, "mybank123456");
    bank.fileType = FILE_OK;
    EXPECT_FALSE(bank_.isReadOnly(&bank));  // current version -> writable

    std::vector<uint8_t> data(4096, 0);
    uint32_t v1 = SEQUENCE_BANK_VERSION1;
    memcpy(data.data(), &v1, 4);
    fatfsShimInjectBytes("0:/pfm3/oldbank", data.data(), data.size());
    PFM3File old;
    strcpy(old.name, "oldbank");
    old.fileType = FILE_OK;
    EXPECT_TRUE(bank_.isReadOnly(&old));    // old version -> read-only

    // Open-failure path: bankVersion stays at its initialized 0 and the
    // return is a defined true (version != current), never stack garbage.
    fatfsShimFailNext("f_open", FR_NO_PATH);
    PFM3File missing;
    strcpy(missing.name, "missing");
    missing.fileType = FILE_OK;
    EXPECT_TRUE(bank_.isReadOnly(&missing)); // open fail -> defined read-only
}

TEST_F(SequenceBankTest, ShortVersionHeaderIsRejected) {
    // A 1-byte file holding a plausible version byte: the readers used to
    // dispatch on a partially-filled bankVersion and consume truncated data.
    // Now every reader requires f_read == FR_OK && byteRead == 4 first.
    std::vector<uint8_t> one(1, 0x01);
    fatfsShimInjectBytes("0:/pfm3/short", one.data(), one.size());
    PFM3File bank;
    strcpy(bank.name, "short");
    bank.fileType = FILE_OK;

    // version dispatch skipped -> same safe paths as an unknown version
    EXPECT_TRUE(bank_.isReadOnly(&bank));            // read-only
    EXPECT_STREQ(bank_.loadSequenceName(&bank, 0), "##");  // fallback name
    EXPECT_EQ(fatfsShimOpenFileCount(), 0u)
        << "short-header fallback must close the opened bank";

    // loadSequence must not touch sequencer state from truncated data
    StampState();
    memset(actions, 0, sizeof(actions));
    memset(stepNotes, 0, sizeof(stepNotes));
    bank_.loadSequence(&bank, 0);
    EXPECT_EQ(actions[0].when, 0);                   // rejected, nothing loaded
    EXPECT_EQ(stepNotes[3][100].full, 0u);

    // loadDefaultSequence on a 1-byte default file: no-op, still returns true
    fatfsShimInjectBytes("0:/pfm3/seq.dfl", one.data(), one.size());
    EXPECT_TRUE(bank_.loadDefaultSequence());
    EXPECT_EQ(actions[0].when, 0);
}

TEST_F(SequenceBankTest, FailedVersionHeaderReadIsRejected) {
    // Same class, other door: the header read itself fails (f_read hook).
    bank_.createSequenceFile("mybank123456"); // 12 chars: addEmptyFile copies 12
    PFM3File bank;
    strcpy(bank.name, "mybank123456");
    bank.fileType = FILE_OK;
    fatfsShimFailNext("f_read", FR_DISK_ERR);
    EXPECT_TRUE(bank_.isReadOnly(&bank));            // fail-safe read-only
    fatfsShimFailNext("f_read", FR_DISK_ERR);
    EXPECT_STREQ(bank_.loadSequenceName(&bank, 0), "##");
    EXPECT_EQ(fatfsShimOpenFileCount(), 0u)
        << "failed-header fallback must close the opened bank";
}

TEST_F(SequenceBankTest, IsCorrectFileRequiresSeqExtension) {
    // isCorrectFile scans name[1..8] for '.' unconditionally: fixtures live
    // in 16-byte buffers (the firmware always passes FILINFO.fname[256]).
    char n1[16]; strcpy(n1, "bank.seq");
    EXPECT_TRUE(bank_.correct(n1));
    char n2[16]; strcpy(n2, "BANK.SEQ");
    EXPECT_TRUE(bank_.correct(n2));
    char n3[16]; strcpy(n3, "bank.mix");
    EXPECT_FALSE(bank_.correct(n3));
    char n4[16]; strcpy(n4, "nodot");
    EXPECT_FALSE(bank_.correct(n4));
}

TEST_F(SequenceBankTest, RemoveDefaultSequenceDeletesFile) {
    ASSERT_TRUE(bank_.saveDefaultSequence());
    bank_.removeDefaultSequence();
    EXPECT_FALSE(fatfsShimFileExists("0:/pfm3/seq.dfl"));
}

TEST_F(SequenceBankTest, LoadDefaultSequenceMissingFileIsTrueNoOp) {
    memset(actions, 0xAA, sizeof(actions));
    EXPECT_TRUE(bank_.loadDefaultSequence());
    EXPECT_EQ(actions[0].when, 0xAAAA);  // untouched
}

TEST_F(SequenceBankTest, CreateSequenceFileReturnsAfterWriteStall) {
    // Regression (5.6): a failed/short f_write must break the zero-fill loop
    // instead of spinning forever on byteWritten == 0.
    fatfsShimFailNext("f_write", FR_INT_ERR);
    bank_.createSequenceFile("mybank123456");  // must simply RETURN (no hang)
    EXPECT_TRUE(fatfsShimFileExists("0:/pfm3/mybank123456"));
    // File stays short: full file would be 4 + 32 * (1024+16384+24576).
    EXPECT_LT(fatfsShimFileSize("0:/pfm3/mybank123456"),
              4u + 32u * (1024u + 16384u + 24576u));
}

TEST_F(SequenceBankTest, CreateSequenceFileWithoutEmptySlotBails) {
    // Pad by one entry because addEmptyFile's known/deferred condition-order
    // quirk reads slot cap before checking k < cap.
    struct PFM3File full[NUMBEROFPREENFMSEQUENCES + 1]{};
    for (int k = 0; k < NUMBEROFPREENFMSEQUENCES; k++) {
        full[k].fileType = FILE_OK;
    }
    bank_.setListing(full, NUMBEROFPREENFMSEQUENCES);
    bank_.createSequenceFile("fullbank1234");
    EXPECT_FALSE(fatfsShimFileExists("0:/pfm3/fullbank1234"));
}
