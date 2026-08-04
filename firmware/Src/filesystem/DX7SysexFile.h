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


#ifndef DX7SYSEXFILE_H_
#define DX7SYSEXFILE_H_

#include "PreenFMFileType.h"

class DX7SysexFile: public PreenFMFileType {
public:
	DX7SysexFile();
	virtual ~DX7SysexFile();

	uint8_t* dx7LoadPatch(const struct PFM3File* bank, int patchNumber);

	// --- DX7 folder picker (E-picker β) -----------------------------------
	// Root is the top of the DX7 library (configurable via dx7bankdir, default
	// DX7_DIR). The picker lists the root's immediate subfolders; the chosen
	// folder becomes the active read folder (currentDir_). A flat root with no
	// subfolders reads .syx directly from the root (today's behaviour).
	void setRoot(const char* root);
	const char* getRoot() { return root_; }
	void applySelectedSubDir(const char* subDirName);
	const char* getSelectedSubDir() { return selectedSubDir_; }
	void selectRoot();
	void selectSubDir(int index);
	int initSubDirs();
	int getSubDirCount() { return dx7SubDirCount_; }
	const struct PFM3File* getSubDir(int index);

protected:
	const char* getFolderName();
	bool isCorrectFile(char *name, int size);
	struct PFM3File *dx7Bank;

private:
	void rebuildCurrent();
	char root_[24];
	char currentDir_[40];
	char selectedSubDir_[13];
	struct PFM3File *dx7SubDirs;
	int dx7SubDirCount_;
};

#endif /* DX7SYSEXFILE_H_ */
