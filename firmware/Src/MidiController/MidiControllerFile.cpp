/*
 * Copyright 2021 Xavier Hosxe
 *
 * Author: Xavier Hosxe (xavier <dot> hosxe (at) g m a i l <dot> com)
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


#include "MidiControllerFile.h"

namespace {

const char* const MIDI_CONTROLLER_STATE_CANONICAL = MIDI_CONTROLLER_STATE_NAME;
const char* const MIDI_CONTROLLER_STATE_TMP = "0:/pfm3/MidiCtl1.tmp";
const char* const MIDI_CONTROLLER_STATE_BACKUP = "0:/pfm3/MidiCtl1.bak";
constexpr int MIDI_CONTROLLER_STATE_V1_SIZE = 2 + MIDI_NUMBER_OF_PAGES * 12 * 20;

} // namespace

MidiControllerFile::MidiControllerFile() {
}

MidiControllerFile::~MidiControllerFile() {
}

bool MidiControllerFile::isValidConfigFile(const char* fileName) {
    if (checkSize(fileName) != MIDI_CONTROLLER_STATE_V1_SIZE) {
        return false;
    }

    uint16_t version = 0;
    return load(fileName, 0, &version, sizeof(version)) == sizeof(version)
            && version == MIDI_CONTROLLER_VERSION_1;
}

const char* MidiControllerFile::recoverConfigFile() {
    if (isValidConfigFile(MIDI_CONTROLLER_STATE_CANONICAL)) {
        // The canonical file is authoritative. A backup means promotion
        // completed before cleanup; a temp means saving stopped before the
        // old file was rotated. Neither should replace a valid canonical file.
        f_unlink(MIDI_CONTROLLER_STATE_BACKUP);
        f_unlink(MIDI_CONTROLLER_STATE_TMP);
        return MIDI_CONTROLLER_STATE_CANONICAL;
    }

    if (isValidConfigFile(MIDI_CONTROLLER_STATE_BACKUP)) {
        // Saving stopped after the old file was moved aside. Restore the last
        // known-good config; if recovery itself fails, load directly from the
        // backup and leave every recovery artifact intact for the next boot.
        int canonicalSize = checkSize(MIDI_CONTROLLER_STATE_CANONICAL);
        if (canonicalSize != -1 && f_unlink(MIDI_CONTROLLER_STATE_CANONICAL) != FR_OK) {
            return MIDI_CONTROLLER_STATE_BACKUP;
        }
        if (f_rename(MIDI_CONTROLLER_STATE_BACKUP, MIDI_CONTROLLER_STATE_CANONICAL) == FR_OK) {
            f_unlink(MIDI_CONTROLLER_STATE_TMP);
            return MIDI_CONTROLLER_STATE_CANONICAL;
        }
        return MIDI_CONTROLLER_STATE_BACKUP;
    }

    if (isValidConfigFile(MIDI_CONTROLLER_STATE_TMP)) {
        // First-save recovery, or a completed temp write with no recoverable
        // canonical/backup file. Promote it when possible and otherwise load
        // it in place so a valid configuration is never discarded.
        int canonicalSize = checkSize(MIDI_CONTROLLER_STATE_CANONICAL);
        if (canonicalSize != -1 && f_unlink(MIDI_CONTROLLER_STATE_CANONICAL) != FR_OK) {
            return MIDI_CONTROLLER_STATE_TMP;
        }
        if (f_rename(MIDI_CONTROLLER_STATE_TMP, MIDI_CONTROLLER_STATE_CANONICAL) == FR_OK) {
            return MIDI_CONTROLLER_STATE_CANONICAL;
        }
        return MIDI_CONTROLLER_STATE_TMP;
    }

    return MIDI_CONTROLLER_STATE_CANONICAL;
}

void MidiControllerFile::loadConfig(MidiControllerState* midiControllerState) {
    char* reachableProperties = storageBuffer;
    const char* configFileName = recoverConfigFile();

    int size = checkSize(configFileName);
    if (size >= PROPERTY_FILE_SIZE || size == -1) {
        // ERROR
        return;
    }
    reachableProperties[size] = 0;

    // A short read or failed close returns 0 from load(); deserializing the
    // stale storageBuffer would inject garbage records into the state.
    if (load(configFileName, 0, reachableProperties, size) != size) {
        return;
    }
    // First int is the version
    uint16_t* p = (uint16_t*)reachableProperties;
    int version = (int)*(p++);

    switch (version) {
    case MIDI_CONTROLLER_VERSION_1: {
        if (size != MIDI_CONTROLLER_STATE_V1_SIZE) {
            // Truncated body: walking the records would deserialize stale
            // storageBuffer bytes past the file's end.
            return;
        }
        for (int pageNumber = 0; pageNumber < MIDI_NUMBER_OF_PAGES; pageNumber++) {
            for (int e = 0; e < 6; e++) {
                MidiEncoder *encoder = midiControllerState->getEncoder(pageNumber, e);
                char *nameP = (char*)p;
                for (int c = 0; c < 6 ; c++) {
                    encoder->name[c] = *(nameP++);
                }
                // Let skip 8 bytes
                p += 4;
                encoder->encoderType = (MidiEncoderType)*(p++);
                encoder->midiChannel = *(p++);
                if (encoder->midiChannel > 16) {
                    // Corrupt byte: fail safe to the 'use global' sentinel.
                    encoder->midiChannel = 16;
                }
                encoder->controller = *(p++);
                encoder->value = *(p++);
                encoder->maxValue = *(p++);
                encoder->minValue = *(p++);
            }
            for (int b = 0; b < 6; b++) {
                MidiButton *button = midiControllerState->getButton(pageNumber, b);
                char *nameP = (char*)p;
                for (int c = 0; c < 6 ; c++) {
                    button->name[c] = *(nameP++);
                }
                // Let skip 8 bytes
                p += 4;
                button->buttonType = (MidiButtonType)*(p++);
                if (button->buttonType != MIDI_BUTTON_TYPE_PUSH
                        && button->buttonType != MIDI_BUTTON_TYPE_TOGGLE) {
                    // Corrupt byte: fail safe to the PUSH default.
                    button->buttonType = MIDI_BUTTON_TYPE_PUSH;
                }
                button->midiChannel = *(p++);
                if (button->midiChannel > 16) {
                    // Corrupt byte: fail safe to the 'use global' sentinel.
                    button->midiChannel = 16;
                }
                button->controller = *(p++);
                button->value = *(p++);
                button->valueOff = *(p++);
                button->valueOn = *(p++);
            }
        }
        break;
    }
    }
}


void MidiControllerFile::saveConfig(MidiControllerState* midiControllerState) {
    char* reachableProperties = storageBuffer;

    for (int i = 0; i < PROPERTY_FILE_SIZE; i++) {
        reachableProperties[i] = 0;
    }

    uint16_t* p = (uint16_t*)reachableProperties;
    *(p++) = MIDI_CONTROLLER_CURRENT_VERSION;

    for (int pageNumber = 0; pageNumber < MIDI_NUMBER_OF_PAGES; pageNumber++) {
        for (int e = 0; e < 6; e++) {
            MidiEncoder *encoder = midiControllerState->getEncoder(pageNumber, e);
            char *nameP = (char*)p;
            for (int c = 0; c < 6 ; c++) {
                *(nameP++) = encoder->name[c];
            }
            // Let skip 8 bytes
            p += 4;
            *(p++) = encoder->encoderType;
            *(p++) = encoder->midiChannel;
            *(p++) = encoder->controller;
            *(p++) = encoder->value;
            *(p++) = encoder->maxValue;
            *(p++) = encoder->minValue;
        }
        for (int b = 0; b < 6; b++) {
            MidiButton *button = midiControllerState->getButton(pageNumber, b);
            char *nameP = (char*)p;
            for (int c = 0; c < 6 ; c++) {
                *(nameP++) = button->name[c];
            }
            // Let skip 8 bytes
            p += 4;
            *(p++) = button->buttonType;
            *(p++) = button->midiChannel;
            *(p++) = button->controller;
            *(p++) = button->value;
            *(p++) = button->valueOff;
            *(p++) = button->valueOn;
        }
    }

#ifdef PFM3_HOST
    int size = reinterpret_cast<char*>(p) - reachableProperties;
#else
    int size = ((uint32_t)p) -  ((uint32_t)reachableProperties);
#endif
    // Recover an interrupted previous save before starting another. If a
    // valid fallback cannot be restored to the canonical name, leave it alone
    // so loadConfig() can still use it directly.
    const char* activeConfig = recoverConfigFile();
    if (activeConfig != MIDI_CONTROLLER_STATE_CANONICAL) {
        return;
    }

    const bool hadValidConfig = isValidConfigFile(MIDI_CONTROLLER_STATE_CANONICAL);
    f_unlink(MIDI_CONTROLLER_STATE_TMP);
    if (save(MIDI_CONTROLLER_STATE_TMP, 0, reachableProperties, size) != size
            || !isValidConfigFile(MIDI_CONTROLLER_STATE_TMP)) {
        return;
    }

    if (hadValidConfig) {
        // FatFs cannot rename over an existing destination. Rotate the old
        // file to a backup first; loadConfig() understands every intermediate
        // state and restores the last known-good file after interruption.
        f_unlink(MIDI_CONTROLLER_STATE_BACKUP);
        if (checkSize(MIDI_CONTROLLER_STATE_BACKUP) != -1
                || f_rename(MIDI_CONTROLLER_STATE_CANONICAL, MIDI_CONTROLLER_STATE_BACKUP) != FR_OK) {
            return;
        }
    } else {
        int canonicalSize = checkSize(MIDI_CONTROLLER_STATE_CANONICAL);
        if (canonicalSize != -1 && f_unlink(MIDI_CONTROLLER_STATE_CANONICAL) != FR_OK) {
            return;
        }
    }

    if (f_rename(MIDI_CONTROLLER_STATE_TMP, MIDI_CONTROLLER_STATE_CANONICAL) != FR_OK) {
        if (hadValidConfig) {
            // Best-effort immediate rollback. If it fails, the backup remains
            // loadable and the next load/save retries recovery.
            f_rename(MIDI_CONTROLLER_STATE_BACKUP, MIDI_CONTROLLER_STATE_CANONICAL);
        }
        return;
    }

    f_unlink(MIDI_CONTROLLER_STATE_BACKUP);
}



