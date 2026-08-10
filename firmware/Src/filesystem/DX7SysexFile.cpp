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


#include "DX7SysexFile.h"

__attribute__((section(".ram_d2b")))  uint8_t dx7PackedPatch[DX7_PACKED_PATCH_SIZED];
__attribute__((section(".ram_d2b"))) struct PFM3File dx7BankAlloc[NUMBEROFDX7BANKS];
__attribute__((section(".ram_d2b"))) struct PFM3File dx7SubDirAlloc[NUMBEROFDX7SUBDIRS];


// Copy a NUL-terminated string literal without depending on fsu_ (which is not
// attached yet when the DX7SysexFile constructor runs).
static void copyLiteral(char *dest, const char *src, int max) {
    int k = 0;
    while (k < max - 1 && src[k] != 0) {
        dest[k] = src[k];
        k++;
    }
    dest[k] = 0;
}

DX7SysexFile::DX7SysexFile() {
	numberOfFilesMax_ = NUMBEROFDX7BANKS;
    dx7Bank = dx7BankAlloc;
	myFiles_ = dx7Bank;

	dx7SubDirs = dx7SubDirAlloc;
	dx7SubDirCount_ = 0;

	// Persisted cursor staging (overwritten by loadConfig if Settings.txt
	// carries dx7bank/dx7preset; stays 0 on an old/missing file).
	lastBank_ = 0;
	lastPreset_ = 0;

	// Default DX7 root + active folder = DX7_DIR (config can override later).
	copyLiteral(root_, DX7_DIR, sizeof(root_));
	copyLiteral(currentDir_, DX7_DIR, sizeof(currentDir_));
	selectedSubDir_[0] = 0;
}

DX7SysexFile::~DX7SysexFile() {
}

const char* DX7SysexFile::getFolderName() {
	return currentDir_;
}

uint8_t* DX7SysexFile::dx7LoadPatch(const struct PFM3File* bank, int patchNumber) {
	const char* fullBankName = getFullName(bank->name);
    int result = load(fullBankName, patchNumber * DX7_PACKED_PATCH_SIZED + 6,  (void*)dx7PackedPatch, DX7_PACKED_PATCH_SIZED);
    if (result != DX7_PACKED_PATCH_SIZED) {
    	return (uint8_t*)0;
    }
    return dx7PackedPatch;
}

bool DX7SysexFile::isCorrectFile(char *name, int size)  {
	// DX7 Dump sysex size is 4104
	if (size != 4104) {
		return false;
	}

	int pointPos = -1;
    for (int k=1; k<9 && pointPos == -1; k++) {
        if (name[k] == '.') {
            pointPos = k;
        }
    }
    if (pointPos == -1) return false;
    if (name[pointPos+1] != 's' && name[pointPos+1] != 'S') return false;
    if (name[pointPos+2] != 'y' && name[pointPos+2] != 'Y') return false;
    if (name[pointPos+3] != 'x' && name[pointPos+3] != 'X') return false;

    return true;
}


// --- DX7 folder picker (E-picker β) ---------------------------------------

// Rebuild currentDir_ = root_ [+ '/' + selectedSubDir_] and force the bank
// list to be re-enumerated from the new folder on next access.
void DX7SysexFile::rebuildCurrent() {
    int k = 0;
    while (k < (int)sizeof(currentDir_) - 1 && root_[k] != 0) {
        currentDir_[k] = root_[k];
        k++;
    }
    if (selectedSubDir_[0] != 0) {
        if (k < (int)sizeof(currentDir_) - 1) {
            currentDir_[k++] = '/';
        }
        int j = 0;
        while (k < (int)sizeof(currentDir_) - 1 && selectedSubDir_[j] != 0) {
            currentDir_[k++] = selectedSubDir_[j];
            j++;
        }
    }
    currentDir_[k] = 0;
    isInitialized_ = false;
}

void DX7SysexFile::setRoot(const char* root) {
    if (root == 0 || root[0] == 0) {
        return;
    }
    copyLiteral(root_, root, sizeof(root_));
    rebuildCurrent();
}

// Called from config load (dx7current). Empty/absent value => active = root.
void DX7SysexFile::applySelectedSubDir(const char* subDirName) {
    int k = 0;
    if (subDirName != 0) {
        while (k < (int)sizeof(selectedSubDir_) - 1 && subDirName[k] != 0) {
            selectedSubDir_[k] = subDirName[k];
            k++;
        }
    }
    selectedSubDir_[k] = 0;
    rebuildCurrent();
}

// Flat-root auto-skip: active folder = root itself.
// Returns true when the active folder changed (a subfolder was previously
// selected), so the menu can keep the bank/preset cursor on re-entry.
bool DX7SysexFile::selectRoot() {
    bool changed = selectedSubDir_[0] != 0;
    selectedSubDir_[0] = 0;
    rebuildCurrent();
    return changed;
}

// User picked subfolder `index` from the picker list.
// Returns true when the active folder changed. The comparison snapshots the
// full selectedSubDir_ buffer (not just up to the new name's length) so a
// re-select of the same folder is detected even with 12-char truncation.
bool DX7SysexFile::selectSubDir(int index) {
    if (index < 0 || index >= dx7SubDirCount_) {
        return selectRoot();
    }

    char prev[sizeof(selectedSubDir_)];
    for (int p = 0; p < (int)sizeof(prev); p++) {
        prev[p] = selectedSubDir_[p];
    }

    int k = 0;
    while (k < (int)sizeof(selectedSubDir_) - 1 && dx7SubDirs[index].name[k] != 0) {
        selectedSubDir_[k] = dx7SubDirs[index].name[k];
        k++;
    }
    selectedSubDir_[k] = 0;

    bool changed = false;
    for (int p = 0; p < (int)sizeof(prev); p++) {
        if (prev[p] != selectedSubDir_[p]) {
            changed = true;
            break;
        }
    }

    rebuildCurrent();
    return changed;
}

int DX7SysexFile::initSubDirs() {
    dx7SubDirCount_ = enumerateSubDirs(root_, dx7SubDirs, NUMBEROFDX7SUBDIRS);
    return dx7SubDirCount_;
}

const struct PFM3File* DX7SysexFile::getSubDir(int index) {
    if (index < 0 || index >= dx7SubDirCount_) {
        return &errorFile_;
    }
    return &dx7SubDirs[index];
}
