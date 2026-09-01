// Host-side coverage for firmware/Src/hardware/TftAlgo.cpp — the algorithm
// schema drawing + operator highlight on the TFT foreground mask buffer.
//
// Regression target (finding 7.6, hardware-test-plan §Phase 7):
//   highlightOperator() used to draw its 20x21 box with int32/int16
//   cast-stores into the uint8_t algo mask buffer at offset (x) + y*80.
//   The middle operator columns (opPosition 2/5/8/11 — the grid is 3 columns
//   wide, GETX1(o) = ((o-1)%3)*26 + 6) yield x1 = GETX1 - 2 == 30, so the
//   int32 stores sat at offsets ≡ 2 (mod 4). On the ARM build gcc 15 fuses
//   them into strd, which requires word alignment → UsageFault UNALIGNED →
//   HardFault → latched freeze. Proven on device (probe autopsy
//   2026-09-01): default algo 1 places operator 1 at position 11 (middle
//   column), so merely opening the operator page with op 1 selected, or
//   turning its MIX encoder on the OSC page, crashed a86c96b. The fix
//   redraws the box with plain byte stores (SETFGPIXEL/DELFGPIXEL
//   semantics), alignment-defined for every position.
//
// The alignment half of the regression guard lives in the sanitizer build:
//   tests/CMakeLists.txt sets UBSAN_OPTIONS=halt_on_error=1 for these tests,
//   so the -fsanitize=alignment check (already in the test-asan flag set)
//   turns any reintroduced misaligned cast-store into a hard failure. On the
//   plain build these tests pin the BYTE COVERAGE of the box (rows span
//   columns x1..x1+19; vertical edges are 2 px wide: x1, x1+1 and x1+19,
//   x1+20 — matching the old int16 edge stores) for the crash geometry
//   (middle column), every other column, draw AND erase, plus the
//   position-0 early return.
//
// Geometry replicated independently here from the frozen upstream tables
// (TftAlgo.cpp allAlgos[]) — NOTE drawAlgo(n) indexes allAlgos[n] directly
// (0-based; the UI's displayed "Algo N" is table N-1): table 0 ("algo1") →
// op1@11 (middle column, THE crash geometry), op2@4 (middle column), op3@3
// (right column); table 1 ("algo2") → op1@10 (left), op2@12 (right/deepest),
// op3@5 (middle, second grid row — exercises GETY1's /3 branch past the
// first row). GETX1/GETY1 formulas are frozen upstream geometry (3-col x
// 4-row grid, 26/25 px cells, 80-byte stride), not derived from the code
// under test.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "TftAlgo.h"  // firmware-under-test (PFM3_HOST seam: stdint only)

// --- UBSan enforcement hook (7.6 regression guard) --------------------------
// -fsanitize=undefined (the test-asan flag set) REPORTS misaligned stores
// but exits 0 by default, and per-test ENVIRONMENT properties can't reach
// gtest_discover_tests results at configure time — so a reintroduced
// int32/int16 cast-store in TftAlgo would print a runtime error yet still
// PASS ctest. Override the UBSan monitor hook instead: when a report fires
// from TftAlgo.cpp (the TU under test), abort the process so the running
// test fails loudly. Reports from any other TU keep the suite's documented
// reporting-only behavior (only fatal for this file).
#if defined(__clang__) && defined(__has_feature)
#if __has_feature(undefined_behavior_sanitizer)
#include <cstdio>
#include <cstdlib>
extern "C" {
// compiler-rt ubsan_monitor interface (weak defaults; overriding
// __ubsan_on_report makes the runtime call us after every report).
void __ubsan_on_report(void);
void __ubsan_get_current_report_data(const char **OutIssueKind,
                                     const char **OutMessage,
                                     const char **OutFilename,
                                     unsigned *OutLine, unsigned *OutCol,
                                     char **OutMemoryAddr);
}
static bool StringContains(const char *haystack, const char *needle) {
    if (haystack == nullptr) return false;
    const size_t n = std::strlen(needle);
    for (const char *p = haystack; *p != '\0'; ++p) {
        if (std::strncmp(p, needle, n) == 0) return true;
    }
    return false;
}
extern "C" void __ubsan_on_report(void) {
    const char *issueKind = nullptr;
    const char *message = nullptr;
    const char *filename = nullptr;
    unsigned line = 0, col = 0;
    char *addr = nullptr;
    __ubsan_get_current_report_data(&issueKind, &message, &filename, &line,
                                    &col, &addr);
    if (StringContains(filename, "TftAlgo.cpp")) {
        std::fprintf(
            stderr,
            "\n*** TftAlgo.cpp:%u:%u triggered a UBSan report (%s): %s\n"
            "*** Aborting: misaligned cast-stores in TftAlgo are finding 7.6\n"
            "*** regressions and must fail the test, not just report.\n",
            line, col, issueKind != nullptr ? issueKind : "?",
            message != nullptr ? message : "?");
        std::_Exit(70);  // fail the running test; no unwinding under a report
    }
}
#endif  // __has_feature(undefined_behavior_sanitizer)
#endif  // defined(__clang__)

namespace {

// Frozen upstream geometry (TftAlgo.cpp GETX1/GETY1).
int ExpectedX1(int opPosition) { return ((opPosition - 1) % 3) * 26 + 6 - 2; }
int ExpectedY1(int opPosition) { return ((opPosition - 1) / 3) * 25 + 2 - 2; }

// The fg algo buffer is 80 x 100 bytes (TftDisplay.cpp: memset(fgAlgo, 0,
// 80 * 100)); the widest box write is row 97, column 77 — in range.
constexpr int kFgStride = 80;
constexpr int kFgRows = 100;

class TftAlgoHighlightTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::memset(fg_, 0, sizeof(fg_));
        std::memset(main_, 0, sizeof(main_));
        algo_.setFGBufferAdress(fg_);
        algo_.setBufferAdress(main_);
    }

    // Byte the old int32/int16 stores + the new byte stores must all set /
    // clear: the box outline. Rows y1, y1+1, y1+19, y1+20 over columns
    // x1..x1+19; vertical edges at columns x1, x1+1, x1+19, x1+20 over rows
    // y1..y1+19 (the old int16 edge stores covered 2 px each).
    static void ExpectBox(const uint8_t* fg, int x1, int y1, uint8_t expected,
                          int opNumForMessage) {
        for (int x = x1; x < x1 + 20; x++) {
            EXPECT_EQ(fg[x + (y1)*kFgStride], expected)
                << "top row x=" << x << " opNum=" << opNumForMessage;
            EXPECT_EQ(fg[x + (y1 + 1) * kFgStride], expected)
                << "second row x=" << x << " opNum=" << opNumForMessage;
            EXPECT_EQ(fg[x + (y1 + 19) * kFgStride], expected)
                << "row y1+19 x=" << x << " opNum=" << opNumForMessage;
            EXPECT_EQ(fg[x + (y1 + 20) * kFgStride], expected)
                << "bottom row x=" << x << " opNum=" << opNumForMessage;
        }
        for (int y = y1; y < y1 + 20; y++) {
            EXPECT_EQ(fg[(x1) + y * kFgStride], expected)
                << "left edge y=" << y << " opNum=" << opNumForMessage;
            EXPECT_EQ(fg[(x1 + 1) + y * kFgStride], expected)
                << "left edge+1 y=" << y << " opNum=" << opNumForMessage;
            EXPECT_EQ(fg[(x1 + 19) + y * kFgStride], expected)
                << "right edge y=" << y << " opNum=" << opNumForMessage;
            EXPECT_EQ(fg[(x1 + 20) + y * kFgStride], expected)
                << "right edge+1 y=" << y << " opNum=" << opNumForMessage;
        }
    }

    TftAlgo algo_;
    uint8_t fg_[kFgStride * kFgRows];
    uint16_t main_[kFgStride * kFgRows];
};

// The 7.6 crash geometry: table algo1 (drawAlgo(0)), operator 1 at
// position 11 → x1 == 30 (≡ 2 mod 4 — the offset that HardFaulted via strd
// on device).
TEST_F(TftAlgoHighlightTest, MiddleColumnHighlightCrashGeometryWritesBox) {
    algo_.drawAlgo(0);  // table algo1: op1@11, op2@4, op3@3
    algo_.highlightOperator(true, 0);

    EXPECT_EQ(ExpectedX1(11), 30);  // the faulting x1, pinned
    ExpectBox(fg_, ExpectedX1(11), ExpectedY1(11), 0xff, 0);
}

// Erase must clear exactly the same bytes (draw=false path, colorByte 0x00).
TEST_F(TftAlgoHighlightTest, MiddleColumnHighlightEraseClearsBox) {
    algo_.drawAlgo(0);
    algo_.highlightOperator(true, 0);
    algo_.highlightOperator(false, 0);

    ExpectBox(fg_, ExpectedX1(11), ExpectedY1(11), 0x00, 0);
}

// All three placed operators of table algo1 — ops 2 (pos 4, middle column)
// and 3 (pos 3, right column) cover the remaining columns of the crash class
// and the aligned case.
TEST_F(TftAlgoHighlightTest, AllAlgo1OperatorsDrawTheirBoxes) {
    algo_.drawAlgo(0);

    algo_.highlightOperator(true, 1);  // op2 @ position 4 (middle column)
    algo_.highlightOperator(true, 2);  // op3 @ position 3 (right column)
    ExpectBox(fg_, ExpectedX1(4), ExpectedY1(4), 0xff, 1);
    ExpectBox(fg_, ExpectedX1(3), ExpectedY1(3), 0xff, 2);
}

// A second algorithm (table algo2: op1@10 left, op2@12 right, op3@5 middle
// — second row of the grid).
TEST_F(TftAlgoHighlightTest, Algo2BoxesIncludingSecondRowMiddleColumn) {
    algo_.drawAlgo(1);

    algo_.highlightOperator(true, 0);  // op1 @ position 10 (left column)
    algo_.highlightOperator(true, 1);  // op2 @ position 12 (right column)
    algo_.highlightOperator(true, 2);  // op3 @ position 5 (middle, row 2)
    ExpectBox(fg_, ExpectedX1(10), ExpectedY1(10), 0xff, 0);
    ExpectBox(fg_, ExpectedX1(12), ExpectedY1(12), 0xff, 1);
    ExpectBox(fg_, ExpectedX1(5), ExpectedY1(5), 0xff, 2);
}

// Operators not placed by the algorithm (position 0) must early-return and
// touch nothing — algo 1 places only ops 1, 2, 3.
TEST_F(TftAlgoHighlightTest, UnplacedOperatorHighlightsNothing) {
    algo_.drawAlgo(0);
    algo_.highlightOperator(true, 3);  // op4: no position in algo 1
    algo_.highlightOperator(true, 4);  // op5
    algo_.highlightOperator(true, 5);  // op6

    for (int i = 0; i < kFgStride * kFgRows; i++) {
        EXPECT_EQ(fg_[i], 0x00) << "byte " << i << " changed by unplaced op";
    }
}

// Highlighting must stay inside the 80x100 mask buffer for the deepest grid
// position (12 → row 97, column 77): a regression that widens the box or
// shifts geometry would run off the end (ASAN would also catch it).
TEST_F(TftAlgoHighlightTest, HighlightStaysInsideMaskBuffer) {
    algo_.drawAlgo(1);  // table algo2: op2 @ position 12 — deepest cell
    algo_.highlightOperator(true, 1);  // op2 @ position 12 — deepest cell

    // Everything outside the expected box footprint must still be zero.
    const int x1 = ExpectedX1(12), y1 = ExpectedY1(12);
    int touched = 0;
    for (int y = 0; y < kFgRows; y++) {
        for (int x = 0; x < kFgStride; x++) {
            const bool inBoxRows =
                (y == y1 || y == y1 + 1 || y == y1 + 19 || y == y1 + 20) && x >= x1 && x < x1 + 20;
            const bool inBoxCols = (x == x1 || x == x1 + 1 || x == x1 + 19 || x == x1 + 20) && y >= y1 && y < y1 + 20;
            if (fg_[x + y * kFgStride] != 0x00) {
                touched++;
                EXPECT_TRUE(inBoxRows || inBoxCols)
                    << "byte set outside the box at x=" << x << " y=" << y;
            }
        }
    }
    EXPECT_GT(touched, 0);  // the box itself WAS drawn
}

}  // namespace
