// Golden-master render harness for preenfm3. Reuses the minimal-SynthState
// fixture pattern from tests/midi_decoder_test.cpp (memset + field patch) and
// drives the REAL Synth::buildNewSampleBlock render path. No mocks; no
// MidiDecoder (notes go in via Synth::noteOn directly).
//
// CHARACTERIZATION STANCE: this harness locks the CURRENT render output of the
// firmware, including any latent quirks. It is a regression guard, not a spec
// of intended behavior. A golden mismatch is a signal to investigate, not an
// automatic "fix" — see tests/golden/README.md.

#pragma once

#include "Synth.h"
#include "MidiDecoder.h"        // Phase G4: MIDI_BYTE events drive newByte
#include "NullVisualInfo.h"      // Phase G4: null VisualInfo for MidiDecoder
#include "Sequencer.h"           // Phase G4: seq-external golden (owned Sequencer)
#include "FMDisplaySequencer.h"  // Phase G4: owned FMDisplaySequencer (stub ctor)

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "golden/golden_compare.h"

namespace golden {

// Backing storage for a memset+patched SynthState. SynthState's ctor + vtable
// live in SynthState.cpp (deliberately NOT pulled — its closure drags the
// FMDisplay family + HAL). The harness memsets this buffer, reinterprets it as
// SynthState*, and patches exactly the fields Synth::init / noteOn /
// buildNewSampleBlock read. No virtual is dispatched through the resulting
// pointer. Copied verbatim from tests/midi_decoder_test.cpp.
struct SynthStateBacking {
    alignas(alignof(SynthState)) unsigned char bytes[sizeof(SynthState)];
};

// Per-timbre scale-frequency tables (MIDI note range = 128). The fixture owns
// the storage and points each instrumentState_[t].scaleFrequencies at one.
struct ScaleFreqTables {
    float tables[NUMBER_OF_TIMBRES][128];
};

// Backing storage for Synth, zeroed then placement-constructed. In the firmware
// `Synth synth;` is a GLOBAL (preenfm3.cpp:58) -> static storage -> BSS
// zero-initialized before the (empty) Synth ctor runs. Synth::buildNewSampleBlock
// has an UNGUARDED third mix loop that reads every timbre's sample block
// regardless of numberOfVoices, so disabled timbres' buffers MUST start at
// zero. A plain stack member leaves them indeterminate -> garbage mixed into
// the unused outputs -> blowup. Zeroing the backing then placement-new'ing
// mirrors the firmware exactly (zero buffers + valid vtable pointer).
struct SynthBacking {
    alignas(alignof(Synth)) unsigned char bytes[sizeof(Synth)];
};

// Backing storage for Sequencer + FMDisplaySequencer, zeroed then placement-
// constructed — same rationale as SynthBacking. In firmware both are GLOBALS
// (preenfm3.cpp:53/61) -> BSS zero-init before their ctors run. Several
// Sequencer bool/flag members (e.g. extMidiRunning_, Sequencer.cpp:205) are
// read by onMidiStart/onMidiClock WITHOUT being initialized by the ctor or
// reset() — they rely on BSS zero-init. A plain stack member leaves them
// indeterminate -> UBSAN 'load of value N, not a valid bool' under test-asan.
// Zeroing the backing then placement-new'ing mirrors the firmware exactly.
// The FMDisplaySequencer stub ctor (sequencer_collaborators_stub.cpp) sets only
// seqMode_/stepSize_; zeroing first NULLs refreshStatusP_ etc. (setRefresh-
// StatusPointer is called right after construction, before any refresh()).
struct SequencerBacking {
    alignas(alignof(Sequencer)) unsigned char bytes[sizeof(Sequencer)];
};
struct FMDisplaySequencerBacking {
    alignas(alignof(FMDisplaySequencer))
        unsigned char bytes[sizeof(FMDisplaySequencer)];
};
// Backing storage for MidiDecoder, zeroed then placement-constructed — same
// rationale as SynthBacking/SequencerBacking. MidiDecoder's ctor
// (MidiDecoder.cpp:61-78) inits most members (midiClockCpt=0,
// isExternalMidiClockStarted=false) but NOT songPosition (read on every 6th
// MIDI_CLOCK at MidiDecoder.cpp:103). A proper 0xFA-first script sets
// songPosition=0 before any read, but zeroing the backing first is belt-and-
// suspenders matching the Sequencer treatment of the same uninit-member class —
// a future script emitting 0xF8 before 0xFA can't then dispatch an arbitrary
// song position (UB + wrong render). (Step-04 Edge Case Hunter finding.)
struct MidiDecoderBacking {
    alignas(alignof(MidiDecoder)) unsigned char bytes[sizeof(MidiDecoder)];
};

// The kind of a RenderEvent. NOTE_ON/NOTE_OFF fire Synth::noteOn/noteOff;
// PARAM_CHANGE fires Synth::setNewValueFromMidi(timbre, row, encoder, value) —
// the production-faithful CC-routing entry point (Synth.h:122). PARAM_CHANGE is
// the Phase G3 addition: it locks the live mid-render param-update path on a
// SOUNDING voice. NOTE: Synth::newParamValueFromExternal (the listener
// setNewValueFromMidi propagates to) has switch cases for matrix rows
// (ENCODER_MATRIX_MUL writes rows[r].mul, read live every block by
// Matrix::computeAllDestinations), LFO rows (ENCODER_LFO_FREQ writes lfo->freq,
// read live), effect rows, etc. — but NO case for ROW_OSC1..6 or ROW_ENGINE. So
// a PARAM_CHANGE can only move the render live if its (row, encoder) has a case;
// changing osc frequency or engine algo mid-note does NOT take effect on a
// sounding voice (the voice reads those at note allocation). Route live
// pitch/env changes THROUGH THE MATRIX (destination OSC1_FREQ / ALL_ENV_DECAY)
// instead — which is exactly the setMatrixSource path the golden-master plan
// targets. See spec-golden-master-phase-g2-g3.md Design Notes.
enum class RenderEventKind { NOTE_ON, NOTE_OFF, PARAM_CHANGE, MIDI_BYTE };

// A single event in a RenderScript, applied at a deterministic block offset.
// The note payload (note/velocity) is valid for NOTE_ON/NOTE_OFF; the param
// payload (row/encoder/value) is valid for PARAM_CHANGE. Use the static
// factories (noteOn/noteOff/paramChange) — aggregate init across 8 fields is
// error-prone and the factories are the single source of truth for what each
// fixture locks.
struct RenderEvent {
    std::size_t     blockOffset;   // fires immediately before this block's render
    RenderEventKind kind;
    int             timbre;
    char            note;          // NOTE_ON / NOTE_OFF payload
    char            velocity;      // NOTE_ON payload (NOTE_OFF ignores it)
    int             row;           // PARAM_CHANGE payload (Synth param-grid row)
    int             encoder;       // PARAM_CHANGE payload (row sub-encoder)
    float           value;         // PARAM_CHANGE payload (new float value)
    unsigned char   byte;          // MIDI_BYTE payload (raw MIDI byte, e.g. 0xF8)

    static RenderEvent noteOn(std::size_t blk, int timbre, char note, char vel) {
        return { blk, RenderEventKind::NOTE_ON, timbre, note, vel, 0, 0, 0.0f, 0 };
    }
    static RenderEvent noteOff(std::size_t blk, int timbre, char note) {
        return { blk, RenderEventKind::NOTE_OFF, timbre, note, 0, 0, 0, 0.0f, 0 };
    }
    static RenderEvent paramChange(std::size_t blk, int timbre,
                                   int row, int encoder, float value) {
        return { blk, RenderEventKind::PARAM_CHANGE, timbre, 0, 0, row, encoder, value, 0 };
    }
    // Phase G4: feed one raw MIDI byte through the REAL MidiDecoder (newByte)
    // at a block offset — the production clock-byte path. 0xFA=MIDI_START,
    // 0xFB=MIDI_CONTINUE, 0xF8=MIDI_CLOCK (every 6th advances the song
    // position), 0xFC=MIDI_STOP. Used by the sequencer external-clock golden;
    // the arpeggiator golden needs no MIDI bytes (its clock is internal).
    static RenderEvent midiByte(std::size_t blk, unsigned char b) {
        return { blk, RenderEventKind::MIDI_BYTE, 0, 0, 0, 0, 0, 0.0f, b };
    }
};

// A scripted event sequence. renderScript() applies events in their LISTED ORDER
// as each block offset is reached, so factories MUST emit events pre-sorted by
// blockOffset (stable original order disambiguates same-offset events, e.g. the
// two noteOns in multiTimbreMix() at block 0, or a PARAM_CHANGE + noteOn at the
// same block). The named factories below are the SINGLE source of truth for the
// sequences the committed fixtures lock — changing one invalidates its fixture
// (regenerate via PFM3_REGENERATE_GOLDENS=1; see tests/golden/README.md).
// Phase G1 note: the two FM-algo goldens (fm_algo2, fm_algo27_6carrier) reuse
// a4Sustain()'s events — the FM algorithm is set out-of-band via setTimbreAlgo()
// (a per-timbre state change, not a note event), so the script alone does not
// describe those fixtures; the TEST pairs setTimbreAlgo + renderScript.
// Phase G3 note: the three live-modulation goldens additionally fire
// PARAM_CHANGE events mid-render; their matrix routing is set out-of-band via
// setMatrixRow() before render (also a per-timbre state change), so — like the
// FM goldens — the script + the TEST's setMatrixRow call together describe the
// fixture.
struct RenderScript {
    std::vector<RenderEvent> events;
    static RenderScript a4Sustain();                  // G0: noteOn(0,69,100)@0; no note-off.
    static RenderScript envelopeAdsrFull();           // G1: noteOn(0,69,100)@0 + noteOff(0,69)@300.
    static RenderScript multiTimbreMix();             // G1: noteOn(0,69,100)@0 + noteOn(1,72,100)@0.
    static RenderScript liveLfoPitchModulation();     // G3: noteOn@0; LFO1->OSC1_FREQ set out-of-band; render steady (LFO auto-modulates).
    static RenderScript liveMatrixMulChange();        // G3: noteOn@0; PARAM_CHANGE(ROW_MATRIX8,ENCODER_MATRIX_MUL,0.6)@blk80 turns the LFO1->OSC1_FREQ routing on mid-note (row 8 = the row setMatrixRow targets).
    static RenderScript liveLfoFreqChange();        // G3: noteOn@0; PARAM_CHANGE(ROW_LFOOSC1,ENCODER_LFO_FREQ,9.0)@blk100 doubles the LFO rate driving LFO1->MIX_OSC1 (tremolo).
    static RenderScript arpTriadUp();                // G4: C-major triad (60/64/67) held; arp (enabled out-of-band via enableArpeggiator, internal clock, UP, 2 oct) cycles the notes across blocks.
    static RenderScript seqExternalPlayback();        // G4: 0xFA@0 + bursts of 4x0xF8@1..96 (384 clocks = 1 bar) drive the sequencer external-MIDI-clock path; step notes (C4/E4/G4) set out-of-band via setupSequencerTriadPlayback fire via noteOnFromSequencer.
};

// Per-timbre voice allocation at Synth::init time. 0 silences a timbre
// (numberOfVoices==0 short-circuits Synth::noteOn / Timbre::preenNoteOn). The
// sum across timbres must be <= MAX_NUMBER_OF_VOICES (16); rebuidVoiceAllTimbre
// (Synth.cpp:824) statically partitions the global pool in timbre order. Every
// timbre's scaleFrequencies table is populated by the harness setup loop
// regardless of voice count, so enabling timbre 1 only requires voices>0 here.
struct TimbreSetup {
    int voices[NUMBER_OF_TIMBRES];
    static TimbreSetup g0Default();    // {6,0,0,0,0,0} — single-timbre (G0).
    static TimbreSetup multiTimbre();  // {6,6,0,0,0,0} — timbres 0+1 (sum 12 <= 16).
};

class GoldenHarness {
public:
    static constexpr std::size_t kBuffersPerBlock  = 3;
    static constexpr std::size_t kFramesPerBlock   = 32;   // BLOCK_SIZE
    static constexpr std::size_t kSamplesPerBuffer = kFramesPerBlock * 2;  // stereo
    static constexpr std::size_t kSamplesPerBlock  =
        kSamplesPerBuffer * kBuffersPerBlock;   // 192 int32 per render block

    // The render's final DAC formatting is `int32 = (24-bit clamped sample) << 8`
    // (Synth.cpp clamps to ±0x7FFFFF then `<<= 8`), so 1 audio-LSB = 256 in the
    // stored int32 units. The compare tolerance is therefore 256 to absorb a
    // genuine ±1 audio-LSB drift (the meaningful signal tolerance). The hash
    // tolerance stays at 1 (granularity of the diagnostic fingerprint) and is
    // DECOUPLED from the compare tolerance so the committed .xxh (a tol-1 hash)
    // stays valid when the compare tolerance changes. The hash is diagnostic-
    // only (see compareAgainstFixture); goldenCompare is the authoritative gate.
    static constexpr int kCompareLsbTolerance = 256;  // ±1 audio-LSB in stored int32 units
    static constexpr int kHashLsbTolerance   = 1;     // fingerprint granularity (diagnostic only)

    // fixtureDir: absolute or cwd-relative path to tests/golden/ (passed in so
    //             file I/O is cwd-independent under ctest).
    // setup:      per-timbre voice counts (default = G0 single-timbre). Passing
    //             TimbreSetup::multiTimbre() enables timbre 1 for the
    //             multi_timbre_mix golden; everything else uses the default.
    explicit GoldenHarness(std::string fixtureDir,
                           TimbreSetup setup = TimbreSetup::g0Default());
    ~GoldenHarness();

    // Access the wired Synth (for noteOn/noteOff between render calls).
    Synth& synth() { return *synth_; }

    // Override a timbre's FM algorithm AFTER construction (which ran Synth::init
    // / preset copy) and BEFORE the first render. Writes params_.engine1.algo
    // (a float; Common.h:282) — read LIVE every block by Voice/Env via the
    // pointer wired in Timbre::init (Timbre.cpp:283-288), so the field change
    // alone takes effect — then calls the production-faithful re-init
    // synth_->afterNewParamsLoad(timbre) (Synth.h:95; idempotent; re-applies env
    // curves/matrix/FX). Call BEFORE noteOn so voice-allocation arithmetic
    // (Timbre.cpp:686) sees the new algo. The default preset is ALGO1, so the
    // G0 + multi-timbre goldens never call this.
    void setTimbreAlgo(int timbre, Algorithm algo);

    // Override the timbre's modulation indices + FM feedback AFTER construction
    // and BEFORE the first noteOn (Phase 3 / spec-test-coverage-phase3). The
    // default preenMainPreset has ALL-ZERO modulation indices, so under it only
    // the carrier COUNT distinguishes algorithms (tests/golden/README.md -> G1
    // algorithm note). This helper closes that gap: it writes
    // params_.engineIm1.modulationIndex1/2 + params_.engineIm2.modulationIndex3/4
    // (Common.h:328/337 EngineIm1/EngineIm2) and the FM feedback,
    // params_.engineIm3.modulationIndex6 (Common.h:347 — there is NO separate
    // "engine1.feedback" field; osc self-feedback IS modulationIndex6, range
    // [0,1] vs [0,16] for the others, Voice.h:125-132 updateAllModulationIndexes
    // clamps it). The voice copies these into its per-voice modulationIndex1..4/
    // feedbackModulation via updateAllModulationIndexes (Voice.h:122), which is
    // re-run EVERY BLOCK from the per-block matrix update (call site Voice.h:350)
    // — so the field write is read live, exactly like engine1.algo.
    // afterNewParamsLoad then re-runs the production re-init (resetting the
    // matrix sources/destinations caches via Voice::afterNewParamsLoad) so the
    // patch is in effect from block 0. Call BEFORE noteOn per the setTimbreAlgo
    // precedent (kept uniform even though the read is live). Keep values moderate (0.5-2.0 IMs,
    // feedback <= 1) — huge indices flatten against the DAC clamp and the
    // fixture stops distinguishing topology. modulationIndex5 is left at its
    // preset default (some deep algos read it; im[4] deliberately covers only
    // IM1..IM4, matching the spec).
    // Override the timbre's FM modulation indices (+ fast-attack modulator
    // envelopes) AFTER construction and BEFORE the first render (Phase 3).
    // Writes engineIm1/2 + engineIm3.modulationIndex6 (feedback), then — see
    // the .cpp — ALSO fast-attacks env3..env6 so the modulators are audible
    // from block 0. Use all-distinct im[] values: routing pairs that differ
    // only by an IM swap render identically under equal indices.
    void setTimbreModulationIndices(int timbre, const float im[4], float feedback);

    // Override the timbre's per-voice FX filter AFTER construction and BEFORE
    // the first render (Phase 3). Writes params_.effect1.{type,param1,param2,
    // param3} (Common.h EffectRowParams — Common.h:545 in OneSynthParams), then
    // calls afterNewParamsLoad. TWO read paths make this work: (1) the type/
    // params are read LIVE every block by Voice::fxAfterBlock (Voice.cpp:4124
    // `int effectType = currentTimbre->params_.effect1.type;` and every case's
    // `params_.effect1.param1/param2`), so the patch alone reaches the
    // sounding-path FX; AND (2) afterNewParamsLoad DOES reach Voice::setNewEffect-
    // Param (Synth.cpp:569 -> Timbre::afterNewParamsLoad, Timbre.cpp:2603-2605
    // loops setNewEffecParam(k) -> Voice::setNewEffectParam, Voice.cpp:7897)
    // which resets per-type filter state (fxParamA1/A2/B2, v0L..v8R, BP
    // recompute markers) — needed so the filter state matches a voice that
    // STARTED with this FX rather than carrying default-preset leftovers.
    // param3 is the FX gain (fxAfterBlock Voice.cpp:4125 clamp(param3,0,16) ->
    // mixerGain; 0 would attenuate the output to silence) — defaults to 0.6
    // (the value SynthState.cpp:723 random preset gen also uses).
    void setTimbreFx(int timbre, int type, float param1, float param2,
                     float param3 = 0.6f);

    // Override the TIMBRE-level FX bus AFTER construction and BEFORE the first
    // render (Phase 3 coverage follow-up). This is a DIFFERENT surface from
    // setTimbreFx: effect1 is the per-VOICE FILTER_TYPE chain (Voice::
    // fxAfterBlock); effect2 is the per-TIMBRE FILTER2_TYPE bus (Timbre::
    // fxAfterBlock, Timbre.cpp:751+) — flange/dimension/chorus/wide/doubler/
    // tripler/bode/delaycrunch/pingpong/diffuser/grain1/grain2 — using
    // Timbre::delayBuffer_. Writes params_.effect2.{type,param1,param2,param3}
    // (Common.h EffectRowParams). NO afterNewParamsLoad is needed here —
    // VERIFIED read paths: (1) Timbre::fxAfterBlock reads effect2.{type,param1,
    // param2} LIVE each block (Timbre.cpp:752-753 and per-case uses); (2) the
    // type-change detection `prevFx2Type != fx2Type` (Timbre.cpp:764-771)
    // zeroes mixerGain_/feedback/delayBuffer_ on the first block with the new
    // type, replacing the state-reset role Voice::setNewEffectParam plays for
    // effect1; (3) param3 drives the wet-path gain via
    // gainTmp = clamp(param3 + matrixFilterAmp, 0, 16) -> mixerGain_
    // (Timbre.cpp:762; param3=0 would ramp the wet gain to silence). The call
    // site is Synth::buildNewSampleBlock -> Timbre::fxAfterBlock
    // (Synth.cpp:354), unconditionally every block per timbre.
    void setTimbreFx2(int timbre, int type, float param1, float param2,
                      float param3);

    // Override a single modulation-matrix row AFTER construction and BEFORE the
    // first render (Phase G3). Writes params_.matrixRowState{rowIdx}
    // {source, mul, dest1, dest2} (Common.h:491/574) — the rows are bound to the
    // runtime Matrix via &matrixRowState1 (Matrix::init, Matrix.h:31) and read
    // LIVE every block by Matrix::computeAllDestinations (Matrix.h:85-118,
    // `destination += sources[source] * mul`, skipping source==NONE or mul==0
    // rows) — then calls synth_->afterNewParamsLoad(timbre) (idempotent;
    // Voice::afterNewParamsLoad resets the runtime sources/destinations caches
    // which recompute from the patched row fields next block; it does NOT
    // clobber the row fields). Same proven pattern as setTimbreAlgo. Call BEFORE
    // noteOn. The default preset ships 12 rows (several active, e.g. row 1
    // MODWHEEL->INDEX_ALL_MODULATION, row 2 LFO1->PAN_OSC2@0.5); overwriting an
    // inactive row (row 8 is {LFO1,0,INDEX_MODULATION2,0}) adds a routing
    // without disturbing the active defaults. The LFO1 source is auto-injected
    // per-block by Voice (lfoOsc[0].nextValueInMatrix); the default preset's
    // lfoOsc1 is {LFO_SIN, 4.5, 0, 0} (active), so MATRIX_SOURCE_LFO1 produces a
    // modulating value with no MIDI input — sources needing MIDI (MODWHEEL,
    // PITCHBEND, CC1-4) resolve to 0 and would be a no-op trap.
    void setMatrixRow(int timbre, int rowIdx, int source, float mul,
                      int dest1, int dest2 = DESTINATION_NONE);

    // Enable the arpeggiator on `timbre` with INTERNAL clock (Phase G4). Fires
    // setNewValueFromMidi on ROW_ARPEGGIATOR1 for BPM, CLOCK (CLOCK_INTERNAL=1),
    // DIRECTION (e.g. ARPEGGIO_DIRECTION_UP=0), and OCTAVE (range). The arp's
    // internal clock is a pure sample-block counter (Timbre::updateArpegiator-
    // InternalClock, advanced inside buildNewSampleBlock) — NO MIDI clock / HAL
    // shim needed (verified: zero HAL_GetTick in the arp path). Call BEFORE
    // noteOn so the note stack builds under an armed arp. The stub's permissive
    // ParameterRowDisplay for ROW_ARPEGGIATOR1 (midi_decoder_collaborators_
    // stub.cpp) lets the values through — without it clock clamps to 0 and the
    // arp stays disabled (zero-signal trap; see spec Design Notes).
    void enableArpeggiator(int timbre, int bpm, int direction, int octave);

    // Load a minimal triad sequence (C4/E4/G4 = MIDI 60/64/67 at step indices
    // 0/32/64) into the sequencer's stepNotes[0], activate step-seq 0, and arm
    // playback (Phase G4 seq-external golden). stepActivated_ is private with
    // no public setter (set only by the recording path stepRecordNotes); the
    // FAITHFUL activation route is setFullState, which reads stepActivated_[]
    // from the last NUMBER_OF_STEP_SEQUENCES bytes of the getFullState buffer
    // (Sequencer.cpp getFullState writes them last). stepNotes (populated
    // directly via stepGetSequence) are NOT in the buffer, so setFullState does
    // not clobber them. Call BEFORE render; the TEST feeds 0xFA + 0xF8 via
    // MIDI_BYTE events. Verified by a throwaway smoke test (subagent probe:
    // max|sample|=254M stored units across 96 blocks; removing stepActivated
    // -> silent, proving the gate).
    void setupSequencerTriadPlayback();

    // Access the wired MidiDecoder (Phase G4). Owns its own decoder so MIDI_BYTE
    // events can drive newByte without going through a MidiController. The
    // decoder is wired to synth_ + a null VisualInfo in the ctor.
    MidiDecoder& midiDecoder() { return *midiDecoder_; }

    // Generic render: apply `script`'s events by block offset, rendering
    // `nBlocks` blocks into `out` (must hold nBlocks*kSamplesPerBlock int32_t).
    // buildNewSampleBlock zeroes the 3 buffers itself (Synth.cpp:325-333).
    // Output layout per block: [b1(64) b2(64) b3(64)].
    void renderScript(const RenderScript& script, std::size_t nBlocks, int32_t* out);

    // Thin delegator retained so G0's GoldenMaster.A4DefaultSustain200Blocks
    // compiles unchanged. Equivalent to renderScript(RenderScript::a4Sustain(), …).
    void renderA4DefaultSustain(std::size_t nBlocks, int32_t* out);

    // Compare `actual` against the committed fixture `id`. Returns true on
    // match — goldenCompare (±1 audio-LSB = ±256 stored units) is AUTHORITATIVE.
    // The committed .xxh hash is also computed and compared as a DIAGNOSTIC
    // only (printed to stderr on mismatch; never fails the test). On a
    // sample-level mismatch fills *diffOut with the first offending sample.
    bool compareAgainstFixture(const std::string& id, const int32_t* actual,
                               std::size_t nBlocks, GoldenDiff* diffOut);

    // Regeneration: write .bin + .xxh + .diff.txt for `actual`. Returns the
    // computed hash (also printed by the caller). No comparison is performed.
    uint64_t writeFixture(const std::string& id, const int32_t* actual,
                          std::size_t nBlocks);

private:
    void setUpSynthState();   // the memset + field-patch sequence

    std::string fixtureDir_;
    TimbreSetup setup_;
    SynthStateBacking ssBacking_;
    ScaleFreqTables scaleFreqs_;
    SynthState* ss_;
    SynthBacking synthBacking_;
    Synth* synth_;   // placement-new'd into synthBacking_ in the ctor
    // Phase G4: a real MidiDecoder wired to synth_ + a null VisualInfo, so
    // MIDI_BYTE events drive the production clock-byte path (newByte ->
    // synth->midiTick/midiClockStart -> sequencer). The arpeggiator golden
    // doesn't feed MIDI bytes but the decoder is wired unconditionally (cheap;
    // keeps the harness model uniform + ready for the seq golden). Placement-
    // new'd into a ZEROED backing (MidiDecoderBacking) — the ctor inits most
    // members but NOT songPosition; zeroing covers it (step-04 review).
    MidiDecoderBacking midiDecoderBacking_;
    MidiDecoder* midiDecoder_;
    NullVisualInfo visualInfo_;
    // Phase G4: a real Sequencer + FMDisplaySequencer, wired bidirectionally
    // (synth.setSequencer / seq.setSynth / seq.setDisplaySequencer — the
    // preenfm3.cpp:417-420 wiring). Needed only by the seq-external golden but
    // wired unconditionally: render-neutral for G0-G3/arp (they never feed
    // clock bytes, so onMidiClock never fires + synthMode=MIXER means noteOn
    // doesn't route to the sequencer). displaySeq_'s refreshStatusP_ is pointed
    // at the dummy ints below (refresh() derefs it unconditionally; the stub
    // ctor at sequencer_collaborators_stub.cpp doesn't init it). displaySeq_'s
    // ctor is itself a test stub (FMDisplaySequencer.cpp is off-host).
    SequencerBacking seqBacking_;
    Sequencer* seq_;   // placement-new'd into seqBacking_ in the ctor
    FMDisplaySequencerBacking dispSeqBacking_;
    FMDisplaySequencer* displaySeq_;   // placement-new'd into dispSeqBacking_
    int dummyRefreshStatusA_ = 0;
    int dummyRefreshStatusB_ = 0;
};

}  // namespace golden
