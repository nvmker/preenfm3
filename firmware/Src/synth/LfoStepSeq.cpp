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

#include "LfoStepSeq.h"
#include "Osc.h"

float expValues[] = {0, 0.0594630944, 0.1224620483, 0.189207115, 0.2599210499, 0.3348398542, 0.4142135624, 0.4983070769,
        0.587401052, 0.6817928305, 0.7817974363, 0.8877486254, 1, 1.1189261887, 1.2449240966, 1.37841423 };

void LfoStepSeq::init(struct StepSequencerParams* stepSeqParam, struct StepSequencerSteps *stepSeqSteps, Matrix *matrix, SourceEnum source, DestinationEnum dest) {
	Lfo::init(matrix, source, (DestinationEnum)0);
    this->seqParams = stepSeqParam;
    this->seqSteps = stepSeqSteps;
    this->matrixGateDestination = dest;
	gated = false;
	valueChanged(0);
	ticks = 1536;
	midiClock(0, true);
}

void LfoStepSeq::midiClock(int songPosition, bool computeStep) {

	ticks &= 0x7ff;

    // Folded-B: a corrupt bank can hold a NaN/-Inf/+Inf bpm. The float->int
    // cast is UB for those, so reject non-finite values up front and route
    // them to the default/TIME_4 fail-safe (same as unmatched 246-255).
    // Review patch: a finite-but-out-of-int-range bpm (1e30f) is also UB in
    // the cast, so the guard covers the whole enum domain [0,255].
    float bpm = this->seqParams->bpm;
    int bpmInt = (__builtin_isfinite(bpm) && bpm >= 0.0f && bpm <= 255.0f)
        ? (int)bpm : LFO_SEQ_MIDICLOCK_TIME_4;
    switch (bpmInt)  {
	case LFO_SEQ_MIDICLOCK_DIV_4:
		// Midi Clock  / 4
		if ((songPosition & 0x1)==0) {
			if (computeStep) {
                phaseStep = 0.5f * invTab[ticks];
                ticks = 0;
			}
            phase = (float)(songPosition & 0x3f) * .25f;
		}
		break;
	case LFO_SEQ_MIDICLOCK_DIV_2:
		if ((songPosition & 0x1)==0) {
			if (computeStep) {
                phaseStep = 1.0f * invTab[ticks];
				ticks = 0;
			}
			phase = (float)(songPosition & 0x1f) * .5f;
		}
		break;
	case LFO_SEQ_MIDICLOCK:
		if ((songPosition & 0x1)==0) {
			if (computeStep) {
			    // We're called evey 2 ticks which is half a quarter
			    // We want to move forward 4 beats per quarter so : 2 beats per half a quarter
                phaseStep = 2.0f * invTab[ticks];
				ticks = 0;
			}
			phase = (songPosition & 0xF);
		}
		break;
	case LFO_SEQ_MIDICLOCK_TIME_2:
		if ((songPosition & 0x1)==0) {
			if (computeStep) {
                phaseStep = 4.0f * invTab[ticks];
				ticks = 0;
			}
			phase = ((songPosition << 1) & 0xF);
		}
		break;
	case LFO_SEQ_MIDICLOCK_TIME_4:
	default:
		// Corrupt/unmatched sync values (e.g. 246-255 from a bad bank)
		// fall back to the nearest supported division (TIME_4). Values
		// below the sync range are free-run: keep the valueChanged rate.
		if (bpmInt < LFO_SEQ_MIDICLOCK_TIME_4) {
			break;
		}
		if ((songPosition & 0x1)==0) {
			if (computeStep) {
                phaseStep = 8.0f * invTab[ticks];
				ticks = 0;
			}
            phase = ((songPosition << 1) & 0xF);
		}
		break;
	}
}


void LfoStepSeq::valueChanged(int encoder) {
	if (encoder < 2) {
		if (seqParams->bpm < LFO_SEQ_MIDICLOCK_DIV_4 ) {
		    // bpm / 60 = Herz
		    // We have 4 tick per beat so * 4
		    // 4/60 = .066666
			phaseStep = seqParams->bpm * 0.066666666666666f * PREENFM_FREQUENCY_INVERSED_LFO;
		}
	}
}

void LfoStepSeq::nextValueInMatrix() {

	ticks++;

	phase += phaseStep;
	int phaseInteger = phase;
	phase -= phaseInteger;
	float phaseDecimal = phase;
	phaseInteger &= 0xf; // modulo 16
	phase += phaseInteger;

	// Add gate and matrix value
	float gatePlusMatrix = seqParams->gate + (this->matrix->getDestination(matrixGateDestination));

	// We'll reach the new value phaseStep by phaseStep to reduce audio click !
	if (gatePlusMatrix <= 0) {
		target = 0;
	} else if (gatePlusMatrix < 1.0) {
		// Gated ?
		if (!gated && phaseDecimal > gatePlusMatrix) {
			target = 0;
			gated = true;
		}
		// End of gate ?
		if (gated && phaseDecimal < gatePlusMatrix) {
			target = stepValue(seqSteps->steps[(int)phase]); // 16 steps
			gated = false;
		}
	} else {
		target = stepValue(seqSteps->steps[(int)phase]);
	}

	if (currentValue < target) {
		currentValue++;
	}
	if (currentValue > target) {
		currentValue--;
	}
	//matrix->setSource(source, expValues[currentValue]);
	matrix->setSource((enum SourceEnum)source, expValues[currentValue]);
}

int LfoStepSeq::stepValue(int step) {
	// 6.2: steps[] holds signed bytes from the preset — the UI clamps to [0,15]
	// but a corrupt bank does not; a hostile value (<0 or >15) used to index
	// expValues[16] out of bounds. Clamp the step VALUE at the use site (4.5
	// fallback idiom): a corrupt step fails safe to an in-table value.
	if (step < 0) {
		return 0;
	}
	if (step > 15) {
		return 15;
	}
	return step;
}

void LfoStepSeq::noteOn() {
	if (seqParams->bpm < LFO_SEQ_MIDICLOCK_DIV_4) {
		phase = 0;
		target = stepValue(seqSteps->steps[0]);
	}
}

void LfoStepSeq::noteOff() {
}

