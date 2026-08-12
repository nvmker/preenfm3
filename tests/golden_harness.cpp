// Implementation of the golden-master render harness. See golden_harness.h.
//
// The SynthState setup is copied verbatim from tests/midi_decoder_test.cpp's
// MidiDecoderRouting fixture, minus the MidiDecoder / VisualInfo members (the
// render path needs neither — notes enter via Synth::noteOn directly).

#include "golden_harness.h"

#include <cstdlib>
#include <cstring>
#include <cmath>
#include <iostream>
#include <vector>

#include "golden/golden_snapshot.h"

namespace golden {
namespace {

// Equal-tempered frequency for a MIDI note (A4=440). Patches scaleFrequencies[]
// so Timbre::preenNoteOn's `scaleFrequencies[note] == 0` early-return does not
// fire and the note actually allocates a voice. Copied from midi_decoder_test.
float EqualTemperedFreq(int note) {
    return 440.0f * powf(2.0f, (note - 69) / 12.0f);
}

}  // namespace

GoldenHarness::GoldenHarness(std::string fixtureDir, TimbreSetup setup)
    : fixtureDir_(std::move(fixtureDir)),
      setup_(setup),
      ss_(nullptr),
      synth_(nullptr) {
    // Defensive precondition on the per-timbre voice budget: rebuidVoiceAllTimbre
    // (Synth.cpp:824) statically partitions the global pool by summing
    // numberOfVoices across timbres into voiceNumber_[0..15]; if the sum exceeds
    // MAX_NUMBER_OF_VOICES (16) the partition write goes OOB and the subsequent
    // render reads garbage voices. The factories (g0Default=6, multiTimbre=12)
    // stay within budget; this guard catches a future TimbreSetup that doesn't.
    int voiceSum = 0;
    for (int t = 0; t < NUMBER_OF_TIMBRES; t++) {
        if (setup_.voices[t] < 0) {
            std::cerr << "golden: TimbreSetup.voices[" << t << "] = "
                      << setup_.voices[t] << " is negative — wraps to a huge "
                         "uint8_t at assignment (MixerState.h numberOfVoices) "
                         "and OOBs voiceNumber_; refusing to construct.\n";
            std::abort();
        }
        voiceSum += setup_.voices[t];
    }
    if (voiceSum > MAX_NUMBER_OF_VOICES) {
        std::cerr << "golden: TimbreSetup voice sum " << voiceSum
                  << " > MAX_NUMBER_OF_VOICES (" << MAX_NUMBER_OF_VOICES
                  << ") — rebuidVoiceAllTimbre would write voiceNumber_ OOB; "
                  << "refusing to construct. Reduce a TimbreSetup voice count.\n";
        std::abort();
    }
    // Mirror the firmware: Synth is a global (static storage -> BSS zero-init)
    // so all member buffers start at zero before the (empty) Synth ctor runs.
    // buildNewSampleBlock's UNGUARDED third mix loop reads every timbre's sample
    // block regardless of numberOfVoices, so disabled timbres' buffers MUST be
    // zero. Zero the backing then placement-construct: valid vptr + zero
    // buffers, exactly matching the firmware's BSS+ctor. See golden_harness.h.
    std::memset(&synthBacking_, 0, sizeof(synthBacking_));
    synth_ = new (&synthBacking_) Synth();
    // Phase G4: zero + placement-construct MidiDecoder (same rationale as
    // Synth — its ctor inits most members but NOT songPosition, read on every
    // 6th MIDI_CLOCK; zeroing covers it). Precede setUpSynthState's wiring.
    std::memset(&midiDecoderBacking_, 0, sizeof(midiDecoderBacking_));
    midiDecoder_ = new (&midiDecoderBacking_) MidiDecoder();
    // Phase G4: zero + placement-construct Sequencer + FMDisplaySequencer the
    // same way Synth is handled. Firmware globals are BSS zero-init; several
    // Sequencer bool members (e.g. extMidiRunning_, Sequencer.cpp:205) are read
    // without being set by the ctor/reset() and rely on that zero-init. A plain
    // member would leave them indeterminate -> UBSAN under test-asan. Must
    // precede the wiring in setUpSynthState.
    std::memset(&seqBacking_, 0, sizeof(seqBacking_));
    seq_ = new (&seqBacking_) Sequencer();
    std::memset(&dispSeqBacking_, 0, sizeof(dispSeqBacking_));
    displaySeq_ = new (&dispSeqBacking_) FMDisplaySequencer();
    setUpSynthState();
}

GoldenHarness::~GoldenHarness() {
    if (synth_) synth_->~Synth();                     // reverse construction order:
    if (midiDecoder_) midiDecoder_->~MidiDecoder();  //  dependents before deps;
    if (seq_) seq_->~Sequencer();                    //  Synth, MidiDecoder, then
    if (displaySeq_) displaySeq_->~FMDisplaySequencer();   //  Sequencer (uses displaySeq_)
}

void GoldenHarness::setUpSynthState() {
    std::memset(&ssBacking_, 0, sizeof(ssBacking_));
    ss_ = reinterpret_cast<SynthState*>(&ssBacking_);

    // Replicate SynthState::SynthState()'s mixer default-init (firmware ctor,
    // SynthState.cpp:99-103): the real ctor serializes the default mixer preset
    // (tuning=440, per-timbre volume=1.0 / outs / numberOfVoices, reverb +
    // global-FX defaults) and restores it. Without this the audio render path
    // reads zeroed mixer fields (volume=0, tuning=0, fxBus coeffs=0) and the
    // voice/FX path produces inf/nan -> UB on the int32 mix. MixerState.cpp is
    // already linked (Target #4), so this is the faithful fix, not a stub.
    // fxBus_.init() is normally called by SynthState::init() (which needs
    // FMDisplay args the host build doesn't have); call it directly.
    {
        char mixerStateChars[sizeof(ss_->mixerState)];
        uint32_t mixerSize = 0;
        ss_->mixerState.getFullDefaultState(mixerStateChars, &mixerSize, 0);
        ss_->mixerState.restoreFullState(mixerStateChars);
        ss_->mixerState.fxBus_.init();
    }

    // fullState.synthMode: MIXER (so Synth::noteOn routes to voices, not the
    // sequencer).
    ss_->fullState.synthMode = SYNTH_MODE_MIXER;

    // midiConfigValue defaults (SynthState::SynthState() normally sets these;
    // memset zeroes them, so patch explicitly): RECEIVES=3 (CC + NRPN enabled),
    // PROGRAM_CHANGE=1, SENDS=1, USB=OFF.
    ss_->fullState.midiConfigValue[MIDICONFIG_RECEIVES] = 3;
    ss_->fullState.midiConfigValue[MIDICONFIG_PROGRAM_CHANGE] = 1;
    ss_->fullState.midiConfigValue[MIDICONFIG_SENDS] = 1;
    ss_->fullState.midiConfigValue[MIDICONFIG_USB] = USBMIDI_OFF;

    // Mixer routing: globalChannel_=0 (no global), MPE off.
    ss_->mixerState.globalChannel_ = 0;
    ss_->mixerState.currentChannel_ = 0;
    ss_->mixerState.MPE_inst1_ = 0;
    // NOTE: the midi_decoder fixture also patches mixerState.userCC_[i]=255
    // (an impossible CC number) to defeat the MATRIX_SOURCE_USER_CC
    // short-circuit in MidiDecoder::controlChange. The render harness has no
    // MidiDecoder and never dispatches controlChange; with no MIDI input the
    // user-CC matrix sources resolve to 0 regardless, so that patch is
    // irrelevant here and is intentionally omitted.

    // Per-timbre instrumentState: MIDI channel t+1, full note range, and
    // numberOfVoices from TimbreSetup (g0Default: timbre 0=6, timbres 1-5=0 ->
    // silenced; multiTimbre: timbres 0+1=6). Synth::init's
    // numberOfVoicesChanged allocates from this; buildNewSampleBlock skips any
    // timbre whose numberOfVoices==0.
    for (int t = 0; t < NUMBER_OF_TIMBRES; t++) {
        ss_->mixerState.instrumentState_[t].midiChannel = (t == 0) ? 1 : (t + 1);
        ss_->mixerState.instrumentState_[t].firstNote = 0;
        ss_->mixerState.instrumentState_[t].lastNote = 127;
        ss_->mixerState.instrumentState_[t].shiftNote = 0;
        ss_->mixerState.instrumentState_[t].numberOfVoices = setup_.voices[t];
        ss_->mixerState.instrumentState_[t].scaleFrequencies =
            scaleFreqs_.tables[t];
        for (int n = 0; n < 128; n++) {
            scaleFreqs_.tables[t][n] = EqualTemperedFreq(n);
        }
    }

    // Wire Synth: setSynthState runs Synth::init, populating timbres_ /
    // voices_ / params_ from preenMainPreset (SystemCoreClock, needed by init's
    // totalNumberofCyclesInv_, is stubbed in midi_decoder_collaborators_stub).
    synth_->setSynthState(ss_);
    ss_->params = synth_->getTimbre(0)->getParamRaw();

    // Register Synth as a SynthParamListener, mirroring firmware/preenfm3.cpp:449
    // (`synthState.insertParamListener(&synth)`). WITHOUT this, SynthState's
    // firstParamListener stays 0 (memset) and Synth::setNewValueFromMidi's
    // `propagateNewParamValueFromExternal` loops over zero listeners, so
    // Synth::newParamValue NEVER runs for the G3 PARAM_CHANGE path — the flat
    // field write (Timbre::setNewValue) still lands and IS read live by
    // Matrix/LfoOsc, so the goldens render correctly, but the newParamValue
    // side-effects (verifyLfoUsed, resetMatrixDestination) would be a dead path.
    // Registering the listener makes setNewValueFromMidi fully production-
    // faithful. Render-neutral for the committed G3 goldens: the LFO case body
    // in Synth::newParamValue is commented out, and the matrix case's
    // verifyLfoUsed/resetMatrixDestination are no-ops for the goldens' mul/freq
    // changes (verified by re-running the G3 goldens against the committed
    // fixtures after this change — byte-identical). Found by step-04 review
    // (Blind Hunter finding: param-listener registration gap).
    ss_->insertParamListener(synth_);

    // Phase G4: wire a real MidiDecoder to synth_ + a null VisualInfo so
    // MIDI_BYTE events can drive the production clock-byte path (newByte ->
    // synth->midiTick / midiClockStart -> sequencer). Same wiring as
    // midi_decoder_test.cpp's fixture (setSynthState + setSynth +
    // setVisualInfo). The arpeggiator golden sends no MIDI bytes, but the
    // decoder is wired unconditionally — it is cheap, keeps the harness model
    // uniform, and makes the seq-external golden a pure additive script.
    midiDecoder_->setSynthState(ss_);
    midiDecoder_->setSynth(synth_);
    midiDecoder_->setVisualInfo(&visualInfo_);

    // Phase G4: wire a real Sequencer bidirectionally (preenfm3.cpp:417-420).
    // setRefreshStatusPointer MUST precede any refresh() call — onMidiStart ->
    // displaySequencer_->refresh derefs *refreshStatusP_, which the stub ctor
    // (sequencer_collaborators_stub.cpp) doesn't init. displaySeq_.setSequencer
    // is skipped: FMDisplaySequencer::setSequencer is off-host (no stub) ->
    // link error; the smoke test proved playback doesn't need it. Render-neutral
    // for G0-G3/arp: no MIDI clock bytes -> onMidiClock never fires, and
    // synthMode=MIXER means Synth::noteOn never routes to sequencer_->insertNote.
    displaySeq_->setRefreshStatusPointer(&dummyRefreshStatusA_,
                                         &dummyRefreshStatusB_);
    synth_->setSequencer(seq_);
    seq_->setSynth(synth_);
    seq_->setDisplaySequencer(displaySeq_);
}

void GoldenHarness::renderScript(const RenderScript& script,
                                std::size_t nBlocks, int32_t* out) {
    // Apply events in listed order as each block offset is reached. Factories
    // emit events pre-sorted by blockOffset (see golden_harness.h); a cursor
    // advances through `events` so each event fires exactly once, immediately
    // before its block's buildNewSampleBlock. Same-offset events (e.g. the two
    // noteOns in multiTimbreMix) fire in listed order before that block.
    //
    // Defensive: every event must fire within the render window. The for-loop
    // below only visits blocks [0, nBlocks); an event whose blockOffset >= nBlocks
    // would NEVER fire and the render would silently change character (e.g. the
    // envelopeAdsrFull noteOff@300 vanishing if nBlocks is later cut to <=300,
    // turning a full-ADSR golden into a silent sustain). Abort loudly instead of
    // locking a misleading fixture.
    for (const auto& ev : script.events) {
        if (ev.blockOffset >= nBlocks) {
            std::cerr << "golden: renderScript event at block " << ev.blockOffset
                      << " is past nBlocks=" << nBlocks
                      << " — it would never fire and the render would silently "
                      << "change. Fix the script or raise nBlocks.\n";
            std::abort();
        }
    }
    std::size_t nextEvent = 0;
    int32_t b1[kSamplesPerBuffer];
    int32_t b2[kSamplesPerBuffer];
    int32_t b3[kSamplesPerBuffer];

    for (std::size_t blk = 0; blk < nBlocks; blk++) {
        while (nextEvent < script.events.size() &&
               script.events[nextEvent].blockOffset == blk) {
            const RenderEvent& ev = script.events[nextEvent++];
            switch (ev.kind) {
                case RenderEventKind::NOTE_ON:
                    synth_->noteOn(ev.timbre, ev.note, ev.velocity);
                    break;
                case RenderEventKind::NOTE_OFF:
                    synth_->noteOff(ev.timbre, ev.note);
                    break;
                case RenderEventKind::PARAM_CHANGE: {
                    // Production-faithful CC-routing entry point (Synth.h:122).
                    // Only (row,encoder) pairs with a newParamValueFromExternal
                    // switch case take effect live on a sounding voice —
                    // ENCODER_MATRIX_MUL (writes rows[r].mul, read live by
                    // Matrix::computeAllDestinations) and ENCODER_LFO_FREQ
                    // (writes lfo->freq, read live) both qualify. See
                    // RenderEventKind doc + spec Design Notes for the
                    // ROW_OSC1..6 / ROW_ENGINE no-case constraint.
                    //
                    // Bounds validation (step-04 review, Edge Case Hunter):
                    // setNewValueFromMidi indexes allParameterRows.row[row] and
                    // params_[row*4+encoder] with NO internal check; a fat-
                    // fingered row/encoder would OOB-heap-read/write. The
                    // factory paramChange(...) is the only constructor, but the
                    // fields are public ints, so validate here as the last line
                    // of defense (consistent with the blockOffset/rowIdx aborts
                    // elsewhere in the harness). NaN/Inf value would bypass
                    // Timbre::setNewValue's clamp (NaN comparisons are false)
                    // and poison the field — reject non-finite values too.
                    if (ev.timbre < 0 || ev.timbre >= NUMBER_OF_TIMBRES ||
                        ev.row < 0 || ev.row >= NUMBER_OF_ROWS ||
                        ev.encoder < 0 ||
                        ev.encoder >= NUMBER_OF_ENCODERS_PFM2) {
                        std::cerr << "golden: PARAM_CHANGE at block "
                                  << ev.blockOffset << " has out-of-range "
                                  << "(timbre=" << ev.timbre << " row=" << ev.row
                                  << " encoder=" << ev.encoder << "; valid "
                                  << "timbre[0," << NUMBER_OF_TIMBRES
                                  << ") row[0," << NUMBER_OF_ROWS
                                  << ") encoder[0," << NUMBER_OF_ENCODERS_PFM2
                                  << ")) — refusing to dispatch (would OOB "
                                  << "index params_/allParameterRows).\n";
                        std::abort();
                    }
                    if (!std::isfinite(ev.value)) {
                        std::cerr << "golden: PARAM_CHANGE at block "
                                  << ev.blockOffset << " (row=" << ev.row
                                  << " encoder=" << ev.encoder
                                  << ") value is non-finite (" << ev.value
                                  << ") — would bypass the clamp and poison the "
                                  << "param field.\n";
                        std::abort();
                    }
                    synth_->setNewValueFromMidi(ev.timbre, ev.row, ev.encoder,
                                                ev.value);
                    break;
                }
                case RenderEventKind::MIDI_BYTE: {
                    // Phase G4: feed one raw MIDI byte through the REAL
                    // MidiDecoder (newByte) — the production clock-byte parse +
                    // dispatch path (0xF8 -> midiTick + every-6th ->
                    // midiClockSongPositionStep; 0xFA -> midiClockStart).
                    // Exercises MidiDecoder -> Synth -> Sequencer for the
                    // seq-external golden; the arp golden sends no MIDI bytes.
                    // No bounds check: a uint8_t can't OOB, and newByte
                    // classifies all 256 values (>=0xF8 realtime switch, else
                    // the status/data state machine, which no-ops unknown bytes
                    // harmlessly).
                    midiDecoder_->newByte(ev.byte);
                    break;
                }
                default:
                    // A non-factory RenderEventKind (memory corruption, or a
                    // future kind without a dispatch arm). Aborting matches the
                    // harness's blockOffset/rowIdx guard philosophy — never
                    // silently render through an event we did not handle.
                    std::cerr << "golden: renderScript event at block "
                              << ev.blockOffset << " has unknown kind "
                              << static_cast<int>(ev.kind) << " — refusing to "
                              << "silently skip it.\n";
                    std::abort();
            }
        }
        // buildNewSampleBlock zeroes the 3 buffers itself (Synth.cpp:325-333).
        synth_->buildNewSampleBlock(b1, b2, b3);
        int32_t* dst = out + blk * kSamplesPerBlock;
        std::memcpy(dst + 0 * kSamplesPerBuffer, b1,
                    kSamplesPerBuffer * sizeof(int32_t));
        std::memcpy(dst + 1 * kSamplesPerBuffer, b2,
                    kSamplesPerBuffer * sizeof(int32_t));
        std::memcpy(dst + 2 * kSamplesPerBuffer, b3,
                    kSamplesPerBuffer * sizeof(int32_t));
    }
}

void GoldenHarness::renderA4DefaultSustain(std::size_t nBlocks, int32_t* out) {
    // Thin delegator (kept so G0's GoldenMaster.A4DefaultSustain200Blocks
    // compiles unchanged). Byte-identical to the former inline implementation:
    // the noteOn fires before block 0's buildNewSampleBlock, matching the old
    // pre-loop call.
    renderScript(RenderScript::a4Sustain(), nBlocks, out);
}

void GoldenHarness::setTimbreAlgo(int timbre, Algorithm algo) {
    // Write the float field read LIVE every block by Voice/Env via the pointer
    // wired in Timbre::init (Timbre.cpp:283-288), then run the production-
    // faithful re-init (Synth.h:95; idempotent; re-applies env curves/matrix/
    // FX). Must precede noteOn (see golden_harness.h).
    synth_->getTimbre(timbre)->getParamRaw()->engine1.algo = algo;
    synth_->afterNewParamsLoad(timbre);
}

void GoldenHarness::setMatrixRow(int timbre, int rowIdx, int source, float mul,
                                 int dest1, int dest2) {
    // Overwrite one matrix row's {source, mul, dest1, dest2} fields. The rows
    // live at params_.matrixRowState1..12 (Common.h:574); MatrixRowParams is
    // {source, mul, dest1, dest2} (Common.h:491) — exactly 4 floats, matching
    // NUMBER_OF_ENCODERS_PFM2 so the flat-index math in setNewValueFromMidi
    // (row*4+encoder) lands on the right field. Matrix::init bound
    // &matrixRowState1 to the runtime Matrix (Matrix.h:31) at Synth::init
    // time; that pointer stays valid (params_ doesn't move), so the patched
    // field values are read live every block by computeAllDestinations.
    // afterNewParamsLoad resets the runtime sources/destinations caches
    // (Voice::afterNewParamsLoad) so the new routing takes effect from block 0.
    // Same proven pattern as setTimbreAlgo. Must precede noteOn.
    // Validate ALL inputs (step-04 review + Copilot PR review): an invalid
    // timbre indexes timbres_[] OOB via getTimbre(); source/dest1/dest2 are
    // later dereferenced as array indices by Matrix::computeAllDestinations
    // (sources[(int)source], destinations[(int)dest1], destinations[(int)dest2]);
    // a non-finite mul poisons the render. Same defensive philosophy as the
    // PARAM_CHANGE dispatch + the blockOffset guard. MATRIX_SOURCE_MAX and
    // DESTINATION_MAX are counts (Common.h), so half-open [0, MAX). DESTINATION_NONE
    // (=0) is a valid dest ("no second destination").
    if (timbre < 0 || timbre >= NUMBER_OF_TIMBRES) {
        std::cerr << "golden: setMatrixRow timbre=" << timbre << " out of range "
                  << "[0," << NUMBER_OF_TIMBRES << ") — would OOB timbres_[].\n";
        std::abort();
    }
    if (rowIdx < 0 || rowIdx >= MATRIX_SIZE) {
        std::cerr << "golden: setMatrixRow rowIdx=" << rowIdx
                  << " out of range [0," << MATRIX_SIZE
                  << ") — refusing to write OOB; fix the caller.\n";
        std::abort();
    }
    if (source < 0 || source >= MATRIX_SOURCE_MAX ||
        dest1 < 0 || dest1 >= DESTINATION_MAX ||
        dest2 < 0 || dest2 >= DESTINATION_MAX) {
        std::cerr << "golden: setMatrixRow(timbre=" << timbre
                  << ",rowIdx=" << rowIdx << ") has out-of-range source/dest "
                  << "(source=" << source << " dest1=" << dest1
                  << " dest2=" << dest2 << "; valid source[0,"
                  << MATRIX_SOURCE_MAX << ") dest[0," << DESTINATION_MAX
                  << ")) — would OOB-index Matrix sources/destinations.\n";
        std::abort();
    }
    if (!std::isfinite(mul)) {
        std::cerr << "golden: setMatrixRow(timbre=" << timbre
                  << ",rowIdx=" << rowIdx << ") mul is non-finite (" << mul
                  << ") — would poison the render.\n";
        std::abort();
    }
    struct MatrixRowParams* row;
    switch (rowIdx) {
        case 0:  row = &synth_->getTimbre(timbre)->getParamRaw()->matrixRowState1;  break;
        case 1:  row = &synth_->getTimbre(timbre)->getParamRaw()->matrixRowState2;  break;
        case 2:  row = &synth_->getTimbre(timbre)->getParamRaw()->matrixRowState3;  break;
        case 3:  row = &synth_->getTimbre(timbre)->getParamRaw()->matrixRowState4;  break;
        case 4:  row = &synth_->getTimbre(timbre)->getParamRaw()->matrixRowState5;  break;
        case 5:  row = &synth_->getTimbre(timbre)->getParamRaw()->matrixRowState6;  break;
        case 6:  row = &synth_->getTimbre(timbre)->getParamRaw()->matrixRowState7;  break;
        case 7:  row = &synth_->getTimbre(timbre)->getParamRaw()->matrixRowState8;  break;
        case 8:  row = &synth_->getTimbre(timbre)->getParamRaw()->matrixRowState9;  break;
        case 9:  row = &synth_->getTimbre(timbre)->getParamRaw()->matrixRowState10; break;
        case 10: row = &synth_->getTimbre(timbre)->getParamRaw()->matrixRowState11; break;
        case 11: row = &synth_->getTimbre(timbre)->getParamRaw()->matrixRowState12; break;
        default: return;  // unreachable (guarded above)
    }
    row->source = static_cast<float>(source);
    row->mul    = mul;
    row->dest1  = static_cast<float>(dest1);
    row->dest2  = static_cast<float>(dest2);
    synth_->afterNewParamsLoad(timbre);
}

void GoldenHarness::enableArpeggiator(int timbre, int bpm, int direction,
                                      int octave) {
    // Validate (same defensive philosophy as setMatrixRow / PARAM_CHANGE): an
    // invalid timbre OOBs timbres_[] via getTimbre(); bpm<=0 zeroes
    // ticksPerSecond_ in setNewBPMValue -> division-by-zero in
    // updateArpegiatorInternalClock (ticksEveryNCalls_ = CALLED_PER_SECOND /
    // ticksPerSecond_). direction/octave have no OOB risk in the arp engine
    // (bounded by usage) but reject negatives defensively.
    if (timbre < 0 || timbre >= NUMBER_OF_TIMBRES) {
        std::cerr << "golden: enableArpeggiator timbre=" << timbre << " out of "
                  << "range [0," << NUMBER_OF_TIMBRES << ") — would OOB "
                  << "timbres_[].\n";
        std::abort();
    }
    if (bpm <= 0) {
        std::cerr << "golden: enableArpeggiator bpm=" << bpm << " <= 0 — would "
                  << "zero ticksPerSecond_ and divide-by-zero in the arp clock.\n";
        std::abort();
    }
    // Synth::newParamValue casts bpm to uint8_t (Synth.cpp:646) — values > 255
    // wrap (256->0) -> setNewBPMValue(0) -> divide-by-zero. Reject > 255.
    // direction/octave are also uint8_t-cast but the arp engine bounds them by
    // usage (octave in modular arithmetic; unknown direction -> UP), so no
    // extra clamp is needed for correctness. (step-04 review, Edge Case Hunter.)
    if (bpm > 255) {
        std::cerr << "golden: enableArpeggiator bpm=" << bpm << " > 255 — the "
                  << "firmware cast to uint8_t wraps it (256->0), zeroing "
                  << "ticksPerSecond_ -> divide-by-zero.\n";
        std::abort();
    }
    // Order matters: BPM FIRST (writes params_.engineArp1.BPM + calls
    // setNewBPMValue -> ticksPerSecond_/ticksEveryNCalls_), THEN CLOCK_INTERNAL
    // (setArpeggiatorClock re-calls setNewBPMValue(params_.engineArp1.BPM), now
    // non-zero). DIRECTION/OCTAVE write flat fields read live by the arp engine
    // — OCTAVE has no newParamValue case but the flat write lands and
    // engineArp1.octave is read in StepArpeggio (Timbre.cpp:2865+). The stub's
    // permissive ParameterRowDisplay for ROW_ARPEGGIATOR1 lets all four values
    // through (see midi_decoder_collaborators_stub.cpp).
    synth_->setNewValueFromMidi(timbre, ROW_ARPEGGIATOR1, ENCODER_ARPEGGIATOR_BPM,
                                static_cast<float>(bpm));
    synth_->setNewValueFromMidi(timbre, ROW_ARPEGGIATOR1, ENCODER_ARPEGGIATOR_CLOCK,
                                static_cast<float>(CLOCK_INTERNAL));
    synth_->setNewValueFromMidi(timbre, ROW_ARPEGGIATOR1, ENCODER_ARPEGGIATOR_DIRECTION,
                                static_cast<float>(direction));
    synth_->setNewValueFromMidi(timbre, ROW_ARPEGGIATOR1, ENCODER_ARPEGGIATOR_OCTAVE,
                                static_cast<float>(octave));
}

void GoldenHarness::setupSequencerTriadPlayback() {
    // Guard the serialize-patch below against a future sequencer-state format
    // bump: the patch assumes stepActivated_[] is the LAST
    // NUMBER_OF_STEP_SEQUENCES bytes of the getFullState buffer
    // (Sequencer.cpp:727-733, true for SEQ_VERSION2). A version change could
    // move stepActivated_[] -> the patch flips the wrong byte -> silent seq
    // golden (zero-signal trap). static_assert fails the build at the bump.
    // (step-04 review, Blind Hunter + Edge Case Hunter.)
    static_assert(SEQ_CURRENT_VERSION == SEQ_VERSION2,
                  "serialize-patch offset assumes SEQ_VERSION2 layout; "
                  "re-verify stepActivated_[] position after a format bump");
    // Populate instrument 0's step sequence: C4/E4/G4 (MIDI 60/64/67) at step
    // indices 0/32/64 over 1 bar. stepGetSequence returns StepSeqValue* (a
    // union: full / values[8] / unique share storage; values[2]=velocity,
    // values[3]=note; unique = the per-step retrigger token). full=0 first
    // zeroes the union, then unique/values write specific bytes. Each step's
    // unique MUST differ or the gate `instrumentStepLastUnique_ != newUnique`
    // suppresses retrigger (Sequencer.cpp:~407); 10/11/12 are distinct non-zero
    // tokens. The array INDEX (0/32/64) is WHEN the note fires (step index =
    // midiClockTimer_>>4); values[3] is WHICH note.
    StepSeqValue* steps = seq_->stepGetSequence(0);
    steps[0].full  = 0; steps[0].unique  = 10; steps[0].values[2]  = 100; steps[0].values[3]  = 60;
    steps[32].full = 0; steps[32].unique = 11; steps[32].values[2] = 100; steps[32].values[3] = 64;
    steps[64].full = 0; steps[64].unique = 12; steps[64].values[2] = 100; steps[64].values[3] = 67;
    // Activate step-seq 0 via the FAITHFUL serialize route. stepActivated_ is
    // private (only the recording path stepRecordNotes sets it; no public
    // setter), but setFullState reads it from the buffer. getFullState writes
    // stepActivated_[] LAST (Sequencer.cpp:727-733), so it occupies the final
    // NUMBER_OF_STEP_SEQUENCES bytes — flip byte 0 to activate seq 0. The
    // default instrumentStepSeq_[0]=0 maps instrument 0 -> step-seq 0.
    // stepNotes (populated above) are NOT serialized, so setFullState does NOT
    // clobber them. externalClock_ is already true (the Sequencer ctor's
    // getFullDefaultState sets it).
    uint8_t buf[2048]; uint32_t size = 0;
    seq_->getFullState(buf, &size);
    // Assumes instrumentStepSeq_[0]==0 (the default post-ctor: instrument 0 ->
    // step-seq 0). If a future default-state change made instrumentStepSeq_[0]
    // != 0, activating stepActivated_[0] would arm the WRONG instrument's seq
    // -> silent seq golden. The default is stable (loadStateVersion2 sets
    // instrumentStepSeq_[t]=t). (step-04 review, Edge Case Hunter.)
    buf[size - NUMBER_OF_STEP_SEQUENCES] = 1;   // stepActivated_[0] = true
    seq_->setFullState(buf);
    // Arm: start playback (running_=true). onMidiClock gates on running_;
    // without this the 0xF8 bytes advance nothing.
    seq_->start();
}

bool GoldenHarness::compareAgainstFixture(const std::string& id,
                                          const int32_t* actual,
                                          std::size_t nBlocks,
                                          GoldenDiff* diffOut) {
    const std::size_t count = nBlocks * kSamplesPerBlock;

    // Read the committed render into an RAII buffer.
    const std::string binPath = fixturePath(fixtureDir_, id, ".bin");
    std::vector<int32_t> expected(count);
    if (!readRenderBin(binPath, expected.data(), count)) {
        if (diffOut) {
            diffOut->matched = false;
            diffOut->firstMismatchIndex = 0;
            diffOut->expectedSample = 0;
            diffOut->actualSample = 0;
            diffOut->sampleDelta = 0;
            diffOut->blockIndex = 0;
        }
        std::cerr << "golden: could not read fixture bin: " << binPath << "\n";
        return false;
    }

    // Authoritative tolerance gate: goldenCompare in stored int32 units, where
    // 256 = ±1 audio-LSB (the firmware clamps to 24-bit then <<8). See
    // golden_harness.h kCompareLsbTolerance.
    bool cmp = goldenCompare(expected.data(), actual, count, kCompareLsbTolerance,
                             kSamplesPerBlock, diffOut);

    // DIAGNOSTIC-only hash fingerprint (tolerance 1 granularity; decoupled from
    // the compare tolerance so the committed .xxh stays stable). A mismatch is
    // printed to stderr but NEVER fails the test — goldenCompare is
    // authoritative. (The tolerance-normalized hash can flip on a normalization
    // bucket boundary even when goldenCompare passes; making it authoritative
    // would defeat the tolerance.)
    const uint64_t actualHash = goldenHash(actual, count, kHashLsbTolerance);
    uint64_t expectedHash = 0;
    const std::string xxhPath = fixturePath(fixtureDir_, id, ".xxh");
    if (readHashFile(xxhPath, &expectedHash)) {
        if (actualHash != expectedHash) {
            std::cerr << "golden: hash DIAGNOSTIC mismatch for " << id
                      << " (expected=" << std::hex << expectedHash
                      << " actual=" << actualHash << std::dec
                      << "); goldenCompare is authoritative — this is informational only\n";
        }
    } else {
        std::cerr << "golden: hash DIAGNOSTIC — could not read .xxh for " << id
                  << " (informational only; goldenCompare is authoritative)\n";
    }

    return cmp;
}

uint64_t GoldenHarness::writeFixture(const std::string& id, const int32_t* actual,
                                     std::size_t nBlocks) {
    const std::size_t count = nBlocks * kSamplesPerBlock;
    const std::string binPath = fixturePath(fixtureDir_, id, ".bin");
    const std::string xxhPath = fixturePath(fixtureDir_, id, ".xxh");
    const std::string diffPath = fixturePath(fixtureDir_, id, ".diff.txt");

    if (!writeRenderBin(binPath, actual, count)) {
        std::cerr << "golden: FAILED to write " << binPath << "\n";
        return 0;
    }
    uint64_t h = goldenHash(actual, count, kHashLsbTolerance);
    if (!writeHashFile(xxhPath, h)) {
        std::cerr << "golden: FAILED to write " << xxhPath << "\n";
        return 0;
    }
    if (!writeDiffTxt(diffPath, actual, nBlocks, kSamplesPerBuffer,
                      kBuffersPerBlock)) {
        std::cerr << "golden: FAILED to write " << diffPath << "\n";
        return 0;
    }
    std::cout << "golden: regenerated fixture '" << id << "' hash=" << std::hex
              << h << std::dec << " blocks=" << nBlocks
              << " samples=" << count << "\n";
    return h;
}

RenderScript RenderScript::a4Sustain() {
    // G0 + the two FM-algo goldens: single A4 (MIDI 69) noteOn at vel 100,
    // sustain plateau (no note-off). The FM goldens set the algorithm out-of-
    // band via setTimbreAlgo before calling renderScript with this script.
    return { { RenderEvent::noteOn(0, 0, (char)69, (char)100) } };
}

RenderScript RenderScript::envelopeAdsrFull() {
    // Full ADSR: attack+sustain+release. noteOn at block 0, noteOff at block 300
    // (release tail captured across the remaining 300 blocks). Render 600.
    return { { RenderEvent::noteOn(0, 0, (char)69, (char)100),
               RenderEvent::noteOff(300, 0, (char)69) } };
}

RenderScript RenderScript::multiTimbreMix() {
    // Timbres 0 + 1 both noteOn at block 0 (sum 12 <= MAX_NUMBER_OF_VOICES 16).
    // Guards voicesToTimbre mix + per-timbre smoothVolume_ + fxBus->mixAdd.
    return { { RenderEvent::noteOn(0, 0, (char)69, (char)100),
               RenderEvent::noteOn(0, 1, (char)72, (char)100) } };
}

RenderScript RenderScript::liveLfoPitchModulation() {
    // G3 golden 1 (steady LFO->pitch): noteOn@0, render 400 blocks (~1.2
    // LFO1 cycles; lfoOsc1 default = {LFO_SIN, 4.5, 0, 0}). The LFO1->OSC1_FREQ
    // routing is set out-of-band via setMatrixRow(0, row8, LFO1, 0.5, OSC1_FREQ)
    // in the TEST before render — it is a per-timbre state change, not an event.
    // The LFO auto-modulates osc1 pitch; no PARAM_CHANGE needed. Render window
    // 400 captures ~1.2 LFO1 cycles (~267 ms of modulation); a routing/amplitude
    // regression still moves many samples across 400 blocks (not a 1-block edge
    // effect).
    return { { RenderEvent::noteOn(0, 0, (char)69, (char)100) } };
}

RenderScript RenderScript::liveMatrixMulChange() {
    // G3 golden 2 (live matrix-mul change): noteOn@0 with the LFO1->OSC1_FREQ
    // routing set to mul=0.0 out-of-band on matrixRowState8 (rowIdx 7), then a
    // PARAM_CHANGE at block 80 sets ROW_MATRIX8/ENCODER_MATRIX_MUL = 0.6,
    // turning modulation ON mid-note. ROW_MATRIX8 (not ROW_MATRIX1) so the
    // PARAM_CHANGE writes the SAME row the TEST's setMatrixRow(0,7,...) set up
    // (matrixRowState8). Row 7 (rows 4+ generic loop) avoids the rows 0-3
    // special-case path + the MTX1..4_MUL feedback-lock quirk. Locks the live
    // CC->matrix-mul->voice path: pitch wobble kicks in from block 80. Render 200.
    return { { RenderEvent::noteOn(0, 0, (char)69, (char)100),
               RenderEvent::paramChange(80, 0, ROW_MATRIX8,
                                        ENCODER_MATRIX_MUL, 0.6f) } };
}

RenderScript RenderScript::liveLfoFreqChange() {
    // G3 golden 3 (live LFO-frequency change on an amplitude destination):
    // noteOn@0 with LFO1->MIX_OSC1 routing set out-of-band (mul=0.5, a tremolo
    // on osc1's level — audible on a sustained note, unlike ALL_ENV_DECAY which
    // is silent once the env leaves its decay stage ~block 50; see Spec Change
    // Log). A PARAM_CHANGE at block 100 sets ROW_LFOOSC1/ENCODER_LFO_FREQ = 9.0
    // (doubling the LFO speed from the default 4.5), so the tremolo rate
    // doubles mid-note. Locks the live LFO-freq CC update path + a non-pitch
    // (amplitude) matrix destination. Render 300. (ROW_LFOOSC1/ENCODER_LFO_FREQ
    // writes lfoOsc1.freq, read live by LfoOsc; the stub's permissive
    // ParameterDisplay for LFO rows lets the value through — see
    // midi_decoder_collaborators_stub.cpp.)
    return { { RenderEvent::noteOn(0, 0, (char)69, (char)100),
               RenderEvent::paramChange(100, 0, ROW_LFOOSC1,
                                        ENCODER_LFO_FREQ, 9.0f) } };
}

RenderScript RenderScript::arpTriadUp() {
    // G4: C-major triad (MIDI 60/64/67) held at block 0; the arpeggiator
    // (enabled out-of-band via enableArpeggiator in the TEST: internal clock
    // @120 BPM, UP=0, 2 octaves) cycles the three notes across octaves. At
    // 120 BPM the internal arp clock advances ~1 step per 31 blocks
    // (CALLED_PER_SECOND=1500 calls/sec / (120*24/60)=48 ticks-per-sec -> ~31.25
    // calls/tick), so 300 blocks captures ~9 arp steps (3 notes x 2 oct =
    // 6-note cycle, ~1.5 full cycles) — enough periodic retriggering to lock
    // the arp note-cycling + octave-shift + voice-realloc path. The arp's
    // clock is a pure block counter (Timbre::updateArpegiatorInternalClock,
    // advanced inside buildNewSampleBlock) — no MIDI bytes / HAL shim.
    return { { RenderEvent::noteOn(0, 0, (char)60, (char)100),
               RenderEvent::noteOn(0, 0, (char)64, (char)100),
               RenderEvent::noteOn(0, 0, (char)67, (char)100) } };
}

RenderScript RenderScript::seqExternalPlayback() {
    // G4 seq-external: 0xFA (MIDI_START) at block 0, then bursts of 4x0xF8
    // (MIDI_CLOCK) at blocks 1..96 = 384 clocks (1 full bar @ the Sequencer's
    // default tempo). Each 0xF8 -> synth->midiTick -> sequencer->onMidiClock,
    // which adds 256/24 ~= 10.667 to midiClockTimer_; step index =
    // midiClockTimer_>>4 advances ~every 1.5 clocks, so the 3 step notes
    // (C4/E4/G4 at step indices 0/32/64, set out-of-band via
    // setupSequencerTriadPlayback) fire at ~0/48/96 clocks and the bar wraps.
    // Each clock burst fires noteOnFromSequencer synchronously during newByte,
    // BEFORE that block's buildNewSampleBlock captures the resulting audio.
    // Bursts of 4 + 1 render (vs all 384 clocks up front) yields separable
    // note-onset transients in the fixture. Render 97 blocks (0..96).
    std::vector<RenderEvent> ev;
    ev.reserve(1 + 96 * 4);
    ev.push_back(RenderEvent::midiByte(0, 0xFA));
    for (int blk = 1; blk <= 96; blk++) {
        for (int c = 0; c < 4; c++) ev.push_back(RenderEvent::midiByte(blk, 0xF8));
    }
    return { std::move(ev) };
}

TimbreSetup TimbreSetup::g0Default() {
    TimbreSetup s{};   // value-init zeroes all 6 timbres
    s.voices[0] = 6;
    return s;
}

TimbreSetup TimbreSetup::multiTimbre() {
    TimbreSetup s{};
    s.voices[0] = 6;
    s.voices[1] = 6;   // sum 12 <= MAX_NUMBER_OF_VOICES (16)
    return s;
}

}  // namespace golden
