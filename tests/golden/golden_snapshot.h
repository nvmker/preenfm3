// Fixture file I/O for the golden-master tier. Header-only (<fstream>/<string>).
//
// Fixture layout for a golden id "X" under tests/golden/:
//   X.bin       — raw int32_t render (nBlocks * samplesPerBlock = nBlocks*192),
//                 host-native byte order (all CI legs are little-endian
//                 x86_64/arm64; the int32 DAC format is endian-stable there).
//   X.xxh       — one line: the 64-bit golden hash as 16-digit lowercase hex.
//   X.diff.txt  — downsampled human-readable dump: first 4 blocks in full,
//                 then 1-in-10 thereafter. For failure diffing only.
//
// The render is stored as 3 contiguous buffers per block: for block b,
//   [b1(64) b2(64) b3(64)]  →  out[b*192 .. b*192+192).
// samplesPerBuffer=64 (BLOCK_SIZE=32 frames × 2 stereo).

#pragma once

#include <cstdint>
#include <cstddef>
#include <fstream>
#include <iomanip>
#include <string>

namespace golden {

inline std::string fixturePath(const std::string& dir, const std::string& id,
                               const char* ext) {
    std::string p = dir;
    if (p.empty() || p.back() != '/') p += "/";
    p += id;
    p += ext;
    return p;
}

// --- raw int32 render (.bin) -------------------------------------------------

inline bool writeRenderBin(const std::string& path, const int32_t* render,
                           std::size_t count) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(render),
            static_cast<std::streamsize>(count * sizeof(int32_t)));
    return static_cast<bool>(f);
}

inline bool readRenderBin(const std::string& path, int32_t* out,
                          std::size_t count) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.read(reinterpret_cast<char*>(out),
           static_cast<std::streamsize>(count * sizeof(int32_t)));
    if (!f || static_cast<std::size_t>(f.gcount()) != count * sizeof(int32_t))
        return false;
    // Reject a fixture with trailing bytes (stale longer fixture from a prior
    // nBlocks, or a concatenated file) — silently ignoring extras would mask a
    // wrong-size fixture as a passing compare.
    return f.peek() == std::ifstream::traits_type::eof();
}

// --- hash file (.xxh) --------------------------------------------------------

inline bool writeHashFile(const std::string& path, uint64_t h) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << std::hex << std::setw(16) << std::setfill('0') << h << "\n";
    return static_cast<bool>(f);
}

inline bool readHashFile(const std::string& path, uint64_t* out) {
    std::ifstream f(path);
    if (!f) return false;
    std::string s;
    if (!std::getline(f, s)) return false;
    // strip trailing whitespace/newlines only
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                          s.back() == ' ' || s.back() == '\t')) s.pop_back();
    // strict: exactly 16 hex digits, nothing else. A fat-fingered/corrupted
    // manifest (trailing junk, wrong length) must NOT silently parse as a
    // truncated hash — the .xxh is the committed CI lock.
    if (s.size() != 16) return false;
    for (char c : s) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                         (c >= 'A' && c <= 'F');
        if (!hex) return false;
    }
    try {
        *out = std::stoull(s, nullptr, 16);
        return true;
    } catch (...) {
        return false;
    }
}

// --- downsampled text dump (.diff.txt) --------------------------------------
// First `headBlocks` blocks in full, then every `stride`-th block thereafter.
inline bool writeDiffTxt(const std::string& path, const int32_t* render,
                         std::size_t nBlocks,
                         std::size_t samplesPerBuffer = 64,
                         std::size_t buffersPerBlock = 3,
                         std::size_t headBlocks = 4, std::size_t stride = 10) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    const std::size_t perBlock = samplesPerBuffer * buffersPerBlock;
    f << "# preenfm3 golden diff dump\n";
    f << "# downsampled: first " << headBlocks << " blocks in full, then 1-in-"
      << stride << " thereafter\n";
    f << "# nBlocks=" << nBlocks << " samplesPerBuffer=" << samplesPerBuffer
      << " buffersPerBlock=" << buffersPerBlock
      << " samplesPerBlock=" << perBlock << "\n\n";
    for (std::size_t b = 0; b < nBlocks; b++) {
        const bool inHead = b < headBlocks;
        const bool onStride = (b % stride) == 0;
        if (!(inHead || onStride)) continue;
        f << "block " << std::dec << std::setw(4) << std::setfill('0') << b;
        f << (inHead ? "  (full)" : "  (stride)");
        f << "\n";
        for (std::size_t buf = 0; buf < buffersPerBlock; buf++) {
            f << "  buf" << buf << ":";
            const int32_t* p =
                render + b * perBlock + buf * samplesPerBuffer;
            for (std::size_t i = 0; i < samplesPerBuffer; i++) {
                f << " " << std::hex << std::setw(8) << std::setfill('0')
                  << static_cast<uint32_t>(p[i]);
            }
            f << "\n";
        }
        f << "\n";
    }
    return static_cast<bool>(f);
}

}  // namespace golden
