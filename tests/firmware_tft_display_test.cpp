// Host-side coverage for firmware/Src/hardware/FirmwareTftDisplay.cpp —
// the B1 TFT display-corruption class (spec-b1-tft-display-corruption).
//
// The REAL FirmwareTftDisplay TU is linked (see tests/CMakeLists.txt); the
// base TftDisplay comes from the real lib/Inc/TftDisplay.h via the
// dual-flavor guard trick below, with out-of-line collaborators stubbed in
// tests/stubs/tft_display_collaborators_stub.cpp (base ctor/dtor+vtable,
// tftPalette565, dummy DMA2D registers).
//
// INCLUDE ORDER MATTERS (B1 seam): tests/host_shims/TftDisplay.h is a NO-OP
// shadow of the real header that exists for the filesystem TUs — but this TU
// needs the REAL class (FirmwareTftDisplay derives from it). The shadow
// deliberately reuses the real header's guard (HARDWARE_TFTDISPLAY_H_), so
// pre-including the real header BY PATH first makes every later
// "TftDisplay.h" include (e.g. from FirmwareTftDisplay.h) a no-op and the
// real class wins in this TU. Filesystem test TUs still hit the shadow
// first via the -I order. See the shadow's header comment.
//
// CANARY DESIGN: on the device a backward bgOscillo overflow lands in
// bgColorChar[] (init-once at boot → the persistent-garbage symptom); a
// forward overflow lands in bgAlgo. On the host, adjacent static/heap
// arrays are plain valid memory — ASAN sees nothing — so the fixture flanks
// a 160*100 uint16 box with canary arrays and asserts they are bit-exact
// after every draw. The canaries are sized 12480 elements so they cover the
// FULL reachable index span of the shape draw (oy is an int8_t, so even the
// pre-fix truncated values live in [-128,127] → box-relative index in
// [-12479, 28478]) — "canaries intact" is then a complete out-of-box
// detector, not a spot check.
//
// `#define private public` is scoped around the FirmwareTftDisplay.h
// include only (midi_decoder_test.cpp precedent): every header it reaches
// is already included (TftDisplay.h above), so only the FirmwareTftDisplay
// class body is affected — needed to observe operatorInQueue for the
// off-by-one test. Access specifiers do not change layout (Itanium ABI),
// and the real TU compiles without the macro.
#include "../../lib/Inc/TftDisplay.h"

#include "gtest/gtest.h"

#define private public
#include "FirmwareTftDisplay.h"
#undef private

// CurveType enum (CURVE_TYPE_LIN/EXP/LOG) for the envelope-curve arguments
// — declared in SynthState.h.
#include "SynthState.h"

extern DMA2D_HandleTypeDef hdma2d;                            // stub TU
extern uint16_t tftPalette565[NUMBER_OF_TFT_COLORS];          // stub TU

namespace {

const int kBoxElems = 160 * 100;
// See file header: max backward reach is 8000 + 1 - 128*160 = -12479; max
// forward reach is 8000 + 158 + 127*160 - 16000 = +12478.
const int kCanaryElems = 12480;

// Declaration order guarantees low < box < high (same access, same type):
// a backward overflow hits low[] first, a forward one high[] — exactly the
// bgColorChar / bgAlgo roles from the device memory layout.
struct CanaryLayout {
    uint16_t low[kCanaryElems];
    uint16_t box[kBoxElems];
    uint16_t high[kCanaryElems];
};

uint16_t ExpectedLow(int i) { return (uint16_t)(0xA500 + i); }
uint16_t ExpectedHigh(int i) { return (uint16_t)(0x5A00 + i); }

class TestFirmwareTftDisplay : public FirmwareTftDisplay {
public:
    // TftDisplay::tic()'s dequeue step, mirrored: additionalActions() only
    // executes currentAction, so the pump pops the next queued action first
    // (exactly what tic() does before dispatching). The trailing CR clear
    // stands in for the DMA2D transfer-complete IRQ that clears START on
    // device, so the next step sees a "ready" DMA2D.
    void pumpAction() {
        if (currentAction.actionType == 0 && tftActions.getCount() > 0) {
            currentAction = tftActions.remove();
        }
        additionalActions();
        hdma2d.Instance->CR = 0;
    }

    void pointBgOscillo(uint16_t* p) { bgOscillo = p; }
    void setTftAlgo(TftAlgo* algo) { tftAlgo = algo; }
    void setAction(uint8_t type, uint8_t p1, uint8_t p3) {
        currentAction.actionType = type;
        currentAction.param1 = p1;
        currentAction.param3 = p3;
    }
    int pendingActions() { return tftActions.getCount(); }
    int queuedOperatorDraws() { return operatorInQueue; }
};

class FirmwareTftDisplayTest : public ::testing::Test {
protected:
    void SetUp() override {
        display_.pointBgOscillo(layout_.box);
        display_.setTftAlgo(&algo_);  // oscilloBgDrawEnvelope renders digits
        for (int i = 0; i < kCanaryElems; i++) {
            layout_.low[i] = ExpectedLow(i);
            layout_.high[i] = ExpectedHigh(i);
        }
        // B1 (review finding): sentinel-fill the box so BoxContains is a
        // real oracle — gtest allocates a fresh fixture per test and frees
        // it, so a later test's box can reuse the previous test's memory
        // (containing exactly the palette colors asserted); without the
        // sentinel, "clamps regressed to drawing nothing" could false-pass.
        for (int i = 0; i < kBoxElems; i++) {
            layout_.box[i] = 0x1234;
        }
    }

    void ExpectCanariesIntact() {
        for (int i = 0; i < kCanaryElems; i++) {
            ASSERT_EQ(layout_.low[i], ExpectedLow(i))
                << "bgColorChar-side canary corrupted at element " << i;
            ASSERT_EQ(layout_.high[i], ExpectedHigh(i))
                << "bgAlgo-side canary corrupted at element " << i;
        }
    }

    bool BoxContains(uint16_t color) {
        for (int i = 0; i < kBoxElems; i++) {
            if (layout_.box[i] == color) return true;
        }
        return false;
    }

    TestFirmwareTftDisplay display_;
    TftAlgo algo_;
    CanaryLayout layout_;
};

// --- T1a: oscilloBgDrawOperatorShape clamps ---------------------------------

TEST_F(FirmwareTftDisplayTest, OperatorShapeExtremeValuesStayInsideOscilloBox) {
    // Corrupt-bin garbage far outside the ±1.0 waveform contract.
    // * ±1.1 keeps a plain oy of ±52 (rows 102 / -2): ~650 bytes of canary
    //   on each side — the deterministic pre-fix failure.
    // * ±10.0 exercises the int8_t wrap (pre-fix oy = ∓32 → in-box but
    //   WRONG pixels; post-fix clamps to row 98/1).
    float wf[64];
    for (int i = 0; i < 32; i++) wf[i] = (i % 2 == 0) ? 1.1f : -1.1f;
    for (int i = 32; i < 64; i++) wf[i] = (i % 2 == 0) ? 10.0f : -10.0f;
    display_.initWaveFormExt(0, wf, 64);

    display_.oscilloBgActionOperatorShape(0);
    display_.pumpAction();  // step 0: DMA2D R2M "clear" (dummy regs only)
    display_.pumpAction();  // step 1: the CPU shape draw

    ExpectCanariesIntact();
    // The clipped draw still renders the waveform color inside the box.
    EXPECT_TRUE(BoxContains(tftPalette565[COLOR_YELLOW]));
}

TEST_F(FirmwareTftDisplayTest, OperatorShapeInRangeExtremeSamplesAreNotClipped) {
    // In-range contract boundary: |v| == 1.0 → oy == ±48 must still write
    // rows 98 and 2 — the clamp may not clip in-range values (byte-identical
    // output for legit presets).
    float wf[160];
    for (int x = 0; x < 160; x++) wf[x] = (x < 80) ? 1.0f : -1.0f;
    display_.initWaveFormExt(1, wf, 160);

    display_.oscilloBgActionOperatorShape(1);
    display_.pumpAction();
    display_.pumpAction();

    ExpectCanariesIntact();
    uint16_t yellow = tftPalette565[COLOR_YELLOW];
    // +1.0 half: row 98 (= 50 + 48), columns 1..79 (x = 0..79 samples +1)
    EXPECT_EQ(layout_.box[98 * 160 + 1], yellow);
    EXPECT_EQ(layout_.box[98 * 160 + 40], yellow);
    EXPECT_EQ(layout_.box[98 * 160 + 79], yellow);
    // -1.0 half: row 2 (= 50 - 48), columns 80..158
    EXPECT_EQ(layout_.box[2 * 160 + 80], yellow);
    EXPECT_EQ(layout_.box[2 * 160 + 120], yellow);
    EXPECT_EQ(layout_.box[2 * 160 + 158], yellow);
}

// --- T1a: envelope / env-step / vertical-line / Bresenham clamps ------------

TEST_F(FirmwareTftDisplayTest, EnvelopeLevelBeyondOneStaysInsideOscilloBox) {
    // DX7-import style level 1.5: (int)(1.5 * 97) = 145 drives the vertical
    // lines and curve steps ~7.5 KB BELOW the box start (straight into
    // bgColorChar on device).
    display_.oscilloBgSetEnvelope(0.5f, 0.5f, 0.5f, 0.5f, 1.5f, 1.0f, 0.7f,
            0.3f, CURVE_TYPE_LIN, CURVE_TYPE_LIN, CURVE_TYPE_LIN, CURVE_TYPE_LIN);

    display_.oscilloBgActionEnvelope();
    display_.pumpAction();
    display_.pumpAction();

    ExpectCanariesIntact();
    // The clipped envelope still renders inside the box (yellow curve +
    // dark-grey level markers).
    EXPECT_TRUE(BoxContains(tftPalette565[COLOR_YELLOW]));
    EXPECT_TRUE(BoxContains(tftPalette565[COLOR_DARK_GRAY]));
}

TEST_F(FirmwareTftDisplayTest, EnvelopeLegitLevelsAndTimesRenderUnclippedInBox) {
    // Legit preset values (levels 0..1 → y ≤ 97, times → x ≤ 94): the
    // clamps must be transparent — pixels land exactly where the unpatched
    // draw puts them (deterministic goldens below), all inside the box.
    display_.oscilloBgSetEnvelope(0.25f, 0.75f, 1.0f, 1.0f, 1.0f, 0.8f, 0.6f,
            0.0f, CURVE_TYPE_LIN, CURVE_TYPE_EXP, CURVE_TYPE_LIN, CURVE_TYPE_LOG);

    display_.oscilloBgActionEnvelope();
    display_.pumpAction();
    display_.pumpAction();

    ExpectCanariesIntact();
    uint16_t yellow = tftPalette565[COLOR_YELLOW];
    uint16_t darkGrey = tftPalette565[COLOR_DARK_GRAY];
    // sum(times) = 3.0 → div = 5 → scale = 31.6:
    //   vertical line #1 at x = (int)(0.25 * 31.6) = 7 → column 8,
    //   height (int)(1.0 * 97) = 97 → rows 98 down to 1 (darkGrey).
    EXPECT_EQ(layout_.box[98 * 160 + 8], darkGrey);
    // Mid-height marker pixel (row 50 → y = 48) — the curve endpoints do not
    // reach column 8 there, so it keeps the vertical line's darkGrey (the
    // line's BOTTOM pixel at row 1/col 8 is legitimately overwritten by the
    // curve's yellow endpoint — steps draw after the markers).
    EXPECT_EQ(layout_.box[50 * 160 + 8], darkGrey);
    // First env step is the LIN line (0,0) → (7,97): its first pixel is
    // row 98, column 1 (indexLow = 98*160 + 1), drawn in the curve color.
    EXPECT_EQ(layout_.box[98 * 160 + 1], yellow);
}

// --- T1b: additionalActions off-by-one ---------------------------------------

TEST_F(FirmwareTftDisplayTest, WaveformIndexEqualToSlotCountIsNoDraw) {
    // All 14 slots (0..13) registered with safe buffers...
    float safe[64];
    for (int i = 0; i < 64; i++) safe[i] = 0.5f;
    for (int i = 0; i < TFT_NUMBER_OF_WAVEFORM_EXT; i++) {
        display_.initWaveFormExt(i, safe, 64);
    }
    // ...then pin the memory that waveForm[14] would reinterpret: the array
    // is immediately followed by oscilParams1[], so a set envelope makes the
    // OOB read deterministic (pointer bits 0x3F8000003F000000 → non-canonical
    // address; the pre-fix code SEGVs on the first sample deref).
    display_.oscilloBgSetEnvelope(0.5f, 0.5f, 0.5f, 0.5f, 1.0f, 1.0f, 1.0f,
            1.0f, CURVE_TYPE_LIN, CURVE_TYPE_LIN, CURVE_TYPE_LIN, CURVE_TYPE_LIN);
    // Sentinel box (from SetUp): step 0's "clear" only programs the DMA2D
    // dummy registers on host, and a correctly-handled param1 == 14 draws
    // nothing — so every element must still be the sentinel after both steps.

    // Phase 1: malformed action injected directly (a rogue queue entry).
    display_.setAction(TFT_DRAW_OSCILLO_BACKGROUND_WAVEFORM, 14, 0);
    display_.pumpAction();
    display_.pumpAction();
    for (int i = 0; i < kBoxElems; i++) {
        ASSERT_EQ(layout_.box[i], 0x1234) << "box element " << i << " written";
    }
    EXPECT_EQ(display_.queuedOperatorDraws(), 0);  // unchanged: no draw ran
    EXPECT_EQ(display_.pendingActions(), 0);       // action completed
    ExpectCanariesIntact();

    // Phase 2: the same malformed param1 through the public enqueue API —
    // the enqueue must count it (1), and completing it must never decrement
    // the counter below zero (the pre-fix branch would have decremented to
    // -1 had the garbage read happened to be benign). NOTE the existing
    // "just in case" semantics in additionalActions(): it resets all three
    // queue counters to 0 whenever it runs with an empty action queue — and
    // the dequeue happens before additionalActions() sees the queue — so
    // once the action has been pumped the counter is 0 again by design.
    display_.oscilloBgActionOperatorShape(14);
    EXPECT_EQ(display_.queuedOperatorDraws(), 1);
    display_.pumpAction();
    display_.pumpAction();
    for (int i = 0; i < kBoxElems; i++) {
        ASSERT_EQ(layout_.box[i], 0x1234) << "box element " << i << " written";
    }
    // B1 (review finding): dropped the post-pump counter assertion —
    // additionalActions()'s queue-empty "just in case" reset zeroes the
    // counter either way, so it passed pre- and post-fix (could not fail);
    // the enqueue-side count above and the sentinel carry the signal.
    EXPECT_EQ(display_.pendingActions(), 0);
    ExpectCanariesIntact();
}

}  // namespace
