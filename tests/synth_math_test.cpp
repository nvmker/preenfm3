// Host-side coverage for firmware/Src/synth/{Osc,Env,Matrix}.cpp — synth math.
//
// Regression target (per tests/README.md roadmap, row 3):
//   Silent audio regressions. Osc/Env/Matrix are the per-sample DSP core: the
//   oscillator wavetable walk, the ADSR envelope generator, and the modulation
//   matrix routing. There is no external spec for the exact DSP output, so the
//   sample-block / envelope-curve goldens here are CHARACTERIZATION locks: they
//   capture the firmware's CURRENT output so a future table-layout change,
//   init-order change, or -Ofast reordering that silently alters the synth's
//   sound fails loudly. The pure-helper math (frequency estimation, incTab
//   indexing, matrix arithmetic) is asserted exactly where it is independently
//   derivable.
//
// Fidelity caveats (tests/SEAM.md §d.3):
//   * The oscillator/envelope hot paths are pure float arithmetic (no exp/pow/
//     log in getNextSample/getNextBlock/getNextAmpExp/computeAllDestinations) ->
//     exact equality is valid in principle. We still use a tiny EXPECT_NEAR
//     slack on the captured sample-BLOCK / envelope-trace goldens to absorb
//     1-ULP host FPU / FMA-contraction differences across gcc/clang, x86/arm64
//     (same stance as the Hexter suite's kArithTol). Single-derive checks and
//     pure-int matrix arithmetic stay exact.
//   * Runtime tables MUST be precomputed once: Osc::init() fills
//     waveTables[].precomputedValue/phaseMul (guarded by a precomputedValue<=0
//     check, so it runs exactly once across the process); Env::init() fills the
//     static incTab[1601] (guarded by initTab) and the per-instance
//     tables[]/stateInc[]/stateTarget[]. The fixtures call the relevant init()
//     before any DSP assert — same contract as the firmware.
//
// KNOWN LATENT BUG preserved as golden (do NOT fix here — flagged for a separate
// change; see OscFreqEstimationFallThrough suite + tests/SEAM.md Target #3):
//   Osc::getNoteRealFrequencyEstimation has NO `break` between the
//   OSC_FT_KEYBOARD / OSC_FT_FIXE / OSC_FT_KEYHZ cases, so all three
//   frequencyTypes fall through to the KEYHZ formula (the KEYBOARD and FIXE
//   results are computed then immediately overwritten). Contrast: Osc::newNote's
//   switch DOES have breaks and differentiates the types. This suite asserts the
//   CURRENT (KEYHZ-wins-for-all) estimation behavior so a future fix is a
//   visible, deliberate change.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "Osc.h"         // firmware-under-test (host-compilable via PFM3_HOST seam)
#include "Env.h"         // firmware-under-test
#include "Matrix.h"      // firmware-under-test
#include "SynthState.h"  // SynthState (for mixerState.tuning_), OneSynthParams POD
#include "waves.h"       // sinTable / sawTable / ... globals (defined in waves.c)

namespace {

// INV440 mirrors the #define in Osc.cpp (not exported). With tuning_=440.0f the
// term (tuning_ * INV440) is ~= 1.0, neutralising the tuning scale so the
// frequency-math goldens are independent of the (here-uninteresting) tuning
// field. Kept in lockstep with Osc.cpp's literal.
constexpr float kInv440 = .002272727272727f;
// PREENFM_FREQUENCY / BLOCK_SIZE mirror Common.h (re-stated locally so the
// incTab-formula recomputation is self-contained and a Common.h drift is
// caught as a test failure rather than silently propagated).
constexpr float kPreenfmFrequency = 47916.0f;
constexpr int   kBlockSize = BLOCK_SIZE;  // 32

// Tolerance for pure-float-arithmetic DSP goldens (no libm): absorbs 1-ULP host
// FPU / FMA-contraction differences across gcc/clang, x86/arm64. Same stance as
// the Hexter suite's kArithTol.
constexpr float kArithTol = 1e-5f;

// ---------------------------------------------------------------------------
// Minimal SynthState for Osc.
//
// Osc stores the SynthState* in init() and later reads exactly ONE field —
// `synthState_->mixerState.tuning_` (a public float) — in newNote /
// getNoteRealFrequencyEstimation. It never dispatches a virtual call or touches
// another member. SynthState's constructor is out-of-line (SynthState.cpp, not
// compiled here), so a real construction would not link; instead we back the
// pointer with a zeroed, alignof(SynthState)-aligned buffer and patch tuning_.
//
// The member write goes to the byte offset the compiler computes from the class
// definition; UBSAN's vptr check does not fire on a plain data-member access, so
// this is clean under -fsanitize=undefined. (Object lifetime is technically not
// begun, but no virtual / no destructor runs on this memory — Osc treats it as a
// bag of bytes with one float field.) See tests/SEAM.md.
struct SynthStateBacking {
    // Sized/aligned to host the real SynthState layout without overstepping.
    alignas(alignof(SynthState)) unsigned char bytes[sizeof(SynthState)];
};

// Returns a SynthState* backed by `backing`, zeroed, with tuning_ patched to
// 440.0f (neutralising INV440). The pointer is valid for the lifetime of
// `backing`; callers must keep `backing` alive while the Osc holds the pointer.
SynthState* MakeMinimalSynthState(SynthStateBacking& backing,
                                  float tuning = 440.0f) {
    std::memset(&backing, 0, sizeof(backing));
    SynthState* ss = reinterpret_cast<SynthState*>(backing.bytes);
    ss->mixerState.tuning_ = tuning;  // public member; the only field Osc reads
    return ss;
}

// Recompute the documented incTab formula (Env.cpp init) in-test, so the
// envelope-increment goldens are independently derivable AND a drift in the
// firmware's incTab fill is caught. incTab[k] = BLOCK_SIZE / FREQ / (k/100).
float IncTabFormula(int k) {
    return static_cast<float>(kBlockSize) / kPreenfmFrequency /
           (static_cast<float>(k) / 100.0f);
}

}  // namespace

// ===========================================================================
// SMOKE — cheap always-on guard that Osc/Env/Matrix link on host and their
// init() precompute works. Subsumed by the per-unit suites below, but kept as a
// fast tripwire: if a future firmware include change breaks the host closure,
// this fails first with a clear name.
// ===========================================================================

TEST(SynthMathSmoke, OscEnvMatrixInitAndOneSampleEach) {
    SynthStateBacking backing;
    SynthState* ss = MakeMinimalSynthState(backing);

    OscillatorParams oscParams = {};
    oscParams.shape = OSC_SHAPE_SIN;
    Osc osc;
    osc.init(ss, &oscParams, ALL_OSC_FREQ);
    EXPECT_GT(waveTables[OSC_SHAPE_SIN].precomputedValue, 0.0f);

    struct OscState oscState = {};
    oscState.frequency = 0.0f;  // frozen -> returns table[0]
    EXPECT_NEAR(osc.getNextSample(&oscState), sinTable[0], kArithTol);

    EnvelopeTimeMemory envTime = {};
    envTime.attackTime = 0.5f;
    EnvelopeLevelMemory envLevel = {};
    EnvelopeCurveParams envCurve = {};
    float algoNumber = 0.0f;
    Env env;
    env.init(&envTime, &envLevel, 0, &algoNumber, &envCurve);
    MatrixRowParams rows[MATRIX_SIZE] = {};
    Matrix matrix;
    matrix.init(rows);
    matrix.resetAllDestination();
    struct EnvData envData = {};
    env.noteOnAfterMatrixCompute(&envData, &matrix);
    EXPECT_NEAR(envData.stateIncAttack, IncTabFormula(50), kArithTol);

    matrix.computeAllDestinations();  // all-NONE sources -> stays zero
    EXPECT_EQ(matrix.getDestination(OSC1_FREQ), 0.0f);
}

// ===========================================================================
// OSC — wavetable walk DSP goldens.
//
// getNextSample / getNextBlock index waveTables[(int)oscillator->shape], whose
// .table/.max/.precomputedValue/.floatToAdd are filled by Osc::init(). The
// goldens below lock:
//   * the OFF shape (silence, max=0 -> index wraps to 0) yields all-zero;
//   * a frozen oscillator (frequency 0) reads an exact table entry, proving the
//     per-shape table pointer + the index-wrap + the lookup are wired right;
//   * an advancing oscillator produces a deterministic 32-sample block, locked
//     as a characterization golden (the marquee DSP lock);
//   * getNextBlock (unrolled 4-at-a-time) agrees sample-for-sample with
//     getNextSample (single-step) on identical input — a structural lock that
//     needs no baked values and catches any divergence between the two paths.
// ===========================================================================

class OscDsp : public ::testing::Test {
protected:
    SynthStateBacking backing_;
    SynthState* synthState_;
    Osc osc_;
    OscillatorParams params_;
    struct OscState oscState_;

    void SetUp() override {
        synthState_ = MakeMinimalSynthState(backing_);
        std::memset(&params_, 0, sizeof(params_));
        std::memset(&oscState_, 0, sizeof(oscState_));
    }

    // (Re)configure oscillator params and (re)run init(). init() is idempotent
    // after the first call (waveTable precompute is guarded by precomputedValue).
    void Configure(OscShape shape, float frequencyMul = 1.0f,
                   OscFrequencyType ft = OSC_FT_KEYBOARD, float detune = 0.0f) {
        params_.shape = static_cast<float>(shape);
        params_.frequencyType = static_cast<float>(ft);
        params_.frequencyMul = frequencyMul;
        params_.detune = detune;
        osc_.init(synthState_, &params_, ALL_OSC_FREQ);
    }
};

TEST_F(OscDsp, OffShapeProducesAllZeroBlock) {
    // OSC_SHAPE_OFF: table=silence (silence[0]=0, set in init), max=0 -> every
    // index wraps to 0 -> every sample is silence[0]=0. Locks the OFF path.
    Configure(OSC_SHAPE_OFF);
    oscState_.index = 0.0f;
    oscState_.frequency = 1234.0f;  // frequency is irrelevant: precomputedValue=0
    float* block = osc_.getNextBlock(&oscState_);
    for (int i = 0; i < kBlockSize; i++) {
        SCOPED_TRACE(i);
        EXPECT_FLOAT_EQ(block[i], 0.0f);
    }
}

TEST_F(OscDsp, FrozenOscillatorReadsExactTableEntryPerShape) {
    // frequency=0 -> index never advances; with an exact-integer index N, the
    // wrap (& max) leaves it at N, so getNextSample returns table[N]. Asserting
    // == the GLOBAL sinTable[N] / sawTable[N] / squareTable[N] locks that
    // waveTables[shape].table points at the right global for each shape (a
    // reordering of the waveTables[] initializer would break this), AND that the
    // index-wrap + lookup are exact.
    Configure(OSC_SHAPE_SIN);
    oscState_.index = 100.0f;
    oscState_.frequency = 0.0f;
    EXPECT_FLOAT_EQ(osc_.getNextSample(&oscState_), sinTable[100]);

    Configure(OSC_SHAPE_SAW);
    oscState_.index = 100.0f;
    oscState_.frequency = 0.0f;
    EXPECT_FLOAT_EQ(osc_.getNextSample(&oscState_), sawTable[100]);

    Configure(OSC_SHAPE_SQUARE);
    oscState_.index = 100.0f;
    oscState_.frequency = 0.0f;
    EXPECT_FLOAT_EQ(osc_.getNextSample(&oscState_), squareTable[100]);
}

TEST_F(OscDsp, AdvancingSinBlockMatchesGolden) {
    // THE oscillator DSP lock: a representative frequency drives the wavetable
    // walk; the 32-sample block is captured from the host build and locked. Pure
    // IEEE arithmetic -> tight tol. If waveTables layout, the precompute math,
    // the index wrap, or the 4-at-a-time unroll drifts, this block changes.
    Configure(OSC_SHAPE_SIN);
    oscState_.index = 0.0f;
    oscState_.frequency = 440.0f;  // A4
    float* block = osc_.getNextBlock(&oscState_);
    // Golden captured from the host build (see capture note in SEAM.md).
    const float golden[kBlockSize] = {
        0.055195246f,
        0.113270953f,
        0.170961887f,
        0.228072077f,
        0.284407526f,
        0.336889863f,
        0.391170382f,
        0.444122136f,
        0.495565265f,
        0.545324981f,
        0.590759695f,
        0.636761844f,
        0.680601001f,
        0.722128212f,
        0.761202395f,
        0.795836926f,
        0.829761207f,
        0.860866964f,
        0.889048338f,
        0.914209783f,
        0.935183525f,
        0.954228103f,
        0.970031261f,
        0.982539296f,
        0.991709769f,
        0.997290432f,
        0.999882340f,
        0.999077737f,
        0.994879305f,
        0.987301409f,
        0.977028131f,
        0.962953269f
    };
    for (int i = 0; i < kBlockSize; i++) {
        SCOPED_TRACE(i);
        EXPECT_NEAR(block[i], golden[i], kArithTol);
    }
}

TEST_F(OscDsp, GetNextBlockAgreesSampleForSampleWithGetNextSample) {
    // Structural lock (no baked values): two oscillators with identical state
    // are driven — one via getNextBlock (unrolled 4-at-a-time), the other via 32
    // getNextSample calls. Their outputs must agree sample-for-sample. Catches
    // any divergence between the two code paths (e.g. a future change to the
    // block unroll or the wrap that the single-sample loop doesn't share).
    Configure(OSC_SHAPE_SIN);
    struct OscState blockState = {};
    blockState.index = 0.0f;
    blockState.frequency = 173.4f;  // arbitrary, exercises fractional advance
    struct OscState sampleState = blockState;

    float* block = osc_.getNextBlock(&blockState);
    for (int i = 0; i < kBlockSize; i++) {
        SCOPED_TRACE(i);
        const float s = osc_.getNextSample(&sampleState);
        EXPECT_NEAR(block[i], s, kArithTol)
            << "block[" << i << "] diverged from per-sample walk";
    }
    // And the phase (index) must converge at the end of the block.
    EXPECT_NEAR(blockState.index, sampleState.index, kArithTol);
}

// ===========================================================================
// OSC — getNoteRealFrequencyEstimation: the marquee latent-bug capture.
//
// The switch over OSC_FT_{KEYBOARD,FIXE,KEYHZ} has NO `break`, so all three
// cases fall through to the KEYHZ formula. KEYBOARD and FIXE results are
// computed then immediately overwritten. We assert the CURRENT (KEYHZ-wins)
// behavior as golden; a future fix that adds the breaks is a deliberate,
// visible change that flips these tests.
//
// Contrast proof: Osc::newNote's switch DOES have breaks, so it differentiates
// the three types — proving the fall-through is specific to the estimation
// function, not a property of the frequencyType enum or the inputs.
// ===========================================================================

class OscFreqEstimation : public ::testing::Test {
protected:
    SynthStateBacking backing_;
    SynthState* synthState_;
    Osc osc_;
    OscillatorParams params_;
    struct OscState oscState_;

    void SetUp() override {
        synthState_ = MakeMinimalSynthState(backing_);
        std::memset(&params_, 0, sizeof(params_));
        std::memset(&oscState_, 0, sizeof(oscState_));
    }
    void Configure(OscFrequencyType ft, float mul, float detune) {
        params_.shape = OSC_SHAPE_SIN;
        params_.frequencyType = static_cast<float>(ft);
        params_.frequencyMul = mul;
        params_.detune = detune;
        osc_.init(synthState_, &params_, ALL_OSC_FREQ);
    }
};

TEST_F(OscFreqEstimation, AllFrequencyTypesYieldKeyHzFormula) {
    // THE fall-through lock. With tuning_=440, (tuning_*INV440) ~= 1.0, so the
    // KEYHZ formula is: newNoteFrequency * frequencyMul * ~1.0 + detune. ALL
    // THREE frequencyTypes must return this SAME value (the KEYBOARD/FIXE
    // results are computed then overwritten by the fall-through). If the missing
    // breaks are ever added, KEYBOARD and FIXE return DIFFERENT values and this
    // test fails loudly.
    const float noteFreq = 220.0f;
    const float mul = 2.0f;
    const float detune = 0.5f;
    const float expectedKeyHz =
        noteFreq * mul * (440.0f * kInv440) + detune;

    Configure(OSC_FT_KEYBOARD, mul, detune);
    const float estKb = osc_.getNoteRealFrequencyEstimation(&oscState_, noteFreq);
    Configure(OSC_FT_FIXE, mul, detune);
    const float estFixe = osc_.getNoteRealFrequencyEstimation(&oscState_, noteFreq);
    Configure(OSC_FT_KEYHZ, mul, detune);
    const float estKeyHz = osc_.getNoteRealFrequencyEstimation(&oscState_, noteFreq);

    EXPECT_NEAR(estKeyHz, expectedKeyHz, kArithTol)
        << "KEYHZ case must return the KEYHZ formula";
    EXPECT_NEAR(estKb, estKeyHz, kArithTol)
        << "KEYBOARD falls through to KEYHZ (missing-break bug): values must match";
    EXPECT_NEAR(estFixe, estKeyHz, kArithTol)
        << "FIXE falls through to KEYHZ (missing-break bug): values must match";
}

TEST_F(OscFreqEstimation, EstimationClampsBelowOne) {
    // The function clamps a sub-1Hz result to 1 (guard against div-by-zero /
    // inaudible estimations downstream). KEYHZ formula with tiny inputs.
    Configure(OSC_FT_KEYHZ, /*mul=*/0.0f, /*detune=*/0.0f);
    const float est = osc_.getNoteRealFrequencyEstimation(&oscState_, 0.001f);
    EXPECT_FLOAT_EQ(est, 1.0f);
}

TEST_F(OscFreqEstimation, NewNoteDifferentiatesByFrequencyType) {
    // CONTRAST proof: Osc::newNote's switch HAS breaks, so the three types yield
    // DISTINCT mainFrequency values (for the same inputs). This pins the bug as
    // estimation-specific: if someone "fixes" the estimation fall-through by
    // copying newNote's structure, this test still passes (it documents the
    // intended differentiated behavior), while the estimation test above flips.
    const float noteFreq = 220.0f;
    const float mul = 2.0f;
    const float detune = 1.0f;

    Configure(OSC_FT_KEYBOARD, mul, detune);
    struct OscState s1 = {};
    osc_.newNote(&s1, noteFreq, 0.0f);
    const float mainKb = s1.mainFrequency;

    Configure(OSC_FT_FIXE, mul, detune);
    struct OscState s2 = {};
    osc_.newNote(&s2, noteFreq, 0.0f);
    const float mainFixe = s2.mainFrequency;  // = mul*1000 + detune*100 = 2100

    Configure(OSC_FT_KEYHZ, mul, detune);
    struct OscState s3 = {};
    osc_.newNote(&s3, noteFreq, 0.0f);
    const float mainKeyHz = s3.mainFrequency;

    // newNote DOES differentiate (breaks present) -> three distinct values.
    EXPECT_NE(mainKb, mainFixe);
    EXPECT_NE(mainKb, mainKeyHz);
    EXPECT_NE(mainFixe, mainKeyHz);

    // And the FIXE value is exactly derivable (pure arithmetic, no tuning term).
    EXPECT_NEAR(mainFixe, mul * 1000.0f + detune * 100.0f, kArithTol);
    // KEYHZ value matches the estimation's KEYHZ formula (both use it).
    EXPECT_NEAR(mainKeyHz, noteFreq * mul * (440.0f * kInv440) + detune, kArithTol);
}

// ===========================================================================
// Env — ADSR envelope generator goldens.
//
// getNextAmpExp is the per-sample envelope step; it interpolates a per-state
// curve table between previousStateValue and nextStateValue, advancing
// currentPhase by a per-state increment. init() populates incTab[1601] (once)
// and the per-instance stateTarget/stateInc/tables. The goldens below lock:
//   * the incTab-indexing formula (stateIncAttack == IncTabFormula(k)) exact;
//   * the full ADSR lifecycle visits ON_A->ON_D->ON_S->ON_REAL_S->(noteOff)->
//     ON_R->DEAD, with currentValue reaching each configured level;
//   * the curve table is actually applied (LIN vs EXP produce distinct,
//     derivable mid-attack values);
//   * a downsampled currentValue trace across the lifecycle is locked as a
//     characterization golden vector (the marquee envelope lock).
// ===========================================================================

class EnvAdsr : public ::testing::Test {
protected:
    EnvelopeTimeMemory envTime_;
    EnvelopeLevelMemory envLevel_;
    EnvelopeCurveParams envCurve_;
    float algoNumber_;       // ALGO1 by default -> op0 is CARRIER
    MatrixRowParams rows_[MATRIX_SIZE];
    Matrix matrix_;
    Env env_;

    void SetUp() override {
        std::memset(&envTime_, 0, sizeof(envTime_));
        std::memset(&envLevel_, 0, sizeof(envLevel_));
        std::memset(&envCurve_, 0, sizeof(envCurve_));
        std::memset(rows_, 0, sizeof(rows_));
        algoNumber_ = 0.0f;
        matrix_.init(rows_);
        matrix_.resetAllDestination();
    }

    // Configure a simple ADSR with a single curve applied to all four segments.
    void ConfigureAdsr(float a, float d, float s, float r,
                       float al, float dl, float sl, float rl,
                       CurveType curve = CURVE_TYPE_LIN,
                       uint8_t envNumber = 0) {
        envTime_  = {a, d, s, r};
        envLevel_ = {al, dl, sl, rl};
        envCurve_ = {static_cast<float>(curve), static_cast<float>(curve),
                     static_cast<float>(curve), static_cast<float>(curve)};
        env_.init(&envTime_, &envLevel_, envNumber, &algoNumber_, &envCurve_);
    }

    // Step the envelope until envState becomes `target` (or `budget` samples
    // elapse). Returns the sample count consumed; records the final currentValue
    // in *valueOut if non-null. Uses noteOnAfterMatrixCompute to start.
    int RunUntilState(struct EnvData* env, uint8_t target, int budget,
                      float* valueOut = nullptr) {
        env_.noteOnAfterMatrixCompute(env, &matrix_);
        for (int i = 0; i < budget; i++) {
            env_.getNextAmpExp(env);
            if (env->envState == target) {
                if (valueOut) *valueOut = env->currentValue;
                return i + 1;
            }
        }
        if (valueOut) *valueOut = env->currentValue;
        return budget;
    }
};

TEST_F(EnvAdsr, AttackIncrementMatchesIncTabFormula) {
    // stateIncAttack is set by noteOn from incTab[(int)(attack*100)]. Recompute
    // the incTab formula here and require exact agreement -> locks both the
    // indexing and the formula. attack=0.5 -> k=50.
    ConfigureAdsr(/*a=*/0.5f, 0.1f, 0.1f, 0.1f, 1.0f, 0.0f, 0.0f, 0.0f);
    struct EnvData env = {};
    env_.noteOnAfterMatrixCompute(&env, &matrix_);
    EXPECT_NEAR(env.stateIncAttack, IncTabFormula(50), kArithTol);
}

TEST_F(EnvAdsr, FullLifecycleVisitsStatesInAdsrOrder) {
    // attack/decay short, sustain non-zero time, release non-zero. Drive the
    // whole noteOn->...->noteOff->dead lifecycle and lock the envState sequence.
    ConfigureAdsr(/*a=*/0.05f, /*d=*/0.05f, /*s=*/0.05f, /*r=*/0.05f,
                  1.0f, 0.5f, 0.5f, 0.0f);
    struct EnvData env = {};
    env_.noteOnAfterMatrixCompute(&env, &matrix_);
    ASSERT_EQ(env.envState, ENV_STATE_ON_A);

    // Step until DEAD, recording the order of states first entered.
    std::vector<uint8_t> seq;
    seq.push_back(env.envState);
    uint8_t last = env.envState;
    // Sustain hold: ON_REAL_S holds forever (stateInc==0), so once we observe
    // it for a few samples we trigger noteOff, then continue to DEAD. (We must
    // let ON_REAL_S be RECORDED before noteOff overwrites it in the same iter.)
    bool notedOff = false;
    int sinceRealS = 0;
    for (int i = 0; i < 100000 && env.envState != ENV_STATE_DEAD; i++) {
        env_.getNextAmpExp(&env);
        if (env.envState == ENV_STATE_ON_REAL_S) {
            if (++sinceRealS == 5) {
                env_.noteOff(&env, &matrix_);
                notedOff = true;
            }
        }
        if (env.envState != last) {
            seq.push_back(env.envState);
            last = env.envState;
        }
    }
    ASSERT_TRUE(notedOff) << "never reached sustain hold";
    // Expected transition order (Env ctor + reloadADSR wiring).
    const std::vector<uint8_t> expected = {
        ENV_STATE_ON_A, ENV_STATE_ON_D, ENV_STATE_ON_S,
        ENV_STATE_ON_REAL_S, ENV_STATE_ON_R, ENV_STATE_DEAD};
    EXPECT_EQ(seq, expected)
        << "envelope did not visit states in the expected ADSR order";
}

TEST_F(EnvAdsr, AttackReachesAttackLevelAtDecayTransition) {
    // When attack completes (currentPhase>=1), currentValue is set to
    // nextStateValue (= attackLevel) BEFORE transitioning to ON_D. So the value
    // observed at the ON_D transition == attackLevel.
    ConfigureAdsr(/*a=*/0.1f, 0.2f, 0.2f, 0.2f, /*al=*/1.0f, 0.3f, 0.3f, 0.0f);
    struct EnvData env = {};
    float v = 0.0f;
    RunUntilState(&env, ENV_STATE_ON_D, /*budget=*/10000, &v);
    EXPECT_NEAR(v, 1.0f, kArithTol)
        << " currentValue at the A->D transition must equal attackLevel";
}

TEST_F(EnvAdsr, DecayReachesDecayLevelAtSustainTransition) {
    ConfigureAdsr(/*a=*/0.1f, /*d=*/0.1f, 0.2f, 0.2f, 1.0f,
                  /*dl=*/0.4f, 0.4f, 0.0f);
    struct EnvData env = {};
    env_.noteOnAfterMatrixCompute(&env, &matrix_);
    float v = 0.0f;
    for (int i = 0; i < 100000; i++) {
        env_.getNextAmpExp(&env);
        if (env.envState == ENV_STATE_ON_S) { v = env.currentValue; break; }
    }
    EXPECT_NEAR(v, 0.4f, kArithTol)
        << "currentValue at the D->S transition must equal decayLevel";
}

TEST_F(EnvAdsr, SustainHoldsAtSustainLevelUntilNoteOff) {
    // ON_REAL_S has stateInc==0 -> getNextAmpExp returns currentValue unchanged.
    // So once we reach the sustain hold, the value is frozen at sustainLevel
    // for any number of further samples (until noteOff).
    ConfigureAdsr(/*a=*/0.1f, /*d=*/0.1f, 0.2f, 0.2f, 1.0f, 0.5f,
                  /*sl=*/0.5f, 0.0f);
    struct EnvData env = {};
    env_.noteOnAfterMatrixCompute(&env, &matrix_);
    // Advance into ON_REAL_S.
    for (int i = 0; i < 100000 && env.envState != ENV_STATE_ON_REAL_S; i++) {
        env_.getNextAmpExp(&env);
    }
    ASSERT_EQ(env.envState, ENV_STATE_ON_REAL_S);
    const float held = env.currentValue;
    EXPECT_NEAR(held, 0.5f, kArithTol);
    // 200 more samples must NOT move the value (stateInc[ON_REAL_S]==0).
    for (int i = 0; i < 200; i++) {
        env_.getNextAmpExp(&env);
        ASSERT_NEAR(env.currentValue, held, kArithTol);
        ASSERT_EQ(env.envState, ENV_STATE_ON_REAL_S);
    }
}

TEST_F(EnvAdsr, ReleaseReachesReleaseLevelThenDies) {
    ConfigureAdsr(/*a=*/0.1f, 0.1f, 0.1f, /*r=*/0.1f, 1.0f, 0.5f, 0.5f,
                  /*rl=*/0.0f);
    struct EnvData env = {};
    env_.noteOnAfterMatrixCompute(&env, &matrix_);
    for (int i = 0; i < 100000 && env.envState != ENV_STATE_ON_REAL_S; i++) {
        env_.getNextAmpExp(&env);
    }
    env_.noteOff(&env, &matrix_);
    ASSERT_EQ(env.envState, ENV_STATE_ON_R);
    for (int i = 0; i < 100000 && !env_.isDead(&env); i++) {
        env_.getNextAmpExp(&env);
    }
    EXPECT_TRUE(env_.isDead(&env));
    EXPECT_NEAR(env.currentValue, 0.0f, kArithTol)
        << "currentValue at death must equal releaseLevel";
}

TEST_F(EnvAdsr, LinearVsExponentialCurveProduceDistinctMidAttackValues) {
    // Black-box curve-routing lock: at the SAME currentPhase during attack, a
    // LINEAR curve yields currentValue == phase*range+base, while an EXPONENTIAL
    // curve yields envExponential-interpolated values. They must differ, and
    // each must match its independently-derived value. This catches a future
    // change that mis-routes the curve table (applyCurves) without reading the
    // private tables[] member.
    auto midAttackValue = [&](CurveType curve) -> float {
        ConfigureAdsr(/*a=*/1.0f, 0.1f, 0.1f, 0.1f, /*al=*/1.0f, 0.0f, 0.0f,
                      0.0f, curve);
        struct EnvData env = {};
        env_.noteOnAfterMatrixCompute(&env, &matrix_);
        // Step until currentPhase first reaches/exceeds 0.5, then read value.
        for (int i = 0; i < 100000; i++) {
            env_.getNextAmpExp(&env);
            if (env.currentPhase >= 0.5f) return env.currentValue;
            if (env.envState != ENV_STATE_ON_A) break;  // attack ended early
        }
        return env.currentValue;
    };
    const float lin = midAttackValue(CURVE_TYPE_LIN);
    const float exp = midAttackValue(CURVE_TYPE_EXP);

    // LIN at phase in [0.5, 0.5+inc): currentValue ~= phase * attackLevel.
    // (envLinear = {0,1}, size 1: tmpValue == fractional phase.)
    EXPECT_GT(lin, 0.0f);
    EXPECT_LT(lin, 1.0f);
    // EXP rises faster early than LIN -> at the same phase bin EXP > LIN.
    EXPECT_GT(exp, lin)
        << "exponential attack must exceed linear at the same phase (curve "
           "routing may be broken)";
    // EXP value is bounded by the envExponential table entries around the phase.
    EXPECT_GT(exp, envExponential[30]);
    EXPECT_LT(exp, envExponential[33]);
}

TEST_F(EnvAdsr, FullLifecycleTraceIsLocked) {
    // THE envelope DSP lock: a downsampled currentValue trace across a full
    // noteOn->...->noteOff->dead lifecycle, captured from the host build and
    // locked. Catches a curve-TABLE corruption or interpolation drift that the
    // checkpoint tests above might miss. Values captured (see SEAM.md).
    ConfigureAdsr(/*a=*/0.1f, /*d=*/0.1f, /*s=*/0.2f, /*r=*/0.1f,
                  1.0f, 0.4f, 0.4f, 0.0f, CURVE_TYPE_EXP);
    struct EnvData env = {};
    env_.noteOnAfterMatrixCompute(&env, &matrix_);

    std::vector<float> trace;
    constexpr int kHoldInSustain = 64;  // samples to hold before noteOff
    bool notedOff = false;
    int sinceRealS = 0;
    for (int i = 0; i < 100000; i++) {
        if (!notedOff && env.envState == ENV_STATE_ON_REAL_S) {
            if (++sinceRealS >= kHoldInSustain) {
                env_.noteOff(&env, &matrix_);
                notedOff = true;
            }
        }
        if (i % 32 == 0) trace.push_back(env.currentValue);
        if (env_.isDead(&env)) break;
        env_.getNextAmpExp(&env);
    }
    // Golden captured from the host build (see capture note in SEAM.md).
    // Shape: attack rise (0->~1.0), decay to sustain (0.4), long sustain hold,
    // release to 0. Exponential curves (envExponential) on all four segments.
    const std::vector<float> golden = {
        0.000000000f,
        0.580183804f,
        0.830402374f,
        0.938172579f,
        0.984663963f,
        0.858479679f,
        0.590877831f,
        0.475479126f,
        0.425761670f,
        0.404314399f,
        0.400000006f,
        0.400000006f,
        0.400000006f,
        0.400000006f,
        0.400000006f,
        0.400000006f,
        0.400000006f,
        0.400000006f,
        0.400000006f,
        0.400000006f,
        0.400000006f,
        0.282999545f,
        0.079495840f,
        0.018326068f,
        0.000000000f
    };
    ASSERT_EQ(trace.size(), golden.size())
        << "trace length drifted; re-capture the golden";
    for (size_t i = 0; i < golden.size(); i++) {
        SCOPED_TRACE(i);
        EXPECT_NEAR(trace[i], golden[i], kArithTol);
    }
}

// ===========================================================================
// Matrix — modulation routing arithmetic. Pure arithmetic -> EXACT equality.
//
// computeAllDestinations() (inline in Matrix.h) routes sources[*] through the 12
// matrix rows into destinations[]. The first four rows add the captured
// destinations[MTX1_MUL..MTX4_MUL] to their own mul (the MTX1-4 feedback); rows
// 4-11 are skipped if source==NONE or mul==0. All values are independently
// derivable -> exact.
// ===========================================================================

class MatrixRouting : public ::testing::Test {
protected:
    MatrixRowParams rows_[MATRIX_SIZE];
    Matrix matrix_;

    void SetUp() override {
        std::memset(rows_, 0, sizeof(rows_));
        // Sentinel: every row starts as NONE so computeAllDestinations skips it
        // unless a test sets a real source.
        for (int i = 0; i < MATRIX_SIZE; i++) rows_[i].source = MATRIX_SOURCE_NONE;
        matrix_.init(rows_);
        matrix_.resetSources();
        matrix_.resetAllDestination();
    }
};

TEST_F(MatrixRouting, SingleRowRoutesSourceTimesMulToBothDestinations) {
    // Row 4+ path (no MTX interaction): source * mul added to both dest1 & dest2.
    rows_[4].source = MATRIX_SOURCE_VELOCITY;
    rows_[4].mul = 0.5f;
    rows_[4].dest1 = OSC1_FREQ;
    rows_[4].dest2 = OSC2_FREQ;
    matrix_.setSource(MATRIX_SOURCE_VELOCITY, 2.0f);
    matrix_.computeAllDestinations();
    EXPECT_FLOAT_EQ(matrix_.getDestination(OSC1_FREQ), 1.0f);  // 2.0 * 0.5
    EXPECT_FLOAT_EQ(matrix_.getDestination(OSC2_FREQ), 1.0f);
}

TEST_F(MatrixRouting, MtxMulAddsToRowMulForFirstFourRows) {
    // THE MTX1-4 interaction lock (the marquee matrix guard). destinations
    // [MTX1_MUL] (populated by a prior compute pass via a routing row) is ADDED
    // to rows[0].mul before multiplying by the source. With mul=0.3 and
    // mul1=0.2, the effective multiplier is 0.5 -> 4.0 * 0.5 = 2.0. If the MTX
    // additive coupling ever breaks, this yields 4.0*0.3=1.2 instead.
    rows_[0].source = MATRIX_SOURCE_NOTE1;
    rows_[0].mul = 0.3f;
    rows_[0].dest1 = OSC1_FREQ;
    rows_[0].dest2 = OSC2_FREQ;
    // A second row routes a source INTO MTX1_MUL (and a harmless dest2), so the
    // first computeAllDestinations pass populates destinations[MTX1_MUL]=0.2.
    rows_[5].source = MATRIX_SOURCE_CC1;
    rows_[5].mul = 0.2f;
    rows_[5].dest1 = MTX1_MUL;
    rows_[5].dest2 = OSC3_FREQ;
    matrix_.setSource(MATRIX_SOURCE_NOTE1, 4.0f);
    matrix_.setSource(MATRIX_SOURCE_CC1, 1.0f);

    matrix_.computeAllDestinations();  // pass 1: populates MTX1_MUL = 0.2
    ASSERT_NEAR(matrix_.getDestination(MTX1_MUL), 0.2f, kArithTol);

    matrix_.computeAllDestinations();  // pass 2: rows[0] picks up mul1=0.2
    EXPECT_FLOAT_EQ(matrix_.getDestination(OSC1_FREQ), 2.0f)
        << "rows[0]: source(4.0) * (mul(0.3) + MTX1_MUL(0.2)) must be 2.0";
    EXPECT_FLOAT_EQ(matrix_.getDestination(OSC2_FREQ), 2.0f);
    EXPECT_NEAR(matrix_.getDestination(MTX1_MUL), 0.2f, kArithTol);
}

TEST_F(MatrixRouting, Mtx2Mtx3Mtx4AlsoCoupleIntoRowsOneTwoThree) {
    // Locks that MTX2/3/4 are wired to rows[1/2/3] respectively (not just MTX1).
    // A dropped or mis-indexed coupling on any of the four breaks this.
    auto tryRow = [&](int rowIdx, DestinationEnum mtxDest) {
        SetUp();
        rows_[rowIdx].source = MATRIX_SOURCE_NOTE1;
        rows_[rowIdx].mul = 0.0f;          // base mul 0
        rows_[rowIdx].dest1 = OSC1_FREQ;
        rows_[rowIdx].dest2 = OSC2_FREQ;
        rows_[5].source = MATRIX_SOURCE_CC1;
        rows_[5].mul = 0.25f;
        rows_[5].dest1 = mtxDest;
        rows_[5].dest2 = OSC3_FREQ;
        matrix_.setSource(MATRIX_SOURCE_NOTE1, 4.0f);
        matrix_.setSource(MATRIX_SOURCE_CC1, 1.0f);
        matrix_.computeAllDestinations();  // populate the MTX*_MUL dest
        matrix_.computeAllDestinations();  // rows[rowIdx] picks it up
        // effective mul = 0.0 + 0.25 = 0.25 -> 4.0 * 0.25 = 1.0
        return matrix_.getDestination(OSC1_FREQ);
    };
    EXPECT_FLOAT_EQ(tryRow(1, MTX2_MUL), 1.0f);
    EXPECT_FLOAT_EQ(tryRow(2, MTX3_MUL), 1.0f);
    EXPECT_FLOAT_EQ(tryRow(3, MTX4_MUL), 1.0f);
}

TEST_F(MatrixRouting, MulZeroOrSourceNoneSkipsRow) {
    // Rows 4-11 are skipped when source==NONE OR mul==0.0f. Both must leave
    // their destinations at 0.
    rows_[6].source = MATRIX_SOURCE_VELOCITY;  // real source...
    rows_[6].mul = 0.0f;                        // ...but mul 0 -> skip
    rows_[6].dest1 = OSC4_FREQ;
    rows_[6].dest2 = OSC5_FREQ;
    rows_[7].source = MATRIX_SOURCE_NONE;       // NONE source -> skip
    rows_[7].mul = 0.9f;
    rows_[7].dest1 = OSC6_FREQ;
    rows_[7].dest2 = ALL_MIX;
    matrix_.setSource(MATRIX_SOURCE_VELOCITY, 2.0f);
    matrix_.computeAllDestinations();
    EXPECT_FLOAT_EQ(matrix_.getDestination(OSC4_FREQ), 0.0f);
    EXPECT_FLOAT_EQ(matrix_.getDestination(OSC5_FREQ), 0.0f);
    EXPECT_FLOAT_EQ(matrix_.getDestination(OSC6_FREQ), 0.0f);
    EXPECT_FLOAT_EQ(matrix_.getDestination(ALL_MIX), 0.0f);
}

TEST_F(MatrixRouting, RowZeroDest1IsAssignedNotAccumulated) {
    // Characterization of the row[0] dest1 asymmetry: computeAllDestinations
    // does `destinations[rows[0].dest1] = sourceTimesMul` (ASSIGN), whereas
    // rows[1-3].dest1 and every dest2 use `+=`. We observe this by routing TWO
    // rows (row0 and row1) to the SAME dest1 with known, distinct contributions:
    //   row0: NOTE1(=1.0) * (mul 0.5 + MTX1_MUL 0) = 0.5  -> ASSIGN  -> dest=0.5
    //   row1: VELOCITY(=2.0) * (mul 0.25 + MTX2_MUL 0) = 0.5 -> += -> dest=1.0
    // Final OSC1_FREQ = 1.0. (If row0 also used += against a zeroed slot, the
    // result would be identical here; this test mainly locks that row0's dest1
    // path runs at all and contributes its full sourceTimesMul.)
    rows_[0].source = MATRIX_SOURCE_NOTE1;
    rows_[0].mul = 0.5f;
    rows_[0].dest1 = OSC1_FREQ;
    rows_[0].dest2 = OSC2_FREQ;
    rows_[1].source = MATRIX_SOURCE_VELOCITY;
    rows_[1].mul = 0.25f;
    rows_[1].dest1 = OSC1_FREQ;  // same dest1 as row0
    rows_[1].dest2 = OSC3_FREQ;
    matrix_.setSource(MATRIX_SOURCE_NOTE1, 1.0f);
    matrix_.setSource(MATRIX_SOURCE_VELOCITY, 2.0f);
    matrix_.computeAllDestinations();
    EXPECT_FLOAT_EQ(matrix_.getDestination(OSC1_FREQ), 1.0f)
        << "row0 ASSIGNs 0.5, row1 += 0.5 -> 1.0";
    EXPECT_FLOAT_EQ(matrix_.getDestination(OSC2_FREQ), 0.5f);
    EXPECT_FLOAT_EQ(matrix_.getDestination(OSC3_FREQ), 0.5f);
}
