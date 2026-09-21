// Host-side coverage for firmware/Src/pfm3_diag.cpp — the restored 8.1
// diagnostic module (phase 8.8 SW5). The module's hardware surface (DWT,
// SysTick/SCB, LED, USB MIDI, fault shims) is target-only; what IS
// host-testable is exercised here through the PFM3_HOST seam:
//
//   (a) the crash-capture replay engine: encoding (the FROZEN contract that
//       scripts/hardware/h8_crashwatch.py decodes — CC#48..63 on ch16,
//       [0]=0xC7 magic, [1]=frameIdx / 0xFF trailer, payload pairs b>>1/b&1,
//       7 struct bytes per frame), the boot settle + 5 ms pacing, and the
//       consume-once rule (magic cleared after the trailer, never streamed
//       twice);
//   (b) invalid magic (power-cycled SRAM / no capture) -> no replay at all;
//   (c) the host command codes (CC#119 / magic SysEx entry points) setting
//       exactly the documented state, and the CC/SysEx hook matching;
//   (d) the preenfm3Tic section-timing accumulators;
//
// The companion TU tests/pfm3_diag_disabled_test.cpp proves the OTHER gate
// branch: with neither PFM3_DIAG_ENABLED nor PFM3_HOST defined every hook
// compiles to an inline no-op.
//
// Module-state access goes through the PFM3_HOST test-hook block at the
// bottom of pfm3_diag.cpp (pfm3DiagTest*) plus the header's public externs —
// no widening of the target API. The replay burst is recorded by the host
// sink instead of USB TX; the poll clock is synthetic milliseconds.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "pfm3_diag.h"

// --- PFM3_HOST test hooks (definitions live in pfm3_diag.cpp) ---------------
// Pfm3DiagTestSecStatsMirror mirrors the layout of Pfm3DiagTestSecStats
// defined in pfm3_diag.cpp (plain 8x uint32 struct — identical layout).
extern "C" {
void pfm3DiagTestReset(void);
int pfm3DiagTestReplayPoll(uint32_t nowMs);
int pfm3DiagTestReplayFrameCount(void);
void pfm3DiagTestReplayFrame(int idx, uint8_t out[16]);
void pfm3DiagTestSetResetCause(int wasSoftwareReset);

struct Pfm3DiagTestSecStatsMirror {
    uint32_t count;
    uint32_t encTotal, seqTotal, tftTotal, totalTotal;
    uint32_t encMax, seqMax, tftMax;
};
void pfm3DiagTestSecStats(Pfm3DiagTestSecStatsMirror *out);
}

namespace {

// Frames in a full burst: 76 struct bytes, 7 per frame -> 11 data frames,
// then the 0xFF trailer. Mirrors pfm3DiagReplayPoll's formula.
constexpr int kReplayDataFrames = (int)(sizeof(Pfm3DiagFaultInfo) + 6) / 7;  // 11
constexpr int kReplayTotalFrames = kReplayDataFrames + 1;                     // 12

// Faithful transcription of the FROZEN decoder's commit logic
// (scripts/hardware/h8_crashwatch.py, main loop): a frame begins ONLY when a
// slot-0 CC carries the 0xC7 replay magic; slots 1..15 accumulate into a
// pending buffer; a frame is COMMITTED ONLY when its slot-15 CC arrives
// (frames[fr[1]] = fr). An under-emitted final data frame or trailer is
// therefore NEVER committed — the P7 defect this mirror exists to catch.
struct FrozenDecoderMirror {
    uint8_t pending[16];
    int pendingLen = 0;
    uint8_t frames[16][16];   // committed frames by frameIdx (0..10)
    bool frameSeen[16] = {};
    bool trailerCommitted = false;
    int commits = 0;

    void slot(uint8_t s, uint8_t v) {
        if (s == 0 && v == 0xC7) {
            pendingLen = 0;
            pending[pendingLen++] = v;
        } else if (pendingLen > 0 && 1 <= s && s <= 15) {
            pending[pendingLen++] = v;
            if (s == 15) {
                int idx = pending[1];
                if (idx == 0xFF) {
                    trailerCommitted = true;
                } else if (idx < 16) {
                    memcpy(frames[idx], pending, 16);
                    frameSeen[idx] = true;
                }
                commits++;
                pendingLen = 0;
            }
        }
    }

    // decode_frames(): committed data frames in index order, payload pairs
    // (lo<<1)|hi, then struct.unpack("<19I", data[:76]).
    bool decode(uint32_t out[19]) {
        uint8_t data[12 * 7];
        int n = 0;
        for (int f = 0; f < 16; f++) {
            if (!frameSeen[f]) continue;
            const uint8_t *vals = frames[f] + 2;  // drop magic+idx slots
            for (int j = 0; j + 1 < 14; j += 2) {
                data[n++] = (uint8_t)((vals[j] << 1) | vals[j + 1]);
            }
        }
        if (n < 76) return false;
        for (int w = 0; w < 19; w++) {
            out[w] = (uint32_t)data[w * 4]
                | ((uint32_t)data[w * 4 + 1] << 8)
                | ((uint32_t)data[w * 4 + 2] << 16)
                | ((uint32_t)data[w * 4 + 3] << 24);
        }
        return true;
    }
};

// Plants a distinctive capture so byte-level encoding checks catch any
// field-order drift (the 8.1 decoder is frozen on this exact layout).
void plantCapture(uint32_t faultId, uint32_t cfsr) {
    pfm3DiagFault.magic = PFM3_DIAG_FAULT_MAGIC;
    pfm3DiagFault.faultId = faultId;
    pfm3DiagFault.cfsr = cfsr;
    pfm3DiagFault.hfsr = 0x40000000u;
    pfm3DiagFault.dfsr = 0x00000008u;
    pfm3DiagFault.mmfar = 0x2001ABCDu;
    pfm3DiagFault.bfar = 0x08021ABCu;
    pfm3DiagFault.afsr = 0;
    pfm3DiagFault.r0 = 0x11111110u;
    pfm3DiagFault.r1 = 0x22222221u;
    pfm3DiagFault.r2 = 0x33333332u;
    pfm3DiagFault.r3 = 0x44444443u;
    pfm3DiagFault.r12 = 0x55555554u;
    pfm3DiagFault.lr = 0x08001234u;
    pfm3DiagFault.psr = 0x61000000u;
    pfm3DiagFault.pc = 0x08021234u;
    pfm3DiagFault.sysTickCtrl = 0x00000007u;
    pfm3DiagFault.sysTickAliveCpt = 0x00ABCDEFu;
    pfm3DiagFault.mainLoopSeenAlive = 1;
}

// P18 golden fixture: the 76 replay bytes HAND-WRITTEN from the documented
// field order (19 x uint32 LE: magic, faultId, cfsr, hfsr, dfsr, mmfar, bfar,
// afsr, r0, r1, r2, r3, r12, lr, psr, pc, sysTickCtrl, sysTickAliveCpt,
// mainLoopSeenAlive) with plantCapture(4, 0x00010000)'s values — deliberately
// NOT derived from the live struct, so a struct layout change that dodges the
// static_asserts still fails here.
const uint8_t kGoldenUsageFaultFrame[76] = {
    /* magic 0x08C1FA01   */ 0x01, 0xFA, 0xC1, 0x08,
    /* faultId 4          */ 0x04, 0x00, 0x00, 0x00,
    /* cfsr 0x00010000    */ 0x00, 0x00, 0x01, 0x00,
    /* hfsr 0x40000000    */ 0x00, 0x00, 0x00, 0x40,
    /* dfsr 0x00000008    */ 0x08, 0x00, 0x00, 0x00,
    /* mmfar 0x2001ABCD   */ 0xCD, 0xAB, 0x01, 0x20,
    /* bfar 0x08021ABC    */ 0xBC, 0x1A, 0x02, 0x08,
    /* afsr 0             */ 0x00, 0x00, 0x00, 0x00,
    /* r0 0x11111110      */ 0x10, 0x11, 0x11, 0x11,
    /* r1 0x22222221      */ 0x21, 0x22, 0x22, 0x22,
    /* r2 0x33333332      */ 0x32, 0x33, 0x33, 0x33,
    /* r3 0x44444443      */ 0x43, 0x44, 0x44, 0x44,
    /* r12 0x55555554     */ 0x54, 0x55, 0x55, 0x55,
    /* lr 0x08001234      */ 0x34, 0x12, 0x00, 0x08,
    /* psr 0x61000000     */ 0x00, 0x00, 0x00, 0x61,
    /* pc 0x08021234      */ 0x34, 0x12, 0x02, 0x08,
    /* sysTickCtrl 7      */ 0x07, 0x00, 0x00, 0x00,
    /* sysTickAliveCpt    */ 0xEF, 0xCD, 0xAB, 0x00,
    /* mainLoopSeenAlive 1*/ 0x01, 0x00, 0x00, 0x00,
};

// Streams the recorded burst through the frozen-decoder mirror: every
// emitted (slot, value) pair, in emission order (slot 0..15 per frame).
static int FeedBurstToFrozenDecoder(FrozenDecoderMirror *dec, uint8_t outFrames[13][16]) {
    const int total = pfm3DiagTestReplayFrameCount();
    for (int f = 0; f < total; f++) {
        uint8_t cc[16];
        pfm3DiagTestReplayFrame(f, cc);
        memcpy(outFrames[f], cc, 16);
        for (int s = 0; s < 16; s++) {
            dec->slot((uint8_t)s, cc[s]);
        }
    }
    return total;
}

// The decoder-side inverse of the replay encoding (h8_crashwatch.py
// decode_frames, transcribed): reassembles a little-endian word from the
// reconstructed struct bytes.
static uint32_t leWord(const uint8_t *data, int w) {
    return (uint32_t)data[w * 4 + 0] | ((uint32_t)data[w * 4 + 1] << 8)
        | ((uint32_t)data[w * 4 + 2] << 16) | ((uint32_t)data[w * 4 + 3] << 24);
}

}  // namespace

// --- (a) replay encoding + pacing + consume-once -----------------------------

TEST(Pfm3DiagReplay, EncodesFrozenContractAndConsumesOnce) {
    pfm3DiagTestReset();
    plantCapture(4, 0x00010000u);  // UsageFault, UFSR UNDEFINSTR (bit 16)

    // Boot: magic valid -> armed, but the 3 s USB settle holds the burst.
    EXPECT_EQ(pfm3DiagTestReplayPoll(0), 0);
    EXPECT_EQ(pfm3DiagTestReplayFrameCount(), 0);
    EXPECT_EQ(pfm3DiagTestReplayPoll(3000), 0);   // strictly-greater boundary
    EXPECT_EQ(pfm3DiagTestReplayFrameCount(), 0);
    EXPECT_EQ(pfm3DiagTestReplayPoll(3001), 0);   // arms streaming, no emit yet
    EXPECT_EQ(pfm3DiagTestReplayFrameCount(), 0);
    EXPECT_EQ(pfm3DiagTestReplayPoll(3002), 0);   // < 5 ms since arm: pacing
    EXPECT_EQ(pfm3DiagTestReplayFrameCount(), 0);

    // Stream: one frame per >= 5 ms poll.
    const struct {
        uint32_t faultId, cfsr, hfsr, dfsr, mmfar, bfar, afsr;
        uint32_t r0, r1, r2, r3, r12, lr, psr, pc;
        uint32_t sysTickCtrl, sysTickAliveCpt, mainLoopSeenAlive;
    } planted = {
        4, 0x00010000u, 0x40000000u, 0x00000008u, 0x2001ABCDu,
        0x08021ABCu, 0, 0x11111110u, 0x22222221u, 0x33333332u, 0x44444443u,
        0x55555554u, 0x08001234u, 0x61000000u, 0x08021234u, 0x00000007u,
        0x00ABCDEFu, 1};

    uint8_t raw[76];
    memcpy(raw, &pfm3DiagFault, sizeof raw);

    int emitted = 0;
    for (uint32_t t = 3006; emitted < kReplayTotalFrames; t += 6) {
        int rc = pfm3DiagTestReplayPoll(t);
        if (rc == 1) {
            emitted = pfm3DiagTestReplayFrameCount();
        }
    }
    ASSERT_EQ(emitted, kReplayTotalFrames);

    // Byte-level encoding check against the frozen contract, frame by frame.
    for (int f = 0; f < kReplayDataFrames; f++) {
        uint8_t cc[16];
        pfm3DiagTestReplayFrame(f, cc);
        EXPECT_EQ(cc[0], 0xC7) << "frame " << f << " slot 0 replay magic";
        EXPECT_EQ(cc[1], (uint8_t)f) << "frame " << f << " slot 1 index";
        for (int j = 0; j < 7; j++) {
            int byteIdx = f * 7 + j;
            if (byteIdx >= 76) break;
            EXPECT_EQ(cc[2 + 2 * j], (uint8_t)(raw[byteIdx] >> 1))
                << "frame " << f << " byte " << byteIdx << " hi";
            EXPECT_EQ(cc[3 + 2 * j], (uint8_t)(raw[byteIdx] & 1))
                << "frame " << f << " byte " << byteIdx << " lo";
        }
    }

    // Trailer: magic, 0xFF marker, faultId, slots 3..15 zero-padded (the
    // decoder commits ONLY on slot 15 — every frame emits all 16 slots).
    uint8_t trailer[16];
    pfm3DiagTestReplayFrame(kReplayDataFrames, trailer);
    EXPECT_EQ(trailer[0], 0xC7);
    EXPECT_EQ(trailer[1], 0xFF);
    EXPECT_EQ(trailer[2], (uint8_t)planted.faultId);
    for (int s = 3; s < 16; s++) {
        EXPECT_EQ(trailer[s], 0u) << "trailer slot " << s << " must be emitted (0-padded)";
    }

    // EVERY data frame carries slot 15 (P7): the last data frame (index 10,
    // 6 payload bytes) must still emit through slot 15 with zero padding.
    uint8_t lastData[16];
    pfm3DiagTestReplayFrame(kReplayDataFrames - 1, lastData);
    EXPECT_EQ(lastData[14], (uint8_t)(raw[76 - 1] >> 1));
    EXPECT_EQ(lastData[15], (uint8_t)(raw[76 - 1] & 1));

    // Decoder-side inverse (h8_crashwatch.py): reassemble LE words.
    uint8_t data[76];
    memset(data, 0, sizeof data);
    for (int f = 0; f < kReplayDataFrames; f++) {
        uint8_t cc[16];
        pfm3DiagTestReplayFrame(f, cc);
        for (int j = 0; j < 7 && f * 7 + j < 76; j++) {
            data[f * 7 + j] = (uint8_t)((cc[2 + 2 * j] << 1) | cc[3 + 2 * j]);
        }
    }
    EXPECT_EQ(leWord(data, 0), PFM3_DIAG_FAULT_MAGIC);
    EXPECT_EQ(leWord(data, 1), planted.faultId);
    EXPECT_EQ(leWord(data, 2), planted.cfsr);
    EXPECT_EQ(leWord(data, 15), planted.pc);            // word 15 = pc
    EXPECT_EQ(leWord(data, 16), planted.sysTickCtrl);   // word 16
    EXPECT_EQ(leWord(data, 17), planted.sysTickAliveCpt);
    EXPECT_EQ(leWord(data, 18), planted.mainLoopSeenAlive);

    // Consume-once: the trailer cleared the magic; no frame ever again.
    EXPECT_EQ(pfm3DiagFault.magic, 0u);
    EXPECT_EQ(pfm3DiagTestReplayPoll(10000), 0);
    EXPECT_EQ(pfm3DiagTestReplayPoll(20000), 0);
    EXPECT_EQ(pfm3DiagTestReplayFrameCount(), kReplayTotalFrames);
}

// --- (b) invalid magic -> no replay ------------------------------------------

TEST(Pfm3DiagReplay, InvalidMagicNeverStreams) {
    pfm3DiagTestReset();
    // Default: magic 0 (power-cycled SRAM).
    for (uint32_t t = 0; t <= 5000; t += 100) {
        pfm3DiagTestReplayPoll(t);
    }
    EXPECT_EQ(pfm3DiagTestReplayFrameCount(), 0);

    // Garbage magic (partial SRAM decay) also never streams.
    pfm3DiagTestReset();
    pfm3DiagFault.magic = 0xDEADBEEFu;
    for (uint32_t t = 0; t <= 5000; t += 100) {
        pfm3DiagTestReplayPoll(t);
    }
    EXPECT_EQ(pfm3DiagTestReplayFrameCount(), 0);
    EXPECT_EQ(pfm3DiagFault.magic, 0xDEADBEEFu);  // untouched: not consumed
}

// --- (b') boot-validity gate (P16) -------------------------------------------

TEST(Pfm3DiagReplay, ColdBootCaptureNeverStreams) {
    // Valid capture + valid faultId, but the reset cause was NOT a software
    // reset (cold boot: SRAM coincidence) -> never streams, not consumed
    // (preserved for SWD inspection).
    pfm3DiagTestReset();
    plantCapture(4, 0x00010000u);
    pfm3DiagTestSetResetCause(0);
    for (uint32_t t = 0; t <= 5000; t += 100) {
        pfm3DiagTestReplayPoll(t);
    }
    EXPECT_EQ(pfm3DiagTestReplayFrameCount(), 0);
    EXPECT_EQ(pfm3DiagFault.magic, PFM3_DIAG_FAULT_MAGIC);  // not consumed
}

TEST(Pfm3DiagReplay, GarbageFaultIdNeverStreams) {
    // Valid magic + software reset, but faultId outside the documented 1..6
    // -> never streams (guards a torn write exposing a stale magic).
    pfm3DiagTestReset();
    plantCapture(7, 0);        // 7: not a contract faultId
    for (uint32_t t = 0; t <= 5000; t += 100) {
        pfm3DiagTestReplayPoll(t);
    }
    EXPECT_EQ(pfm3DiagTestReplayFrameCount(), 0);
    pfm3DiagTestReset();
    plantCapture(0, 0);        // 0: also invalid
    for (uint32_t t = 0; t <= 5000; t += 100) {
        pfm3DiagTestReplayPoll(t);
    }
    EXPECT_EQ(pfm3DiagTestReplayFrameCount(), 0);
}

// --- (a') frozen-decoder fidelity (P7) + golden fixture (P18) -----------------

// The emitted CC stream must survive a FAITHFUL copy of h8_crashwatch.py's
// commit logic: every frame commits (slot 15 arrives for data frames AND the
// trailer — the under-emission defect could never pass this), the burst
// reassembles to >= 76 bytes, and all 19 words unpack to the planted values.
TEST(Pfm3DiagReplay, EmittedCcStreamDecodesViaFrozenDecoder) {
    pfm3DiagTestReset();
    plantCapture(4, 0x00010000u);
    for (uint32_t t = 3006; pfm3DiagTestReplayFrameCount() < kReplayTotalFrames; t += 6) {
        pfm3DiagTestReplayPoll(t);
    }
    ASSERT_EQ(pfm3DiagTestReplayFrameCount(), kReplayTotalFrames);

    FrozenDecoderMirror dec;
    uint8_t outFrames[13][16];
    ASSERT_EQ(FeedBurstToFrozenDecoder(&dec, outFrames), kReplayTotalFrames);

    // ALL frames committed: 11 data + the trailer (the old encoder emitted
    // the final data frame and trailer without slot 15 -> never committed,
    // burst unpacked at 70 bytes).
    EXPECT_EQ(dec.commits, kReplayTotalFrames);
    EXPECT_TRUE(dec.trailerCommitted) << "trailer never committed: decoder would not terminate the burst";
    for (int f = 0; f < kReplayDataFrames; f++) {
        EXPECT_TRUE(dec.frameSeen[f]) << "data frame " << f << " never committed";
    }

    uint32_t words[19];
    ASSERT_TRUE(dec.decode(words));
    EXPECT_EQ(words[0], PFM3_DIAG_FAULT_MAGIC);
    EXPECT_EQ(words[1], 4u);                    // faultId
    EXPECT_EQ(words[2], 0x00010000u);           // cfsr
    EXPECT_EQ(words[3], 0x40000000u);           // hfsr
    EXPECT_EQ(words[4], 0x00000008u);           // dfsr
    EXPECT_EQ(words[5], 0x2001ABCDu);           // mmfar
    EXPECT_EQ(words[6], 0x08021ABCu);           // bfar
    EXPECT_EQ(words[7], 0u);                    // afsr
    EXPECT_EQ(words[8], 0x11111110u);           // r0
    EXPECT_EQ(words[9], 0x22222221u);           // r1
    EXPECT_EQ(words[10], 0x33333332u);          // r2
    EXPECT_EQ(words[11], 0x44444443u);          // r3
    EXPECT_EQ(words[12], 0x55555554u);          // r12
    EXPECT_EQ(words[13], 0x08001234u);          // lr
    EXPECT_EQ(words[14], 0x61000000u);          // psr
    EXPECT_EQ(words[15], 0x08021234u);          // pc
    EXPECT_EQ(words[16], 0x00000007u);          // sysTickCtrl
    EXPECT_EQ(words[17], 0x00ABCDEFu);          // sysTickAliveCpt
    EXPECT_EQ(words[18], 1u);                   // mainLoopSeenAlive

    // P18: the same reconstructed byte stream matches the INDEPENDENT
    // hand-written golden fixture (documented field order, not struct bytes).
    // Rebuild the stream via the mirror's byte pairs.
    uint8_t stream[84];
    int n = 0;
    for (int f = 0; f < kReplayDataFrames; f++) {
        const uint8_t *vals = dec.frames[f] + 2;
        for (int j = 0; j + 1 < 14; j += 2) {
            stream[n++] = (uint8_t)((vals[j] << 1) | vals[j + 1]);
        }
    }
    EXPECT_EQ(n, kReplayDataFrames * 7);
    EXPECT_EQ(memcmp(stream, kGoldenUsageFaultFrame, sizeof kGoldenUsageFaultFrame), 0)
        << "replay bytes diverge from the documented-layout golden fixture";
}

// --- (c) command codes + CC/SysEx hook matching -------------------------------

TEST(Pfm3DiagCommand, CodesSetDocumentedState) {
    pfm3DiagTestReset();
    EXPECT_EQ(pfm3DiagEnabled, 1);      // documented defaults
    EXPECT_EQ(pfm3DiagDeferSeqTft, 0);
    EXPECT_EQ(pfm3DiagReportRequest, 0);

    pfm3DiagCommand(1);
    EXPECT_EQ(pfm3DiagReportRequest, 1);

    pfm3DiagCommand(2);
    EXPECT_EQ(pfm3DiagDeferSeqTft, 1);
    EXPECT_EQ(pfm3DiagSeqTftDeferredCpt, 0u);  // deferral counted at the gates
    EXPECT_EQ(pfm3DiagSeqTftGate(), 1);        // bisect arm engaged
    EXPECT_EQ(pfm3DiagSeqTftDeferredCpt, 1u);

    pfm3DiagCommand(3);
    EXPECT_EQ(pfm3DiagDeferSeqTft, 0);
    EXPECT_EQ(pfm3DiagSeqTftGate(), 0);
    EXPECT_EQ(pfm3DiagSeqTftDeferredCpt, 1u);  // no new deferral

    pfm3DiagCommand(5);
    EXPECT_EQ(pfm3DiagEnabled, 0);
    pfm3DiagCommand(4);
    EXPECT_EQ(pfm3DiagEnabled, 1);

    // Code 6 (forced alignment trap): compiled out on host — a no-op that
    // leaves every state word untouched.
    pfm3DiagTestReset();
    pfm3DiagCommand(6);
    EXPECT_EQ(pfm3DiagReportRequest, 0);
    EXPECT_EQ(pfm3DiagDeferSeqTft, 0);
    EXPECT_EQ(pfm3DiagEnabled, 1);

    // Unknown codes: ignored.
    pfm3DiagCommand(0);
    pfm3DiagCommand(7);
    pfm3DiagCommand(255);
    EXPECT_EQ(pfm3DiagReportRequest, 0);
    EXPECT_EQ(pfm3DiagDeferSeqTft, 0);
    EXPECT_EQ(pfm3DiagEnabled, 1);
}

TEST(Pfm3DiagCommand, CcHookMatchesOnlyCh16Cc119Codes1To6) {
    pfm3DiagTestReset();

    // Channel 16 (0-based 15) — the ONLY accepted channel (P15a).
    EXPECT_EQ(pfm3DiagCcHook(15, 119, 1), 1);   // valid: dispatch + consume
    EXPECT_EQ(pfm3DiagReportRequest, 1);
    EXPECT_EQ(pfm3DiagCcHook(15, 119, 6), 1);   // forced-trap code accepted

    EXPECT_EQ(pfm3DiagCcHook(15, 119, 0), 0);   // value range 1..6 only
    EXPECT_EQ(pfm3DiagCcHook(15, 119, 7), 0);
    EXPECT_EQ(pfm3DiagCcHook(15, 118, 1), 0);   // CC number must match
    EXPECT_EQ(pfm3DiagCcHook(15, 120, 5), 0);

    // ANY other channel: never a diag command — a legit CC#119 value 6 on a
    // user channel must NOT force-reset the unit (the P15a defect).
    pfm3DiagReportRequest = 0;
    EXPECT_EQ(pfm3DiagCcHook(0, 119, 1), 0);
    EXPECT_EQ(pfm3DiagCcHook(1, 119, 6), 0);
    EXPECT_EQ(pfm3DiagCcHook(14, 119, 5), 0);
    EXPECT_EQ(pfm3DiagReportRequest, 0);
    // Other channels + other CCs: nothing dispatched either.
    EXPECT_EQ(pfm3DiagCcHook(0, 118, 1), 0);
    EXPECT_EQ(pfm3DiagReportRequest, 0);
}

// P15(b): code 6 is ignored while a valid capture is pending replay — the
// pending capture must survive (a reboot would overwrite it / loop the unit).
TEST(Pfm3DiagCommand, Code6IgnoredWhileCapturePending) {
    pfm3DiagTestReset();
    plantCapture(4, 0x00010000u);
    pfm3DiagCommand(6);
    EXPECT_EQ(pfm3DiagFault.magic, PFM3_DIAG_FAULT_MAGIC)
        << "code 6 must not clear/replace a pending capture";

    // And the pending capture still replays afterwards (nothing consumed it).
    for (uint32_t t = 0; pfm3DiagTestReplayFrameCount() < kReplayTotalFrames; t += 6) {
        pfm3DiagTestReplayPoll(t < 3001 ? 3001 : t);
    }
    EXPECT_EQ(pfm3DiagTestReplayFrameCount(), kReplayTotalFrames);
}

TEST(Pfm3DiagCommand, SysexHookMatchesMagicP3D) {
    pfm3DiagTestReset();
    // analyseSysexBuffer delivers the payload WITHOUT F0/F7: the documented
    // wire form F0 7D 'P' '3' 'D' <cmd> <arg> F7 arrives as 6 bytes (P15c).
    uint8_t buf[6] = {0x7d, 'P', '3', 'D', 'R', 0};

    EXPECT_EQ(pfm3DiagSysexHook(buf, 6), 1);
    EXPECT_EQ(pfm3DiagReportRequest, 1);

    buf[4] = 'D'; buf[5] = 1;
    EXPECT_EQ(pfm3DiagSysexHook(buf, 6), 1);
    EXPECT_EQ(pfm3DiagDeferSeqTft, 1);
    buf[5] = 0;
    EXPECT_EQ(pfm3DiagSysexHook(buf, 6), 1);
    EXPECT_EQ(pfm3DiagDeferSeqTft, 0);

    buf[4] = 'W'; buf[5] = 0;
    EXPECT_EQ(pfm3DiagSysexHook(buf, 6), 1);
    EXPECT_EQ(pfm3DiagEnabled, 0);
    buf[5] = 1;
    EXPECT_EQ(pfm3DiagSysexHook(buf, 6), 1);
    EXPECT_EQ(pfm3DiagEnabled, 1);

    // The full 8-byte wire form (F0/F7 included) is accepted too.
    uint8_t wire[8] = {0xF0, 0x7d, 'P', '3', 'D', 'R', 1, 0xF7};
    pfm3DiagReportRequest = 0;
    EXPECT_EQ(pfm3DiagSysexHook(wire, 8), 1);
    EXPECT_EQ(pfm3DiagReportRequest, 1);

    buf[4] = 'X';  // unknown cmd byte
    EXPECT_EQ(pfm3DiagSysexHook(buf, 6), 0);
    EXPECT_EQ(pfm3DiagSysexHook(buf, 5), 0);   // size must be 6 (or 8 framed)
    {   // the old (buggy) matcher's size — 7 bytes, must also be rejected;
        // a properly sized 7-byte buffer so the hook's F7 peek stays in-bounds.
        uint8_t seven[7] = {0x7d, 'P', '3', 'D', 'R', 0, 0x00};
        EXPECT_EQ(pfm3DiagSysexHook(seven, 7), 0);
    }
    buf[0] = 0x7e;                              // wrong manufacturer byte
    EXPECT_EQ(pfm3DiagSysexHook(buf, 6), 0);
}

// --- (d) tic-section accumulators ---------------------------------------------

TEST(Pfm3DiagTicSections, AccumulatesTotalsAndMaxes) {
    pfm3DiagTestReset();
    Pfm3DiagTestSecStatsMirror st;
    pfm3DiagTestSecStats(&st);
    EXPECT_EQ(st.count, 0u);
    EXPECT_EQ(st.encTotal, 0u);

    pfm3DiagTicSections(100, 200, 300, 700);
    pfm3DiagTicSections(50, 250, 10, 400);
    pfm3DiagTestSecStats(&st);
    EXPECT_EQ(st.count, 2u);
    EXPECT_EQ(st.encTotal, 150u);
    EXPECT_EQ(st.seqTotal, 450u);
    EXPECT_EQ(st.tftTotal, 310u);
    EXPECT_EQ(st.totalTotal, 1100u);
    EXPECT_EQ(st.encMax, 100u);
    EXPECT_EQ(st.seqMax, 250u);
    EXPECT_EQ(st.tftMax, 300u);
}
