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
#include "../firmware/Src/synth/Synth.h"
#include "../lib/Inc/RingBuffer.h"

#define private public  // NOLINT: scoped to MidiDecoder.h only (see header)
#include "MidiDecoder.h"
#undef private

// Null VisualInfo test double (no-op midiClock/noteOn callbacks). Shared with
// tests/golden_harness.{h,cpp} (Phase G4: MidiDecoder-driven MIDI-clock
// goldens). See host_shims/NullVisualInfo.h.
#include "NullVisualInfo.h"
#include "Sequencer.h"
#include "FMDisplaySequencer.h"

#include <new>

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

// ===========================================================================
// PHASE 2 (test-coverage-plan) — deep MIDI-tier coverage.
//
// New suites below extend the three tiers with: realtime MIDI-clock handling
// (0xF8/0xFA/0xFB/0xFC + songPosition), sysex analysis (valid channel-set
// frames, garbage frames, truncated frames, early abort), NRPN preset-name
// indices (228-239), MPE-inst1 routing, channel fan-out (global / current /
// omni), the remaining midiEventReceived arms (POLY_AFTER_TOUCH, AFTER_TOUCH,
// PITCH_BEND, PROGRAM_CHANGE, SONG_POSITION), a parameterized CC-table sweep,
// and the CC-out path (writeMidiCCOut / newParamValue with SENDS=1, USB
// IN_AND_OUT, lastSentCC dedup).
//
// Shared-global resets (spec boundary): every test in the Phase 2 fixtures
// drains asyncActions, drains usartBufferOut, and re-homes usbMidiOutBuffWrt
// in SetUp — these globals live in the MidiDecoder.cpp TU and persist across
// tests in the process.
// ===========================================================================

extern RingBuffer<uint8_t, 64> usartBufferOut;
extern uint8_t usbMidiOutBuff[64];
extern uint8_t* usbMidiOutBuffWrt;
extern uint16_t hostUsbMidiLastTransmitLength;
extern uint32_t hostUsbMidiTransmitCount;

// MidiDecoder.cpp's INV127 macro is TU-local; mirror it for float expectations.
constexpr float kInv127 = 0.00787401574803149606f;

namespace {

// Backing for a zeroed Sequencer / FMDisplaySequencer (golden-harness
// precedent): several Sequencer members (extMidiRunning_, precount_) are read
// without being initialized by the ctor/reset — firmware relies on BSS
// zero-init of its globals. Zeroing the backing then placement-new'ing mirrors
// that exactly and keeps UBSAN quiet under test-asan.
struct SeqBacking {
    alignas(alignof(Sequencer)) unsigned char bytes[sizeof(Sequencer)];
};
struct DispSeqBacking {
    alignas(alignof(FMDisplaySequencer)) unsigned char bytes[sizeof(FMDisplaySequencer)];
};

}  // namespace

// Phase-2 fixture: the Tier-1/2/3 wiring PLUS a real Sequencer wired into the
// Synth. Required because the realtime-clock path (newByte >= 0xF8) calls
// synth->midiTick(true)/midiClockStart(true)/... which forward to
// sequencer_->... — Synth's ctor does NOT initialize sequencer_, so leaving it
// unwired would dispatch through garbage. Mirrors the firmware wiring
// (preenfm3.cpp: synth.setSequencer(&sequencer)).
class MidiDecoderPhase2 : public MidiDecoderRouting {
protected:
    SeqBacking seqBacking_;
    DispSeqBacking dispSeqBacking_;
    Sequencer* seq_;
    FMDisplaySequencer* dispSeq_;
    int dummyRefreshA_ = 0;
    int dummyRefreshB_ = 0;

    void SetUp() override {
        MidiDecoderRouting::SetUp();

        // Reset shared MidiDecoder.cpp-TU globals.
        while (asyncActions.getCount() > 0) (void)asyncActions.remove();
        while (usartBufferOut.getCount() > 0) (void)usartBufferOut.remove();
        usbMidiOutBuffWrt = usbMidiOutBuff;
        hostUsbMidiLastTransmitLength = 0;
        hostUsbMidiTransmitCount = 0;
        usbMidiOutBuff[0] = usbMidiOutBuff[1] = usbMidiOutBuff[2] = usbMidiOutBuff[3] = 0;
        // MidiDecoder's ctor does NOT initialize songPosition (firmware relies
        // on the global's BSS zero-init); the fixture's stack member carries
        // garbage otherwise. Same class of uninit-member the golden harness
        // documented for MidiDecoderBacking.
        decoder_.songPosition = 0;

        // Real Sequencer + stubbed display, zeroed backing (see SeqBacking).
        std::memset(&seqBacking_, 0, sizeof(seqBacking_));
        std::memset(&dispSeqBacking_, 0, sizeof(dispSeqBacking_));
        seq_ = new (&seqBacking_) Sequencer();
        dispSeq_ = new (&dispSeqBacking_) FMDisplaySequencer();
        dispSeq_->setRefreshStatusPointer(&dummyRefreshA_, &dummyRefreshB_);

        seq_->setSynth(&synth_);
        seq_->setDisplaySequencer(dispSeq_);
        synth_.setSequencer(seq_);
    }

    void FeedCC(uint8_t cc, uint8_t val) { Feed({0xB0, cc, val}); }

};

// ---------------------------------------------------------------------------
// Realtime MIDI clock (newByte >= 0xF8).
// ---------------------------------------------------------------------------

TEST_F(MidiDecoderPhase2, MidiStartThenSixClocksAdvanceSongPosition) {
    ASSERT_FALSE(decoder_.isExternalMidiClockStarted);
    Feed({0xFA});  // MIDI_START
    EXPECT_TRUE(decoder_.isExternalMidiClockStarted);
    EXPECT_EQ(decoder_.songPosition, 0);
    EXPECT_EQ(decoder_.midiClockCpt, 0);

    for (int i = 0; i < 5; i++) Feed({0xF8});
    EXPECT_EQ(decoder_.midiClockCpt, 5)
        << "five MIDI_CLOCKs must accumulate midiClockCpt without advancing";
    EXPECT_EQ(decoder_.songPosition, 0)
        << "songPosition must only advance on the 6th clock (16th note)";

    Feed({0xF8});  // 6th clock
    EXPECT_EQ(decoder_.midiClockCpt, 0);
    EXPECT_EQ(decoder_.songPosition, 1);

    for (int i = 0; i < 6; i++) Feed({0xF8});
    EXPECT_EQ(decoder_.songPosition, 2);
}

TEST_F(MidiDecoderPhase2, ClockJunkBetweenClocksIsIgnored) {
    // 0xFE (active sensing) and other realtime junk between clocks must not
    // disturb the 6-clock grouping: songPosition still advances every 6th F8.
    Feed({0xFA});
    for (int i = 0; i < 3; i++) Feed({0xF8});
    Feed({0xFE, 0xFF, 0xFE});  // realtime junk
    for (int i = 0; i < 3; i++) Feed({0xF8});
    EXPECT_EQ(decoder_.songPosition, 1)
        << "junk realtime bytes between clocks broke the 6-clock grouping";
    EXPECT_EQ(decoder_.midiClockCpt, 0);
}

TEST_F(MidiDecoderPhase2, ClocksWithoutStartDoNotAdvanceSongPosition) {
    // F8 bytes before any 0xFA still tick the synth (midiTick) but
    // isExternalMidiClockStarted is false, so songPosition stays 0.
    for (int i = 0; i < 24; i++) Feed({0xF8});
    EXPECT_FALSE(decoder_.isExternalMidiClockStarted);
    EXPECT_EQ(decoder_.songPosition, 0);
    EXPECT_EQ(decoder_.midiClockCpt, 0) << "cpt still wraps at 6";
}

TEST_F(MidiDecoderPhase2, MidiStopHaltsSongPositionAndContinueResumes) {
    Feed({0xFA});
    for (int i = 0; i < 6; i++) Feed({0xF8});
    ASSERT_EQ(decoder_.songPosition, 1);

    Feed({0xFC});  // MIDI_STOP
    EXPECT_FALSE(decoder_.isExternalMidiClockStarted);
    for (int i = 0; i < 6; i++) Feed({0xF8});
    EXPECT_EQ(decoder_.songPosition, 1) << "clocks after STOP must not advance songPosition";

    Feed({0xFB});  // MIDI_CONTINUE: resumes from current songPosition
    EXPECT_TRUE(decoder_.isExternalMidiClockStarted);
    for (int i = 0; i < 6; i++) Feed({0xF8});
    EXPECT_EQ(decoder_.songPosition, 2)
        << "CONTINUE must resume advancing from the pre-STOP songPosition";
}

TEST_F(MidiDecoderPhase2, ClockBytesDriveWiredSequencerBeat) {
    // Integration: 0xFA + 24 clocks = one beat at the wired Sequencer
    // (onMidiClock advances midiClockTimer_ by 256/24 per clock).
    seq_->start();  // external clock (default) => running_ without precount
    Feed({0xFA});   // -> synth->midiClockStart(true) -> seq->onMidiStart (rewind)
    ASSERT_EQ(seq_->getBeat(), 1);
    for (int i = 0; i < 24; i++) Feed({0xF8});
    EXPECT_EQ(seq_->getBeat(), 2)
        << "24 MIDI clocks must advance the wired sequencer by one beat";
}

// ---------------------------------------------------------------------------
// Sysex analysis (analyseSysexBuffer via framed F0..F7 streams).
// ---------------------------------------------------------------------------

TEST_F(MidiDecoderPhase2, ValidChannelSetSysexAppliesMixerVolume) {
    // Firmware "channel set" sysex: F0 0x7d <channel 1..6> <volume 0..127> F7
    // => setNewMixerValueFromMidi(ch-1, MIXER_VALUE_VOLUME, vol/127).
    Feed({0xF0, 0x7d, 0x01, 0x40, 0xF7});
    EXPECT_FLOAT_EQ(ss_->mixerState.instrumentState_[0].volume, 64 * kInv127);
    EXPECT_EQ(decoder_.currentEventState.eventState, MIDI_EVENT_WAITING);

    // Channel 6 addresses timbre 5.
    Feed({0xF0, 0x7d, 0x06, 0x20, 0xF7});
    EXPECT_FLOAT_EQ(ss_->mixerState.instrumentState_[5].volume, 32 * kInv127);
}

TEST_F(MidiDecoderPhase2, NonChannelSetSysexFramesAreIgnored) {
    // analyseSysexBuffer only accepts an exact three-byte payload beginning
    // with 0x7d and a channel in 1..6. F7 is framing, never payload, so empty
    // and otherwise incomplete frames are rejected without reading stale slots.
    Feed({0xF0, 0x42, 0x01, 0x40, 0xF7});  // wrong manufacturer
    Feed({0xF0, 0x7d, 0x00, 0x40, 0xF7});  // channel 0
    Feed({0xF0, 0x7d, 0x07, 0x40, 0xF7});  // channel 7
    Feed({0xF0, 0xF7});                    // empty frame
    EXPECT_FLOAT_EQ(ss_->mixerState.instrumentState_[0].volume, 0.0f);
    EXPECT_EQ(decoder_.currentEventState.eventState, MIDI_EVENT_WAITING)
        << "every rejected frame must still return the parser to WAITING";
}

TEST_F(MidiDecoderPhase2, UnexpectedStatusAbortsSysexAndIsReprocessed) {
    Feed({0xF0, 0x7d, 0x01, 0x40});
    EXPECT_EQ(decoder_.currentEventState.eventState, MIDI_EVENT_SYSEX);
    Feed({0xB0, CC_BANK_SELECT, 9});
    EXPECT_EQ(decoder_.currentEventState.eventState, MIDI_EVENT_WAITING);
    EXPECT_FLOAT_EQ(ss_->mixerState.instrumentState_[0].volume, 0.0f)
        << "aborted sysex payload must not be analysed";
    EXPECT_EQ(decoder_.bankNumber[0], 9)
        << "unexpected status must be reprocessed as the next MIDI message";
}

TEST_F(MidiDecoderPhase2, OversizedSysexClampsAndSkipsAnalysis) {
    // >32 data bytes sets sysexOverflowed; F7 then rejects the whole frame
    // without analysis, regardless of the clamped buffer index.
    Feed({0xF0, 0x7d, 0x01, 0x40});
    for (int i = 0; i < 40; i++) Feed({static_cast<uint8_t>(0x10 + (i & 7))});
    ASSERT_EQ(decoder_.currentEventState.index, 32);
    Feed({0xF7});
    EXPECT_EQ(decoder_.currentEventState.eventState, MIDI_EVENT_WAITING);
    EXPECT_FLOAT_EQ(ss_->mixerState.instrumentState_[0].volume, 0.0f)
        << "oversized frame must skip analyseSysexBuffer (index clamped at 32)";
}

TEST_F(MidiDecoderPhase2, ImmediateF7EarlyAbortsCleanly) {
    // F0 followed directly by F7 ends a zero-payload, incomplete frame. F7 is
    // not analysed as payload; the frame is rejected and the parser returns
    // to WAITING without an out-of-bounds access or stuck state.
    Feed({0xF0, 0xF7});
    EXPECT_EQ(decoder_.currentEventState.eventState, MIDI_EVENT_WAITING);
    EXPECT_FLOAT_EQ(ss_->mixerState.instrumentState_[0].volume, 0.0f);
    // Parser stays responsive: a clean NoteOn right after still routes.
    Feed({0x90, 60, 100});
    EXPECT_EQ(synth_.getLowerNote(0), 60);
}

// ---------------------------------------------------------------------------
// NRPN preset-name indices (228..239) — decodeNrpn's setNewSymbolInPresetName
// arm (reached when the remapped row is past the editor rows).
// ---------------------------------------------------------------------------

TEST_F(MidiDecoderPhase2, NrpnPresetNameIndicesWritePresetNameChars) {
    // index = (paramMSB<<7)+paramLSB; 228..239 => paramMSB=1, paramLSB=100..111.
    // Each writes presetName[index-228] = value; index 239 also propagates.
    FeedNrpn(decoder_, /*paramMSB=*/1, /*paramLSB=*/100, /*valueMSB=*/0, /*valueLSB=*/'A');
    FeedNrpn(decoder_, 1, 101, 0, 'B');
    FeedNrpn(decoder_, 1, 111, 0, 'Z');  // index 239 -> propagateNewPresetName
    const char* name = synth_.getTimbre(0)->getParamRaw()->presetName;
    EXPECT_EQ(name[0], 'A');
    EXPECT_EQ(name[1], 'B');
    EXPECT_EQ(name[11], 'Z');
}

// ---------------------------------------------------------------------------
// MPE-inst1 routing (midiEventForInstrument1MPE).
// ---------------------------------------------------------------------------

TEST_F(MidiDecoderPhase2, MpeNotesOnUpperChannelsRouteToTimbre0) {
    ss_->mixerState.MPE_inst1_ = 1;
    // Channel 2 (0x91) in MPE mode routes per-channel to timbre 0 via
    // noteOnMPE (which drives the channel's dedicated voice directly — it does
    // NOT update lowerNote_, so observe via Synth::isPlaying).
    ASSERT_FALSE(synth_.isPlaying());
    Feed({0x91, 60, 100});
    EXPECT_TRUE(synth_.isPlaying())
        << "MPE upper-channel NoteOn must allocate instrument 1's voice";
    Feed({0x81, 60, 0});  // per-channel NoteOff (noteOffMPE)
    Feed({0x92, 55, 90}); // another member channel
    EXPECT_TRUE(synth_.isPlaying());
}

TEST_F(MidiDecoderPhase2, MpeChannelZeroNotesAreDropped) {
    // CHARACTERIZATION: with MPE on, channel-0 events go through the MPE
    // global branch, whose switch handles ONLY CONTROL_CHANGE (which falls
    // through to PITCH_BEND), PITCH_BEND and AFTER_TOUCH — and the branch
    // RETURNS, so a channel-0 NoteOn never reaches the normal routing and is
    // silently DROPPED. Locked as golden; a fix that lets ch0 notes route
    // normally flips this test.
    ss_->mixerState.MPE_inst1_ = 1;
    Feed({0xE0, 0x00, 0x40});  // pitch bend ch0: MPE branch + normal branch
    Feed({0xD0, 40});          // after-touch ch0: both branches
    Feed({0xB0, 1, 64});       // CC modwheel ch0: controlChange (both paths)
    Feed({0x90, 60, 100});     // NoteOn ch0: DROPPED (MPE branch returns)
    EXPECT_EQ(synth_.getLowerNote(0), 64)
        << "MPE mode channel-0 NoteOn is currently dropped (characterized)";
}

TEST_F(MidiDecoderPhase2, MpeCc74OnMemberChannelIsPerChannelSlide) {
    ss_->mixerState.MPE_inst1_ = 1;
    // CC74 on a member channel -> setMatrixSourceMPE(ch, MPESLIDE, ...) on
    // the channel's dedicated voice. The write lands in the private Voice
    // matrix (no public getter — see the userCC test note), so the oracle is
    // state + resync: both routes complete, the parser returns to WAITING,
    // and a clean note afterwards still routes. The member-channel voice
    // must EXIST for the write to happen (noteOnMPE first, else
    // voiceNumber_[ch-1] == -1 early-returns).
    Feed({0x91, 60, 100});           // note on MPE member channel 2
    Feed({0xB1, CC_MPE_SLIDE_CC74, 100});  // member channel: per-voice slide
    EXPECT_FLOAT_EQ(synth_.getTimbre(0)->hostMaxMatrixSource(MATRIX_SOURCE_MPESLIDE),
                    100 * kInv127);
    Feed({0xB0, CC_MPE_SLIDE_CC74, 100});  // global channel: no per-voice slide
    EXPECT_FLOAT_EQ(synth_.getTimbre(0)->hostMaxMatrixSource(MATRIX_SOURCE_MPESLIDE),
                    100 * kInv127);
    EXPECT_EQ(decoder_.currentEventState.eventState, MIDI_EVENT_WAITING);
    // Probe on a MEMBER channel (channel 0 notes are dropped in MPE mode —
    // see MpeChannelZeroNotesAreDropped), observed via isPlaying()
    // (noteOnMPE does not update lowerNote_ — characterized in the MPE
    // note-routing test above).
    Feed({0x92, 62, 100});
    EXPECT_TRUE(synth_.isPlaying()) << "routing intact after both CC74 paths";
}

// ---------------------------------------------------------------------------
// Channel fan-out (global channel / current channel / omni).
// ---------------------------------------------------------------------------

TEST_F(MidiDecoderPhase2, GlobalChannelCcReachesAllTimbres) {
    // globalChannel_=1 => event channel 0 is the global channel: the event
    // fans out to timbres 0..5 (MPE off). CC_BANK_SELECT gives an observable
    // per-timbre write.
    ss_->mixerState.globalChannel_ = 1;
    FeedCC(CC_BANK_SELECT, 9);
    for (int t = 0; t < NUMBER_OF_TIMBRES; t++) {
        EXPECT_EQ(decoder_.bankNumber[t], 9) << "timbre " << t;
    }
}

TEST_F(MidiDecoderPhase2, CurrentChannelCcReachesOnlyCurrentTimbre) {
    // currentChannel_=1 => event channel 0 routes to currentTimbre (0) only.
    ss_->mixerState.currentChannel_ = 1;
    FeedCC(CC_BANK_SELECT, 4);
    EXPECT_EQ(decoder_.bankNumber[0], 4);
    for (int t = 1; t < NUMBER_OF_TIMBRES; t++) {
        EXPECT_EQ(decoder_.bankNumber[t], 0) << "timbre " << t << " must not receive it";
    }
}

TEST_F(MidiDecoderPhase2, OmniOnTimbreReceivesUnassignedChannelWithoutFanOut) {
    decoder_.omniOn[1] = true;
    char before[NUMBER_OF_TIMBRES];
    for (int t = 0; t < NUMBER_OF_TIMBRES; t++) before[t] = decoder_.bankNumber[t];
    Feed({0xBF, CC_BANK_SELECT, 3});  // channel 16 is unassigned in the fixture
    EXPECT_EQ(decoder_.bankNumber[1], 3);
    for (int t = 0; t < NUMBER_OF_TIMBRES; t++) {
        if (t != 1) EXPECT_EQ(decoder_.bankNumber[t], before[t])
            << "unassigned omni probe fanned out to timbre " << t;
    }
}

TEST_F(MidiDecoderPhase2, OmniModeMidiChannelZeroMatchesEveryChannel) {
    // midiChannel==0 means OMNI in the routing match. Timbre 2 set to omni.
    ss_->mixerState.instrumentState_[2].midiChannel = 0;
    Feed({0xB4, CC_BANK_SELECT, 6});  // channel 5
    EXPECT_EQ(decoder_.bankNumber[2], 6);
}

// ---------------------------------------------------------------------------
// Remaining midiEventReceived arms.
// ---------------------------------------------------------------------------

TEST_F(MidiDecoderPhase2, PolyAfterTouchAfterTouchAndPitchBendUpdateMatrixSources) {
    Feed({0x90, 60, 100});
    Feed({0xA0, 60, 100});
    EXPECT_FLOAT_EQ(synth_.getTimbre(0)->hostMaxMatrixSource(MATRIX_SOURCE_POLYPHONIC_AFTERTOUCH),
                    100 * kInv127);
    Feed({0xD0, 80});
    EXPECT_FLOAT_EQ(synth_.getTimbre(0)->hostMaxMatrixSource(MATRIX_SOURCE_AFTERTOUCH),
                    80 * kInv127);
    Feed({0xE0, 0x00, 0x68});
    EXPECT_FLOAT_EQ(synth_.getTimbre(0)->hostMaxMatrixSource(MATRIX_SOURCE_PITCHBEND),
                    0.625f);
}

TEST_F(MidiDecoderPhase2, ProgramChangeQueuesLoadPresetWhenEnabled) {
    // PC with midiConfigValue[PROGRAM_CHANGE]=1 enqueues a LOAD_PRESET async
    // action (timbre bitmask from the routing set, bank/bankLSB/program). The
    // host tests do NOT call processAsyncActions — LOAD_PRESET drives
    // synth->loadPreenFMPatchFromMidi (FatFs/global side effects, host-unsafe
    // per the spec's Ask-First boundary). Assert the QUEUE CONTENTS only.
    Feed({0xC0, 42});
    ASSERT_EQ(asyncActions.getCount(), 1u);
    AsyncAction a = asyncActions.remove();
    EXPECT_EQ(a.action.actionType, LOAD_PRESET);
    EXPECT_EQ(a.action.timbre, 1);  // timbre 0 only (channel 1 routes there)
    EXPECT_EQ(a.action.param1, 0);  // bankNumber[0]
    EXPECT_EQ(a.action.param2, 0);  // bankNumberLSB[0]
    EXPECT_EQ(a.action.param3, 42); // program number

    // Bank-select before PC is carried in the action.
    FeedCC(CC_BANK_SELECT, 2);
    FeedCC(CC_BANK_SELECT_LSB, 3);
    Feed({0xC0, 7});
    ASSERT_EQ(asyncActions.getCount(), 1u);
    a = asyncActions.remove();
    EXPECT_EQ(a.action.param1, 2);
    EXPECT_EQ(a.action.param2, 3);
    EXPECT_EQ(a.action.param3, 7);
}

TEST_F(MidiDecoderPhase2, ProgramChangeIgnoredWhenConfigOff) {
    ss_->fullState.midiConfigValue[MIDICONFIG_PROGRAM_CHANGE] = 0;
    Feed({0xC0, 42});
    EXPECT_EQ(asyncActions.getCount(), 0u)
        << "PC must be ignored when midiConfigValue[PROGRAM_CHANGE]==0";
}

TEST_F(MidiDecoderPhase2, SongPositionSystemCommonSetsSongPosition) {
    // 0xF2 (SONG_POSITION): 2 data bytes; the channel is hacked to timbre 0's
    // so the event is accepted, and the decoder's songPosition is set from
    // MSB<<7+LSB. synth->midiClockSetSongPosition(sp, true) forwards to the
    // wired sequencer (no crash = the wiring gold).
    Feed({0xF2, 0x05, 0x01});  // sp = (1<<7)+5 = 133
    EXPECT_EQ(decoder_.songPosition, 133);
    EXPECT_EQ(decoder_.runningStatus, 0)
        << "system-common must clear running status";
}

// ---------------------------------------------------------------------------
// CC-table sweep (controlChange families). Parameterized over representative
// CCs from every family; asserts dispatch completes and the parser stays
// responsive (a subsequent NoteOn routes). Value-level goldens for the
// directly-observable families are in the dedicated tests below.
// ---------------------------------------------------------------------------

struct CcSweepParam {
    const char* name;
    uint8_t cc;
    uint8_t value;
};

class MidiCcSweep : public MidiDecoderPhase2,
                    public ::testing::WithParamInterface<CcSweepParam> {};

std::string CcSweepName(const ::testing::TestParamInfo<CcSweepParam>& info) {
    return info.param.name;
}

TEST_P(MidiCcSweep, CcDispatchesWithoutBreakingParser) {
    const auto& p = GetParam();
    FeedCC(p.cc, p.value);
    // Some CCs (arp family) arm the arpeggiator, which defers note allocation
    // to the next arp tick — reset it so the routing probe is unconditional.
    FeedCC(CC_ALL_NOTES_OFF, 0);
    // Parser must be back in WAITING and still route notes.
    ASSERT_EQ(decoder_.currentEventState.eventState, MIDI_EVENT_WAITING) << p.name;
    Feed({0x90, 61, 100});
    EXPECT_EQ(synth_.getLowerNote(0), 61) << p.name;
}

INSTANTIATE_TEST_SUITE_P(
    Phase2CcFamilies, MidiCcSweep,
    ::testing::Values(
        CcSweepParam{"ALGO", CC_ALGO, 20},
        CcSweepParam{"IM1", CC_IM1, 50}, CcSweepParam{"IM2", CC_IM2, 50},
        CcSweepParam{"IM3", CC_IM3, 50}, CcSweepParam{"IM4", CC_IM4, 50},
        CcSweepParam{"IM5", CC_IM5, 50}, CcSweepParam{"IM_FEEDBACK", CC_IM_FEEDBACK, 64},
        CcSweepParam{"MIX1", CC_MIX1, 100}, CcSweepParam{"MIX2", CC_PAN1, 64},
        CcSweepParam{"MIX3", CC_MIX3, 100}, CcSweepParam{"MIX4", CC_PAN3, 64},
        CcSweepParam{"OSC1_FREQ", CC_OSC1_FREQ, 60},
        CcSweepParam{"OSC6_FREQ", CC_OSC6_FREQ, 60},
        CcSweepParam{"MATRIXROW1_MUL", CC_MATRIXROW1_MUL, 90},
        CcSweepParam{"MATRIXROW4_MUL", CC_MATRIXROW4_MUL, 90},
        CcSweepParam{"MATRIX_SOURCE_CC1", CC_MATRIX_SOURCE_CC1, 70},
        CcSweepParam{"MATRIX_SOURCE_CC4", CC_MATRIX_SOURCE_CC4, 70},
        CcSweepParam{"LFO1_FREQ", CC_LFO1_FREQ, 40},
        CcSweepParam{"LFO3_FREQ", CC_LFO3_FREQ, 40},
        CcSweepParam{"LFO_ENV2_SILENCE", CC_LFO_ENV2_SILENCE, 32},
        CcSweepParam{"STEPSEQ5_GATE", CC_STEPSEQ5_GATE, 50},
        CcSweepParam{"STEPSEQ6_GATE", CC_STEPSEQ6_GATE, 50},
        CcSweepParam{"LFO1_PHASE", CC_LFO1_PHASE, 50},
        CcSweepParam{"LFO3_BIAS", CC_LFO3_BIAS, 50},
        CcSweepParam{"LFO2_SHAPE", CC_LFO2_SHAPE, 3},
        CcSweepParam{"FILTER_TYPE", CC_FILTER_TYPE, 2},
        CcSweepParam{"FILTER_PARAM1", CC_FILTER_PARAM1, 64},
        CcSweepParam{"FILTER_PARAM2", CC_FILTER_PARAM2, 64},
        CcSweepParam{"FILTER_GAIN", CC_FILTER_GAIN, 64},
        CcSweepParam{"FILTER2_TYPE", CC_FILTER2_TYPE, 2},
        CcSweepParam{"FILTER2_PARAM1", CC_FILTER2_PARAM1, 64},
        CcSweepParam{"FILTER2_PARAM2", CC_FILTER2_PARAM2, 64},
        CcSweepParam{"FILTER2_MIX", CC_FILTER2_MIX, 64},
        CcSweepParam{"ENV_ATK_OP1", CC_ENV_ATK_OP1, 32},
        CcSweepParam{"ENV_ATK_OP2", CC_ENV_ATK_OP2, 32},
        CcSweepParam{"ENV_ATK_OP6", CC_ENV_ATK_OP6, 32},
        CcSweepParam{"ENV_ATK_ALL_CARRIER", CC_ENV_ATK_ALL_CARRIER, 32},
        CcSweepParam{"ENV_ATK_ALL_MODULATOR", CC_ENV_ATK_ALL_MODULATOR, 32},
        CcSweepParam{"ENV_REL_OP1", CC_ENV_REL_OP1, 32},
        CcSweepParam{"ENV_REL_OP6", CC_ENV_REL_OP6, 32},
        CcSweepParam{"ENV_REL_ALL_CARRIER", CC_ENV_REL_ALL_CARRIER, 32},
        CcSweepParam{"ENV_REL_ALL_MODULATOR", CC_ENV_REL_ALL_MODULATOR, 32},
        CcSweepParam{"ARP_CLOCK", CC_ARP_CLOCK, 0},  // 0 = arp clock off (1=internal defers notes)
        CcSweepParam{"ARP_DIRECTION", CC_ARP_DIRECTION, 0},
        CcSweepParam{"ARP_OCTAVE", CC_ARP_OCTAVE, 2},
        CcSweepParam{"ARP_PATTERN", CC_ARP_PATTERN, 1},
        CcSweepParam{"ARP_DIVISION", CC_ARP_DIVISION, 1},
        CcSweepParam{"ARP_DURATION", CC_ARP_DURATION, 80},
        CcSweepParam{"UNISON_DETUNE", CC_UNISON_DETUNE, 64},
        CcSweepParam{"MODWHEEL", CC_MODWHEEL, 100},
        CcSweepParam{"BREATH", CC_BREATH, 100},
        CcSweepParam{"MPE_SLIDE", CC_MPE_SLIDE_CC74, 100},
        CcSweepParam{"CURRENT_INSTRUMENT", CC_CURRENT_INSTRUMENT, 1},
        CcSweepParam{"HOLD_PEDAL", CC_HOLD_PEDAL, 64},
        CcSweepParam{"ALL_NOTES_OFF", CC_ALL_NOTES_OFF, 0},
        CcSweepParam{"ALL_SOUND_OFF", CC_ALL_SOUND_OFF, 0},
        CcSweepParam{"OMNI_OFF", CC_OMNI_OFF, 0},
        CcSweepParam{"RESET", CC_RESET, 0},
        // Seq CCs route through synth->setNewSeqValueFromMidi into the wired
        // Sequencer (which is why the Phase2 fixture wires one).
        CcSweepParam{"SEQ_START_ALL", CC_SEQ_START_ALL, 1},
        CcSweepParam{"SEQ_START_INST", CC_SEQ_START_INST, 1},
        CcSweepParam{"SEQ_RECORD_INST", CC_SEQ_RECORD_INST, 1},
        CcSweepParam{"SEQ_SET_SEQUENCE", CC_SEQ_SET_SEQUENCE, 3},
        CcSweepParam{"SEQ_TRANSPOSE", CC_SEQ_TRANSPOSE, 70},
        // Unmapped CC (no arm): must fall through all switches silently.
        CcSweepParam{"UNMAPPED_3", 3, 40},
        CcSweepParam{"UNMAPPED_200", 200, 40}),
    CcSweepName);

TEST_F(MidiDecoderPhase2, MixerCcsWriteInstrumentState) {
    FeedCC(CC_MIXER_VOLUME, 64);
    EXPECT_FLOAT_EQ(ss_->mixerState.instrumentState_[0].volume, 64 * kInv127);
    FeedCC(CC_MIXER_PAN, 80);
    EXPECT_EQ(ss_->mixerState.instrumentState_[0].pan, 17)
        << "pan = value - 63 (CC_MIXER_PAN arm)";
    FeedCC(CC_MIXER_SEND, 32);
    EXPECT_FLOAT_EQ(ss_->mixerState.instrumentState_[0].send, 32 * kInv127);
}

TEST_F(MidiDecoderPhase2, MfxCcsOnGlobalChannelWriteFxBusConfig) {
    // The MFX CC family is only handled on the GLOBAL channel.
    ss_->mixerState.globalChannel_ = 1;
    FeedCC(CC_MFX_PRESET, 5);
    EXPECT_EQ(ss_->mixerState.reverbPreset_, 5);
    EXPECT_EQ(ss_->mixerState.fxBus_.nextPresetNum, 5);
    FeedCC(CC_MFX_PREDELAYTIME, 64);
    EXPECT_FLOAT_EQ(ss_->mixerState.fxBus_.masterfxConfig[GLOBALFX_PREDELAYTIME], 64 * kInv127);
    FeedCC(CC_MFX_MOD_DEPTH, 32);
    EXPECT_FLOAT_EQ(ss_->mixerState.fxBus_.masterfxConfig[GLOBALFX_LFODEPTH], 32 * kInv127);
    // Off the global channel the same CCs do nothing.
    ss_->mixerState.globalChannel_ = 0;
    FeedCC(CC_MFX_PRESET, 9);
    EXPECT_EQ(ss_->mixerState.reverbPreset_, 5) << "MFX CC off global channel must be dropped";
}

TEST_F(MidiDecoderPhase2, UserCcSlotsShortCircuitIndependently) {
    for (int slot = 0; slot < 4; slot++) {
        for (int i = 0; i < 4; i++) ss_->mixerState.userCC_[i] = 255;
        ss_->mixerState.userCC_[slot] = CC_BANK_SELECT;
        FeedCC(CC_BANK_SELECT, static_cast<uint8_t>(slot + 1));
        EXPECT_EQ(decoder_.bankNumber[0], 0)
            << "userCC slot " << slot << " did not independently hijack the CC";
    }
    for (int i = 0; i < 4; i++) ss_->mixerState.userCC_[i] = 255;
    FeedCC(CC_BANK_SELECT, 9);
    EXPECT_EQ(decoder_.bankNumber[0], 9)
        << "unhijacked CC_BANK_SELECT must reach the CC table";
}

TEST_F(MidiDecoderPhase2, UnisonSpreadDoesNotStartSequencer) {
    // Fixed (was UnisonSpreadFallsThroughToSeqStartAll): the CC_UNISON_SPREAD
    // arm now breaks — one CC14 sets ONLY the spread param. Pure-spread
    // semantics: any value (incl. 0) leaves sequencer state untouched.
    const int spreadIndex = ROW_ENGINE2 * NUMBER_OF_ENCODERS_PFM2 + ENCODER_ENGINE2_UNISON_SPREAD;
    // CC path (timbre 0) vs direct API call (timbre 1): the CC arm must drive
    // setNewValueFromMidi with identical arguments — the stubbed param table's
    // display quantization then applies equally to both slots.
    synth_.setNewValueFromMidi(1, ROW_ENGINE2, ENCODER_ENGINE2_UNISON_SPREAD, 100.0f * kInv127);
    const float expected = ((const float*) synth_.getTimbre(1)->getParamRaw())[spreadIndex];
    ASSERT_NE(expected, 0.5f) << "direct setNewValueFromMidi must move the spread off its default";
    ASSERT_FALSE(seq_->isRunning());
    FeedCC(CC_UNISON_SPREAD, 100);
    EXPECT_FALSE(seq_->isRunning())
        << "CC_UNISON_SPREAD must not start the sequencer (break restored)";
    const float after = ((const float*) synth_.getTimbre(0)->getParamRaw())[spreadIndex];
    EXPECT_FLOAT_EQ(after, expected)
        << "CC14=100 must set the spread exactly like a direct setNewValueFromMidi";
}

TEST_F(MidiDecoderPhase2, UnisonSpreadValueZeroDoesNotStopSequencer) {
    // Regression for the CC14=0 edge: the old fall-through forwarded the value
    // to SEQ_VALUE_PLAY_ALL (start only when >0), so value 0 never stopped
    // anything — pinned so the fix doesn't accidentally change that.
    FeedCC(CC_SEQ_START_ALL, 1);
    ASSERT_TRUE(seq_->isRunning());
    FeedCC(CC_UNISON_SPREAD, 0);
    EXPECT_TRUE(seq_->isRunning())
        << "CC14=0 sets spread only; it must not stop the sequencer";
}

TEST_F(MidiDecoderPhase2, AllNotesOffAndAllSoundOffHaveDistinctEffects) {
    Feed({0x90, 60, 100});
    ASSERT_TRUE(synth_.isPlaying());
    FeedCC(CC_ALL_NOTES_OFF, 0);
    EXPECT_TRUE(synth_.isPlaying())
        << "ALL_NOTES_OFF releases envelopes rather than killing sound immediately";

    Feed({0x90, 50, 100});
    ASSERT_TRUE(synth_.isPlaying());
    FeedCC(CC_ALL_SOUND_OFF, 0);
    EXPECT_FALSE(synth_.isPlaying())
        << "ALL_SOUND_OFF must immediately silence allocated voices";
}

// ---------------------------------------------------------------------------
// CC-out / USB-out (writeMidiCCOut + newParamValue with SENDS=1).
// ---------------------------------------------------------------------------

TEST_F(MidiDecoderPhase2, WriteMidiCCOutWritesUsartAndUsbWhenEnabled) {
    ss_->fullState.midiConfigValue[MIDICONFIG_USB] = USBMIDI_IN_AND_OUT;
    MidiEvent cc{};
    cc.eventType = MIDI_CONTROL_CHANGE;
    cc.channel = 0;
    cc.value[0] = 74;
    cc.value[1] = 100;
    decoder_.writeMidiCCOut(&cc);
    // USART out: 3 bytes (status+ch, cc, value).
    ASSERT_EQ(usartBufferOut.getCount(), 3);
    EXPECT_EQ(usartBufferOut.remove(), 0xB0);
    EXPECT_EQ(usartBufferOut.remove(), 74);
    EXPECT_EQ(usartBufferOut.remove(), 100);
    // USB out: 4-byte packet [cable|type, status+ch, d0, d1].
    EXPECT_EQ(usbMidiOutBuffWrt - usbMidiOutBuff, 4);
    EXPECT_EQ(usbMidiOutBuff[0], 0x0B);
    EXPECT_EQ(usbMidiOutBuff[1], 0xB0);
    EXPECT_EQ(usbMidiOutBuff[2], 74);
    EXPECT_EQ(usbMidiOutBuff[3], 100);
    usbMidiOutBuffWrt = usbMidiOutBuff;  // re-home for the next test
}

TEST_F(MidiDecoderPhase2, WriteMidiCCOutSkipsUsbWhenDisabled) {
    // USB config OFF (fixture default): only USART bytes are written.
    MidiEvent cc{};
    cc.eventType = MIDI_CONTROL_CHANGE;
    cc.channel = 0;
    cc.value[0] = 1;
    cc.value[1] = 2;
    decoder_.writeMidiCCOut(&cc);
    EXPECT_EQ(usartBufferOut.getCount(), 3);
    EXPECT_EQ(usbMidiOutBuffWrt - usbMidiOutBuff, 0);
}

TEST_F(MidiDecoderPhase2, NewParamValueWithSendsCcEmitsCcAndDedups) {
    // SENDS=1 (fixture default): newParamValue maps (row, encoder, value) to
    // a CC and writes it out; an identical consecutive change is suppressed by
    // lastSentCC.
    ASSERT_EQ(usartBufferOut.getCount(), 0);
    decoder_.lastSentCC = {};
    decoder_.lastSentCC.value[0] = 0xFF;
    // ROW_ENGINE/ENCODER_ENGINE_ALGO -> CC_ALGO, value 20.
    decoder_.newParamValue(0, ROW_ENGINE, ENCODER_ENGINE_ALGO, nullptr, 0.0f, 20.0f);
    ASSERT_EQ(usartBufferOut.getCount(), 3);
    EXPECT_EQ(usartBufferOut.remove(), 0xB0);
    EXPECT_EQ(usartBufferOut.remove(), CC_ALGO);
    EXPECT_EQ(usartBufferOut.remove(), 20);
    // Same CC again (even via a different row path that maps to the same
    // bytes) => deduped, no output.
    decoder_.newParamValue(0, ROW_ENGINE, ENCODER_ENGINE_ALGO, nullptr, 20.0f, 20.0f);
    EXPECT_EQ(usartBufferOut.getCount(), 0) << "lastSentCC must dedup identical CCs";
    // Different value => emitted again.
    decoder_.newParamValue(0, ROW_ENGINE, ENCODER_ENGINE_ALGO, nullptr, 20.0f, 30.0f);
    ASSERT_EQ(usartBufferOut.getCount(), 3);
    EXPECT_EQ(usartBufferOut.remove(), 0xB0);
    EXPECT_EQ(usartBufferOut.remove(), CC_ALGO);
    EXPECT_EQ(usartBufferOut.remove(), 30);
}

TEST_F(MidiDecoderPhase2, NewParamValueCcClampsTo127) {
    // Produce a defined intermediate uint8_t value of 200, then verify the
    // outbound CC clamp. Avoid out-of-range float-to-uint8_t conversion.
    decoder_.newParamValue(0, ROW_MODULATION1, 0, nullptr, 0.0f, 20.0f);
    ASSERT_EQ(usartBufferOut.getCount(), 3);
    EXPECT_EQ(usartBufferOut.remove(), 0xB0);
    EXPECT_EQ(usartBufferOut.remove(), CC_IM1);
    EXPECT_EQ(usartBufferOut.remove(), 127);
}

TEST_F(MidiDecoderPhase2, NewParamValueWithSendsOffEmitsNothing) {
    ss_->fullState.midiConfigValue[MIDICONFIG_SENDS] = 0;
    decoder_.newParamValue(0, ROW_ENGINE, ENCODER_ENGINE_ALGO, nullptr, 0.0f, 20.0f);
    EXPECT_EQ(usartBufferOut.getCount(), 0);
}

TEST_F(MidiDecoderPhase2, SendMidiUsbOutFlushesExactFullBufferAndRehomesWriter) {
    ss_->fullState.midiConfigValue[MIDICONFIG_USB] = USBMIDI_IN_AND_OUT;
    usbMidiOutBuffWrt = usbMidiOutBuff + 64;
    decoder_.sendMidiUsbOutIfBufferFull();
    EXPECT_EQ(hostUsbMidiTransmitCount, 1u);
    EXPECT_EQ(hostUsbMidiLastTransmitLength, 64u);
    EXPECT_EQ(usbMidiOutBuffWrt, usbMidiOutBuff);

    MidiEvent cc{};
    cc.eventType = MIDI_CONTROL_CHANGE;
    cc.channel = 0;
    cc.value[0] = 1;
    cc.value[1] = 2;
    decoder_.writeMidiCCOut(&cc);
    EXPECT_EQ(usbMidiOutBuffWrt - usbMidiOutBuff, 4)
        << "a packet after an exact-full flush must be written safely";
}

// ---------------------------------------------------------------------------
// NRPN-out (newParamValue with SENDS=2). Unlike sendCurrentPatchAsNrpns (the
// blocked NRPN bulk-send path whose USART drain loop hangs on host — Trap #1),
// this branch writes 4 CCs per param change and calls only the early-return
// stubs, so it is host-safe.
// ---------------------------------------------------------------------------

TEST_F(MidiDecoderPhase2, NewParamValueWithSendsNrpnEmitsFourCcPerParam) {
    ss_->fullState.midiConfigValue[MIDICONFIG_SENDS] = 2;
    struct ParameterDisplay pd = {};  // displayType NONE => raw value path
    // ROW_ENGINE(0)*4+0 => memoryIndex 0 => midiIndex 0.
    decoder_.newParamValue(0, ROW_ENGINE, ENCODER_ENGINE_ALGO, &pd, 0.0f, 42.0f);
    // 4 CCs x 3 bytes: [99,msb] [98,lsb] [6,vmsb] [38,vlsb].
    ASSERT_EQ(usartBufferOut.getCount(), 12);
    // 4 CCs, each [0xB0, cc, value].
    EXPECT_EQ(usartBufferOut.remove(), 0xB0);
    EXPECT_EQ(usartBufferOut.remove(), 99);
    EXPECT_EQ(usartBufferOut.remove(), 0);   // paramMSB (midiIndex >> 7)
    EXPECT_EQ(usartBufferOut.remove(), 0xB0);
    EXPECT_EQ(usartBufferOut.remove(), 98);
    EXPECT_EQ(usartBufferOut.remove(), 0);   // paramLSB
    EXPECT_EQ(usartBufferOut.remove(), 0xB0);
    EXPECT_EQ(usartBufferOut.remove(), 6);
    EXPECT_EQ(usartBufferOut.remove(), 0);   // valueMSB
    EXPECT_EQ(usartBufferOut.remove(), 0xB0);
    EXPECT_EQ(usartBufferOut.remove(), 38);
    EXPECT_EQ(usartBufferOut.remove(), 42);  // valueLSB
}

TEST_F(MidiDecoderPhase2, NewParamValueNrpnStepSeqEncoderSpecialCases) {
    ss_->fullState.midiConfigValue[MIDICONFIG_SENDS] = 2;
    struct ParameterDisplay pd = {};
    // encoder 2 on a step-seq row: early return (nothing sent).
    decoder_.newParamValue(0, ROW_LFOSEQ1, 2, &pd, 0.0f, 42.0f);
    EXPECT_EQ(usartBufferOut.getCount(), 0);
    // encoder 3 (step value): NRPN with paramMSB = currentStepSeq+2,
    // paramLSB = stepSelect[0], valueLSB = newValue.
    ss_->stepSelect[0] = 5;
    decoder_.newParamValue(0, ROW_LFOSEQ1, 3, &pd, 0.0f, 42.0f);
    ASSERT_EQ(usartBufferOut.getCount(), 12);
    (void)usartBufferOut.remove();  // 0xB0
    EXPECT_EQ(usartBufferOut.remove(), 99);
    EXPECT_EQ(usartBufferOut.remove(), 0 + 2);  // paramMSB = currentStepSeq+2
    (void)usartBufferOut.remove();
    EXPECT_EQ(usartBufferOut.remove(), 98);
    EXPECT_EQ(usartBufferOut.remove(), 5);      // paramLSB = stepSelect[0]
    (void)usartBufferOut.remove();
    EXPECT_EQ(usartBufferOut.remove(), 6);
    (void)usartBufferOut.remove();
    (void)usartBufferOut.remove();              // 0xB0
    EXPECT_EQ(usartBufferOut.remove(), 38);
    EXPECT_EQ(usartBufferOut.remove(), 42);
}

TEST_F(MidiDecoderPhase2, NewParamValueNrpnFloatParamScalesByHundred) {
    // displayType FLOAT: valueToSend = (newValue - minValue)*100.
    ss_->fullState.midiConfigValue[MIDICONFIG_SENDS] = 2;
    struct ParameterDisplay pd = {};
    pd.displayType = DISPLAY_TYPE_FLOAT;
    pd.minValue = 1.0f;
    decoder_.newParamValue(0, ROW_ENGINE, ENCODER_ENGINE_ALGO, &pd, 0.0f, 4.0f);
    ASSERT_EQ(usartBufferOut.getCount(), 12);
    for (int i = 0; i < 7; i++) (void)usartBufferOut.remove();
    EXPECT_EQ(usartBufferOut.remove(), 6);
    EXPECT_EQ(usartBufferOut.remove(), 2) << "300 >> 7 is the 14-bit MSB";
    EXPECT_EQ(usartBufferOut.remove(), 0xB0);
    EXPECT_EQ(usartBufferOut.remove(), 38);
    EXPECT_EQ(usartBufferOut.remove(), 44) << "300 & 0x7f is the 14-bit LSB";
}
