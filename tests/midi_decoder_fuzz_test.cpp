// Host-side deterministic fuzz coverage for firmware/Src/midi/MidiDecoder.cpp
// — sysex / byte-stream corruption robustness (Phase 2 of the coverage plan).
//
// Hexter precedent: truncated/corrupt sysex is the bug class that already
// surfaced a real global-buffer-overflow under ASAN. This suite drives the
// decode state machine with deterministically-generated adversarial byte
// streams (a fixed-seed xorshift32 PRNG — NO rand(), so failures reproduce
// bit-for-bit) and asserts two properties:
//   1. NO CRASH / no sanitizer report (run under `make test-asan`).
//   2. RESYNC: after any garbage, a clean NoteOn still routes to the Synth
//      (the parser never gets stuck in a state that swallows real notes).
//
// Scenario mix (bounded, ~10k frames total): truncated sysex frames (no F7),
// corrupt sysex bodies, oversized sysex (>32 data bytes), random status/data
// streams, and pure-realtime-byte bursts — plus periodic clean-note probes.
//
// The fixture mirrors tests/midi_decoder_test.cpp's MidiDecoderPhase2 wiring
// (memset+patched SynthState, real Synth, real Sequencer wired into the Synth
// so realtime clock bytes have a valid sequencer_ to forward to, stubbed
// FMDisplaySequencer with a dummy refresh-status pointer). See that file +
// tests/SEAM.md Target #4 for the seam rationale.

// Pre-include every firmware header MidiDecoder.h reaches with a `private:`
// section, so the scoped `#define private public` below affects ONLY the
// MidiDecoder class body (same contained pattern as midi_decoder_test.cpp).
#include "Synth.h"
#include "RingBuffer.h"

#define private public  // NOLINT: scoped to MidiDecoder.h only
#include "MidiDecoder.h"
#undef private

#include "VisualInfo.h"
#include "Sequencer.h"
#include "FMDisplaySequencer.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <new>

extern RingBuffer<AsyncAction, 16> asyncActions;

namespace {

constexpr float kInv127 = 0.00787401574803149606f;

struct SynthStateBacking {
    alignas(alignof(SynthState)) unsigned char bytes[sizeof(SynthState)];
};
struct ScaleFreqTables {
    float tables[NUMBER_OF_TIMBRES][128];
};
struct SeqBacking {
    alignas(alignof(Sequencer)) unsigned char bytes[sizeof(Sequencer)];
};
struct DispSeqBacking {
    alignas(alignof(FMDisplaySequencer)) unsigned char bytes[sizeof(FMDisplaySequencer)];
};

// Counting VisualInfo double: the RESYNC probe observes dispatched NoteOns
// through the noteOn() callback (independent of lowerNote_, which garbage
// streams can legitimately move with routed random notes). Counts ANY
// timbre: garbage CCs can legitimately re-route currentChannel/currentTimbre,
// so a timbre-0-only oracle would false-fail while routing is intact.
class CountingVisualInfo : public VisualInfo {
public:
    void midiClock(bool) override {}
    void noteOn(int timbre, bool) override {
        (void)timbre;
        count_++;
    }
    int count() const { return count_; }
private:
    int count_ = 0;
};

float EqualTemperedFreq(int note) {
    return 440.0f * powf(2.0f, (note - 69) / 12.0f);
}

// Deterministic xorshift32 (Marsaglia). Fixed seed => reproducible fuzz.
class Prng {
public:
    explicit Prng(uint32_t seed) : state_(seed ? seed : 0x9E3779B9u) {}
    uint32_t Next() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return state_;
    }
    uint8_t NextByte() { return static_cast<uint8_t>(Next() & 0xFF); }
    // Uniform-ish byte in [lo, hi].
    uint8_t NextInRange(uint8_t lo, uint8_t hi) {
        return lo + static_cast<uint8_t>(Next() % static_cast<uint32_t>(hi - lo + 1));
    }

private:
    uint32_t state_;
};

}  // namespace

// MidiDecoder.cpp TU-globals (same hygiene targets as MidiDecoderPhase2).
extern RingBuffer<uint8_t, 64> usartBufferOut;
extern uint8_t usbMidiOutBuff[64];
extern uint8_t* usbMidiOutBuffWrt;

class MidiDecoderFuzz : public ::testing::Test {
protected:
    SynthStateBacking ssBacking_;
    ScaleFreqTables scaleFreqs_;
    SynthState* ss_;
    Synth synth_;
    CountingVisualInfo visualInfo_;
    MidiDecoder decoder_;
    SeqBacking seqBacking_;
    DispSeqBacking dispSeqBacking_;
    Sequencer* seq_;
    FMDisplaySequencer* dispSeq_;
    int dummyRefreshA_ = 0;
    int dummyRefreshB_ = 0;

    void SetUp() override {
        std::memset(&ssBacking_, 0, sizeof(ssBacking_));
        ss_ = reinterpret_cast<SynthState*>(&ssBacking_);
        ss_->fullState.synthMode = SYNTH_MODE_MIXER;
        ss_->fullState.midiConfigValue[MIDICONFIG_RECEIVES] = 3;
        ss_->fullState.midiConfigValue[MIDICONFIG_PROGRAM_CHANGE] = 1;
        ss_->fullState.midiConfigValue[MIDICONFIG_SENDS] = 1;
        ss_->fullState.midiConfigValue[MIDICONFIG_USB] = USBMIDI_IN_AND_OUT;
        ss_->mixerState.globalChannel_ = 0;
        ss_->mixerState.currentChannel_ = 0;
        ss_->mixerState.MPE_inst1_ = 0;
        for (int i = 0; i < NUMBER_OF_ECC; i++) ss_->mixerState.userCC_[i] = 255;
        for (int t = 0; t < NUMBER_OF_TIMBRES; t++) {
            ss_->mixerState.instrumentState_[t].midiChannel = (t == 0) ? 1 : (t + 1);
            ss_->mixerState.instrumentState_[t].firstNote = 0;
            ss_->mixerState.instrumentState_[t].lastNote = 127;
            ss_->mixerState.instrumentState_[t].shiftNote = 0;
            ss_->mixerState.instrumentState_[t].numberOfVoices = (t == 0) ? 6 : 0;
            ss_->mixerState.instrumentState_[t].scaleFrequencies = scaleFreqs_.tables[t];
            for (int n = 0; n < 128; n++) {
                scaleFreqs_.tables[t][n] = EqualTemperedFreq(n);
            }
        }
        synth_.setSynthState(ss_);
        ss_->params = synth_.getTimbre(0)->getParamRaw();

        while (asyncActions.getCount() > 0) (void)asyncActions.remove();
        // Same TU-global hygiene as MidiDecoderPhase2::SetUp: drain the USART
        // out ring and re-home the USB packet writer. usbMidiOutBuff is 64
        // bytes and the PFM3_HOST sendMidiUsbOut stub does NOT re-home
        // usbMidiOutBuffWrt (the real body does, post-transmit), so a stale
        // pointer from a prior test would overflow on the next 4-byte packet.
        while (usartBufferOut.getCount() > 0) (void)usartBufferOut.remove();
        usbMidiOutBuffWrt = usbMidiOutBuff;
        usbMidiOutBuff[0] = usbMidiOutBuff[1] = usbMidiOutBuff[2] = usbMidiOutBuff[3] = 0;

        std::memset(&seqBacking_, 0, sizeof(seqBacking_));
        std::memset(&dispSeqBacking_, 0, sizeof(dispSeqBacking_));
        seq_ = new (&seqBacking_) Sequencer();
        dispSeq_ = new (&dispSeqBacking_) FMDisplaySequencer();
        dispSeq_->setRefreshStatusPointer(&dummyRefreshA_, &dummyRefreshB_);
        seq_->setSynth(&synth_);
        seq_->setDisplaySequencer(dispSeq_);
        synth_.setSequencer(seq_);

        decoder_.setSynthState(ss_);
        decoder_.setSynth(&synth_);
        decoder_.setVisualInfo(&visualInfo_);
        decoder_.newTimbre(0);
        decoder_.songPosition = 0;  // ctor leaves it uninitialized (BSS in fw)
    }

    void Feed(const uint8_t* bytes, size_t n) {
        for (size_t i = 0; i < n; i++) decoder_.newByte(bytes[i]);
    }
    void Feed(std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) decoder_.newByte(b);
    }

    // RESYNC probe: after arbitrary garbage, a clean NoteOn must eventually
    // dispatch. Up to 3 attempts absorb a parser left mid-message (the
    // firmware treats a status byte mid-message as data, so one attempt can
    // be consumed completing the stale message — characterized quirk).
    // On success, ALL_SOUND_OFF releases the probe voices so ~10k frames
    // don't accumulate stuck voices against the voice-allocation boundary
    // (a future allocation bug would otherwise surface here as an
    // UNRELATED flake, not a parser regression).
    void AssertParserResyncs(const char* ctx, int frame) {
        decoder_.newByte(0xF7);  // abandon any dangling sysex frame
        int before = visualInfo_.count();
        for (int attempt = 0; attempt < 3; attempt++) {
            Feed({0x90, 60, 100});
            if (visualInfo_.count() > before) {
                Feed({0xB0, CC_ALL_SOUND_OFF, 0});
                return;
            }
        }
        FAIL() << "parser lost routing after " << ctx << " frame " << frame;
    }
};

// Property: the parser survives arbitrary adversarial sysex streams and stays
// responsive — a clean NoteOn after any garbage still routes to the Synth.
TEST_F(MidiDecoderFuzz, CorruptSysexStreamsNeverBreakRouting) {
    Prng rng(0x12345678u);
    constexpr int kFrames = 5000;

    for (int f = 0; f < kFrames; f++) {
        switch (rng.Next() % 5) {
        case 0: {
            // Truncated sysex: F0 + 0..10 random bytes, NO F7 (left dangling).
            uint8_t buf[16];
            size_t n = 0;
            buf[n++] = 0xF0;
            size_t len = rng.Next() % 11;
            for (size_t i = 0; i < len; i++) buf[n++] = rng.NextByte();
            Feed(buf, n);
            // Abandon the frame so later frames start clean-ish (the parser
            // would otherwise stay in SYSEX — also a valid persistent state).
            decoder_.newByte(0xF7);
            break;
        }
        case 1: {
            // Corrupt "channel-set" frame: F0 0x7d <random ch> <random vol> F7
            // with the channel byte deliberately out of 1..6 sometimes.
            uint8_t ch = rng.NextByte();
            Feed({0xF0, 0x7d, ch, rng.NextByte(), 0xF7});
            break;
        }
        case 2: {
            // Oversized sysex: >32 data bytes before F7 (index must clamp).
            decoder_.newByte(0xF0);
            size_t len = 33 + (rng.Next() % 40);
            for (size_t i = 0; i < len; i++) decoder_.newByte(rng.NextByte());
            decoder_.newByte(0xF7);
            break;
        }
        case 3: {
            // Random status/data mix, incl. F0/F7 mid-stream.
            uint8_t buf[24];
            for (size_t i = 0; i < sizeof(buf); i++) buf[i] = rng.NextByte();
            Feed(buf, sizeof(buf));
            break;
        }
        case 4: {
            // Pure realtime burst (0xF8..0xFF).
            size_t len = 1 + (rng.Next() % 16);
            for (size_t i = 0; i < len; i++) {
                decoder_.newByte(rng.NextInRange(0xF8, 0xFF));
            }
            break;
        }
        }

        // RESYNC probe every 100 frames.
        if ((f % 100) == 99) AssertParserResyncs("sysex fuzz", f);
    }
    AssertParserResyncs("sysex fuzz (final)", kFrames - 1);
}

// Property: full-message fuzz (random valid-ish status bytes with random data
// payloads) never crashes and periodically-generated clean notes always route.
TEST_F(MidiDecoderFuzz, RandomMessageStreamsNeverBreakRouting) {
    Prng rng(0xDEADBEEFu);
    constexpr int kFrames = 5000;
    static const uint8_t statuses[] = {
        0x80, 0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF0, 0xF2,
        0x90, 0x90, 0xB0, 0xF0,  // weight note/CC/sysex higher
        0xF1, 0xF3, 0xF4, 0xF6,  // arity-0/1 system-common resync paths
    };

    for (int f = 0; f < kFrames; f++) {
        uint8_t st = statuses[rng.Next() % (sizeof(statuses) / sizeof(statuses[0]))];
        decoder_.newByte(st);
        // 0..3 data bytes (truncation + overrun of the expected arity both
        // occur; the state machine must tolerate either).
        size_t data = rng.Next() % 4;
        for (size_t i = 0; i < data; i++) decoder_.newByte(rng.NextByte());
        if (st == 0xF0 && (rng.Next() & 1)) decoder_.newByte(0xF7);

        if ((f % 100) == 99) AssertParserResyncs("message fuzz", f);
    }
    AssertParserResyncs("message fuzz (final)", kFrames - 1);
}

// Targeted determinism guard: the exact Hexter-class input — a sysex frame
// whose first data bytes look like a channel-set but with adversarial length
// and values — applied over every channel byte 0..255. Bounded and exhaustive
// over the one byte analyseSysexBuffer switches on.
TEST_F(MidiDecoderFuzz, EverySysexChannelByteIsSafe) {
    for (int ch = 0; ch < 256; ch++) {
        ss_->mixerState.instrumentState_[0].volume = 0.0f;
        Feed({0xF0, 0x7d, static_cast<uint8_t>(ch), 0x7F, 0xF7});
        if (ch >= 1 && ch <= 6) {
            EXPECT_FLOAT_EQ(ss_->mixerState.instrumentState_[ch - 1].volume,
                            127 * kInv127)
                << "channel byte " << ch;
        }
        // Parser must be back in WAITING after every frame.
        ASSERT_EQ(decoder_.currentEventState.eventState, MIDI_EVENT_WAITING);
    }
    // Short-frame shapes: the F7 TERMINATOR is stored as a data byte before
    // analyseSysexBuffer runs, so [1] (channel slot) can never be stale in a
    // 2-byte frame — the terminator itself (0xF7) can't match 1..6.
    ss_->mixerState.instrumentState_[0].volume = 0.0f;
    Feed({0xF0, 0x7d, 0xF7});  // [0]=0x7d, [1]=0xF7 -> switch can't match
    EXPECT_FLOAT_EQ(ss_->mixerState.instrumentState_[0].volume, 0.0f);
    // CHARACTERIZATION: 3-byte channel-set — the terminator lands in the
    // VOLUME slot [2] and is applied as a value: 0xF7/127.
    Feed({0xF0, 0x7d, 0x01, 0xF7});
    EXPECT_FLOAT_EQ(ss_->mixerState.instrumentState_[0].volume, 0xF7 * kInv127)
        << "F7 terminator analysed as the volume byte (characterized quirk)";
    // ...and routing still works afterwards.
    Feed({0x90, 64, 100});
    EXPECT_EQ(synth_.getLowerNote(0), 64);
}
