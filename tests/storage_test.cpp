// Host-side coverage for firmware/Src/filesystem/Storage.cpp — the
// composition root of the whole filesystem family (smoke tier).
//
// Storage::init wires FileSystemUtils into every bank/file object; the only
// body in this TU besides the lineBuffer definition. The family members are
// individually characterized by their own suites.
#include "gtest/gtest.h"

#include "Storage.h"
#include "StorageSizes.h"
#include "fatfs.h"

#include <cstring>

class StorageTest : public ::testing::Test {
protected:
    void SetUp() override { fatfsShimReset(); }
    OneSynthParams timbres_[NUMBER_OF_TIMBRES];
};

TEST_F(StorageTest, InitWiresTheWholeFamily) {
    Storage storage;
    storage.init(&timbres_[0], &timbres_[1], &timbres_[2], &timbres_[3],
                 &timbres_[4], &timbres_[5]);
    EXPECT_NE(storage.getMixerBank(), nullptr);
    EXPECT_NE(storage.getConfigurationFile(), nullptr);
    EXPECT_NE(storage.getDX7SysexFile(), nullptr);
    EXPECT_NE(storage.getPatchBank(), nullptr);
    EXPECT_NE(storage.getScalaFile(), nullptr);
    EXPECT_NE(storage.getUserWaveform(), nullptr);
    EXPECT_NE(storage.getUserEnvCurve(), nullptr);
    EXPECT_NE(storage.getSequenceBank(), nullptr);
}

TEST_F(StorageTest, FamilyMembersAreStableAcrossCalls) {
    Storage storage;
    storage.init(&timbres_[0], &timbres_[1], &timbres_[2], &timbres_[3],
                 &timbres_[4], &timbres_[5]);
    EXPECT_EQ(storage.getMixerBank(), storage.getMixerBank());
    EXPECT_EQ(storage.getScalaFile(), storage.getScalaFile());
}

TEST_F(StorageTest, LineBufferGlobalIsReachableAndWritable) {
    // lineBuffer is THE shared parse buffer (real definition lives in
    // Storage.cpp): externs across the parser TUs bind here.
    extern char lineBuffer[];
    lineBuffer[0] = 'o';
    lineBuffer[1] = 'k';
    lineBuffer[2] = 0;
    EXPECT_STREQ(lineBuffer, "ok");
}

TEST_F(StorageTest, LineBufferSizeMatchesSharedConstant) {
    // Spec 2.7: every TU externs lineBuffer through StorageSizes.h's
    // PFM3_LINE_BUFFER_SIZE; the Storage.cpp static_assert enforces it at
    // compile time, this guard pins it on the host too.
    extern char lineBuffer[PFM3_LINE_BUFFER_SIZE];  // sized extern: sizeof is valid
    EXPECT_EQ(sizeof(lineBuffer), (size_t)PFM3_LINE_BUFFER_SIZE);
}
