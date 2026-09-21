/*
 * pfm3_diag.cpp — Anomaly 8.1 instrumentation implementation, restored for
 * phase 8.8 SW5. See pfm3_diag.h for the design rationale and the gating
 * contract.
 *
 * Host build (PFM3_HOST): the register-touching parts (DWT, SysTick/SCB reads,
 * LED strobe, fault hook) compile to inert bodies so Sequencer.cpp and
 * MidiDecoder.cpp link unchanged; the counters exist so the decode-path gates
 * are exercised by the existing host tests. The replay engine (encoder +
 * phase machine) is shared target/host code so tests/pfm3_diag_test.cpp can
 * prove the encoding + consume-once contract without hardware.
 *
 * If neither PFM3_DIAG_ENABLED nor PFM3_HOST is defined this TU compiles to
 * NOTHING (empty translation unit). That is deliberate belt-and-braces next
 * to the firmware/CMakeLists.txt exclusion of this file from non-diagnostic
 * builds: even if a future GLOB change pulls it back in, an empty TU cannot
 * alter the release image.
 */

#include "pfm3_diag.h"

#if defined(PFM3_DIAG_ENABLED) || defined(PFM3_HOST)

#ifndef PFM3_HOST

#include "main.h"
#include "stm32h7xx_hal.h"
#include "preenfm3.h"
#include "MidiDecoder.h"

// Global MIDI decoder instance owned by preenfm3.cpp (report/replay TX path).
extern MidiDecoder midiDecoder;

// Globals owned by preenfm3.cpp / main.c that the snapshot reports.
extern "C" {
// volatile bool readyForTFT, uint32_t tftCpt/ledCpt live in preenfm3.cpp
// (already exported via extern "C" block there); declared here for our use.
extern volatile bool readyForTFT;
extern uint32_t tftCpt;
extern uint32_t ledCpt;
extern volatile uint8_t midiControllerMode;
}

#else  // PFM3_HOST

#include <stdbool.h>

static bool hostReadyForTft = false;
static uint32_t hostTftCpt = 0;
static uint32_t hostLedCpt = 0;
static volatile uint8_t hostMidiControllerMode = 0;

#endif // PFM3_HOST

// --- state -----------------------------------------------------------------

volatile uint32_t pfm3DiagSysTickAliveCpt = 0;
volatile uint32_t pfm3DiagAudioCpt = 0;
volatile uint32_t pfm3DiagSeqTftCpt = 0;
volatile uint32_t pfm3DiagSeqTftDeferredCpt = 0;

volatile uint8_t pfm3DiagDeferSeqTft = 0;
volatile uint8_t pfm3DiagEnabled = 1;

volatile uint8_t pfm3DiagReportRequest = 0;
volatile uint8_t pfm3DiagAckPending = 0;

// Host build has no .noinit section support (section attributes are a Mach-O
// hard error there — see tests/CMakeLists.txt); a plain static stands in.
#ifdef PFM3_HOST
Pfm3DiagFaultInfo pfm3DiagFault;
#else
Pfm3DiagFaultInfo pfm3DiagFault __attribute__((section(".noinit")));
#endif

// Set to 1 by every completed watchdog sample (read by the fault hook).
static volatile uint32_t diagWatchdogCycled = 0;

// --- Duty-cycle accumulator state (build #7; functions below) --------------

static uint32_t audioWinCycles = 0;    // audio-callback self time, window
static uint32_t systickWinCycles = 0;  // preenfm3Tic self time, window
static uint32_t dutyWindowStart = 0;   // DWT at window open
static uint32_t dutyCallbacks = 0;     // callbacks since window open
static uint32_t dutyTickCount = 0;     // SysTicks since window open
// Latched window results (for the report).
static volatile uint8_t latchSystickPct = 0, latchAudioPct = 0, latchResidualPct = 0;
static volatile uint16_t latchTicks = 0;
static volatile uint8_t latchDwtMhz8 = 0;   // windowCycles/2^22 (x4 = ~MHz/4)

// --- preenfm3Tic section timing (8.1 famine) --------------------------------

static uint32_t secCount = 0;
static uint32_t secEncTotal = 0, secSeqTotal = 0, secTftTotal = 0;
static uint32_t secEncMax = 0, secSeqMax = 0, secTftMax = 0;
static uint32_t secTotalTotal = 0;
static uint32_t tickCountWindow = 0;
static uint32_t tickWindowStartCycles = 0;
static uint32_t tickRateHz = 0;   // measured: ticks per DWT second

#ifndef PFM3_HOST

void pfm3DiagTicSections(uint32_t encCycles, uint32_t seqCycles, uint32_t tftCycles, uint32_t totalCycles) {
    systickWinCycles += totalCycles;
    secCount++;
    secEncTotal += encCycles; if (encCycles > secEncMax) secEncMax = encCycles;
    secSeqTotal += seqCycles; if (seqCycles > secSeqMax) secSeqMax = seqCycles;
    secTftTotal += tftCycles; if (tftCycles > secTftMax) secTftMax = tftCycles;
    secTotalTotal += totalCycles;

    // True tick rate over a ~1 s DWT window.
    tickCountWindow++;
    uint32_t now = DWT->CYCCNT;
    if (tickWindowStartCycles == 0) {
        tickWindowStartCycles = now ? now : 1;
    } else if ((uint32_t)(now - tickWindowStartCycles) >= SystemCoreClock) {
        uint32_t dr = now - tickWindowStartCycles;
        tickRateHz = (uint32_t)((uint64_t)tickCountWindow * SystemCoreClock / dr);
        tickCountWindow = 0;
        tickWindowStartCycles = now ? now : 1;
    }
}

#else  // PFM3_HOST

void pfm3DiagTicSections(uint32_t encCycles, uint32_t seqCycles, uint32_t tftCycles, uint32_t totalCycles) {
    // Host: counters only (no DWT — the systickWinCycles accumulation and
    // the ~1 s tick-rate window are target-only). Max tracking is pure
    // arithmetic, so the host body keeps it (8.8 SW5) and the test suite
    // covers the full accumulator contract.
    secCount++;
    secEncTotal += encCycles; if (encCycles > secEncMax) secEncMax = encCycles;
    secSeqTotal += seqCycles; if (seqCycles > secSeqMax) secSeqMax = seqCycles;
    secTftTotal += tftCycles; if (tftCycles > secTftMax) secTftMax = tftCycles;
    secTotalTotal += totalCycles;
}

#endif // PFM3_HOST

// --- Duty-cycle accumulators (build #7) --------------------------------------
// NO calibration pass: windows are driven by AUDIO-CALLBACK COUNT (sample-
// clock law, the one clock that stayed perfect through the whole famine), so
// window length is time-exact and every percentage is a dimensionless DWT
// ratio (audio self cycles / window cycles) — the unknown DWT rate cancels.
// Build #6 lesson: min-inter-tick "calibration" caught back-to-back burst
// ticks (4 us) under famine and scaled every number by ~4x; and 10 s DWT
// windows can overflow 32 bits at 480 MHz. 1378 callbacks ~= 1.0007 s
// (2 callbacks per 64-sample block at 44100 Hz).

#define DUTY_CALLBACKS_PER_WINDOW 1378u

#ifndef PFM3_HOST

// Called from both SAI callbacks (pfm3DiagAudioWatchdog entry).
static void pfm3DiagDutyWindow(uint32_t now) {
    if (dutyWindowStart == 0) {
        dutyWindowStart = now ? now : 1;
        dutyCallbacks = 0;
        return;
    }
    dutyCallbacks++;
    if (dutyCallbacks < DUTY_CALLBACKS_PER_WINDOW) {
        return;
    }
    uint32_t w = now - dutyWindowStart;   // 32-bit-safe: ~1 s window
    if (w < 1000u) {
        w = 1000u;   // degenerate guard
    }
    uint32_t a = (audioWinCycles * 100u + w / 2) / w;
    uint32_t s = (systickWinCycles * 100u + w / 2) / w;
    latchAudioPct = a > 127 ? 127 : (uint8_t)a;
    latchSystickPct = s > 127 ? 127 : (uint8_t)s;
    uint32_t resid = 100u - (a > 100 ? 100 : a) - (s > 100 ? 100 : s);
    latchResidualPct = resid > 100 ? 0 : (uint8_t)resid;
    latchTicks = (uint16_t)dutyTickCount;
    latchDwtMhz8 = (uint8_t)((w >> 22) & 0x7F);
    audioWinCycles = 0;
    systickWinCycles = 0;
    dutyTickCount = 0;
    dutyCallbacks = 0;
    dutyWindowStart = now ? now : 1;
}

// --- SysTick entry (called first thing from SysTick_Handler) ----------------

extern "C" void pfm3DiagSysTickEnter() {
    pfm3DiagSysTickAliveCpt++;
    dutyTickCount++;

    // Tri-state LED encoding: preenfm3Tic drives the normal ~0.4 Hz blink,
    // but only once readyForTFT is set and midiControllerMode == 0. If the
    // preenfm3Tic path is being skipped, blink fast here so a frozen panel
    // still tells us WHY it is frozen (see header triage table).
    if (midiControllerMode == 0 && !readyForTFT) {
        static uint32_t blink = 0;
        if (++blink >= 62) {  // ~8 Hz at 1 kHz
            blink = 0;
            if (GPIOE->ODR & LED_TEST_Pin) {
                PFM_CLEAR_PIN(GPIOE, LED_TEST_Pin);
            } else {
                PFM_SET_PIN(GPIOE, LED_TEST_Pin);
            }
        }
    }
}

// --- audio-IRQ-context SysTick watchdog + VECTACTIVE histogram -------------
// Runs from both SAI callbacks (the only context guaranteed alive in the
// H1-type hang: main loop AND SysTick dead, audio alive). DWT-timed sample
// window = SystemCoreClock/5 (~0.2 s); five consecutive windows without
// SysTickAlive movement (>=1 s) while audio keeps running => capture a
// faultId 6 STALL snapshot and soft-reset. GPIO/register only — no queue,
// no USB, safe from IRQ context.
//
// HISTOGRAM (8.1 famine, build #5): each audio callback samples SCB->ICSR
// VECTACTIVE (0=thread, 15=SysTick, 27=SAI1_A/self, else=other exception).
// Over a 10 s window (~1875 samples) it latches thread%/systick%/self%/
// other% + top-other id/%. Audio (pri 3) and SPI1-TX DMA (pri 2) outrank
// SysTick (TICK_INT_PRIORITY 4) — an eater there shows as other%/topOther.

static uint32_t vaThread = 0, vaSysTick = 0, vaSelf = 0, vaOther = 0;
static uint32_t vaTopOtherId = 0, vaTopOtherCount = 0;
static uint32_t vaWindowCount = 0;
static uint32_t vaWindowStartCycles = 0;
// Latched report values (7-bit percentages).
static volatile uint8_t vaLatchThread = 0, vaLatchSysTick = 0, vaLatchSelf = 0, vaLatchOther = 0;
static volatile uint8_t vaLatchTopId = 0, vaLatchTopPct = 0;

#define VA_WINDOW_CYCLES (SystemCoreClock * 10u)   // 10 s
#define VA_SYSTICK_VECT 15u
#define VA_SELF_VECT 27u                            // 16 + DMA1_Stream0_IRQn(11)

static void pfm3DiagVaSample() {
    uint32_t va = SCB->ICSR & 0x1FF;
    vaWindowCount++;
    if (va == 0) {
        vaThread++;
    } else if (va == VA_SYSTICK_VECT) {
        vaSysTick++;
    } else if (va == VA_SELF_VECT) {
        vaSelf++;
    } else {
        vaOther++;
        // cheap top-tracker: exact only when few distinct ids (true here).
        if (vaTopOtherId == 0) {
            vaTopOtherId = va;
            vaTopOtherCount = 1;
        } else if (va == vaTopOtherId) {
            vaTopOtherCount++;
        }
    }
    uint32_t now = DWT->CYCCNT;
    if (vaWindowStartCycles == 0) {
        vaWindowStartCycles = now ? now : 1;
    } else if ((uint32_t)(now - vaWindowStartCycles) >= (uint32_t)VA_WINDOW_CYCLES) {
        uint32_t n = vaWindowCount ? vaWindowCount : 1;
        vaLatchThread = (uint8_t)((vaThread * 100u + n / 2) / n);
        vaLatchSysTick = (uint8_t)((vaSysTick * 100u + n / 2) / n);
        vaLatchSelf = (uint8_t)((vaSelf * 100u + n / 2) / n);
        vaLatchOther = (uint8_t)((vaOther * 100u + n / 2) / n);
        vaLatchTopId = (uint8_t)(vaTopOtherId & 0xFF);
        vaLatchTopPct = (uint8_t)((vaTopOtherCount * 100u + n / 2) / n);
        vaThread = vaSysTick = vaSelf = vaOther = 0;
        vaTopOtherId = 0;
        vaTopOtherCount = 0;
        vaWindowCount = 0;
        vaWindowStartCycles = now ? now : 1;
    }
}

static uint32_t audioEntryCycles = 0;

extern "C" void pfm3DiagAudioWatchdog() {
    pfm3DiagAudioCpt++;
    audioEntryCycles = DWT->CYCCNT;
    pfm3DiagVaSample();
    pfm3DiagDutyWindow(DWT->CYCCNT);

    static uint32_t lastSampleCycles = 0;
    static uint32_t aliveAtLastSample = 0;
    static uint8_t stalledWindows = 0;

    uint32_t now = DWT->CYCCNT;
    if ((uint32_t)(now - lastSampleCycles) < (uint32_t)(SystemCoreClock / 5)) {
        return;
    }
    lastSampleCycles = now;

    if (pfm3DiagSysTickAliveCpt != aliveAtLastSample) {
        aliveAtLastSample = pfm3DiagSysTickAliveCpt;
        stalledWindows = 0;
        return;
    }

    // A full window with zero SysTick movement.
    if (stalledWindows < 5) {
        stalledWindows++;
        return;
    }
    // 5 consecutive ~0.2 s windows (1 s) with the SysTick alive counter
    // frozen while audio keeps running: SysTick AND main loop dead.
    // (Famine states always show sub-second burst movement; 1 s of total
    // silence is a true stall.) Capture a stall snapshot and soft-reset:
    // boot replays it over USB MIDI (.noinit survives NVIC reset).
    pfm3DiagFault.magic = PFM3_DIAG_FAULT_MAGIC;
    pfm3DiagFault.faultId = 6;   // STALL (1..5 = real faults)
    pfm3DiagFault.cfsr = SCB->CFSR;
    pfm3DiagFault.hfsr = SCB->HFSR;
    pfm3DiagFault.bfar = SCB->BFAR;
    pfm3DiagFault.pc = 0;
    pfm3DiagFault.lr = 0;
    pfm3DiagFault.psr = 0;
    pfm3DiagFault.sysTickCtrl = SysTick->CTRL;
    pfm3DiagFault.sysTickAliveCpt = pfm3DiagSysTickAliveCpt;
    pfm3DiagFault.mainLoopSeenAlive = diagWatchdogCycled;
    pfm3DiagFault.r0 = NVIC->ISPR[0];
    pfm3DiagFault.r1 = NVIC->ISPR[1];
    pfm3DiagFault.r2 = SCB->ICSR;
    pfm3DiagFault.r3 = SCB->SHCSR;
    for (volatile uint32_t d = 0; d < 24000000; d++) {
    }   // ~50 ms: let any in-flight USB TX drain
    NVIC_SystemReset();
}

// Call at the END of each SAI callback (both half and full).
extern "C" void pfm3DiagAudioExit() {
    audioWinCycles += DWT->CYCCNT - audioEntryCycles;
}

#else  // PFM3_HOST

extern "C" void pfm3DiagSysTickEnter() {
    pfm3DiagSysTickAliveCpt++;
    hostTftCpt++;  // keep the host symbols referenced (parity with target)
    (void)hostReadyForTft;
    (void)hostMidiControllerMode;
    (void)hostLedCpt;
}

extern "C" void pfm3DiagAudioWatchdog() {
    // Host build: counters only, no GPIO.
    pfm3DiagAudioCpt++;
}

extern "C" void pfm3DiagAudioExit() {
    // Host build: inert.
}

#endif // PFM3_HOST

// --- host command entry (idempotent; called from decode context) ---------

void pfm3DiagCommand(uint8_t code) {
    switch (code) {
    case 1:
        pfm3DiagReportRequest = 1;
        break;
    case 2:
        pfm3DiagDeferSeqTft = 1;
        break;
    case 3:
        pfm3DiagDeferSeqTft = 0;
        break;
    case 4:
        pfm3DiagEnabled = 1;
        break;
    case 5:
        pfm3DiagEnabled = 0;
        break;
    case 6:
#ifndef PFM3_HOST
        // 8.8 SW5 device gate: forced alignment trap. A volatile uint32 load
        // from a deliberately misaligned DTCMRAM address. In the ubsan-trap
        // build the alignment sanitizer turns the check into a UDF ->
        // UsageFault (faultId 4, UFSR UNDEFINSTR in CFSR) -> capture ->
        // self-reset -> boot replay. The address lives in a volatile
        // uintptr_t so the optimizer cannot constant-fold the load away.
        // Inert in a non-sanitized diag build (ARMv7-M tolerates the
        // unaligned word load from normal memory); compiled out on host.
        {
            volatile uintptr_t badAddr = 0x20000001u;
            volatile uint32_t *mis = (volatile uint32_t *)badAddr;
            (void)*mis;
        }
#endif
        break;
    default:
        break;
    }
}

// --- foldable decode-path hooks (called from MidiDecoder.cpp) --------------
// Matching lives here (not at the call sites) so the header's disabled path
// can fold the sites away entirely — see pfm3_diag.h.

int pfm3DiagCcHook(uint8_t cc, uint8_t value) {
    // CC#119 on ANY channel, consumed before routing/timbre fan-out (works
    // whatever the unit's global/current/omni channel config is).
    if (cc == PFM3_DIAG_CC && value >= 1 && value <= 6) {
        pfm3DiagCommand(value);
        return 1;
    }
    return 0;
}

int pfm3DiagSysexHook(const uint8_t *sysexBuffer, uint16_t size) {
    // Magic SysEx F0 7D 'P' '3' 'D' <cmd> <arg> F7, accepted from ANY channel
    // (real-time framing, no timbre routing). Accepted from the decode
    // context but consumed by the main loop (report TX and acks never run in
    // IRQ context). Cmds: 'D'=defer bisect arm, 'W'=watchdog enable,
    // 'R'=request one report. Ack comes back as CC#47 on MIDI channel 16.
    if (size == 7 && sysexBuffer[0] == 0x7d && sysexBuffer[1] == 'P' && sysexBuffer[2] == '3'
            && sysexBuffer[3] == 'D') {
        // Secondary transport: CC#119 (see pfm3DiagCcHook) is the reliable
        // path — USB-MIDI sysex reassembly can drop the terminating F7
        // depending on host packet batching. Same command codes.
        switch (sysexBuffer[4]) {
        case 'D':
            pfm3DiagCommand(sysexBuffer[5] != 0 ? 2 : 3);
            return 1;
        case 'W':
            pfm3DiagCommand(sysexBuffer[5] != 0 ? 4 : 5);
            return 1;
        case 'R':
            pfm3DiagCommand(1);
            return 1;
        default:
            return 0;
        }
    }
    return 0;
}

// --- DWT init + watchdog ----------------------------------------------------

#ifndef PFM3_HOST

// Cycle counter at SystemCoreClock (480 MHz): 8 samples/s.
#define DIAG_SAMPLE_CYCLES (SystemCoreClock / 8)
// While stalled, re-report every 2 s.
#define DIAG_REPORT_CYCLES (SystemCoreClock * 2)

void pfm3DiagInit() {
    // Unlock CoreSight (H7 requires LAR unlock before writing DWT registers).
    (*(volatile uint32_t *)0xE0001FB0) = 0xC5ACCE55;  // DWT->LAR
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    // 8.8 SW5: enable Usage/MemManage/Bus faults. Stock firmware leaves them
    // disabled, so a trap-mode UDF (or any configurable fault) escalates to
    // HardFault and loses its UFSR/CFSR detail. With all three enabled, a
    // sanitizer UDF lands directly in UsageFault (faultId 4) with the UFSR
    // bits intact in CFSR.
    SCB->SHCSR |= SCB_SHCSR_USGFAULTENA_Msk | SCB_SHCSR_MEMFAULTENA_Msk | SCB_SHCSR_BUSFAULTENA_Msk;
}

// 7-bit-clamped section timing in 1 ms units (127 => >=127 ms).
static uint8_t diagClampMs(uint32_t cycles) {
    uint32_t v = cycles / (SystemCoreClock / 1000u);
    return v > 127 ? 127 : (uint8_t)v;
}

// Snapshot + report. CC#48..63 on channel 16 (see header for the payload map).
static void pfm3DiagSendReport() {
    struct MidiEvent ev;
    ev.eventType = MIDI_CONTROL_CHANGE;
    ev.channel = 15;
    ev.value[1] = 0;

    uint32_t n = secCount ? secCount : 1;
    uint8_t payload[16];
    payload[0] = 0x5A;                                              // magic
    payload[1] = (uint8_t)(pfm3DiagSysTickAliveCpt & 0x7F);
    payload[2] = (uint8_t)(SysTick->CTRL & 0xFF);
    payload[3] = (uint8_t)(latchTicks & 0x7F);                      // SysTicks in window, bits 0-6
    payload[4] = latchSystickPct;                                  // preenfm3Tic duty %
    payload[5] = latchResidualPct;                                 // 100 - audio - systick = thread + unmeasured
    payload[6] = latchAudioPct;                                    // audio-callback duty %
    payload[7] = latchDwtMhz8;                                     // windowCycles/2^22 (x4.19 = MHz)
    payload[8] = (uint8_t)(ledCpt & 0x7F);
    payload[9] = (uint8_t)((latchTicks >> 7) & 0x7F);              // SysTicks in window, bits 7-14
    payload[10] = readyForTFT ? 1 : 0;                            // preenfm3Tic gate
    payload[11] = vaLatchTopId;                                    // top other vector id (7-bit)
    payload[12] = (uint8_t)((__get_PRIMASK() & 0x1)
            | ((SCB->VTOR != 0x08000000u) ? 2 : 0)
            | (pfm3DiagDeferSeqTft ? 4 : 0)
            | (pfm3DiagEnabled ? 8 : 0));
    payload[13] = vaLatchTopPct;                                   // top other %
    payload[14] = (uint8_t)(pfm3DiagSeqTftDeferredCpt & 0x7F);
    payload[15] = (uint8_t)(pfm3DiagAudioCpt & 0x7F);

    (void)n;
    (void)diagClampMs;   // section averages dropped from the report in build #7

    for (int i = 0; i < 16; i++) {
        ev.value[0] = 48 + i;
        ev.value[1] = payload[i];
        midiDecoder.writeMidiCCOut(&ev);
        midiDecoder.sendMidiUsbOutIfBufferFull();
    }
    midiDecoder.sendMidiDin5Out();
    midiDecoder.sendMidiUsbOut();
}

#endif // !PFM3_HOST

// --- crash-capture replay (build #8) ----------------------------------------
// On boot, if .noinit holds a valid capture (fault hook or stall watchdog
// fired before the soft reset), stream it over USB MIDI once USB is up,
// then clear it. Encoding (FROZEN — scripts/hardware/h8_crashwatch.py decodes
// this; do not change): frames of CCs on ch16; CC#48 (slot 0) = 0xC7 replay
// magic, #49 (slot 1) = frame index (0xFF marks the trailer frame), #50..63
// carry payload pairs where each struct byte is two 7-bit values (b>>1, b&1).
// 7 bytes per frame; the struct is 19 uint32 = 76 bytes -> 11 data frames +
// trailer. Paced one frame per 5 ms (back-to-back USB transmits drop when the
// previous is in flight).
//
// Split (8.8 SW5) into a host-testable core + target-only TX: the encoder and
// the phase machine below are compiled for BOTH target and host so the host
// tests can prove the encoding + consume-once contract; pfm3DiagReplayEmit
// swaps USB MIDI TX for a recording sink under PFM3_HOST.

#define PFM3_DIAG_REPLAY_MAGIC 0xC7u
#define PFM3_DIAG_REPLAY_SLOTS 16   // CC#48..63
#define PFM3_DIAG_REPLAY_FRAMES_MAX 13   // 11 data frames + trailer + guard

static uint8_t replayPhase = 0;   // 0=unchecked 1=armed 2=streaming 3=done
static uint8_t replayFrame = 0;
static uint32_t replayLastFrameMs = 0;

#ifdef PFM3_HOST
// Recording sink: the last emitted burst, for tests/pfm3_diag_test.cpp.
static uint8_t replaySinkFrames[PFM3_DIAG_REPLAY_FRAMES_MAX][PFM3_DIAG_REPLAY_SLOTS];
static int replaySinkCount = 0;
#endif

// Encodes replay frame `frameIdx` of pfm3DiagFault into cc[0..n-1] (the CC
// values for CC#48+slot). frameIdx == frame count emits the trailer. Pure
// function of the struct + index: no hardware, no TX — host-testable.
static int pfm3DiagReplayEncodeFrame(int frameIdx, uint8_t cc[PFM3_DIAG_REPLAY_SLOTS]) {
    const uint8_t *raw = (const uint8_t *) &pfm3DiagFault;
    const int total = (int) sizeof(Pfm3DiagFaultInfo);
    const int frames = (total + 6) / 7;   // 7 bytes per frame

    int slot = 0;
    if (frameIdx >= frames) {
        // trailer
        cc[slot++] = PFM3_DIAG_REPLAY_MAGIC;
        cc[slot++] = 0xFF;
        cc[slot++] = (uint8_t)pfm3DiagFault.faultId;
    } else {
        cc[slot++] = PFM3_DIAG_REPLAY_MAGIC;
        cc[slot++] = (uint8_t)frameIdx;
        for (int i = frameIdx * 7; i < frameIdx * 7 + 7 && i < total; i++) {
            cc[slot++] = (uint8_t)(raw[i] >> 1);
            cc[slot++] = (uint8_t)(raw[i] & 1);
        }
    }
    return slot;
}

// Emits one encoded frame: USB MIDI on target, recorded on host.
static void pfm3DiagReplayEmitFrame(int frameIdx) {
    uint8_t cc[PFM3_DIAG_REPLAY_SLOTS];
    int n = pfm3DiagReplayEncodeFrame(frameIdx, cc);

#ifndef PFM3_HOST
    struct MidiEvent ev;
    ev.eventType = MIDI_CONTROL_CHANGE;
    ev.channel = 15;
    for (int i = 0; i < n; i++) {
        ev.value[0] = 48 + i;
        ev.value[1] = cc[i];
        midiDecoder.writeMidiCCOut(&ev);
        midiDecoder.sendMidiUsbOutIfBufferFull();
    }
    midiDecoder.sendMidiDin5Out();
    midiDecoder.sendMidiUsbOut();
#else
    if (frameIdx >= 0 && frameIdx < PFM3_DIAG_REPLAY_FRAMES_MAX) {
        for (int i = 0; i < PFM3_DIAG_REPLAY_SLOTS; i++) {
            replaySinkFrames[frameIdx][i] = cc[i];
        }
    }
    replaySinkCount++;
    (void)n;
#endif
}

// Drives the replay state machine with a millisecond clock. Returns 1 when a
// frame was emitted THIS call (caller yields its loop pass — one USB transmit
// per pass on target); 0 otherwise. Phase 0 checks the magic exactly once;
// phase 1 waits 3 s for USB to come up; phase 2 streams one frame per >=5 ms;
// the trailer frame consumes the capture (magic = 0) so it never replays
// twice. Shared target/host (host tests drive it with synthetic times).
static int pfm3DiagReplayPoll(uint32_t nowMs) {
    if (replayPhase == 0) {
        replayPhase = (pfm3DiagFault.magic == PFM3_DIAG_FAULT_MAGIC) ? 1 : 3;
    }
    if (replayPhase == 1) {
        if (nowMs > 3000) {
            replayPhase = 2;
            replayFrame = 0;
            replayLastFrameMs = nowMs;
        }
    } else if (replayPhase == 2) {
        if (nowMs - replayLastFrameMs >= 5) {
            replayLastFrameMs = nowMs;
            const int frames = ((int) sizeof(Pfm3DiagFaultInfo) + 6) / 7;
            pfm3DiagReplayEmitFrame(replayFrame);
            replayFrame++;
            if (replayFrame > frames) {
                pfm3DiagFault.magic = 0;   // consume: never replay twice
                replayPhase = 3;
            }
            return 1;
        }
    }
    return 0;
}

#ifndef PFM3_HOST

void pfm3DiagWatchdogMainLoop() {
    // Crash-capture replay first: one frame (one USB transmit) per pass.
    if (pfm3DiagReplayPoll(HAL_GetTick())) {
        return;
    }

    // Report request (CC#119 code 1 / SysEx 'R'): drained here, main-loop
    // context only — never TX from the decode context.
    if (pfm3DiagReportRequest) {
        pfm3DiagReportRequest = 0;
        if (pfm3DiagEnabled) {
            pfm3DiagSendReport();
        }
    }

    static uint32_t lastCycles = 0;
    static uint32_t lastAlive = 0;
    static uint32_t lastReport = 0;
    static uint8_t stalled = 0;

    uint32_t now = DWT->CYCCNT;
    if (!pfm3DiagEnabled) {
        return;
    }
    if ((uint32_t)(now - lastCycles) < (uint32_t)DIAG_SAMPLE_CYCLES) {
        return;
    }
    lastCycles = now;
    diagWatchdogCycled = 1;

    if (pfm3DiagSysTickAliveCpt != lastAlive) {
        lastAlive = pfm3DiagSysTickAliveCpt;
        stalled = 0;
        return;
    }

    // Counter unchanged for a full sample window: SysTick layer stalled.
    if (!stalled) {
        stalled = 1;
        lastReport = 0;  // force an immediate first report
    }
    if ((uint32_t)(now - lastReport) >= (uint32_t)DIAG_REPORT_CYCLES) {
        lastReport = now;
        pfm3DiagSendReport();
    }
}

#else  // PFM3_HOST

void pfm3DiagInit() {
}

void pfm3DiagWatchdogMainLoop() {
    // Host: inert (no DWT/USB). The replay engine is driven directly by the
    // host tests through pfm3DiagTestReplayPoll below.
}

#endif // PFM3_HOST

// --- fault hook (never returns) ----------------------------------------------

#ifndef PFM3_HOST

extern "C" void pfm3DiagFaultHook(uint32_t faultId, uint32_t *frame) {
    pfm3DiagFault.magic = PFM3_DIAG_FAULT_MAGIC;
    pfm3DiagFault.faultId = faultId;
    pfm3DiagFault.cfsr = SCB->CFSR;
    pfm3DiagFault.hfsr = SCB->HFSR;
    pfm3DiagFault.dfsr = SCB->DFSR;
    pfm3DiagFault.mmfar = SCB->MMFAR;
    pfm3DiagFault.bfar = SCB->BFAR;
    pfm3DiagFault.afsr = SCB->AFSR;
    pfm3DiagFault.r0 = frame[0];
    pfm3DiagFault.r1 = frame[1];
    pfm3DiagFault.r2 = frame[2];
    pfm3DiagFault.r3 = frame[3];
    pfm3DiagFault.r12 = frame[4];
    pfm3DiagFault.lr = frame[5];
    pfm3DiagFault.psr = frame[7];
    pfm3DiagFault.pc = frame[6];
    pfm3DiagFault.sysTickCtrl = SysTick->CTRL;
    pfm3DiagFault.sysTickAliveCpt = pfm3DiagSysTickAliveCpt;
    pfm3DiagFault.mainLoopSeenAlive = diagWatchdogCycled;

    // Self-postmortem (build #8): capture is complete — soft-reset now.
    // .noinit survives NVIC_SystemReset; boot code replays the capture over
    // USB MIDI (see pfm3DiagReplayPoll). LED strobe was invisible in the
    // case anyway; the reset makes the unit heal itself into a state
    // that can report.
    for (volatile uint32_t d = 0; d < 24000000; d++) {
    }   // ~50 ms: let any in-flight USB TX drain
    NVIC_SystemReset();
}

#else  // PFM3_HOST

extern "C" void pfm3DiagFaultHook(uint32_t faultId, uint32_t *frame) {
    (void)faultId;
    (void)frame;
}

// --- host test hooks (tests/pfm3_diag_test.cpp only; NOT target API) --------
// The host build has no USB/DWT/SCB, so these expose just enough module state
// for the host suite: a clean reset, the replay phase machine with a
// synthetic clock, the recorded replay burst, and the tic-section
// accumulators. Prefer these over widening the public header API.

extern "C" {

void pfm3DiagTestReset(void) {
    pfm3DiagSysTickAliveCpt = 0;
    pfm3DiagAudioCpt = 0;
    pfm3DiagSeqTftCpt = 0;
    pfm3DiagSeqTftDeferredCpt = 0;
    pfm3DiagDeferSeqTft = 0;
    pfm3DiagEnabled = 1;
    pfm3DiagReportRequest = 0;
    pfm3DiagAckPending = 0;
    for (uint32_t i = 0; i < sizeof(Pfm3DiagFaultInfo) / sizeof(uint32_t); i++) {
        ((uint32_t *)&pfm3DiagFault)[i] = 0;
    }
    diagWatchdogCycled = 0;
    secCount = 0;
    secEncTotal = 0; secSeqTotal = 0; secTftTotal = 0; secTotalTotal = 0;
    secEncMax = 0; secSeqMax = 0; secTftMax = 0;
    tickCountWindow = 0;
    tickWindowStartCycles = 0;
    tickRateHz = 0;
    audioWinCycles = 0;
    systickWinCycles = 0;
    replayPhase = 0;
    replayFrame = 0;
    replayLastFrameMs = 0;
    replaySinkCount = 0;
    for (int f = 0; f < PFM3_DIAG_REPLAY_FRAMES_MAX; f++) {
        for (int s = 0; s < PFM3_DIAG_REPLAY_SLOTS; s++) {
            replaySinkFrames[f][s] = 0;
        }
    }
    hostTftCpt = 0;
}

int pfm3DiagTestReplayPoll(uint32_t nowMs) {
    return pfm3DiagReplayPoll(nowMs);
}

int pfm3DiagTestReplayFrameCount(void) {
    return replaySinkCount;
}

// Copies the recorded frame `idx` (0-based, trailer last) into out[16].
void pfm3DiagTestReplayFrame(int idx, uint8_t out[PFM3_DIAG_REPLAY_SLOTS]) {
    if (idx < 0 || idx >= PFM3_DIAG_REPLAY_FRAMES_MAX) {
        return;
    }
    for (int s = 0; s < PFM3_DIAG_REPLAY_SLOTS; s++) {
        out[s] = replaySinkFrames[idx][s];
    }
}

typedef struct {
    uint32_t count;
    uint32_t encTotal, seqTotal, tftTotal, totalTotal;
    uint32_t encMax, seqMax, tftMax;
} Pfm3DiagTestSecStats;

void pfm3DiagTestSecStats(Pfm3DiagTestSecStats *out) {
    out->count = secCount;
    out->encTotal = secEncTotal;
    out->seqTotal = secSeqTotal;
    out->tftTotal = secTftTotal;
    out->totalTotal = secTotalTotal;
    out->encMax = secEncMax;
    out->seqMax = secSeqMax;
    out->tftMax = secTftMax;
}

}  // extern "C"

#endif // PFM3_HOST

#endif /* PFM3_DIAG_ENABLED || PFM3_HOST */
