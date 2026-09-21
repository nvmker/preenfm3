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

#include <string.h>   /* memset for the fully-initialized capture locals */

#ifndef PFM3_HOST

#include "main.h"
#include "stm32h7xx_hal.h"
#include "preenfm3.h"
#include "MidiDecoder.h"

// USB device stack for the diag-owned replay writer (P10): the MIDI class
// header carries MIDI_IN_EP + the USBD types. Same extern-"C" include
// pattern as MidiDecoder.cpp (the chain has no host build; guarded).
extern "C" {
#include "../../Middlewares/ST/STM32_USB_Device_Library/Class/MIDI/Inc/usbd_midi.h"
}

// Global MIDI decoder instance owned by preenfm3.cpp (report TX path).
extern MidiDecoder midiDecoder;

// USB device + PCD handles (defined in usbd_conf.c / usbd_core / usb_device.c).
extern "C" {
extern USBD_HandleTypeDef hUsbDeviceFS;
extern PCD_HandleTypeDef hpcd_USB_OTG_FS;
}

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

// Capture-publication reentry guard (8.8 SW5 review round): set by BOTH
// capture paths before they touch .noinit. A fault landing inside the fault
// hook (fault-in-fault) or a stall window closing during a publication must
// never half-overwrite a capture — the early-exit paths just delay+reset and
// let the already-published (or absent) capture stand.
static volatile uint32_t diagCaptureActive = 0;

// Snapshot of RCC->RSR.SFTRSTF taken exactly once in pfm3DiagInit (P16).
// The reset-cause flags survive until RMVF is written — nothing in this
// firmware writes it — but they are latched at init so no later consumer
// can race a (hypothetical future) flag clear. Host: always 1 (tests plant
// captures directly; there is no reset controller on host).
static uint8_t diagResetWasSoftware = 1;

// ~50 ms drain delay then soft reset — shared by all capture paths. TARGET
// ONLY (host has no NVIC): the host paths never capture-and-reset.
#ifndef PFM3_HOST
static void pfm3DiagDelayAndReset() {
    for (volatile uint32_t d = 0; d < 24000000; d++) {
    }   // ~50 ms: let any in-flight USB TX drain
    NVIC_SystemReset();
}

// Publication discipline (8.8 SW5 review round): callers build a FULLY
// initialized capture in a local (memset 0 first — no stale dfsr/mmfar/
// afsr/r12 word from a previous capture can leak into a STALL frame) with
// magic == 0, then this copies it to .noinit, barriers, and writes the
// magic LAST. A partial payload is therefore never observable behind a
// valid magic, on any path. TARGET ONLY (barriers are CMSIS intrinsics).
static void pfm3DiagPublishCapture(const Pfm3DiagFaultInfo *cap) {
    pfm3DiagFault = *cap;          // 76-byte struct copy, magic == 0
    __DSB();
    __ISB();
    pfm3DiagFault.magic = PFM3_DIAG_FAULT_MAGIC;
    __DSB();
}
#endif /* !PFM3_HOST */

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
    // Snapshot + reset the accumulators under a brief critical section
    // (P13): audioWinCycles is written from the audio IRQ (we are in it, but
    // a HIGHER-priority preemption could re-enter), systickWinCycles and
    // dutyTickCount from the SysTick IRQ. Without the guard, cycles landing
    // between the latch-read and the reset are silently dropped (or, worse,
    // double-counted into the next window). Percentages in uint64 (P11):
    // audioWinCycles * 100 overflows u32 above ~8.95% duty at 480 MHz.
    uint32_t aCyc, sCyc, ticks;
    __disable_irq();
    aCyc = audioWinCycles;
    sCyc = systickWinCycles;
    ticks = dutyTickCount;
    audioWinCycles = 0;
    systickWinCycles = 0;
    dutyTickCount = 0;
    __enable_irq();
    uint32_t a = (uint32_t)(((uint64_t)aCyc * 100u + w / 2) / w);
    uint32_t s = (uint32_t)(((uint64_t)sCyc * 100u + w / 2) / w);
    latchAudioPct = a > 127 ? 127 : (uint8_t)a;
    latchSystickPct = s > 127 ? 127 : (uint8_t)s;
    uint32_t resid = 100u - (a > 100 ? 100 : a) - (s > 100 ? 100 : s);
    latchResidualPct = resid > 100 ? 0 : (uint8_t)resid;
    latchTicks = (uint16_t)ticks;
    latchDwtMhz8 = (uint8_t)((w >> 22) & 0x7F);
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

// --- audio-IRQ-context SysTick watchdog -----------------------------------
// Runs from both SAI callbacks (the only context guaranteed alive in the
// H1-type hang: main loop AND SysTick dead, audio alive). DWT-timed sample
// window = SystemCoreClock/5 (~0.2 s). A STALL is declared when the SysTick
// alive counter has been silent for at least five consecutive sample
// windows AND >= 1.0 s of DWT time since the first silent sample (P14: the
// documented contract is a >= 1 s stall — five 0.2 s windows alone can span
// as little as ~0.8 s). BY DESIGN the unit is captured+reset even when only
// SysTick is dead and the main loop is still alive: the capture's
// mainLoopSeenAlive word disambiguates the two cases post-mortem, and a
// frozen SysTick alone is already an unrecoverable state for this firmware
// (all encode/tft/sequencer work lives in preenfm3Tic). GPIO/register only
// — no queue, no USB, safe from IRQ context.

static uint32_t audioEntryCycles = 0;

extern "C" void pfm3DiagAudioWatchdog() {
    pfm3DiagAudioCpt++;
    audioEntryCycles = DWT->CYCCNT;
    pfm3DiagDutyWindow(DWT->CYCCNT);

    static uint32_t lastSampleCycles = 0;
    static uint32_t aliveAtLastSample = 0;
    static uint32_t stalledWindows = 0;
    static uint32_t stallStartCycles = 0;

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
    // P14(b): a watchdog disabled via CC#119 code 5 must NEVER reset the
    // unit from here either — reset the accumulator and stay silent.
    if (!pfm3DiagEnabled) {
        stalledWindows = 0;
        return;
    }
    if (stalledWindows == 0) {
        stallStartCycles = now;
    }
    stalledWindows++;
    // P14(a): >= 5 consecutive silent windows AND >= 1.0 s DWT-elapsed —
    // fires 1.0-1.2 s into the stall (contract: ">= 1 s", h8_crashwatch.py).
    if (stalledWindows < 5u || (uint32_t)(now - stallStartCycles) < SystemCoreClock) {
        return;
    }
    // SysTick AND main loop dead while audio keeps running (main-loop-only
    // death is distinguished post-mortem by mainLoopSeenAlive). Capture a
    // stall snapshot and soft-reset: boot replays it over USB MIDI (.noinit
    // survives NVIC reset).
    if (!diagCaptureActive) {
        diagCaptureActive = 1;
        Pfm3DiagFaultInfo cap;
        memset(&cap, 0, sizeof cap);   // no stale dfsr/mmfar/afsr/r12 words
        cap.faultId = 6;               // STALL (1..5 = real faults)
        cap.cfsr = SCB->CFSR;
        cap.hfsr = SCB->HFSR;
        cap.dfsr = SCB->DFSR;
        cap.mmfar = SCB->MMFAR;
        cap.bfar = SCB->BFAR;
        cap.afsr = SCB->AFSR;
        cap.psr = __get_IPSR();
        cap.sysTickCtrl = SysTick->CTRL;
        cap.sysTickAliveCpt = pfm3DiagSysTickAliveCpt;
        cap.mainLoopSeenAlive = diagWatchdogCycled;
        cap.r0 = NVIC->ISPR[0];
        cap.r1 = NVIC->ISPR[1];
        cap.r2 = SCB->ICSR;
        cap.r3 = SCB->SHCSR;
        pfm3DiagPublishCapture(&cap);
    }
    pfm3DiagDelayAndReset();
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
        // P15(b): IGNORED while a valid capture is pending replay — the
        // reboot would overwrite the .noinit capture (and a stuck host
        // sending the CC repeatedly would reboot-loop the unit). The pending
        // capture replays first; re-send code 6 after it drains. (Checked on
        // host too so the gate is unit-tested.)
        if (pfm3DiagFault.magic == PFM3_DIAG_FAULT_MAGIC) {
            break;
        }
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

int pfm3DiagCcHook(uint8_t channel, uint8_t cc, uint8_t value) {
    // P15(a): CC#119 commands are accepted ONLY on MIDI channel 16
    // (0-based 15) — the private channel the report/replay traffic already
    // uses. The parked code accepted ANY channel, so a legitimate CC#119
    // value 6 routed to a user channel force-reset the unit.
    if (channel == PFM3_DIAG_CC_CHANNEL && cc == PFM3_DIAG_CC
            && value >= 1 && value <= 6) {
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
    //
    // P15(c): MidiDecoder::analyseSysexBuffer delivers the payload WITHOUT
    // the F0/F7 framing — the documented 8-byte wire form arrives here as 6
    // bytes. Accept BOTH forms (strip a leading F0 / trailing F7 when
    // present); the old `size == 7` test could never match real traffic.
    // Secondary transport: CC#119 (see pfm3DiagCcHook) is the reliable path —
    // USB-MIDI sysex reassembly can drop the terminating F7 depending on
    // host packet batching. Same command codes.
    const uint8_t *p = sysexBuffer;
    if (size >= 1 && p[0] == 0xF0) {
        p++;
        size--;
    }
    if (size >= 1 && p[size - 1] == 0xF7) {
        size--;
    }
    if (size == 6 && p[0] == 0x7d && p[1] == 'P' && p[2] == '3'
            && p[3] == 'D') {
        switch (p[4]) {
        case 'D':
            pfm3DiagCommand(p[5] != 0 ? 2 : 3);
            return 1;
        case 'W':
            pfm3DiagCommand(p[5] != 0 ? 4 : 5);
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

    // P16: latch the reset cause EXACTLY ONCE, before anything downstream
    // could (now or ever) clear it. RCC->RSR reset flags survive until RMVF
    // is written; nothing in this firmware writes RMVF, so an init-time
    // read is safe — and the boot-validity gate (pfm3DiagReplayPoll phase
    // 0) uses this latched copy, never the register.
    diagResetWasSoftware = (RCC->RSR & RCC_RSR_SFTRSTF) ? 1 : 0;

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
    // Slots 11/13 carried the VECTACTIVE top-other histogram (build #5).
    // REMOVED (8.8 SW5 review round): sampling ICSR.VECTACTIVE from inside
    // the SAI callback can structurally only observe the SAI vector itself,
    // and its 10 s DWT window overflowed u32 at 480 MHz. The slot LAYOUT is
    // frozen (rig scripts address by slot), so the slots stay, always 0.
    payload[11] = 0;                                               // (was top-other vector id)
    payload[12] = (uint8_t)((__get_PRIMASK() & 0x1)
            | ((SCB->VTOR != 0x08000000u) ? 2 : 0)
            | (pfm3DiagDeferSeqTft ? 4 : 0)
            | (pfm3DiagEnabled ? 8 : 0));
    payload[13] = 0;                                               // (was top-other %)
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

// Encodes replay frame `frameIdx` of pfm3DiagFault into cc[0..15] (the CC
// values for CC#48+slot). frameIdx == frame count emits the trailer. Pure
// function of the struct + index: no hardware, no TX — host-testable.
//
// P7 (CRITICAL): the frozen decoder (h8_crashwatch.py) commits a frame ONLY
// when its slot-15 CC arrives — every frame, data AND trailer, therefore
// emits ALL 16 slots, with unused payload slots padded to 0. The parked
// encoder stopped at slot 13/2, so the last data frame and the trailer were
// never committed and the burst unpacked at 70 bytes. Padding bytes land
// above byte 76 in the decoder's buffer and are ignored by data[:76].
static void pfm3DiagReplayEncodeFrame(int frameIdx, uint8_t cc[PFM3_DIAG_REPLAY_SLOTS]) {
    const uint8_t *raw = (const uint8_t *) &pfm3DiagFault;
    const int total = (int) sizeof(Pfm3DiagFaultInfo);
    const int frames = (total + 6) / 7;   // 7 bytes per frame

    for (int i = 0; i < PFM3_DIAG_REPLAY_SLOTS; i++) {
        cc[i] = 0;
    }
    cc[0] = PFM3_DIAG_REPLAY_MAGIC;
    if (frameIdx >= frames) {
        cc[1] = 0xFF;                              // trailer marker
        cc[2] = (uint8_t)pfm3DiagFault.faultId;
    } else {
        cc[1] = (uint8_t)frameIdx;
        for (int i = frameIdx * 7; i < frameIdx * 7 + 7 && i < total; i++) {
            cc[2 + 2 * (i - frameIdx * 7)] = (uint8_t)(raw[i] >> 1);
            cc[3 + 2 * (i - frameIdx * 7)] = (uint8_t)(raw[i] & 1);
        }
    }
}

#ifndef PFM3_HOST

// P10 — diag-owned replay writer (diagnostic builds only). The parked path
// reused MidiDecoder's TX stack, which (a) honors the user's MIDICONFIG_USB
// routing (crash replay would be silently dropped when USB-out is off) and
// (b) never checked endpoint/device state, so the capture was consumed even
// when ZERO frames reached the host. This writer instead:
//   (a) waits for USBD_STATE_CONFIGURED (USB enumerated),
//   (b) bypasses the routing config — crash replay is diagnostic traffic,
//   (c) refuses while the MIDI IN endpoint still has a transfer in flight
//       (PCD IN_ep xfer_len counts down to 0 once drained) — the poll
//       retries the SAME frame on the next pass,
//   (d) reports acceptance so the capture is consumed ONLY after the
//       trailer frame really went out. If USB never comes up, the capture
//       persists in .noinit and the next boot retries it.
// One frame = 16 CC events = exactly one 64-byte USB-MIDI bulk packet. The
// CC encoding itself (slot/value pairs on ch16) is FROZEN — unchanged.
static uint8_t diagReplayUsbPacket[64];

static int pfm3DiagUsbReady() {
    return hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED;
}

static int pfm3DiagReplayTxFrames(const uint8_t cc[PFM3_DIAG_REPLAY_SLOTS]) {
    if (!pfm3DiagUsbReady()) {
        return 0;
    }
    if (hpcd_USB_OTG_FS.IN_ep[MIDI_IN_EP & 0x0Fu].xfer_len != 0u) {
        return 0;   // endpoint busy: retry the same frame next pass
    }
    for (int i = 0; i < PFM3_DIAG_REPLAY_SLOTS; i++) {
        diagReplayUsbPacket[i * 4 + 0] = 0x0B;                        // cable 0, CC
        diagReplayUsbPacket[i * 4 + 1] = 0xB0 | PFM3_DIAG_CC_CHANNEL; // CC ch16
        diagReplayUsbPacket[i * 4 + 2] = (uint8_t)(48 + i);           // slot CC#
        diagReplayUsbPacket[i * 4 + 3] = cc[i];
    }
    return USBD_LL_Transmit(&hUsbDeviceFS, MIDI_IN_EP, diagReplayUsbPacket,
                            sizeof diagReplayUsbPacket) == USBD_OK;
}

// Emits one encoded frame; returns 1 when the frame was ACCEPTED by the
// transport (USB transmit queued). 0 = refused (unconfigured/busy) — the
// caller keeps the frame index and retries.
static int pfm3DiagReplayEmitFrame(int frameIdx) {
    uint8_t cc[PFM3_DIAG_REPLAY_SLOTS];
    pfm3DiagReplayEncodeFrame(frameIdx, cc);
    return pfm3DiagReplayTxFrames(cc);
}

#else  // PFM3_HOST

// Host seam: always accepted, recorded for the tests.
static int pfm3DiagReplayEmitFrame(int frameIdx) {
    uint8_t cc[PFM3_DIAG_REPLAY_SLOTS];
    pfm3DiagReplayEncodeFrame(frameIdx, cc);
    if (frameIdx >= 0 && frameIdx < PFM3_DIAG_REPLAY_FRAMES_MAX) {
        for (int i = 0; i < PFM3_DIAG_REPLAY_SLOTS; i++) {
            replaySinkFrames[frameIdx][i] = cc[i];
        }
    }
    replaySinkCount++;
    return 1;
}

#endif // PFM3_HOST

// Drives the replay state machine with a millisecond clock. Returns 1 when a
// frame was ACCEPTED this call (caller yields its loop pass — one USB
// transmit per pass on target); 0 otherwise. Phase 0 checks validity exactly
// once — valid magic AND software-reset cause AND faultId 1..6 (P16: guards
// cold-boot SRAM coincidentally holding the magic, and garbage faultIds);
// phase 1 waits 3 s AND USB CONFIGURED (P10: no settle-count guess — if USB
// never comes up, the capture persists and retries next boot); phase 2
// attempts one frame per >=5 ms (refused frames retry immediately — the
// busy/configured checks are the real gate); the capture is consumed ONLY
// after the trailer frame was accepted. Shared target/host (host tests drive
// it with synthetic times; the host transport always accepts).
static int pfm3DiagReplayPoll(uint32_t nowMs) {
    if (replayPhase == 0) {
        replayPhase = (pfm3DiagFault.magic == PFM3_DIAG_FAULT_MAGIC
                       && diagResetWasSoftware
                       && pfm3DiagFault.faultId >= 1u
                       && pfm3DiagFault.faultId <= 6u) ? 1 : 3;
    }
    if (replayPhase == 1) {
#ifndef PFM3_HOST
        if (nowMs > 3000 && pfm3DiagUsbReady())
#else
        if (nowMs > 3000)
#endif
        {
            replayPhase = 2;
            replayFrame = 0;
            replayLastFrameMs = nowMs;
        }
    } else if (replayPhase == 2) {
        if (nowMs - replayLastFrameMs >= 5) {
            const int frames = ((int) sizeof(Pfm3DiagFaultInfo) + 6) / 7;
            if (pfm3DiagReplayEmitFrame(replayFrame)) {
                replayLastFrameMs = nowMs;
                if (replayFrame >= frames) {
                    // Trailer ACCEPTED: consume now — never replay twice.
                    // (Consumed only here: a refused trailer leaves the
                    // capture intact for the retry / next boot.)
                    pfm3DiagFault.magic = 0;
                    replayPhase = 3;
                } else {
                    replayFrame++;
                }
                return 1;
            }
            // Refused (USB down / endpoint busy): same frame next pass.
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
    // Host: no DWT/SCB/RCC — but keep the P16 reset-cause latch at its
    // host default ("was software reset") so the replay path is testable.
    diagResetWasSoftware = 1;
}

void pfm3DiagWatchdogMainLoop() {
    // Host: inert (no DWT/USB). The replay engine is driven directly by the
    // host tests through pfm3DiagTestReplayPoll below.
}

#endif // PFM3_HOST

// --- fault hook (never returns) ----------------------------------------------

#ifndef PFM3_HOST

// CFSR stacking-error bits (P4): when exception entry itself failed to stack
// the frame, the frame pointer does NOT point at a stacked r0..psr —
// dereferencing it in the hook would fault again (fault-in-fault, capture
// lost). Those fields get distinct sentinel constants instead; addr2line on
// a sentinel immediately says "stacking error", not "random garbage PC".
#define PFM3_DIAG_CFSR_MSTKERR (1u << 4)    /* MMFSR: mem-manager stacking  */
#define PFM3_DIAG_CFSR_MLSPERR (1u << 5)    /* MMFSR: mem-manager lazy FP   */
#define PFM3_DIAG_CFSR_STKERR  (1u << 12)   /* BFSR: bus stacking           */
#define PFM3_DIAG_CFSR_LSPERR  (1u << 13)   /* BFSR: bus lazy FP            */
#define PFM3_DIAG_CFSR_STACKING_ERRS \
    (PFM3_DIAG_CFSR_MSTKERR | PFM3_DIAG_CFSR_MLSPERR \
     | PFM3_DIAG_CFSR_STKERR | PFM3_DIAG_CFSR_LSPERR)
#define PFM3_DIAG_FRAME_SENTINEL(i) (0xFA110000u | (uint32_t)(i))  /* FAIL-i */

extern "C" void pfm3DiagFaultHook(uint32_t faultId, uint32_t *frame, uint32_t excReturn) {
    if (diagCaptureActive) {
        // Fault INSIDE a capture (fault-in-fault): the in-flight publication
        // stands (or nothing was published); never touch .noinit again.
        pfm3DiagDelayAndReset();
    }
    diagCaptureActive = 1;

    Pfm3DiagFaultInfo cap;
    memset(&cap, 0, sizeof cap);   // FULLY initialized — no stale words
    cap.faultId = faultId;
    cap.cfsr = SCB->CFSR;
    cap.hfsr = SCB->HFSR;
    cap.dfsr = SCB->DFSR;
    cap.mmfar = SCB->MMFAR;
    cap.bfar = SCB->BFAR;
    cap.afsr = SCB->AFSR;
    cap.sysTickCtrl = SysTick->CTRL;
    cap.sysTickAliveCpt = pfm3DiagSysTickAliveCpt;
    cap.mainLoopSeenAlive = diagWatchdogCycled;

    if (cap.cfsr & PFM3_DIAG_CFSR_STACKING_ERRS) {
        // P4: exception stacking faulted — the frame pointer is not a valid
        // stacked frame. Sentinels, never a dereference.
        cap.r0 = PFM3_DIAG_FRAME_SENTINEL(0);
        cap.r1 = PFM3_DIAG_FRAME_SENTINEL(1);
        cap.r2 = PFM3_DIAG_FRAME_SENTINEL(2);
        cap.r3 = PFM3_DIAG_FRAME_SENTINEL(3);
        cap.r12 = PFM3_DIAG_FRAME_SENTINEL(4);
        cap.lr = PFM3_DIAG_FRAME_SENTINEL(5);
        cap.pc = PFM3_DIAG_FRAME_SENTINEL(6);
        cap.psr = PFM3_DIAG_FRAME_SENTINEL(7);
    } else {
        // P3: EXC_RETURN bit 4 == 0 => extended frame: the hardware pushed
        // 18 words of FP context (S0-S15, FPSCR, reserved) BELOW the basic
        // frame, so r0..psr sit 18 words up. Without this, every frame read
        // from FP-context code (any -mfpu build with lazy stacking!) would
        // report S-register garbage as PC/LR/PSR.
        if ((excReturn & (1u << 4)) == 0u) {
            frame += 18;
        }
        cap.r0 = frame[0];
        cap.r1 = frame[1];
        cap.r2 = frame[2];
        cap.r3 = frame[3];
        cap.r12 = frame[4];
        cap.lr = frame[5];
        cap.pc = frame[6];
        cap.psr = frame[7];
    }

    // Publication (P6): copy + barriers + magic LAST, then self-reset.
    // .noinit survives NVIC_SystemReset; boot replays the capture over
    // USB MIDI (see pfm3DiagReplayPoll).
    pfm3DiagPublishCapture(&cap);
    pfm3DiagDelayAndReset();
}

#else  // PFM3_HOST

extern "C" void pfm3DiagFaultHook(uint32_t faultId, uint32_t *frame, uint32_t excReturn) {
    (void)faultId;
    (void)frame;
    (void)excReturn;
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
    diagCaptureActive = 0;
    diagResetWasSoftware = 1;   // host default: tests plant captures directly
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

// P16 seam: override the latched reset-cause for the boot-validity gate
// (host default 1 = software reset; tests flip it to 0 to prove the gate).
void pfm3DiagTestSetResetCause(int wasSoftwareReset) {
    diagResetWasSoftware = wasSoftwareReset ? 1 : 0;
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
