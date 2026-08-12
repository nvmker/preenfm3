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
    setUpSynthState();
}

GoldenHarness::~GoldenHarness() {
    if (synth_) synth_->~Synth();   // virtual dtor on the placement-new'd object
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
            if (ev.isNoteOn) {
                synth_->noteOn(ev.timbre, ev.note, ev.velocity);
            } else {
                synth_->noteOff(ev.timbre, ev.note);
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
    return { { {0, true, 0, (char)69, (char)100} } };
}

RenderScript RenderScript::envelopeAdsrFull() {
    // Full ADSR: attack+sustain+release. noteOn at block 0, noteOff at block 300
    // (release tail captured across the remaining 300 blocks). Render 600.
    return { { {0,   true,  0, (char)69, (char)100},
               {300, false, 0, (char)69, (char)0} } };
}

RenderScript RenderScript::multiTimbreMix() {
    // Timbres 0 + 1 both noteOn at block 0 (sum 12 <= MAX_NUMBER_OF_VOICES 16).
    // Guards voicesToTimbre mix + per-timbre smoothVolume_ + fxBus->mixAdd.
    return { { {0, true, 0, (char)69, (char)100},
               {0, true, 1, (char)72, (char)100} } };
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
