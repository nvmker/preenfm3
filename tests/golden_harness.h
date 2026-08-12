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
    explicit GoldenHarness(std::string fixtureDir);
    ~GoldenHarness();

    // Access the wired Synth (for noteOn/noteOff between render calls).
    Synth& synth() { return *synth_; }

    // Render `nBlocks` blocks into `out`, which must hold
    // nBlocks * kSamplesPerBlock int32_t. The canonical G0 script is applied:
    // noteOn(0, 69, 100) before block 0; no note-off (sustain plateau). Output
    // layout per block: [b1(64) b2(64) b3(64)].
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
    SynthStateBacking ssBacking_;
    ScaleFreqTables scaleFreqs_;
    SynthState* ss_;
    SynthBacking synthBacking_;
    Synth* synth_;   // placement-new'd into synthBacking_ in the ctor
};

}  // namespace golden
