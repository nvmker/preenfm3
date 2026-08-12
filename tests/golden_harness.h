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

// A single note event in a RenderScript, applied at a deterministic block
// offset. isNoteOn=true  -> Synth::noteOn(timbre, note, velocity);
// isNoteOn=false         -> Synth::noteOff(timbre, note)   (velocity ignored).
// Velocity is kept on the noteOff event too only so a single aggregate-init
// literal reads naturally; renderScript() does not forward it.
struct RenderEvent {
    std::size_t blockOffset;   // fires immediately before this block's render
    bool        isNoteOn;
    int         timbre;
    char        note;
    char        velocity;
};

// A scripted note sequence. renderScript() applies events in their LISTED ORDER
// as each block offset is reached, so factories MUST emit events pre-sorted by
// blockOffset (stable original order disambiguates same-offset events, e.g. the
// two noteOns in multiTimbreMix() at block 0). The named factories below are the
// SINGLE source of truth for the sequences the committed fixtures lock —
// changing one invalidates its fixture (regenerate via
// PFM3_REGENERATE_GOLDENS=1; see tests/golden/README.md). Phase G1 note: the two
// FM-algo goldens (fm_algo2, fm_algo17_6op) reuse a4Sustain()'s events — the FM
// algorithm is set out-of-band via setTimbreAlgo() (it is a per-timbre state
// change, not a note event), so the script alone does not describe those
// fixtures; the TEST pairs setTimbreAlgo + renderScript.
struct RenderScript {
    std::vector<RenderEvent> events;
    static RenderScript a4Sustain();         // noteOn(0,69,100)@0; no note-off.
    static RenderScript envelopeAdsrFull();  // noteOn(0,69,100)@0 + noteOff(0,69)@300.
    static RenderScript multiTimbreMix();    // noteOn(0,69,100)@0 + noteOn(1,72,100)@0.
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
};

}  // namespace golden
