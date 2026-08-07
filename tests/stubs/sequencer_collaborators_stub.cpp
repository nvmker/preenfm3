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
void FMDisplaySequencer::displayBeat() {}
void FMDisplaySequencer::newNoteEntered(int) {}
void FMDisplaySequencer::updateCurrentData() {}
void FMDisplaySequencer::sequencerWasUpdated(uint8_t, uint8_t, uint8_t) {}
