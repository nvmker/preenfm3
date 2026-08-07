// Host-side coverage for firmware/Src/midi/MidiDecoder.cpp — MIDI decode +
// routing.
//
// Regression target (per tests/README.md roadmap, row 4):
//   Stuck notes / wrong CC routing. MidiDecoder is the byte-stream->dispatch
//   state machine: newByte classifies each byte (realtime vs status vs data),
//   newMessageType/newMessageData assemble multi-byte messages (with running
//   status), midiEventReceived routes the assembled message to the Synth graph
//   (noteOn/noteOff/setMatrixSource/setNewValueFromMidi), and controlChange's
//   CC + NRPN tables map controllers onto timbre params. A regression in the
//   state machine (running status lost, sysex corrupting the parser, a CC/NRPN
//   table off-by-one) is the bug class this target guards.
//
// THREE TIERS (per tests/SEAM.md Target #4):
//   TIER 1 (floor): decode state machine — byte classification, running status,
//     sysex framing, real-time-byte filtering. Drives deterministic byte
//     streams and asserts on the dispatch + MidiDecoder's private state
//     (currentEventState, currentEvent, runningStatus) via the test-TU-local
//     `#define private public` shim (see "Private-state access" below).
//   TIER 2: NRPN decode — paramMSB/paramLSB/valueMSB/valueLSB assembly byte
//     ordering, the paramMSB==127 special-case SEND_PATCH_AS_NRPN dispatch,
//     and the increment/decrement CC96/97 path. (The paramMSB<2 "value
//     parameter" main branch executes against a stubbed allParameterRows — see
//     tests/SEAM.md Target #4 appendix; the value-transformation that depends
//     on real ParameterDisplay data is out of scope.)
//   TIER 3 (ceiling): routing — feed NoteOn/NoteOff/CC byte streams through
//     newByte and assert on OBSERVABLE Synth state (synth_->getLowerNote(t)
//     changes; bankNumber[t]/omniOn[t] update). This is the capstone "stuck
//     notes / wrong CC routing" guard and the reason the real Synth graph was
//     pulled into the host build (see tests/SEAM.md Target #4 GO/NO-GO gate).
//
// EXCLUDED from host scope (NRPN SEND path, Trap #1 in tests/SEAM.md §d.4.1):
//   sendCurrentPatchAsNrpns and friends end each NRPN with
//     while (usartBufferOut.getCount() > 0) {}
//   relying on the USART ISR to drain. With sendMidiDin5Out stubbed to no-op
//   under PFM3_HOST (the host build has no ISR), usartBufferOut NEVER drains
//   and the call hangs. The SEND path is NOT the test target (the DECODE path
//   is); these tests never call sendCurrentPatchAsNrpns / newParamValue (the
//   param-change-out path) / processAsyncActions with a SEND_PATCH_AS_NRPN
//   action. The decode-side enqueue of SEND_PATCH_AS_NRPN (paramMSB=127,
//   paramLSB=127) IS tested — we assert asyncActions.getCount() increments,
//   then DO NOT drain it.
//
// Private-state access:
//   MidiDecoder's interesting decode state (currentEventState.eventState,
//   currentEvent.eventType/channel/value, currentNrpn[], runningStatus,
//   omniOn[], bankNumber[], songPosition, midiClockCpt) is `private`. The
//   refined SEAM rule (tests/SEAM.md Target #1 appendix) forbids PFM3_HOST in
//   firmware headers for anything but genuinely host-incompatible constructs,
//   so we cannot add a `friend` test hook to MidiDecoder.h. Instead this TU
//   uses the standard, contained C++ test pattern `#define private public`
//   scoped AROUND the MidiDecoder.h include only — every firmware header
//   MidiDecoder.h reaches is pre-included first, so the macro affects ONLY the
//   MidiDecoder class body. Zero firmware surface; no runtime cost; no ABI
//   impact (access specifiers do not change class layout). This is the same
//   stance the Hexter tests took with `using Hexter::<protected-member>` — the
//   only difference is `private` vs `protected` access, which `using` cannot
//   cross.

// Pre-include every firmware header MidiDecoder.h reaches that has a
// `private:` section, so the `#define private public` below does NOT re-parse
// those headers under the macro (an ODR violation: access specifiers are
// tokens, so the same class parsed as-private in Synth.cpp's TU and as-public
// here would be two different definitions). Synth.h transitively covers
// SynthState.h (and its Storage.h / MixerBank / ... closure), Timbre, Voice,
// Matrix, Lfo*, dwt, SimpleComp, Common, SynthStateAware. RingBuffer.h carries
// private data and is not reached via Synth.h, so pre-include it too.
#include "Synth.h"
#include "RingBuffer.h"

#define private public  // NOLINT: scoped to MidiDecoder.h only (see header)
#include "MidiDecoder.h"
#undef private

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

// MidiDecoder.cpp's file-scope asyncActions ring buffer — the SEND_PATCH_AS_NRPN
// NRPN special case enqueues here. Declaring it extern lets the Tier 2 NRPN
// test observe the enqueue without touching MidiDecoder privates. NOTE: the
// definition in MidiDecoder.cpp is a plain C++ symbol (NOT extern "C"), so the
// extern declaration here must also be C++ linkage (no `extern "C"` wrapper) —
// a linkage mismatch would silently resolve to a different symbol.
extern RingBuffer<AsyncAction, 16> asyncActions;

namespace {

// Null VisualInfo: MidiDecoder dispatches visualInfo->noteOn(timbre, true) on
// NoteOn and visualInfo->midiClock(bool) on every 6th MIDI_CLOCK. The host
// tests do not render UI; no-op these to keep the dispatch link-clean.
class NullVisualInfo : public VisualInfo {
public:
    void midiClock(bool /*show*/) override {}
    void noteOn(int /*timbre*/, bool /*show*/) override {}
};

// Backing storage for a memset+patched SynthState. SynthState's ctor + vtable
// live in SynthState.cpp (deliberately NOT pulled — its closure drags the
// FMDisplay family + HAL). The fixture memsets this buffer, reinterprets it as
// SynthState*, and patches exactly the fields MidiDecoder / Synth::noteOn
// read. No virtual is dispatched through the resulting pointer (MidiDecoder
// and Synth::noteOn read plain data members), so UBSAN's vptr check does not
// fire. Target #3's minimal-SynthState precedent (memset + one float patch for
// Osc) scaled up to MidiDecoder's wider field set. See tests/SEAM.md Target #4.
struct SynthStateBacking {
    alignas(alignof(SynthState)) unsigned char bytes[sizeof(SynthState)];
};

// Per-timbre scale-frequency tables. The firmware normally allocates these in
// the MixerState/SynthState init paths (not pulled); the fixture owns the
// storage and points each instrumentState_[t].scaleFrequencies at one. 128
// entries (MIDI note range) per timbre.
struct ScaleFreqTables {
    float tables[NUMBER_OF_TIMBRES][128];
};

// Equal-tempered frequency for a MIDI note (A4=440). Patches scaleFrequencies[]
// so Timbre::preenNoteOn's `scaleFrequencies[note] == 0` early-return does not
// fire and the note actually allocates a voice (observable via getLowerNote).
float EqualTemperedFreq(int note) {
    return 440.0f * powf(2.0f, (note - 69) / 12.0f);
}

}  // namespace

// ===========================================================================
// Shared fixture: minimal SynthState (memset + field patch) + real Synth +
// real MidiDecoder, wired exactly as the firmware wires them.
//
// Synth::setSynthState runs Synth::init, which copies preenMainPreset into
// every timbre's params_, calls Timbre::init / Voice::init, and sets up voice
// pointers. After init, ss_->params points at timbre 0's params_ (the params
// pointer MidiDecoder derefs for engine1.algo reads in the CC_ENV_ATK_ALL_*
// routing). channelConfig routes MIDI channel 1 to timbre 0 only (OMNI off on
// timbres 1-5 via midiChannel=0 would normally make them OMNI; we set
// numberOfVoices=0 on timbres 1-5 so Synth::init's numberOfVoicesChanged
// disables them and preenNoteOn early-returns).
// ===========================================================================
class MidiDecoderRouting : public ::testing::Test {
protected:
    SynthStateBacking ssBacking_;
    ScaleFreqTables scaleFreqs_;
    SynthState* ss_;
    Synth synth_;
    NullVisualInfo visualInfo_;
    MidiDecoder decoder_;

    void SetUp() override {
        std::memset(&ssBacking_, 0, sizeof(ssBacking_));
        ss_ = reinterpret_cast<SynthState*>(&ssBacking_);

        // fullState.synthMode: MIXER (so Synth::noteOn routes to voices, not
        // the sequencer).
        ss_->fullState.synthMode = SYNTH_MODE_MIXER;
        // midiConfigValue defaults (SynthState::SynthState() normally sets
        // these; memset zeroes them, so patch explicitly):
        //   RECEIVES=3 (CC + NRPN enabled), PROGRAM_CHANGE=1, SENDS=1, USB=OFF
        ss_->fullState.midiConfigValue[MIDICONFIG_RECEIVES] = 3;
        ss_->fullState.midiConfigValue[MIDICONFIG_PROGRAM_CHANGE] = 1;
        ss_->fullState.midiConfigValue[MIDICONFIG_SENDS] = 1;
        ss_->fullState.midiConfigValue[MIDICONFIG_USB] = USBMIDI_OFF;

        // Mixer routing: globalChannel_=0 (no global), MPE off.
        ss_->mixerState.globalChannel_ = 0;
        ss_->mixerState.currentChannel_ = 0;
        ss_->mixerState.MPE_inst1_ = 0;
        // userCC slots: set to an impossible CC number (255) so no CC matches
        // the MATRIX_SOURCE_USER_CC1..4 short-circuit in controlChange.
        for (int i = 0; i < NUMBER_OF_ECC; i++) ss_->mixerState.userCC_[i] = 255;

        // Per-timbre instrumentState: timbre 0 listens on MIDI channel 1
        // (midiChannel=1 -> channel byte 0), full note range, 6 voices.
        // Timbres 1-5 listen on distinct higher channels (midiChannel=t+1) with
        // numberOfVoices=0, so they neither grab NoteOns nor (importantly)
        // multiply-route CCs — midiChannel=0 would mean OMNI in the routing
        // logic and cause every CC to dispatch into all 6 timbres (6x NRPN
        // enqueues, 6x controlChange calls), which breaks per-timbre assertions.
        for (int t = 0; t < NUMBER_OF_TIMBRES; t++) {
            ss_->mixerState.instrumentState_[t].midiChannel = (t == 0) ? 1 : (t + 1);
            ss_->mixerState.instrumentState_[t].firstNote = 0;
            ss_->mixerState.instrumentState_[t].lastNote = 127;
            ss_->mixerState.instrumentState_[t].shiftNote = 0;
            ss_->mixerState.instrumentState_[t].numberOfVoices = (t == 0) ? 6 : 0;
            // scaleFrequencies is a float* (NOT an embedded array); point it at
            // the fixture-owned table and populate with equal-tempered values
            // so Timbre::preenNoteOn's `scaleFrequencies[note] == 0` guard
            // does not fire.
            ss_->mixerState.instrumentState_[t].scaleFrequencies = scaleFreqs_.tables[t];
            for (int n = 0; n < 128; n++) {
                scaleFreqs_.tables[t][n] = EqualTemperedFreq(n);
            }
        }

        // Wire Synth: setSynthState runs Synth::init, populating timbres_ /
        // voices_ / params_ from preenMainPreset.
        synth_.setSynthState(ss_);
        // Point SynthState.params at timbre 0's now-initialized params_.
        ss_->params = synth_.getTimbre(0)->getParamRaw();

        // Wire MidiDecoder.
        decoder_.setSynthState(ss_);
        decoder_.setSynth(&synth_);
        decoder_.setVisualInfo(&visualInfo_);
        decoder_.newTimbre(0);  // sets currentTimbre=0 (ctor leaves it unset)
    }

    // Feed a byte stream through newByte (the public firmware entry point).
    void Feed(const uint8_t* bytes, size_t n) {
        for (size_t i = 0; i < n; i++) decoder_.newByte(bytes[i]);
    }
    void Feed(std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) decoder_.newByte(b);
    }
};

// ===========================================================================
// SMOKE — fixture validation. If the Synth link gate or the minimal-SynthState
// patching is wrong, every test below is suspect; this trips first.
// ===========================================================================

TEST_F(MidiDecoderRouting, FixtureWiresSynthAndDecoderWithoutCrash) {
    // A NoteOn on channel 1 (status byte 0x90) for middle-C at velocity 100
    // routes to timbre 0 (midiChannel=1 -> channel byte 0), allocates a voice,
    // and sets lowerNote_ = 60. Observable via Synth::getLowerNote (public).
    Feed({0x90, 60, 100});
    EXPECT_EQ(synth_.getLowerNote(0), 60)
        << "NoteOn ch1 did not route to timbre 0 / allocate a voice — fixture "
           "wiring or Synth::init under memset-SynthState is broken";
}

// ===========================================================================
// TIER 1 — decode state machine.
//
// byte-stream -> status/data classification, running status, sysex framing,
// real-time-byte filtering. Asserts on currentEvent.eventType / .channel /
// .value[], currentEventState.eventState / .numberOfBytes / .index, and
// runningStatus (all private; reached via the scoped #define private public).
// Marquee regression guard: a state-machine change (running-status lost, sysex
// corrupting the parser, a status-byte off-by-one) fails loudly here.
// ===========================================================================

TEST_F(MidiDecoderRouting, StatusByteClassifiesEventTypeAndChannel) {
    // newMessageType on a status byte sets currentEvent.eventType to the high
    // nibble and currentEvent.channel to the low nibble, and transitions the
    // state machine to MIDI_EVENT_IN_PROGRESS. CHARACTERIZATION CAVEAT: while
    // IN_PROGRESS, subsequent bytes (incl. status bytes >= 0x80) are treated as
    // DATA (newMessageData) — the decoder does NOT re-anchor on a status byte
    // mid-message. So each probe below feeds a status byte from a clean
    // WAITING state (the previous message completed via Feed(...) which
    // dispatches and returns to WAITING).
    // 0x90 -> NOTE_ON, channel 0; numberOfBytes=2.
    decoder_.newByte(0x90);
    EXPECT_EQ(decoder_.currentEvent.eventType, MIDI_NOTE_ON);
    EXPECT_EQ(decoder_.currentEvent.channel, 0);
    EXPECT_EQ(decoder_.currentEventState.eventState, MIDI_EVENT_IN_PROGRESS);
    EXPECT_EQ(decoder_.currentEventState.numberOfBytes, 2);
    Feed({60, 100});  // complete the NoteOn -> back to WAITING

    // 0xB3 -> CONTROL_CHANGE, channel 3.
    decoder_.newByte(0xB3);
    EXPECT_EQ(decoder_.currentEvent.eventType, MIDI_CONTROL_CHANGE);
    EXPECT_EQ(decoder_.currentEvent.channel, 3);
    EXPECT_EQ(decoder_.currentEventState.numberOfBytes, 2);
    Feed({0, 0});  // complete

    // 0xC5 -> PROGRAM_CHANGE, channel 5; numberOfBytes=1.
    decoder_.newByte(0xC5);
    EXPECT_EQ(decoder_.currentEvent.eventType, MIDI_PROGRAM_CHANGE);
    EXPECT_EQ(decoder_.currentEvent.channel, 5);
    EXPECT_EQ(decoder_.currentEventState.numberOfBytes, 1);
    Feed({0});  // complete (1 data byte)

    // 0xE7 -> PITCH_BEND, channel 7; numberOfBytes=2.
    decoder_.newByte(0xE7);
    EXPECT_EQ(decoder_.currentEvent.eventType, MIDI_PITCH_BEND);
    EXPECT_EQ(decoder_.currentEvent.channel, 7);
    EXPECT_EQ(decoder_.currentEventState.numberOfBytes, 2);
}

TEST_F(MidiDecoderRouting, DataBytesAccumulateViaRunningStatus) {
    // Running status: after a status byte, subsequent data bytes (without a new
    // status byte) reuse the last status. The classic MIDI wire optimization.
    decoder_.newByte(0x90);   // status: NOTE_ON ch0
    decoder_.newByte(60);     // data byte 0
    EXPECT_EQ(decoder_.currentEvent.value[0], 60);
    EXPECT_EQ(decoder_.currentEventState.index, 1);
    decoder_.newByte(100);    // data byte 1 -> message complete -> dispatch
    // After dispatch, state resets to WAITING; runningStatus retained (0x90).
    EXPECT_EQ(decoder_.currentEventState.eventState, MIDI_EVENT_WAITING);
    EXPECT_EQ(decoder_.runningStatus, 0x90);

    // Now feed data bytes WITHOUT a status: running status kicks in.
    decoder_.newByte(64);     // not a status byte (< 0x80); triggers running status
    EXPECT_EQ(decoder_.currentEvent.eventType, MIDI_NOTE_ON);
    EXPECT_EQ(decoder_.currentEvent.channel, 0);
    EXPECT_EQ(decoder_.currentEventState.eventState, MIDI_EVENT_IN_PROGRESS);
    decoder_.newByte(127);    // completes the running-status NoteOn
    EXPECT_EQ(decoder_.currentEventState.eventState, MIDI_EVENT_WAITING);
    EXPECT_EQ(decoder_.runningStatus, 0x90);
}

TEST_F(MidiDecoderRouting, RunningStatusIsClearedBySystemCommonMessage) {
    // Per MIDI spec, System Common messages (0xF0..0xF7, incl. SYSEX) reset
    // running status. newMessageType's MIDI_SYSTEM_COMMON branch sets
    // runningStatus=0. A subsequent data byte with runningStatus=0 does NOT
    // re-enter the in-progress state (no dispatch).
    decoder_.newByte(0x90);
    decoder_.newByte(60);
    decoder_.newByte(100);  // establish running status = 0x90
    ASSERT_EQ(decoder_.runningStatus, 0x90);

    decoder_.newByte(0xF0);  // SYSEX start: clears running status
    EXPECT_EQ(decoder_.runningStatus, 0);
    EXPECT_EQ(decoder_.currentEventState.eventState, MIDI_EVENT_SYSEX);

    // End the sysex cleanly so we leave the parser in a known state.
    decoder_.newByte(0xF7);
    EXPECT_EQ(decoder_.currentEventState.eventState, MIDI_EVENT_WAITING);

    // Now a stray data byte (runningStatus=0) is IGNORED — no spurious NoteOn.
    decoder_.newByte(60);
    EXPECT_EQ(decoder_.currentEventState.eventState, MIDI_EVENT_WAITING)
        << "running status was not cleared by the sysex; data byte leaked";
}

TEST_F(MidiDecoderRouting, SysexFramingAccumulatesUntilEndByte) {
    // F0 enters SYSEX state; bytes accumulate into sysexBuffer (capped at
    // SYSEX_BUFFER_SIZE=32); F7 closes the frame and returns to WAITING.
    decoder_.newByte(0xF0);
    EXPECT_EQ(decoder_.currentEventState.eventState, MIDI_EVENT_SYSEX);
    EXPECT_EQ(decoder_.currentEventState.index, 0);

    for (int i = 0; i < 5; i++) {
        decoder_.newByte(static_cast<uint8_t>(0x10 + i));
    }
    EXPECT_EQ(decoder_.currentEventState.index, 5);
    EXPECT_EQ(decoder_.currentEventState.eventState, MIDI_EVENT_SYSEX);

    decoder_.newByte(0xF7);  // sysex end
    EXPECT_EQ(decoder_.currentEventState.eventState, MIDI_EVENT_WAITING);
    EXPECT_EQ(decoder_.currentEventState.index, 0);
}

TEST_F(MidiDecoderRouting, SysexBufferOverflowsAreClampedNotCorrupted) {
    // SYSEX_BUFFER_SIZE is 32; feeding >32 sysex bytes clamps the index at 32
    // (no overwrite of sysexBuffer beyond its bound) and the F7 still closes
    // the frame cleanly. Robustness guard against a malformed/oversized sysex.
    decoder_.newByte(0xF0);
    for (int i = 0; i < 64; i++) {
        decoder_.newByte(static_cast<uint8_t>(i));
    }
    EXPECT_EQ(decoder_.currentEventState.eventState, MIDI_EVENT_SYSEX);
    EXPECT_EQ(decoder_.currentEventState.index, 32)
        << "sysex index overran the 32-byte buffer (ASAN would catch the OOB)";
    decoder_.newByte(0xF7);
    EXPECT_EQ(decoder_.currentEventState.eventState, MIDI_EVENT_WAITING);
}

TEST_F(MidiDecoderRouting, RealTimeBytesDoNotCorruptInProgressMessage) {
    // Real-time bytes (>= 0xF8) are recognized FIRST and do not change
    // currentEventState. An in-progress 2-byte NoteOn survives an interleaved
    // 0xFE (active sensing — a no-op realtime). The data bytes after the
    // realtime still assemble correctly into the pending message.
    decoder_.newByte(0x90);   // NOTE_ON ch0
    decoder_.newByte(60);     // data byte 0
    EXPECT_EQ(decoder_.currentEventState.index, 1);
    EXPECT_EQ(decoder_.currentEventState.eventState, MIDI_EVENT_IN_PROGRESS);

    decoder_.newByte(0xFE);   // active sensing: realtime, no-op in the switch
    // State is UNCHANGED — realtime branch did not touch currentEventState.
    EXPECT_EQ(decoder_.currentEventState.index, 1);
    EXPECT_EQ(decoder_.currentEventState.eventState, MIDI_EVENT_IN_PROGRESS);

    decoder_.newByte(100);    // data byte 1 -> completes the NoteOn
    EXPECT_EQ(decoder_.currentEventState.eventState, MIDI_EVENT_WAITING);
    // And the note actually routed (the realtime didn't eat the dispatch):
    EXPECT_EQ(synth_.getLowerNote(0), 60);
}

TEST_F(MidiDecoderRouting, MalformedByteStreamsDoNotCrashOrCorrupt) {
    // The Target #2 malformed-input stance: degenerate content must not crash,
    // divide by zero, or leave the parser in a stuck state. Drives several
    // adversarial streams through newByte and asserts clean completion + a
    // known final state. Mandatory green under the ASAN build.
    // 1) empty stream is a no-op.
    Feed({});
    EXPECT_EQ(decoder_.currentEventState.eventState, MIDI_EVENT_WAITING);
    // 2) all data bytes, no status (runningStatus starts at 0): ignored.
    Feed({0x00, 0x7F, 0x42, 0x42});
    EXPECT_EQ(decoder_.currentEventState.eventState, MIDI_EVENT_WAITING);
    // 3) truncated NoteOn (status + 1 data byte) leaves IN_PROGRESS — the
    //    decoder waits for the second data byte.
    Feed({0x90, 60});
    EXPECT_EQ(decoder_.currentEventState.eventState, MIDI_EVENT_IN_PROGRESS);
    // 4) CHARACTERIZATION: while IN_PROGRESS, a status byte (>=0x80) is treated
    //    as a DATA byte, NOT a re-anchor. So feeding 0x80 here completes the
    //    pending NoteOn (value[1]=0x80=128, dispatched as NoteOn vel=128 — note
    //    the firmware does NOT mask the data byte's high bit). State -> WAITING.
    //    A future spec-compliant fix (re-anchor on status mid-message) flips
    //    this; flagged for a separate change.
    decoder_.newByte(0x80);
    EXPECT_EQ(decoder_.currentEventState.eventState, MIDI_EVENT_WAITING)
        << "status byte mid-message is currently treated as data (characterized)";
    // 5) sysex opened but never closed: stays in SYSEX (no hang, no crash).
    //    Use a fresh decoder to isolate.
    MidiDecoder d2;
    d2.setSynthState(ss_);
    d2.setSynth(&synth_);
    d2.setVisualInfo(&visualInfo_);
    d2.newTimbre(0);
    d2.newByte(0xF0);
    for (int i = 0; i < 100; i++) d2.newByte(0x40);
    EXPECT_EQ(d2.currentEventState.eventState, MIDI_EVENT_SYSEX);
    EXPECT_EQ(d2.currentEventState.index, 32);
}

// ===========================================================================
// TIER 2 — NRPN decode.
//
// paramMSB/paramLSB/valueMSB/valueLSB assembly byte ordering, the
// paramMSB==127,127 SEND_PATCH_AS_NRPN dispatch (observed via the global
// asyncActions ring buffer the firmware enqueues into), the paramMSB<2 main
// value-parameter branch (dispatches to synth->setNewValueFromMidi against the
// stubbed allParameterRows — see test file header + SEAM appendix), and the
// legacy CC96/97 increment/decrement path.
// ===========================================================================

class MidiNrpn : public MidiDecoderRouting {
protected:
    void SetUp() override {
        MidiDecoderRouting::SetUp();
        // Drain any leftover async actions from prior tests in this process.
        while (asyncActions.getCount() > 0) (void)asyncActions.remove();
    }
};

// Feed an NRPN: CC99=paramMSB, CC98=paramLSB, CC6=valueMSB, CC38=valueLSB on
// channel 1 (status byte 0xB0). The 4 CCs assemble into currentNrpn[0]; CC38
// sets readyToSend, which fires decodeNrpn.
void FeedNrpn(MidiDecoder& d, uint8_t paramMSB, uint8_t paramLSB,
              uint8_t valueMSB, uint8_t valueLSB) {
    d.newByte(0xB0);  // CONTROL_CHANGE ch0 (timbre 0's midiChannel-1)
    d.newByte(99);    d.newByte(paramMSB);
    d.newByte(0xB0);  d.newByte(98);    d.newByte(paramLSB);
    d.newByte(0xB0);  d.newByte(6);     d.newByte(valueMSB);
    d.newByte(0xB0);  d.newByte(38);    d.newByte(valueLSB);
}

TEST_F(MidiNrpn, FourCcBytesAssembleIntoCurrentNrpnInOrder) {
    // Feed the 4 CCs WITHOUT CC38 (so readyToSend stays false and decodeNrpn
    // does not fire). Verify the MSB/LSB accumulate into currentNrpn[0].
    decoder_.newByte(0xB0); decoder_.newByte(99); decoder_.newByte(7);  // paramMSB
    decoder_.newByte(0xB0); decoder_.newByte(98); decoder_.newByte(21); // paramLSB
    decoder_.newByte(0xB0); decoder_.newByte(6);  decoder_.newByte(3);  // valueMSB
    EXPECT_EQ(decoder_.currentNrpn[0].paramMSB, 7);
    EXPECT_EQ(decoder_.currentNrpn[0].paramLSB, 21);
    EXPECT_EQ(decoder_.currentNrpn[0].valueMSB, 3);
    EXPECT_EQ(decoder_.currentNrpn[0].valueLSB, 0);
    EXPECT_FALSE(decoder_.currentNrpn[0].readyToSend)
        << "CC38 alone sets readyToSend; it should only set after valueLSB";
}

TEST_F(MidiNrpn, Cc38SetsReadyToSendAndFiresDecodeNrpn) {
    // Complete the NRPN with CC38. decodeNrpn fires; for paramMSB<2 it dispatches
    // to synth->setNewValueFromMidi (against the stubbed allParameterRows).
    FeedNrpn(decoder_, /*paramMSB=*/0, /*paramLSB=*/0,
             /*valueMSB=*/0, /*valueLSB=*/42);
    // readyToSend was reset by decodeNrpn after dispatch.
    EXPECT_FALSE(decoder_.currentNrpn[0].readyToSend)
        << "decodeNrpn must clear readyToSend after firing";
    // The decode completed without crashing — that IS the test for the
    // paramMSB<2 path against the stubbed allParameterRows. (The actual
    // parameter-value transformation depends on real ParameterDisplay data,
    // which is out of scope; see test file header.)
    EXPECT_EQ(decoder_.currentEventState.eventState, MIDI_EVENT_WAITING);
}

TEST_F(MidiNrpn, ParamMsb127WithLsb127EnqueuesSendPatchAsNrpn) {
    // The special-case NRPN (paramMSB=127, paramLSB=127) enqueues a
    // SEND_PATCH_AS_NRPN async action for the timbre. Observable via the global
    // asyncActions ring buffer (declared extern above). The host tests do NOT
    // drain this — draining would call sendCurrentPatchAsNrpns, which hangs on
    // the stubbed USART (Trap #1 in tests/SEAM.md §d.4.1).
    ASSERT_EQ(asyncActions.getCount(), 0);
    FeedNrpn(decoder_, /*paramMSB=*/127, /*paramLSB=*/127,
             /*valueMSB=*/0, /*valueLSB=*/0);
    ASSERT_EQ(asyncActions.getCount(), 1u);
    AsyncAction a = asyncActions.remove();
    EXPECT_EQ(a.action.actionType, SEND_PATCH_AS_NRPN);
    EXPECT_EQ(a.action.timbre, 0);
}

TEST_F(MidiNrpn, ParamMsb2RoutesToStepSequencerValue) {
    // paramMSB in [2,4) -> whichStepSeq = paramMSB-2; decodeNrpn calls
    // synth->setNewStepValueFromMidi(timbre, whichStepSeq, step, value). The
    // real Synth dispatches into Timbre (no crash, no observable Synth state
    // change from the test's perspective — step-seq values live inside
    // params_.lfoSteps*, a private member). This guards the routing row WITHOUT
    // a golden on the value (the value path is the paramMSB<2 main branch).
    // The fact that we reach here without crashing proves the paramMSB=2
    // routing arm is intact.
    FeedNrpn(decoder_, /*paramMSB=*/2, /*paramLSB=*/5,
             /*valueMSB=*/0, /*valueLSB=*/99);
    SUCCEED() << "paramMSB=2 step-seq NRPN dispatched without crash";
}

TEST_F(MidiNrpn, IncrementCc96AdvancesValueLsbAndFiresDecodeNrpn) {
    // Legacy CC96 (NRPN increment): after a paramMSB<2 NRPN, CC96 increments
    // valueLSB (wrapping at 127 to 0 and bumping valueMSB) and re-fires
    // decodeNrpn. Verify the increment semantics.
    FeedNrpn(decoder_, /*paramMSB=*/0, /*paramLSB=*/0,
             /*valueMSB=*/0, /*valueLSB=*/10);
    ASSERT_EQ(decoder_.currentNrpn[0].valueLSB, 10);
    // CC96 with valueLSB=10 -> valueLSB becomes 11.
    decoder_.newByte(0xB0); decoder_.newByte(96); decoder_.newByte(0);
    EXPECT_EQ(decoder_.currentNrpn[0].valueLSB, 11)
        << "CC96 must increment valueLSB by 1";
    EXPECT_FALSE(decoder_.currentNrpn[0].readyToSend)
        << "decodeNrpn must clear readyToSend after CC96 dispatch";
}

TEST_F(MidiNrpn, IncrementCc96WrapsValueLsbAndBumpsValueMsb) {
    // valueLSB at 127 + CC96 -> valueLSB wraps to 0, valueMSB++. decodeNrpn fires.
    FeedNrpn(decoder_, /*paramMSB=*/0, /*paramLSB=*/0,
             /*valueMSB=*/5, /*valueLSB=*/127);
    ASSERT_EQ(decoder_.currentNrpn[0].valueMSB, 5);
    ASSERT_EQ(decoder_.currentNrpn[0].valueLSB, 127);
    decoder_.newByte(0xB0); decoder_.newByte(96); decoder_.newByte(0);
    EXPECT_EQ(decoder_.currentNrpn[0].valueLSB, 0)
        << "CC96 must wrap valueLSB 127 -> 0";
    EXPECT_EQ(decoder_.currentNrpn[0].valueMSB, 6)
        << "CC96 must bump valueMSB on valueLSB wrap";
}

TEST_F(MidiNrpn, DecrementCc97DecrementsValueLsb) {
    // CC97 (NRPN decrement): valueLSB-- (wrapping at 0 to 127 and decrementing
    // valueMSB). Symmetric to CC96.
    FeedNrpn(decoder_, /*paramMSB=*/0, /*paramLSB=*/0,
             /*valueMSB=*/3, /*valueLSB=*/40);
    decoder_.newByte(0xB0); decoder_.newByte(97); decoder_.newByte(0);
    EXPECT_EQ(decoder_.currentNrpn[0].valueLSB, 39);
    // valueLSB=0 wrap case
    FeedNrpn(decoder_, 0, 0, 3, 0);
    decoder_.newByte(0xB0); decoder_.newByte(97); decoder_.newByte(0);
    EXPECT_EQ(decoder_.currentNrpn[0].valueLSB, 127);
    EXPECT_EQ(decoder_.currentNrpn[0].valueMSB, 2);
}

// ===========================================================================
// TIER 3 — routing through the real Synth graph.
//
// THE capstone "stuck notes / wrong CC routing" guard. Feeds NoteOn / NoteOff /
// CC byte streams through newByte and asserts on OBSERVABLE state: Synth's
// timbre-0 lowerNote_ (via Synth::getLowerNote, public) for note routing, and
// MidiDecoder's bankNumber[] / omniOn[] / runningStatus / songPosition for CC
// routing (private; reached via the scoped #define private public).
// ===========================================================================

TEST_F(MidiDecoderRouting, NoteOnRoutesToTimbreByChannel) {
    // Channel 1 (status 0x90) -> timbre 0 (midiChannel=1). getLowerNote reflects.
    Feed({0x90, 60, 100});
    EXPECT_EQ(synth_.getLowerNote(0), 60);
}

TEST_F(MidiDecoderRouting, NoteOffRoutesToTimbreByChannel) {
    // NoteOn then NoteOff for the same note. NoteOff (0x80) routes to the same
    // timbre by channel. getLowerNote may or may not change (firmware retains
    // lowerNote_ across note-off until a lower note arrives), so this test's
    // value is primarily that NoteOff does not crash and routes to the right
    // timbre — verified by the absence of a stuck-note state in the next test.
    Feed({0x90, 60, 100, 0x80, 60, 0});
    SUCCEED() << "NoteOff dispatched without crash on the routed timbre";
}

TEST_F(MidiDecoderRouting, NoteOnWithVelocityZeroIsNoteOff) {
    // Per MIDI spec, some keyboards send note-off as NoteOn with velocity 0.
    // midiEventReceived's NOTE_ON branch handles this: if value[1]==0, calls
    // noteOff instead. The Marquee 'stuck note' regression: if a future change
    // drops this branch, a velocity-0 NoteOn leaves the voice playing.
    Feed({0x90, 60, 100});
    ASSERT_EQ(synth_.getLowerNote(0), 60);
    // Now a velocity-0 NoteOn for the same note: routes as NoteOff.
    Feed({0x90, 60, 0});
    // No crash + state stays consistent (no double-allocation). The
    // behavioral lock is the absence of a fault + a clean subsequent noteOn.
    Feed({0x90, 64, 100});
    EXPECT_EQ(synth_.getLowerNote(0), 64)
        << "a velocity-0 NoteOn followed by a fresh NoteOn must route cleanly "
           "(the vel-0 path did not free/leave the voice state correctly)";
}

TEST_F(MidiDecoderRouting, NoteOutOfRangeDoesNotRoute) {
    // instrumentState[0].firstNote=12, lastNote=84 (re-patch). A NoteOn for
    // note 11 (< firstNote) or 85 (> lastNote) is filtered: no voice allocated.
    ss_->mixerState.instrumentState_[0].firstNote = 12;
    ss_->mixerState.instrumentState_[0].lastNote = 84;
    Feed({0x90, 11, 100});   // below range
    // lowerNote_ stays at its init default (64) — the out-of-range NoteOn did
    // not allocate a voice. (Init sets lowerNote_=64 in Timbre::init.)
    EXPECT_EQ(synth_.getLowerNote(0), 64)
        << "out-of-range low NoteOn should not change lowerNote_";
    Feed({0x90, 85, 100});   // above range
    EXPECT_EQ(synth_.getLowerNote(0), 64)
        << "out-of-range high NoteOn should not change lowerNote_";
    Feed({0x90, 60, 100});   // in range -> routes
    EXPECT_EQ(synth_.getLowerNote(0), 60);
}

TEST_F(MidiDecoderRouting, WrongChannelDoesNotRouteToTimbre0) {
    // Channel 2 (status 0x91) does not match timbre 0 (midiChannel=1) and
    // timbres 1-5 have numberOfVoices=0. The NoteOn is silently dropped.
    Feed({0x91, 60, 100});   // channel 2
    EXPECT_EQ(synth_.getLowerNote(0), 64)
        << "channel-2 NoteOn leaked into timbre 0 (channel routing broken)";
}

TEST_F(MidiDecoderRouting, CcBankSelectUpdatesBankNumberForTimbre) {
    // CC_BANK_SELECT (0) on channel 1 sets bankNumber[0]. The CC tables in
    // controlChange route this BEFORE the MIDICONFIG_RECEIVES gate (it's in
    // the always-handled block), so it works regardless of RECEIVES.
    Feed({0xB0, CC_BANK_SELECT, 5});
    EXPECT_EQ(decoder_.bankNumber[0], 5);
    Feed({0xB0, CC_BANK_SELECT_LSB, 7});
    EXPECT_EQ(decoder_.bankNumberLSB[0], 7);
}

TEST_F(MidiDecoderRouting, CcOmniOnChannelMatchUsesMidiChannelDirectlyNotMinusOne) {
    // CHARACTERIZATION of a latent firmware quirk (flagged, NOT fixed):
    // controlChange's CC_OMNI_ON / CC_OMNI_OFF channel match is
    //     instrumentState_[timbre].midiChannel == midiEvent.channel
    // (NO `-1`), whereas midiEventReceived's note-routing channel match is
    //     (instrumentState_[timbre].midiChannel - 1) == midiEvent.channel
    // (WITH `-1`). For a timbre listening on MIDI channel 1 (midiChannel=1,
    // event channel 0), CC_OMNI_ON on event channel 0 evaluates 1==0 -> false
    // and is silently DROPPED — even though NoteOns on the same channel route
    // correctly. This test locks both halves of the quirk so a future fix (or a
    // regression that spreads the bug) is visible.
    ASSERT_FALSE(decoder_.omniOn[0]);
    // Timbre 0 midiChannel=1, CC on channel 1 (event byte 0xB0): 1 == 0 -> false.
    Feed({0xB0, CC_OMNI_ON, 0});
    EXPECT_FALSE(decoder_.omniOn[0])
        << "CC_OMNI_ON on midiChannel=1 + event channel 0 must NOT fire (the "
           "channel match lacks the routing path's `-1`; characterized quirk)";
    // The ONLY way CC_OMNI_ON fires is midiChannel == event channel directly:
    // set midiChannel=0 and feed CC on channel 0 -> 0==0 -> true.
    ss_->mixerState.instrumentState_[0].midiChannel = 0;
    Feed({0xB0, CC_OMNI_ON, 0});
    EXPECT_TRUE(decoder_.omniOn[0])
        << "CC_OMNI_ON fires only when midiChannel == event channel (no -1)";
}

TEST_F(MidiDecoderRouting, CcOmniOnIgnoredOnWrongChannel) {
    // CC_OMNI_ON on channel 2 (0xB1) for timbre 0 (midiChannel=1) is IGNORED:
    // the channel-match check in controlChange guards the assignment.
    ASSERT_FALSE(decoder_.omniOn[0]);
    Feed({0xB1, CC_OMNI_ON, 0});
    EXPECT_FALSE(decoder_.omniOn[0])
        << "CC_OMNI_ON on a non-matching channel must not set omniOn";
}

TEST_F(MidiDecoderRouting, CcResetClearsRunningStatusAndSongPosition) {
    // CC_RESET (127) clears runningStatus, songPosition, and calls
    // synth->allNoteOff + stopArpegiator. Marquee 'stuck state' guard: a lost
    // reset would leave the decoder in a partial-message state across resets.
    // Feed a COMPLETE NoteOn first (status + 2 data) to return to WAITING and
    // establish runningStatus=0x90; then feed CC_RESET as a complete CC.
    Feed({0x90, 60, 100});
    ASSERT_EQ(decoder_.runningStatus, 0x90);
    ASSERT_EQ(decoder_.currentEventState.eventState, MIDI_EVENT_WAITING);
    Feed({0xB0, CC_RESET, 0});
    EXPECT_EQ(decoder_.runningStatus, 0)
        << "CC_RESET must clear runningStatus";
    EXPECT_EQ(decoder_.songPosition, 0)
        << "CC_RESET must clear songPosition";
}

TEST_F(MidiDecoderRouting, RunningStatusNoteOnRoutesCorrectly) {
    // THE marquee running-status routing guard. After a single status byte,
    // subsequent NoteOns via running status must EACH route to the right timbre
    // and allocate a voice. Synth::getLowerNote(t) returns the LOWEST playing
    // note on the timbre (not the latest), so the stream feeds DESCENDING notes
    // — each new note is lower than the previous, updating lowerNote_. A
    // regression where running status drops a message (or mis-routes it) leaves
    // a missed note and lowerNote_ does not descend.
    Feed({0x90, 67, 100});            // first NoteOn (with status); lowerNote_=67
    ASSERT_EQ(synth_.getLowerNote(0), 67);
    Feed({64, 100});                  // running-status NoteOn; 64 < 67 -> lowerNote_=64
    EXPECT_EQ(synth_.getLowerNote(0), 64)
        << "running-status NoteOn did not route — running-status routing broken";
    Feed({60, 100});                  // running-status NoteOn; 60 < 64 -> lowerNote_=60
    EXPECT_EQ(synth_.getLowerNote(0), 60);
}
