/*
 * Copyright 2020 Xavier Hosxe
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



/*
 * SEQUENCE_BANK_VERSION1 : uin32_t on 4 bytes
 * Per Bank :
 * - State : 1024 bytes (contains its own version)
 * - Sequencer : 16384 + 12336 bytes
     - SeqMidiAction actions[2048]; (16384 bytes)
        uint16_t when;
        uint16_t nextIndex;
        uint8_t actionType;
        uint8_t param1;
        uint8_t param2;
        uint8_t unused;
     - StepSeqValue stepNotes[6][256 + 1]; (12336 bytes)
        uint8_t values[8];


 * SEQUENCE_BANK_VERSION2 : uin32_t on 4 bytes
 * Per Bank :
 * - State : 1024 bytes (contains its own version)
 * - Sequencer : 16384 + 24576 bytes
     - SeqMidiAction actions[2048]; (16384 bytes)
        uint16_t when;
        uint16_t nextIndex;
        uint8_t actionType;
        uint8_t param1;
        uint8_t param2;
        uint8_t unused;
     - StepSeqValue stepNotes[12][256]; (24576 bytes)
        uint8_t values[8];


 */


#include <SequenceBank.h>
#include "TftDisplay.h"

extern TftDisplay tft;


#ifndef PFM3_HOST
__attribute__((section(".ram_d2b")))
#endif
struct PFM3File preenFMSequenceAlloc[NUMBEROFPREENFMSEQUENCES];
#ifndef PFM3_HOST
__attribute__((section(".ram_d2b")))
#endif
static FIL sequenceFile;

extern SeqMidiAction actions[SEQ_ACTION_SIZE];
extern StepSeqValue stepNotes[NUMBER_OF_STEP_SEQUENCES][256];

// Payload loads are transactional: FatFs may return an error after copying a
// short prefix, so reading directly into the live sequencer tables can expose
// a mixed old/new state. Split the staging tables across SRAM regions with
// enough verified linker headroom (16 KiB in D3, 24 KiB in D2B).
#ifndef PFM3_HOST
__attribute__((section(".ram_d3")))
#endif
static SeqMidiAction stagedActions[SEQ_ACTION_SIZE];
#ifndef PFM3_HOST
__attribute__((section(".ram_d2b")))
#endif
static StepSeqValue stagedStepNotes[NUMBER_OF_STEP_SEQUENCES][256];

static_assert(sizeof(stagedActions) == 16384, "sequence action file layout changed");
static_assert(sizeof(stagedStepNotes) == 24576, "sequence step file layout changed");

SequenceBank::SequenceBank() {
    this->numberOfFilesMax_ = NUMBEROFPREENFMSEQUENCES;
    this->myFiles_ = preenFMSequenceAlloc;
}

const char* SequenceBank::getFolderName() {
    return PREENFM_DIR;
}


bool SequenceBank::isCorrectFile(char *name, int size) {
    int pointPos = -1;
    for (int k = 1; k < 9 && pointPos == -1; k++) {
        if (name[k] == '.') {
            pointPos = k;
        }
    }
    if (pointPos == -1)
        return false;
    if (name[pointPos + 1] != 's' && name[pointPos + 1] != 'S')
        return false;
    if (name[pointPos + 2] != 'e' && name[pointPos + 2] != 'E')
        return false;
    if (name[pointPos + 3] != 'q' && name[pointPos + 3] != 'Q')
        return false;

    return true;
}

void SequenceBank::setSequencer(Sequencer* sequencer) {
    this->sequencer = sequencer;
}

bool SequenceBank::isReadOnly(struct PFM3File *file) {
    const char* fullSeqBankName = getFullName(file->name);
    uint32_t bankVersion = 0;
    UINT byteRead;
    this->sequencer = sequencer;
    if (f_open(&sequenceFile, fullSeqBankName, FA_READ) == FR_OK) {
        if (f_read(&sequenceFile, (void *)&bankVersion, 4, &byteRead) != FR_OK || byteRead != 4) {
            // Short/failed version-header read: bankVersion is unusable —
            // same safe path as an unknown version (read-only).
            bankVersion = 0;
        }
        f_close(&sequenceFile);
    }
    if (bankVersion == SEQUENCE_BANK_CURRENT_VERSION) {
    	return false;
    } else {
        return true;
    }
}

void SequenceBank::loadSequence(const struct PFM3File* bank, int patchNumber) {
    if (!isInitialized_) {
        initFiles();
    }

    const char* fullSeqBankName = getFullName(bank->name);
    uint32_t bankVersion = 0;
    UINT byteRead;
    if (f_open(&sequenceFile, fullSeqBankName, FA_READ) == FR_OK) {
        if (f_read(&sequenceFile, (void *)&bankVersion, 4, &byteRead) != FR_OK || byteRead != 4) {
            // Short/failed version-header read: skip version dispatch.
            bankVersion = 0;
        }

        switch (bankVersion) {
            case SEQUENCE_BANK_VERSION1:
                loadSequenceDataVersion1(&sequenceFile, patchNumber);
                break;
            case SEQUENCE_BANK_VERSION2:
                loadSequenceDataVersion2(&sequenceFile, patchNumber);
                break;
        }
        f_close(&sequenceFile);
    }
}

void SequenceBank::loadSequenceDataVersion1(FIL* sequenceFile, int patchNumber) {
    // folded-A: a truncated file must be rejected BEFORE any setFullState
    // mutation. Slot payload = 1024 state + 16384 actions + 12336 stepNotes.
    if (f_size(sequenceFile) < 4 + (FSIZE_t)(1024 + 16384 + 12336) * (patchNumber + 1)) {
        return;
    }
    UINT byteRead;
    // Review patch: an unchecked seek failure would leave reads continuing
    // from the wrong offset — exact-length reads then "succeed" on
    // wrong-slot bytes and mutate state. Abort before any read.
    if (f_lseek(sequenceFile, 4 + ((1024 + 16384 + 12336) * patchNumber)) != FR_OK) {
        return;
    }

    // Read every section into staging storage. No live sequencer state changes
    // until all exact-length reads have succeeded.
    for (int i = 0; i < 1024; i++) {
        storageBuffer[i] = 0;
    }

    if (f_read(sequenceFile, storageBuffer, 1024, &byteRead) != FR_OK || byteRead != 1024) {
        return;
    }
    if (f_read(sequenceFile, stagedActions, sizeof(stagedActions), &byteRead) != FR_OK
            || byteRead != sizeof(stagedActions)) {
        return;
    }
    constexpr UINT version1StepSize = 12336;
    if (f_read(sequenceFile, stagedStepNotes, version1StepSize, &byteRead) != FR_OK
            || byteRead != version1StepSize) {
        return;
    }

    __builtin_memcpy(actions, stagedActions, sizeof(stagedActions));
    // V1 intentionally updates only its six-sequence payload span; preserve
    // the historical behavior for the remaining V2-only live table bytes.
    __builtin_memcpy(stepNotes, stagedStepNotes, version1StepSize);
    sequencer->setFullState((uint8_t*)storageBuffer);
    // 8.1-H4: the action block is an unvalidated RAM snapshot — a chain
    // corrupted before the save (head-bypass links, zeroed sentinels)
    // reloads verbatim and freezes the SysTick walk on first play. Heal per
    // instrument: malformed chains reset to empty instead of playing.
    for (int t = 0; t < NUMBER_OF_TIMBRES; t++) {
        sequencer->validateActionList(t);
    }
}

void SequenceBank::loadSequenceDataVersion2(FIL* sequenceFile, int patchNumber) {
    // folded-A: a truncated file must be rejected BEFORE any setFullState
    // mutation. Slot payload = 1024 state + 16384 actions + 24576 stepNotes.
    if (f_size(sequenceFile) < 4 + (FSIZE_t)(1024 + 16384 + 24576) * (patchNumber + 1)) {
        return;
    }
    UINT byteRead;
    // Review patch: same checked-seek contract as the v1 loader.
    if (f_lseek(sequenceFile, 4 + ((1024 + 16384 + 24576) * patchNumber)) != FR_OK) {
        return;
    }

    for (int i = 0; i < 1024; i++) {
        storageBuffer[i] = 0;
    }

    if (f_read(sequenceFile, storageBuffer, 1024, &byteRead) != FR_OK || byteRead != 1024) {
        return;
    }
    if (f_read(sequenceFile, stagedActions, sizeof(stagedActions), &byteRead) != FR_OK
            || byteRead != sizeof(stagedActions)) {
        return;
    }
    if (f_read(sequenceFile, stagedStepNotes, sizeof(stagedStepNotes), &byteRead) != FR_OK
            || byteRead != sizeof(stagedStepNotes)) {
        return;
    }

    __builtin_memcpy(actions, stagedActions, sizeof(stagedActions));
    __builtin_memcpy(stepNotes, stagedStepNotes, sizeof(stagedStepNotes));
    sequencer->setFullState((uint8_t*)storageBuffer);
    // 8.1-H4: same heal as the v1 loader — validate every instrument's
    // restored chain before any playback can walk it.
    for (int t = 0; t < NUMBER_OF_TIMBRES; t++) {
        sequencer->validateActionList(t);
    }
}



const char* SequenceBank::loadSequenceName(const struct PFM3File* bank, int patchNumber) {
    if (!isInitialized_) {
        initFiles();
    }

    const char* fullSeqBankName = getFullName(bank->name);
    uint32_t bankVersion = 0;
    UINT byteRead;

    if (f_open(&sequenceFile, fullSeqBankName, FA_READ) == FR_OK) {
        if (f_read(&sequenceFile, (void *)&bankVersion, 4, &byteRead) != FR_OK || byteRead != 4) {
            // Short/failed version-header read: skip version dispatch.
            bankVersion = 0;
        }

        switch (bankVersion) {
            case SEQUENCE_BANK_VERSION1: {
                // folded-A: exact-length name read on a big-enough file;
                // any failure falls through to the "##" fallback.
                // Review patch: the name needs only its own extent
                // (4 + slotSize*N + 20) — a bank truncated mid-slot still
                // yields the readable name.
                if (f_size(&sequenceFile) >= 4 + (FSIZE_t)(1024 + 16384 + 12336) * patchNumber + 20
                        && f_lseek(&sequenceFile, 4 + (1024 + 16384 + 12336) * patchNumber) == FR_OK
                        && f_read(&sequenceFile, storageBuffer, 20, &byteRead) == FR_OK
                        && byteRead == 20) {
                    // NUL-aware copy: getSequenceNameInBuffer can return the
                    // 3-byte "##" literal fallback; a fixed 12-byte copy
                    // would read past it. Real 12-char names (space-padded,
                    // no NUL) copy identically.
                    const char* sequenceNameInBuffer = sequencer->getSequenceNameInBuffer(storageBuffer);
                    int s = 0;
                    while (s < 12 && sequenceNameInBuffer[s] != '\0') {
                        sequenceName[s] = sequenceNameInBuffer[s];
                        s++;
                    }
                    sequenceName[s] = 0;
                    sequenceName[12] = 0;
                    f_close(&sequenceFile);
                    return sequenceName;
                }
                break;
            }
            case SEQUENCE_BANK_VERSION2: {
                // Review patch: name's own extent, as in the v1 arm.
                if (f_size(&sequenceFile) >= 4 + (FSIZE_t)(1024 + 16384 + 24576) * patchNumber + 20
                        && f_lseek(&sequenceFile, 4 + (1024 + 16384 + 24576) * patchNumber) == FR_OK
                        && f_read(&sequenceFile, storageBuffer, 20, &byteRead) == FR_OK
                        && byteRead == 20) {
                    // NUL-aware copy, as in the v1 arm ("##" fallback bound).
                    const char* sequenceNameInBuffer = sequencer->getSequenceNameInBuffer(storageBuffer);
                    int s = 0;
                    while (s < 12 && sequenceNameInBuffer[s] != '\0') {
                        sequenceName[s] = sequenceNameInBuffer[s];
                        s++;
                    }
                    sequenceName[s] = 0;
                    sequenceName[12] = 0;
                    f_close(&sequenceFile);
                    return sequenceName;
                }
                break;
            }
        }
        // Every successful open must be paired with a close, including short,
        // failed, and unknown version-header fallback paths.
        f_close(&sequenceFile);
    }

    return "##";
}


void SequenceBank::createSequenceFile(const char* name) {
    if (!isInitialized_) {
        initFiles();
    }

    const struct PFM3File * newBank = addEmptyFile(name);
    const char* fullBankName = getFullName(name);
    UINT byteWritten;
    uint32_t seqStatesize;

    if (newBank == 0) {
        return;
    }

    FRESULT fatFSResult = f_open(&sequenceFile, fullBankName, FA_OPEN_ALWAYS | FA_WRITE);
    if (fatFSResult != FR_OK) {
        return;
    }

    uint32_t bankVersion = (uint32_t)SEQUENCE_BANK_CURRENT_VERSION;
    FRESULT headerResult = f_write(&sequenceFile, (void *)&bankVersion, 4, &byteWritten);
    // 6.5: a failed/short header write leaves a malformed bank — stop the
    // whole creation transaction instead of appending state blocks at a
    // shifted offset (same contract as the guarded zero-fill loop below).
    if (headerResult != FR_OK || byteWritten != 4) {
        f_close(&sequenceFile);
        return;
    }

    for (int i = 0; i < PROPERTY_FILE_SIZE; i++) {
        storageBuffer[i] = 0;
    }

    for (int s = 0; s < NUMBER_OF_SEQUENCES_PER_BANK; s++) {

        tft.setCharColor(COLOR_GRAY);
        tft.setCursor(15, 12);
        tft.print(s + 1);
        tft.print("/");
        tft.print(NUMBER_OF_SEQUENCES_PER_BANK);

        sequencer->getFullDefaultState((uint8_t*)storageBuffer, &seqStatesize, s + 1);
        // We save 1024 bytes for sequencer fullstate
        FRESULT stateResult = f_write(&sequenceFile, storageBuffer, 1024, &byteWritten);
        // 6.5: same bail as the header and the zero-fill loop — a short state
        // block would shift every later slot.
        if (stateResult != FR_OK || byteWritten != 1024) {
            f_close(&sequenceFile);
            return;
        }

        // we save the sizes of the current version
        int numberOfZeros = sizeof(actions) + sizeof(stepNotes);
        while (numberOfZeros > 0) {
            UINT toWrite = numberOfZeros > 1024 ? 1024 : numberOfZeros;
            FRESULT writeResult = f_write(&sequenceFile, storageBuffer + 1024, toWrite, &byteWritten);
            // Stop the entire creation transaction. Continuing the outer slot
            // loop would write every later slot at a shifted offset.
            if (writeResult != FR_OK || byteWritten != toWrite) {
                f_close(&sequenceFile);
                return;
            }
            numberOfZeros -= byteWritten;
#ifndef PFM3_HOST
            HAL_Delay(1);
#endif
        }
    }
    f_close(&sequenceFile);
}

/*
 * This save the sequence with the current version
 */
void SequenceBank::saveSequence(const struct PFM3File* sequencePFMFile, int sequenceNumber, char* sequenceName) {
    if (!isInitialized_) {
        initFiles();
    }

    const char* fullSeqBankName = getFullName(sequencePFMFile->name);

    sequencer->setSequenceName(sequenceName);

    if (f_open(&sequenceFile, fullSeqBankName, FA_WRITE) == FR_OK) {
        f_lseek(&sequenceFile, 4 + ((1024 + 16384 + 24576) * sequenceNumber));

        saveSequencerData(&sequenceFile);
        f_close(&sequenceFile);
    }
}

bool SequenceBank::saveDefaultSequence() {
    bool savedOK = true;
    UINT byteWritten;

    FRESULT fatFSResult = f_open(&sequenceFile, getFileName(DEFAULT_SEQUENCE), FA_OPEN_ALWAYS | FA_WRITE);
    if (fatFSResult == FR_OK) {
        uint32_t bankVersion = (uint32_t)SEQUENCE_BANK_CURRENT_VERSION;
        f_write(&sequenceFile, (void *)&bankVersion, 4, &byteWritten);

        savedOK = saveSequencerData(&sequenceFile);

        f_close(&sequenceFile);
    } else {
        savedOK = false;
    }

    return savedOK;
}

bool SequenceBank::loadDefaultSequence() {
	uint32_t bankVersion = 0;
    UINT byteRead;

    if (f_open(&sequenceFile, getFileName(DEFAULT_SEQUENCE), FA_READ) == FR_OK) {
        if (f_read(&sequenceFile, (void *)&bankVersion, 4, &byteRead) != FR_OK || byteRead != 4) {
            // Short/failed version-header read: skip version dispatch
            // (bankVersion was previously used UNINITIALIZED here on failure).
            bankVersion = 0;
        }

        switch (bankVersion) {
            case SEQUENCE_BANK_VERSION1:
                loadSequenceDataVersion1(&sequenceFile, 0);
                break;
            case SEQUENCE_BANK_VERSION2:
                loadSequenceDataVersion2(&sequenceFile, 0);
                break;
        }
        f_close(&sequenceFile);
    }

	return true;
}

void SequenceBank::removeDefaultSequence() {
    remove(DEFAULT_SEQUENCE);
}


bool SequenceBank::saveSequencerData(FIL* file) {
	uint32_t seqStateSize;
	UINT byteWritten;

    for (int i = 0; i < 1024; i++) {
        storageBuffer[i] = 0;
    }

    // We copy the state to property files
    sequencer->getFullState((uint8_t*)storageBuffer, &seqStateSize);

    // and save 1024 chars
    f_write(&sequenceFile, storageBuffer, 1024, &byteWritten);

    f_write(&sequenceFile, actions, 16384, &byteWritten);
    f_write(&sequenceFile, stepNotes, 24576, &byteWritten);

    return true;
}


