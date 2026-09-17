// Phase 8.7 / A1 — FX1-type live-voice write path: permanent host guards
// (_bmad-output/implementation-artifacts/spec-8-7-a1-fx-type-live-voice-mute.md).
//
// Stance: INVARIANT PROBE (not characterization, not golden). The 8.7
// investigation CLOSED defect candidate A1 ("FX-type write mutes a live
// voice") as NOT-A-DEFECT: the held-vs-fresh oracle below held on the
// UNPATCHED tree for every FILTER type, and the device evidence
// (fx_mute_probe.py) was reinterpreted as a classifier artifact — it
// compares the held level against the PRE-WRITE dry reference with no
// per-type fresh reference, so legitimate per-type filter attenuation
// (param-dependent; e.g. LP at param1≈0 is true digital silence) classifies
// as "muting". These tests are the permanent guard for the actual contract:
//
//   a held voice stays within 20 dB (amplitude x10 in stored int32 units)
//   of that type's FRESH-note reference after an FX1 type write.
//
// The write is driven through `RenderEvent::paramChange` → the production
// CC-routing entry point `Synth::setNewValueFromMidi` (synth-side dispatch:
// param write → propagate → Timbre::setNewEffecParam → per-voice
// Voice::setNewEffectParam). The CC/NRPN decode layers converge on this
// same entry (MidiDecoder.cpp controlChange / decodeNrpn); their
// equivalence is a code-trace claim recorded in the spec, not something
// these tests exercise. On device the decode runs inside the SAI audio
// callback sequentially before buildNewSampleBlock (preenfm3.cpp:322-334)
// — the same block-boundary semantics renderScript applies.
//
// Oracle strength (review round 1): levels are compared per 50-block
// SUB-WINDOW (median of the fresh reference's sub-window maxima vs the
// MINIMUM held sub-window maximum), so a single early peak cannot mask a
// persistent or delayed mute, and a decaying reference cannot pass the
// audibility guard — the vacuity traps the reviewers flagged for a
// whole-window max. Param convention mirrors GoldenMaster.FxSweep
// (golden_master_test.cpp): param3 is the FX gain (0 would fake a mute);
// two healthy vectors are swept because the device evidence was
// param-dependent. Degenerate vectors (e.g. param1=0, where fresh
// references are legitimately silent) are excluded BY DESIGN by the
// audibility guard: a mute cannot be judged for a type whose fresh voice
// is silent.

#include "golden_harness.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifndef PFM3_GOLDEN_DIR
#error "PFM3_GOLDEN_DIR must be defined by tests/CMakeLists.txt"
#endif

namespace {

constexpr std::size_t kTotalBlocks    = 1500;
constexpr std::size_t kWriteBlock     = 80;   // type write fires before this block
constexpr std::size_t kWindowStart    = 120;  // oracle window start (post-transient)
constexpr std::size_t kSubWindow      = 50;   // blocks per oracle sub-window (~36 ms)
constexpr int64_t     kSilenceFloor   = 100000;  // ~390 audio-LSB (see FxSweep)

constexpr std::size_t kSamplesPerBlock = golden::GoldenHarness::kSamplesPerBlock;

struct FxParams { float p1, p2, p3; };
constexpr FxParams kVectors[2] = {
    {0.6f, 0.55f, 0.6f},   // FxSweep convention
    {0.3f, 0.30f, 1.0f},   // second healthy vector (device evidence was param-dependent)
};

int64_t blockMaxAbs(const std::vector<int32_t>& r, std::size_t block) {
    int64_t m = 0;
    for (std::size_t i = block * kSamplesPerBlock;
         i < (block + 1) * kSamplesPerBlock; ++i) {
        const int64_t v = std::abs(static_cast<int64_t>(r[i]));
        if (v > m) m = v;
    }
    return m;
}

// Max |sample| per kSubWindow-block sub-window over [kWindowStart, total).
std::vector<int64_t> subWindowMaxes(const std::vector<int32_t>& r) {
    std::vector<int64_t> out;
    for (std::size_t b = kWindowStart; b + kSubWindow <= kTotalBlocks;
         b += kSubWindow) {
        int64_t m = 0;
        for (std::size_t blk = b; blk < b + kSubWindow; ++blk) {
            const int64_t v = blockMaxAbs(r, blk);
            if (v > m) m = v;
        }
        out.push_back(m);
    }
    return out;
}

// Median of a non-empty vector (sub-window maxima are block-quantized, so a
// plain median is stable against the preset LFO's slow modulation dips).
int64_t medianOf(std::vector<int64_t> v) {
    for (std::size_t i = 1; i < v.size(); ++i) {  // insertion sort (n ~ 27)
        int64_t key = v[i];
        std::size_t j = i;
        while (j > 0 && v[j - 1] > key) { v[j] = v[j - 1]; --j; }
        v[j] = key;
    }
    return v[v.size() / 2];
}

int64_t minOf(const std::vector<int64_t>& v) {
    int64_t m = v[0];
    for (int64_t x : v) if (x < m) m = x;
    return m;
}

std::vector<int32_t> allocRender() {
    return std::vector<int32_t>(kTotalBlocks * kSamplesPerBlock);
}

// FRESH reference render: type T active before the note (setTimbreFx runs
// afterNewParamsLoad, so the voice STARTS with valid T state — the
// production-faithful "fresh note" the corrected device probe would compare
// against). FILTER_OFF is the true dry baseline: the fxAfterBlock switch has
// no FILTER_OFF case (Voice.cpp — switch starts at FILTER_LP), so nothing
// consumes the smoothed mixerGain and the voice passes through untouched.
std::vector<int32_t> renderFreshReference(int type, FxParams p) {
    golden::GoldenHarness h(PFM3_GOLDEN_DIR);
    h.setTimbreFx(0, type, p.p1, p.p2, p.p3);
    std::vector<int32_t> r = allocRender();
    h.renderScript(golden::RenderScript::a4Sustain(), kTotalBlocks, r.data());
    return r;
}

// HELD probe render: voice starts on `src` (dry when FILTER_OFF), note held
// from block 0, FX1 type T written mid-hold at block kWriteBlock through the
// production CC-routing entry point.
std::vector<int32_t> renderHeldProbe(int src, int dst, FxParams p) {
    golden::GoldenHarness h(PFM3_GOLDEN_DIR);
    h.setTimbreFx(0, src, p.p1, p.p2, p.p3);
    golden::RenderScript script;
    script.events = {
        golden::RenderEvent::noteOn(0, 0, 69, 100),
        golden::RenderEvent::paramChange(kWriteBlock, 0, ROW_EFFECT1,
                                         ENCODER_EFFECT_TYPE,
                                         static_cast<float>(dst)),
    };
    std::vector<int32_t> r = allocRender();
    h.renderScript(script, kTotalBlocks, r.data());
    return r;
}

// Idle-timbre write, then a fresh note: write at kWriteBlock with no note
// sounding, noteOn at block 100. Covers the "type write on idle timbre" +
// "fresh note after a type write" matrix rows in one deterministic render.
// (The write fan-out reaches all granted voices whether playing or not.)
std::vector<int32_t> renderWriteThenNote(int type, FxParams p) {
    golden::GoldenHarness h(PFM3_GOLDEN_DIR);
    h.setTimbreFx(0, FILTER_OFF, p.p1, p.p2, p.p3);
    golden::RenderScript script;
    script.events = {
        golden::RenderEvent::paramChange(kWriteBlock, 0, ROW_EFFECT1,
                                         ENCODER_EFFECT_TYPE,
                                         static_cast<float>(type)),
        golden::RenderEvent::noteOn(100, 0, 69, 100),
    };
    std::vector<int32_t> r = allocRender();
    h.renderScript(script, kTotalBlocks, r.data());
    return r;
}

// Assert the A1 contract for one (src, dst, params) probe against the
// dst-typed fresh reference sub-windows.
void assertHeldWithin20Db(const char* label, int src, int dst, FxParams p,
                          const std::vector<int64_t>& refSubs) {
    SCOPED_TRACE(label);
    const std::vector<int32_t> held = renderHeldProbe(src, dst, p);
    const std::vector<int64_t> heldSubs = subWindowMaxes(held);
    const int64_t refMed = medianOf(refSubs);
    const int64_t heldMin = minOf(heldSubs);
    EXPECT_GE(heldMin * 10, refMed)
        << "src " << src << " -> dst " << dst << ": worst held sub-window "
        << heldMin << " vs fresh-reference median " << refMed
        << " (>20 dB down — the A1 live-voice mute)";
}

}  // namespace

// Zero-signal-trap guard for the oracle itself: every type's FRESH reference
// must be audibly sustained over the oracle window (MEDIAN sub-window level,
// so an early transient followed by decay cannot pass). Degenerate param
// vectors where a type's fresh voice is legitimately silent are excluded
// here BY DESIGN — the mute contract is unjudgeable for them.
TEST(FxHoldMute, FreshReferencesAreAudibleOverOracleWindow) {
    for (const FxParams& p : kVectors) {
        for (int t = FILTER_OFF; t < FILTER_LAST; t++) {
            SCOPED_TRACE("fx type " + std::to_string(t));
            const std::vector<int32_t> ref = renderFreshReference(t, p);
            ASSERT_GT(medianOf(subWindowMaxes(ref)), kSilenceFloor)
                << "fresh reference for type " << t
                << " is not audibly sustained over the oracle window — the "
                   "held-vs-fresh oracle would be vacuous for this type";
        }
    }
}

// Non-vacuity guard: the mid-hold type write must actually engage, and stay
// engaged. At least HALF the oracle sub-windows must differ from the dry
// FILTER_OFF control by more than 1 audio-LSB (256 stored units) — a single
// transient (switch click) or a one-sample difference cannot satisfy it.
// The device probe's own discovery (LP at param1=0 rendering true digital
// silence) makes persistent-difference — not audibility — the right
// engagement criterion.
TEST(FxHoldMute, TypeWriteMidHoldEngagesTheWetPath) {
    for (const FxParams& p : kVectors) {
        golden::GoldenHarness hDry(PFM3_GOLDEN_DIR);
        hDry.setTimbreFx(0, FILTER_OFF, p.p1, p.p2, p.p3);
        std::vector<int32_t> dry = allocRender();
        hDry.renderScript(golden::RenderScript::a4Sustain(), kTotalBlocks,
                          dry.data());

        // FILTER_OFF is excluded as a destination: the bed is FILTER_OFF, so
        // an OFF->OFF write changes no value and Synth::setNewValueFromMidi
        // suppresses propagation entirely (pinned by the same-type test).
        for (int t = FILTER_OFF + 1; t < FILTER_LAST; t++) {
            SCOPED_TRACE("fx type " + std::to_string(t));
            const std::vector<int32_t> held = renderHeldProbe(FILTER_OFF, t, p);
            std::size_t engaged = 0, subs = 0;
            for (std::size_t b = kWindowStart; b + kSubWindow <= kTotalBlocks;
                 b += kSubWindow, ++subs) {
                int64_t diff = 0;
                for (std::size_t i = b * kSamplesPerBlock;
                     i < (b + kSubWindow) * kSamplesPerBlock; ++i) {
                    const int64_t d =
                        std::abs(static_cast<int64_t>(held[i]) -
                                 static_cast<int64_t>(dry[i]));
                    if (d > diff) diff = d;
                }
                if (diff > 256) ++engaged;
            }
            EXPECT_GE(engaged * 2, subs)
                << "type " << t << ": only " << engaged << "/" << subs
                << " oracle sub-windows differ from the dry control — the "
                   "mid-hold type write never engaged (oracle vacuous)";
        }
    }
}

// THE contract (A1): a held voice survives an FX1 type write, from a dry
// start and from every stateful source type. For each destination, the
// probe's WORST oracle sub-window must stay within 20 dB of that type's
// fresh-note reference median. Swept over the full FILTER enum (the device
// characterization sampled 0..13; the write path is type-agnostic) and both
// param vectors. dst = FILTER_OFF is included here (src != OFF -> OFF is a
// real dispatch: the filter disengages and the dry voice must continue).
TEST(FxHoldMute, TypeWriteMidHoldKeepsHeldVoiceWithin20DbOfFreshReference) {
    for (const FxParams& p : kVectors) {
        std::vector<std::vector<int64_t>> refSubsByType(FILTER_LAST);
        for (int t = FILTER_OFF; t < FILTER_LAST; t++) {
            refSubsByType[t] = subWindowMaxes(renderFreshReference(t, p));
        }
        for (int t = FILTER_OFF + 1; t < FILTER_LAST; t++) {
            const std::string label =
                "fx type " + std::to_string(t) + " (dry start)";
            assertHeldWithin20Db(label.c_str(), FILTER_OFF, t, p,
                                 refSubsByType[t]);
        }
    }
}

// Stateful transitions: the A1 defect hypothesis was specifically about a
// HELD voice carrying populated per-voice FX state (coefficients, filter
// memories) across a type switch. The dry-start matrix cannot catch a
// regression that needs a stateful SOURCE type. Representative sources
// cover the state-bearing arms: LP (smoothed one-pole), BASS (seeded
// coefficients), BP (guarded biquad recompute), CRUSHER (cached quantizer
// params), PEAK (biquad family). A full 49x49 sweep is documented as the
// exhaustive form; this subset keeps the suite fast while covering each
// caching mechanism.
TEST(FxHoldMute, StatefulSourceTypeTransitionsKeepHeldVoiceWithin20Db) {
    const FxParams p = kVectors[0];
    std::vector<std::vector<int64_t>> refSubsByType(FILTER_LAST);
    for (int t = FILTER_OFF; t < FILTER_LAST; t++) {
        refSubsByType[t] = subWindowMaxes(renderFreshReference(t, p));
    }
    const int sources[] = {FILTER_LP, FILTER_BASS, FILTER_BP, FILTER_CRUSHER,
                           FILTER_PEAK};
    for (int src : sources) {
        for (int dst = FILTER_OFF; dst < FILTER_LAST; dst++) {
            if (dst == src) continue;  // same-type no-op, pinned separately
            const std::string label = "src " + std::to_string(src) + " -> dst " +
                                      std::to_string(dst);
            assertHeldWithin20Db(label.c_str(), src, dst, p, refSubsByType[dst]);
        }
    }
}

// Regression pin — dispatch semantics: a same-type T->T write through
// setNewValueFromMidi is a complete no-op (Synth.cpp guards propagation on
// oldValue != newValue), so the render must be byte-identical to the no-write
// control. Sweeps the FULL enum so type-specific no-change side effects
// cannot hide behind representative sampling. FILTER_OFF included: OFF->OFF
// is the canonical no-op.
TEST(FxHoldMute, SameTypeWriteIsByteIdenticalToNoWrite) {
    for (int t = FILTER_OFF; t < FILTER_LAST; t++) {
        SCOPED_TRACE("fx type " + std::to_string(t));
        golden::GoldenHarness hControl(PFM3_GOLDEN_DIR);
        hControl.setTimbreFx(0, t, kVectors[0].p1, kVectors[0].p2,
                             kVectors[0].p3);
        std::vector<int32_t> control = allocRender();
        hControl.renderScript(golden::RenderScript::a4Sustain(), kTotalBlocks,
                              control.data());

        golden::GoldenHarness hProbe(PFM3_GOLDEN_DIR);
        hProbe.setTimbreFx(0, t, kVectors[0].p1, kVectors[0].p2,
                           kVectors[0].p3);
        golden::RenderScript script;
        script.events = {
            golden::RenderEvent::noteOn(0, 0, 69, 100),
            golden::RenderEvent::paramChange(kWriteBlock, 0, ROW_EFFECT1,
                                             ENCODER_EFFECT_TYPE,
                                             static_cast<float>(t)),
        };
        std::vector<int32_t> probe = allocRender();
        hProbe.renderScript(script, kTotalBlocks, probe.data());

        ASSERT_EQ(0, std::memcmp(control.data(), probe.data(),
                                 control.size() * sizeof(int32_t)))
            << "same-type write changed the render for type " << t;
    }
}

// Regression pin — idle-timbre write + fresh note after a type write: no
// crash, and the note is audibly sustained through the new type (median
// sub-window level, so a note-on transient cannot pass a decaying render).
// FILTER_OFF excluded: OFF->OFF on an FILTER_OFF bed is a no-dispatch no-op.
TEST(FxHoldMute, TypeWriteThenFreshNoteIsAudible) {
    for (const FxParams& p : kVectors) {
        for (int t = FILTER_OFF + 1; t < FILTER_LAST; t++) {
            SCOPED_TRACE("fx type " + std::to_string(t));
            const std::vector<int32_t> r = renderWriteThenNote(t, p);
            EXPECT_GT(medianOf(subWindowMaxes(r)), kSilenceFloor)
                << "fresh note after a type-" << t
                << " write is not audibly sustained — the write broke "
                   "fresh-note playback";
        }
    }
}

// Phase 8.7 hardening red->green pin: Timbre::setNewEffecParam must skip
// ungranted (-1) voice slots. Manufactures the documented mid-transition
// state (a counted slot holding -1 — the FMDisplayMixer
// writes-mixer-state-before-propagate window; same manufacturing approach
// as the 8.4 getFreeVoice test) through the public setVoiceNumber seam, then
// drives the FX1 param dispatch DIRECTLY (synth_core_test.cpp precedent —
// setNewValueFromMidi is public) with no render: the render loops share the
// same systemic -1 window (Timbre::voicesNextBlock, Synth::allNoteOff et al.)
// which is pre-existing, documented 8.4 review residue — out of this story's
// scope — and would crash this test for reasons unrelated to the guarded
// function. Pre-guard this test is red (voices_[-1]->setNewEffectParam
// dereferences garbage); post-guard the write reaches the granted voices
// and the param lands.
TEST(FxHoldMute, FxDispatchSkipsUngrantedVoiceSlots) {
    golden::GoldenHarness h(PFM3_GOLDEN_DIR);
    h.setTimbreFx(0, FILTER_OFF, kVectors[0].p1, kVectors[0].p2,
                  kVectors[0].p3);

    // Slot 0 is granted (g0Default: timbre 0 has 6 voices, slots 0..5);
    // revoke it without touching numberOfVoices_ — exactly the documented
    // mid-window state.
    h.synth().getTimbre(0)->setVoiceNumber(0, -1);

    // The production CC-routing dispatch into the guarded fan-out — must not
    // crash (pre-guard: voices_[-1] dereference inside setNewEffectParam).
    h.synth().setNewValueFromMidi(0, ROW_EFFECT1, ENCODER_EFFECT_TYPE,
                                  static_cast<float>(FILTER_LP));

    // The write still landed on the timbre param (dispatch was not swallowed
    // by the skipped slot) — read back through the raw param block.
    EXPECT_EQ(FILTER_LP,
              static_cast<int>(
                  h.synth().getTimbre(0)->getParamRaw()->effect1.type))
        << "the type write did not land after skipping the ungranted slot";
}
