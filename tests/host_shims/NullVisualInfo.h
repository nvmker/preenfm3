// Null VisualInfo test double, shared by tests/midi_decoder_test.cpp (MIDI
// decode + dispatch coverage) and tests/golden_harness.{h,cpp} (Phase G4:
// MidiDecoder-driven MIDI-clock goldens).
//
// MidiDecoder::newByte dispatches visualInfo->noteOn(timbre, true) on NoteOn
// and visualInfo->midiClock(bool) on every 6th MIDI_CLOCK byte (MidiDecoder.cpp
// ~midiClockCpt==6 branch). The host tests render no UI; no-op these so the
// dispatch stays link-clean without a real FMDisplay3. Extracted from
// midi_decoder_test.cpp (where it was a file-local class) so the golden harness
// can own a MidiDecoder without duplicating it.

#pragma once

#include "VisualInfo.h"

class NullVisualInfo : public VisualInfo {
public:
    void midiClock(bool /*show*/) override {}
    void noteOn(int /*timbre*/, bool /*show*/) override {}
};
