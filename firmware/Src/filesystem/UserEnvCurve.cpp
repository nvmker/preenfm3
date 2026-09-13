/*
 * Copyright 2022
 *
 * Author: Patrice Vigouroux (Toltekradiation)
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

#include "StorageSizes.h"

// Local chunk size for txt reads; the extern now declares the true buffer
// size (spec 2.7) — reads stay capped at 64 bytes per pass as before.
#define LINE_BUFFER_SIZE 64
extern char lineBuffer[PFM3_LINE_BUFFER_SIZE];

// User curves
extern float userEnvCurves[4][64];
extern char *envCurveNames[];

#include "UserEnvCurve.h"

#include <math.h>   // lround (was (int)(x + .5f) -- bugprone-incorrect-roundings)
#include <string.h>  // memcmp (8.1 v2 magic gate; no alignment-sensitive casts)

UserEnvCurve::UserEnvCurve() {
    for (int k=0; k<4; k++) {
        userEnvCurveNames[k][4] = 0;
    }
}

UserEnvCurve::~UserEnvCurve() {
}

const char* UserEnvCurve::getFolderName() {
    return USERCURVE_DIR;
}

void UserEnvCurve::loadUserEnvCurves() {
    char fileName[30];


    for (int f = 0; f < 4; f++) {
        // Check if bin exists

        fsu_->copy_string(fileName, USERCURVE_FILENAME_BIN);
        fileName[20] = (char)('1' + f);

        int sizeBin = checkSize(fileName);

        // 8.1 (B5): versioned bin cache (magic "P3C2"). 0 = legacy layout
        // (no magic): untrustworthy — an already-poisoned cache is count/
        // size-indistinguishable — so fall through to the txt path and let
        // the v2 save replace the same file in place (one-shot migration).
        // 1 = v2 loaded; -1 = v2 rejected (numberOfSampleError already
        // applied — NO txt fallback). Legacy-without-txt takes the no-file
        // default (linear ramp).
        bool tryTxt = true;
        if (sizeBin != -1) {
            tryTxt = (loadUserEnvCurveFromBin(f, fileName) == 0);
        }
        if (tryTxt) {
            fsu_->copy_string(fileName, USERCURVE_FILENAME_TXT);
            fileName[20] = (char)('1' + f);

            int sizeTxt = checkSize(fileName);
            if (sizeTxt == -1) {
                // Does not exist. Neither Bin nor txt => Linear env
                for (float s = 0; s < 64; s++) {
                    userEnvCurves[f][(int) s] = s / 64;
                }
            } else {
                numberOfSample = -1;
                userEnvCurveNames[f][0] = 0;
                loadUserEnvCurveFromTxt(f, fileName, sizeTxt);

                if (numberOfSample > 0) {
                    // 5.4: the old 3 < numberOfSample < 64 interpolate call
                    // here was dead code — the txt parser only accepts
                    // exactly 64 samples, so numberOfSample is always 64
                    // (or an error) when this point is reached.
                    normalize(userEnvCurves[f], numberOfSample);

                    fsu_->copy_string(fileName, USERCURVE_FILENAME_BIN);
                    fileName[20] = (char)('1' + f);
                    saveUserEnvCurveToBin(f, fileName);
                    // Reload from Bin
                    loadUserEnvCurveFromBin(f, fileName);
                }
            }
        }
    }
}

/*
 * 8.1 (B5): v2 bin loader. Layout: magic "P3C2" @0, name[4] @4, uint16
 * count @8, float32 body @10 (266 bytes). Returns 0 = legacy layout (no
 * magic — no state mutation, caller falls back to the source txt),
 * 1 = v2 loaded, -1 = v2 rejected via numberOfSampleError. This is the
 * loader's FIRST-EVER validation: the old one fed the raw count straight
 * into numberOfSample*4 — a garbage count multiplied into an unbounded
 * body read that overran the 256-byte curve slot into neighboring memory.
 */
int UserEnvCurve::loadUserEnvCurveFromBin(int f, const char* fileName) {
    char magic[4];
    // Copilot review: ANY failure to read a committed magic — including a
    // 0..3-byte file left by an interrupted initial save (the magic is
    // written LAST as the commit marker) — means "no committed v2 cache",
    // not corruption: return 0 so the caller regenerates from the canonical
    // txt. Only a PRESENT magic with invalid count/extent/body rejects (-1).
    if (load(fileName, 0, magic, 4) != 4 || memcmp(magic, USERCURVE_BIN_MAGIC, 4) != 0) {
        return 0;
    }

    // -1-style pre-set (B1 mirror), but on a LOCAL uint16: the old plan of
    // pre-setting the 4-byte member to -1 and loading 2 bytes leaves the
    // high half 0xFFFF — a valid count of 64 would read as 0xFFFF0040 and
    // reject. A fresh local pre-set to 0xFFFF keeps the partial-read
    // guarantee (any short write leaves 0xFF bits in the window, never 64)
    // and makes stale member values impossible. Only exactly 64 with a
    // full 266-byte extent may reach the body read — that bounds the
    // previously unbounded numberOfSample*4 access to the 256-byte slot.
    uint16_t count = 0xFFFF;
    load(fileName, 8, &count, 2);
    numberOfSample = count;
    int sizeBin = checkSize(fileName);
    if (numberOfSample != 64 || sizeBin < 10 + 64 * 4) {
        numberOfSampleError(f);
        return -1;
    }
    // 8.1 review: check every read — a name/body short-read (transient I/O)
    // must reject, not publish stale/partial memory as success.
    if (load(fileName, 4, userEnvCurveNames[f], 4) != 4) {
        numberOfSampleError(f);
        return -1;
    }
    envCurveNames[3 + f] = userEnvCurveNames[f];
    if (load(fileName, 10, userEnvCurves[f], 64 * 4) != 64 * 4) {
        numberOfSampleError(f);
        return -1;
    }
    return 1;
}

void UserEnvCurve::saveUserEnvCurveToBin(int f, const char* fileName) {
    // 8.1 (B5): v2 layout — magic @0, name @4, uint16 count @8, 256-byte
    // body @10 (266 bytes total). The magic is written LAST as a commit
    // marker (8.1 review): an interrupted save that had already written the
    // magic would leave a magic-bearing partial file that rejects with no
    // txt fallback; magic-less, it regenerates from the txt on next boot.
    // Magic bytes copied per-char from the shared define (no const-cast for
    // save()'s void*, no memcpy-from-literal analyzer complaint on a
    // deliberately raw 4-byte buffer).
    char magic[4];
    for (int i = 0; i < 4; i++) {
        magic[i] = USERCURVE_BIN_MAGIC[i];
    }
    save(fileName, 4, userEnvCurveNames[f], 4);
    save(fileName, 8, &numberOfSample, 2);
    save(fileName, 10, userEnvCurves[f], 64 * 4);
    save(fileName, 0, magic, 4);
}

void UserEnvCurve::loadUserEnvCurveFromTxt(int f, const char* fileName, int size) {
    int readIndex = 0;
    floatRead = 0;

    while (floatRead != numberOfSample) {
        // 8.1 (B5): EOF guards, mirroring the waveform loader. A truncated
        // txt (declares 64, provides fewer) used to parse stale lineBuffer
        // bytes past the populated part, normalize the poisoned curve, and
        // persist it as the bin cache. Reject through numberOfSampleError
        // BEFORE parsing anything stale.
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
        // (64) into the 1024-byte lineBuffer — always room for the NUL.
        lineBuffer[toRead] = 0;

        int used = fillUserEnvCurveFromTxt(f, lineBuffer, toRead, (readIndex+toRead) >= size);
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

int UserEnvCurve::fillUserEnvCurveFromTxt(int f, char* buffer, int filled, bool last) {
    int index = 0;
    bool bStop = false;
    while (!bStop) {
        int floatSize = 0;

        // Init name
        if (userEnvCurveNames[f][0] == 0) {

            while (fsu_->isSeparator(buffer[index])) {
                index++;
            }
            int b = 0;
            // 8.1 review (B5): NUL is not a separator — without this bound a
            // name cut by the chunk edge copied the NUL and then up to 3
            // STALE bytes past the populated region into the name.
            while (b<4 && buffer[index] != 0 && !fsu_->isSeparator(buffer[index])) {
                userEnvCurveNames[f][b++] = buffer[index++];
            }
            // complete with space
            while (b<4) {
                userEnvCurveNames[f][b++] = ' ';
            }
        }
        // init number of sample
        if (numberOfSample <= 0) {
            while (fsu_->isSeparator(buffer[index])) {
                index++;
            }
            numberOfSample = (int) lround(fsu_->stof(&buffer[index], floatSize));

            if (numberOfSample != 64) {
                return numberOfSampleError(f);
            }

            index += floatSize;
        }

        userEnvCurves[f][floatRead] = fsu_->stof(&buffer[index], floatSize);
        // 8.1 review (B5): skip separators explicitly, then require an actual
        // token to be present. stof's skip phase swallows a trailing
        // separator run and returns 0.0f with a POSITIVE floatSize — a file
        // declaring 64 but providing 63 floats + trailing whitespace used to
        // fabricate the final 0.0 sample and persist it as a v2 cache.
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
        userEnvCurves[f][floatRead] = fsu_->stof(&buffer[index], floatSize);
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

void UserEnvCurve::normalize(float* buffer, int numberOfSamples) {
    if (numberOfSamples <= 0) {
        return;
    }

    // Sanitize before seeding extrema: a non-finite first sample must not
    // poison min/max. Imported text can overflow the parser's float
    // accumulator even though normal curve files contain only finite values.
    for (int i = 0; i < numberOfSamples; i++) {
        if (!isfinite(buffer[i])) {
            buffer[i] = 0.0f;
        }
    }

    float min = buffer[0];
    float max = buffer[0];
    for (int i = 1; i < numberOfSamples; i++) {
        if (buffer[i] < min) {
            min = buffer[i];
        }
        if (buffer[i] > max) {
            max = buffer[i];
        }
    }

    // Widen the subtraction so opposite-sign finite float extrema cannot
    // overflow the range to infinity. A flat curve has no shape to scale;
    // preserve its finite DC level exactly as required by the file contract.
    const double range = static_cast<double>(max) - static_cast<double>(min);
    if (range == 0.0) {
        return;
    }

    for (int i = 0; i < numberOfSamples; i++) {
        double normalized = (static_cast<double>(buffer[i]) - static_cast<double>(min)) / range;
        if (normalized > 1.0) {
            normalized = 1.0;
        }
        if (normalized < 0.0) {
            normalized = 0.0;
        }
        buffer[i] = static_cast<float>(normalized);
    }
}

int UserEnvCurve::numberOfSampleError(int f) {
    numberOfSample = 0;
    userEnvCurveNames[f][0] = '#';
    // A4c (8.1): the waveform twin repoints oscShapeNames[8+f], but the '#'
    // marker never reached envCurveNames[3+f] — the curve row list kept
    // showing "Usr1".."Usr4" on a cold boot after a rejected load. Repoint
    // like the twin so the error is visible.
    envCurveNames[3 + f] = userEnvCurveNames[f];
    // 7.7: like userWaveform, the curve table is not zeroed at boot —
    // reset the rejected slot to the no-file linear-ramp default so a
    // wrongly selected curve degrades to stock behavior, not power-on garbage.
    for (int s = 0; s < 64; s++) {
        userEnvCurves[f][s] = s / 64.0f;
    }
    return -1;
}

