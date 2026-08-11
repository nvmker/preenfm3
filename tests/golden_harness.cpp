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

GoldenHarness::GoldenHarness(std::string fixtureDir, int lsbTolerance)
    : fixtureDir_(std::move(fixtureDir)),
      lsbTolerance_(lsbTolerance),
      ss_(nullptr),
      synth_(nullptr) {
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

    // Per-timbre instrumentState: timbre 0 listens on MIDI channel 1, full note
    // range, 6 voices. Timbres 1-5 silenced (numberOfVoices=0) so Synth::init's
    // numberOfVoicesChanged disables them and buildNewSampleBlock skips them.
    for (int t = 0; t < NUMBER_OF_TIMBRES; t++) {
        ss_->mixerState.instrumentState_[t].midiChannel = (t == 0) ? 1 : (t + 1);
        ss_->mixerState.instrumentState_[t].firstNote = 0;
        ss_->mixerState.instrumentState_[t].lastNote = 127;
        ss_->mixerState.instrumentState_[t].shiftNote = 0;
        ss_->mixerState.instrumentState_[t].numberOfVoices = (t == 0) ? 6 : 0;
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

void GoldenHarness::renderA4DefaultSustain(std::size_t nBlocks, int32_t* out) {
    // Canonical G0 note script: noteOn on timbre 0, A4 (MIDI 69), vel 100,
    // before the first render block. No note-off (sustain plateau).
    synth_->noteOn(0, 69, 100);

    int32_t b1[kSamplesPerBuffer];
    int32_t b2[kSamplesPerBuffer];
    int32_t b3[kSamplesPerBuffer];

    for (std::size_t blk = 0; blk < nBlocks; blk++) {
        // buildNewSampleBlock zeroes the 3 buffers itself (verified at
        // Synth.cpp:325-333), so no pre-zero is needed.
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

    // Authoritative tolerance gate.
    bool cmp = goldenCompare(expected.data(), actual, count, lsbTolerance_,
                             kSamplesPerBlock, diffOut);

    // Hash gate over the normalized buffer.
    const uint64_t actualHash = goldenHash(actual, count, lsbTolerance_);
    uint64_t expectedHash = 0;
    const std::string xxhPath = fixturePath(fixtureDir_, id, ".xxh");
    const bool hashOk = readHashFile(xxhPath, &expectedHash);
    const bool hashMatch = hashOk && (actualHash == expectedHash);

    if (cmp && !hashMatch) {
        // Compare passed within tolerance but the normalized hash differs —
        // benign drift crossed a normalization bucket boundary (documented
        // limitation). Report as a mismatch: the hash is a committed lock.
        if (diffOut) {
            diffOut->matched = false;
            diffOut->hashMismatch = true;   // distinguishes from a sample-level mismatch
            diffOut->firstMismatchIndex = 0;
            diffOut->expectedSample = 0;
            diffOut->actualSample = 0;
            diffOut->sampleDelta = 0;
            diffOut->blockIndex = 0;
        }
        std::cerr << "golden: hash mismatch for " << id
                  << " (expected=" << std::hex << expectedHash
                  << " actual=" << actualHash << std::dec
                  << "); goldenCompare passed within tolerance\n";
        cmp = false;
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
    uint64_t h = goldenHash(actual, count, lsbTolerance_);
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

}  // namespace golden
