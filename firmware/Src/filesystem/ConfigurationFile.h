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


#ifndef CONFIGURATIONFILE_H_
#define CONFIGURATIONFILE_H_

#include "PreenFMFileType.h"
#include "ScalaFile.h"
#include "DX7SysexFile.h"

class ConfigurationFile: public PreenFMFileType {
public:
	ConfigurationFile();
	virtual ~ConfigurationFile();


	void loadConfig(uint8_t* midiConfigBytes);
	void saveConfig(uint8_t* midiConfigBytes);

	// Lets the config parser read/write the DX7 folder picker state.
	void setDX7SysexFile(DX7SysexFile *dx7SysexFile) { dx7SysexFile_ = dx7SysexFile; }

protected:
	const char* getFolderName();
	bool isCorrectFile(char *name, int size) { return true; }

	void fillMidiConfig(uint8_t* midiConfigBytes, char* line);

private:
	DX7SysexFile *dx7SysexFile_;
};

#endif /* CONFIGURATIONFILE_H_ */
