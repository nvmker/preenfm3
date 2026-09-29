// Compile-time + behavioral proof of the DISABLED branch of pfm3_diag.h
// (phase 8.8 SW5 gating): with NEITHER PFM3_DIAG_ENABLED NOR PFM3_HOST
// defined, every hook resolves to an empty `static inline` body.
//
// HOW this TU gets there: the host test build defines PFM3_HOST on the
// command line for every pfm3_tests TU (PUBLIC on the pfm3_fw_host object
// library). This file #undef's BOTH gates BEFORE including the header, so
// the preprocessor selects the #else no-op branch — the exact code a
// release/debug firmware build sees.
//
// What succeeding proves:
//   * COMPILING: every hook called from other TUs exists as a no-op — the
//     disabled header branch declares NO extern variable (pfm3DiagFault,
//     the counters) and NO out-of-line function, so any such reference
//     would be a hard compile error here. Compilation IS the gate proof.
//   * BEHAVIOR: pfm3DiagSeqTftGate/CcHook/SysexHook return constant 0, so
//     the hooked call sites neither run diag code nor swallow events in a
//     non-diagnostic build (the release/debug builds stay byte-identical).
// The void hooks (Init/Watchdog/TicSections/Command/Audio*) have nothing to
// observe BY DESIGN — no state exists in the disabled path.

#undef PFM3_HOST
#undef PFM3_DIAG_ENABLED

#include "pfm3_diag.h"

#include <gtest/gtest.h>

#include <cstdint>

TEST(Pfm3DiagDisabledGate, EveryHookCompilesToAnInlineNoOp) {
    // All void hooks: callable, no state anywhere to change.
    pfm3DiagInit();
    pfm3DiagWatchdogMainLoop();
    pfm3DiagTicSections(1, 2, 3, 4);
    pfm3DiagCommand(1);
    pfm3DiagAudioWatchdog();
    pfm3DiagAudioExit();

    // Value hooks: constant results — the original code always runs and no
    // CC/SysEx event is ever consumed.
    EXPECT_EQ(pfm3DiagSeqTftGate(), 0);
    EXPECT_EQ(pfm3DiagCcHook(15, 119, 1), 0);   // even the real diag ch16+CC+code
    EXPECT_EQ(pfm3DiagCcHook(15, 119, 6), 0);
    EXPECT_EQ(pfm3DiagCcHook(0, 0, 0), 0);

    const uint8_t magicP3D[6] = {0x7d, 'P', '3', 'D', 'R', 0};
    EXPECT_EQ(pfm3DiagSysexHook(magicP3D, 6), 0);
    EXPECT_EQ(pfm3DiagSysexHook(magicP3D, 5), 0);

    SUCCEED() << "disabled-path hooks compile to inline no-ops";
}
