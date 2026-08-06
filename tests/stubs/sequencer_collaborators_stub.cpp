// Test-double bodies for Sequencer.cpp's collaborators.
//
// WHY THIS FILE EXISTS (see tests/SEAM.md "Contact with the code")
// ----------------------------------------------------------------
// Sequencer.cpp is host-compilable as a translation unit, but its compiled body
// references out-of-line Synth / FMDisplaySequencer methods (in mainSequencerTic,
// reset, stop, insertNote, ...). The serialization tests only ever exercise the
// get/setFull* path, which dereferences neither collaborator. To LINK without
// pulling in the full synth engine (Synth.cpp + Voice/Timbre/FxBus/...), we
// supply empty definitions for exactly the methods Sequencer.cpp's compiled
// body references. These are standard test doubles, NOT HAL fakes; they contain
// no logic and must not drift from the real signatures (a mismatch fails to
// compile, which is the intended early signal).
//
// Methods that are already inline in Synth.h / FMDisplaySequencer.h
// (midiClockSongPositionStep, refresh, refreshMemory, refreshActivated,
// refreshStepSeq) are deliberately NOT stubbed here.

#include "Synth.h"
#include "FMDisplaySequencer.h"
#include "Timbre.h"  // for Timbre::midiClockSongPositionStep (see below)

// --- Synth -------------------------------------------------------------------
void Synth::stopArpegiator(int) {}
void Synth::allNoteOff(int) {}
void Synth::midiClockStart(bool) {}
void Synth::midiClockContinue(int, bool) {}
void Synth::midiClockStop(bool) {}
void Synth::midiTick(bool) {}
void Synth::noteOnFromSequencer(uint8_t, int16_t, uint8_t) {}
void Synth::noteOffFromSequencer(uint8_t, int16_t) {}

// --- FMDisplaySequencer ------------------------------------------------------
void FMDisplaySequencer::displayBeat() {}
void FMDisplaySequencer::newNoteEntered(int) {}
void FMDisplaySequencer::updateCurrentData() {}
void FMDisplaySequencer::sequencerWasUpdated(uint8_t, uint8_t, uint8_t) {}

// --- Timbre ------------------------------------------------------------------
// Synth::midiClockSongPositionStep is INLINE in Synth.h; Sequencer.cpp's
// ticMillis() references it, so it is emitted into Sequencer.o and its body
// calls Timbre::midiClockSongPositionStep. Empty body satisfies the link.
void Timbre::midiClockSongPositionStep(int) {}
