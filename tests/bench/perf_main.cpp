// pfm3_bench — standalone perf driver for the CI Callgrind Ir gate.
//
// Renders the golden-master workload deterministically and exits 0 on success;
// the MEASUREMENT happens outside this process: scripts/ci/perf-gate.sh runs
// it under `valgrind --tool=callgrind --toggle-collect='*buildNewSampleBlock*'`
// so only the render window (buildNewSampleBlock + callees) is counted. This
// binary therefore prints nothing the gate parses — stdout is for humans
// (--json) only. See _bmad-output/planning-artifacts/ci-performance-proposal.md
// (three-signal design) + ci-performance-implementation-plan.md §2.2.
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
//   pfm3_bench --script=<name> [--blocks=N] [--mode=ir] [--json]
//              [--repeat=R] [--warmup=W]
//   pfm3_bench --list        one registry key per line (for perf-gate.sh)
//
//   --script   registry key (required). Unknown name → registry list, exit 2.
//   --blocks   render block count (default: the entry's golden count). Must be
//              a plain decimal integer ≥ 1 (strtoul quirk: '-1' would wrap to
//              ULONG_MAX and blow up the output allocation — reject signs).
//   --mode     ir  — render once, exit 0 (Callgrind measures around us).
//              wall — NOT IMPLEMENTED YET (Phase 3); exit 3.
//   --json     print a machine-readable summary line (humans/diagnostics).
//   --repeat / --warmup — accepted for CLI-shape compatibility, used by
//              --mode=wall only (Phase 3); ignored in ir mode.
//
// Exit codes: 0 success · 2 usage/unknown script · 3 unsupported mode.

#include "../golden_harness.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
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
              << " --script=<name> [--blocks=N] [--mode=ir] [--json]"
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
        } else if (flag == "--repeat" || flag == "--warmup") {
            // Phase 3 (wall mode) CLI shape — parsed for acceptance, unused in ir mode.
        } else {
            std::cerr << "ERR: unknown argument '" << arg << "'\n";
            printUsage(argv[0]);
            return 2;
        }
    }

    if (mode != "ir") {
        std::cerr << "ERR: --mode=" << mode
                  << " is not implemented (Phase 3 — wall-clock trend). "
                     "Use --mode=ir.\n";
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

    // Construct → preRender → render, mirroring runGolden's sequence exactly.
    // fixtureDir is empty: the bench never touches fixtures (no compare/regen).
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
