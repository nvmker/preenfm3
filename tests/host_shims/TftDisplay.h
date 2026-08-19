/*
 * Host shadow header for lib/Inc/TftDisplay.h (Phase 4 seam).
 *
 * WHY THIS EXISTS: the real lib/Inc/TftDisplay.h includes stm32h7xx_hal.h and
 * TftAlgo.h (-> DMA2D register macros, RTOS action queues) — unhostable. The
 * filesystem TUs (MixerBank.cpp, SequenceBank.cpp, PPMImage.cpp) include
 * "TftDisplay.h" only for progress-display side effects; the class surface
 * they touch is the tiny no-op set mirrored below. This shadow is placed FIRST
 * on the test include path (tests/host_shims), so the firmware build never
 * sees it.
 *
 * The shadow also declares HAL_Delay() (called by PPMImage::saveImage after
 * the capture); the host stub in tests/stubs/tft_display_stub.cpp defines it
 * as a no-op — a host delay of a nonexistent TFT would only slow tests.
 */
#ifndef PFM3_HOST_SHIM_TFTDISPLAY_H
#define PFM3_HOST_SHIM_TFTDISPLAY_H

#include <stdint.h>

/* Same enumerators/values as lib/Inc/TftDisplay.h's TFT_COLOR (the filesystem
 * TUs pass these by value to the no-op surface; value parity keeps any future
 * assertion faithful). */
enum TFT_COLOR {
    COLOR_BLACK = 0,
    COLOR_WHITE,
    COLOR_BLUE,
    COLOR_DARK_BLUE,
    COLOR_CYAN,
    COLOR_YELLOW,
    COLOR_DARK_YELLOW,
    COLOR_ORANGE,
    COLOR_RED,
    COLOR_DARK_RED,
    COLOR_GREEN,
    COLOR_DARK_GREEN,
    COLOR_LIGHT_GREEN,
    COLOR_GRAY,
    COLOR_DARK_GRAY,
    COLOR_LIGHT_GRAY,
    COLOR_MEDIUM_GRAY,
    NUMBER_OF_TFT_COLORS
};

/* Minimal no-op surface + observation counters (the observation state is for
 * tests only; the firmware TUs just call through). */
class TftDisplay {
public:
    TftDisplay() : colorCalls_(0), bgCalls_(0), fillCalls_(0), printCalls_(0),
                   cursorCalls_(0), lastColor_(NUMBER_OF_TFT_COLORS),
                   lastBgColor_(NUMBER_OF_TFT_COLORS),
                   cursorX_(0), cursorY_(0) {}

    void setCharColor(TFT_COLOR c) { lastColor_ = c; colorCalls_++; }
    void setCharBackgroundColor(TFT_COLOR c) { lastBgColor_ = c; bgCalls_++; }
    void setCursor(uint8_t x, uint16_t y) { cursorX_ = x; cursorY_ = y; cursorCalls_++; }
    void print(const char* s) { (void)s; printCalls_++; }
    void print(int n) { (void)n; printCalls_++; }
    void fillArea(uint8_t x, uint16_t y, uint8_t w, uint16_t h, uint8_t color) {
        (void)x; (void)y; (void)w; (void)h; (void)color; fillCalls_++;
    }

    /* --- test observation --- */
    int colorCalls() const { return colorCalls_; }
    int bgCalls() const { return bgCalls_; }
    int fillCalls() const { return fillCalls_; }
    int printCalls() const { return printCalls_; }
    int cursorCalls() const { return cursorCalls_; }
    TFT_COLOR lastColor() const { return lastColor_; }
    TFT_COLOR lastBgColor() const { return lastBgColor_; }

private:
    int colorCalls_, bgCalls_, fillCalls_, printCalls_, cursorCalls_;
    TFT_COLOR lastColor_, lastBgColor_;
    uint8_t cursorX_;
    uint16_t cursorY_;
};

/* Defined in tests/stubs/tft_display_stub.cpp. */
extern TftDisplay tft;

/* The TFT shadow framebuffer PPMImage dumps (mirrors the firmware extern in
 * PPMImage.cpp; the stub owns the definition). */
extern uint16_t tftMemory[240 * 320];

/* Host no-op delay (stub-defined); see header comment. */
void HAL_Delay(uint32_t ms);

#endif /* PFM3_HOST_SHIM_TFTDISPLAY_H */
