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

    // Trailer: magic, 0xFF marker, faultId.
    uint8_t trailer[16];
    pfm3DiagTestReplayFrame(kReplayDataFrames, trailer);
    EXPECT_EQ(trailer[0], 0xC7);
    EXPECT_EQ(trailer[1], 0xFF);
    EXPECT_EQ(trailer[2], (uint8_t)planted.faultId);

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

TEST(Pfm3DiagCommand, CcHookMatchesOnlyCc119Codes1To6) {
    pfm3DiagTestReset();

    EXPECT_EQ(pfm3DiagCcHook(119, 1), 1);   // valid: dispatch + consume
    EXPECT_EQ(pfm3DiagReportRequest, 1);
    EXPECT_EQ(pfm3DiagCcHook(119, 6), 1);   // forced-trap code accepted

    EXPECT_EQ(pfm3DiagCcHook(119, 0), 0);   // value range 1..6 only
    EXPECT_EQ(pfm3DiagCcHook(119, 7), 0);
    EXPECT_EQ(pfm3DiagCcHook(118, 1), 0);   // CC number must match
    EXPECT_EQ(pfm3DiagCcHook(120, 5), 0);

    // Non-matching hooks must not have dispatched anything new: only the
    // two accepted calls above set state; reset the flag they set first.
    pfm3DiagReportRequest = 0;
    EXPECT_EQ(pfm3DiagCcHook(118, 1), 0);
    EXPECT_EQ(pfm3DiagReportRequest, 0);
}

TEST(Pfm3DiagCommand, SysexHookMatchesMagicP3D) {
    pfm3DiagTestReset();
    // F0 7D 'P' '3' 'D' <cmd> <arg> F7 — the F0/F7 framing is stripped by
    // the time analyseSysexBuffer runs; the 7 payload bytes carry the magic.
    uint8_t buf[7] = {0x7d, 'P', '3', 'D', 'R', 0, 0x00};

    EXPECT_EQ(pfm3DiagSysexHook(buf, 7), 1);
    EXPECT_EQ(pfm3DiagReportRequest, 1);

    buf[4] = 'D'; buf[5] = 1;
    EXPECT_EQ(pfm3DiagSysexHook(buf, 7), 1);
    EXPECT_EQ(pfm3DiagDeferSeqTft, 1);
    buf[5] = 0;
    EXPECT_EQ(pfm3DiagSysexHook(buf, 7), 1);
    EXPECT_EQ(pfm3DiagDeferSeqTft, 0);

    buf[4] = 'W'; buf[5] = 0;
    EXPECT_EQ(pfm3DiagSysexHook(buf, 7), 1);
    EXPECT_EQ(pfm3DiagEnabled, 0);
    buf[5] = 1;
    EXPECT_EQ(pfm3DiagSysexHook(buf, 7), 1);
    EXPECT_EQ(pfm3DiagEnabled, 1);

    buf[4] = 'X';  // unknown cmd byte
    EXPECT_EQ(pfm3DiagSysexHook(buf, 7), 0);
    EXPECT_EQ(pfm3DiagSysexHook(buf, 6), 0);   // size must be 7
    buf[0] = 0x7e;                              // wrong manufacturer byte
    EXPECT_EQ(pfm3DiagSysexHook(buf, 7), 0);
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
