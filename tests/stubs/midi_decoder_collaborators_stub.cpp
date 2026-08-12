// Test-double bodies for the Synth-graph collaborators that MidiDecoder.cpp
// (and the real Synth graph pulled for Tier 3 routing tests) reference but
// that live in TUs the host build deliberately does NOT compile.
//
// WHY THIS FILE EXISTS (see tests/SEAM.md "Target #4 contact with the code")
// ----------------------------------------------------------------
// 1. SynthState.cpp is NOT pulled into the host build: its closure drags in
//    the FMDisplay family (FMDisplayMixer/Menu/Editor/Sequencer.h via
//    SynthState.cpp's #includes) plus HAL (HRNG). The Synth routing tests do
//    not need ANY of that — SynthState is backed by a memset+aligned buffer
//    and reinterpret_cast<SynthState*> (Target #3 minimal-SynthState precedent,
//    scaled up to the wider field set MidiDecoder reads); the ctor is NEVER
//    called, so it is deliberately NOT defined here (defining it would pull
//    the vtable + every virtual). The out-of-line NON-virtual SynthState
//    methods the compiled Synth/Timbre/Voice bodies reference are stubbed
//    below.
// 2. Synth.cpp reads `SystemCoreClock` (the CMSIS system-clock variable,
//    normally declared via the HAL chain that PFM3_HOST gates out and defined
//    in system_stm32h7xx.c). It is a plain uint32_t data symbol, NOT HAL —
//    defining it here is the standard link-time test double (same pattern as
//    sequencer_collaborators_stub.cpp), not a HAL fake.
// 3. `allParameterRows` is data defined in FMDisplayEditor.cpp (4365 lines of
//    TFT/UI code, infeasible to pull). See the FAVOR-REAL-DATA EXCEPTION
//    comment at the definition below for the scoped, justified stub.
//
// SynthState's heavy methods (loadNewPreset, randomizePreset, encoderTurned,
// ...) are NOT stubbed — the fixture never calls them; their bodies stay
// absent unless a compiled reference forces a stub.

#include "SynthState.h"   // AllParameterRowsDisplay, ParameterRowDisplay (via row[]), SynthState
#include "Common.h"      // NUMBER_OF_ROWS

// --- firmware global normally defined in system_stm32h7xx.c ------------------
// `noise[32]` is defined in Osc.cpp:25 (pulled via Target #3); no stub needed.
//
// `SystemCoreClock` is the CMSIS system-clock variable; Synth::init reads it
// once to compute the CPU-usage inverse. Define a sane nonzero value so the
// division is finite. NOT a HAL stub: plain uint32_t data symbol.
uint32_t SystemCoreClock = 480000000;  // 480 MHz STM32H7 default

// --- SynthState -------------------------------------------------------------
// Stub bodies for the out-of-line NON-virtual SynthState methods the compiled
// Synth/Timbre/Voice bodies reference in code paths the routing tests never
// drive (preset-load / Scala / instrument-switch flows). Signature drift
// fails to compile (intended early signal). The ctor is deliberately NOT
// defined — see file header.

// Called from Synth::setCurrentInstrument (MidiDecoder CC_CURRENT_INSTRUMENT).
// Routing tests observe the CC dispatch into Synth, not SynthState's internals.
void SynthState::setCurrentInstrument(int /*value*/) {}

// Referenced from Timbre init paths; the routing tests never reload Scala tuning.
bool SynthState::scalaSettingsChanged(int /*timbre*/) { return false; }

// Referenced from Synth preset-load paths the routing tests never drive.
void SynthState::setParamsAndTimbre(struct OneSynthParams* /*newParams*/, int /*newCurrentTimbre*/) {}
void SynthState::loadPresetFromMidi(int /*timbre*/, int /*bank*/, int /*bankLSB*/, int /*patchNumber*/, struct OneSynthParams* /*params*/) {}

// --- allParameterRows -------------------------------------------------------
// FAVOR-REAL-DATA EXCEPTION (flagged in tests/SEAM.md Target #4 appendix).
// `allParameterRows` is a `struct AllParameterRowsDisplay` (an array of
// `ParameterRowDisplay*` row pointers) defined in
// firmware/Src/hardware/FMDisplayEditor.cpp. That TU is 4365 lines of TFT/UI
// code (FirmwareTftDisplay.h, COLOR_*, LINE_PARAM_*, tft_->...) and is NOT
// host-compilable in reasonable effort — so we cannot pull the real data the
// way Targets #2/#3 pulled Presets.cpp / waves.c / Common.cpp.
//
// We provide ONE zero-initialized ParameterRowDisplay and point every row[i]
// at it. The zeroed ParameterDisplay entries have displayType=DISPLAY_TYPE_NONE
// (=0), so MidiDecoder::decodeNrpn's FLOAT-family check is false and the raw
// NRPN value passes through unchanged to synth->setNewValueFromMidi. This lets
// the paramMSB<2 branch execute and dispatch without crashing; the value-
// transformation logic that depends on real ParameterDisplay data (displayType,
// minValue) is NOT under test here — that would need the real allParameterRows.
// The NRPN ASSEMBLY (the Tier 2 target) is independent of this data and is
// faithfully exercised. Signature drift (AllParameterRowsDisplay layout
// change) fails to compile, the intended early signal.
namespace {
struct ParameterRowDisplay dummyParamRow = {};  // zeroed: displayType=DISPLAY_TYPE_NONE

// Phase G3 golden-master live-param-change support (FAVOR-REAL-DATA EXCEPTION
// extension). The golden tier drives Synth::setNewValueFromMidi MID-RENDER on
// matrix-mul (ROW_MATRIX8/ENCODER_MATRIX_MUL) and LFO-freq
// (ROW_LFOOSC1/ENCODER_LFO_FREQ) rows. Timbre::setNewValue clamps newValue to
// ParameterDisplay.{min,max}Value; the zeroed dummy above (max=0) rejects every
// positive value, silently defeating those changes (the PARAM_CHANGE writes 0,
// the render is byte-identical to a no-change render — a zero-signal golden).
// This permissive row accepts any in-range value the golden sends. The real
// bounds (matrix mul [-10,24] per FMDisplayEditor.cpp matrixParameterRow;
// LFO freq [0, LFO_FREQ_MAX+0.9=100.8] per lfoParameterRow) would clamp
// IDENTICALLY for the golden's in-range values (matrix mul 0.6, LFO freq 9.0);
// permissive bounds avoid duplicating FMDisplayEditor.cpp's display-layer data
// (names/order arrays) that is not host-relevant. Only the matrix rows and the
// 3 LFO-osc rows point here; every other row stays on the zeroed dummy so
// midi_decoder_test's NRPN-assembly behavior is unchanged.
struct ParameterRowDisplay permissiveParamRow = []{
    struct ParameterRowDisplay r = {};
    for (int e = 0; e < NUMBER_OF_ENCODERS_PFM2; e++) {
        r.params[e].minValue = -1000000.0f;
        r.params[e].maxValue =  1000000.0f;
    }
    return r;
}();
}

struct AllParameterRowsDisplay allParameterRows = []{
    struct AllParameterRowsDisplay a;
    for (int i = 0; i <= NUMBER_OF_ROWS; i++) a.row[i] = &dummyParamRow;
    // G3 golden overrides: matrix rows + LFO-osc rows use permissive bounds so
    // setNewValueFromMidi accepts the golden's in-range values without clamping
    // to 0. See permissiveParamRow doc above.
    for (int r = ROW_MATRIX1; r <= ROW_MATRIX_LAST; r++) a.row[r] = &permissiveParamRow;
    a.row[ROW_LFOOSC1] = &permissiveParamRow;
    a.row[ROW_LFOOSC2] = &permissiveParamRow;
    a.row[ROW_LFOOSC3] = &permissiveParamRow;
    return a;
}();
