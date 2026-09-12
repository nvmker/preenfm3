// pfm3_bench — standalone perf driver for the CI perf gates (two signals).
//
//   --mode=ir   (default) Render once, N blocks, exit 0. The MEASUREMENT
//               happens outside this process: scripts/ci/perf-gate.sh runs it
//               under `valgrind --tool=callgrind
//               --toggle-collect='*buildNewSampleBlock*'` so only the render
//               window (buildNewSampleBlock + callees) is counted. stdout is
//               for humans (--json) only — the gate never parses it.
//   --mode=wall Time the render INSIDE the process: W warmup renders, then R
//               measured renders — a FRESH harness per iteration (workload ≡
//               the golden workload's first render; no cross-repeat
//               synth-state drift), each timed with steady_clock around ONLY
//               the renderScript call (harness construction and output
//               allocation stay outside the timed window). Prints the MEDIAN
//               ns/block. With --json, EXACTLY ONE JSON line goes to stdout
//               and the gate PARSES it — the documented exception to Phase
//               2's "gate never parses bench stdout": wall time can only be
//               observed where it happens, and perf-gate.sh's fail-closed
//               guards (JSON shape, blocks cross-check, plausibility floor)
//               replace the out-file parsing guarantees.
//
// See _bmad-output/planning-artifacts/ci-performance-proposal.md (three-signal
// design; ir + wall are the two host signals) +
// ci-performance-implementation-plan.md §2.2/§3a.
//
// WORKLOAD ≡ GOLDEN WORKLOAD: the registry below pairs each script factory
// with the exact out-of-band patches the corresponding TEST in
// tests/golden_master_test.cpp applies (setTimbreAlgo for the FM fixtures,
// enableArpeggiator for the arp fixture) — copied from runGolden's
// construct → setTimbreAlgo → preRender → renderScript sequence, so the
// measured render is the same render the committed fixtures lock.
//
// No gtest anywhere: own main(), links only the pfm3_fw_host object library
// (the same objects pfm3_tests compiles — same flags via its PUBLIC
// interface, including -ffp-contract=off, so Ir counts are reproducible).
//
// Usage:
//   pfm3_bench --script=<name> [--blocks=N] [--mode=ir|wall] [--json]
//              [--repeat=R] [--warmup=W]
//   pfm3_bench --list        one registry key per line (for perf-gate.sh)
//
//   --script   registry key (required). Unknown name → registry list, exit 2.
//   --blocks   render block count (default: the entry's golden count). Must be
//              a plain decimal integer ≥ 1 (strtoul quirk: '-1' would wrap to
//              ULONG_MAX and blow up the output allocation — reject signs).
//   --mode     ir (default) or wall — see the mode notes at the top. Any other
//              value → usage + exit 3.
//   --repeat   wall mode: measured render count (default 10). Plain decimal
//              integer ≥ 1. Ignored in ir mode.
//   --warmup   wall mode: untimed warmup render count (default 3). Plain
//              decimal integer ≥ 0. Ignored in ir mode.
//   --json     machine-readable summary line. wall+--json prints the single
//              JSON line the gate parses: script/blocks/mode/warmup/repeat/
//              ns_per_block (median ns/block as a double).
//
// Exit codes: 0 success · 2 usage/unknown script/bad numeric arg · 3 unknown
// mode.

#include "../golden_harness.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace {

// One measurable workload. `preRender` carries the out-of-band per-timbre
// state changes the golden TEST applies between construction and render
// (runGolden's algoTimbre/preRender slots — the script factories alone do
// not describe those fixtures; see golden_harness.h's RenderScript note).
struct BenchEntry {
    const char* description;                        // shown by the registry list
    std::size_t defaultBlocks;                      // matches the golden TEST's count
    golden::RenderScript (*script)();               // script factory
    void (*preRender)(golden::GoldenHarness&);      // out-of-band patches (may be null)
};

void preRenderFmAlgo27(golden::GoldenHarness& h) {
    // GoldenMaster.FmAlgo27_6carrier: algoTimbre=0, algo=ALG27.
    h.setTimbreAlgo(0, ALG27);
}

void preRenderArpTriadUp(golden::GoldenHarness& h) {
    // GoldenMaster.ArpTriadUp: enableArpeggiator(timbre 0, 120 BPM,
    // ARPEGGIO_DIRECTION_UP, 2 octaves) — internal clock, no MIDI bytes.
    h.enableArpeggiator(0, /*bpm=*/120, /*direction=*/0 /*ARPEGGIO_DIRECTION_UP*/,
                        /*octave=*/2);
}

const std::map<std::string, BenchEntry>& registry() {
    static const std::map<std::string, BenchEntry> r = {
        {"a4_default_sustain",
         {"G0 steady-state sustain: noteOn(t0, 69, 100)@0, no note-off "
          "(single timbre, 6 voices)",
          200,
          &golden::RenderScript::a4Sustain,
          nullptr}},
        {"fm_algo27_6carrier",
         {"heaviest multi-carrier FM topology: a4Sustain + setTimbreAlgo(0, "
          "ALG27) — full additive summing path",
          200,
          &golden::RenderScript::a4Sustain,
          &preRenderFmAlgo27}},
        {"arp_triad_up",
         {"note-allocation churn: arpTriadUp + enableArpeggiator(0, 120, UP, "
          "2 oct) — periodic voice realloc across blocks",
          300,
          &golden::RenderScript::arpTriadUp,
          &preRenderArpTriadUp}},
    };
    return r;
}

void printUsage(const char* argv0) {
    std::cerr << "usage: " << argv0
              << " --script=<name> [--blocks=N] [--mode=ir|wall] [--json]"
                 " [--repeat=R] [--warmup=W]\n";
}

void printRegistry() {
    std::cerr << "available scripts (workload = the matching golden test):\n";
    for (const auto& kv : registry()) {
        std::cerr << "  " << kv.first << "  [default blocks: "
                  << kv.second.defaultBlocks << "]\n    " << kv.second.description
                  << "\n";
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string scriptName;
    std::size_t blocks = 0;      // 0 = use the entry's defaultBlocks
    std::string mode = "ir";
    std::size_t repeat = 10;     // wall: measured renders (default 10)
    std::size_t warmup = 3;      // wall: untimed warmup renders (default 3)
    bool json = false;
    bool listMode = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--list") {
            listMode = true;
            continue;
        }
        const std::string::size_type eq = arg.find('=');
        const std::string flag = arg.substr(0, eq == std::string::npos ? arg.size() : eq);
        const std::string value = eq == std::string::npos ? std::string() : arg.substr(eq + 1);

        if (flag == "--script" && !value.empty()) {
            scriptName = value;
        } else if (flag == "--blocks" && !value.empty()) {
            // Plain decimal digits only: a leading '+'/'-' would survive
            // strtoul as a huge wrapped value and blow up the allocation.
            if (value.find_first_not_of("0123456789") != std::string::npos) {
                std::cerr << "ERR: --blocks must be a positive integer, got '"
                          << value << "'\n";
                return 2;
            }
            char* end = nullptr;
            const unsigned long v = std::strtoul(value.c_str(), &end, 10);
            if (end == nullptr || *end != '\0' || v == 0) {
                std::cerr << "ERR: --blocks must be a positive integer, got '"
                          << value << "'\n";
                return 2;
            }
            blocks = static_cast<std::size_t>(v);
        } else if (flag == "--mode" && !value.empty()) {
            mode = value;
        } else if (flag == "--json") {
            json = true;
        } else if (flag == "--repeat") {
            // Same digits-only discipline as --blocks: a leading '+'/'-'
            // would survive strtoul as a huge wrapped value. repeat >= 1
            // (a median needs at least one sample) and capped at a sane
            // bound: without the cap, strtoul(ULONG_MAX) wraps the
            // warmup+repeat loop bound and the median would index an EMPTY
            // sample vector (UB).
            if (value.empty() ||
                value.find_first_not_of("0123456789") != std::string::npos) {
                std::cerr << "ERR: --repeat must be an integer >= 1, got '"
                          << value << "'\n";
                return 2;
            }
            char* end = nullptr;
            const unsigned long v = std::strtoul(value.c_str(), &end, 10);
            if (end == nullptr || *end != '\0' || v == 0 || v > 1000000) {
                std::cerr << "ERR: --repeat must be an integer in [1, 1000000], got '"
                          << value << "'\n";
                return 2;
            }
            repeat = static_cast<std::size_t>(v);
        } else if (flag == "--warmup") {
            // Digits-only; warmup may be 0 (no untimed renders). Same cap:
            // warmup+repeat must never overflow size_t (empty-median UB) and
            // the per-repeat harness allocation must stay bounded.
            if (value.empty() ||
                value.find_first_not_of("0123456789") != std::string::npos) {
                std::cerr << "ERR: --warmup must be an integer >= 0, got '"
                          << value << "'\n";
                return 2;
            }
            char* end = nullptr;
            const unsigned long v = std::strtoul(value.c_str(), &end, 10);
            if (end == nullptr || *end != '\0' || v > 1000000) {
                std::cerr << "ERR: --warmup must be an integer in [0, 1000000], got '"
                          << value << "'\n";
                return 2;
            }
            warmup = static_cast<std::size_t>(v);
        } else {
            std::cerr << "ERR: unknown argument '" << arg << "'\n";
            printUsage(argv[0]);
            return 2;
        }
    }

    // Mode is validated EARLY — before --list and before any render — so
    // `--list --mode=wall` keeps working and an unknown mode never reaches a
    // workload. exit 3 keeps the Phase-2 contract (unknown mode ≠ usage).
    if (mode != "ir" && mode != "wall") {
        std::cerr << "ERR: --mode=" << mode << " is not a known mode (ir, wall)\n";
        printUsage(argv[0]);
        return 3;
    }
    if (listMode) {
        // Machine-readable registry keys for scripts/ci/perf-gate.sh's
        // coverage check (bench scripts missing from the baseline must be a
        // loud failure, not a silently unmeasured workload).
        for (const auto& kv : registry()) {
            std::cout << kv.first << "\n";
        }
        return 0;
    }
    if (scriptName.empty() && !listMode) {
        printUsage(argv[0]);
        printRegistry();
        return 2;
    }

    const auto it = registry().find(scriptName);
    if (it == registry().end()) {
        std::cerr << "ERR: unknown script '" << scriptName << "'\n";
        printRegistry();
        return 2;
    }
    const BenchEntry& entry = it->second;
    if (blocks == 0) {
        blocks = entry.defaultBlocks;
    }
    // Output-buffer overflow guard (both modes share this allocation): a
    // --blocks value near ULONG_MAX wraps blocks*kSamplesPerBlock to a tiny
    // size_t and renderScript would write out of bounds. The registry
    // defaults (200/300) can never trip this — only an explicit --blocks can.
    if (blocks > std::numeric_limits<std::size_t>::max() /
                     golden::GoldenHarness::kSamplesPerBlock) {
        std::cerr << "ERR: --blocks value " << blocks
                  << " overflows the output allocation (blocks * "
                  << golden::GoldenHarness::kSamplesPerBlock
                  << " samples)\n";
        return 2;
    }

    if (mode == "wall") {
        // Wall-clock trend mode: W warmup renders, then R measured renders,
        // each on a FRESH harness (construction + output allocation strictly
        // OUTSIDE the timed window; only renderScript is timed). One repeat's
        // ns_per_block = elapsed_ns / blocks. The script is constructed ONCE
        // before the loop — the factory builds a RenderScript vector, and
        // building it inside the timed window would bill script-construction
        // allocation to every sample (measurement-contract violation).
        const golden::RenderScript script = entry.script();
        std::vector<double> nsPerBlock;
        nsPerBlock.reserve(repeat);
        for (std::size_t i = 0; i < warmup + repeat; ++i) {
            golden::GoldenHarness harness(std::string(),
                                           golden::TimbreSetup::g0Default());
            if (entry.preRender != nullptr) {
                entry.preRender(harness);
            }
            std::vector<int32_t> out(
                blocks * golden::GoldenHarness::kSamplesPerBlock);
            const auto t0 = std::chrono::steady_clock::now();
            harness.renderScript(script, blocks, out.data());
            const auto t1 = std::chrono::steady_clock::now();
            if (i >= warmup) {
                const auto elapsedNs =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                        .count();
                nsPerBlock.push_back(static_cast<double>(elapsedNs) /
                                     static_cast<double>(blocks));
            }
        }
        std::sort(nsPerBlock.begin(), nsPerBlock.end());
        const std::size_t n = nsPerBlock.size();  // == repeat, validated >= 1
        const double median =
            (n % 2 == 1) ? nsPerBlock[n / 2]
                         : (nsPerBlock[n / 2 - 1] + nsPerBlock[n / 2]) / 2.0;
        if (json) {
            // EXACTLY one JSON line on stdout — perf-gate.sh parses this (the
            // documented wall-mode exception). setprecision(12) keeps the
            // double round-trippable instead of collapsing to 6 digits /
            // scientific notation. min/max ride along (diagnostics only —
            // the gate gates on ns_per_block; the spread is what separates a
            // stable regression from scheduler noise in post-mortems).
            std::cout << std::setprecision(12);
            std::cout << "{\"script\": \"" << scriptName
                      << "\", \"blocks\": " << blocks
                      << ", \"mode\": \"wall\""
                      << ", \"warmup\": " << warmup
                      << ", \"repeat\": " << repeat
                      << ", \"ns_per_block\": " << median
                      << ", \"min\": " << nsPerBlock.front()
                      << ", \"max\": " << nsPerBlock.back() << "}\n";
        } else {
            std::cout << scriptName << ": " << blocks
                      << " blocks, wall median " << median
                      << " ns/block (min " << nsPerBlock.front() << ", max "
                      << nsPerBlock.back() << "; repeat=" << repeat
                      << " warmup=" << warmup << ")\n";
        }
        return 0;
    }

    // ir mode: construct → preRender → render, mirroring runGolden's sequence
    // exactly. fixtureDir is empty: the bench never touches fixtures (no
    // compare/regen).
    golden::GoldenHarness harness(std::string(), golden::TimbreSetup::g0Default());
    if (entry.preRender != nullptr) {
        entry.preRender(harness);
    }
    std::vector<int32_t> out(blocks * golden::GoldenHarness::kSamplesPerBlock);
    harness.renderScript(entry.script(), blocks, out.data());

    if (json) {
        std::cout << "{\"script\": \"" << scriptName << "\", \"blocks\": " << blocks
                  << ", \"samples\": " << out.size()
                  << ", \"mode\": \"" << mode << "\"}\n";
    }
    return 0;
}
