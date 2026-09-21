/*
 * pfm3_diag.h — Anomaly 8.1 instrumentation (2026-08-26 session), restored
 * 2026-09-22 for phase 8.8 SW5 behind the PFM3_DIAG_ENABLED gate.
 *
 * Evidence so far (see _bmad-output/planning-artifacts/hardware-test-plan-v1.11.md
 * §0): two hangs on eecb8a0 where the SysTick layer (preenfm3Tic: encoder scan,
 * tft.tic, sequencer.ticMillis) went inert while the audio IRQ (USB-MIDI decode
 * + voices) and the main loop (async actions, NRPN echo) stayed alive. A frozen
 * SysTick_Handler would have pinned the CPU and killed those too, so the handler
 * itself still returns — the death is either (a) the SysTick IRQ no longer
 * firing, or (b) preenfm3Tic early-returning (readyForTFT flipped false by a
 * wild write is the canonical candidate), or (c) midiControllerMode corrupted
 * to non-zero.
 *
 * This module adds, with zero behavior change when idle:
 *   - a SysTick-alive counter incremented at the very top of SysTick_Handler;
 *   - a main-loop watchdog (DWT cycle counter, immune to a frozen HAL tick)
 *     that detects a stalled counter and reports a register snapshot over
 *     USB MIDI as CC 48..63 on channel 16, repeated every ~2 s;
 *   - a host-controlled bisect arm: magic SysEx F0 7D 'P' '3' 'D' cmd arg F7
 *     defers the decode-context TFT calls (the 8.1 prime suspect) so one DFU
 *     can serve both arms of the A/B repro;
 *   - fault handlers (Hard/MemManage/Bus/Usage/NMI) that capture the stacked
 *     frame + fault-status registers into a .noinit struct, delay ~50 ms and
 *     soft-reset; on boot the capture is replayed over USB MIDI (consume-once).
 *
 * GATING (8.8 SW5): the whole module is diagnostic-only. The real
 * declarations below are visible ONLY when PFM3_DIAG_ENABLED (firmware diag
 * build, set by the `make ubsan-trap` variant via firmware/CMakeLists.txt) or
 * PFM3_HOST (host test seam, set by tests/CMakeLists.txt) is defined. In
 * every other build (release, debug) this header resolves every hook to a
 * `static inline` no-op so callers compile to byte-identical code — no
 * counters, no .noinit, no extern symbol references. stm32h7xx_it.c gates
 * its shims itself (#ifdef PFM3_DIAG_ENABLED) because it is a C TU.
 *
 * This header is C++-only (included by pfm3_diag.cpp, preenfm3.cpp,
 * Sequencer.cpp, FMDisplaySequencer.cpp, FMDisplay3.cpp, MidiDecoder.cpp).
 * The two entry points called from C TUs (stm32h7xx_it.c) —
 * pfm3DiagSysTickEnter() and pfm3DiagFaultHook() — are declared there with
 * plain-C prototypes (same idiom as `void preenfm3Tic();` in that file) and
 * defined with C linkage in pfm3_diag.cpp.
 */

#ifndef PFM3_DIAG_H
#define PFM3_DIAG_H

#include <stdint.h>

#if defined(PFM3_DIAG_ENABLED) || defined(PFM3_HOST)

/* --- liveness counters (written from IRQ / SysTick, read by watchdog) ------ */

/* ++ at the very top of SysTick_Handler, before HAL_IncTick / preenfm3Tic. */
extern volatile uint32_t pfm3DiagSysTickAliveCpt;

/* ++ in the SAI audio callbacks (HAL_SAI_Tx[C|HalfC]pltCallback). */
extern volatile uint32_t pfm3DiagAudioCpt;

/* ++ each time a suspect decode-context TFT call site is entered. */
extern volatile uint32_t pfm3DiagSeqTftCpt;

/* ++ when such a call was actually deferred (bisect arm engaged). */
extern volatile uint32_t pfm3DiagSeqTftDeferredCpt;

/* --- host-controlled state (set via magic SysEx / CC, read everywhere) ----- */

/* 1 = defer the suspect decode-context TFT calls (8.1 bisect arm). Default 0. */
extern volatile uint8_t pfm3DiagDeferSeqTft;

/* 1 = watchdog + reporting active. Default 1. */
extern volatile uint8_t pfm3DiagEnabled;

/* Main loop drains these (never TX from decode context):
 *   pfm3DiagReportRequest — send one register snapshot immediately;
 *   pfm3DiagAckPending    — last accepted diag SysEx cmd, sent back as CC#63
 *                           on ch16 so the rig can confirm RX. */
extern volatile uint8_t pfm3DiagReportRequest;
extern volatile uint8_t pfm3DiagAckPending;

/* --- fault capture (target only; filled by pfm3DiagFaultHook) ------------- */

#define PFM3_DIAG_FAULT_MAGIC 0x8C1FA01u

/* Host command CC (see pfm3DiagCommand / pfm3DiagCcHook). */
#define PFM3_DIAG_CC 119

typedef struct {
    uint32_t magic;      /* PFM3_DIAG_FAULT_MAGIC when the capture is valid   */
    uint32_t faultId;    /* 1=Hard 2=MemManage 3=Bus 4=Usage 5=NMI 6=STALL    */
    uint32_t cfsr, hfsr, dfsr, mmfar, bfar, afsr;
    uint32_t r0, r1, r2, r3, r12, lr, psr, pc;   /* stacked (basic) frame     */
    uint32_t sysTickCtrl;        /* SysTick->CTRL sampled in the handler      */
    uint32_t sysTickAliveCpt;    /* alive counter sampled in the handler      */
    uint32_t mainLoopSeenAlive;  /* 1 = a watchdog cycle completed pre-fault  */
} Pfm3DiagFaultInfo;

/* Lives in .noinit (survives a soft reset for SWD post-mortem; a power cycle
 * naturally clears it — magic gates validity). */
extern Pfm3DiagFaultInfo pfm3DiagFault;

/* Called (as extern "C") from the naked shims in stm32h7xx_it.c with the
 * stacked frame. Never returns: fills pfm3DiagFault, delays ~50 ms (let any
 * in-flight USB TX drain), then NVIC_SystemReset(); boot replays the capture
 * over USB MIDI (see pfm3DiagWatchdogMainLoop). */
extern "C" void pfm3DiagFaultHook(uint32_t faultId, uint32_t *stackedFrame);

/* ++ at the very top of SysTick_Handler (called from stm32h7xx_it.c, plain-C
 * prototype there). Also drives the tri-state LED triage blink when the
 * preenfm3Tic path is being skipped. */
extern "C" void pfm3DiagSysTickEnter();

/* Watchdog service, called from preenfm3Loop() every iteration (C++ side).
 * DWT-timed (immune to a frozen HAL tick): on a SysTick stall it snapshots
 * SysTick/SCB/interrupt-state registers and sends them over USB MIDI via
 * midiDecoder.writeMidiCCOut — CC#48..63, channel 16, repeated every 2 s.
 * Also drains pfm3DiagReportRequest / pfm3DiagAckPending and streams any
 * boot-time crash-capture replay (consume-once). */
void pfm3DiagWatchdogMainLoop();

/* Enable the DWT cycle counter + Usage/MemManage/Bus faults (call once at
 * init, before the main loop). The SHCSR fault enables make a trap-mode UDF
 * land in UsageFault (faultId 4, UFSR bits in CFSR) instead of escalating
 * silently to HardFault. */
void pfm3DiagInit();

/* 8.1 famine instrumentation: preenfm3Tic section timings. Call with the
 * DWT cycle deltas of the three sections (encoders.checkStatus /
 * sequencer.ticMillis / tft.tic) plus the WHOLE-tic delta (entry→exit).
 * The whole-vs-sections difference is preemption (audio or any IRQ landing
 * inside the measured window) — the discriminator between "time inside
 * tft.tic" and "IRQ storm at priority above SysTick". Accumulates
 * count/total/max per section; report exposes averages in 1 ms units. */
void pfm3DiagTicSections(uint32_t encCycles, uint32_t seqCycles, uint32_t tftCycles, uint32_t totalCycles);

/* Host command entry (idempotent, IRQ-context-safe: sets state only, the
 * main loop does the TX). Codes (CC#119 on ANY channel, or the magic SysEx):
 *   1 = send one report now      2 = defer decode-TFT ON   3 = defer OFF
 *   4 = watchdog ON              5 = watchdog OFF
 *   6 = forced alignment trap (8.8 SW5 device gate): a volatile uint32 load
 *       from a deliberately misaligned address. In the ubsan-trap build the
 *       alignment sanitizer turns it into a UDF -> UsageFault (faultId 4,
 *       UFSR UNDEFINSTR in CFSR) -> capture -> self-reset -> boot replay.
 *       Inert in a non-sanitized diag build (ARMv7-M tolerates the unaligned
 *       word load) and compiled out on host (no-op). */
void pfm3DiagCommand(uint8_t code);

/* Foldable MidiDecoder hooks (8.8 SW5). The parked build inlined the CC/SysEx
 * matching at the decode sites; the gated restore moves the matching INTO the
 * module so the disabled no-op path (below) folds the whole hook site away —
 * an inline-constant `return 0` leaves zero code AND zero behavior (a no-op
 * pfm3DiagCommand alone would still swallow the CC#119 event at the site).
 *
 *   pfm3DiagCcHook(cc, value): 1 when (cc == PFM3_DIAG_CC && 1 <= value <= 6)
 *       after dispatching pfm3DiagCommand(value) — caller consumes the event.
 *   pfm3DiagSysexHook(buf, size): 1 when the 7-byte magic SysEx
 *       F0 7D 'P' '3' 'D' <cmd> <arg> F7 was recognized and dispatched
 *       (D=defer arm, W=watchdog, R=report) — caller consumes the buffer. */
int pfm3DiagCcHook(uint8_t cc, uint8_t value);
int pfm3DiagSysexHook(const uint8_t *sysexBuffer, uint16_t size);

/* Audio-IRQ-context SysTick watchdog (target only). Called from both SAI
 * callbacks. If the SysTick alive counter has not moved across five
 * consecutive ~0.2 s sample windows (~1 s) while the audio IRQ keeps
 * running, captures a STALL snapshot (faultId 6), delays ~50 ms and
 * soft-resets — the "main loop AND SysTick dead, audio alive" signature of
 * the H1-type hang. Only touches DWT/SCB/GPIO — safe from IRQ context, no
 * queue or USB access. */
extern "C" void pfm3DiagAudioWatchdog();

/* Call at the END of each SAI callback (entry timestamp taken by
 * pfm3DiagAudioWatchdog): accumulates exact audio-callback self time for
 * the duty-cycle report. */
extern "C" void pfm3DiagAudioExit();

/* Inline guard used at the suspect call sites. Counts every entry, and tells
 * the caller whether to defer (bisect arm) or run the original code. */
static inline int pfm3DiagSeqTftGate() {
    pfm3DiagSeqTftCpt++;
    if (pfm3DiagDeferSeqTft) {
        pfm3DiagSeqTftDeferredCpt++;
        return 1;
    }
    return 0;
}

#else  /* !(PFM3_DIAG_ENABLED || PFM3_HOST) — disabled: zero-code no-op path */

/* 8.8 SW5 gating: every hook called from another TU resolves to an empty
 * inline body, so call sites fold away entirely (release/debug builds stay
 * byte-identical to the un-instrumented tree). No extern variable — the
 * counters, pfm3DiagFault and the .noinit section — is referenced here.
 * stm32h7xx_it.c gates its own shims with #ifdef PFM3_DIAG_ENABLED (C TU). */

static inline void pfm3DiagInit() {}
static inline void pfm3DiagWatchdogMainLoop() {}
static inline void pfm3DiagTicSections(uint32_t encCycles, uint32_t seqCycles, uint32_t tftCycles, uint32_t totalCycles) {
    (void)encCycles; (void)seqCycles; (void)tftCycles; (void)totalCycles;
}
static inline void pfm3DiagCommand(uint8_t code) { (void)code; }

/* Foldable decode-path hooks: constant 0 = never a diag event, so the
 * MidiDecoder sites (`if (pfm3DiagCcHook(...)) return;`) compile to nothing
 * and events are never swallowed in a non-diagnostic build. */
static inline int pfm3DiagCcHook(uint8_t cc, uint8_t value) { (void)cc; (void)value; return 0; }
static inline int pfm3DiagSysexHook(const uint8_t *sysexBuffer, uint16_t size) {
    (void)sysexBuffer; (void)size;
    return 0;
}

/* Audio-IRQ no-ops (called from preenfm3.cpp SAI callbacks). */
static inline void pfm3DiagAudioWatchdog() {}
static inline void pfm3DiagAudioExit() {}

/* Gate guard: constant 0 = run the original code, always. */
static inline int pfm3DiagSeqTftGate() { return 0; }

#endif /* PFM3_DIAG_ENABLED || PFM3_HOST */

#endif /* PFM3_DIAG_H */
