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

#ifndef LFOOSC_H_
#define LFOOSC_H_

#include "Lfo.h"
#include "Osc.h"



class LfoOsc: public Lfo {
public:
    virtual ~LfoOsc() {};

	void init(struct LfoParams *lfoParams, float* lfoPhase, Matrix* matrix, SourceEnum source, DestinationEnum dest);

	void valueChanged(int encoder) {
	    switch (encoder) {
	    case ENCODER_LFO_KSYNC: {
            // 6.3: keybRamp can be negative (UI "off" is -0.01) or exceed the
            // display's [0,2] range (PAD random presets reach 4.0, the DX7
            // import ~6.6). The raw (int)(keybRamp*50) indexes invTab[2048]
            // out of bounds for anything below -0.02 (and the cast itself is
            // UB for NaN/huge values). Guard before the cast — folded-B
            // idiom — then clamp: hostile ramps behave like ramp 0.
            // Valid ramps are byte-identical.
            float keybRamp = lfo->keybRamp;
            int rampIndex = (__builtin_isfinite(keybRamp) && keybRamp > 0.0f)
                ? (int)(keybRamp * 50.0f) : 0;
            if (rampIndex > 2047) {
                rampIndex = 2047;
            }
            this->rampInv = 50 * invTab[rampIndex];
            this->ramp = keybRamp;
            if (this->ramp < 0 ) {
                // resync all LFO
                phase = 0;
            }
            break;
        }
	    case ENCODER_LFO_FREQ:
	        isNotMidiSynchronized = ((lfo->freq * 10.0f) < LFO_MIDICLOCK_MC_DIV_16);
	        break;
	    }
	}


	void midiClock(int songPosition, bool computeStep);

	void nextValueInMatrix();

	void noteOn();

	void noteOff() {
		// Nothing to do
	}



private:
	LfoType type;
	LfoParams* lfo ;
    float currentRamp, ramp, rampInv;
    float phase;
    float* initPhase;
    DestinationEnum destination;
    float currentRandomValue;
    float nextRandomValue = 0;
    float noiseLp = 0;
    float currentFreq ;
    //
    bool isNotMidiSynchronized;

};

#endif /* LFOOSC_H_ */
