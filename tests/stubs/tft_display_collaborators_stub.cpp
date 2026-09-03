/*
 * Host stubs for the out-of-line TftDisplay (REAL class) symbols that
 * firmware/Src/hardware/FirmwareTftDisplay.o references — B1 tier
 * (spec-b1-tft-display-corruption-hardening). Same pattern as
 * tests/stubs/sequencer_collaborators_stub.cpp: link-satisfaction test
 * doubles, not HAL fakes.
 *
 * This TU includes the REAL lib/Inc/TftDisplay.h by relative path FIRST, so
 * the shared dual-flavor include guard (see tests/host_shims/TftDisplay.h)
 * keeps the later no-op shadow out and the real class wins here. That is
 * required for the vtable: defining TftDisplay::~TftDisplay() (the class's
 * key function) in this TU is what emits the TftDisplay vtable + typeinfo
 * the FirmwareTftDisplay construction needs. The vtable's other entries are
 * inline in the header (additionalActions/clearActions) and come along for
 * the ride.
 *
 * TftDisplay.cpp itself is deliberately NOT pulled into the host build: its
 * body is the SPI/DMA2D action machine (ili9341.c, HAL_SPI_*, real
 * framebuffer RAM_D1 sections) — unhostable, and irrelevant to the B1 CPU
 * draw paths under test. Instead this stub supplies exactly:
 *
 *   TftDisplay::TftDisplay()   — minimal: currentAction.actionType = 0 so
 *                                the fixture's dequeue logic (mirroring
 *                                tic()) starts from a quiescent action.
 *   TftDisplay::~TftDisplay()  — empty; emits the vtable here (see above).
 *   uint16_t tftPalette565[]   — defined in TftDisplay.cpp on firmware;
 *                                filled with distinctive 0x1000+i values so
 *                                draw tests can identify palette colors
 *                                (tftPalette565[COLOR_YELLOW] etc.).
 *   DMA2D_HandleTypeDef hdma2d — defined in firmware main.c; Instance points
 *                                at a static zero-initialized dummy register
 *                                block (CR == 0 → PFM_IS_DMA2D_READY() true;
 *                                tests clear CR between action pumps).
 *
 * NOTE on the one shared mangled name: the no-op TftDisplay shadow class in
 * tests/host_shims/TftDisplay.h also declares an (inline) constructor, which
 * tests/stubs/tft_display_stub.cpp emits weakly for its `TftDisplay tft`
 * global. This TU's strong TftDisplay::TftDisplay() definition wins the
 * link for that name; the shadow `tft` object then keeps its zero-init
 * (static storage) instead of running the shadow ctor's initializers. The
 * only observable difference is lastColor_/lastBgColor_ reading
 * COLOR_BLACK (0) instead of NUMBER_OF_TFT_COLORS before the first
 * setCharColor call — no test asserts that, and all call counters are
 * identical (they start at 0 either way).
 */
#include "../../lib/Inc/TftDisplay.h"

/* Dummy DMA2D register block: zero-initialized → CR == 0 → "ready". */
static DMA2D_TypeDef tftDma2dDummyRegs;

DMA2D_HandleTypeDef hdma2d = { &tftDma2dDummyRegs };

/* Distinctive palette (the firmware fills this from reducedColor() RGBs in
 * TftDisplay.cpp — not host-reachable; the exact values only need to be
 * distinguishable per COLOR_* index for the draw tests). */
uint16_t tftPalette565[NUMBER_OF_TFT_COLORS];

namespace {
struct TftPaletteFill {
    TftPaletteFill() {
        for (int i = 0; i < NUMBER_OF_TFT_COLORS; i++) {
            tftPalette565[i] = (uint16_t)(0x1000 + i);
        }
    }
};
static TftPaletteFill tftPaletteFill;
}  // namespace

TftDisplay::TftDisplay() {
    currentAction.actionType = 0;
}

TftDisplay::~TftDisplay() {
    // empty (vtable key function — see file comment)
}
