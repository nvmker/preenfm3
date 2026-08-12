// Test-double bodies for Sequencer.cpp's collaborators.
//
// WHY THIS FILE EXISTS (see tests/SEAM.md "Contact with the code")
// ----------------------------------------------------------------
// Sequencer.cpp is host-compilable as a translation unit, but its compiled
// body references out-of-line Synth / Timbre / FMDisplaySequencer methods. In
// Target #1 only Sequencer.cpp was pulled, so empty bodies for all referenced
// methods lived here. Targets #4 pulls the REAL Synth graph (Synth.cpp /
// Timbre.cpp / Voice.cpp / FxBus.cpp / ...), so the Synth::stopArpegiator /
// allNoteOff / midiClock* / noteOn/noteOffFromSequencer and
// Timbre::midiClockSongPositionStep stubs became duplicates and were REMOVED.
// The FMDisplaySequencer stubs REMAIN: FMDisplaySequencer.cpp is part of the
// TFT/UI FMDisplay family (not pulled) and Sequencer.cpp's compiled body still
// references its out-of-line methods in paths the tests never exercise.
//
// Standard test doubles, NOT HAL fakes. Signature drift fails to compile
// (intended early signal). Methods already inline in Synth.h /
// FMDisplaySequencer.h (midiClockSongPositionStep, refresh, refreshMemory,
// refreshActivated, refreshStepSeq) are deliberately NOT stubbed here.

#include "FMDisplaySequencer.h"

// --- FMDisplaySequencer ------------------------------------------------------
// Phase G4: the real ctor (FMDisplaySequencer.cpp:35) is off-host
// (FMDisplaySequencer.cpp is NOT compiled — TFT/UI family). Its body is
// trivial (2 field inits), replicated here so the golden harness can
// instantiate a FMDisplaySequencer for the seq-external golden. Without it,
// Sequencer::onMidiStart -> displaySequencer_->refresh -> *refreshStatusP_
// crashes (the ctor never sets refreshStatusP_; the harness calls
// setRefreshStatusPointer to a dummy int pair after construction). Other
// members stay default-init; the playback path reads only seqMode_/stepSize_
// + calls the inline refresh methods (which write the dummy refresh ints).
FMDisplaySequencer::FMDisplaySequencer() {
    seqMode_ = SEQ_MODE_NORMAL;
    stepSize_ = 16;
}
void FMDisplaySequencer::displayBeat() {}
void FMDisplaySequencer::newNoteEntered(int) {}
void FMDisplaySequencer::updateCurrentData() {}
void FMDisplaySequencer::sequencerWasUpdated(uint8_t, uint8_t, uint8_t) {}
