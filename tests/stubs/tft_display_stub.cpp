/*
 * Host stubs for the only hardware symbols the firmware/Src/filesystem TUs
 * touch (Phase 4 seam — see tests/SEAM.md + the fatfs.h/TftDisplay.h shims).
 *
 *   TftDisplay tft          — no-op display surface (shadow class; progress
 *                             output only — MixerBank::createMixerBank,
 *                             SequenceBank::createSequenceFile,
 *                             PPMImage::saveImage).
 *   uint16_t tftMemory[]    — the TFT framebuffer PPMImage dumps. Fixture
 *                             data: tests write it before saveImage().
 *   HAL_Delay(uint32_t)     — host no-op (PPMImage::saveImage's post-capture
 *                             delay; the firmware-side delays in
 *                             SequenceBank/Synth are PFM3_HOST-guarded).
 *
 *   const struct MidiConfig midiConfig[MIDICONFIG_SIZE]
 *                         — FIXTURE DATA, not firmware fidelity (the real
 *                             table lives in hardware/Menu.cpp, which is not
 *                             host-compilable). Struct layout, field names
 *                             and the 13-entry count mirror
 *                             firmware/Src/hardware/Menu.h exactly;
 *                             ConfigurationFile is data-driven, so titles /
 *                             nameInFile keys here are deterministic
 *                             stand-ins. See the spec's Design Notes.
 */
#include "TftDisplay.h"
#include "Menu.h"

TftDisplay tft;
uint16_t tftMemory[240 * 320];

void HAL_Delay(uint32_t) {
    /* host no-op */
}

/* --- midiConfig stub table (13 entries, MIDICONFIG_SIZE parity) ----------- */

static const char* kUsbNames[]    = { "Off", "In", "In+Out" };
static const char* kRecvNames[]   = { "No", "Yes" };
static const char* kRevNames[9]  = { "r0", "r1", "r2", "r3", "r4",
                                     "r5", "r6", "r7", "r8" };
static const char* kTestNote[]    = { "C2", "C3", "C4" };
static const char* kEncoder[]     = { "Rel", "Abs" };
static const char* kPushNames[]   = { "None", "Enter" };
static const char* kArpInPreset[] = { "No", "Yes" };

const struct MidiConfig midiConfig[MIDICONFIG_SIZE] = {
    /* title            nameInFile          maxValue  valueName      */
    { "Usb Mode",       "usbmode",                 2, kUsbNames     },
    { "Receives",       "midireceives",            1, kRecvNames    },
    { "Sends",          "midisends",              16, nullptr       },
    { "Prog Change",    "programchange",           1, kRecvNames    },
    { "Encoder",        "encoder",                 1, kEncoder      },
    { "Test Note",      "testnote",                2, kTestNote     },
    { "Test Velocity",  "testvelocity",          127, nullptr       },
    { "Arp In Preset",  "arpinpreset",             1, kArpInPreset  },
    { "Cpu Usage",      "cpuusage",              100, nullptr       },
    { "Tft AutoReinit", "tftautoreinit",           1, kRecvNames    },
    { "Encoder Push",   "encoderpush",             1, kPushNames    },
    { "Tft Backlight",  "tftbacklight",          100, nullptr       },
    { "Reverb Params",  "reverbparams",            9, kRevNames     },
};

static_assert(sizeof(midiConfig) / sizeof(midiConfig[0]) == MIDICONFIG_SIZE,
              "midiConfig stub table must match Menu.h's MIDICONFIG_SIZE");

/*
 * oscShapeNames / envCurveNames: defined in the real firmware by
 * hardware/FMDisplayEditor.cpp (4k+ lines of TFT/UI code — not
 * host-compilable). UserWaveform/UserEnvCurve assign the user slots
 * (indices 8+/3+) into these tables, so they must be MUTABLE char* arrays.
 * Names mirror the real FMDisplayEditor.cpp tables (fixture fidelity).
 */
const char *oscShapeNames[] = {
    "sin ", "saw ", "squa", "s^2 ", "szer", "spos", "porS", "porL",
    "usr1", "usr2", "usr3", "usr4", "usr5", "usr6", "off ",
};

const char *envCurveNames[] = { "Exp ", "Lin ", "Log ", "Usr1", "Usr2", "Usr3",
        "Usr4" };

/* Firmware index contracts (see UserWaveform.cpp / UserEnvCurve.cpp):
 * oscShapeNames[8+curve] for 6 user waveforms -> needs >= 14 slots;
 * real table has 15 (sin..off). envCurveNames[3+curve] for 4 user curves
 * -> needs >= 7 slots. A future table edit must fail to compile here, not
 * index garbage at test time. */
static_assert(sizeof(oscShapeNames) / sizeof(*oscShapeNames) >= 8 + 6,
              "oscShapeNames must cover the 6 user-waveform slots (8..13)");
static_assert(sizeof(envCurveNames) / sizeof(*envCurveNames) >= 3 + 4,
              "envCurveNames must cover the 4 user-curve slots (3..6)");
