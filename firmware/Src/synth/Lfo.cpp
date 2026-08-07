/*
 * Copyright 2013 Xavier Hosxe
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

#include "Lfo.h"


// Standard 128kb memory
// `.ram_d1` names an STM32H7 SRAM-D1 region via the Arm linker script; the
// attribute is a hard ERROR on Mach-O (macOS host) and irrelevant to host
// semantics, so drop it under PFM3_HOST. The DEFINITION is preserved. Inert
// under the Arm build. See tests/SEAM.md (Target #1 appendix Correction 2).
#ifndef PFM3_HOST
__attribute__((section(".ram_d1")))
#endif
float Lfo::invTab[2048];
int Lfo::initTab = 0;

Lfo::Lfo() {
}

void Lfo::init(Matrix *matrix, SourceEnum source, DestinationEnum dest) {
    this->destination = (uint8_t)dest;
    this->source = (uint8_t)source;
	this->matrix = matrix;

    if (initTab == 0) {
        initTab = 1;
        for (int k=1; k<2048; k++) {
            invTab[k] = 1.0f / ((float)k);
        }
        invTab[0] = 1.0;
    }
}
