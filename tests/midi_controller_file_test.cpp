// Byte-level persistence coverage for the real MidiControllerFile TU using
// the Phase-4 in-memory FatFs shim and real PreenFMFileType plumbing.
#include "gtest/gtest.h"

#include "MidiControllerFile.h"
#include "fatfs.h"

#include <cstddef>
#include <cstring>
#include <type_traits>
#include <vector>

namespace {

struct ZeroedController {
    typename std::aligned_storage<sizeof(MidiControllerState), alignof(MidiControllerState)>::type storage;
    MidiControllerState* state;
    ZeroedController() : state(nullptr) {
        std::memset(&storage, 0, sizeof(storage));
        state = new (&storage) MidiControllerState;
    }
    ~ZeroedController() { state->~MidiControllerState(); }
};

void differentiate(MidiControllerState* state, int seed) {
    int ordinal = 0;
    for (int page = 0; page < MIDI_NUMBER_OF_PAGES; ++page) {
        for (int i = 0; i < 6; ++i, ++ordinal) {
            MidiEncoder* e = state->getEncoder(page, i);
            MidiButton* b = state->getButton(page, i);
            for (int c = 0; c < 6; ++c) {
                e->name[c] = char('A' + (seed + ordinal + c) % 26);
                b->name[c] = char('a' + (seed + ordinal + c) % 26);
            }
            e->encoderType = (ordinal & 1) ? MIDI_ENCODER_TYPE_NRPN : MIDI_ENCODER_TYPE_CC;
            e->midiChannel = uint8_t((seed + ordinal) % 17);
            e->controller = uint16_t(100 + seed + ordinal);
            e->value = uint16_t(200 + seed + ordinal);
            e->maxValue = uint16_t(300 + seed + ordinal);
            e->minValue = uint16_t(10 + seed + ordinal);
            b->buttonType = (ordinal & 1) ? MIDI_BUTTON_TYPE_TOGGLE : MIDI_BUTTON_TYPE_PUSH;
            b->midiChannel = uint8_t((seed + ordinal + 3) % 17);
            b->controller = uint16_t(400 + seed + ordinal);
            b->value = uint8_t(ordinal & 1);
            b->valueOff = uint16_t(500 + seed + ordinal);
            b->valueOn = uint16_t(600 + seed + ordinal);
        }
    }
}

void expectEqual(const MidiControllerState* expectedConst, MidiControllerState* actual) {
    MidiControllerState* expected = const_cast<MidiControllerState*>(expectedConst);
    for (int page = 0; page < MIDI_NUMBER_OF_PAGES; ++page) {
        for (int i = 0; i < 6; ++i) {
            MidiEncoder* a = expected->getEncoder(page, i);
            MidiEncoder* b = actual->getEncoder(page, i);
            EXPECT_EQ(std::memcmp(a->name, b->name, 6), 0);
            EXPECT_EQ(a->encoderType, b->encoderType);
            EXPECT_EQ(a->midiChannel, b->midiChannel);
            EXPECT_EQ(a->controller, b->controller);
            EXPECT_EQ(a->value, b->value);
            EXPECT_EQ(a->maxValue, b->maxValue);
            EXPECT_EQ(a->minValue, b->minValue);
            MidiButton* ab = expected->getButton(page, i);
            MidiButton* bb = actual->getButton(page, i);
            EXPECT_EQ(std::memcmp(ab->name, bb->name, 6), 0);
            EXPECT_EQ(ab->buttonType, bb->buttonType);
            EXPECT_EQ(ab->midiChannel, bb->midiChannel);
            EXPECT_EQ(ab->controller, bb->controller);
            EXPECT_EQ(ab->value, bb->value);
            EXPECT_EQ(ab->valueOff, bb->valueOff);
            EXPECT_EQ(ab->valueOn, bb->valueOn);
        }
    }
}

void appendU16Le(std::vector<uint8_t>& bytes, uint16_t value) {
    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
}

void appendNameAndPadding(std::vector<uint8_t>& bytes, const char (&name)[6]) {
    for (char c : name) bytes.push_back(static_cast<uint8_t>(c));
    bytes.push_back(0);
    bytes.push_back(0);
}

} // namespace

class MidiControllerFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        fatfsShimReset();
        fatfsShimMkdir("0:/pfm3");
    }
    MidiControllerFile file_;
};

TEST_F(MidiControllerFileTest, SavePinsAllSixtyVersionOneRecordsAndPadding) {
    ZeroedController controller;
    differentiate(controller.state, 2);
    file_.saveConfig(controller.state);

    std::vector<uint8_t> actual;
    ASSERT_TRUE(fatfsShimExtract(MIDI_CONTROLLER_STATE_NAME, actual));

    // Independent format oracle: version, then six encoders + six buttons per
    // page. Every 20-byte record has six name bytes, two zero padding bytes,
    // and six little-endian uint16 fields.
    std::vector<uint8_t> expected;
    expected.reserve(1202);
    appendU16Le(expected, MIDI_CONTROLLER_VERSION_1);
    for (int page = 0; page < MIDI_NUMBER_OF_PAGES; ++page) {
        for (int index = 0; index < 6; ++index) {
            MidiEncoder* encoder = controller.state->getEncoder(page, index);
            appendNameAndPadding(expected, encoder->name);
            appendU16Le(expected, encoder->encoderType);
            appendU16Le(expected, encoder->midiChannel);
            appendU16Le(expected, encoder->controller);
            appendU16Le(expected, encoder->value);
            appendU16Le(expected, encoder->maxValue);
            appendU16Le(expected, encoder->minValue);
        }
        for (int index = 0; index < 6; ++index) {
            MidiButton* button = controller.state->getButton(page, index);
            appendNameAndPadding(expected, button->name);
            appendU16Le(expected, button->buttonType);
            appendU16Le(expected, button->midiChannel);
            appendU16Le(expected, button->controller);
            appendU16Le(expected, button->value);
            appendU16Le(expected, button->valueOff);
            appendU16Le(expected, button->valueOn);
        }
    }
    ASSERT_EQ(expected.size(), 1202u);
    ASSERT_EQ(actual.size(), expected.size());
    EXPECT_EQ(actual, expected);
    for (std::size_t record = 0; record < 60; ++record) {
        const std::size_t padding = 2U + record * 20U + 6U;
        EXPECT_EQ(actual[padding], 0) << "record " << record;
        EXPECT_EQ(actual[padding + 1U], 0) << "record " << record;
    }
}

TEST_F(MidiControllerFileTest, DifferentiatedStateRoundTripsEveryPersistedField) {
    ZeroedController source;
    ZeroedController restored;
    differentiate(source.state, 5);
    differentiate(restored.state, 19);
    file_.saveConfig(source.state);
    file_.loadConfig(restored.state);
    expectEqual(source.state, restored.state);
}

TEST_F(MidiControllerFileTest, SaveOverwritesExistingConfiguration) {
    ZeroedController first;
    ZeroedController second;
    ZeroedController restored;
    differentiate(first.state, 1);
    differentiate(second.state, 11);
    file_.saveConfig(first.state);
    file_.saveConfig(second.state);
    EXPECT_EQ(fatfsShimFileSize(MIDI_CONTROLLER_STATE_NAME), 1202u);
    file_.loadConfig(restored.state);
    expectEqual(second.state, restored.state);
}

TEST_F(MidiControllerFileTest, WriteFailureDuringSaveLeavesOriginalIntact) {
    // Fixed (bugfix-phase1 item 1.4): saveConfig writes a temp file first and
    // only rotates the real config after a complete verified write. An I/O
    // failure must leave the last valid config loadable (the old
    // remove()-then-save() destroyed it).
    ZeroedController good;
    ZeroedController next;
    ZeroedController restored;
    differentiate(good.state, 3);
    differentiate(next.state, 7);
    file_.saveConfig(good.state);
    std::vector<uint8_t> before;
    ASSERT_TRUE(fatfsShimExtract(MIDI_CONTROLLER_STATE_NAME, before));

    fatfsShimFailNext("f_write", FR_DENIED);  // the tmp write fails mid-save
    file_.saveConfig(next.state);

    std::vector<uint8_t> after;
    ASSERT_TRUE(fatfsShimExtract(MIDI_CONTROLLER_STATE_NAME, after));
    EXPECT_EQ(after, before) << "failed save must not touch the stored config";
    file_.loadConfig(restored.state);
    expectEqual(good.state, restored.state);  // last good config still loads
    // leftover tmp is an accepted orphan; a subsequent successful save reuses it
    file_.saveConfig(next.state);
    file_.loadConfig(restored.state);
    expectEqual(next.state, restored.state);
    EXPECT_FALSE(fatfsShimFileExists("0:/pfm3/MidiCtl1.tmp"));
}

TEST_F(MidiControllerFileTest, RenameFailureDuringSaveLeavesOriginalIntact) {
    // Failure while rotating the canonical file to the backup leaves the
    // original untouched and the verified temp available for diagnosis/retry.
    ZeroedController good;
    ZeroedController next;
    ZeroedController restored;
    differentiate(good.state, 13);
    differentiate(next.state, 17);
    file_.saveConfig(good.state);
    std::vector<uint8_t> before;
    ASSERT_TRUE(fatfsShimExtract(MIDI_CONTROLLER_STATE_NAME, before));

    fatfsShimFailNext("f_rename", FR_DENIED);
    file_.saveConfig(next.state);

    std::vector<uint8_t> after;
    ASSERT_TRUE(fatfsShimExtract(MIDI_CONTROLLER_STATE_NAME, after));
    EXPECT_EQ(after, before);
    EXPECT_TRUE(fatfsShimFileExists("0:/pfm3/MidiCtl1.tmp"))
        << "failed rename may leave the tmp orphan until recovery";
    file_.loadConfig(restored.state);
    expectEqual(good.state, restored.state);
    EXPECT_FALSE(fatfsShimFileExists("0:/pfm3/MidiCtl1.tmp"));
}

TEST_F(MidiControllerFileTest, FirstSavePromotionFailureRecoversVerifiedTempOnLoad) {
    ZeroedController source;
    ZeroedController restored;
    differentiate(source.state, 21);

    fatfsShimFailNext("f_rename", FR_DISK_ERR);
    file_.saveConfig(source.state);
    EXPECT_FALSE(fatfsShimFileExists(MIDI_CONTROLLER_STATE_NAME));
    EXPECT_TRUE(fatfsShimFileExists("0:/pfm3/MidiCtl1.tmp"));

    file_.loadConfig(restored.state);
    expectEqual(source.state, restored.state);
    EXPECT_TRUE(fatfsShimFileExists(MIDI_CONTROLLER_STATE_NAME));
    EXPECT_FALSE(fatfsShimFileExists("0:/pfm3/MidiCtl1.tmp"));
}

TEST_F(MidiControllerFileTest, InterruptedRotationRestoresLastKnownGoodBackup) {
    ZeroedController good;
    ZeroedController next;
    ZeroedController restored;
    differentiate(good.state, 23);
    differentiate(next.state, 29);

    file_.saveConfig(good.state);
    std::vector<uint8_t> goodBytes;
    ASSERT_TRUE(fatfsShimExtract(MIDI_CONTROLLER_STATE_NAME, goodBytes));
    file_.saveConfig(next.state);
    std::vector<uint8_t> nextBytes;
    ASSERT_TRUE(fatfsShimExtract(MIDI_CONTROLLER_STATE_NAME, nextBytes));

    // Power loss after canonical -> backup but before temp -> canonical.
    fatfsShimReset();
    fatfsShimMkdir("0:/pfm3");
    fatfsShimInjectBytes("0:/pfm3/MidiCtl1.bak", goodBytes.data(), goodBytes.size());
    fatfsShimInjectBytes("0:/pfm3/MidiCtl1.tmp", nextBytes.data(), nextBytes.size());

    file_.loadConfig(restored.state);
    expectEqual(good.state, restored.state);
    EXPECT_TRUE(fatfsShimFileExists(MIDI_CONTROLLER_STATE_NAME));
    EXPECT_FALSE(fatfsShimFileExists("0:/pfm3/MidiCtl1.bak"));
    EXPECT_FALSE(fatfsShimFileExists("0:/pfm3/MidiCtl1.tmp"));
}

TEST_F(MidiControllerFileTest, CompletedPromotionKeepsNewConfigAndCleansBackup) {
    ZeroedController good;
    ZeroedController next;
    ZeroedController restored;
    differentiate(good.state, 31);
    differentiate(next.state, 37);

    file_.saveConfig(good.state);
    std::vector<uint8_t> goodBytes;
    ASSERT_TRUE(fatfsShimExtract(MIDI_CONTROLLER_STATE_NAME, goodBytes));
    file_.saveConfig(next.state);
    std::vector<uint8_t> nextBytes;
    ASSERT_TRUE(fatfsShimExtract(MIDI_CONTROLLER_STATE_NAME, nextBytes));

    // Power loss after temp -> canonical but before backup cleanup.
    fatfsShimReset();
    fatfsShimMkdir("0:/pfm3");
    fatfsShimInjectBytes(MIDI_CONTROLLER_STATE_NAME, nextBytes.data(), nextBytes.size());
    fatfsShimInjectBytes("0:/pfm3/MidiCtl1.bak", goodBytes.data(), goodBytes.size());

    file_.loadConfig(restored.state);
    expectEqual(next.state, restored.state);
    EXPECT_FALSE(fatfsShimFileExists("0:/pfm3/MidiCtl1.bak"));
}

TEST_F(MidiControllerFileTest, MissingFileLeavesStateUnchanged) {
    ZeroedController expected;
    ZeroedController actual;
    differentiate(expected.state, 7);
    differentiate(actual.state, 7);
    file_.loadConfig(actual.state);
    expectEqual(expected.state, actual.state);
}

TEST_F(MidiControllerFileTest, UnknownVersionLeavesStateUnchanged) {
    ZeroedController expected;
    ZeroedController actual;
    differentiate(expected.state, 8);
    differentiate(actual.state, 8);
    const uint8_t unknown[] = {0xFF, 0x7F};
    fatfsShimInjectBytes(MIDI_CONTROLLER_STATE_NAME, unknown, sizeof(unknown));
    file_.loadConfig(actual.state);
    expectEqual(expected.state, actual.state);
}

TEST_F(MidiControllerFileTest, PropertySizedAndStrictlyLargerFilesLeaveStateUnchanged) {
    for (std::size_t invalidSize : {std::size_t(PROPERTY_FILE_SIZE),
                                    std::size_t(PROPERTY_FILE_SIZE + 1)}) {
        SCOPED_TRACE(invalidSize);
        ZeroedController expected;
        ZeroedController actual;
        differentiate(expected.state, 9);
        differentiate(actual.state, 9);
        std::vector<uint8_t> invalid(invalidSize, 0);
        invalid[0] = MIDI_CONTROLLER_VERSION_1;
        fatfsShimInjectBytes(MIDI_CONTROLLER_STATE_NAME, invalid.data(), invalid.size());
        file_.loadConfig(actual.state);
        expectEqual(expected.state, actual.state);
    }
}
