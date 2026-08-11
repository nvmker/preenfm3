// Golden-master comparison + hashing primitives for the preenfm3 full-render
// regression tier. Header-only, zero system deps beyond <cstdint>/<cstddef>.
//
// Design (see _bmad-output/planning-artifacts/golden-master-test-plan.md §3):
//   - The render artifact is int32_t DAC-format output. A 1-ULP float drift in
//     the mix becomes 0-or-1 LSB after the x0x7fffff scale + truncation, so a
//     ±lsbTolerance band absorbs benign compiler drift while any real DSP
//     change moves many samples by many LSBs.
//   - goldenCompare is the AUTHORITATIVE tolerance gate.
//   - goldenHash runs over the tolerance-NORMALIZED buffer (each sample
//     quantized to its ±tolerance bucket) so benign drift is hash-stable in
//     the common case. Bucket boundaries can still split under a uniform
//     shift; that is a known, documented limitation — Phase G2 cross-host
//     validation will decide whether to tighten to an exact-bit hash.
//   - The hash is a self-contained FNV-1a 64-bit + splitmix64 finalizer. No
//     system/Homebrew xxhash (CI is ubuntu/gcc; a macOS-only dep would break
//     it). Collision-resistance for a ~150 KB regression lock is more than
//     sufficient.

#pragma once

#include <cstdint>
#include <cstddef>

namespace golden {

// Result of a failed (or successful) comparison. blockIndex is filled by the
// caller (which knows samplesPerBlock); goldenCompare only has the flat index.
struct GoldenDiff {
    bool matched = true;
    bool hashMismatch = false;   // true when goldenCompare passed within tolerance
                                // but the tolerance-normalized hash differed
                                // (a bucket-boundary split; see golden/README.md)
    std::size_t firstMismatchIndex = 0;   // flat index into the buffer
    int32_t expectedSample = 0;
    int32_t actualSample = 0;
    int32_t sampleDelta = 0;              // |actual - expected|; always >= 0
    std::size_t blockIndex = 0;           // firstMismatchIndex / samplesPerBlock
};

// Returns true if every sample agrees within lsbTolerance. On the FIRST
// mismatch, fills *diff (if non-null).samplesPerBlock is used only to derive
// blockIndex; pass 0 to leave it unset.
inline bool goldenCompare(const int32_t* expected, const int32_t* actual,
                          std::size_t count, int lsbTolerance,
                          std::size_t samplesPerBlock = 0,
                          GoldenDiff* diff = nullptr) {
    for (std::size_t i = 0; i < count; i++) {
        int64_t a = static_cast<int64_t>(actual[i]);
        int64_t e = static_cast<int64_t>(expected[i]);
        int64_t delta = a - e;
        if (delta < 0) delta = -delta;
        if (delta > lsbTolerance) {
            if (diff) {
                diff->matched = false;
                diff->firstMismatchIndex = i;
                diff->expectedSample = static_cast<int32_t>(e);
                diff->actualSample = static_cast<int32_t>(a);
                diff->sampleDelta = static_cast<int32_t>(delta);
                diff->blockIndex =
                    samplesPerBlock ? (i / samplesPerBlock) : 0;
            }
            return false;
        }
    }
    if (diff) diff->matched = true;
    return true;
}

// Quantize a sample to its ±lsbTolerance bucket representative so benign
// drift collapses to the same canonical value (keeps the hash stable under
// compiler drift in the common case). Bucket width = 2*lsbTolerance+1; the
// representative is the bucket center. Floor-division for negatives. The
// q*bucket+center product is computed in int64 because at sample=INT32_MIN
// with bucket=3 it underflows int32_t (signed-overflow UB).
inline int32_t normalizeSampleForHash(int32_t sample, int lsbTolerance) {
    if (lsbTolerance <= 0) return sample;
    const int32_t bucket = 2 * lsbTolerance + 1;
    const int32_t center = bucket / 2;
    int32_t q = sample / bucket;
    int32_t r = sample % bucket;
    if (r < 0) { q -= 1; r += bucket; }   // C++ / truncates toward 0 -> floor
    int64_t res = static_cast<int64_t>(q) * bucket + center;
    return static_cast<int32_t>(res);    // in int32 range (quantization of an int32 input)
}

// FNV-1a 64-bit over the tolerance-normalized buffer, with a splitmix64-style
// finalizer for avalanche. Stable across gcc/clang, x86/arm64 (no libm, no UB).
inline uint64_t goldenHash(const int32_t* buf, std::size_t count,
                           int lsbTolerance) {
    uint64_t h = 0xcbf29ce484222325ULL;            // FNV-1a 64 offset basis
    const uint64_t prime = 0x100000001b3ULL;       // FNV-1a 64 prime
    for (std::size_t i = 0; i < count; i++) {
        int32_t n = normalizeSampleForHash(buf[i], lsbTolerance);
        uint64_t v = static_cast<uint64_t>(static_cast<uint32_t>(n));
        for (int b = 0; b < 8; b++) {              // mix all 8 bytes, FNV-1a
            h ^= (v >> (b * 8)) & 0xff;
            h *= prime;
        }
    }
    h ^= h >> 30;                                  // splitmix64 finalizer
    h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebULL;
    h ^= h >> 31;
    return h;
}

}  // namespace golden
