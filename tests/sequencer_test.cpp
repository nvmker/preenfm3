// Host-side coverage for firmware/Src/midi/Sequencer.cpp serialization.
//
// Regression target (per tests/README.md roadmap, row 1):
//   The -Ofast unaligned-float hard-fault. The sequencer packs a float (tempo)
//   at buffer offset 14, which is 2 mod 4 — MISALIGNED for a 4-byte float on
//   Cortex-M7. The firmware now uses __builtin_memcpy helpers
//   (pfm3_seq_put_f32 / pfm3_seq_get_f32) at that offset; these tests lock the
//   byte layout those helpers produce as a contract, and prove the float
//   round-trips bit-exactly through serialize -> deserialize -> serialize.
//
// Fidelity caveat (tests/SEAM.md §d.1): a host CPU permits unaligned float
// access, so these tests CANNOT reproduce the Cortex-M7 fault. Their job is to
// assert byte-faithful round-trips — the proxy for the __builtin_memcpy fix
// being intact and correct. If the layout, offset, version dispatch, or helper
// implementation drifts, these tests fail. Running under -fsanitize=undefined
// additionally confirms the current code path is free of the UB the old
// *(float*)&buf[i] cast carried.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>

#include "Sequencer.h"  // firmware-under-test (host-compilable via PFM3_HOST seam)

namespace {

// Serialization layout constants — must match getFullDefaultState / getFullState
// / loadStateVersion2 in firmware/Src/midi/Sequencer.cpp. Encoding them here is
// deliberate: if the firmware layout drifts, these tests fail loudly.
//
// Layout (V2): [0]version [1..12]name [13]extClock [14..17]tempo(f32,MISALIGNED)
//   [18..19]lastFreeAction(u16)  then 6 timbres * 8 bytes
//   (stepUnique u16, timerMask u16, seqActivated, recording, muted,
//    instrumentStepSeq)  then 12 step-sequences * 1 byte (stepActivated).
//   Total = 1+12+1+4+2 + 6*8 + 12 = 80.
constexpr uint32_t kExpectedStateSize = 80;
constexpr uint32_t kVersionOffset = 0;
constexpr uint32_t kNameOffset = 1;
constexpr uint32_t kNameLen = 12;
constexpr uint32_t kExternalClockOffset = 13;
constexpr uint32_t kTempoOffset = 14;             // 2 mod 4 => MISALIGNED float
constexpr uint32_t kLastFreeActionOffset = 18;    // 18 mod 2 => aligned u16
constexpr uint8_t kCurrentVersion = SEQ_VERSION2;  // == SEQ_CURRENT_VERSION

// Build a Sequencer with null collaborators. The serialization path under test
// (ctor -> getFullDefaultState -> setFullState -> loadStateVersion2 -> get/
// setFullState) never dereferences synth_ or displaySequencer_: confirmed by
// reading Sequencer::Sequencer(), reset(false), and the load/getFullState
// bodies. The collaborator stubs (stubs/sequencer_collaborators_stub.cpp)
// satisfy the rest of Sequencer.o's compiled references.
std::unique_ptr<Sequencer> MakeSequencer() {
    auto s = std::make_unique<Sequencer>();
    s->setSynth(nullptr);
    s->setDisplaySequencer(nullptr);
    return s;
}

// Host-safe reference: the exact IEEE-754 bytes of a float, via memcpy (never
// through a cast — the whole point of the firmware fix).
uint32_t FloatBits(float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

// Read 4 little-endian bytes at an offset as a uint32, via memcpy. Asserts the
// offset + length stay in bounds of a 256-byte state buffer.
uint32_t ReadU32Le(const uint8_t* buf, uint32_t off) {
    uint32_t v = 0;
    std::memcpy(&v, buf + off, sizeof(v));
    return v;
}

}  // namespace

// ---------------------------------------------------------------------------
// Layout contract — locks the byte format the __builtin_memcpy helpers target.
// ---------------------------------------------------------------------------
TEST(SeqSerialization, DefaultStateHasExpectedLayout) {
    auto s = MakeSequencer();
    uint8_t buf[256] = {};
    uint32_t size = 0;
    s->getFullDefaultState(buf, &size, /*seqNumber=*/0);

    ASSERT_EQ(size, kExpectedStateSize)
        << "State size drifted; every offset assertion below is now suspect";
    EXPECT_EQ(buf[kVersionOffset], kCurrentVersion);
    EXPECT_EQ(buf[kExternalClockOffset], 1) << "default external-clock flag is on";

    // Default tempo is 90.0f (see getFullDefaultState). It is written at offset
    // 14 by pfm3_seq_put_f32 — the exact site that hard-faulted under -Ofast.
    EXPECT_EQ(ReadU32Le(buf, kTempoOffset), FloatBits(90.0f))
        << "default tempo bytes at the MISALIGNED offset 14 are not IEEE-754 of 90.0f";

    // Default lastFreeAction is 12 (see getFullDefaultState).
    uint16_t lfa = 0;
    std::memcpy(&lfa, buf + kLastFreeActionOffset, 2);
    EXPECT_EQ(lfa, 12u);

    // Default name "Seq 0\0..." (seqNumber=0 -> '0','0').
    EXPECT_EQ(std::memcmp(buf + kNameOffset, "Seq 00", 6), 0);
}

// ---------------------------------------------------------------------------
// THE regression guard. Two tests:
//   (1) self-check that offset 14 is genuinely misaligned, so the misalignment
//       story is consciously revisited if anyone ever re-aligns tempo;
//   (2) tempo round-trips bit-exactly across byte-pattern-diverse values.
// ---------------------------------------------------------------------------
TEST(UnalignedFloatRegression, TempoOffsetIsGenuinelyMisaligned) {
    // If a future change moves tempo to an aligned offset (e.g. 16), this fails
    // loudly. The __builtin_memcpy helpers remain correct, but the "unaligned"
    // framing in tests/SEAM.md would need updating — hence the explicit guard.
    alignas(32) uint8_t buf[256] = {};
    ASSERT_EQ(reinterpret_cast<uintptr_t>(buf) % 32, 0u);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(buf + kTempoOffset) % 4, 2u)
        << "tempo is no longer at a 2-mod-4 misaligned offset";
}

TEST(UnalignedFloatRegression, TempoRoundTripsBitExactlyAcrossValues) {
    // Floats chosen to stress distinct byte patterns: the firmware default, an
    // all-zero-bits value, a value with 0x00 bytes, a fractional, a small int,
    // a large value. Each must survive setTempo -> getFullState (PUT) and
    // setFullState -> getFullState (GET round-trip) with bit-identical bytes at
    // the misaligned offset 14.
    const float tempos[] = {
        90.0f,        // firmware default
        0.0f,         // all-zero bits
        1.0f,         // 0x3f800000
        123.456f,     // arbitrary fractional
        240.0f,       // 0x43700000 — contains 0x00 bytes
        33.33333f,    // repeating fraction
        20000.0f,     // large
    };

    for (float t : tempos) {
        SCOPED_TRACE(::testing::PrintToString(t));

        auto s1 = MakeSequencer();
        s1->setTempo(t);

        uint8_t buf1[256] = {};
        uint32_t size1 = 0;
        s1->getFullState(buf1, &size1);
        ASSERT_EQ(size1, kExpectedStateSize);

        // PUT fidelity: bytes at the misaligned offset are exactly tempo's
        // IEEE-754 representation. This is what __builtin_memcpy guarantees;
        // *(float*)&buf[14] was the -Ofast hard-fault path.
        EXPECT_EQ(ReadU32Le(buf1, kTempoOffset), FloatBits(t))
            << "PUT corrupted tempo at misaligned offset 14";

        // GET fidelity: deserialize into a fresh instance, re-serialize, and
        // require offset 14 to be unchanged. Exercises get_f32 -> setTempo ->
        // put_f32; proves the round-trip is bit-exact at the misaligned offset.
        auto s2 = MakeSequencer();
        s2->setFullState(buf1);

        uint8_t buf2[256] = {};
        uint32_t size2 = 0;
        s2->getFullState(buf2, &size2);
        ASSERT_EQ(size2, kExpectedStateSize);

        EXPECT_EQ(ReadU32Le(buf2, kTempoOffset), FloatBits(t))
            << "GET round-trip altered tempo at misaligned offset 14";
    }
}

// ---------------------------------------------------------------------------
// Full round-trip equality on a representative, differentiated preset.
// Uses only public setters that need no collaborators, so it stays within the
// host-compilable serialization surface.
// ---------------------------------------------------------------------------
TEST(SeqRoundTrip, DifferentiatedPresetRoundTripsByteForByte) {
    auto s1 = MakeSequencer();
    s1->setSequenceName("TEST42.....");  // exactly kNameLen chars
    s1->setTempo(111.0f);
    s1->setExternalClock(true);
    s1->setNumberOfBars(0, 2);  // instrument 0 -> 2 bars
    s1->setNumberOfBars(1, 4);  // instrument 1 -> 4 bars

    uint8_t buf1[256] = {};
    uint32_t size1 = 0;
    s1->getFullState(buf1, &size1);
    ASSERT_EQ(size1, kExpectedStateSize);

    auto s2 = MakeSequencer();
    s2->setFullState(buf1);

    uint8_t buf2[256] = {};
    uint32_t size2 = 0;
    s2->getFullState(buf2, &size2);
    ASSERT_EQ(size2, kExpectedStateSize);

    // Whole-buffer equality: catches any field the deserialize path fails to
    // restore (name, tempo, masks, external clock, instrumentStepSeq, ...)
    // without poking private members.
    EXPECT_EQ(std::memcmp(buf1, buf2, size1), 0)
        << "serialize -> deserialize -> serialize diverged";
}

// ---------------------------------------------------------------------------
// Determinism: two independently-constructed Sequencers serialize identically.
// Guards against accidental non-determinism (uninitialized padding fields,
// heap-dependent layout) leaking into the on-disk format.
// ---------------------------------------------------------------------------
TEST(SeqSerialization, DefaultStateIsDeterministicAcrossInstances) {
    uint8_t a[256] = {};
    uint8_t b[256] = {};
    uint32_t sa = 0, sb = 0;

    {
        auto s = MakeSequencer();
        s->getFullState(a, &sa);
    }
    {
        auto s = MakeSequencer();
        s->getFullState(b, &sb);
    }

    ASSERT_EQ(sa, sb);
    EXPECT_EQ(std::memcmp(a, b, sa), 0)
        << "two default Sequencers produced different on-disk bytes";
}

// ---------------------------------------------------------------------------
// Version dispatch: a hand-built V1 buffer must parse. V1 and V2 share the
// tempo offset (14) but differ in the per-timbre block layout; this test
// confirms the misaligned float survives the V1->V2 migration path.
// ---------------------------------------------------------------------------
TEST(SeqSerialization, Version1BufferParsesAndPreservesTempo) {
    // V1 layout (loadStateVersion1):
    //   [0] version=1, [1..12] name, [13] extClock,
    //   [14..17] tempo (MISALIGNED), [18..19] lastFreeAction,
    //   then 6 timbres * 8 bytes (stepUnique u16, timerMask u16, seqActivated,
    //   stepActivated, recording, muted). No step-seq trailer.
    //   loadStateVersion1 reads exactly 1+12+1+4+2 + 6*8 = 68 bytes; the buffer
    //   is sized to the V2 total (80) so there is no over-read under ASan.
    alignas(8) uint8_t v1[80] = {};
    v1[0] = SEQ_VERSION1;
    std::memcpy(v1 + kNameOffset, "V1TEST      ", kNameLen);
    v1[kExternalClockOffset] = 0;
    const float tempo = 75.0f;
    std::memcpy(v1 + kTempoOffset, &tempo, 4);  // write via safe memcpy
    const uint16_t lfa = 7;
    std::memcpy(v1 + kLastFreeActionOffset, &lfa, 2);

    auto s = MakeSequencer();
    s->setFullState(v1);  // dispatches to loadStateVersion1

    uint8_t out[256] = {};
    uint32_t size = 0;
    s->getFullState(out, &size);  // re-serializes as V2 (current)
    ASSERT_EQ(size, kExpectedStateSize);

    EXPECT_EQ(out[kVersionOffset], kCurrentVersion)
        << "re-serialize must emit the current version, not the loaded one";
    EXPECT_EQ(ReadU32Le(out, kTempoOffset), FloatBits(tempo))
        << "V1->V2 migration corrupted the misaligned tempo float";
}

// ===========================================================================
// PHASE 2 (test-coverage-plan) — Sequencer transport / step-edit coverage.
//
// The serialization suites above stay untouched. The suites below exercise the
// transport (start / onMidiStart / onMidiClock / ticMillis / mainSequencerTic),
// step-record + insertNote bounds, clear() defrag, note/velocity edit bounds,
// external clock, transpose, stepseq assignment, and the v0 name dispatch.
//
// Wiring: the transport paths dereference synth_ (noteOnFromSequencer /
// allNoteOff / midiClock*), so this fixture builds a REAL Synth graph over a
// memset+patched SynthState — the exact wiring tests/midi_decoder_test.cpp's
// MidiDecoderRouting established (copied, comments abridged; see that file +
// tests/SEAM.md Target #4 for the full rationale). The Sequencer and its
// FMDisplaySequencer are placement-new'd into ZEROED backings: several members
// (extMidiRunning_, precount_) are read without ctor/reset initialization —
// firmware relies on BSS zero-init of its globals (golden-harness precedent).
// ===========================================================================

#include "Synth.h"
#include "MidiDecoder.h"   // reaches Synth.h/SynthState.h closure cleanly
#include "FMDisplaySequencer.h"

#include <new>

namespace {

struct P2SynthStateBacking {
    alignas(alignof(SynthState)) unsigned char bytes[sizeof(SynthState)];
};
struct P2ScaleFreqTables {
    float tables[NUMBER_OF_TIMBRES][128];
};
float P2EqualTemperedFreq(int note) {
    return 440.0f * powf(2.0f, (note - 69) / 12.0f);
}

}  // namespace

class SequencerPhase2 : public ::testing::Test {
protected:
    P2SynthStateBacking ssBacking_;
    P2ScaleFreqTables scaleFreqs_;
    SynthState* ss_;
    Synth synth_;
    alignas(alignof(Sequencer)) unsigned char seqBacking_[sizeof(Sequencer)];
    Sequencer* seq_;
    alignas(alignof(FMDisplaySequencer)) unsigned char dispSeqBacking_[sizeof(FMDisplaySequencer)];
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
        ss_->fullState.midiConfigValue[MIDICONFIG_USB] = USBMIDI_OFF;
        ss_->mixerState.globalChannel_ = 0;
        ss_->mixerState.currentChannel_ = 0;
        ss_->mixerState.MPE_inst1_ = 0;
        for (int t = 0; t < NUMBER_OF_TIMBRES; t++) {
            ss_->mixerState.instrumentState_[t].midiChannel = (t == 0) ? 1 : (t + 1);
            ss_->mixerState.instrumentState_[t].firstNote = 0;
            ss_->mixerState.instrumentState_[t].lastNote = 127;
            ss_->mixerState.instrumentState_[t].shiftNote = 0;
            ss_->mixerState.instrumentState_[t].numberOfVoices = (t == 0) ? 6 : 0;
            ss_->mixerState.instrumentState_[t].scaleFrequencies = scaleFreqs_.tables[t];
            for (int n = 0; n < 128; n++) {
                scaleFreqs_.tables[t][n] = P2EqualTemperedFreq(n);
            }
        }
        synth_.setSynthState(ss_);
        ss_->params = synth_.getTimbre(0)->getParamRaw();

        std::memset(seqBacking_, 0, sizeof(seqBacking_));
        std::memset(dispSeqBacking_, 0, sizeof(dispSeqBacking_));
        seq_ = new (seqBacking_) Sequencer();
        dispSeq_ = new (dispSeqBacking_) FMDisplaySequencer();
        dispSeq_->setRefreshStatusPointer(&dummyRefreshA_, &dummyRefreshB_);
        seq_->setSynth(&synth_);
        seq_->setDisplaySequencer(dispSeq_);
        synth_.setSequencer(seq_);
    }
};

// ---------------------------------------------------------------------------
// Default state.
// ---------------------------------------------------------------------------

TEST_F(SequencerPhase2, DefaultStateMatchesConstructorGoldens) {
    EXPECT_FALSE(seq_->isRunning());
    EXPECT_EQ(seq_->getMeasure(), 0);
    EXPECT_EQ(seq_->getBeat(), 1);
    EXPECT_EQ(seq_->getMemory(), 0) << "12 reserved actions = 100*12/2048 = 0";
    EXPECT_TRUE(seq_->isExternalClockEnabled());
    EXPECT_EQ(std::string(seq_->getSequenceName()), "Seq 00");
    EXPECT_EQ(seq_->getTempo(), 90);  // uint8 truncation of 90.0f
    EXPECT_EQ(seq_->getNumberOfBars(0), 4);
    EXPECT_EQ(seq_->getInstrumentStepSeq(0), 0);
    EXPECT_EQ(seq_->getInstrumentStepSeq(5), 5);
}

// ---------------------------------------------------------------------------
// Transport.
// ---------------------------------------------------------------------------

TEST_F(SequencerPhase2, InternalStartFromZeroedTimersEntersPrecountFirst) {
    // CHARACTERIZATION (the spec's "precount quirk"): with the INTERNAL clock,
    // start() from zeroed timers does NOT begin playback — it arms a 1023-tic
    // precount that mainSequencerTic burns down first. Locked as golden.
    seq_->setExternalClock(false);
    ASSERT_FALSE(seq_->isRunning());
    seq_->start();
    EXPECT_TRUE(seq_->isRunning());
    EXPECT_FLOAT_EQ(seq_->getPrecount(), 1023.0f)
        << "start() from zeroed timers must arm the 1023 precount";

    // Each ticMillis burns millisInBits_ (0.384 @90bpm) off the precount.
    for (int i = 0; i < 100; i++) seq_->ticMillis();
    EXPECT_LT(seq_->getPrecount(), 1023.0f);
    EXPECT_GT(seq_->getPrecount(), 0.0f);
    EXPECT_EQ(seq_->getMeasure(), 0) << "still precounting: measure stays 0";
}

TEST_F(SequencerPhase2, TicMillisPlaysThroughPrecountIntoBars) {
    // 90bpm: one tic = 0.384 timer bits; the 1023 precount needs ~2665 tics,
    // then the transport starts from 0. 7000 tics ≈ 1663 bits ≈ past beat 6.
    seq_->setExternalClock(false);
    seq_->start();
    for (int i = 0; i < 7000; i++) seq_->ticMillis();
    EXPECT_LE(seq_->getPrecount(), 0.0f) << "precount must exhaust";
    EXPECT_GE(seq_->getMeasure(), 1) << "transport must reach measure 2";
    EXPECT_GE(seq_->getBeat(), 1);
    EXPECT_LE(seq_->getBeat(), 4);
}

TEST_F(SequencerPhase2, TicMillisDoesNothingWithExternalClock) {
    // With the external clock enabled (default), ticMillis returns at once:
    // transport advances only via onMidiClock.
    seq_->start();
    for (int i = 0; i < 1000; i++) seq_->ticMillis();
    EXPECT_EQ(seq_->getMeasure(), 0);
    EXPECT_EQ(seq_->getBeat(), 1);
    EXPECT_TRUE(seq_->isRunning());
}

TEST_F(SequencerPhase2, MainSequencerTicDrivesMeasureAndBeat) {
    // Direct deterministic drive (the gold the external-clock path uses).
    seq_->start();        // external clock (default): running_, no precount
    seq_->onMidiStart();  // rewind + extMidiRunning_
    EXPECT_EQ(seq_->getMeasure(), 0);
    EXPECT_EQ(seq_->getBeat(), 1);

    seq_->mainSequencerTic(0);     // no move (same timer)
    EXPECT_EQ(seq_->getBeat(), 1);
    seq_->mainSequencerTic(256);   // +1 beat
    EXPECT_EQ(seq_->getBeat(), 2);
    EXPECT_EQ(seq_->getMeasure(), 0);
    seq_->mainSequencerTic(512);
    EXPECT_EQ(seq_->getBeat(), 3);
    seq_->mainSequencerTic(768);
    EXPECT_EQ(seq_->getBeat(), 4);
    seq_->mainSequencerTic(1024);  // wrap into measure 2
    EXPECT_EQ(seq_->getMeasure(), 1);
    EXPECT_EQ(seq_->getBeat(), 1);
    // CHARACTERIZATION: a direct mainSequencerTic(4096) does NOT wrap to
    // measure 0 — the counter is used raw (the wrap happens one tic later via
    // the lastInstrument16bitTimer comparison inside the instrument loop).
    seq_->mainSequencerTic(4096);
    EXPECT_EQ(seq_->getMeasure(), 4);
    EXPECT_EQ(seq_->getBeat(), 1);
}

TEST_F(SequencerPhase2, MainSequencerTicIgnoredWhenNotRunning) {
    seq_->mainSequencerTic(300);
    EXPECT_EQ(seq_->getMeasure(), 0) << "!running_ must short-circuit mainSequencerTic";
}

TEST_F(SequencerPhase2, OnMidiClockAdvancesOneBeatPer24Clocks) {
    seq_->start();
    seq_->onMidiStart();
    for (int i = 0; i < 24; i++) seq_->onMidiClock();
    EXPECT_EQ(seq_->getBeat(), 2)
        << "24 clocks x 256/24 = 256 timer units = one beat";
    // CHARACTERIZATION: the clock accumulator is a FLOAT (256/24 per clock);
    // truncation to uint16 means the 48th clock still lands at 511.99..=511
    // (beat 2), and beat 3 only arrives around the 54th clock. Empirical
    // goldens; integer-exact math would flip them.
    for (int i = 0; i < 24; i++) seq_->onMidiClock();
    EXPECT_EQ(seq_->getBeat(), 2) << "float truncation: 48 clocks are one tic short of beat 3";
    for (int i = 0; i < 24; i++) seq_->onMidiClock();
    EXPECT_EQ(seq_->getBeat(), 4) << "72 clocks reach beat 4";
}

TEST_F(SequencerPhase2, OnMidiClockIgnoredWhenNotRunning) {
    seq_->onMidiStart();
    for (int i = 0; i < 96; i++) seq_->onMidiClock();
    EXPECT_EQ(seq_->getBeat(), 1) << "clocks without start() must not advance";
}

TEST_F(SequencerPhase2, OnMidiStopThenContinue) {
    seq_->start();
    seq_->onMidiStart();
    for (int i = 0; i < 24; i++) seq_->onMidiClock();
    ASSERT_EQ(seq_->getBeat(), 2);
    seq_->onMidiStop();
    seq_->onMidiStop();  // second stop: no-op (already stopped)
    // CHARACTERIZATION: onMidiStop stops the NOTES (allNoteOff) and clears
    // extMidiRunning_, but does NOT clear running_ — with external clocks
    // still arriving, the transport keeps advancing.
    for (int i = 0; i < 30; i++) seq_->onMidiClock();
    EXPECT_EQ(seq_->getBeat(), 3) << "transport keeps running after MIDI STOP (characterized)";
    seq_->onMidiContinue(0);  // resumes WITHOUT rewind (vs onMidiStart)
    for (int i = 0; i < 18; i++) seq_->onMidiClock();
    EXPECT_EQ(seq_->getBeat(), 4) << "CONTINUE must resume from the stop point";
}

TEST_F(SequencerPhase2, StopHaltsTransport) {
    seq_->start();
    seq_->stop();
    EXPECT_FALSE(seq_->isRunning());
    seq_->mainSequencerTic(300);
    EXPECT_EQ(seq_->getMeasure(), 0);
}

// ---------------------------------------------------------------------------
// Step-record / insertNote bounds + playback.
// ---------------------------------------------------------------------------

TEST_F(SequencerPhase2, InsertNoteFillsActionListThenDropsAndCounts) {
    seq_->start();  // external clock: no precount, current16bitTimer_ = 0
    seq_->setRecording(0, true);
    // lastFreeAction_ starts at NUMBER_OF_TIMBRES*2 (12 reserved sentinel
    // entries, re-wired by the ctor); the remaining slots fill the list.
    // Derived, not hardcoded, so a firmware capacity change keeps this test
    // meaningful. SetUp zeroed the whole list first, so this holds for any
    // test execution order.
    constexpr int kReserved = NUMBER_OF_TIMBRES * 2;
    constexpr int kCapacity = SEQ_ACTION_SIZE - kReserved;
    for (int i = 0; i < kCapacity; i++) seq_->insertNote(0, 60, 100);
    EXPECT_EQ(seq_->getMemory(), 100) << "full action list = 100% memory";
    // One more insert: refused and counted (lastFreeAction_ == SEQ_ACTION_SIZE).
    EXPECT_EQ(seq_->getDroppedInsertNotes(), 0);
    seq_->insertNote(0, 60, 100);
    EXPECT_EQ(seq_->getMemory(), 100);
    EXPECT_EQ(seq_->getDroppedInsertNotes(), 1);
    EXPECT_TRUE(seq_->isSeqActivated(0));
}

TEST_F(SequencerPhase2, InsertNoteRequiresRunningAndRecording) {
    seq_->setRecording(0, true);
    seq_->insertNote(0, 60, 100);  // not running
    EXPECT_FALSE(seq_->isSeqActivated(0));
    seq_->start();
    seq_->setRecording(0, false);
    seq_->insertNote(0, 60, 100);  // running but not recording
    EXPECT_FALSE(seq_->isSeqActivated(0));
    seq_->setRecording(0, true);
    seq_->insertNote(0, 60, 100);
    EXPECT_TRUE(seq_->isSeqActivated(0));
}

TEST_F(SequencerPhase2, RecordedNoteFiresOnPlaybackAcrossWraps) {
    seq_->start();
    seq_->setRecording(0, true);
    seq_->mainSequencerTic(100);      // position the transport at timer 100
    seq_->insertNote(0, 60, 100);     // note at when=100
    ASSERT_EQ(synth_.getLowerNote(0), 64);  // Timbre::init default; not fired yet
    // CHARACTERIZATION: the action walker (nextActionIndex_) is already past
    // the head sentinel, so passing the timer over the just-recorded action
    // does NOT fire it this cycle.
    seq_->mainSequencerTic(200);
    EXPECT_EQ(synth_.getLowerNote(0), 64)
        << "a just-recorded action at the playhead does not fire this cycle (characterized)";
    // Wrap the 4-bar loop: the action fires on the next pass.
    seq_->mainSequencerTic(4095);
    seq_->mainSequencerTic(100);
    EXPECT_EQ(synth_.getLowerNote(0), 60)
        << "recorded action must fire noteOnFromSequencer on the next loop pass";
    // And again on the pass after that.
    seq_->mainSequencerTic(4095);
    seq_->mainSequencerTic(100);
    EXPECT_EQ(synth_.getLowerNote(0), 60);
}

TEST_F(SequencerPhase2, StepModeInsertAccumulatesThenRecords) {
    seq_->setStepMode(true);
    seq_->insertNote(0, 60, 100);
    seq_->insertNote(0, 64, 100);
    seq_->insertNote(0, 67, 100);
    // velocity-0 insert decrements the held-note count (note-off in step mode)
    seq_->insertNote(0, 67, 0);
    bool moreThanOne = seq_->stepRecordNotes(0, 0, 16);
    EXPECT_TRUE(moreThanOne) << "2 held notes => moreThanOneNote";
    StepSeqValue* seqData = seq_->stepGetSequence(0);
    EXPECT_EQ(seqData[0].values[3], 60);
    EXPECT_EQ(seqData[0].values[4], 64);
    // CHARACTERIZATION: a velocity-0 insert only decrements the held-note
    // COUNTER — it does NOT remove the note from tmpStepValue_, so the
    // released note is still recorded.
    EXPECT_EQ(seqData[0].values[5], 67)
        << "released step-mode note is still recorded (characterized quirk)";
    EXPECT_EQ(seqData[0].values[2], 100);  // velocity of the first press
    EXPECT_TRUE(seq_->isStepActivated(0));
    // All 16 steps of the stepSize share the recorded value.
    EXPECT_EQ(seqData[15].values[3], 60);
}

TEST_F(SequencerPhase2, StepClearPartAndClearAllResetSteps) {
    seq_->setStepMode(true);
    seq_->insertNote(0, 60, 100);
    seq_->stepRecordNotes(0, 0, 16);
    ASSERT_NE(seq_->stepGetSequence(0)[0].full, 0u);
    seq_->stepClearPart(0, 0, 16);
    // CHARACTERIZATION: stepClearPart zeroes .full but then stamps a fresh
    // .unique, so full != 0 afterwards — assert on the note byte instead.
    EXPECT_EQ(seq_->stepGetSequence(0)[0].values[3], 0);
    // stepActivated_ is NOT cleared by stepClearPart (only stepClearAll does).
    EXPECT_TRUE(seq_->isStepActivated(0));
    seq_->insertNote(0, 62, 100);
    seq_->stepRecordNotes(0, 0, 16);
    seq_->stepClearAll(0);
    EXPECT_EQ(seq_->stepGetSequence(0)[0].full, 0u);
    EXPECT_FALSE(seq_->isStepActivated(0));

    // Bugfix-phase4 4.2: the old `s < 255` bound left step 255 uncleared.
    // Step 255 must clear for both stepClearPart and stepClearAll.
    seq_->insertNote(0, 64, 100);
    seq_->stepRecordNotes(0, 255, 1);
    ASSERT_NE(seq_->stepGetSequence(0)[255].values[3], 0);
    seq_->stepClearPart(0, 255, 1);
    // .full is re-stamped with a fresh .unique; assert the note byte.
    EXPECT_EQ(seq_->stepGetSequence(0)[255].values[3], 0);
    seq_->insertNote(0, 65, 100);
    seq_->stepRecordNotes(0, 255, 1);
    ASSERT_NE(seq_->stepGetSequence(0)[255].values[3], 0);
    seq_->stepClearAll(0);
    EXPECT_EQ(seq_->stepGetSequence(0)[255].full, 0u);
}

// ---------------------------------------------------------------------------
// clear() defrag.
// ---------------------------------------------------------------------------

TEST_F(SequencerPhase2, ClearDefragsAndKeepsOtherInstruments) {
    seq_->start();
    seq_->setRecording(0, true);
    seq_->setRecording(1, true);
    seq_->mainSequencerTic(100);
    seq_->insertNote(1, 62, 100);
    seq_->insertNote(0, 60, 100);  // must move when instrument 1 is reclaimed
    for (int i = 1; i < 30; i++) {
        seq_->insertNote(1, 62, 100);
    }
    ASSERT_TRUE(seq_->isSeqActivated(0));
    ASSERT_TRUE(seq_->isSeqActivated(1));
    uint8_t memBefore = seq_->getMemory();

    seq_->clear(1);
    EXPECT_FALSE(seq_->isSeqActivated(1));
    EXPECT_TRUE(seq_->isSeqActivated(0)) << "instrument 0 must survive clear(1)";
    EXPECT_LT(seq_->getMemory(), memBefore) << "clear must reclaim action capacity";

    // Public playback behavior proves the remaining chain survived compaction.
    seq_->rewind();
    seq_->mainSequencerTic(0);
    seq_->mainSequencerTic(4094);
    seq_->mainSequencerTic(0);
    seq_->mainSequencerTic(100);
    EXPECT_TRUE(synth_.isPlaying())
        << "remaining instrument action must still fire after compaction";

    seq_->setRecording(1, true);
    for (int i = 0; i < 30; i++) seq_->insertNote(1, 65, 100);
    EXPECT_EQ(seq_->getMemory(), memBefore)
        << "reclaimed action capacity must be immediately reusable";
}

TEST_F(SequencerPhase2, ClearLastInstrumentResetsActionPool) {
    seq_->start();
    seq_->setRecording(2, true);
    seq_->insertNote(2, 60, 100);
    ASSERT_TRUE(seq_->isSeqActivated(2));
    seq_->clear(2);
    EXPECT_FALSE(seq_->isSeqActivated(2));
    EXPECT_EQ(seq_->getMemory(), 0)
        << "clearing the last active seq resets lastFreeAction_ to 12 (=0%)";
}

TEST_F(SequencerPhase2, ClearInactiveInstrumentIsNoOp) {
    seq_->clear(3);
    EXPECT_EQ(seq_->getMemory(), 0);
    EXPECT_FALSE(seq_->isSeqActivated(3));
}

// ---------------------------------------------------------------------------
// Note / velocity edit bounds (step editor).
// ---------------------------------------------------------------------------

TEST_F(SequencerPhase2, ChangeCurrentNoteCreatesNoteOnEmptyPattern) {
    // Empty pattern => createNewNoteIfEmpty: velocity 100, note 64.
    seq_->changeCurrentNote(0, 0, 16, 0);
    uint64_t step = seq_->getStepData(0, 0);
    EXPECT_EQ((step >> 16) & 0xFF, 100);  // values[2] velocity
    EXPECT_EQ((step >> 24) & 0xFF, 64);   // values[3] note
    // Fixed (spec 2.4): the local StepSeqValue is zero-initialized, so the
    // unused note slots values[4..7] are defined zeros, not stack garbage.
    for (int n = 4; n <= 7; n++) {
        EXPECT_EQ((step >> (8 * n)) & 0xFF, 0)
            << "unused note slot values[" << n << "] must be a defined zero";
    }
}

TEST_F(SequencerPhase2, ChangeCurrentNoteEditsAndRejectsOutOfRange) {
    seq_->changeCurrentNote(0, 0, 16, 0);     // create 64/100
    // FIRMWARE QUIRK (worked around test-side): createNewNoteIfEmpty copies a
    // default-init LOCAL StepSeqValue, so values[4..7] (the unused note slots)
    // receive INDETERMINATE stack bytes (zero in -O3 builds, stale data in
    // Debug). Zero them so the edit-bounds assertions below are deterministic;
    // the quirk itself is documented here rather than papered over silently.
    for (int st = 0; st < 16; st++) {
        for (int n = 4; n <= 7; n++) seq_->stepGetSequence(0)[st].values[n] = 0;
    }
    seq_->changeCurrentNote(0, 0, 16, 12);    // +12 semitones
    EXPECT_EQ((seq_->getStepData(0, 0) >> 24) & 0xFF, 76);
    // Walk down to 1, then one more -1 must be REJECTED (note 0 = "no note").
    seq_->changeCurrentNote(0, 0, 16, -75);   // 76 -> 1
    ASSERT_EQ((seq_->getStepData(0, 0) >> 24) & 0xFF, 1);
    seq_->changeCurrentNote(0, 0, 16, -1);
    EXPECT_EQ((seq_->getStepData(0, 0) >> 24) & 0xFF, 1)
        << "note 0 is 'no note': the edit must be rejected";
    // Same at the top end.
    seq_->changeCurrentNote(0, 0, 16, 126);   // 1 -> 127
    ASSERT_EQ((seq_->getStepData(0, 0) >> 24) & 0xFF, 127);
    seq_->changeCurrentNote(0, 0, 16, 1);
    EXPECT_EQ((seq_->getStepData(0, 0) >> 24) & 0xFF, 127)
        << "notes above 127 must be rejected";
}

TEST_F(SequencerPhase2, ChangeCurrentVelocityClamps1To127) {
    seq_->changeCurrentNote(0, 0, 16, 0);     // create 64/100
    seq_->changeCurrentVelocity(0, 0, 16, 50);   // -> 127 (clamped)
    EXPECT_EQ((seq_->getStepData(0, 0) >> 16) & 0xFF, 127);
    seq_->changeCurrentVelocity(0, 0, 16, -200); // -> 1 (clamped, never 0)
    EXPECT_EQ((seq_->getStepData(0, 0) >> 16) & 0xFF, 1)
        << "velocity 0 would make the note uneditable; clamps to 1";
    // No note => no-op.
    seq_->changeCurrentVelocity(1, 0, 16, 50);
    EXPECT_EQ(seq_->getStepData(1, 0), 0u);
}

// ---------------------------------------------------------------------------
// Misc: transpose, stepseq assignment, external clock, v0 name.
// ---------------------------------------------------------------------------

TEST_F(SequencerPhase2, StepPlaybackPlaysFiveNotesWithoutPhantomSixth) {
    // REGRESSION (spec 2.3): the step note-on/off loops used to evaluate
    // values[3 + n] BEFORE the n < 6 bound, reading values[8] — one past the
    // 8-byte StepSeqValue union, i.e. byte 0 (position LSB) of the NEXT step.
    // No writer stores values[5..7] today, but a fully populated step must
    // play exactly its 5 note slots and never treat the adjacent byte as a
    // 6th note. The note-on and note-off loops share the fixed condition.
    seq_->setStepMode(true);
    seq_->insertNote(0, 60, 100);
    seq_->stepRecordNotes(0, 0, 16);  // one note: moreThanOneNote is false
    ASSERT_TRUE(seq_->isStepActivated(0));
    seq_->setStepMode(false);
    StepSeqValue* seqData = seq_->stepGetSequence(0);

    // Step 1 (the one the playhead lands on): all 5 note slots populated.
    seqData[1].full = 0;
    seqData[1].values[1] = 1;                      // unique != lastUnique
    seqData[1].values[2] = 100;                    // velocity
    seqData[1].values[3] = 60;
    seqData[1].values[4] = 61;
    seqData[1].values[5] = 62;
    seqData[1].values[6] = 63;
    seqData[1].values[7] = 64;
    // Step 2 stays empty but keeps its unique so the transport advances;
    // its values[0] (10) is the byte the old loop played as a phantom 6th
    // note of step 1 (values[8] of step 1 == values[0] of step 2).
    seqData[2].full = 0;
    seqData[2].values[0] = 10;
    seqData[2].values[1] = 1;

    seq_->start();
    seq_->onMidiStart();
    seq_->mainSequencerTic(16);   // playhead: step 0 -> step 1
    EXPECT_EQ(synth_.getLowerNote(0), 60)
        << "step 1 must play its 5 notes (lowest 60), not the adjacent byte as a 6th";

    seq_->mainSequencerTic(32);   // playhead: step 1 (populated) -> step 2 (empty)
    EXPECT_EQ(synth_.getLowerNote(0), 60)
        << "note-off pass must turn off step 1's real notes without phantom behavior";
}

TEST_F(SequencerPhase2, TransposeBoundariesAreObservedIndependentlyAtPlayback) {
    seq_->start();
    seq_->setRecording(0, true);
    seq_->mainSequencerTic(100);
    seq_->insertNote(0, 60, 100);

    auto wrapAndPlay = [&]() {
        seq_->mainSequencerTic(4094);
        seq_->mainSequencerTic(0);
        seq_->mainSequencerTic(100);
    };
    seq_->setTranspose(0, 47);
    wrapAndPlay();
    EXPECT_EQ(synth_.getLowerNote(0), 107);
    seq_->setTranspose(0, 48);  // rejected; +47 remains
    wrapAndPlay();
    EXPECT_EQ(synth_.getLowerNote(0), 107);
    seq_->setTranspose(0, -47);
    wrapAndPlay();
    EXPECT_EQ(synth_.getLowerNote(0), 13);
    seq_->setTranspose(0, -48);  // rejected; -47 remains
    wrapAndPlay();
    EXPECT_EQ(synth_.getLowerNote(0), 13);
}

TEST_F(SequencerPhase2, SetInstrumentStepSeqRoutesGetStepData) {
    seq_->setStepMode(true);
    seq_->setInstrumentStepSeq(0, 7);
    EXPECT_EQ(seq_->getInstrumentStepSeq(0), 7);
    seq_->insertNote(0, 60, 100);
    seq_->stepRecordNotes(0, 0, 16);
    EXPECT_NE(seq_->getStepData(0, 0), 0u)
        << "getStepData must read through instrumentStepSeq_[0]==7";
    EXPECT_EQ(seq_->getStepData(1, 0), 0u) << "sequence 1 stays empty";
}

TEST_F(SequencerPhase2, SetNewSeqValueFromMidiDrivesTransportAndMute) {
    // PLAY_ALL >0 starts, 0 stops.
    seq_->setNewSeqValueFromMidi(0, SEQ_VALUE_PLAY_ALL, 1);
    EXPECT_TRUE(seq_->isRunning());
    seq_->setNewSeqValueFromMidi(0, SEQ_VALUE_PLAY_ALL, 0);
    EXPECT_FALSE(seq_->isRunning());
    // PLAY_INST 0 mutes, >0 unmutes.
    seq_->setNewSeqValueFromMidi(0, SEQ_VALUE_PLAY_INST, 0);
    EXPECT_TRUE(seq_->isMuted(0));
    seq_->setNewSeqValueFromMidi(0, SEQ_VALUE_PLAY_INST, 1);
    EXPECT_FALSE(seq_->isMuted(0));
    // RECORD_INST arms recording.
    seq_->setNewSeqValueFromMidi(0, SEQ_VALUE_RECORD_INST, 1);
    EXPECT_TRUE(seq_->isRecording(0));
    // SEQUENCE_NUMBER wraps mod 12.
    seq_->setNewSeqValueFromMidi(0, SEQ_VALUE_SEQUENCE_NUMBER, 14);
    EXPECT_EQ(seq_->getInstrumentStepSeq(0), 2);
    // TRANSPOSE = value - 64 (70 -> +6): no public transpose getter, so
    // verify via playback like the transpose test — restart (PLAY_ALL 0
    // stopped the transport above), record note 40, wrap the loop, the fired
    // note must be 40 + 6.
    seq_->setNewSeqValueFromMidi(0, SEQ_VALUE_TRANSPOSE, 70);
    seq_->setNewSeqValueFromMidi(0, SEQ_VALUE_PLAY_ALL, 1);  // restart
    ASSERT_TRUE(seq_->isRunning());
    ASSERT_TRUE(seq_->isRecording(0));
    seq_->mainSequencerTic(100);
    seq_->insertNote(0, 40, 100);
    seq_->mainSequencerTic(200);
    seq_->mainSequencerTic(4095);
    seq_->mainSequencerTic(100);  // next loop pass: the action fires
    EXPECT_EQ(synth_.getLowerNote(0), 46)
        << "SEQ_VALUE_TRANSPOSE=70 must apply +6 at fire time (value-64 mapping)";
}

TEST_F(SequencerPhase2, ToggleMutedAndToggleRecording) {
    seq_->toggleRecording(0);
    EXPECT_TRUE(seq_->isRecording(0));
    seq_->toggleRecording(0);
    EXPECT_FALSE(seq_->isRecording(0));
    seq_->toggleMuted(0);
    EXPECT_TRUE(seq_->isMuted(0));
    seq_->toggleMuted(0);
    EXPECT_FALSE(seq_->isMuted(0));
}

TEST_F(SequencerPhase2, ExternalClockToggleRoundTrips) {
    EXPECT_TRUE(seq_->isExternalClockEnabled());
    seq_->setExternalClock(false);
    EXPECT_FALSE(seq_->isExternalClockEnabled());
    seq_->setExternalClock(true);
    EXPECT_TRUE(seq_->isExternalClockEnabled());
}

TEST_F(SequencerPhase2, SetNumberOfBarsRejectsInvalidAndAccepts124) {
    seq_->setNumberOfBars(0, 0);   // rejected
    seq_->setNumberOfBars(0, 3);   // rejected (no 3-bar patterns)
    seq_->setNumberOfBars(0, 5);   // rejected
    EXPECT_EQ(seq_->getNumberOfBars(0), 4);
    seq_->setNumberOfBars(0, 1);
    EXPECT_EQ(seq_->getNumberOfBars(0), 1);
    seq_->setNumberOfBars(0, 2);
    EXPECT_EQ(seq_->getNumberOfBars(0), 2);
    seq_->setNumberOfBars(0, 4);
    EXPECT_EQ(seq_->getNumberOfBars(0), 4);
}

TEST_F(SequencerPhase2, VersionZeroBufferNameReturnsHashHash) {
    // v0 (unknown version byte) has no case in getSequenceNameInBuffer's
    // switch => falls to `return "##"`. Locks the no-default-switch behavior.
    char buf[16] = {};
    buf[0] = 0;
    EXPECT_STREQ(seq_->getSequenceNameInBuffer(buf), "##");
    // Known versions return the buffer name slot.
    buf[0] = SEQ_VERSION2;
    EXPECT_EQ(seq_->getSequenceNameInBuffer(buf), buf + 1);
}

TEST_F(SequencerPhase2, MidiClockSetSongPositionAnchorsTimer) {
    seq_->start();
    seq_->onMidiStart();
    seq_->midiClockSetSongPosition(2);
    for (int i = 0; i < 24; i++) seq_->onMidiClock();
    // 24 clocks from timer 8 lands at 264 => beat 2.
    EXPECT_EQ(seq_->getBeat(), 2);
}

TEST_F(SequencerPhase2, RewindResetsTimers) {
    seq_->start();
    seq_->onMidiStart();
    for (int i = 0; i < 54; i++) seq_->onMidiClock();  // 54 = beat-3 boundary (float acc.)
    ASSERT_EQ(seq_->getBeat(), 3);
    seq_->rewind();
    EXPECT_EQ(seq_->getMeasure(), 0);
    EXPECT_EQ(seq_->getBeat(), 1);
}

TEST_F(SequencerPhase2, ResetRestoresActionPoolWithoutTouchingSynth) {
    seq_->start();
    seq_->setRecording(0, true);
    seq_->insertNote(0, 60, 100);
    ASSERT_TRUE(seq_->isSeqActivated(0));
    seq_->reset(false);  // synthNoteOff=false: must NOT dereference synth paths
    EXPECT_FALSE(seq_->isSeqActivated(0));
    EXPECT_EQ(seq_->getMemory(), 0);
}

// ===========================================================================
// 8.1 — decode-context async queue (spec-8.1-seq-list-context-race).
//
// queueNote / queueNewSeqValue are the audio-IRQ decode entry points: they
// may ONLY enqueue. All structural mutation happens when the main loop
// drains via processAsyncActions (mirrored here). These tests pin that
// contract: nothing observable changes before the drain, everything applies
// in FIFO decode order after it, and overflow drops the newest event with a
// counter instead of overwriting an unread one.
// ===========================================================================

TEST_F(SequencerPhase2, QueueNoteDefersMutationUntilDrain) {
    seq_->start();
    seq_->setRecording(0, true);
    seq_->mainSequencerTic(100);
    seq_->queueNote(0, 60, 100);
    // Enqueue must not touch the action list — the walk owns it until the
    // main-loop drain.
    EXPECT_FALSE(seq_->isSeqActivated(0));
    EXPECT_EQ(seq_->getMemory(), 0);
    seq_->processAsyncActions();
    EXPECT_TRUE(seq_->isSeqActivated(0)) << "drain applies the queued note";
    // The applied note behaves exactly like a direct insertNote: fires on
    // the next loop pass (see RecordedNoteFiresOnPlaybackAcrossWraps).
    seq_->mainSequencerTic(200);
    seq_->mainSequencerTic(4095);
    seq_->mainSequencerTic(100);
    EXPECT_EQ(synth_.getLowerNote(0), 60);
}

TEST_F(SequencerPhase2, QueuePreservesDecodeOrderAcrossNoteAndCc) {
    seq_->start();
    seq_->setRecording(0, true);
    seq_->mainSequencerTic(100);
    // Decode order: note-on 67, then REC-off, then note-on 60. FIFO drain
    // must apply them in exactly that order — the second note arrives after
    // REC was disarmed and must NOT be recorded. Note choice matters: 67 =
    // only the first recorded; 60 = FIFO broken (second recorded); 64 =
    // nothing recorded (first dropped too).
    seq_->queueNote(0, 67, 100);
    seq_->queueNewSeqValue(0, SEQ_VALUE_RECORD_INST, 0);
    seq_->queueNote(0, 60, 100);
    EXPECT_FALSE(seq_->isSeqActivated(0)) << "queueing must not mutate the list";
    EXPECT_TRUE(seq_->isRecording(0)) << "REC-off must not apply before the drain";
    seq_->processAsyncActions();
    EXPECT_FALSE(seq_->isRecording(0)) << "REC-off applied by the drain";
    EXPECT_TRUE(seq_->isSeqActivated(0)) << "the pre-REC-off note is recorded";
    seq_->mainSequencerTic(200);
    seq_->mainSequencerTic(4095);
    seq_->mainSequencerTic(100);
    EXPECT_EQ(synth_.getLowerNote(0), 67)
        << "only the note queued before REC-off is recorded (FIFO order)";
}

TEST_F(SequencerPhase2, QueueNewSeqValueAppliesOnlyOnDrain) {
    seq_->queueNewSeqValue(0, SEQ_VALUE_PLAY_ALL, 1);
    EXPECT_FALSE(seq_->isRunning()) << "start must not apply before the drain";
    seq_->processAsyncActions();
    EXPECT_TRUE(seq_->isRunning());
    seq_->queueNewSeqValue(0, SEQ_VALUE_RECORD_INST, 1);
    EXPECT_FALSE(seq_->isRecording(0));
    seq_->processAsyncActions();
    EXPECT_TRUE(seq_->isRecording(0));
    seq_->queueNewSeqValue(0, SEQ_VALUE_SEQUENCE_NUMBER, 14);
    seq_->processAsyncActions();
    EXPECT_EQ(seq_->getInstrumentStepSeq(0), 2) << "14 % NUMBER_OF_STEP_SEQUENCES";
}

TEST_F(SequencerPhase2, ResetAndStateLoadDiscardQueuedEntries) {
    // Review finding (BH6/EH2): reset()/setFullState() supersede queued
    // decode-context mutations — no stale note/CC may land in fresh state.
    seq_->start();
    seq_->setRecording(0, true);
    seq_->queueNote(0, 60, 100);
    seq_->queueNewSeqValue(0, SEQ_VALUE_PLAY_ALL, 0);  // queued stop
    seq_->reset(false);
    seq_->processAsyncActions();
    EXPECT_FALSE(seq_->isSeqActivated(0)) << "queued note discarded by reset";
    // setFullState (SD sequence load) path likewise.
    seq_->start();
    ASSERT_TRUE(seq_->isRunning());
    seq_->queueNewSeqValue(0, SEQ_VALUE_PLAY_ALL, 0);  // queued stop
    uint8_t buf[256];
    uint32_t size = 0;
    seq_->getFullState(buf, &size);
    seq_->setFullState(buf);
    seq_->processAsyncActions();
    EXPECT_TRUE(seq_->isRunning())
        << "queued stop discarded by state load — running state comes from the loaded buffer";
    EXPECT_TRUE(seq_->isRecording(0))
        << "recording_ round-trips through the state buffer (reset() does not clear it; queued entries never touch it)";
}

TEST_F(SequencerPhase2, UnsupportedStateVersionPreservesQueuedEntries) {
    seq_->start();
    seq_->setRecording(0, true);
    seq_->mainSequencerTic(100);
    seq_->queueNote(0, 60, 100);
    uint8_t unsupportedState[] = {0};
    seq_->setFullState(unsupportedState);
    EXPECT_FALSE(seq_->isSeqActivated(0)) << "the queued note still defers";
    seq_->processAsyncActions();
    EXPECT_TRUE(seq_->isSeqActivated(0))
        << "a rejected state must not discard otherwise valid queued actions";
}

TEST_F(SequencerPhase2, QueueOverflowDropsNewestAndCounts) {
    seq_->start();
    seq_->setRecording(0, true);
    constexpr int kUsableCapacity = 63;
    for (int queued = 0; queued < kUsableCapacity; queued++) {
        seq_->queueNote(0, 60, 100);
        ASSERT_EQ(seq_->getDroppedAsyncActions(), 0)
            << "the queue dropped before its specified 63-entry capacity at index " << queued;
    }

    // A distinguishable 64th action must be the one dropped. If the ring
    // overwrites an older note and retains this stop, running_ becomes false.
    seq_->queueNewSeqValue(0, SEQ_VALUE_PLAY_ALL, 0);
    EXPECT_EQ(seq_->getDroppedAsyncActions(), 1) << "only the newest entry dropped";
    EXPECT_FALSE(seq_->isSeqActivated(0)) << "no mutation before the drain";
    seq_->processAsyncActions();
    EXPECT_EQ(seq_->getDroppedAsyncActions(), 1) << "the drain does not reset the counter";
    EXPECT_TRUE(seq_->isRunning()) << "the overflow stop action must not enter the queue";
    EXPECT_TRUE(seq_->isSeqActivated(0)) << "all 63 accepted notes applied";

    // The drain freed the queue: further entries fit without new drops.
    seq_->queueNote(0, 62, 100);
    seq_->processAsyncActions();
    EXPECT_EQ(seq_->getDroppedAsyncActions(), 1);
}

// ===========================================================================
// 8.1-H4 — corrupt action-list hardening (spec-8-1-h4-seq-list-hardening).
//
// The walk (processActionBetwen) terminates only on `when` windows; a chain
// that re-enters the head sentinel loops forever inside SysTick and froze
// the unit on device (autopsy 2026-08-28: head.next=12, 48 zero-`when`
// notes 12..59 sequential, actions[59].next=0 -> head, tail sentinel
// zeroed). These tests pin the three guards: the bounded walk, the
// head-bypass-proof insertNote, and validateActionList's load healing.
// ===========================================================================

extern SeqMidiAction actions[SEQ_ACTION_SIZE];

namespace {

// The on-device corpse, verbatim: every note when=0, strictly sequential
// links, the last note pointing back at the HEAD sentinel, all other
// sentinels zeroed (tail.when=0, not the 4095 bar limit).
void WriteH4CorpseList() {
    memset(actions, 0, sizeof(actions));
    static const uint8_t kMelody[] = {
        65, 65, 60, 62, 65, 65, 65, 65, 65, 65, 60, 62,
        62, 62, 62, 62, 62, 65, 65, 65, 60, 60, 62, 62,
    };
    actions[0].nextIndex = 12;  // head -> first dynamic slot
    for (int i = 0; i < 48; i++) {
        const uint16_t slot = 12 + i;
        actions[slot].when = 0;
        actions[slot].nextIndex = (i == 47) ? 0 : slot + 1;  // last -> HEAD
        actions[slot].actionType = SEQ_ACTION_NOTE;
        actions[slot].param1 = kMelody[i / 2];
        actions[slot].param2 = (i % 2 == 0) ? 100 : 0;      // on/off pairs
    }
}

// Craft a V2 state with lastFreeAction_ and seqActivated[0] patched — the
// header the poisoned slot carried (tempo 90, masks 4095, activated t1).
void WriteH4State(Sequencer* seq, uint16_t lastFreeAction, uint8_t seqActivated0) {
    uint8_t buf[256];
    uint32_t size = 0;
    seq->getFullState(buf, &size);
    ASSERT_EQ(size, kExpectedStateSize);
    std::memcpy(buf + kLastFreeActionOffset, &lastFreeAction, sizeof(uint16_t));
    buf[20 + 4] = seqActivated0;  // timbre 0 seqActivated (after u16 pair)
    seq->setFullState(buf);
}

// Walk a chain from the head sentinel; returns the hop count, or -1 when the
// chain never reaches the tail within the budget (corrupt).
int ChainHopsToTail(uint8_t instrument) {
    const uint16_t head = instrument * 2;
    const uint16_t tail = head + 1;
    uint16_t index = actions[head].nextIndex;
    int hops = 0;
    while (index != tail) {
        if (++hops > 2048) return -1;
        index = actions[index].nextIndex;
    }
    return hops;
}

bool gInsertPublishSawLiveNode = false;
void CaptureInsertPublication(Sequencer* seq, uint8_t instrument) {
    const uint16_t published = actions[instrument * 2].nextIndex;
    gInsertPublishSawLiveNode = published < seq->getLastFreeActionForTest();
}

}  // namespace

TEST_F(SequencerPhase2, H4CorpseWalkIsBoundedAndHeals) {
    WriteH4State(seq_, 60, 1);
    WriteH4CorpseList();
    ASSERT_TRUE(seq_->isSeqActivated(0));
    ASSERT_EQ(seq_->getWalkTrips(), 0);

    seq_->start();        // external clock (default): running_, no precount
    seq_->onMidiStart();  // rewind: timers to 0
    seq_->mainSequencerTic(0);   // cursor at tail: the call ends at the wrap
    seq_->mainSequencerTic(1);   // head-side entry: walks into the head-bypass -> trip

    // Pre-fix the head-side walk never returned (infinite SysTick loop).
    // Post-fix: the head-bypass link trips the guard — walk stops, list
    // deactivated, trip counted. The sequencer itself keeps running.
    EXPECT_EQ(seq_->getWalkTrips(), 1);
    EXPECT_FALSE(seq_->isSeqActivated(0));
    EXPECT_TRUE(seq_->isRunning());

    // Deactivated: later tics must not re-trip (no repeated note spam).
    seq_->mainSequencerTic(100);
    seq_->mainSequencerTic(4095);
    EXPECT_EQ(seq_->getWalkTrips(), 1);
}

TEST_F(SequencerPhase2, RuntimeTripRepairsBeforeRecordingCanReactivatePoison) {
    WriteH4State(seq_, 60, 1);
    WriteH4CorpseList();
    seq_->setRecording(0, true);

    seq_->start();
    seq_->onMidiStart();
    seq_->mainSequencerTic(0);
    seq_->mainSequencerTic(1);
    ASSERT_EQ(seq_->getWalkTrips(), 1);
    ASSERT_FALSE(seq_->isSeqActivated(0));

    // The first post-trip note repairs the chain in the main-loop context and
    // is dropped+counted rather than being linked in front of the poison.
    seq_->queueNote(0, 67, 100);
    seq_->processAsyncActions();
    EXPECT_EQ(seq_->getDroppedInsertNotes(), 1);
    EXPECT_FALSE(seq_->isSeqActivated(0));
    EXPECT_EQ(actions[0].nextIndex, 1);
    EXPECT_EQ(seq_->getLastFreeActionForTest(), NUMBER_OF_TIMBRES * 2);

    // Recording can then resume on the clean list without another trip.
    seq_->queueNote(0, 69, 100);
    seq_->processAsyncActions();
    EXPECT_TRUE(seq_->isSeqActivated(0));
    EXPECT_EQ(actions[0].nextIndex, 12);
    EXPECT_EQ(actions[12].nextIndex, 1);
    EXPECT_EQ(ChainHopsToTail(0), 1);
    seq_->mainSequencerTic(2);
    EXPECT_EQ(seq_->getWalkTrips(), 1);
}

TEST_F(SequencerPhase2, InsertAtBarWrapCursorDoesNotBypassHead) {
    seq_->start();
    seq_->setRecording(0, true);

    // Record one note mid-bar, then drive the walk across the bar wrap. The
    // walk exits the wrap with the cursor at prev=tail/next=head — the exact
    // state that used to mint a head-bypass link on the next recorded note.
    seq_->mainSequencerTic(100);
    seq_->queueNote(0, 60, 100);
    seq_->processAsyncActions();
    ASSERT_TRUE(seq_->isSeqActivated(0));
    // First walk after activation: window [0..200] plays the note and exits
    // at the tail (realistic incremental timer steps — no full-bar jumps).
    seq_->mainSequencerTic(200);
    EXPECT_EQ(seq_->getWalkTrips(), 0);
    // Walk across the bar end: the cursor exits at prev=tail/next=head —
    // the exact state that used to mint a head-bypass link.
    seq_->mainSequencerTic(4095);
    EXPECT_EQ(seq_->getWalkTrips(), 0);

    // Note recorded in the bar-wrap window: must splice at bar start with a
    // walk-safe successor (never next == head).
    seq_->queueNote(0, 62, 100);
    seq_->processAsyncActions();
    EXPECT_EQ(seq_->getDroppedInsertNotes(), 0);
    EXPECT_EQ(seq_->getWalkTrips(), 0);

    // Structure: head -> new note -> old note -> tail, every hop in range.
    EXPECT_EQ(actions[0].nextIndex, 13);  // newest at bar start
    EXPECT_EQ(actions[13].when, 0);
    EXPECT_EQ(actions[13].nextIndex, 12);
    EXPECT_EQ(actions[12].nextIndex, 1);  // tail sentinel
    EXPECT_EQ(ChainHopsToTail(0), 2);      // two dynamic hops to the tail

    // And the repaired chain still plays through the next bar: incremental
    // windows only, no trip, still activated.
    seq_->mainSequencerTic(0);
    EXPECT_EQ(synth_.getLowerNote(0), 62) << "the normalized bar-start note fires at zero";
    seq_->mainSequencerTic(100);
    EXPECT_EQ(synth_.getLowerNote(0), 60) << "the existing 100-tick note is not delayed to bar end";
    seq_->mainSequencerTic(4095);
    EXPECT_EQ(seq_->getWalkTrips(), 0);
    EXPECT_TRUE(seq_->isSeqActivated(0));
}

TEST_F(SequencerPhase2, InsertPublishesLiveLimitBeforeExposingLink) {
    seq_->start();
    seq_->setRecording(0, true);
    seq_->mainSequencerTic(100);

    gInsertPublishSawLiveNode = false;
    seq_->setInsertPublishHookForTest(CaptureInsertPublication);
    seq_->queueNote(0, 60, 100);
    seq_->processAsyncActions();
    seq_->setInsertPublishHookForTest(nullptr);

    EXPECT_TRUE(gInsertPublishSawLiveNode)
        << "an IRQ observing the published link must also see its slot inside lastFreeAction";
    EXPECT_EQ(actions[0].nextIndex, 12);
    EXPECT_GT(seq_->getLastFreeActionForTest(), actions[0].nextIndex);
    EXPECT_EQ(seq_->getWalkTrips(), 0);
}

TEST_F(SequencerPhase2, InsertIntoZeroedHeadDropsAndCounts) {
    // The H4 minting scenario: a fully zeroed action block (what a poison
    // slot memcpy leaves) has head.next == head (self-loop). Recording must
    // refuse to splice into that — the link that used to start every chain
    // corruption — and count the drop instead.
    WriteH4State(seq_, 12, 0);  // empty live region, deactivated
    memset(actions, 0, sizeof(actions));  // head.next = 0 = head

    seq_->start();
    seq_->setRecording(0, true);
    seq_->onMidiStart();
    seq_->mainSequencerTic(0);
    seq_->queueNote(0, 60, 100);
    seq_->processAsyncActions();

    EXPECT_EQ(seq_->getDroppedInsertNotes(), 1);
    EXPECT_FALSE(seq_->isSeqActivated(0));
    int liveNotes = 0;
    for (int i = 12; i < SEQ_ACTION_SIZE; i++) {
        if (actions[i].actionType == SEQ_ACTION_NOTE) liveNotes++;
    }
    EXPECT_EQ(liveNotes, 0);
}

TEST_F(SequencerPhase2, SameTickChordRecordsBothNotes) {
    // The B3 guard must not throttle legitimate inserts: two note-ons in the
    // same tick both land.
    seq_->start();
    seq_->setRecording(0, true);
    seq_->mainSequencerTic(100);
    seq_->queueNote(0, 60, 100);
    seq_->queueNote(0, 64, 100);
    seq_->processAsyncActions();

    EXPECT_EQ(seq_->getDroppedInsertNotes(), 0);
    bool saw60 = false, saw64 = false;
    for (int i = 12; i < 32; i++) {
        if (actions[i].actionType != SEQ_ACTION_NOTE) continue;
        saw60 |= actions[i].param1 == 60;
        saw64 |= actions[i].param1 == 64;
    }
    EXPECT_TRUE(saw60);
    EXPECT_TRUE(saw64);
    EXPECT_EQ(ChainHopsToTail(0), 2);
}

TEST_F(SequencerPhase2, ValidateActionListResetsPoisonChain) {
    // A pristine instrument validates clean, no reset counted.
    EXPECT_TRUE(seq_->validateActionList(1));
    EXPECT_EQ(seq_->getListsResetOnLoad(), 0);

    WriteH4State(seq_, 60, 1);
    WriteH4CorpseList();

    // The corpse fails validation and is healed in place.
    EXPECT_FALSE(seq_->validateActionList(0));
    EXPECT_EQ(seq_->getListsResetOnLoad(), 1);
    EXPECT_FALSE(seq_->isSeqActivated(0));
    EXPECT_EQ(actions[0].nextIndex, 1);            // head -> tail
    EXPECT_EQ(actions[1].when, 4095);              // bar limit restored
    EXPECT_EQ(actions[1].nextIndex, 0);            // tail -> head (circular)
    EXPECT_EQ(ChainHopsToTail(0), 0);
    for (int i = 12; i < 60; i++) {
        EXPECT_EQ(actions[i].actionType, SEQ_ACTION_NONE)
            << "reachable poison slot " << i << " must be reclaimed";
    }

    // The healed list validates clean on re-check (idempotent, no new reset).
    EXPECT_TRUE(seq_->validateActionList(0));
    EXPECT_EQ(seq_->getListsResetOnLoad(), 1);
}

TEST_F(SequencerPhase2, ValidateRejectsEveryCorruptSentinelInvariant) {
    for (int corruption = 0; corruption < 4; corruption++) {
        SCOPED_TRACE(corruption);
        seq_->reset(false);
        WriteH4State(seq_, 13, 1);
        switch (corruption) {
        case 0:
            actions[0].when = 7;                    // head must be at zero
            break;
        case 1:
            actions[0].actionType = SEQ_ACTION_NOTE; // sentinels never emit
            break;
        case 2:
            actions[1].actionType = SEQ_ACTION_NOTE;
            break;
        case 3:
            actions[1].nextIndex = 12;              // tail may only wrap to head
            break;
        }

        const uint32_t before = seq_->getListsResetOnLoad();
        EXPECT_FALSE(seq_->validateActionList(0));
        EXPECT_EQ(seq_->getListsResetOnLoad(), before + 1);
        EXPECT_FALSE(seq_->isSeqActivated(0));
        EXPECT_EQ(actions[0].when, 0);
        EXPECT_EQ(actions[0].actionType, SEQ_ACTION_NONE);
        EXPECT_EQ(actions[0].nextIndex, 1);
        EXPECT_EQ(actions[1].actionType, SEQ_ACTION_NONE);
        EXPECT_EQ(actions[1].nextIndex, 0);
    }
}

TEST_F(SequencerPhase2, ValidateRejectsDescendingDynamicTimestamps) {
    WriteH4State(seq_, 14, 1);
    actions[0].nextIndex = 12;
    actions[12].when = 100;
    actions[12].nextIndex = 13;
    actions[12].actionType = SEQ_ACTION_NOTE;
    actions[13].when = 50;
    actions[13].nextIndex = 1;
    actions[13].actionType = SEQ_ACTION_NOTE;

    EXPECT_FALSE(seq_->validateActionList(0));
    EXPECT_FALSE(seq_->isSeqActivated(0));
    EXPECT_EQ(actions[0].nextIndex, 1);
}

TEST_F(SequencerPhase2, HealingFullPoisonReclaimsAllocatorCapacity) {
    WriteH4State(seq_, SEQ_ACTION_SIZE, 1);
    memset(actions, 0, sizeof(actions));
    actions[0].nextIndex = NUMBER_OF_TIMBRES * 2;
    for (uint16_t index = NUMBER_OF_TIMBRES * 2; index < SEQ_ACTION_SIZE; index++) {
        actions[index].when = 0;
        actions[index].actionType = SEQ_ACTION_NOTE;
        actions[index].nextIndex = index + 1;
    }
    actions[SEQ_ACTION_SIZE - 1].nextIndex = 0;

    EXPECT_FALSE(seq_->validateActionList(0));
    EXPECT_EQ(seq_->getLastFreeActionForTest(), NUMBER_OF_TIMBRES * 2);
    for (uint16_t index = NUMBER_OF_TIMBRES * 2; index < SEQ_ACTION_SIZE; index++) {
        EXPECT_EQ(actions[index].actionType, SEQ_ACTION_NONE);
    }
}

TEST_F(SequencerPhase2, OversizedAllocatorHeaderIsNormalizedAndCanRecord) {
    WriteH4State(seq_, UINT16_MAX, 1);
    ASSERT_TRUE(seq_->validateActionList(0));
    EXPECT_EQ(seq_->getLastFreeActionForTest(), NUMBER_OF_TIMBRES * 2);

    seq_->start();
    seq_->setRecording(0, true);
    seq_->mainSequencerTic(100);
    seq_->queueNote(0, 60, 100);
    seq_->processAsyncActions();
    EXPECT_EQ(seq_->getDroppedInsertNotes(), 0);
    EXPECT_EQ(actions[0].nextIndex, 12);
    EXPECT_EQ(actions[12].nextIndex, 1);
}

// ---------------------------------------------------------------------------
// 8.1-H4 review patches (Blind Hunter + Edge Case Hunter, step-04 loop):
// full-bar single-pass, trip cleans up voices, sentinel-region lastFree
// refusal, counted resync trips.
// ---------------------------------------------------------------------------

TEST_F(SequencerPhase2, FullBarWindowOnValidListPlaysOnceAndStaysActive) {
    // BH2: a full-bar window [0..4095] in ONE walk call — reachable when the
    // first walk after activation lands at the bar end — must play the bar
    // ONCE and stop at the wrap. The unpatched budget admitted two extra
    // hops past the wrap: the first note(s) replayed, then a VALID list was
    // false-deactivated (silent mid-performance stop).
    seq_->start();
    seq_->setRecording(0, true);
    seq_->mainSequencerTic(0);  // not yet activated: walk skipped, last=-1
    seq_->queueNote(0, 60, 100);
    seq_->processAsyncActions();
    ASSERT_TRUE(seq_->isSeqActivated(0));

    // First walk ever, straight to the bar end: wrap call is empty, then
    // the regular call runs [0..4095] — the whole bar in one window.
    seq_->mainSequencerTic(4095);
    EXPECT_EQ(seq_->getWalkTrips(), 0) << "valid list must not trip";
    EXPECT_TRUE(seq_->isSeqActivated(0)) << "no silent deactivation";
    EXPECT_EQ(synth_.getLowerNote(0), 64) << "not yet fired: cursor was at the tail, the note plays from the next bar (loop-pass semantics)";

    // The wrap ended the call cleanly: the next bar services the head side
    // and replays the note (normal looping), still without a trip.
    seq_->mainSequencerTic(0);
    seq_->mainSequencerTic(100);
    EXPECT_EQ(seq_->getWalkTrips(), 0);
    EXPECT_TRUE(seq_->isSeqActivated(0));
    EXPECT_EQ(synth_.getLowerNote(0), 60) << "fired exactly once on the second bar and holds";
}

TEST_F(SequencerPhase2, TripKillsVoicesTheWalkStarted) {
    // BH3: a trip between a recorded note-on and its note-off must not
    // strand the voice — every other list surgery kills voices; the walk
    // trip must too. Slot 12's note-on plays and RENDERS a block before the
    // walk reaches slot 13's head-bypass, so the trip's quick kill takes
    // the deterministic killNow path (note already played).
    WriteH4State(seq_, 14, 1);
    memset(actions, 0, sizeof(actions));
    actions[0].nextIndex = 12;
    actions[12].when = 0;
    actions[12].nextIndex = 13;
    actions[12].actionType = SEQ_ACTION_NOTE;
    actions[12].param1 = 60;
    actions[12].param2 = 100;   // note-on: fires at [0..1]
    actions[13].when = 2;
    actions[13].nextIndex = 0;  // head-bypass: trips at [1..2]
    actions[13].actionType = SEQ_ACTION_NOTE;
    actions[13].param1 = 62;
    actions[13].param2 = 100;

    seq_->start();
    seq_->onMidiStart();
    seq_->mainSequencerTic(0);  // tail-side call: ends at the wrap
    seq_->mainSequencerTic(1);  // head-side entry: slot 12's note-on fires
    EXPECT_EQ(synth_.getLowerNote(0), 60) << "the walk started the note";

    int32_t b1[64], b2[64], b3[64];  // 64 samples per buffer (see synth_core_test)
    synth_.buildNewSampleBlock(b1, b2, b3);  // the note becomes played

    seq_->mainSequencerTic(2);  // walk reaches slot 13 -> head-bypass -> trip
    EXPECT_EQ(seq_->getWalkTrips(), 1);
    EXPECT_FALSE(seq_->isSeqActivated(0));

    // The trip's allNoteOffQuick killNow'd the voice: it retires within a
    // couple of rendered blocks (without the kill it would ring forever).
    bool released = !synth_.isPlaying();
    for (int i = 0; i < 10 && !released; i++) {
        synth_.buildNewSampleBlock(b1, b2, b3);
        released = !synth_.isPlaying();
    }
    EXPECT_TRUE(released) << "the note-on fired at slot 12 must be released by the trip";
}

TEST_F(SequencerPhase2, InsertRefusesSentinelRegionLastFreeAction) {
    // BH1: a loaded header can carry lastFreeAction_ inside the sentinel
    // region. Recording must refuse (and count) rather than splice a new
    // note into, say, actions[5] — another instrument's head sentinel.
    WriteH4State(seq_, 5, 0);   // inactive: activation is not needed to refuse
    memset(actions, 0, sizeof(actions));

    seq_->start();
    seq_->setRecording(0, true);
    seq_->onMidiStart();
    seq_->mainSequencerTic(0);
    seq_->queueNote(0, 60, 100);
    seq_->processAsyncActions();

    EXPECT_EQ(seq_->getDroppedInsertNotes(), 1);
    EXPECT_EQ(actions[5].when, 0) << "sentinel slot must stay untouched";
    EXPECT_EQ(actions[5].nextIndex, 0);
    EXPECT_EQ(actions[5].actionType, 0);
    EXPECT_FALSE(seq_->isSeqActivated(0));
}

TEST_F(SequencerPhase2, ResyncTripIsCounted) {
    // EC4: corruption surfacing through resyncNextAction (bars change on a
    // poison chain) must be observable, not silent.
    WriteH4State(seq_, 60, 1);
    WriteH4CorpseList();
    ASSERT_TRUE(seq_->isSeqActivated(0));

    seq_->start();
    seq_->onMidiStart();
    seq_->setNumberOfBars(0, 2);  // arms nextActionTimerOutOfSync_
    ASSERT_EQ(seq_->getWalkTrips(), 0);
    seq_->mainSequencerTic(1000);  // resync walks from the head -> head-bypass link
    EXPECT_EQ(seq_->getWalkTrips(), 1u) << "resync trip must be counted once";
    EXPECT_FALSE(seq_->isSeqActivated(0));
    EXPECT_EQ(seq_->getPreviousActionIndexForTest(0), 0);
    EXPECT_EQ(seq_->getNextActionIndexForTest(0), 1);
    EXPECT_FALSE(seq_->isNextActionTimerOutOfSyncForTest(0));
}

TEST_F(SequencerPhase2, ResyncNearZeroDoesNotFalseTripValidList) {
    seq_->start();
    seq_->setRecording(0, true);
    seq_->mainSequencerTic(0);
    seq_->queueNote(0, 60, 100);
    seq_->processAsyncActions();
    ASSERT_TRUE(seq_->isSeqActivated(0));

    seq_->setNumberOfBars(0, 2);
    seq_->mainSequencerTic(1);  // previous service point wraps below zero

    EXPECT_EQ(seq_->getWalkTrips(), 0);
    EXPECT_TRUE(seq_->isSeqActivated(0));
    EXPECT_GE(seq_->getNextActionIndexForTest(0), 1);
}

TEST_F(SequencerPhase2, TripClearsArpeggiatorStackBeforeLaterClocks) {
    synth_.setNewValueFromMidi(0, ROW_ARPEGGIATOR1,
        ENCODER_ARPEGGIATOR_BPM, 120.0f);
    synth_.setNewValueFromMidi(0, ROW_ARPEGGIATOR1,
        ENCODER_ARPEGGIATOR_CLOCK, static_cast<float>(CLOCK_INTERNAL));

    WriteH4State(seq_, 14, 1);
    memset(actions, 0, sizeof(actions));
    actions[0].nextIndex = 12;
    actions[12].when = 0;
    actions[12].nextIndex = 13;
    actions[12].actionType = SEQ_ACTION_NOTE;
    actions[12].param1 = 60;
    actions[12].param2 = 100;  // enters the arp stack
    actions[13].when = 2;
    actions[13].nextIndex = 0;
    actions[13].actionType = SEQ_ACTION_NOTE;
    actions[13].param1 = 62;
    actions[13].param2 = 100;

    seq_->start();
    seq_->onMidiStart();
    seq_->mainSequencerTic(0);
    seq_->mainSequencerTic(1);
    seq_->mainSequencerTic(2);  // trip after the arp note was queued
    ASSERT_EQ(seq_->getWalkTrips(), 1);

    int32_t b1[64], b2[64], b3[64];
    bool retriggered = false;
    for (int block = 0; block < 2000; block++) {
        synth_.buildNewSampleBlock(b1, b2, b3);
        retriggered |= synth_.isPlaying();
    }
    EXPECT_FALSE(retriggered)
        << "a trip-cleared arp stack must not retrigger on later internal clocks";
}
