// Host-side coverage for firmware/Src/synth/MixerState.cpp — the mixer
// preset serialization (bank save/load) + versioned migration path.
//
// Regression target (per test-coverage-plan.md Phase 1, row 4):
//   Mixer state corruption / dead channels on bank load. getFullState
//   serializes the whole 6-instrument mixer (name, channels, tuning, per-
//   timbre routing/scala/volume/pan/compressor/FX-send, user CCs, reverb +
//   13 master-FX params) as a flat byte buffer; restoreFullState dispatches
//   on the leading version byte to one of SIX layout readers (v1..v6), each
//   older version missing the fields added later (v2 pan, v3 compressor,
//   v4 userCC, v5 MPE, v6 FX-send + reverb/global FX). A layout drift or a
//   broken migration silently garbles user mixer banks — the byte-exact
//   layout asserts here catch it.
//
// Serialization invariant fixed in this PR:
//   The 0.01f-scaled byte params (FX send, reverb level, the 13 master-FX
//   params) restore as `0.01f * byte` and save with nearest-byte rounding.
//   Every valid percent byte (0..100) must therefore survive repeated
//   save/load cycles exactly; the exhaustive round-trip test below drives all
//   20 percent-valued slots through every value.
//
// Fixture notes (see tests/SEAM.md):
//   * MixerState embeds a REAL FxBus whose delay buffers are STATIC class
//     members (shared process-wide): SetUp calls fxBus_.init() per test per
//     the shared-state hygiene rule. The serialization paths only touch
//     masterfxConfig (no audio buffers), but init() keeps the fixture
//     faithful to the firmware boot order (SynthState::init -> fxBus init).
//   * Standalone class: ctor/dtor are out-of-line in the linked MixerState.cpp
//     TU, so a plain stack member links with no stubs.
//   * shiftNote/pan use only non-negative byte values: on the ARM firmware
//     `char` is unsigned, on the host it is signed — negative encodings would
//     diverge host-vs-firmware, so the layout tests stay in the intersection.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "MixerState.h"  // firmware-under-test (host-compilable, no seam needed)

namespace {

// Distinctive baseline bytes for the v6 send/reverb/master-FX slots. The
// exhaustive round-trip test overwrites all percent-valued slots with every
// valid value in [0, 100].
const uint8_t kParamBytes[13] = {31, 32, 33, 34, 35, 36, 37, 38, 11, 12,
                                 13, 14, 16};

// Tiny layout writer mirroring the firmware's index++ serialization idiom.
struct MixBuf {
    char b[256];
    uint32_t n = 0;
    void u8(uint8_t v) { b[n++] = (char)v; }
    void f32(float v) {
        uint8_t* p = (uint8_t*)&v;
        for (int i = 0; i < 4; i++) u8(p[i]);
    }
    void name12(const char* s) {
        for (int i = 0; i < 12; i++) u8((uint8_t)s[i]);
    }
};

// Per-timbre distinctive core fields (v1 block + the given extras).
void PutTimbre(MixBuf& mb, int t, bool pan, bool comp, bool send) {
    mb.u8(1 + t);              // out
    mb.u8(2 + t);              // midiChannel
    mb.u8(10 * t);             // firstNote
    mb.u8(100 + t);            // lastNote
    mb.u8(t);                  // shiftNote (non-negative: host/firmware char)
    mb.u8(t % 4);              // numberOfVoices
    mb.u8(t % 2);              // scalaEnable
    mb.u8((t + 1) % 2);        // scalaMapping
    mb.u8(1);                  // scaleScaleNumber high
    mb.u8(t);                  // scaleScaleNumber low
    for (int i = 0; i < 12; i++) mb.u8('a' + t);  // scalaScaleFileName
    mb.f32(0.5f + 0.1f * t);   // volume
    if (pan) mb.u8(10 + t);
    if (comp) mb.u8(t % 4);
    if (send) mb.u8(kParamBytes[t % 13]);
}

// Build a minimal valid buffer for any version 1..6 with distinctive values.
MixBuf BuildVersionBuffer(int version) {
    MixBuf mb;
    mb.u8(version);
    mb.name12("UnitTestMix1");
    if (version >= 5) mb.u8(1);  // MPE_inst1_
    mb.u8(3);                    // currentChannel_
    mb.u8(2);                    // globalChannel_
    mb.u8(1);                    // midiThru_
    mb.f32(442.5f);              // tuning_
    for (int t = 0; t < NUMBER_OF_TIMBRES; t++) {
        PutTimbre(mb, t, /*pan=*/version >= 2, /*comp=*/version >= 3,
                  /*send=*/version >= 6);
    }
    if (version >= 3) mb.u8(2);  // levelMeterWhere_
    if (version >= 4) {          // userCC_[4]
        mb.u8(40); mb.u8(41); mb.u8(42); mb.u8(43);
    }
    if (version >= 6) {          // reverb + 13 master-FX params
        mb.u8(9);                // reverbPreset_
        mb.u8(1);                // reverbOutput_
        mb.u8(88);               // reverbLevel_ byte (round-trip safe)
        for (int i = 0; i < 13; i++) mb.u8(kParamBytes[i]);
    }
    return mb;
}

// v6 percent-byte positions: 6 per-timbre sends, global reverb level, and 13
// master-FX params. Fill all 20 so each serializer call site is exercised for
// the same boundary value.
void FillV6PercentBytes(MixBuf& mb, uint8_t value) {
    const int kTimbreStart = 21;
    const int kTimbreStride = 29;
    const int kSendOffset = 28;
    for (int t = 0; t < NUMBER_OF_TIMBRES; t++) {
        mb.b[kTimbreStart + t * kTimbreStride + kSendOffset] = (char)value;
    }
    mb.b[202] = (char)value;  // reverbLevel_
    for (int i = 203; i <= 215; i++) mb.b[i] = (char)value;
}

class MixerStateTest : public ::testing::Test {
protected:
    MixerState ms_;

    void SetUp() override {
        // Embedded FxBus: static delay buffers are class-wide shared state —
        // re-init per test (hygiene rule; also runs setDefaultValue).
        ms_.fxBus_.init();
    }
};

// getFullDefaultState: "MixNN" name (zero-padded, clamped at 99), tuning 440,
// the documented per-timbre defaults, user CC 34..37, reverb preset 7 level
// 100, and the 13 master-FX defaults — 210 bytes total.
TEST_F(MixerStateTest, GetFullDefaultStateWritesNameAndDefaults) {
    char buf[256];
    uint32_t size = 0;
    ms_.getFullDefaultState(buf, &size, 42);
    ASSERT_EQ(size, 216u);
    EXPECT_EQ(buf[0], MIXER_BANK_CURRENT_VERSION);
    EXPECT_EQ(std::string(buf + 1, 12).substr(0, 6), std::string("Mix 42"));

    // Zero padding: mix 7 -> "Mix07"; clamp at 99.
    ms_.getFullDefaultState(buf, &size, 7);
    EXPECT_EQ(std::string(buf + 1, 12).substr(0, 6), std::string("Mix 07"));
    ms_.getFullDefaultState(buf, &size, 200);
    EXPECT_EQ(std::string(buf + 1, 12).substr(0, 6), std::string("Mix 99"));

    // tuning 440.0f little-endian at offset 17.
    float tuning = 0.0f;
    std::memcpy(&tuning, buf + 17, 4);
    EXPECT_FLOAT_EQ(tuning, 440.0f);

    // Per-timbre defaults at offset 21, stride 29 (29-byte v6 timbre block).
    const int kTimbre0 = 21;
    EXPECT_EQ((uint8_t)buf[kTimbre0 + 0], 1);   // out
    EXPECT_EQ((uint8_t)buf[kTimbre0 + 1], 1);   // midiChannel
    EXPECT_EQ((uint8_t)buf[kTimbre0 + 5], 3);   // numberOfVoices
    EXPECT_EQ((uint8_t)buf[kTimbre0 + 27], 2);  // compressorType (t==0 only)
    const int kTimbre5 = kTimbre0 + 5 * 29;
    EXPECT_EQ((uint8_t)buf[kTimbre5 + 0], 8);   // out
    EXPECT_EQ((uint8_t)buf[kTimbre5 + 1], 6);   // midiChannel
    EXPECT_EQ((uint8_t)buf[kTimbre5 + 5], 1);   // numberOfVoices
    EXPECT_EQ((uint8_t)buf[kTimbre5 + 27], 0);  // compressorType

    // Tail defaults (offsets with the 29-byte stride): levelMeter 1,
    // userCC 34..37, reverb 7/0/100.
    EXPECT_EQ((uint8_t)buf[195], 1);
    EXPECT_EQ((uint8_t)buf[196], 34);
    EXPECT_EQ((uint8_t)buf[197], 35);
    EXPECT_EQ((uint8_t)buf[198], 36);
    EXPECT_EQ((uint8_t)buf[199], 37);
    EXPECT_EQ((uint8_t)buf[200], 7);   // reverbPreset_
    EXPECT_EQ((uint8_t)buf[201], 0);   // reverbOutput_
    EXPECT_EQ((uint8_t)buf[202], 100); // reverbLevel_

    // 13 master-FX defaults (canonical v6 save/restore order) at 203..215.
    const uint8_t kFxD[] = {54, 74, 35, 41, 84, 63, 28, 69, 36, 46, 34, 50,
                            69};
    for (int i = 0; i < 13; i++) {
        SCOPED_TRACE(i);
        EXPECT_EQ((uint8_t)buf[203 + i], kFxD[i]);
    }
}

// FIXED (spec 4.8): getFullDefaultState used to write the 13 master-FX
// defaults in a DIFFERENT order (macro order: PREDELAYTIME, PREDELAYMIX,
// SIZE, ...) than the v6 save/restore order, so restoring a DEFAULT mix —
// what a new-bank load does — permuted 8 of the 13 params (DECAY received
// PREDELAYMIX_DEFAULT, ... NOTCHSPREAD received LOOPHP_DEFAULT). The default
// state is now written in the canonical v6 order and lroundf-quantized like
// the save path: every param lands on its OWN default.
TEST_F(MixerStateTest, DefaultBankMasterFxOrderMatchesRestore) {
    char buf[256];
    uint32_t size = 0;
    ms_.getFullDefaultState(buf, &size, 1);
    ASSERT_EQ(size, 216u);
    ms_.restoreFullState(buf);

    // All 13 params land on their OWN defaults.
    EXPECT_FLOAT_EQ(ms_.fxBus_.masterfxConfig[GLOBALFX_PREDELAYTIME], GLOBALFX_PREDELAYTIME_DEFAULT);
    EXPECT_FLOAT_EQ(ms_.fxBus_.masterfxConfig[GLOBALFX_DECAY], GLOBALFX_DECAY_DEFAULT);
    EXPECT_FLOAT_EQ(ms_.fxBus_.masterfxConfig[GLOBALFX_PREDELAYMIX], GLOBALFX_PREDELAYMIX_DEFAULT);
    EXPECT_FLOAT_EQ(ms_.fxBus_.masterfxConfig[GLOBALFX_SIZE], GLOBALFX_SIZE_DEFAULT);
    EXPECT_FLOAT_EQ(ms_.fxBus_.masterfxConfig[GLOBALFX_DIFFUSION], GLOBALFX_DIFFUSION_DEFAULT);
    EXPECT_FLOAT_EQ(ms_.fxBus_.masterfxConfig[GLOBALFX_DAMPING], GLOBALFX_DAMPING_DEFAULT);
    EXPECT_FLOAT_EQ(ms_.fxBus_.masterfxConfig[GLOBALFX_LFODEPTH], GLOBALFX_LFODEPTH_DEFAULT);
    EXPECT_FLOAT_EQ(ms_.fxBus_.masterfxConfig[GLOBALFX_LFOSPEED], GLOBALFX_LFOSPEED_DEFAULT);
    EXPECT_FLOAT_EQ(ms_.fxBus_.masterfxConfig[GLOBALFX_INPUTBASE], GLOBALFX_INPUTBASE_DEFAULT);
    EXPECT_FLOAT_EQ(ms_.fxBus_.masterfxConfig[GLOBALFX_INPUTWIDTH], GLOBALFX_INPUTWIDTH_DEFAULT);
    EXPECT_FLOAT_EQ(ms_.fxBus_.masterfxConfig[GLOBALFX_LOOPHP], GLOBALFX_LOOPHP_DEFAULT);
    EXPECT_FLOAT_EQ(ms_.fxBus_.masterfxConfig[GLOBALFX_NOTCHBASE], GLOBALFX_NOTCHBASE_DEFAULT);
    EXPECT_FLOAT_EQ(ms_.fxBus_.masterfxConfig[GLOBALFX_NOTCHSPREAD], GLOBALFX_NOTCHSPREAD_DEFAULT);
}

// Each version's minimal valid buffer has the exact documented size — a
// layout drift (a field added/removed) changes the size and fails here.
TEST_F(MixerStateTest, VersionBufferSizesAreExact) {
    EXPECT_EQ(BuildVersionBuffer(1).n, 176u);
    EXPECT_EQ(BuildVersionBuffer(2).n, 182u);
    EXPECT_EQ(BuildVersionBuffer(3).n, 189u);
    EXPECT_EQ(BuildVersionBuffer(4).n, 193u);
    EXPECT_EQ(BuildVersionBuffer(5).n, 194u);
    EXPECT_EQ(BuildVersionBuffer(6).n, 216u);
}

// v1 migration: the v1 fields restore; every field added in v2..v6 falls back
// to setDefaultValues() (pan 0, compressor t0=2, send 0, userCC 34..37,
// reverb 7/1.0/0, master-FX defaults, MPE 0).
TEST_F(MixerStateTest, RestoreVersion1MigratesWithDefaultsForNewerFields) {
    MixBuf mb = BuildVersionBuffer(1);
    ms_.restoreFullState(mb.b);

    EXPECT_EQ(std::string(ms_.mixName_, 12), std::string("UnitTestMix1"));
    EXPECT_EQ(ms_.currentChannel_, 3);
    EXPECT_EQ(ms_.globalChannel_, 2);
    EXPECT_EQ(ms_.midiThru_, 1);
    EXPECT_FLOAT_EQ(ms_.tuning_, 442.5f);
    EXPECT_EQ(ms_.instrumentState_[0].out, 1);
    EXPECT_EQ(ms_.instrumentState_[0].midiChannel, 2);
    EXPECT_EQ(ms_.instrumentState_[0].firstNote, 0);
    EXPECT_EQ(ms_.instrumentState_[0].lastNote, 100);
    EXPECT_EQ(ms_.instrumentState_[0].shiftNote, 0);
    EXPECT_EQ(ms_.instrumentState_[0].numberOfVoices, 0);
    EXPECT_EQ(ms_.instrumentState_[0].scaleScaleNumber, 0x0100);
    EXPECT_EQ(ms_.instrumentState_[5].out, 6);
    EXPECT_EQ(ms_.instrumentState_[5].lastNote, 105);
    EXPECT_NEAR(ms_.instrumentState_[5].volume, 0.5f + 0.5f, 1e-6f);

    // Fields v1 did not have -> defaults.
    EXPECT_EQ(ms_.instrumentState_[0].pan, 0);
    EXPECT_EQ(ms_.instrumentState_[0].compressorType, 2);
    EXPECT_EQ(ms_.instrumentState_[0].send, 0.0f);
    EXPECT_EQ(ms_.userCC_[0], 34);
    EXPECT_EQ(ms_.userCC_[3], 37);
    EXPECT_EQ(ms_.MPE_inst1_, 0);
    EXPECT_EQ(ms_.reverbPreset_, 7);
    EXPECT_FLOAT_EQ(ms_.reverbLevel_, 1.0f);
    EXPECT_EQ(ms_.reverbOutput_, 0);
    EXPECT_FLOAT_EQ(ms_.fxBus_.masterfxConfig[GLOBALFX_PREDELAYTIME],
                    GLOBALFX_PREDELAYTIME_DEFAULT);
}

// v2 adds pan; v3 adds compressor + levelMeter; v4 adds user CCs; v5 adds
// MPE. Each restores its new fields and everything older.
TEST_F(MixerStateTest, RestoreVersionsTwoThroughFiveAddFieldsProgressively) {
    {
        MixBuf mb = BuildVersionBuffer(2);
        ms_.restoreFullState(mb.b);
        EXPECT_EQ(ms_.instrumentState_[3].pan, 13);
        EXPECT_EQ(ms_.instrumentState_[3].compressorType, 0);  // default
        EXPECT_EQ(ms_.levelMeterWhere_, 1);                    // default
        EXPECT_EQ(ms_.userCC_[0], 34);                         // default
    }
    {
        MixBuf mb = BuildVersionBuffer(3);
        ms_.restoreFullState(mb.b);
        EXPECT_EQ(ms_.instrumentState_[2].compressorType, 2);  // t%4
        EXPECT_EQ(ms_.levelMeterWhere_, 2);
        EXPECT_EQ(ms_.userCC_[2], 36);  // default
    }
    {
        MixBuf mb = BuildVersionBuffer(4);
        ms_.restoreFullState(mb.b);
        EXPECT_EQ(ms_.userCC_[0], 40);
        EXPECT_EQ(ms_.userCC_[3], 43);
        EXPECT_EQ(ms_.MPE_inst1_, 0);  // default (v5 adds it)
    }
    {
        MixBuf mb = BuildVersionBuffer(5);
        ms_.restoreFullState(mb.b);
        EXPECT_EQ(ms_.MPE_inst1_, 1);
        EXPECT_EQ(ms_.instrumentState_[4].send, 0.0f);  // default (v6 adds it)
        EXPECT_FLOAT_EQ(ms_.fxBus_.masterfxConfig[GLOBALFX_DECAY],
                        GLOBALFX_DECAY_DEFAULT);  // default (v6 adds it)
    }
}

// v6 restores everything incl. per-timbre FX send, reverb block and the 13
// master-FX params (each `0.01f * byte`), in the documented slot order.
TEST_F(MixerStateTest, RestoreVersion6FullFieldSet) {
    MixBuf mb = BuildVersionBuffer(6);
    ms_.restoreFullState(mb.b);
    EXPECT_EQ(ms_.MPE_inst1_, 1);
    EXPECT_NEAR(ms_.instrumentState_[0].send, 0.01f * kParamBytes[0],
                1e-9f);
    EXPECT_NEAR(ms_.instrumentState_[5].send, 0.01f * kParamBytes[5],
                1e-9f);
    EXPECT_EQ(ms_.reverbPreset_, 9);
    EXPECT_EQ(ms_.reverbOutput_, 1);
    EXPECT_NEAR(ms_.reverbLevel_, 0.01f * 88, 1e-9f);
    // Buffer slot order -> GLOBALFX_* enum order.
    EXPECT_NEAR(ms_.fxBus_.masterfxConfig[GLOBALFX_PREDELAYTIME],
                0.01f * kParamBytes[0], 1e-9f);
    EXPECT_NEAR(ms_.fxBus_.masterfxConfig[GLOBALFX_DECAY],
                0.01f * kParamBytes[1], 1e-9f);
    EXPECT_NEAR(ms_.fxBus_.masterfxConfig[GLOBALFX_SIZE],
                0.01f * kParamBytes[3], 1e-9f);
    EXPECT_NEAR(ms_.fxBus_.masterfxConfig[GLOBALFX_LOOPHP],
                0.01f * kParamBytes[10], 1e-9f);
    EXPECT_NEAR(ms_.fxBus_.masterfxConfig[GLOBALFX_NOTCHSPREAD],
                0.01f * kParamBytes[12], 1e-9f);
}

// THE round-trip lock (v6): every valid percent byte (0..100) survives both
// restore->save and a second restore->save cycle exactly. Filling all 20
// percent-valued slots per iteration exercises every rounded serializer call.
TEST_F(MixerStateTest, RoundTripV6IsByteIdenticalForEveryPercentByte) {
    for (int value = 0; value <= 100; value++) {
        SCOPED_TRACE(value);
        MixBuf mb = BuildVersionBuffer(6);
        FillV6PercentBytes(mb, (uint8_t)value);
        ms_.restoreFullState(mb.b);

        char out1[256], out2[256];
        uint32_t n1 = 0, n2 = 0;
        ms_.getFullState(out1, &n1);
        ASSERT_EQ(n1, 216u);
        ASSERT_EQ(std::memcmp(out1, mb.b, n1), 0)
            << "v6 buffer must round-trip byte-identically";

        ms_.restoreFullState(out1);
        ms_.getFullState(out2, &n2);
        ASSERT_EQ(n2, 216u);
        EXPECT_EQ(std::memcmp(out1, out2, n1), 0)
            << "repeated save/load cycle must remain byte-stable";
    }
}

// Direct regression for the byte that exposed the truncation bug: the default
// predelay value 54 must remain 54 after repeated default-bank save/load.
TEST_F(MixerStateTest, DefaultPredelayByte54RemainsStableAcrossCycles) {
    char def[256], out1[256], out2[256];
    uint32_t n = 0;
    ms_.getFullDefaultState(def, &n, 0);
    ASSERT_EQ((uint8_t)def[203], 54) << "default predelay byte moved";
    ms_.restoreFullState(def);
    ms_.getFullState(out1, &n);
    EXPECT_EQ((uint8_t)out1[203], 54);
    ms_.restoreFullState(out1);
    ms_.getFullState(out2, &n);
    EXPECT_EQ((uint8_t)out2[203], 54);
    EXPECT_EQ(std::memcmp(out1, out2, n), 0);
}

// Unknown version: only setDefaultValues() runs — the mixer's serialized
// core (name/channels/tuning/instrument core) is left UNTOUCHED from the
// previous state, everything else resets to defaults. No crash, no garbage.
TEST_F(MixerStateTest, UnknownVersionResetsEverythingToDefaults) {
    // FIXED (6.7, owner decision: reset core too): unknown versions used to
    // retain the PREVIOUS mixer's serialized core (name/channels/tuning/
    // routing) while resetting only the defaulted fields. The core now
    // resets too — values mirror getFullDefaultState ("Mix 01").
    MixBuf mb = BuildVersionBuffer(1);
    ms_.restoreFullState(mb.b);
    ASSERT_EQ(std::string(ms_.mixName_, 4), std::string("Unit"));
    ASSERT_EQ(ms_.currentChannel_, 3);
    ASSERT_EQ(ms_.instrumentState_[0].out, 1);  // PutTimbre sentinel (1+t)

    char bad[64];
    std::memset(bad, 0xAB, sizeof(bad));
    bad[0] = 99;  // unknown version
    ms_.restoreFullState(bad);
    // Serialized core = defaults, nothing stale survives.
    EXPECT_EQ(std::string(ms_.mixName_, 6), std::string("Mix 01"))
        << "unknown version resets the mix name";
    EXPECT_EQ(ms_.mixName_[12], 0);
    EXPECT_EQ(ms_.currentChannel_, 0);
    EXPECT_EQ(ms_.globalChannel_, 0);
    EXPECT_EQ(ms_.midiThru_, 0);
    EXPECT_EQ(ms_.MPE_inst1_, 0);
    EXPECT_FLOAT_EQ(ms_.tuning_, 440.0f);
    const uint8_t outs[] = {1, 1, 4, 4, 6, 8};
    const uint8_t voices[] = {3, 3, 3, 2, 1, 1};
    for (int t = 0; t < NUMBER_OF_TIMBRES; t++) {
        EXPECT_EQ(ms_.instrumentState_[t].out, outs[t]) << "timbre " << t;
        EXPECT_EQ(ms_.instrumentState_[t].midiChannel, 1 + t) << "timbre " << t;
        EXPECT_EQ(ms_.instrumentState_[t].firstNote, 0) << "timbre " << t;
        EXPECT_EQ(ms_.instrumentState_[t].lastNote, 127) << "timbre " << t;
        EXPECT_EQ(ms_.instrumentState_[t].shiftNote, 0) << "timbre " << t;
        EXPECT_EQ(ms_.instrumentState_[t].numberOfVoices, voices[t]) << "timbre " << t;
        EXPECT_EQ(ms_.instrumentState_[t].scalaEnable, 0) << "timbre " << t;
        EXPECT_EQ(ms_.instrumentState_[t].scalaMapping, 0) << "timbre " << t;
        EXPECT_EQ(ms_.instrumentState_[t].scaleScaleNumber, 0) << "timbre " << t;
        EXPECT_EQ(ms_.instrumentState_[t].scalaScaleFileName[0], 0) << "timbre " << t;
        EXPECT_FLOAT_EQ(ms_.instrumentState_[t].volume, 1.0f) << "timbre " << t;
    }
    // ...and the defaulted fields still reset.
    EXPECT_EQ(ms_.userCC_[0], 34);
    EXPECT_EQ(ms_.reverbPreset_, 7);
    EXPECT_EQ(ms_.instrumentState_[0].compressorType, 2);
    EXPECT_EQ(ms_.instrumentState_[0].pan, 0);
    EXPECT_EQ(ms_.instrumentState_[0].send, 0.0f);
    // Version 0 is equally unknown.
    bad[0] = 0;
    ms_.restoreFullState(bad);
    EXPECT_EQ(std::string(ms_.mixName_, 6), std::string("Mix 01"));
    EXPECT_EQ(ms_.reverbPreset_, 7);
}

TEST_F(MixerStateTest, KnownVersionRestoreStillOverwritesCoreDefaults) {
    // 6.7 companion: setDefaultValues now writes core defaults BEFORE the
    // per-version reader runs — every known version must still overwrite
    // all of them with the buffer's values (no default leaking through).
    for (int v = 1; v <= 6; v++) {
        SCOPED_TRACE(v);
        MixBuf mb = BuildVersionBuffer(v);
        ms_.restoreFullState(mb.b);
        EXPECT_EQ(std::string(ms_.mixName_, 4), std::string("Unit"))
            << "version " << v << " must overwrite the default name";
        EXPECT_EQ(ms_.currentChannel_, 3);
        EXPECT_EQ(ms_.globalChannel_, 2);
        EXPECT_EQ(ms_.midiThru_, 1);
        EXPECT_FLOAT_EQ(ms_.tuning_, 442.5f);
        EXPECT_EQ(ms_.instrumentState_[0].out, 1);
        EXPECT_EQ(ms_.instrumentState_[0].midiChannel, 2);
        EXPECT_EQ(ms_.instrumentState_[0].firstNote, 0);
        EXPECT_EQ(ms_.instrumentState_[0].lastNote, 100);
        EXPECT_EQ(ms_.instrumentState_[5].numberOfVoices, 5 % 4);
        EXPECT_FLOAT_EQ(ms_.instrumentState_[5].volume, 0.5f + 0.1f * 5);
        EXPECT_EQ(ms_.instrumentState_[0].scalaScaleFileName[0], 'a');
        EXPECT_EQ(ms_.instrumentState_[5].scalaScaleFileName[0], 'a' + 5);
        EXPECT_EQ(ms_.instrumentState_[5].scaleScaleNumber, (1 << 8) + 5);
    }
}

// getMixNameFromFile: the name is at a fixed offset (+1) in every version.
TEST_F(MixerStateTest, GetMixNameFromFilePointsPastVersionByte) {
    char buf[16] = {};
    buf[0] = MIXER_BANK_CURRENT_VERSION;
    std::memcpy(buf + 1, "Mix42\0\0\0\0\0\0\0", 12);
    char* name = MixerState::getMixNameFromFile(buf);
    EXPECT_EQ(name, buf + 1);
    EXPECT_EQ(std::string(name, 5), std::string("Mix42"));
}

}  // namespace
