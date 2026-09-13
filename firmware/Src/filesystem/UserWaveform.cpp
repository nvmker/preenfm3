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

 #include "LiquidCrystal.h"
extern LiquidCrystal      lcd;
*/

#include "StorageSizes.h"

// User waveforms
extern float userWaveform[6][1024];

extern char lineBuffer[PFM3_LINE_BUFFER_SIZE];
#define LINE_BUFFER_SIZE 512

extern char *oscShapeNames[];
extern struct WaveTable waveTables[];

#include "UserWaveform.h"

#include <math.h>   // lround (was (int)(x + .5f) -- bugprone-incorrect-roundings)
#include <string.h>  // memcmp (8.1 v2 magic gate; no alignment-sensitive casts)

UserWaveform::UserWaveform() {
    for (int k=0; k<6; k++) {
        userWaveFormNames[k][4] = 0;
    }
}

UserWaveform::~UserWaveform() {
}

const char* UserWaveform::getFolderName() {
    return USERWAVEFORM_DIR;
}


void UserWaveform::loadUserWaveforms() {
    char fileName[30];


    for (int f = 0; f < 6; f++) {
        // Check if bin exists

        fsu_->copy_string(fileName, USERWAVEFORM_FILENAME_BIN);
        fileName[20] = (char)('1' + f);

        int sizeBin = checkSize(fileName);

        // 8.1 (B5): the bin cache is now versioned by a magic header.
        // loadUserWaveformFromBin returns 0 = legacy layout (no magic):
        // untrustworthy — an already-poisoned cache is count/size-
        // indistinguishable — so fall through to the txt path below and let
        // the v2 save replace the same file in place (one-shot migration).
        // 1 = v2 loaded; -1 = v2 rejected (numberOfSampleError already
        // applied — NO txt fallback). Legacy-without-txt takes the no-file
        // default (zeros).
        bool tryTxt = true;
        if (sizeBin != -1) {
            tryTxt = (loadUserWaveformFromBin(f, fileName) == 0);
        }
        if (tryTxt) {
            fsu_->copy_string(fileName, USERWAVEFORM_FILENAME_TXT);
            fileName[20] = (char)('1' + f);

            int sizeTxt = checkSize(fileName);
            if (sizeTxt == -1) {
                // Does not exist. Neither Bin nor txt => Silence
                for (int s = 0; s < 1024; s++) {
                    userWaveform[f][s] = 0.0f;
                }
            } else {
                numberOfSample = -1;
                userWaveFormNames[f][0] = 0;
                loadUserWaveformFromTxt(f, fileName, sizeTxt);

                if (numberOfSample > 0) {
                    if (numberOfSample > 512 && numberOfSample < 1024) {
                        interpolate(userWaveform[f], numberOfSample, 1024);
                    } else if (numberOfSample > 256 && numberOfSample < 512) {
                        interpolate(userWaveform[f], numberOfSample, 512);
                    } else if (numberOfSample > 128 && numberOfSample < 256) {
                        interpolate(userWaveform[f], numberOfSample, 256);
                    } else if (numberOfSample > 64 && numberOfSample < 128) {
                        interpolate(userWaveform[f], numberOfSample, 128);
                    } else if (numberOfSample > 32 && numberOfSample < 64) {
                        interpolate(userWaveform[f], numberOfSample, 64);
                    }
                    // 8.1 review: publish max only after the whole txt
                    // parsed AND interpolation settled (moved out of the
                    // fill count-init): a truncated file must leave
                    // waveTables untouched. The bin reload below re-sets it
                    // together with precomputedValue.
                    waveTables[f + 8].max = (numberOfSample  - 1);

                    normalize(userWaveform[f], numberOfSample);

                    fsu_->copy_string(fileName, USERWAVEFORM_FILENAME_BIN);
                    fileName[20] = (char)('1' + f);
                    saveUserWaveformToBin(f, fileName);
                    // Reload from Bin
                    loadUserWaveformFromBin(f, fileName);
                }
            }
        }
    }
}

void UserWaveform::loadUserWaveformFromTxt(int f, const char* fileName, int size) {
    int readIndex = 0;
    floatRead = 0;


    while (floatRead != numberOfSample) {
        // 8.1 (B5): EOF guards. A truncated txt (valid declared count, fewer
        // floats) used to keep looping over stale lineBuffer bytes past the
        // populated part until the count was reached — the poisoned table
        // was then normalized and persisted as the bin cache. Reject
        // through numberOfSampleError BEFORE parsing anything stale.
        if (readIndex >= size) {
            numberOfSampleError(f);
            return;
        }
        int toRead = (size - readIndex > LINE_BUFFER_SIZE) ? LINE_BUFFER_SIZE : size - readIndex;
        if (toRead <= 0) {
            numberOfSampleError(f);
            return;
        }
        if (load(fileName, readIndex,  (void*)&lineBuffer, toRead) != toRead) {
            numberOfSampleError(f);
            return;
        }
        // NUL-terminate the populated bytes: stof stops at NUL and
        // isSeparator(NUL) is false, so every parse/skip loop in the fill
        // routine is structurally bounded. Reads are <= LINE_BUFFER_SIZE
        // (512) into the 1024-byte lineBuffer — always room for the NUL.
        lineBuffer[toRead] = 0;

        int used = fillUserWaveFormFromTxt(f, lineBuffer, toRead, (readIndex+toRead) >= size);
        if (used < 0) {
            return;  // fill already routed through numberOfSampleError
        }
        if (used == 0 || used > toRead) {
            numberOfSampleError(f);
            return;
        }
        readIndex += used;
    }
}


int UserWaveform::fillUserWaveFormFromTxt(int f, char* buffer, int filled, bool last) {
    int index = 0;
    bool bStop = false;
    while (!bStop) {
        int floatSize = 0;

        // Init name
        if (userWaveFormNames[f][0] == 0) {

            while (fsu_->isSeparator(buffer[index])) {
                index++;
            }
            int b = 0;
            // 8.1 review (B5): NUL is not a separator — without this bound
            // a name cut by the chunk edge copied the NUL and then up to 3
            // STALE bytes past the populated region into the name.
            while (b<4 && buffer[index] != 0 && !fsu_->isSeparator(buffer[index])) {
                userWaveFormNames[f][b++] = buffer[index++];
            }
            // complete with space
            while (b<4) {
                userWaveFormNames[f][b++] = ' ';
            }
        }
        // init number of sample
        if (numberOfSample <= 0) {
            while (fsu_->isSeparator(buffer[index])) {
                index++;
            }
            numberOfSample = (int) lround(fsu_->stof(&buffer[index], floatSize));

            if (numberOfSample < 32 || numberOfSample > 1024) {
                return numberOfSampleError(f);
            }
            // 8.1 review: waveTables[f+8].max used to be published here,
            // before the body parsed — a later EOF/token reject left it set
            // on a zeroed slot (violates the untouched-state contract). The
            // caller publishes it after the full txt parse succeeds; the
            // bin reload sets it again together with precomputedValue.
            index += floatSize;
        }

        // 8.1 review (B5): skip separators explicitly, then require an
        // actual token to be present. stof's skip phase swallows a trailing
        // separator run and returns 0.0f with a POSITIVE floatSize — a file
        // providing N-1 floats + trailing whitespace used to fabricate the
        // final 0.0 sample, pass validation, and persist as a v2 cache.
        while (fsu_->isSeparator(buffer[index])) {
            index++;
        }
        if (buffer[index] == 0) {
            if (last) {
                // File ended before the declared count.
                return numberOfSampleError(f);
            }
            // Non-final chunk: the token starts past this chunk's edge (a
            // separator run or a >40-byte token beat the re-read margin).
            // Stop WITHOUT counting a sample; the next read delivers the
            // token whole.
            break;
        }
        int tokenStart = index;
        userWaveform[f][floatRead] = fsu_->stof(&buffer[index], floatSize);
        // 8.1 (B5): floatSize == 0 means the token could not be consumed
        // within the populated bytes (stof stopped at the NUL) — the file
        // ended before the declared count. Reject before consuming it.
        if (floatSize == 0) {
            return numberOfSampleError(f);
        }
        // Copilot review: stof's skip phase reports skipped non-numeric
        // characters as consumed — a digitless token ("abc", "-") yields
        // 0.0f with floatSize > 0 and was accepted as a sample. A valid
        // numeric token always contains at least one digit; reject
        // digitless tokens so malformed files cannot be normalized+cached.
        {
            bool hasDigit = false;
            for (int k = tokenStart; k < tokenStart + floatSize; k++) {
                if (buffer[k] >= '0' && buffer[k] <= '9') {
                    hasDigit = true;
                    break;
                }
            }
            if (!hasDigit) {
                return numberOfSampleError(f);
            }
        }
        // 8.1 review (B5): a token whose digits reach the NUL without a
        // closing separator is TRUNCATED by the chunk boundary, not
        // complete — parsing its prefix plus its remainder as two samples
        // corrupts a valid file (phantom sample + shift). Defer on a
        // non-final chunk: return the token start so the next read
        // re-parses the whole token. (On a final chunk the NUL is the true
        // EOF, so the token is complete.)
        if (!last && buffer[tokenStart + floatSize] == 0) {
            return tokenStart;
        }
        floatRead++;
        index = tokenStart + floatSize;

        // Stop if index > (filled - 30) Or if last && floatRead == 1024
        bStop = (!last && index > (filled - 40)) || floatRead == numberOfSample;
    }
    return index;
}

int UserWaveform::numberOfSampleError(int f) {
    numberOfSample = 0;
    userWaveFormNames[f][0] = '#';
    oscShapeNames[8 + f] = userWaveFormNames[f];
    // 7.7: userWaveform lives in .instruction_ram (ITCMRAM), which the
    // startup FillZerobss loop does not cover — zero the rejected slot or
    // it keeps power-on garbage and renders noise. Mirrors the no-file branch.
    for (int s = 0; s < 1024; s++) {
        userWaveform[f][s] = 0.0f;
    }
    return -1;
}


/*
 * 8.1 (B5): v2 bin loader. Layout: magic "P3W2" @0, name[4] @4, int32
 * count @8, float32 body @12. Returns 0 = legacy layout (no magic — no
 * state mutation, caller falls back to the source txt), 1 = v2 loaded,
 * -1 = v2 rejected via numberOfSampleError. The magic gate is what makes
 * every legacy cache — including already-poisoned ones, which are
 * count/size-indistinguishable from valid ones — regenerate from txt.
 */
int UserWaveform::loadUserWaveformFromBin(int f, const char* fileName) {
    char magic[4];
    // Copilot review: ANY failure to read a committed magic — including a
    // 0..3-byte file left by an interrupted initial save (the magic is
    // written LAST as the commit marker) — means "no committed v2 cache",
    // not corruption: return 0 so the caller regenerates from the canonical
    // txt. Only a PRESENT magic with invalid count/extent/body rejects (-1).
    if (load(fileName, 0, magic, 4) != 4 || memcmp(magic, USERWAVEFORM_BIN_MAGIC, 4) != 0) {
        return 0;
    }

    // B1 (review finding): a truncated bin (< 12 bytes, e.g. an interrupted
    // save) leaves numberOfSample partially written or entirely stale —
    // it is a MEMBER carrying the previous slot's count (the txt path
    // resets it to -1, the bin path didn't), and a stale value inside
    // [32,1024] walks straight through the range check below. Pre-set -1
    // so any partial read lands far outside the valid window.
    numberOfSample = -1;
    load(fileName, 8, &numberOfSample, 4);
    // B1: the txt path validates 32..1024, but the bin path trusted the
    // header — a corrupt/stale bin fed numberOfSample straight into
    // waveTables[].max and the body load (a count > 1024 spills the chunked
    // >512-byte reads into neighboring slots / past .instruction_ram).
    // B1/Copilot: the DECLARED body must also be present — a valid-range
    // count with a truncated body leaves the slot tail as power-on garbage
    // while publishing a valid name and max (7.7 noise class; the body
    // load() results are ignored below). Short-circuit keeps the multiply
    // overflow-safe (count already ∈ [32,1024] when it runs).
    // Reject exactly like a bad txt (zero slot, '#' name, no bin rewrite).
    // 8.1: `>=` not `==` — v2 saves open with FA_OPEN_ALWAYS|FA_WRITE, which
    // does not truncate; overwriting a longer legacy file leaves a harmless
    // stale tail (body reads stay bounded by count).
    int sizeBin = checkSize(fileName);
    if (numberOfSample < 32 || numberOfSample > 1024
            || sizeBin < 12 + numberOfSample * 4) {
        numberOfSampleError(f);
        return -1;
    }
    // 8.1 review: check the header read too — a name short-read (transient
    // I/O) must reject, not publish a stale name as success.
    if (load(fileName, 4, userWaveFormNames[f], 4) != 4) {
        numberOfSampleError(f);
        return -1;
    }
    oscShapeNames[8 + f] = userWaveFormNames[f];

    // 8.1 (B5): exact final chunk. The old loop always moved a full 512
    // bytes — the final one read/wrote past the declared body, which is
    // why legacy caches came out 512-rounded.
    int sampleSize = numberOfSample * 4;
    int loadIndex = 0;
    char* readBuffer = (char*)userWaveform[f];
    // 8.1 review: every body read is checked — a short read mid-body must
    // reject (numberOfSampleError zeroes the slot), not return success over
    // partially-updated memory.
    while (loadIndex < sampleSize) {
        int chunk = (sampleSize - loadIndex > 512) ? 512 : sampleSize - loadIndex;
        if (load(fileName, 12 + loadIndex, readBuffer + loadIndex, chunk) != chunk) {
            numberOfSampleError(f);
            return -1;
        }
        loadIndex += chunk;
    }
    // Copilot review: publish waveTables metadata only AFTER the body fully
    // loaded — a mid-body read failure used to leave max/precomputedValue
    // set on a zeroed slot (the same contract the txt path enforces).
    waveTables[f + 8].max = (numberOfSample  -1);
    waveTables[f + 8].precomputedValue = (waveTables[f + 8].max + 1) * waveTables[f + 8].useFreq * PREENFM_FREQUENCY_INVERSED;
    return 1;
}

void UserWaveform::saveUserWaveformToBin(int f, const char* fileName) {
    // 8.1 (B5): v2 layout — magic @0, name @4, count @8, body @12, with an
    // EXACT final chunk (a fresh file is exactly 12 + 4n bytes). The magic
    // is written LAST as a commit marker (8.1 review): save() calls are not
    // transactional, and an interrupted save that had already written the
    // magic would leave a magic-bearing partial file that rejects with no
    // txt fallback. Magic-less, it regenerates from the txt on next boot.
    // Magic bytes are copied from the shared define one char at a time:
    // save() takes void* (binding the string literal would need a
    // const-cast) and memcpy from a literal trips the not-null-terminated
    // analyzer check on a buffer that is deliberately 4 raw bytes.
    char magic[4];
    for (int i = 0; i < 4; i++) {
        magic[i] = USERWAVEFORM_BIN_MAGIC[i];
    }
    save(fileName, 4, userWaveFormNames[f], 4);
    save(fileName, 8, &numberOfSample, 4);
    int sampleSize = numberOfSample * 4;
    int saveIndex = 0;
    char* readBuffer = (char*)userWaveform[f];
    while (saveIndex < sampleSize) {
        int chunk = (sampleSize - saveIndex > 512) ? 512 : sampleSize - saveIndex;
        save(fileName, 12 + saveIndex, readBuffer + saveIndex, chunk);
        saveIndex += chunk;
    }
    save(fileName, 0, magic, 4);
}

void UserWaveform::normalize(float* buffer, int numberOfSamples) {
    float min = 0;
    float max = 0;
    float average = 0;
    for (int i=0; i < numberOfSamples; i++) {
        average += buffer[i];
        if (buffer[i] < min) {
            min = buffer[i];
        }
        if (buffer[i] > max) {
            max = buffer[i];
        }
    }
    average /= numberOfSamples;
    min -= average;
    max -= average;
    float m1 = min != 0 ? - 1 / min : 1;
    float m2 = max != 0 ? 1 / max : 1;
    float m = m1 > m2 ? m2 : m1;

    for (int i=0; i < numberOfSamples; i++) {
        buffer[i] -= average;
        buffer[i] *= m;
    }
}

void UserWaveform::interpolate(float* buffer, int sourceNumberOfSamples, int targetNumberOfSamples) {
    // Review patch: callers pass 33..1023 (loadUserWaveforms guards), but a
    // zero/negative source would read buffer[-1] below — bail instead.
    if (sourceNumberOfSamples <= 0 || targetNumberOfSamples <= 0) {
        return;
    }
    for (int i = targetNumberOfSamples-1; i>=0; i--) {
        float pos = (float)i * (float)sourceNumberOfSamples /  (float)targetNumberOfSamples;
        int iPos = pos;
        float decimal = pos - iPos;
        // 6.4: for the last target sample iPos+1 can equal
        // sourceNumberOfSamples — one past the window the txt parse
        // populated; the stale/zero tail silently leaked in. Clamp the
        // upper read to the last populated sample.
        int iPosNext = iPos + 1;
        if (iPosNext >= sourceNumberOfSamples) {
            iPosNext = sourceNumberOfSamples - 1;
        }
        buffer[i] = buffer[iPos] * (1-decimal) + buffer[iPosNext] * decimal;
    }
    numberOfSample = targetNumberOfSamples;
}

