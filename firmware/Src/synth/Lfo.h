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

#ifndef LFO_H_
#define LFO_H_

#include "SynthStateAware.h"
#include "Matrix.h"



#define NUMBER_OF_LFO_OSC 3
#define NUMBER_OF_LFO_ENV 1
#define NUMBER_OF_LFO_ENV2 1
#define NUMBER_OF_LFO_STEP 2

#define NUMBER_OF_LFO NUMBER_OF_LFO_OSC+NUMBER_OF_LFO_ENV+NUMBER_OF_LFO_ENV2+NUMBER_OF_LFO_STEP

class Lfo {
public:
	Lfo();
	virtual ~Lfo() {}
	/*
	 * LFO init() overload family (8.8 SW3 decision, recorded 2026-09-17):
	 * the derived classes (LfoOsc/LfoEnv/LfoEnv2/LfoStepSeq) each declare an
	 * `init` with their own params-struct signature. Those overloads are NOT
	 * overrides — they deliberately name-hide this base overload — and no
	 * production code dispatches init() through a base Lfo pointer or
	 * reference: the only
	 * initializer call site is Voice::setCurrentTimbre, which calls the
	 * concrete objects directly; each derived init explicitly calls
	 * Lfo::init(...) for the shared matrix-row setup. The derived headers
	 * re-expose this base overload via `using Lfo::init;` so both signatures
	 * participate in derived-name lookup (documenting the family instead of
	 * leaving the hiding implicit). This is a style/name-visibility record,
	 * NOT a virtual-dispatch repair: nothing overrides this virtual today.
	 */
	virtual void init(Matrix* matrix, SourceEnum source, DestinationEnum dest);

	/*
	 * All calls to these functions are currently type-specific, and the
	 * virtual function calls have a high overhead (~500 cycles total per
	 * buildNewSampleBlock). The savings may be due to inlining which is
	 * (usually) not possible for virtual functions
	virtual void valueChanged(int encoder) = 0;
	virtual void nextValueInMatrix() = 0 ;
	virtual void noteOn() = 0;
	virtual void noteOff() = 0;
	virtual void midiClock(int songPosition, bool computeStep) = 0;
	*/

	void midiContinue() {
		ticks = 0;
	}


protected:
	Matrix *matrix;
    uint8_t destination;
    uint8_t source;
	// Midi Clock sync
	int ticks;

	// Precalcul 1/freq
    static int initTab;
    static float invTab[2048];
};

#endif /* LFO_H_ */
