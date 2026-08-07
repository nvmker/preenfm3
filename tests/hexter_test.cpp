// Host-side coverage for firmware/Src/utils/Hexter.cpp — DX7 sysex import.
//
// Regression target (per tests/README.md roadmap, row 2):
//   Crash / corruption on malformed DX7 sysex. Hexter is the entry point for
//   importing packed DX7 patches: loadHexterPatch -> patchUnpack (128-byte
//   packed -> 155-byte unpacked, a hand-rolled byte/bit pointer walk) ->
//   voiceSetData (unpacks into the OneSynthParams POD). There is no committed
//   sysex corpus and no external spec to derive from, so this is a
//   CHARACTERIZATION suite: it locks the firmware's CURRENT behavior as golden
//   so any future silent regression fails loudly, and — the highest-value part
//   — it proves the malformed-input paths don't crash or corrupt under the
//   AddressSanitizer build.
//
// Fidelity caveats (tests/SEAM.md §d.2):
//   * voiceSetData's fixed-frequency oscillator branch calls
//     exp(M_LN10 * ...); getChangeTime calls pow(2, ...). The firmware links
//     newlib (Arm), these tests link glibc (host) — results agree to ~1 ULP,
//     not bit-for-bit. Those paths are asserted with EXPECT_NEAR + tolerance;
//     pure-int / table-derived helpers stay exact-equality.
//   * Hexter compiles verbatim on host — NO source guards and NO stubs were
//     needed (it is pure logic over the OneSynthParams POD). The only external
//     symbol voiceSetData references is `defaultPreset`, supplied by pulling
//     the real firmware/Src/synth/Presets.cpp into the link (see
//     tests/CMakeLists.txt) rather than a zero-stub, so the golden reflects
//     actual firmware defaults.
//
// KNOWN LATENT BUG preserved as golden (do NOT fix here — flagged for a
// separate change; see HexterTransposeDeadBranch suite + tests/SEAM.md):
//   In voiceSetData, `else if (transpose < -18)` is dead — it follows
//   `if (transpose < -6)`, so any value < -18 already took the first branch.
//   transposeMultiply therefore never reaches 0.25f. This suite asserts the
//   CURRENT (0.5f-only) behavior.
//
// FIRMWARE FIX GUARDED HERE (was Target #2's crash-class finding; now fixed).
//   voiceSetData (firmware/Src/utils/Hexter.cpp:932) used to read the LFO-AMD
//   table with an UNBOUNDED index:
//     dx7_voice_amd_to_ol_adjustment[(patch[140])] / 100.0f;
//   patch[140] is copied raw from packed[115] by patchUnpack's "lamd" loop, so
//   any patch with packed[115] >= 100 read past the 100-entry global. Under
//   ASAN, packed[115]==125 aborted with global-buffer-overflow (full trace in
//   the HexterUnboundedIndex suite). The fix wraps the index in limit(…,0,99),
//   matching every other table access in voiceSetData:
//     dx7_voice_amd_to_ol_adjustment[limit(patch[140], 0, 99)] / 100.0f;
//   The malformed-input tests below now feed genuinely out-of-range LFO-AMD
//   bytes (125, 255) through the FULL pipeline to guard the clamp end-to-end,
//   and the HexterUnboundedIndex suite guards it at the unit level. NOTE the
//   read's result is STILL immediately discarded (matrixRowState2.mul is reset
//   to 0.0f two statements later) — a pre-existing dead-store, intentionally
//   left untouched by the fix (out of scope; the bug was the OOB read, not the
//   discard). History + captured ASAN trace in tests/SEAM.md "Target #2 contact
//   with the code".

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#include "Hexter.h"      // firmware-under-test (host-compilable via PFM3_HOST seam)
#include "SynthState.h"  // struct OneSynthParams + ALG* / destination enums

namespace {

// Hexter's interesting helpers (patchUnpack, voiceSetData, setIM,
// getPreenFMIM, getActualLevel, ...) are `protected`. Rather than touch the
// firmware class interface (the seam forbids logic changes), we expose them
// through a test-only subclass with `using` declarations. This is the standard
// GoogleTest pattern for protected members and adds ZERO firmware surface.
class TestHexter : public Hexter {
public:
    using Hexter::Hexter;
    using Hexter::patchUnpack;
    using Hexter::voiceSetData;
    using Hexter::bulkDumpChecksum;
    using Hexter::getPreenFMIM;
    using Hexter::getActualLevel;
    using Hexter::getActualOutputLevel;
    using Hexter::getChangeTime;
    using Hexter::limit;
    using Hexter::voiceCopyName;
};

// DX7 packed patch is exactly 128 bytes; unpacked is 155.
constexpr int kPackedSize = 128;
constexpr int kUnpackedSize = 155;

// Deterministic synthetic packed patch. There is no committed sysex corpus, so
// we synthesize input that exercises distinct bit patterns per byte. The exact
// pattern is arbitrary but FIXED: the golden output below was captured by
// running the real firmware code path on the host build, then locked. Any
// future change to patchUnpack/voiceSetData that alters the output fails here.
//
// Formula: byte[i] = (i * 7 + 3) & 0x7F  — cycles through all 7 bit patterns of
// the low 3 bits and spreads values across the full 0..127 range.
void MakeRepresentativePatch(uint8_t out[kPackedSize]) {
    for (int i = 0; i < kPackedSize; i++) {
        out[i] = static_cast<uint8_t>((i * 7 + 3) & 0x7F);
    }
}

// Run the full public import pipeline on a packed patch, returning the imported
// params by value. Zeroes params first so only the firmware writes are visible.
struct OneSynthParams ImportPatch(TestHexter* h, const uint8_t packed[kPackedSize]) {
    struct OneSynthParams params;
    std::memset(&params, 0, sizeof(params));
    // loadHexterPatch copies packed into its internal unpackedData then imports;
    // pass a mutable copy in case the firmware ever mutates the input (it does
    // not today, but the contract is not documented).
    uint8_t buf[kPackedSize];
    std::memcpy(buf, packed, kPackedSize);
    h->loadHexterPatch(buf, &params);
    return params;
}

// Tolerance for libm-sensitive floats (exp/pow paths): firmware newlib vs host
// glibc agree to ~1 ULP; 1e-3 is generous for the value magnitudes here (env
// times up to ~16s) while still catching real regressions.
constexpr float kLibmTol = 1e-3f;
// Tolerance for pure float arithmetic (no libm): deterministic across libm
// versions; tiny slack only for host FPU rounding mode differences.
constexpr float kArithTol = 1e-5f;

}  // namespace

// ===========================================================================
// Pure helpers — exact-equality goldens (table-stakes).
// ===========================================================================

TEST(HexterPureHelpers, BulkDumpChecksumImplementsDx7SumComplement) {
    // DX7 bulk checksum: sum of bytes + checksum == 0 (mod 128). The firmware
    // computes it as `(-(sum)) & 0x7F`. Verify the algebraic invariant on a few
    // hand-checked inputs AND that it only reads `length` bytes (it is the ONE
    // Hexter function that takes a length — see HexterMalformedInput suite).
    TestHexter h;
    uint8_t data[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

    // length 0: sum 0 -> checksum 0.
    EXPECT_EQ(h.bulkDumpChecksum(data, 0), 0);

    // length 1: sum 0 -> -0 & 0x7F = 0.
    EXPECT_EQ(h.bulkDumpChecksum(data, 1), 0);

    // length 10: sum(0..9)=45 -> (-45)&0x7F. -45 mod 128 = 83.
    EXPECT_EQ(h.bulkDumpChecksum(data, 10), 83);
    EXPECT_EQ((45 + h.bulkDumpChecksum(data, 10)) & 0x7F, 0)
        << "checksum must make (sum + checksum) == 0 mod 128";

    // All-0x7F: sum = 16*0x7F = 2032. checksum = (-2032) & 0x7F.
    // 2032 mod 128 = 112; -112 mod 128 = 16. Verify via the invariant too.
    uint8_t sevens[16];
    std::memset(sevens, 0x7F, sizeof(sevens));
    const int csAll = h.bulkDumpChecksum(sevens, 16);
    EXPECT_EQ(csAll, 16);
    EXPECT_EQ((2032 + csAll) & 0x7F, 0);
}

TEST(HexterPureHelpers, LimitClampsBelowWithinAndAbove) {
    TestHexter h;
    EXPECT_EQ(h.limit(-5, 0, 10), 0) << "below min clamps to min";
    EXPECT_EQ(h.limit(0, 0, 10), 0);
    EXPECT_EQ(h.limit(5, 0, 10), 5) << "within range is unchanged";
    EXPECT_EQ(h.limit(10, 0, 10), 10);
    EXPECT_EQ(h.limit(15, 0, 10), 10) << "above max clamps to max";
    // min == max (degenerate but must not crash / loop).
    EXPECT_EQ(h.limit(7, 5, 5), 5);
}

TEST(HexterPureHelpers, GetPreenFMIMMatchesPiecewiseTiersExactly) {
    // getPreenFMIM is a pure piecewise map with tier boundaries at
    // 50/60/70/80/85/90 and a >100 clamp to 100. Boundaries are inclusive of the
    // LOWER tier (e.g. 50 uses the <60 branch). Pure arithmetic -> exact.
    TestHexter h;
    struct Case { int lvl; float want; };
    const Case cases[] = {
        {0,   0.00f},                       // <50: 0 + lvl*0.006
        {49,  0.294f},                      // 49*0.006
        {50,  0.30f},                       // <60: 0.3 + (50-50)*0.02
        {59,  0.48f},                       // 0.3 + 9*0.02
        {60,  0.50f},                       // <70: 0.5 + 0
        {69,  1.13f},                       // 0.5 + 9*0.07
        {70,  1.20f},                       // <80: 1.2 + 0
        {79,  2.10f},                       // 1.2 + 9*0.1
        {80,  2.20f},                       // <85
        {84,  3.00f},                       // 2.2 + 4*0.2
        {85,  3.20f},                       // <90
        {89,  4.20f},                       // 3.2 + 4*0.25
        {90,  3.95f},                       // else: 3.95 + 0
        {99,  7.10f},                       // 3.95 + 9*0.35
        {100, 7.45f},                       // 3.95 + 10*0.35 (last reachable)
        {150, 7.45f},                       // clamped to 100 internally
        {9999, 7.45f},                      // clamped
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(::testing::PrintToString(c.lvl));
        EXPECT_FLOAT_EQ(h.getPreenFMIM(c.lvl), c.want);
    }
}

TEST(HexterPureHelpers, GetActualLevelMatchesDocumentedDx7EnvelopeMap) {
    // Maps DX7 level 0..99 to an "actual" level per the documented piecewise
    // (https://code.google.com/p/music-synthesizer-for-android/wiki/Dx7Envelope):
    //   <5  -> 2*l,  <16 -> 5+l,  <20 -> 4+l,  else -> 14 + (l>>1).
    // Pure int arithmetic -> exact. We also probe >99 to document that the
    // function does NOT clamp (callers must): it keeps applying the last branch.
    TestHexter h;
    struct Case { int v; int want; };
    const Case cases[] = {
        {0,   0},    // 2*0
        {4,   8},    // 2*4
        {5,   10},   // 5+5  (boundary into 2nd tier)
        {15,  20},   // 5+15
        {16,  20},   // 4+16 (boundary into 3rd tier)
        {19,  23},   // 4+19
        {20,  24},   // 14 + 10 (4th tier)
        {21,  24},   // 14 + 10  (21>>1 == 10)
        {50,  39},   // 14 + 25
        {99,  63},   // 14 + 49
        // Out-of-range (caller is expected to limit() first): no crash, keeps
        // the last branch — documented, not clamped.
        {150, 89},   // 14 + 75
        {255, 141},  // 14 + 127
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(::testing::PrintToString(c.v));
        EXPECT_EQ(h.getActualLevel(c.v), c.want);
    }
}

TEST(HexterPureHelpers, GetActualOutputLevelMatchesTableAndLinearTail) {
    // <20 indexes a 20-entry lookup table; >=20 is the linear tail 28 + value.
    TestHexter h;
    EXPECT_EQ(h.getActualOutputLevel(0), 0);
    EXPECT_EQ(h.getActualOutputLevel(1), 5);
    EXPECT_EQ(h.getActualOutputLevel(18), 45);
    EXPECT_EQ(h.getActualOutputLevel(19), 46);   // last table entry
    EXPECT_EQ(h.getActualOutputLevel(20), 48);   // 28+20 (tail begins)
    EXPECT_EQ(h.getActualOutputLevel(21), 49);
    EXPECT_EQ(h.getActualOutputLevel(99), 127);  // 28+99
    // Out-of-range: linear tail keeps growing (no clamp) — callers must limit().
    EXPECT_EQ(h.getActualOutputLevel(150), 178);
    EXPECT_EQ(h.getActualOutputLevel(255), 283);
}

TEST(HexterPureHelpers, VoiceCopyNameRemapsDx7SpecialChars) {
    // DX7 char ROM remaps three special codes and forces everything outside the
    // printable ASCII range to space. Pure -> exact byte-for-byte.
    TestHexter h;
    uint8_t patch[kUnpackedSize] = {};
    // patch[i + 145] feeds name[i] for i in 0..9; name[10] = 0.
    patch[145] = 92;    // yen  -> 'Y'
    patch[146] = 126;   // >>   -> '>'
    patch[147] = 127;   // <<   -> '<'
    patch[148] = 10;    // control char (<32) -> ' '
    patch[149] = 200;   // >127 -> ' '
    patch[150] = 'A';   // printable passthrough
    patch[151] = 'Z';   // printable passthrough (upper bound)
    patch[152] = 32;    // space passthrough (lower bound)
    patch[153] = 0;     // <32 -> ' '
    patch[154] = 'z';   // 122 printable, NOT a special code -> passthrough

    char name[16] = {};
    h.voiceCopyName(name, patch);

    EXPECT_EQ(name[0], 'Y');
    EXPECT_EQ(name[1], '>');
    EXPECT_EQ(name[2], '<');
    EXPECT_EQ(name[3], ' ');
    EXPECT_EQ(name[4], ' ');
    EXPECT_EQ(name[5], 'A');
    EXPECT_EQ(name[6], 'Z');
    EXPECT_EQ(name[7], ' ');
    EXPECT_EQ(name[8], ' ');
    EXPECT_EQ(name[9], 'z');
    EXPECT_EQ(name[10], '\0') << "name must be null-terminated at index 10";
}

// ===========================================================================
// Integration golden — representative packed patch through the full pipeline.
// Characterizes CURRENT behavior; not derived from any spec.
// ===========================================================================

TEST(HexterPipeline, RepresentativePatchProducesLockedGolden) {
    TestHexter h;
    uint8_t packed[kPackedSize];
    MakeRepresentativePatch(packed);
    const OneSynthParams p = ImportPatch(&h, packed);

    // Algorithm: unpacked patch[134]&0x1f -> DX7 algo -> preenAlgo enum.
    // For this input the unpack lands on DX7 algo 5/6 -> preenAlgo = ALG12.
    EXPECT_EQ(p.engine1.algo, ALG12);

    // Oscillators: shape/frequencyType exact; frequencyMul/detune pure float
    // arithmetic (keyboard path, no exp) -> tight tolerance.
    EXPECT_EQ(p.osc1.shape, OSC_SHAPE_SIN);
    EXPECT_EQ(p.osc1.frequencyType, OSC_FT_KEYBOARD);
    EXPECT_NEAR(p.osc1.frequencyMul, 1.0f, kArithTol);
    EXPECT_NEAR(p.osc1.detune, 0.0f, kArithTol);

    EXPECT_EQ(p.osc2.shape, OSC_SHAPE_SIN);
    EXPECT_NEAR(p.osc2.frequencyMul, 7.0f, kArithTol);
    EXPECT_NEAR(p.osc2.detune, -0.0025f, kArithTol);

    EXPECT_EQ(p.osc3.shape, OSC_SHAPE_SIN);
    EXPECT_NEAR(p.osc3.frequencyMul, 1.0f, kArithTol);

    EXPECT_EQ(p.osc4.shape, OSC_SHAPE_SIN);
    EXPECT_NEAR(p.osc4.frequencyMul, 6.5f, kArithTol);
    EXPECT_NEAR(p.osc4.detune, 0.0025f, kArithTol);

    EXPECT_EQ(p.osc5.shape, OSC_SHAPE_SIN);
    EXPECT_NEAR(p.osc5.frequencyMul, 1.0f, kArithTol);

    EXPECT_EQ(p.osc6.shape, OSC_SHAPE_SIN);
    EXPECT_NEAR(p.osc6.frequencyMul, 5.5f, kArithTol);
    EXPECT_NEAR(p.osc6.detune, 0.0075f, kArithTol);

    // Envelope times flow through getChangeTime -> pow(): libm-sensitive.
    EXPECT_NEAR(p.env1Time.attackTime,  0.0011f, kLibmTol);
    EXPECT_NEAR(p.env1Time.decayTime,   0.0000f, kLibmTol);
    EXPECT_NEAR(p.env1Time.sustainTime, 0.0034f, kLibmTol);
    EXPECT_NEAR(p.env1Time.releaseTime, 0.0400f, kLibmTol);  // floored at 0.04
    EXPECT_NEAR(p.env6Time.attackTime,  9.1691f, kLibmTol);
    EXPECT_NEAR(p.env6Time.decayTime,   0.3545f, kLibmTol);
    EXPECT_NEAR(p.env6Time.sustainTime, 0.1994f, kLibmTol);
    EXPECT_NEAR(p.env6Time.releaseTime, 8.0486f, kLibmTol);

    // Modulation index / mix: derived from getPreenFMIM (pure) -> tight tol.
    EXPECT_NEAR(reinterpret_cast<const float*>(&p.engineIm1)[0], 0.085f, kArithTol);
    EXPECT_NEAR(reinterpret_cast<const float*>(&p.engineIm1)[1], 0.765f, kArithTol);
    EXPECT_NEAR(reinterpret_cast<const float*>(&p.engineIm1)[2], 0.840f, kArithTol);
    EXPECT_NEAR(reinterpret_cast<const float*>(&p.engineIm1)[3], 1.960f, kArithTol);
    EXPECT_NEAR(reinterpret_cast<const float*>(&p.engineMix1)[0], 0.570f, kArithTol);
    EXPECT_NEAR(reinterpret_cast<const float*>(&p.engineMix1)[2], 0.750f, kArithTol);

    // LFO: freq is a pure table lookup (dx7_voice_lfo_frequency[]) -> exact.
    EXPECT_FLOAT_EQ(p.lfoOsc1.freq, 3.051722f);
    EXPECT_EQ(p.lfoOsc1.shape, LFO_SIN);
    EXPECT_NEAR(p.lfoOsc1.keybRamp, 1.733333f, kArithTol);
    EXPECT_NEAR(p.lfoOsc1.bias, 0.0f, kArithTol);

    // Matrix row 1: source LFO1, mul = patch[139]/120, dest ALL_OSC_FREQ.
    EXPECT_EQ(p.matrixRowState1.source, MATRIX_SOURCE_LFO1);
    EXPECT_NEAR(p.matrixRowState1.mul, 0.275f, kArithTol);
    EXPECT_EQ(p.matrixRowState1.dest1, ALL_OSC_FREQ);

    // Preset name (10 chars + NUL). The unpack copies the last 11 packed bytes
    // into unpacked[144..154]; voiceCopyName remaps them.
    EXPECT_STREQ(p.presetName, "=DKRY`gnu|");
}

TEST(HexterPipeline, RepresentativePatchImportIsDeterministic) {
    // Two independent imports of the same packed patch must yield bit-identical
    // params. Guards against uninitialized fields / heap-dependent state.
    TestHexter h;
    uint8_t packed[kPackedSize];
    MakeRepresentativePatch(packed);

    const OneSynthParams a = ImportPatch(&h, packed);
    const OneSynthParams b = ImportPatch(&h, packed);

    EXPECT_EQ(std::memcmp(&a, &b, sizeof(a)), 0)
        << "two imports of the same patch diverged";
}

// ===========================================================================
// Malformed-input robustness — THE crash-class guard. These earn the target its
// keep under the ASAN build: degenerate CONTENT must not crash, divide by zero,
// or produce NaN/inf. (Truncation is a separate, documented finding — see
// HexterSpanContract below.)
// ===========================================================================

TEST(HexterMalformedInput, AllZeroPackedPatchImportsWithoutCrash) {
    TestHexter h;
    uint8_t packed[kPackedSize] = {};
    const OneSynthParams p = ImportPatch(&h, packed);

    // Characterize: all-zero unpacks to DX7 algo 1 -> ALG10; transpose=-24
    // (see HexterTransposeDeadBranch) -> transposeMultiply 0.5; osc1 keyboard
    // coarse=0 -> 0.5 base, *0.5 = 0.25. Name all-spaces (zero bytes -> ' ').
    EXPECT_EQ(p.engine1.algo, ALG10);
    EXPECT_NEAR(p.osc1.frequencyMul, 0.25f, kArithTol);
    EXPECT_NEAR(p.osc6.frequencyMul, 0.5f, kArithTol);
    EXPECT_NEAR(p.env1Time.attackTime, 0.0f, kLibmTol);
    EXPECT_EQ(static_cast<unsigned char>(p.presetName[0]), ' ');
    // Sanity: no NaN/inf leaked into the float fields we sample.
    EXPECT_FALSE(std::isnan(p.osc1.frequencyMul));
    EXPECT_FALSE(std::isnan(p.env1Time.attackTime));
}

TEST(HexterMalformedInput, AllOnesPackedPatchImportsWithoutCrash) {
    // all-0xFF: packed[115]=255 -> unpacked[140]=255 -> voiceSetData indexes
    // dx7_voice_amd_to_ol_adjustment[limit(255,0,99)]. Pre-fix this was an OOB
    // (index 255) that skipped ASAN's redzone onto an adjacent global (silent
    // corruption); the Hexter.cpp:932 limit() fix makes it safe. This test now
    // guards that clamp through the FULL pipeline (patchUnpack + voiceSetData).
    TestHexter h;
    uint8_t packed[kPackedSize];
    std::memset(packed, 0xFF, kPackedSize);
    const OneSynthParams p = ImportPatch(&h, packed);

    // 0xFF everywhere: patch[134]&0x1f == 31 -> DX7 algo 32 -> ALG27. Name
    // bytes all 0xFF (>127) -> remapped to space.
    EXPECT_EQ(p.engine1.algo, ALG27);
    EXPECT_NEAR(p.osc1.frequencyMul, 5.5f, kArithTol);
    EXPECT_NEAR(p.osc6.frequencyMul, 5.5f, kArithTol);
    EXPECT_NEAR(p.env1Time.attackTime, 0.0003f, kLibmTol);
    EXPECT_EQ(static_cast<unsigned char>(p.presetName[0]), ' ');
    EXPECT_FALSE(std::isnan(p.osc1.frequencyMul));
    // The amd-table read's result is still discarded by the firmware (mul is
    // reset to 0.0f two statements after line 932) — see HexterUnboundedIndex.
    EXPECT_EQ(p.matrixRowState2.mul, 0.0f);
}

TEST(HexterMalformedInput, StructuredGarbageImportsWithoutCrash) {
    // A second, distinct bit pattern (high bits set, interleaved) to exercise
    // the fixed-frequency osc branch and the algorithm/matrix dispatch under
    // non-trivial content. This generator yields packed[115]=125 -> unpacked
    // [140]=125, the EXACT value that pre-fix aborted the suite under ASAN
    // (global-buffer-overflow at Hexter.cpp:932). With the limit() fix the
    // import completes; this test is the end-to-end ASAN guard for that fix.
    TestHexter h;
    uint8_t packed[kPackedSize];
    for (int i = 0; i < kPackedSize; i++) {
        packed[i] = static_cast<uint8_t>((0xA5 ^ (i * 13 + 1)) & 0x7F);
    }
    const OneSynthParams p = ImportPatch(&h, packed);

    EXPECT_GE(p.engine1.algo, ALGO7);
    EXPECT_LE(p.engine1.algo, ALG28);
    ASSERT_FALSE(std::isnan(p.osc1.frequencyMul));
    ASSERT_FALSE(std::isnan(p.env1Time.attackTime));
    ASSERT_FALSE(std::isnan(p.env1Time.releaseTime));
    ASSERT_FALSE(std::isinf(p.osc1.frequencyMul));
    // Envelope times are bounded to [0.04, 16.0] by voiceSetData.
    EXPECT_GE(p.env1Time.releaseTime, 0.04f - kLibmTol);
    EXPECT_LE(p.env1Time.attackTime, 16.0f + kLibmTol);
    EXPECT_LE(p.env1Time.releaseTime, 16.0f + kLibmTol);
    // amd-table read discarded -> mul is 0.0f (see HexterUnboundedIndex).
    EXPECT_EQ(p.matrixRowState2.mul, 0.0f);
}

TEST(HexterMalformedInput, OutOfRangeEbArgsDoNotCrashGetChangeTime) {
    // In firmware, voiceSetData always wraps getChangeTime args in limit(…,0,99).
    // Here we call it DIRECTLY with out-of-range args to prove the function is
    // itself robust (no domain error, no NaN/inf) — defense-in-depth, and a
    // guard against a future caller forgetting the limit().
    TestHexter h;
    struct Case { int ol; int t; int v1; int v2; };
    const Case cases[] = {
        {0,   0,   0,   0},
        {99,  99,  99,  99},
        {255, 255, 255, 255},  // all out-of-range
        {0,   255, 0,   255},
        {127, 64,  32,  96},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(::testing::PrintToString(c.ol));
        const float r = h.getChangeTime(c.ol, c.t, c.v1, c.v2);
        EXPECT_FALSE(std::isnan(r)) << "getChangeTime produced NaN";
        EXPECT_FALSE(std::isinf(r)) << "getChangeTime produced inf";
        EXPECT_GE(r, 0.0f) << "change time must be non-negative";
    }
}

TEST(HexterMalformedInput, BulkDumpChecksumIsSafeOnEveryTruncatedLength) {
    // Contrast with patchUnpack: bulkDumpChecksum is the ONE Hexter function
    // that takes a length, so truncated inputs are safe — it reads exactly
    // `length` bytes. This runs clean under ASAN for every length 0..128.
    TestHexter h;
    uint8_t data[kPackedSize];
    MakeRepresentativePatch(data);  // any deterministic content
    for (int len = 0; len <= kPackedSize; len++) {
        SCOPED_TRACE(::testing::PrintToString(len));
        const int cs = h.bulkDumpChecksum(data, len);
        EXPECT_GE(cs, 0);
        EXPECT_LE(cs, 0x7F) << "checksum is masked to 7 bits";
        // Invariant: (sum + checksum) mod 128 == 0.
        int sum = 0;
        for (int i = 0; i < len; i++) sum += data[i];
        EXPECT_EQ((sum + cs) & 0x7F, 0);
    }
}

// ===========================================================================
// Span contract — documents the truncation unsafety of patchUnpack /
// voiceSetData / loadHexterPatch (the latent crash class). These functions take
// NO length parameter and unconditionally walk the full 128-byte (packed) /
// 155-byte (unpacked) span. A shorter input is an out-of-bounds read.
//
// We do NOT execute patchUnpack on a genuinely short buffer here: under ASAN
// that read aborts the process (correctly), which would fail the whole suite.
// Instead we PROVE the full-span read contract: packed byte 127 (the last byte)
// is consumed and influences the output. Combined with the known unpack layout
// (all 128 packed bytes are dereferenced by the pointer walk), this makes the
// over-read on any shorter input self-evident. See tests/SEAM.md for the full
// finding and where the firmware guards it (sysex length validation upstream of
// loadHexterPatch, in the midi/SynthState path, before this code is reached).
// ===========================================================================

TEST(HexterSpanContract, PackedByte127InfluencesOutput) {
    // The unpack copies the final 11 packed bytes into unpacked[144..154]; the
    // last of those (unpacked[154]) becomes name[9] via voiceCopyName. So
    // changing packed[127] MUST change presetName[9] — proving the read reaches
    // the very last byte of the 128-byte input.
    TestHexter h;
    uint8_t a[kPackedSize], b[kPackedSize];
    MakeRepresentativePatch(a);
    std::memcpy(b, a, kPackedSize);
    b[127] = static_cast<uint8_t>('X');  // differs only in the last byte

    const OneSynthParams pa = ImportPatch(&h, a);
    const OneSynthParams pb = ImportPatch(&h, b);

    EXPECT_NE(pa.presetName[9], pb.presetName[9])
        << "packed[127] did not influence output — span read contract broken";
}

TEST(HexterSpanContract, PatchUnpackReadsFull128ByteInputUnderAsan) {
    // Direct ASAN proof that patchUnpack reads a FULL 128-byte input without
    // over-reading: feed it a stack buffer of exactly 128 packed bytes (plus an
    // unpacked sink of exactly 155) and assert completion + a known output.
    // If patchUnpack ever over-reads, this test aborts under ASAN.
    TestHexter h;
    uint8_t packed[kPackedSize];
    uint8_t unpacked[kUnpackedSize];
    MakeRepresentativePatch(packed);
    std::memset(unpacked, 0xAA, sizeof(unpacked));

    h.patchUnpack(packed, unpacked);

    // The first unpacked byte is the first packed byte (operator 1 rate 1).
    EXPECT_EQ(unpacked[0], packed[0]);
    // The feedback byte lives at unpacked[135]; algorithm at 134. Just sanity
    // check the write span reached the end (index 154 was written, not 0xAA).
    EXPECT_NE(unpacked[154], 0xAA)
        << "patchUnpack did not write the last unpacked byte";
}

// ===========================================================================
// Target #2's crash-class finding — NOW FIXED in firmware, guarded here.
//
// firmware/Src/utils/Hexter.cpp:932 used to read the LFO-AMD table with an
// UNBOUNDED index:
//   params->matrixRowState2.mul = dx7_voice_amd_to_ol_adjustment[(patch[140])] / 100.0f;
//
//  * dx7_voice_amd_to_ol_adjustment is `const float[100]` (400 bytes) — a
//    file-scope global in Hexter.cpp.
//  * patch[140] is unpacked byte 140, which patchUnpack fills from packed[115]
//    via a raw `*up++ = *pp++` copy (the "lamd" loop) — NO mask.
//  * Every OTHER table access in voiceSetData was already wrapped in
//    limit(…,0,99); this one alone was not.
//
// Any patch whose packed[115] >= 100 read past the 100-entry global. Under the
// ASAN build, the structured-garbage generator hit packed[115]==125 and ASAN
// aborted the process:
//
//   Hexter.cpp:932:32: runtime error: index 125 out of bounds for type 'const float[100]'
//   ==ERROR: AddressSanitizer: global-buffer-overflow ... READ of size 4
//       #0 Hexter::voiceSetData(OneSynthParams*, unsigned char*) Hexter.cpp:932
//       #1 Hexter::loadHexterPatch(unsigned char*, OneSynthParams*)  Hexter.cpp:183
//   ... located 100 bytes after global variable 'dx7_voice_amd_to_ol_adjustment' of size 400
//
// Larger indexes (e.g. 255 from an all-0xFF patch) skipped ASAN's global redzone
// onto an adjacent valid global — ASAN-silent, but still wrong-data UB.
//
// The fix wraps the index in limit(…,0,99), matching every other table access
// in voiceSetData:
//   params->matrixRowState2.mul = dx7_voice_amd_to_ol_adjustment[limit(patch[140], 0, 99)] / 100.0f;
//
// NOTE the read's result is STILL immediately discarded — matrixRowState2.mul
// is reassigned to 0.0f two statements after line 932 — so the line remains a
// dead-store; the fix intentionally does NOT remove it (the bug was the OOB
// read, not the discard; deleting is a separate cleanup decision). Full
// history + the fix in tests/SEAM.md "Target #2 contact with the code".
// ===========================================================================

TEST(HexterUnboundedIndex, AmdTableReadResultIsDiscardedByFirmware) {
    // Unchanged by the fix: line 932 writes matrixRowState2.mul, then a later
    // line overwrites it to 0.0f. Locking the discard as golden means a future
    // change that removes either the dead read OR the overwrite is visible.
    TestHexter h;
    uint8_t u[kUnpackedSize] = {};
    u[140] = 50;  // in-range; the result is discarded regardless
    struct OneSynthParams p;
    std::memset(&p, 0, sizeof(p));
    h.voiceSetData(&p, u);
    EXPECT_EQ(p.matrixRowState2.mul, 0.0f)
        << "amd-table read result should be discarded (overwritten to 0.0f)";
}

TEST(HexterUnboundedIndex, OutOfRangeLfoAmdIndexIsClampedByFirmware) {
    // THE regression guard for the Hexter.cpp:932 limit() fix. Pre-fix this
    // aborted the whole suite under ASAN (index 100/125/255 ->
    // global-buffer-overflow). Post-fix the firmware clamps to [0,99] and the
    // import completes. Drives voiceSetData directly with the exact out-of-range
    // indices ASAN caught (no patchUnpack indirection). The integration-level
    // guard is HexterMalformedInput.{AllOnes,StructuredGarbage} above; the
    // transpose dead-branch below is a SEPARATE finding, still unfixed.
    TestHexter h;
    for (uint8_t oob : {uint8_t(100), uint8_t(125), uint8_t(255)}) {
        SCOPED_TRACE(static_cast<int>(oob));
        uint8_t u[kUnpackedSize] = {};
        u[140] = oob;
        struct OneSynthParams p;
        std::memset(&p, 0, sizeof(p));
        h.voiceSetData(&p, u);  // must not abort under ASAN
        EXPECT_EQ(p.matrixRowState2.mul, 0.0f)
            << "read result still discarded (clamp does not change the discard)";
        EXPECT_FALSE(std::isnan(p.osc1.frequencyMul))
            << "clamp failure would corrupt params";
    }
}

// ===========================================================================
// KNOWN LATENT BUG (preserved as golden — DO NOT fix here).
//
// voiceSetData computes:
//   int transpose = limit(patch[144], 0, 48) - 24;   // range -24..24
//   float transposeMultiply = 1.0f;
//   if (transpose < -6)      transposeMultiply = .5f;
//   else if (transpose < -18) transposeMultiply = .25f;   // <-- DEAD
//
// `else if (transpose < -18)` can never execute: any value < -18 is also < -6,
// so the first branch always wins. transposeMultiply is therefore never 0.25f.
// This suite asserts the CURRENT behavior so a future "fix" is a visible,
// deliberate change (and so an accidental one is caught). Flagged in
// tests/SEAM.md for a separate change.
// ===========================================================================

TEST(HexterTransposeDeadBranch, TransposeBelow18StillAppliesHalfNotQuarter) {
    // Call voiceSetData directly on a controlled 155-byte unpacked buffer so we
    // can set patch[144] precisely (bypassing patchUnpack). Osc1 is configured
    // for a pre-transpose frequencyMul of exactly 1.0 (keyboard, coarse=1,
    // fine=0), so the post-transpose value equals transposeMultiply exactly.
    TestHexter h;
    auto mulForTranspose = [&h](uint8_t patch144) -> float {
        uint8_t u[kUnpackedSize] = {};
        // osc1 = operator index 0 -> eb_op = u + (5-0)*21 = u + 105.
        u[105 + 17] = 0;  // frequencyType = keyboard
        u[105 + 18] = 1;  // coarse = 1 -> frequencyMul 1.0
        u[105 + 19] = 0;  // fine = 0
        u[144] = patch144;
        struct OneSynthParams p;
        std::memset(&p, 0, sizeof(p));
        h.voiceSetData(&p, u);
        return p.osc1.frequencyMul;
    };

    // patch[144] -> transpose = limit(p144,0,48) - 24.
    // 0  -> -24 (< -18): DEAD branch would give 0.25; CURRENT gives 0.5.
    // 5  -> -19 (< -18): same.
    // 17 -> -7  (>= -18, < -6): 0.5 (the live first branch).
    // 18 -> -6  (>= -6):        1.0.
    // 24 -> 0:                  1.0.

    EXPECT_NEAR(mulForTranspose(0),  0.5f, kArithTol)
        << "transpose=-24 (<-18) should be 0.25 if the dead branch were live; "
           "current (buggy) behavior is 0.5";
    EXPECT_NEAR(mulForTranspose(5),  0.5f, kArithTol)
        << "transpose=-19 (<-18) same dead-branch proof";
    EXPECT_NEAR(mulForTranspose(17), 0.5f, kArithTol)
        << "transpose=-7 (>=-18, <-6) live first branch -> 0.5";

    // THE key assertion: transpose < -18 produces the SAME multiplier as
    // -18..-7. If the dead branch is ever revived, this fails loudly.
    EXPECT_FLOAT_EQ(mulForTranspose(0), mulForTranspose(17))
        << "transpose < -18 and -18..-7 must yield identical transposeMultiply "
           "under the current (dead-branch) behavior";

    // Above the -6 threshold -> 1.0.
    EXPECT_NEAR(mulForTranspose(18), 1.0f, kArithTol);
    EXPECT_NEAR(mulForTranspose(24), 1.0f, kArithTol);
}
