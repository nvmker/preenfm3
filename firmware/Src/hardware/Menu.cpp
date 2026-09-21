/*
 * Copyright 2020 Xavier Hosxe
 *
 * Author: Xavier Hosxe (xavier . hosxe (at) gmail . com)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "Menu.h"
#include "PreenFMFileType.h"
#include "version.h"

#ifndef OVERCLOCK
#define OVERCLOCK_STRING
#else
#define OVERCLOCK_STRING "o"
#endif

const char* noYes [] = { "No", "Yes" };
const char* midiReceives[] = { "None", "CC", "NRPN", "CC & NRPN" };
const char* midiSends [] = { "None", "CC", "NRPN" };
const char* encoderType [] = { "12", "24", "12i", "24i" };
const char* usbMidiText[] = { "Off", "In", "In/Out" };
const char* version[] = { PFM3_FIRMWARE_VERSION };
const char* tftAutoReinit [] = { "Off", "Auto" };
const char* reverbParam[] = { "Hide", "Show" };



/*
 * Mixer Preset Seq SD Config

SD -> Create Rename
Create -> Mixer Preset Seq
Rename -> Mixre Preset Seq

Mixer -> Load Save Dfl
Dfl -> Load Save Delete

Preset -> Load Save DX7  Rand

Seq -> Load Save Dfl
Dfl -> Load Save Delete

 */


const struct MidiConfig midiConfig[]  = {
        {
                "Usb Midi",
                "usbmidi",
                3,
				usbMidiText
        },
        {
                "Receives",
                "midireceives",
                4,
                midiReceives
        },
        {
                "Sends",
                "midisend",
                3,
                midiSends
        },
        {
                "Program Change",
                "programchange",
                2,
                noYes
        },
        {
                "Encoder Driver",
                "encoders",
                4,
                encoderType
        },
        {
                "Test Note",
                "testnote",
                127,
                0
        },
        {
                "Test Note Velocity",
                "testvelocity",
                127,
                0
        },
        {
                "Arp in Preset",
                "arpinpreset2",
                2,
                noYes
        },
        {
                "Cpu Usage",
                "cpuusage",
                2,
                noYes
        },
        {
                "TFT Reinit",
                "tftautoreinit",
                2,
                tftAutoReinit
        },
        {
                "Encoder Push",
                "encoderpush",
                2,
                noYes
        },
        {
                "TFT backlight",
                "tftbacklight",
                101,
                0
        },
        {
                "Reverb params",
                "reverbparams",
                2,
                reverbParam
        },
        {
                "Firmware Version",
                "",
                1,
                version
        }

};
