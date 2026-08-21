// Host-side STATE-MACHINE coverage for the synth core (Synth + Timbre + Voice
// allocation/state transitions) — spec task 3 of
// _bmad-output/implementation-artifacts/spec-test-coverage-phase3.md.
//
// Stance: CHARACTERIZATION. These tests lock the CURRENT voice-allocation /
// play-mode / pedal / arp / dispatch behavior via PUBLIC Synth/Timbre API only
// (no #define private public, no mocks), driving the REAL Synth graph through
// golden::GoldenHarness. No fixtures are compared here — that is the golden
// tier's job (tests/golden_master_test.cpp); this file asserts observable state
// (getNumberOfPlayingVoices / isPlaying / renders finite-non-silent / byte-
// equality of deterministic renders between two fresh harnesses).
//
// Per-voice identity (WHICH voice a note landed on) is NOT publicly observable
// (Synth::voices_ / Timbre::voices_ are private, no getNotePlayedByVoice
// accessor exists), so the steal/reuse suites assert the aggregate: voice count
// stays capped at the timbre's allocation, the newest note keeps sounding, and
// the render stays finite/non-silent. The NEW_NOTE_OLD / NEW_NOTE_RELEASE code
// paths (Timbre.cpp preenNoteOn) are what these calls drive.
//
// NOTE on play-count semantics: Synth::getNumberOfPlayingVoices() is refreshed
// by buildNewSampleBlock (Synth.cpp:301-312 sums Timbre::voicesNextBlock()), so
// every count assertion follows at least one render.

#include "golden_harness.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#ifndef PFM3_GOLDEN_DIR
#error "PFM3_GOLDEN_DIR must be defined by tests/CMakeLists.txt"
#endif

// allParameterRows is provided by the host stub
// (tests/stubs/midi_decoder_collaborators_stub.cpp) — the FAVOR-REAL-DATA
// EXCEPTION documented in tests/SEAM.md Target #4 appendix. Declaring it extern
// lets the Synth::newParamValue dispatch tests pass the same ParameterDisplay*
// the production setNewValueFromMidi path would.
extern struct AllParameterRowsDisplay allParameterRows;

namespace {

// Silence threshold in stored int32 DAC units. The final DAC formatting is
// `int32 = (24-bit clamped) << 8`, so 1 audio-LSB = 256 stored units. A fully
// decayed voice renders exact zeros; the threshold absorbs smoother residue.
constexpr int64_t kSilenceThreshold = 256;

class SynthCore : public ::testing::Test {
protected:
    void SetUp() override {
        harness_.reset(new golden::GoldenHarness(PFM3_GOLDEN_DIR));
    }

    Synth& synth() { return harness_->synth(); }

    // Render one block; returns max |sample| across the 3 output buffers.
    int64_t renderBlock() {
        synth().buildNewSampleBlock(b1_, b2_, b3_);
        int64_t m = 0;
        for (int i = 0; i < 64; i++) {
            m = std::max(m, std::abs(static_cast<int64_t>(b1_[i])));
            m = std::max(m, std::abs(static_cast<int64_t>(b2_[i])));
            m = std::max(m, std::abs(static_cast<int64_t>(b3_[i])));
        }
        return m;
    }

    int64_t renderBlocks(std::size_t n) {
        int64_t m = 0;
        for (std::size_t i = 0; i < n; i++) m = std::max(m, renderBlock());
        return m;
    }

    // Blocks until the render is silent (<= kSilenceThreshold) or maxBlocks.
    // Returns the number of blocks rendered. The block that first falls
    // silent IS rendered before returning (it is the one measured), so the
    // play-counters reflect it; a return == maxBlocks means silence was
    // NEVER reached — callers using the result as a completion time must
    // assert it is < maxBlocks.
    std::size_t renderUntilSilent(std::size_t maxBlocks) {
        std::size_t n = 0;
        while (n < maxBlocks) {
            if (renderBlock() <= kSilenceThreshold && n > 4) return n;
            n++;
        }
        return n;
    }

    bool renderIsSilent(std::size_t n) {
        return renderBlocks(n) <= kSilenceThreshold;
    }

    uint8_t playCount() { return synth().getNumberOfPlayingVoices(); }

    int voiceForSlot(int slot) {
        const int8_t voice = synth().getTimbre(0)->voiceNumber_[slot];
        if (voice < 0) {
            ADD_FAILURE() << "voice slot " << slot << " is unassigned";
            return 0;  // Safe fallback prevents a failed assertion from OOB access.
        }
        return static_cast<int>(static_cast<uint8_t>(voice));
    }

    std::unique_ptr<golden::GoldenHarness> harness_;
    int32_t b1_[64], b2_[64], b3_[64];
};

// ===========================================================================
// 1. Voice allocation & stealing (Timbre::preenNoteOn NEW_NOTE_OLD path).
// ===========================================================================

TEST_F(SynthCore, SixNoteOnsFillAllSixVoices) {
    for (int n = 0; n < 6; n++) {
        synth().noteOn(0, 60 + n * 4, 100);
        renderBlocks(2);
    }
    EXPECT_EQ(playCount(), 6);
    EXPECT_GT(renderBlocks(4), kSilenceThreshold);
}

TEST_F(SynthCore, SeventhNoteOnStealsOldestPlayingVoiceNotASeventh) {
    // 6 distinct noteOns with renders between (each voice gets a distinct
    // index via voiceIndex_++), then a 7th: getFreeVoiceIndex has no free and
    // no released voice -> the second loop picks the smallest-index (oldest)
    // playing voice -> NEW_NOTE_OLD -> noteOnWithoutPop (quick-die + pending
    // note). Aggregate observable: still exactly 6 playing voices, and the
    // newest note keeps sounding.
    for (int n = 0; n < 6; n++) {
        synth().noteOn(0, 60 + n * 4, 100);
        renderBlocks(3);
    }
    ASSERT_EQ(playCount(), 6);
    synth().noteOn(0, 84, 100);   // 7th — must steal, not exceed
    renderBlocks(2);
    EXPECT_EQ(playCount(), 6) << "7th noteOn exceeded the timbre's 6-voice allocation";
    const int oldestVoice = voiceForSlot(0);
    EXPECT_EQ(static_cast<int>(static_cast<uint8_t>(
                  synth().hostVoice(oldestVoice).getNote())), 84)
        << "the oldest voice slot was not selected for the seventh note";
    EXPECT_GT(renderBlocks(4), kSilenceThreshold) << "newest note must keep sounding";
}

TEST_F(SynthCore, StolenOldestNoteIsGoneNoteOffsOfSoundingNotesDrainAll) {
    // After the steal, note 60 is gone (its voice was quick-killed). NoteOffs
    // for only the six expected SOUNDING notes must drain every voice; omitting
    // noteOff(60) makes the selected steal victim observable.
    for (int n = 0; n < 6; n++) {
        synth().noteOn(0, 60 + n * 4, 100);
        renderBlocks(3);
    }
    synth().noteOn(0, 84, 100);
    renderBlocks(2);
    for (int n = 1; n < 6; n++) synth().noteOff(0, 60 + n * 4);
    synth().noteOff(0, 84);
    // Deliberately do NOT send noteOff(60): reaching silence proves note 60
    // really was the stolen victim rather than merely draining every note ID.
    const std::size_t drained = renderUntilSilent(2200);
    ASSERT_LT(drained, 2200) << "release never completed within the budget "
                                "(regression: both releases stopped decaying)";
    EXPECT_TRUE(renderIsSilent(8));
    // CHARACTERIZATION (quirk, NOT fixed): isPlaying() stays TRUE here — the
    // voice that was mid-quick-die (newNotePending) when its noteOff arrived
    // takes the `pendingNote += 128` branch (Voice.cpp:592-596) and its
    // `playing` flag is never cleared, even after the render decays to
    // silence. No stuck AUDIO (render is silent) and the voice is recoverable:
    // a fresh noteOn steals it back and sounds normally.
    EXPECT_TRUE(synth().isPlaying())
        << "characterized stuck-playing flag unexpectedly cleared";
    synth().noteOn(0, 88, 100);
    renderBlocks(3);
    EXPECT_GT(renderBlocks(4), kSilenceThreshold) << "stuck-flag voice is stealable (no audible stuck note)";
}

TEST_F(SynthCore, ReleasedVoiceIsReusedByNextNoteOn) {
    // 6 notes on, then noteOff one -> that voice enters release (still playing
    // its tail). A new noteOn must reuse it (NEW_NOTE_RELEASE, preferred over
    // stealing a playing voice). Inspect the assigned host voice directly so
    // an implementation that steals the oldest active voice cannot pass on
    // aggregate count alone.
    for (int n = 0; n < 6; n++) {
        synth().noteOn(0, 60 + n * 4, 100);
        renderBlocks(3);
    }
    ASSERT_EQ(playCount(), 6);
    Timbre* timbre = synth().getTimbre(0);
    const int releasedVoice = voiceForSlot(0);
    synth().noteOff(0, 60);
    renderBlocks(5);                  // release tail: still playing
    ASSERT_TRUE(synth().hostVoice(releasedVoice).isReleased());
    synth().noteOn(0, 90, 100);       // must reuse the released voice
    renderBlocks(2);
    EXPECT_EQ(playCount(), 6);
    EXPECT_EQ(static_cast<int>(static_cast<uint8_t>(
                  synth().hostVoice(releasedVoice).getNote())), 90);
    for (int slot = 1; slot < 6; slot++) {
        EXPECT_EQ(static_cast<int>(static_cast<uint8_t>(
                      synth().hostVoice(voiceForSlot(slot)).getNote())),
                  60 + slot * 4)
            << "active voice slot " << slot << " was stolen instead";
    }
    EXPECT_GT(renderBlocks(4), kSilenceThreshold);
}

TEST_F(SynthCore, SameNoteRetriggerUsesSameVoice) {
    // preenNoteOn's priority-1 path: an incoming note equal to a playing
    // voice's note re-triggers THAT voice (noteOnWithoutPop), never a second
    // voice.
    synth().noteOn(0, 69, 100);
    renderBlocks(3);
    ASSERT_EQ(playCount(), 1);
    Timbre* timbre = synth().getTimbre(0);
    const int originalVoice = voiceForSlot(0);
    synth().noteOn(0, 69, 100);
    renderBlocks(2);
    EXPECT_EQ(playCount(), 1) << "same-note retrigger allocated a second voice";
    EXPECT_TRUE(synth().hostVoice(originalVoice).isPlaying());
    EXPECT_EQ(static_cast<int>(static_cast<uint8_t>(
                  synth().hostVoice(originalVoice).getNote())), 69);
    for (int slot = 1; slot < 6; slot++) {
        EXPECT_FALSE(synth().hostVoice(voiceForSlot(slot)).isPlaying())
            << "same-note retrigger moved to voice slot " << slot;
    }
}

// ===========================================================================
// 2. Play modes (engine1.playMode read live by preenNoteOn).
// ===========================================================================

TEST_F(SynthCore, MonoModeUsesASingleLegatoVoice) {
    auto* p = synth().getTimbre(0)->getParamRaw();
    p->engine1.playMode = PLAY_MODE_MONO;
    synth().noteOn(0, 60, 100);
    renderBlocks(3);
    ASSERT_EQ(playCount(), 1);
    synth().noteOn(0, 67, 100);   // legato: same voice, no 2nd allocation
    renderBlocks(3);
    EXPECT_EQ(playCount(), 1);
    EXPECT_GT(renderBlocks(4), kSilenceThreshold);
}

TEST_F(SynthCore, MonoModeNoteOffReleasesWithoutRecall) {
    // CHARACTERIZATION: MONO mode has no note-stack recall in preenNoteOff —
    // after the (single) sounding note is released, a previously-held lower
    // note is NOT restarted. Locked as-is; a recall feature would flip this.
    auto* p = synth().getTimbre(0)->getParamRaw();
    p->engine1.playMode = PLAY_MODE_MONO;
    synth().noteOn(0, 60, 100);
    renderBlocks(3);
    synth().noteOn(0, 67, 100);
    renderBlocks(3);
    synth().noteOff(0, 67);
    renderUntilSilent(2200);
    EXPECT_TRUE(renderIsSilent(8));
    // (isPlaying() may stay true — same pendingNote+=128 stuck-flag quirk as
    // the steal suite; the RENDER is silent and a fresh noteOn recovers.)
    synth().noteOn(0, 72, 100);
    renderBlocks(3);
    EXPECT_GT(renderBlocks(4), kSilenceThreshold);
}

TEST_F(SynthCore, UnisonModeOneNoteSoundsAllVoices) {
    // PLAY_MODE_UNISON: preenNoteOn starts EVERY voice of the timbre (detuned
    // spread); voicesNextBlock's unison branch accumulates into voice0.
    auto* p = synth().getTimbre(0)->getParamRaw();
    p->engine1.playMode = PLAY_MODE_UNISON;
    synth().noteOn(0, 69, 100);
    renderBlocks(3);
    EXPECT_EQ(playCount(), 6) << "unison noteOn must sound all 6 voices";
    EXPECT_GT(renderBlocks(4), kSilenceThreshold);
    synth().noteOff(0, 69);
    renderUntilSilent(2200);
    EXPECT_TRUE(renderIsSilent(8));
    EXPECT_FALSE(synth().isPlaying())
        << "without the pedal the voice must be freed once the tail decays";
}

TEST_F(SynthCore, UnisonModeIsDetectedByTimbreAccessor) {
    EXPECT_FALSE(synth().getTimbre(0)->isUnisonMode());  // default preset = POLY
    synth().getTimbre(0)->getParamRaw()->engine1.playMode = PLAY_MODE_UNISON;
    EXPECT_TRUE(synth().getTimbre(0)->isUnisonMode());
}

// ===========================================================================
// 3. Glide (Voice::glideToNote via noteOnWithoutPop in MONO + OVERLAP).
// ===========================================================================

TEST_F(SynthCore, MonoOverlapGlideChangesTheRenderVersusNoGlide) {
    // glideType is read live by Voice::noteOnWithoutPop (Voice.cpp:424): in
    // MONO with GLIDE_TYPE_OVERLAP a second noteOn glides the oscillators
    // instead of quick-restarting. Observable: the deterministic render of the
    // glide sequence differs byte-wise from the same sequence with glide OFF,
    // while staying non-silent on a single voice.
    auto run = [](bool glideOn) {
        golden::GoldenHarness h(PFM3_GOLDEN_DIR);
        auto* p = h.synth().getTimbre(0)->getParamRaw();
        p->engine1.playMode = PLAY_MODE_MONO;
        p->engine2.glideType = glideOn ? GLIDE_TYPE_OVERLAP : GLIDE_TYPE_OFF;
        p->engine1.glideSpeed = 0.0f;
        std::vector<int32_t> out(160 * golden::GoldenHarness::kSamplesPerBlock);
        h.synth().noteOn(0, 60, 100);
        for (std::size_t b = 0; b < 160; b++) {
            if (b == 20) h.synth().noteOn(0, 72, 100);
            int32_t* dst = &out[b * golden::GoldenHarness::kSamplesPerBlock];
            h.synth().buildNewSampleBlock(dst, dst + 64, dst + 128);
        }
        return out;
    };
    auto withGlide = run(true);
    auto noGlide = run(false);
    // Both non-silent, both on ONE voice (mono legato), but the glide changes
    // the waveform (frequency transition vs hard switch).
    int64_t m = 0;
    for (int32_t s : withGlide) {
        m = std::max(m, std::abs(static_cast<int64_t>(s)));
    }
    EXPECT_GT(m, kSilenceThreshold);
    EXPECT_NE(std::memcmp(withGlide.data(), noGlide.data(), withGlide.size() * sizeof(int32_t)), 0)
        << "glide ON rendered byte-identically to glide OFF — glide path not taken";
}

TEST_F(SynthCore, GlideSpeedControlsTransitionDuration) {
    auto isGlidingAfter = [](float glideSpeed, std::size_t blocks) {
        golden::GoldenHarness h(PFM3_GOLDEN_DIR);
        auto* p = h.synth().getTimbre(0)->getParamRaw();
        p->engine1.playMode = PLAY_MODE_MONO;
        p->engine2.glideType = GLIDE_TYPE_OVERLAP;
        p->engine1.glideSpeed = glideSpeed;
        int32_t b1[64], b2[64], b3[64];
        h.synth().noteOn(0, 60, 100);
        for (int i = 0; i < 3; i++) h.synth().buildNewSampleBlock(b1, b2, b3);
        const int voice = static_cast<int>(static_cast<uint8_t>(
            h.synth().getTimbre(0)->voiceNumber_[0]));
        h.synth().noteOn(0, 72, 100);
        for (std::size_t i = 0; i < blocks; i++) {
            h.synth().buildNewSampleBlock(b1, b2, b3);
        }
        return h.synth().hostVoice(voice).isGliding();
    };

    EXPECT_FALSE(isGlidingAfter(0.0f, 2))
        << "fastest glide should complete in two blocks";
    EXPECT_TRUE(isGlidingAfter(12.0f, 100))
        << "slowest glide completed far earlier than its 2700-block budget";
    EXPECT_FALSE(isGlidingAfter(12.0f, 2800))
        << "slowest glide did not complete within its expected budget";
}

TEST_F(SynthCore, GlideFirstNoteOffKeepsGlideTargetSounding) {
    // preenNoteOff's gliding branch: releasing the FIRST note of a glide calls
    // glideFirstNoteOff; the glide target note keeps sounding.
    auto* p = synth().getTimbre(0)->getParamRaw();
    p->engine1.playMode = PLAY_MODE_MONO;
    p->engine2.glideType = GLIDE_TYPE_OVERLAP;
    p->engine1.glideSpeed = 4.0f;
    synth().noteOn(0, 60, 100);
    renderBlocks(3);
    synth().noteOn(0, 72, 100);   // glide 60 -> 72
    renderBlocks(2);
    ASSERT_EQ(playCount(), 1);
    synth().noteOff(0, 60);       // release the gliding-from note
    renderBlocks(20);
    EXPECT_EQ(playCount(), 1);
    EXPECT_GT(renderBlocks(4), kSilenceThreshold)
        << "glide target must keep sounding after first-note release";
    synth().noteOff(0, 72);
    renderUntilSilent(2200);
    EXPECT_FALSE(synth().isPlaying());
}

// ===========================================================================
// 4. Hold pedal (Synth::setHoldPedal -> Timbre::setHoldPedal).
// ===========================================================================

TEST_F(SynthCore, HoldPedalKeepsNoteSoundingAcrossNoteOff) {
    synth().setHoldPedal(0, 127);
    synth().noteOn(0, 69, 100);
    renderBlocks(10);
    synth().noteOff(0, 69);
    // NOTE ON THE ORACLE: with the default preset the env's sustain stage
    // decays to silence on its own (~1500 blocks), so held-vs-released is NOT
    // amplitude-distinguishable. The reliable observable is voice LIFETIME:
    // the pedal-held voice is never released (envs never note-off), so
    // isPlaying() stays true past the no-pedal silence point (see the control
    // test below, which ALSO asserts the decay completes well inside its
    // budget), and the render stays exactly silent-zero only after the pedal
    // is lifted. The 2400-block hold budget carries ~60% margin over the
    // ~1500-block empirical decay so benign cross-libm decay-rate drift
    // cannot flip the isPlaying assertion.
    renderBlocks(2400);
    EXPECT_TRUE(synth().isPlaying()) << "held voice was freed despite the pedal";
    synth().setHoldPedal(0, 0);   // pedal up -> deferred noteOff fires
    const std::size_t released = renderUntilSilent(2200);
    ASSERT_LT(released, 2200) << "post-pedal release never completed";
    EXPECT_TRUE(renderIsSilent(8));
    EXPECT_FALSE(synth().isPlaying()) << "pedal release must release the held note";
}

TEST_F(SynthCore, WithoutHoldPedalNoteOffReleasesWithinTail) {
    // Control for the pedal test: same sequence, no pedal — the note's release
    // tail fully decays well within 600 blocks (cf. envelope_adsr_full golden:
    // tail < 300 blocks).
    synth().noteOn(0, 69, 100);
    renderBlocks(10);
    synth().noteOff(0, 69);
    const std::size_t decayed = renderUntilSilent(2200);
    ASSERT_LT(decayed, 2200) << "no-pedal decay never completed — the pedal "
                                "test's hold-budget premise is broken";
    EXPECT_TRUE(renderIsSilent(8));
    EXPECT_FALSE(synth().isPlaying())
        << "without the pedal the voice must be freed once the tail decays";
}

// ===========================================================================
// 5. allNoteOff / allNoteOffQuick / allSoundOff semantics.
// ===========================================================================

TEST_F(SynthCore, AllNoteOffReleasesVoicesWithEnvelopeTail) {
    synth().noteOn(0, 69, 100);
    renderBlocks(10);
    synth().allNoteOff(0);
    renderBlocks(2);
    // Release path: still sounding immediately after (envelope tail)...
    EXPECT_TRUE(synth().isPlaying());
    EXPECT_GT(renderBlocks(2), kSilenceThreshold);
    // ...and fully decayed within the tail window.
    renderUntilSilent(2200);
    EXPECT_FALSE(synth().isPlaying());
}

TEST_F(SynthCore, AllSoundOffKillsVoicesImmediately) {
    synth().noteOn(0, 69, 100);
    synth().noteOn(0, 76, 100);
    renderBlocks(10);
    ASSERT_TRUE(synth().isPlaying());
    synth().allSoundOff(0);
    renderBlocks(2);
    EXPECT_FALSE(synth().isPlaying())
        << "allSoundOff(timbre) must killNow() every voice of the timbre";
}

TEST_F(SynthCore, GlobalAllSoundOffKillsEveryVoiceImmediately) {
    synth().noteOn(0, 69, 100);
    renderBlocks(10);
    ASSERT_TRUE(synth().isPlaying());
    synth().allSoundOff();
    renderBlocks(2);
    EXPECT_FALSE(synth().isPlaying());
}

TEST_F(SynthCore, AllNoteOffQuickIsNotSlowerThanAllNoteOff) {
    // noteOffQuick swaps the envelopes to their quick-release curve; the
    // measured silence point must not be LATER than the full release's.
    auto measure = [](int variant) {
        golden::GoldenHarness h(PFM3_GOLDEN_DIR);
        int32_t b1[64], b2[64], b3[64];
        h.synth().noteOn(0, 69, 100);
        for (int i = 0; i < 10; i++) h.synth().buildNewSampleBlock(b1, b2, b3);
        if (variant == 0) h.synth().allNoteOff(0); else h.synth().allNoteOffQuick(0);
        std::size_t n = 0;
        while (n < 2200) {
            h.synth().buildNewSampleBlock(b1, b2, b3);
            n++;
            int64_t m = 0;
            for (int i = 0; i < 64; i++) {
                m = std::max({m,
                    std::abs(static_cast<int64_t>(b1[i])),
                    std::abs(static_cast<int64_t>(b2[i])),
                    std::abs(static_cast<int64_t>(b3[i]))});
            }
            if (m <= kSilenceThreshold && n > 4) break;
        }
        return n;
    };
    std::size_t full = measure(0);
    std::size_t quick = measure(1);
    ASSERT_LT(full, 2200U) << "allNoteOff never reached silence";
    ASSERT_LT(quick, 2200U) << "allNoteOffQuick never reached silence";
    EXPECT_LE(quick, full) << "allNoteOffQuick (" << quick << " blocks) slower than allNoteOff (" << full << ")";
}

// ===========================================================================
// 6. Arpeggiator knobs (ROW_ARPEGGIATOR1/2 via setNewValueFromMidi — the
//    production CC/NRPN route; permissive stub rows).
// ===========================================================================

TEST_F(SynthCore, ArpeggiatorDirectionChangesThePattern) {
    // Same held triad at the same BPM/octave; only the direction differs
    // (0 = UP, 1 = DOWN — the anonymous-namespace enum in Timbre.cpp:57-62,
    // driven here as raw values, exactly what the editor sends). The two
    // deterministic renders must differ.
    auto run = [](int direction) {
        golden::GoldenHarness h(PFM3_GOLDEN_DIR);
        h.enableArpeggiator(0, 120, direction, 1);
        h.synth().noteOn(0, 60, 100);
        h.synth().noteOn(0, 64, 100);
        h.synth().noteOn(0, 67, 100);
        std::vector<int32_t> out(300 * golden::GoldenHarness::kSamplesPerBlock);
        for (std::size_t b = 0; b < 300; b++) {
            int32_t* dst = &out[b * golden::GoldenHarness::kSamplesPerBlock];
            h.synth().buildNewSampleBlock(dst, dst + 64, dst + 128);
        }
        return out;
    };
    auto up = run(0), down = run(1);
    int64_t m = 0;
    for (int32_t s : up) m = std::max(m, (int64_t) std::abs(s));
    EXPECT_GT(m, kSilenceThreshold) << "arp never sounded (zero-signal trap)";
    EXPECT_NE(std::memcmp(up.data(), down.data(), up.size() * sizeof(int32_t)), 0)
        << "arp direction change did not alter the render";
}

TEST_F(SynthCore, ArpeggiatorOctaveRangeExtendsThePattern) {
    auto run = [](int octave) {
        golden::GoldenHarness h(PFM3_GOLDEN_DIR);
        h.enableArpeggiator(0, 120, 0 /*UP*/, octave);
        h.synth().noteOn(0, 60, 100);
        h.synth().noteOn(0, 67, 100);
        std::vector<int32_t> out(2600 * golden::GoldenHarness::kSamplesPerBlock);
        for (std::size_t b = 0; b < 2600; b++) {
            int32_t* dst = &out[b * golden::GoldenHarness::kSamplesPerBlock];
            h.synth().buildNewSampleBlock(dst, dst + 64, dst + 128);
        }
        return out;
    };
    auto one = run(1), three = run(3);
    EXPECT_NE(std::memcmp(one.data(), three.data(), one.size() * sizeof(int32_t)), 0)
        << "arp octave range change did not alter the render";
}

TEST_F(SynthCore, ArpeggiatorLatchKeepsCyclingAfterNoteOff) {
    // ROW_ARPEGGIATOR2 / ENCODER_ARPEGGIATOR_LATCH via the production
    // setNewValueFromMidi route (needs the stub's permissive bounds — the G4
    // documented pattern). Latched: noteOffs stop recording but the note stack
    // is retained, so the arp keeps cycling and the render stays non-silent
    // long after all keys are up.
    harness_->enableArpeggiator(0, 120, 0 /*UP*/, 1);
    synth().setNewValueFromMidi(0, ROW_ARPEGGIATOR2, ENCODER_ARPEGGIATOR_LATCH, 1.0f);
    synth().noteOn(0, 60, 100);
    synth().noteOn(0, 64, 100);
    synth().noteOn(0, 67, 100);
    renderBlocks(120);
    synth().noteOff(0, 60);
    synth().noteOff(0, 64);
    synth().noteOff(0, 67);
    EXPECT_GT(renderBlocks(300), kSilenceThreshold)
        << "latched arp went silent after noteOff — latch not retained";
}

TEST_F(SynthCore, ArpeggiatorWithoutLatchStopsSoundingAfterNoteOff) {
    // Control for the latch test: unlatched, the noteOffs empty the stack and
    // the last scheduled note-off fires; the render goes silent.
    harness_->enableArpeggiator(0, 120, 0 /*UP*/, 1);
    synth().noteOn(0, 60, 100);
    synth().noteOn(0, 64, 100);
    synth().noteOn(0, 67, 100);
    renderBlocks(120);
    ASSERT_GT(renderBlocks(20), kSilenceThreshold);
    synth().noteOff(0, 60);
    synth().noteOff(0, 64);
    synth().noteOff(0, 67);
    renderUntilSilent(2200);
    EXPECT_TRUE(renderIsSilent(8)) << "unlatched arp kept sounding after all noteOffs";
}

TEST_F(SynthCore, ArpeggiatorBpmChangeMidRunAltersThePattern) {
    // 120 BPM throughout vs a mid-run switch to 240 (ENCODER_ARPEGGIATOR_BPM,
    // the production route). The renders must diverge from the switch onward.
    auto run = [](bool doubleAt60) {
        golden::GoldenHarness h(PFM3_GOLDEN_DIR);
        h.enableArpeggiator(0, 120, 0 /*UP*/, 1);
        h.synth().noteOn(0, 60, 100);
        h.synth().noteOn(0, 67, 100);
        std::vector<int32_t> out(300 * golden::GoldenHarness::kSamplesPerBlock);
        for (std::size_t b = 0; b < 300; b++) {
            if (doubleAt60 && b == 60)
                h.synth().setNewValueFromMidi(0, ROW_ARPEGGIATOR1,
                                              ENCODER_ARPEGGIATOR_BPM, 240.0f);
            int32_t* dst = &out[b * golden::GoldenHarness::kSamplesPerBlock];
            h.synth().buildNewSampleBlock(dst, dst + 64, dst + 128);
        }
        return out;
    };
    auto steady = run(false), doubled = run(true);
    // Compare only AFTER the switch point (block 60) — before it the two runs
    // are byte-identical by construction.
    const std::size_t off = 100 * golden::GoldenHarness::kSamplesPerBlock;
    EXPECT_NE(std::memcmp(steady.data() + off, doubled.data() + off,
                          (steady.size() - off) * sizeof(int32_t)), 0)
        << "mid-run BPM change did not alter the arp pattern";
}

// ===========================================================================
// 7. Synth::newParamValue / newMixerValue dispatch arms.
//    Driven BOTH directly (synth().newParamValue — public) and via the
//    production setNewValueFromMidi route.
// ===========================================================================

TEST_F(SynthCore, NewParamValueEnvRowsDispatchReloadADSR) {
    // ROW_ENVn_TIME/LEVEL x encoders 0..3 -> env{n}_.reloadADSR(encoder);
    // ROW_ENVn_CURVE -> env{n}_.applyCurves(). No public env introspection —
    // the lock is "dispatches on a sounding patch without corrupting the
    // render", plus the render keeps producing finite non-silent audio.
    synth().noteOn(0, 69, 100);
    renderBlocks(5);
    for (int row = ROW_ENV1_TIME; row <= ROW_ENV6_LEVEL; row++) {
        for (int e = 0; e < NUMBER_OF_ENCODERS_PFM2; e++) {
            ParameterDisplay* param = &allParameterRows.row[row]->params[e];
            synth().newParamValue(0, row, e, param, 0.0f, 1.5f);
        }
    }
    for (int row = ROW_ENV1_CURVE; row <= ROW_ENV6_CURVE; row++) {
        for (int e = 0; e < NUMBER_OF_ENCODERS_PFM2; e++) {
            ParameterDisplay* param = &allParameterRows.row[row]->params[e];
            synth().newParamValue(0, row, e, param, 0.0f, 1.0f);
        }
    }
    EXPECT_GT(renderBlocks(20), kSilenceThreshold);
}

TEST_F(SynthCore, NewParamValueMidiNoteCurveAndEffectRowsDispatch) {
    // ROW_MIDINOTE1CURVE/2 -> Timbre::updateMidiNoteScale(0/1);
    // ROW_EFFECT1 -> Timbre::setNewEffecParam (per-voice setNewEffectParam).
    synth().noteOn(0, 69, 100);
    renderBlocks(5);
    for (int e = 0; e < NUMBER_OF_ENCODERS_PFM2; e++) {
        synth().newParamValue(0, ROW_MIDINOTE1CURVE, e,
                              &allParameterRows.row[ROW_MIDINOTE1CURVE]->params[e], 0.0f, 1.0f);
        synth().newParamValue(0, ROW_MIDINOTE2CURVE, e,
                              &allParameterRows.row[ROW_MIDINOTE2CURVE]->params[e], 0.0f, 1.0f);
        synth().newParamValue(0, ROW_EFFECT1, e,
                              &allParameterRows.row[ROW_EFFECT1]->params[e], 0.0f, 0.5f);
    }
    // A fresh note after the scale recompute still routes (round-trip guard).
    synth().noteOn(0, 72, 100);
    renderBlocks(10);
    EXPECT_GT(renderBlocks(4), kSilenceThreshold);
}

TEST_F(SynthCore, NewParamValuePerformanceRowSetsMatrixCcSource) {
    // ROW_PERFORMANCE1 -> Timbre::setMatrixSource(MATRIX_SOURCE_CC1+encoder,
    // value) — observable through the PFM3_HOST introspection helper.
    Timbre* timbre = synth().getTimbre(0);
    synth().newParamValue(0, ROW_PERFORMANCE1, 2 /* -> MATRIX_SOURCE_CC3 */,
                          &allParameterRows.row[ROW_PERFORMANCE1]->params[2], 0.0f, 0.75f);
    EXPECT_FLOAT_EQ(timbre->hostMaxMatrixSource(MATRIX_SOURCE_CC3), 0.75f);
}

TEST_F(SynthCore, SetNewValueFromMidiRoutesThroughClampAndDispatch) {
    // The production CC/NRPN entry: Timbre::setNewValue clamps against the
    // (stubbed) ParameterDisplay bounds, writes the flat param field, and —
    // when the value changed — propagates to Synth::newParamValue. For a row
    // on the stub's ZEROED dummy (ROW_ENV1_TIME) every positive value clamps
    // to 0; the flat write still lands and the reloadADSR arm still fires
    // (old != new). CHARACTERIZATION of the stub, not of the firmware bounds.
    auto* params = synth().getTimbre(0)->getParamRaw();
    const int row = ROW_ENV3_TIME, enc = 0;  // default 0.5 (nonzero) so the clamp write actually lands
    const int index = row * NUMBER_OF_ENCODERS_PFM2 + enc;
    float before = ((const float*) params)[index];
    synth().setNewValueFromMidi(0, row, enc, 1.5f);
    float after = ((const float*) params)[index];
    EXPECT_NE(before, after);
    EXPECT_FLOAT_EQ(after, 0.0f) << "zeroed stub bounds clamp positive values to 0 (characterized)";
    // And the sequencer-independent flat-field route still renders.
    synth().noteOn(0, 69, 100);
    EXPECT_GT(renderBlocks(10), kSilenceThreshold);
}

TEST_F(SynthCore, NewMixerValueCompressorAndMpeSettingArms) {
    // MIXER_VALUE_COMPRESSOR 0..3 -> SimpleComp re-config, asserted through
    // its public parameter getters. GLOBAL_SETTINGS_1 setting 0 updates MPE.
    struct ExpectedComp {
        float ratio;
        float threshold;
        float attack;
        float release;
    };
    const ExpectedComp expected[] = {
        {1.0f, 1000.0f, 10.0f, 100.0f},
        {0.33f, -12.0f, 100.0f, 1000.0f},
        {0.33f, -12.0f, 8.0f, 200.0f},
        {0.33f, -12.0f, 1.0f, 50.0f},
    };
    for (int v = 0; v <= 3; v++) {
        SCOPED_TRACE("compressor preset " + std::to_string(v));
        synth().newMixerValue(MIXER_VALUE_COMPRESSOR, 0, 0.0f,
                              static_cast<float>(v));
        const auto& comp = synth().getCompInstrument(0);
        EXPECT_FLOAT_EQ(comp.getRatio(), expected[v].ratio);
        EXPECT_FLOAT_EQ(comp.getThresh(), expected[v].threshold);
        EXPECT_FLOAT_EQ(comp.getAttack(), expected[v].attack);
        EXPECT_FLOAT_EQ(comp.getRelease(), expected[v].release);
    }
    synth().newMixerValue(MIXER_VALUE_GLOBAL_SETTINGS_1, 0, 0.0f, 3.0f);
    EXPECT_EQ(synth().getTimbre(0)->getMPESetting(), 3);
}

TEST_F(SynthCore, NewMixerValueMidiChannelResetsArpAndReleasesVoices) {
    harness_->enableArpeggiator(0, 120, 0 /*UP*/, 1);
    synth().noteOn(0, 60, 100);
    synth().noteOn(0, 67, 100);
    renderBlocks(50);
    ASSERT_GT(renderBlocks(10), kSilenceThreshold);
    synth().newMixerValue(MIXER_VALUE_MIDI_CHANNEL, 0, 1.0f, 2.0f);
    // The MIDI-channel change resets the arp and note-offs the timbre: the
    // render decays to silence.
    renderUntilSilent(2200);
    EXPECT_FALSE(synth().isPlaying());
}

TEST_F(SynthCore, NewMixerValueNumberOfVoicesEqualValuesIsNoOp) {
    uint8_t before = playCount();
    synth().newMixerValue(MIXER_VALUE_NUMBER_OF_VOICES, 0, 6.0f, 6.0f);
    renderBlocks(2);
    EXPECT_EQ(playCount(), before);
}

TEST_F(SynthCore, NewMixerValueNumberOfVoicesIncreaseAndDecreaseReassignsSlots) {
    Timbre* timbre = synth().getTimbre(0);
    ASSERT_EQ(timbre->voiceNumber_[6], -1);
    synth().newMixerValue(MIXER_VALUE_NUMBER_OF_VOICES, 0, 6.0f, 7.0f);
    EXPECT_GE(timbre->voiceNumber_[6], 0);
    EXPECT_FLOAT_EQ(timbre->getNumberOfVoiceInverse(), 1.0f / 7.0f);

    synth().newMixerValue(MIXER_VALUE_NUMBER_OF_VOICES, 0, 7.0f, 4.0f);
    for (int slot = 4; slot <= 6; slot++) {
        EXPECT_EQ(timbre->voiceNumber_[slot], -1)
            << "removed voice slot " << slot << " remained assigned";
    }
    EXPECT_FLOAT_EQ(timbre->getNumberOfVoiceInverse(), 0.25f);
}

TEST_F(SynthCore, NewMixerValueGlobalFxSettingsArmsComplete) {
    // FxBus has no public coefficient introspection; executing each branch on
    // the real initialized bus is the strongest non-invasive host oracle.
    synth().newMixerValue(MIXER_VALUE_GLOBAL_SETTINGS_3, 0, 0.0f, 1.0f);
    synth().newMixerValue(MIXER_VALUE_GLOBAL_SETTINGS_3, 3, 0.0f, 1.0f);
    synth().newMixerValue(MIXER_VALUE_GLOBAL_SETTINGS_4, 0, 0.0f, 1.0f);
    synth().newMixerValue(MIXER_VALUE_GLOBAL_SETTINGS_5, 0, 0.0f, 1.0f);
    SUCCEED() << "global FX settings dispatched on the real FxBus";
}

// ===========================================================================
TEST_F(SynthCore, HostileSendAboveOneClampsDryPathToZero) {
    // Regression (bugfix-phase3 3.10): the dry-output path computed
    // panTable[(int)((1 - send) * 255)] — send > 1 made the index negative and
    // read below the table (UB/OOB). A finite negative derived index must clamp
    // to 0; only non-finite input takes the fail-audible index-255 path.
    // Driven via the production CC-routing entry point
    // Synth::setNewMixerValueFromMidi(MIXER_VALUE_SEND) -> newMixerValue,
    // which writes mixerState.instrumentState_[0].send; output observed at
    // Synth::buildNewSampleBlock level. Value > 1 is unreachable from a
    // normal MIDI CC (0..127/127 <= 1) — it models a corrupt preset/state.
    harness_->setReverbLevel(0.0f);  // isolate the dry path from FxBus output
    synth().noteOn(0, 60, 100);
    ASSERT_GT(renderBlocks(2), kSilenceThreshold);
    synth().setNewMixerValueFromMidi(0, MIXER_VALUE_SEND, 2.0f);
    EXPECT_LE(renderBlocks(4), kSilenceThreshold)
        << "finite send=2 derives -255 and must clamp to panTable[0]";
    synth().noteOff(0, 60);
}

TEST_F(SynthCore, HostileNanSendRendersDefinedDryOutput) {
    // Review follow-up to 3.10: the clamp ran AFTER the (int) cast, but
    // (int)NaN and (int)infinity are undefined float-to-int conversions
    // (UBSan float-cast-overflow) -- the clamp can never repair them. The
    // index is now derived only after a float-domain range check, and a
    // non-finite send fails AUDIBLE (treated as send 0: full dry, index
    // 255 -- panTable[0] is zero, so index 0 would mute the timbre). Valid
    // sends in [0, 1] render byte-identically.
    harness_->setReverbLevel(0.0f);  // non-finite send must still retain dry audio
    synth().noteOn(0, 60, 100);
    renderBlocks(2);
    synth().setNewMixerValueFromMidi(0, MIXER_VALUE_SEND, NAN);
    const int64_t m = renderBlocks(4);
    EXPECT_GT(m, kSilenceThreshold) << "full-dry pan, note should be audible";
    EXPECT_LT(m, (int64_t)1 << 30);
    synth().noteOff(0, 60);
}

// 8. midiClock routing (Synth::midiClock* — tellSequencer=false arms).
// ===========================================================================

TEST_F(SynthCore, MidiClockRoutingTellSequencerFalseIsSafe) {
    // The harness wires a real Sequencer, but the false arms must not touch
    // it: start / 24 ticks / song-position steps / continue / stop must
    // complete cleanly and leave a plain note rendering finite and sounding.
    synth().midiClockStart(false);
    for (int i = 0; i < 24; i++) synth().midiTick(false);
    synth().midiClockSongPositionStep(4);
    synth().midiClockContinue(2, false);
    synth().midiClockStop(false);
    synth().midiClockSetSongPosition(8, false);
    synth().noteOn(0, 69, 100);
    EXPECT_GT(renderBlocks(10), kSilenceThreshold);
    SUCCEED() << "midiClock routing arms exercised without crash";
}

TEST_F(SynthCore, MidiClockRoutingTellSequencerTrueDrivesWiredSequencer) {
    // The golden harness wires a real Sequencer + stub display exactly as
    // preenfm3.cpp does, so the true arms are safe to exercise too (the
    // midi_decoder Phase-2 tests drive the same sequencer via clock bytes).
    // Oracle: nothing is playing and no FX tail exists, so the render must be
    // EXACTLY zero — clock ticks routing a ghost note into the sequencer
    // (e.g. an uninitialized step gate) would produce non-zero audio here.
    synth().midiClockStart(true);
    for (int i = 0; i < 48; i++) synth().midiTick(true);
    synth().midiClockStop(true);
    EXPECT_EQ(renderBlocks(2), 0) << "clock ticks produced audio with no note on";
}

TEST_F(SynthCore, MidiClockStepsReachVoiceLfoSyncWithoutCrash) {
    // Timbre::midiClockSongPositionStep fans out to every voice's LFO
    // midi-clock-sync (Voice::midiClockSongPositionStep) and toggles
    // recomputeNext_. No public per-voice LFO phase introspection exists; the
    // lock is "many steps + renders on an LFO-using patch complete, finite,
    // non-silent" (the default preset's LFO1 is active — G0/G3 goldens).
    synth().noteOn(0, 69, 100);
    renderBlocks(5);
    for (int sp = 0; sp < 96; sp++) {
        synth().midiClockSongPositionStep(sp);
        renderBlock();
    }
    EXPECT_GT(renderBlocks(4), kSilenceThreshold);
}

// ===========================================================================
// Bonus (spec Design Notes): beforeNewParamsLoad keeps-note recall quirk.
// ===========================================================================

TEST_F(SynthCore, BeforeAfterNewParamsLoadRestoresSoundingNote) {
    // beforeNewParamsLoad snapshots non-released playing notes, quick-offs all
    // voices; afterNewParamsLoad re-onNotes them. Observable: the note is
    // still sounding after the reload round-trip.
    synth().noteOn(0, 69, 100);
    renderBlocks(10);
    ASSERT_GT(renderBlocks(2), kSilenceThreshold);
    synth().beforeNewParamsLoad(0);
    synth().afterNewParamsLoad(0);
    renderBlocks(10);
    EXPECT_GT(renderBlocks(4), kSilenceThreshold)
        << "sounding note was lost across before/afterNewParamsLoad";
    EXPECT_TRUE(synth().isPlaying());
}

}  // namespace
