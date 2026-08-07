// Host-side coverage for firmware/Src/midi/Sequencer.cpp serialization.
//
// Regression target (per tests/README.md roadmap, row 1):
//   The -Ofast unaligned-float hard-fault. The sequencer packs a float (tempo)
//   at buffer offset 14, which is 2 mod 4 — MISALIGNED for a 4-byte float on
//   Cortex-M7. The firmware now uses __builtin_memcpy helpers
//   (pfm3_seq_put_f32 / pfm3_seq_get_f32) at that offset; these tests lock the
//   byte layout those helpers produce as a contract, and prove the float
//   round-trips bit-exactly through serialize -> deserialize -> serialize.
//
// Fidelity caveat (tests/SEAM.md §d.1): a host CPU permits unaligned float
// access, so these tests CANNOT reproduce the Cortex-M7 fault. Their job is to
// assert byte-faithful round-trips — the proxy for the __builtin_memcpy fix
// being intact and correct. If the layout, offset, version dispatch, or helper
// implementation drifts, these tests fail. Running under -fsanitize=undefined
// additionally confirms the current code path is free of the UB the old
// *(float*)&buf[i] cast carried.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>

#include "Sequencer.h"  // firmware-under-test (host-compilable via PFM3_HOST seam)

namespace {

// Serialization layout constants — must match getFullDefaultState / getFullState
// / loadStateVersion2 in firmware/Src/midi/Sequencer.cpp. Encoding them here is
// deliberate: if the firmware layout drifts, these tests fail loudly.
//
// Layout (V2): [0]version [1..12]name [13]extClock [14..17]tempo(f32,MISALIGNED)
//   [18..19]lastFreeAction(u16)  then 6 timbres * 8 bytes
//   (stepUnique u16, timerMask u16, seqActivated, recording, muted,
//    instrumentStepSeq)  then 12 step-sequences * 1 byte (stepActivated).
//   Total = 1+12+1+4+2 + 6*8 + 12 = 80.
constexpr uint32_t kExpectedStateSize = 80;
constexpr uint32_t kVersionOffset = 0;
constexpr uint32_t kNameOffset = 1;
constexpr uint32_t kNameLen = 12;
constexpr uint32_t kExternalClockOffset = 13;
constexpr uint32_t kTempoOffset = 14;             // 2 mod 4 => MISALIGNED float
constexpr uint32_t kLastFreeActionOffset = 18;    // 18 mod 2 => aligned u16
constexpr uint8_t kCurrentVersion = SEQ_VERSION2;  // == SEQ_CURRENT_VERSION

// Build a Sequencer with null collaborators. The serialization path under test
// (ctor -> getFullDefaultState -> setFullState -> loadStateVersion2 -> get/
// setFullState) never dereferences synth_ or displaySequencer_: confirmed by
// reading Sequencer::Sequencer(), reset(false), and the load/getFullState
// bodies. The collaborator stubs (stubs/sequencer_collaborators_stub.cpp)
// satisfy the rest of Sequencer.o's compiled references.
std::unique_ptr<Sequencer> MakeSequencer() {
    auto s = std::make_unique<Sequencer>();
    s->setSynth(nullptr);
    s->setDisplaySequencer(nullptr);
    return s;
}

// Host-safe reference: the exact IEEE-754 bytes of a float, via memcpy (never
// through a cast — the whole point of the firmware fix).
uint32_t FloatBits(float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

// Read 4 little-endian bytes at an offset as a uint32, via memcpy. Asserts the
// offset + length stay in bounds of a 256-byte state buffer.
uint32_t ReadU32Le(const uint8_t* buf, uint32_t off) {
    uint32_t v = 0;
    std::memcpy(&v, buf + off, sizeof(v));
    return v;
}

}  // namespace

// ---------------------------------------------------------------------------
// Layout contract — locks the byte format the __builtin_memcpy helpers target.
// ---------------------------------------------------------------------------
TEST(SeqSerialization, DefaultStateHasExpectedLayout) {
    auto s = MakeSequencer();
    uint8_t buf[256] = {};
    uint32_t size = 0;
    s->getFullDefaultState(buf, &size, /*seqNumber=*/0);

    ASSERT_EQ(size, kExpectedStateSize)
        << "State size drifted; every offset assertion below is now suspect";
    EXPECT_EQ(buf[kVersionOffset], kCurrentVersion);
    EXPECT_EQ(buf[kExternalClockOffset], 1) << "default external-clock flag is on";

    // Default tempo is 90.0f (see getFullDefaultState). It is written at offset
    // 14 by pfm3_seq_put_f32 — the exact site that hard-faulted under -Ofast.
    EXPECT_EQ(ReadU32Le(buf, kTempoOffset), FloatBits(90.0f))
        << "default tempo bytes at the MISALIGNED offset 14 are not IEEE-754 of 90.0f";

    // Default lastFreeAction is 12 (see getFullDefaultState).
    uint16_t lfa = 0;
    std::memcpy(&lfa, buf + kLastFreeActionOffset, 2);
    EXPECT_EQ(lfa, 12u);

    // Default name "Seq 0\0..." (seqNumber=0 -> '0','0').
    EXPECT_EQ(std::memcmp(buf + kNameOffset, "Seq 00", 6), 0);
}

// ---------------------------------------------------------------------------
// THE regression guard. Two tests:
//   (1) self-check that offset 14 is genuinely misaligned, so the misalignment
//       story is consciously revisited if anyone ever re-aligns tempo;
//   (2) tempo round-trips bit-exactly across byte-pattern-diverse values.
// ---------------------------------------------------------------------------
TEST(UnalignedFloatRegression, TempoOffsetIsGenuinelyMisaligned) {
    // If a future change moves tempo to an aligned offset (e.g. 16), this fails
    // loudly. The __builtin_memcpy helpers remain correct, but the "unaligned"
    // framing in tests/SEAM.md would need updating — hence the explicit guard.
    alignas(32) uint8_t buf[256] = {};
    ASSERT_EQ(reinterpret_cast<uintptr_t>(buf) % 32, 0u);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(buf + kTempoOffset) % 4, 2u)
        << "tempo is no longer at a 2-mod-4 misaligned offset";
}

TEST(UnalignedFloatRegression, TempoRoundTripsBitExactlyAcrossValues) {
    // Floats chosen to stress distinct byte patterns: the firmware default, an
    // all-zero-bits value, a value with 0x00 bytes, a fractional, a small int,
    // a large value. Each must survive setTempo -> getFullState (PUT) and
    // setFullState -> getFullState (GET round-trip) with bit-identical bytes at
    // the misaligned offset 14.
    const float tempos[] = {
        90.0f,        // firmware default
        0.0f,         // all-zero bits
        1.0f,         // 0x3f800000
        123.456f,     // arbitrary fractional
        240.0f,       // 0x43700000 — contains 0x00 bytes
        33.33333f,    // repeating fraction
        20000.0f,     // large
    };

    for (float t : tempos) {
        SCOPED_TRACE(::testing::PrintToString(t));

        auto s1 = MakeSequencer();
        s1->setTempo(t);

        uint8_t buf1[256] = {};
        uint32_t size1 = 0;
        s1->getFullState(buf1, &size1);
        ASSERT_EQ(size1, kExpectedStateSize);

        // PUT fidelity: bytes at the misaligned offset are exactly tempo's
        // IEEE-754 representation. This is what __builtin_memcpy guarantees;
        // *(float*)&buf[14] was the -Ofast hard-fault path.
        EXPECT_EQ(ReadU32Le(buf1, kTempoOffset), FloatBits(t))
            << "PUT corrupted tempo at misaligned offset 14";

        // GET fidelity: deserialize into a fresh instance, re-serialize, and
        // require offset 14 to be unchanged. Exercises get_f32 -> setTempo ->
        // put_f32; proves the round-trip is bit-exact at the misaligned offset.
        auto s2 = MakeSequencer();
        s2->setFullState(buf1);

        uint8_t buf2[256] = {};
        uint32_t size2 = 0;
        s2->getFullState(buf2, &size2);
        ASSERT_EQ(size2, kExpectedStateSize);

        EXPECT_EQ(ReadU32Le(buf2, kTempoOffset), FloatBits(t))
            << "GET round-trip altered tempo at misaligned offset 14";
    }
}

// ---------------------------------------------------------------------------
// Full round-trip equality on a representative, differentiated preset.
// Uses only public setters that need no collaborators, so it stays within the
// host-compilable serialization surface.
// ---------------------------------------------------------------------------
TEST(SeqRoundTrip, DifferentiatedPresetRoundTripsByteForByte) {
    auto s1 = MakeSequencer();
    s1->setSequenceName("TEST42.....");  // exactly kNameLen chars
    s1->setTempo(111.0f);
    s1->setExternalClock(true);
    s1->setNumberOfBars(0, 2);  // instrument 0 -> 2 bars
    s1->setNumberOfBars(1, 4);  // instrument 1 -> 4 bars

    uint8_t buf1[256] = {};
    uint32_t size1 = 0;
    s1->getFullState(buf1, &size1);
    ASSERT_EQ(size1, kExpectedStateSize);

    auto s2 = MakeSequencer();
    s2->setFullState(buf1);

    uint8_t buf2[256] = {};
    uint32_t size2 = 0;
    s2->getFullState(buf2, &size2);
    ASSERT_EQ(size2, kExpectedStateSize);

    // Whole-buffer equality: catches any field the deserialize path fails to
    // restore (name, tempo, masks, external clock, instrumentStepSeq, ...)
    // without poking private members.
    EXPECT_EQ(std::memcmp(buf1, buf2, size1), 0)
        << "serialize -> deserialize -> serialize diverged";
}

// ---------------------------------------------------------------------------
// Determinism: two independently-constructed Sequencers serialize identically.
// Guards against accidental non-determinism (uninitialized padding fields,
// heap-dependent layout) leaking into the on-disk format.
// ---------------------------------------------------------------------------
TEST(SeqSerialization, DefaultStateIsDeterministicAcrossInstances) {
    uint8_t a[256] = {};
    uint8_t b[256] = {};
    uint32_t sa = 0, sb = 0;

    {
        auto s = MakeSequencer();
        s->getFullState(a, &sa);
    }
    {
        auto s = MakeSequencer();
        s->getFullState(b, &sb);
    }

    ASSERT_EQ(sa, sb);
    EXPECT_EQ(std::memcmp(a, b, sa), 0)
        << "two default Sequencers produced different on-disk bytes";
}

// ---------------------------------------------------------------------------
// Version dispatch: a hand-built V1 buffer must parse. V1 and V2 share the
// tempo offset (14) but differ in the per-timbre block layout; this test
// confirms the misaligned float survives the V1->V2 migration path.
// ---------------------------------------------------------------------------
TEST(SeqSerialization, Version1BufferParsesAndPreservesTempo) {
    // V1 layout (loadStateVersion1):
    //   [0] version=1, [1..12] name, [13] extClock,
    //   [14..17] tempo (MISALIGNED), [18..19] lastFreeAction,
    //   then 6 timbres * 8 bytes (stepUnique u16, timerMask u16, seqActivated,
    //   stepActivated, recording, muted). No step-seq trailer.
    //   loadStateVersion1 reads exactly 1+12+1+4+2 + 6*8 = 68 bytes; the buffer
    //   is sized to the V2 total (80) so there is no over-read under ASan.
    alignas(8) uint8_t v1[80] = {};
    v1[0] = SEQ_VERSION1;
    std::memcpy(v1 + kNameOffset, "V1TEST      ", kNameLen);
    v1[kExternalClockOffset] = 0;
    const float tempo = 75.0f;
    std::memcpy(v1 + kTempoOffset, &tempo, 4);  // write via safe memcpy
    const uint16_t lfa = 7;
    std::memcpy(v1 + kLastFreeActionOffset, &lfa, 2);

    auto s = MakeSequencer();
    s->setFullState(v1);  // dispatches to loadStateVersion1

    uint8_t out[256] = {};
    uint32_t size = 0;
    s->getFullState(out, &size);  // re-serializes as V2 (current)
    ASSERT_EQ(size, kExpectedStateSize);

    EXPECT_EQ(out[kVersionOffset], kCurrentVersion)
        << "re-serialize must emit the current version, not the loaded one";
    EXPECT_EQ(ReadU32Le(out, kTempoOffset), FloatBits(tempo))
        << "V1->V2 migration corrupted the misaligned tempo float";
}
