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


#include "ConfigurationFile.h"
#include "StorageSizes.h"
#include "Menu.h"

extern char lineBuffer[PFM3_LINE_BUFFER_SIZE];

ConfigurationFile::ConfigurationFile() {
	numberOfFilesMax_ = 0;
	dx7SysexFile_ = 0;
}

ConfigurationFile::~ConfigurationFile() {
}

const char* ConfigurationFile::getFolderName() {
	return PREENFM_DIR;
}


void ConfigurationFile::loadConfig(uint8_t* midiConfigBytes) {
	char *line = lineBuffer;
    char* reachableProperties = storageBuffer;
    int size = checkSize(PROPERTIES);
    if (size >= PROPERTY_FILE_SIZE || size == -1) {
    	// ERROR
    	return;
    }
    reachableProperties[size] = 0;

    int result = load(PROPERTIES, 0,  reachableProperties, size);
    if (result != size) {
        // Partial / failed read: don't parse a half-filled buffer (would apply garbage settings).
        return;
    }
    int loop = 0;
    char *readProperties = reachableProperties;
    while (loop !=-1 && (readProperties - reachableProperties) < size) {
    	loop = fsu_->getLine(readProperties, line);
    	if (line[0] != '#') {
    		fillMidiConfig(midiConfigBytes, line);
    		// DX7 folder picker (string keys, not part of midiConfig)
    		if (dx7SysexFile_ != 0) {
    			int dx7EqualPos = fsu_->getPositionOfEqual(line);
    			if (dx7EqualPos != -1) {
    				char dx7Key[21];
    				char dx7Value[21];
    				fsu_->getKey(line, dx7Key);
    				fsu_->getTextValue(line + dx7EqualPos + 1, dx7Value);
    				if (fsu_->str_cmp(dx7Key, "dx7bankdir") == 0) {
    					dx7SysexFile_->setRoot(dx7Value);
    				} else if (fsu_->str_cmp(dx7Key, "dx7current") == 0) {
    					dx7SysexFile_->applySelectedSubDir(dx7Value);
    				} else if (fsu_->str_cmp(dx7Key, "dx7bank") == 0) {
    					dx7SysexFile_->setLastBank((uint16_t)fsu_->toInt(dx7Value));
    				} else if (fsu_->str_cmp(dx7Key, "dx7preset") == 0) {
    					dx7SysexFile_->setLastPreset((uint8_t)fsu_->toInt(dx7Value));
    				}
    			}
    		}
    	}
    	readProperties += loop;
    }
}


void ConfigurationFile::saveConfig(uint8_t* midiConfigBytes) {
    int wptr = 0;
    for (int k=0; k<MIDICONFIG_SIZE; k++) {
    	storageBuffer[wptr++] = '#';
    	storageBuffer[wptr++] = ' ';
    	wptr += fsu_->copy_string((char*)storageBuffer + wptr, midiConfig[k].title);
    	storageBuffer[wptr++] = '\n';
    	if (midiConfig[k].maxValue < 10 && midiConfig[k].valueName != 0) {
	    	wptr += fsu_->copy_string((char*)storageBuffer + wptr, "#   0=");
			for (int o=0; o<midiConfig[k].maxValue; o++) {
		    	wptr += fsu_->copy_string((char*)storageBuffer + wptr, midiConfig[k].valueName[o]);
				if ( o != midiConfig[k].maxValue - 1) {
			    	wptr += fsu_->copy_string((char*)storageBuffer + wptr, ", ");
				} else {
			    	storageBuffer[wptr++] = '\n';
				}
			}
    	}
    	wptr += fsu_->copy_string((char*)storageBuffer + wptr, midiConfig[k].nameInFile);
    	storageBuffer[wptr++] = '=';
    	wptr += fsu_->printInt((char*)storageBuffer + wptr, (int)midiConfigBytes[k]);
    	storageBuffer[wptr++] = '\n';
    	storageBuffer[wptr++] = '\n';
    }

    // DX7 folder picker state (round-trips so it survives the full rewrite).
    // Guard the block so it can never push wptr past the buffer even on a pathological file.
    if (dx7SysexFile_ != 0 && wptr < PROPERTY_FILE_SIZE - 120) {
        wptr += fsu_->copy_string((char*)storageBuffer + wptr, "# DX7 folder picker");
        storageBuffer[wptr++] = '\n';
        wptr += fsu_->copy_string((char*)storageBuffer + wptr, "dx7bankdir=");
        wptr += fsu_->copy_string((char*)storageBuffer + wptr, dx7SysexFile_->getRoot());
        storageBuffer[wptr++] = '\n';
        wptr += fsu_->copy_string((char*)storageBuffer + wptr, "dx7current=");
        wptr += fsu_->copy_string((char*)storageBuffer + wptr, dx7SysexFile_->getSelectedSubDir());
        storageBuffer[wptr++] = '\n';
        wptr += fsu_->copy_string((char*)storageBuffer + wptr, "dx7bank=");
        wptr += fsu_->printInt((char*)storageBuffer + wptr, (int)dx7SysexFile_->getLastBank());
        storageBuffer[wptr++] = '\n';
        wptr += fsu_->copy_string((char*)storageBuffer + wptr, "dx7preset=");
        wptr += fsu_->printInt((char*)storageBuffer + wptr, (int)dx7SysexFile_->getLastPreset());
        storageBuffer[wptr++] = '\n';
    }
    // delete it so that we're sure the new one has the right size...
    remove(PROPERTIES);
    save(PROPERTIES, 0,  storageBuffer, wptr);
}

void ConfigurationFile::saveConfigWithDx7(uint8_t* midiConfigBytes, uint16_t dx7Bank, uint8_t dx7Preset) {
    // Refresh staging from the live cursor immediately before the full-file
    // rewrite, otherwise saveConfig would persist whatever was loaded at boot
    // (dx7bank/dx7preset only change through the menu, never through staging).
    if (dx7SysexFile_ != 0) {
        dx7SysexFile_->setLastBank(dx7Bank);
        dx7SysexFile_->setLastPreset(dx7Preset);
    }
    saveConfig(midiConfigBytes);
}



void ConfigurationFile::fillMidiConfig(uint8_t* midiConfigBytes, char* line) {
	char key[21];
	char value[21];

	int equalPos = fsu_->getPositionOfEqual(line);
	if (equalPos == -1) {
		return;
	}
	fsu_->getKey(line, key);

	for (int k=0; k < MIDICONFIG_SIZE; k++) {
		if (fsu_->str_cmp(key, midiConfig[k].nameInFile) == 0) {
			fsu_->getValue(line + equalPos+1, value);
			midiConfigBytes[k] = fsu_->toInt(value);
			return;
		}
	}
}
