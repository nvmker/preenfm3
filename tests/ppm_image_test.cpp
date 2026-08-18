// Host-side coverage for firmware/Src/filesystem/PPMImage.cpp —
// screen capture to "0:/PPM/img_####.ppm".
//
// CHARACTERIZATION suite (spec-test-coverage-phase4). FatFs via the shim;
// tft is the Phase-4 no-op stub; tftMemory is the stub framebuffer fixture.
//
// NOTE: PPMIMAGE_ENABLE is #define'd (to 0!) and guarded with #ifdef — the
// classic trap — so this code IS active in every build. Layout golden:
//   "P6\n240 320\n255\n" (15 bytes) + 6400*3 bytes of 5->8-bit RGB.
// QUIRKS pinned (deferred-work.md):
//   * the RGB body is a 6400-pixel RING buffer flushed only when iIndex
//     wraps to 6399, so the dumped image is the LAST 6400 pixels of the
//     76800-pixel framebuffer (bottom ~27 rows), top rows are lost.
//   * 5->8 bit expansion shifts left without replicating low bits: 0x1F
//     becomes 0xF8, not 0xFF.
#include "gtest/gtest.h"

#include "fatfs.h"
#define private public
#include "PPMImage.h"
#undef private
#include "TftDisplay.h"

#include <cstring>
#include <new>
#include <string>
#include <vector>

extern TftDisplay tft;
extern uint16_t tftMemory[240 * 320];

class PpmImageTest : public ::testing::Test {
protected:
    void SetUp() override {
        fatfsShimReset();
        memset(tftMemory, 0, sizeof(tftMemory));
    }

    // QUIRK ADJACENT (deferred-work.md): PPMImage::isInitialized is NEVER
    // initialized by the constructor — on the device the global sits in BSS
    // (zero), so saveImage()'s lazy init runs. A stack instance has garbage
    // there; we reproduce the firmware BSS state with memset + placement new
    // (tests/fx_bus_test.cpp:66 precedent).
    PPMImage* MakeImage() {
        memset(imageStorage_, 0, sizeof(imageStorage_));
        return new (imageStorage_) PPMImage();
    }
    alignas(PPMImage) unsigned char imageStorage_[sizeof(PPMImage)];
    // exact replica of saveImage's pixel math for expectation building
    static uint8_t ExpandR(uint16_t p) { return (uint8_t)(((pixelSwap(p) & 0xF800) >> 11) << 3); }
    static uint8_t ExpandG(uint16_t p) { return (uint8_t)(((pixelSwap(p) & 0x07E0) >> 5) << 2); }
    static uint8_t ExpandB(uint16_t p) { return (uint8_t)((pixelSwap(p) & 0x001F) << 3); }
    static uint16_t pixelSwap(uint16_t p) { return (uint16_t)((p >> 8) + (p << 8)); }
};

TEST_F(PpmImageTest, FirstSaveWritesHeaderAndRgbBody) {
    PPMImage* imgp = MakeImage();
    PPMImage& img = *imgp;
    // distinctive pattern: pixel value = (x*3+y*7) etc.
    for (int j = 0; j < 320; j++) {
        for (int i = 0; i < 240; i++) {
            tftMemory[j * 240 + i] = (uint16_t)((i * 7 + j * 13) & 0xFFFF);
        }
    }
    img.saveImage();

    std::vector<uint8_t> ppm;
    ASSERT_TRUE(fatfsShimExtract("0:/PPM/img_0001.ppm", ppm));
    // QUIRK GOLDEN: the flush condition `iIndex == 6399` fires every 6400
    // pixels, so the file is the 15-byte header + TWELVE 6400*3-byte ring
    // dumps (76800 pixels / 6400 = 12), not one. Declared image is 240x320
    // but the byte payload is 12x too large.
    ASSERT_EQ(ppm.size(), 15u + 12u * 6400u * 3u);
    EXPECT_EQ(memcmp(ppm.data(), "P6\n240 320\n255\n", 15), 0);

    // each block b holds pixels [6400b .. 6400b+6399] at ring slot L % 6400
    for (int b = 0; b < 12; b++) {
        for (int n = 0; n < 6400; n++) {
            int L = b * 6400 + n;
            uint16_t p = tftMemory[L];
            size_t off = 15u + (b * 6400u + (L % 6400)) * 3u;
            EXPECT_EQ(ppm[off + 0], ExpandR(p)) << "L=" << L;
            EXPECT_EQ(ppm[off + 1], ExpandG(p)) << "L=" << L;
            EXPECT_EQ(ppm[off + 2], ExpandB(p)) << "L=" << L;
        }
    }
}

TEST_F(PpmImageTest, BitExpansionShiftsWithoutLowBitReplication) {
    PPMImage& img = *MakeImage();
    memset(tftMemory, 0, sizeof(tftMemory));
    // values chosen so that AFTER the byte swap inside saveImage each lands
    // maxed in exactly one channel: 0x1F00 -> B=0xF8, 0x00F8 -> R=0xF8,
    // 0xE007 -> G=0xFC. Shift-left only: 5-bit 31 -> 0xF8 (not 0xFF).
    tftMemory[76799] = 0x1F00;  // blue 31  -> 0xF8
    tftMemory[76798] = 0x00F8;  // red 31   -> 0xF8
    tftMemory[76797] = 0xE007;  // green 63 -> 0xFC
    img.saveImage();
    std::vector<uint8_t> ppm;
    ASSERT_TRUE(fatfsShimExtract("0:/PPM/img_0001.ppm", ppm));
    // last block (11), last pixels: linear 76799 == ring 6399 of block 11
    size_t base = 15u + 11u * 6400u * 3u;
    EXPECT_EQ(ppm[base + 6399 * 3 + 2], 0xF8);
    EXPECT_EQ(ppm[base + 6398 * 3 + 0], 0xF8);
    EXPECT_EQ(ppm[base + 6397 * 3 + 1], 0xFC);
}

TEST_F(PpmImageTest, InitSkipsExistingImagesToNextFreeName) {
    // collision loop: img_0001 and img_0002 exist -> first save is img_0003
    fatfsShimMkdir("0:/PPM");
    fatfsShimInjectString("0:/PPM/img_0001.ppm", "x");
    fatfsShimInjectString("0:/PPM/img_0002.ppm", "x");
    PPMImage& img = *MakeImage();
    img.saveImage();
    EXPECT_TRUE(fatfsShimFileExists("0:/PPM/img_0003.ppm"));
    // second save on the SAME instance increments
    img.saveImage();
    EXPECT_TRUE(fatfsShimFileExists("0:/PPM/img_0004.ppm"));
    EXPECT_EQ(fatfsShimFileCount(), 4u);
}
