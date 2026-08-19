// Characterization coverage for the real MIDI controller CC state machine.
#include "gtest/gtest.h"

#include "MidiControllerState.h"
#include "RingBuffer.h"

#include <cstring>
#include <new>
#include <type_traits>
#include <vector>

extern RingBuffer<uint8_t, 64> usartBufferOut;

class MidiControllerStateTest : public ::testing::Test {
protected:
    void SetUp() override {
        usartBufferOut.clear();
        std::memset(&storage_, 0, sizeof(storage_));
        state_ = new (&storage_) MidiControllerState;
    }
    void TearDown() override {
        state_->~MidiControllerState();
        usartBufferOut.clear();
    }
    std::vector<uint8_t> drain() {
        std::vector<uint8_t> bytes;
        while (usartBufferOut.getCount()) bytes.push_back(usartBufferOut.remove());
        return bytes;
    }

    typename std::aligned_storage<sizeof(MidiControllerState), alignof(MidiControllerState)>::type storage_;
    MidiControllerState* state_ = nullptr;
};

TEST_F(MidiControllerStateTest, ZeroBackedConstructorBuildsAllCurrentDefaults) {
    int ordinal = 1;
    for (int page = 0; page < MIDI_NUMBER_OF_PAGES; ++page) {
        for (int index = 0; index < 6; ++index, ++ordinal) {
            MidiEncoder* encoder = state_->getEncoder(page, index);
            MidiButton* button = state_->getButton(page, index);
            char encName[6] = {'E','n','c', char('0' + ordinal / 10), char('0' + ordinal % 10), 0};
            char butName[6] = {'B','u','t', char('0' + ordinal / 10), char('0' + ordinal % 10), 0};
            EXPECT_EQ(std::memcmp(encoder->name, encName, 6), 0);
            EXPECT_EQ(std::memcmp(button->name, butName, 6), 0);
            EXPECT_EQ(encoder->encoderType, MIDI_ENCODER_TYPE_CC);
            EXPECT_EQ(encoder->midiChannel, 16);
            EXPECT_EQ(encoder->controller, 15 + ordinal);
            EXPECT_EQ(encoder->value, (page & 1) ? 64 : 0);
            EXPECT_EQ(encoder->minValue, 0);
            EXPECT_EQ(encoder->maxValue, 127);
            EXPECT_EQ(button->buttonType, (page & 1) ? MIDI_BUTTON_TYPE_TOGGLE : MIDI_BUTTON_TYPE_PUSH);
            EXPECT_EQ(button->midiChannel, 16);
            EXPECT_EQ(button->controller, 59 + ordinal);
            EXPECT_EQ(button->value, 0);
            EXPECT_EQ(button->valueOff, 0);
            EXPECT_EQ(button->valueOn, 127);
        }
    }
}

TEST_F(MidiControllerStateTest, ResetRestoresEveryAssignedFieldAcrossAllControls) {
    for (int page = 0; page < MIDI_NUMBER_OF_PAGES; ++page) {
        for (int index = 0; index < 6; ++index) {
            MidiEncoder* encoder = state_->getEncoder(page, index);
            MidiButton* button = state_->getButton(page, index);
            std::memset(encoder->name, 'X', sizeof(encoder->name));
            encoder->encoderType = MIDI_ENCODER_TYPE_NRPN;
            encoder->midiChannel = 3;
            encoder->controller = 999;
            encoder->value = 99;
            encoder->maxValue = 999;
            encoder->minValue = 42;
            std::memset(button->name, 'Y', sizeof(button->name));
            button->buttonType = MIDI_BUTTON_TYPE_TOGGLE;
            button->midiChannel = 4;
            button->controller = 998;
            button->value = 1;
            button->valueOff = 12;
            button->valueOn = 34;
        }
    }

    state_->resetState();
    int ordinal = 1;
    for (int page = 0; page < MIDI_NUMBER_OF_PAGES; ++page) {
        for (int index = 0; index < 6; ++index, ++ordinal) {
            MidiEncoder* encoder = state_->getEncoder(page, index);
            MidiButton* button = state_->getButton(page, index);
            EXPECT_EQ(std::memcmp(encoder->name, "Enc", 3), 0);
            EXPECT_EQ(encoder->name[3], char('0' + ordinal / 10));
            EXPECT_EQ(encoder->name[4], char('0' + ordinal % 10));
            EXPECT_EQ(encoder->name[5], 'X');
            EXPECT_EQ(encoder->encoderType, MIDI_ENCODER_TYPE_NRPN);
            EXPECT_EQ(encoder->minValue, 42);
            EXPECT_EQ(encoder->midiChannel, 16);
            EXPECT_EQ(encoder->controller, 15 + ordinal);
            EXPECT_EQ(encoder->value, (page & 1) ? 64 : 0);
            EXPECT_EQ(encoder->maxValue, 127);

            EXPECT_EQ(std::memcmp(button->name, "But", 3), 0);
            EXPECT_EQ(button->name[3], char('0' + ordinal / 10));
            EXPECT_EQ(button->name[4], char('0' + ordinal % 10));
            EXPECT_EQ(button->name[5], 'Y');
            EXPECT_EQ(button->buttonType,
                      (page & 1) ? MIDI_BUTTON_TYPE_TOGGLE : MIDI_BUTTON_TYPE_PUSH);
            EXPECT_EQ(button->midiChannel, 16);
            EXPECT_EQ(button->controller, 59 + ordinal);
            EXPECT_EQ(button->value, 0);
            EXPECT_EQ(button->valueOff, 0);
            EXPECT_EQ(button->valueOn, 127);
        }
    }
    EXPECT_EQ(usartBufferOut.getCount(), 0);
}

TEST_F(MidiControllerStateTest, EncoderClampsAndEmitsOnlyOnActualChange) {
    MidiEncoder* encoder = state_->getEncoder(0, 0);
    state_->encoderDelta(0, 3, 0, -1);
    EXPECT_TRUE(drain().empty());
    state_->encoderDelta(0, 3, 0, 200);
    EXPECT_EQ((std::vector<uint8_t>{0xB3, 16, 127}), drain());
    state_->encoderDelta(0, 3, 0, 1);
    EXPECT_TRUE(drain().empty());
    state_->encoderDelta(0, 3, 0, -200);
    EXPECT_EQ((std::vector<uint8_t>{0xB3, 16, 0}), drain());
    EXPECT_EQ(encoder->value, 0);
}

TEST_F(MidiControllerStateTest, EncoderClampsToCustomMinimumAndMaximum) {
    MidiEncoder* encoder = state_->getEncoder(0, 1);
    encoder->minValue = 10;
    encoder->maxValue = 20;
    encoder->value = 15;
    state_->encoderDelta(0, 5, 1, -100);
    EXPECT_EQ((std::vector<uint8_t>{0xB5, 17, 10}), drain());
    state_->encoderDelta(0, 5, 1, 100);
    EXPECT_EQ((std::vector<uint8_t>{0xB5, 17, 20}), drain());
}

TEST_F(MidiControllerStateTest, EncoderExplicitChannelOverridesGlobalChannel) {
    MidiEncoder* encoder = state_->getEncoder(1, 2);
    encoder->midiChannel = 7;
    state_->encoderDelta(1, 2, 2, 1);
    EXPECT_EQ((std::vector<uint8_t>{0xB7, 24, 65}), drain());
}

TEST_F(MidiControllerStateTest, PushDownAndUpEmitOnThenOffAndUpReturnsTrue) {
    state_->buttonDown(0, 4, 0);
    EXPECT_EQ((std::vector<uint8_t>{0xB4, 60, 127}), drain());
    EXPECT_EQ(state_->getButton(0, 0)->value, 1);
    EXPECT_TRUE(state_->buttonUp(0, 4, 0));
    EXPECT_EQ((std::vector<uint8_t>{0xB4, 60, 0}), drain());
    EXPECT_EQ(state_->getButton(0, 0)->value, 0);
}

TEST_F(MidiControllerStateTest, ToggleDownFlipsAndUpDoesNotEmit) {
    MidiButton* button = state_->getButton(1, 0);
    button->midiChannel = 9;
    state_->buttonDown(1, 2, 0);
    EXPECT_EQ((std::vector<uint8_t>{0xB9, 66, 127}), drain());
    EXPECT_FALSE(state_->buttonUp(1, 2, 0));
    EXPECT_TRUE(drain().empty());
    state_->buttonDown(1, 2, 0);
    EXPECT_EQ((std::vector<uint8_t>{0xB9, 66, 0}), drain());
}
