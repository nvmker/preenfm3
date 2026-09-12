#!/bin/sh
# Perf gate (Callgrind Ir). Runs the pfm3_bench golden-workload driver under
# valgrind/callgrind with --toggle-collect scoped to Synth::buildNewSampleBlock
# and FAILS if any baseline script's total instruction count (Ir) regresses
# more than meta.threshold_pct (default 2%). This is the "did this PR make the
# render slower?" before/after engine — deterministic because guest Ir = f(built
# binary, script input) and the compiler is pinned by the benchmark.yml
# container (gcc:13.3.0-bookworm). Relative deltas and call counts are the
# signal; absolute x86-64 Ir says nothing about the H7's 640k-cycle deadline.
#
# GATE vs MEASUREMENT (house contract, see coverage-gate.sh): this script is
# the GATE — it parses callgrind output and fails on regression only. The
# measurement itself (valgrind invocation) lives here too because it must use
# exactly these flags; `make test` / the bench binary alone measure nothing.
#
# Baseline: scripts/perf-baseline.json (committed). Entries are script
# name -> {blocks, ir_total}; meta pins schema/container/build_type/
# bench_version/valgrind/threshold_pct. A meta mismatch is a HARD FAIL: a
# container or valgrind bump invalidates every Ir number, and comparing across
# toolchains is apples-to-oranges. Regenerate deliberately via the
# regenerate-perf-baseline.yml workflow (manual dispatch, D4) or locally in
# the SAME pinned container:
#   docker run --rm -v "$PWD:$PWD" -w "$PWD" gcc:13.3.0-bookworm \
#     sh -c 'apt-get update && apt-get install -y --no-install-recommends \
#            cmake valgrind jq && \
#            cmake -B build/bench -S tests -DCMAKE_BUILD_TYPE=Release && \
#            cmake --build build/bench --target pfm3_bench -j && \
#            scripts/ci/perf-gate.sh build/bench/pfm3_bench \
#              scripts/perf-baseline.json --regen && \
#            scripts/ci/perf-gate.sh build/bench/pfm3_bench \
#              scripts/perf-baseline.json'
#
# Ir parsing: the total comes from the callgrind out-file's trailing
# `summary: <N>` line (single Ir event column — version-independent). It is
# cross-checked against cg_annotate/callgrind_annotate TOTALS when either tool
# exists — a mismatch aborts loudly rather than gating on a mis-parsed number.
# The per-function top-N report on FAIL uses the same tool (best effort).
#
# Usage: perf-gate.sh <bench_bin> <baseline_json> [--regen] [--threshold=PCT]
# Env:  PERF_CONTAINER  container identity (CI always sets it; compared against
#                       meta.container when non-empty — gate mode only)
#       PERF_BUILD_TYPE expected CMAKE_BUILD_TYPE (default Release)
#       VALGRIND        valgrind binary (default: PATH)
#       PERF_GATE_KEEP_TMP=1   keep the mktemp measurement dir (its path is
#                       printed) — debugging aid
#       PERF_GATE_REPORT_DIR=<dir>  best-effort copy of the per-script
#                       callgrind .out + annotate tables for CI artifacts
# Exit: 0 PASS / new baseline written · 1 gate or setup failure · 2 usage
set -u

# Bump when the bench registry/workload changes materially (meta.bench_version
# hard-fails on mismatch until a deliberate regen).
BENCH_VERSION=1
# Plausibility floor: the cheapest baseline script (a4_default_sustain — 6
# voices, zero IMs, steady sustain) measures ~8.9k Ir/block on the pinned
# container (measured: 1,786,619 Ir / 200 blocks). 2k/block sits ~4.5x below
# that while still catching a broken toggle-collect match (~0 Ir) or a silent
# render (sub-1k/block) — refuse to gate or regen on such numbers.
MIN_IR_PER_BLOCK=2000

usage_die() {
	echo "usage: perf-gate.sh <bench_bin> <baseline_json> [--regen] [--threshold=PCT]" >&2
	exit 2
}

[ $# -ge 2 ] || usage_die
bench_bin=$1
baseline=$2
shift 2

regen=0
threshold_arg=
for arg in "$@"; do
	case "$arg" in
	--regen) regen=1 ;;
	--threshold=*) threshold_arg=${arg#--threshold=} ;;
	*)
		echo "ERR: unknown argument '$arg'" >&2
		usage_die
		;;
	esac
done

[ -x "$bench_bin" ] || {
	echo "ERR: bench binary '$bench_bin' not found/executable — build pfm3_bench first" >&2
	exit 1
}
[ -f "$baseline" ] || {
	echo "ERR: baseline '$baseline' not found" >&2
	exit 1
}

valgrind=${VALGRIND:-valgrind}
command -v "$valgrind" >/dev/null 2>&1 || {
	echo "ERR: valgrind '$valgrind' not found on PATH" >&2
	exit 1
}
command -v jq >/dev/null 2>&1 || {
	echo "ERR: jq not found on PATH (required to read/write the baseline JSON)" >&2
	exit 1
}

annotate_bin=""
if command -v cg_annotate >/dev/null 2>&1; then
	annotate_bin=cg_annotate
elif command -v callgrind_annotate >/dev/null 2>&1; then
	annotate_bin=callgrind_annotate
else
	echo "WARN: neither cg_annotate nor callgrind_annotate found — skipping Ir cross-check and per-function report" >&2
fi

valgrind_version=$("$valgrind" --version 2>/dev/null) || {
	echo "ERR: '$valgrind --version' failed" >&2
	exit 1
}

jq_check() {
	jq -e "$1" "$baseline" >/dev/null 2>&1
}

# --- baseline meta: shape + hard guards --------------------------------------
jq_check 'type == "object" and (.meta | type == "object") and (.ir | type == "object")' || {
	echo "ERR: '$baseline' is not a perf baseline (needs top-level .meta and .ir objects)" >&2
	exit 1
}

schema=$(jq -r '.meta.schema // 0' "$baseline")
bench_version_meta=$(jq -r '.meta.bench_version // 0' "$baseline")
container_meta=$(jq -r '.meta.container // ""' "$baseline")
build_type_meta=$(jq -r '.meta.build_type // ""' "$baseline")
valgrind_meta=$(jq -r '.meta.valgrind // ""' "$baseline")
threshold=$(jq -r '.meta.threshold_pct // 0' "$baseline")

[ -n "$threshold_arg" ] && threshold=$threshold_arg

fail_meta() {
	{
		echo ""
		echo "FAIL: baseline meta mismatch — $1"
		echo "The committed Ir numbers are only valid for the exact toolchain they"
		echo "were measured with. Regenerate deliberately via the dispatch workflow"
		echo "(.github/workflows/regenerate-perf-baseline.yml) on your PR branch, or"
		echo "see the header of this script for the local docker one-liner."
	} >&2
	exit 1
}

# Schema check always runs (both modes): this script only understands schema 1.
[ "$schema" = "1" ] || fail_meta "schema is '$schema', this gate understands 1"
case "$threshold" in
'' | *[!0-9]*)
	echo "ERR: threshold '${threshold}' is not a positive integer percent" >&2
	exit 1
	;;
esac
[ "$threshold" -ge 1 ] && [ "$threshold" -le 100 ] || {
	echo "ERR: threshold '${threshold}' outside 1..100" >&2
	exit 1
}

if [ "$regen" -eq 0 ]; then
	# Gate mode: every identity pin must match the environment exactly.
	[ "$bench_version_meta" = "$BENCH_VERSION" ] ||
		fail_meta "bench_version is '$bench_version_meta', gate expects '$BENCH_VERSION' (workload definition changed)"
	if [ -n "${PERF_CONTAINER:-}" ]; then
		[ "$container_meta" = "$PERF_CONTAINER" ] ||
			fail_meta "container is '$container_meta', running in '$PERF_CONTAINER'"
	else
		echo "WARN: PERF_CONTAINER unset — container identity check skipped (CI always sets it)" >&2
	fi
	expected_build_type=${PERF_BUILD_TYPE:-Release}
	[ "$build_type_meta" = "$expected_build_type" ] ||
		fail_meta "build_type is '$build_type_meta', expected '$expected_build_type' (PERF_BUILD_TYPE)"
	[ "$valgrind_meta" = "$valgrind_version" ] ||
		fail_meta "valgrind is '$valgrind_meta', running '$valgrind_version'"
else
	# Regen mode: the environment IS the new truth; the workflow/container pin
	# is what makes this deliberate rather than drift. Validate what stays.
	[ -n "$build_type_meta" ] || {
		echo "ERR: regen template must carry meta.build_type" >&2
		exit 1
	}
	[ -n "$container_meta" ] || {
		echo "ERR: regen template must carry meta.container" >&2
		exit 1
	}
fi

# Zero-baseline guard (gate mode): an entry with ir_total <= 0 would divide to
# infinity below — or worse, pass vacuously after a broken measurement wrote
# zeros. The baseline must carry real numbers.
if [ "$regen" -eq 0 ]; then
	zero_entries=$(jq -r '[.ir | to_entries[] | select((.value.ir_total // 0) <= 0) | .key] | join(", ")' "$baseline")
	[ -z "$zero_entries" ] || {
		echo "ERR: baseline entries with non-positive ir_total: $zero_entries" >&2
		echo "     regenerate the baseline (see this script's header) — a zero Ir" >&2
		echo "     baseline can never gate anything." >&2
		exit 1
	}
fi

scripts_list=$(jq -r '.ir | keys[]' "$baseline")
[ -n "$scripts_list" ] || {
	echo "ERR: baseline has no .ir entries" >&2
	exit 1
}

# --- measurement --------------------------------------------------------------
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/pfm3-perf-gate.XXXXXX") || exit 1
if [ "${PERF_GATE_KEEP_TMP:-0}" = "1" ]; then
	trap 'echo "perf-gate: measurement dir kept: $tmp_dir"' EXIT INT TERM
else
	trap 'rm -rf "$tmp_dir"' EXIT INT TERM
fi

# --toggle-collect='*buildNewSampleBlock*': count ONLY inside the render window
# (the function + its callees) — determinism pitfall #4, solved at the tool
# level. The demangled name is 'Synth::buildNewSampleBlock()', so the exact
# form would need the full signature; the '*' anchors the unique substring.
# The call is out-of-line across TUs (bench links the object library, no LTO),
# so -O3 cannot inline it away and silently collect nothing.

measure_script() {
	ms_name=$1
	ms_blocks=$(jq -r --arg s "$ms_name" '.ir[$s].blocks // 0' "$baseline")
	case "$ms_blocks" in
	'' | *[!0-9]*)
		echo "ERR: baseline entry '$ms_name' has non-integer blocks '$ms_blocks'" >&2
		exit 1
		;;
	esac
	[ "$ms_blocks" -ge 1 ] || {
		echo "ERR: baseline entry '$ms_name' has blocks < 1" >&2
		exit 1
	}

	ms_out="$tmp_dir/cg-$ms_name.out"
	ms_log="$tmp_dir/bench-$ms_name.log"
	if ! "$valgrind" --tool=callgrind \
		--toggle-collect='*buildNewSampleBlock*' \
		--callgrind-out-file="$ms_out" \
		-- "$bench_bin" --script="$ms_name" --blocks="$ms_blocks" --mode=ir \
		>"$ms_log" 2>&1; then
		echo "ERR: valgrind/bench failed for script '$ms_name' (exit $?)" >&2
		tail -20 "$ms_log" >&2
		exit 1
	fi

	# Total Ir from the out-file's summary line (the file's `events: Ir`
	# header defines one column, so the bare number IS the Ir total; the file
	# only contains toggle-collected events, so its summary is the
	# render-window total).
	ms_ir=$(sed -n 's/^summary:[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$ms_out" | tail -1)
	[ -n "$ms_ir" ] || {
		echo "ERR: no 'summary:' line in callgrind output for '$ms_name' —" >&2
		echo "     valgrind/callgrind output format changed, or nothing was recorded" >&2
		echo "     (toggle-collect mismatch?). See $ms_out" >&2
		exit 1
	}
	[ "$ms_ir" -gt 0 ] || {
		echo "ERR: callgrind recorded Ir=0 inside buildNewSampleBlock for '$ms_name'." >&2
		echo "     A render executes millions of instructions — zero means the" >&2
		echo "     toggle-collect pattern matched nothing. See $ms_out" >&2
		exit 1
	}
	ms_floor=$((ms_blocks * MIN_IR_PER_BLOCK))
	[ "$ms_ir" -ge "$ms_floor" ] || {
		echo "ERR: Ir=$ms_ir for '$ms_name' is below the plausibility floor" >&2
		echo "     ($MIN_IR_PER_BLOCK/block x $ms_blocks blocks = $ms_floor). The render" >&2
		echo "     window cannot be that cheap — the toggle-collect scope is wrong or the" >&2
		echo "     render is silent. Inspect $ms_out" >&2
		exit 1
	}

	# Cross-check against annotate TOTALS when available: a silent parse drift
	# must never gate a build.
	if [ -n "$annotate_bin" ]; then
		"$annotate_bin" --threshold=99 "$ms_out" >"$tmp_dir/annotate-$ms_name.txt" 2>/dev/null
		ms_totals=$(grep '^TOTALS' "$tmp_dir/annotate-$ms_name.txt" | tail -1 | tr -dc '0-9')
		if [ -n "$ms_totals" ]; then
			[ "$ms_totals" = "$ms_ir" ] || {
				echo "ERR: Ir parse mismatch for '$ms_name': summary=$ms_ir annotate=$ms_totals" >&2
				echo "     refusing to gate on an ambiguous number — see $ms_out" >&2
				exit 1
			}
		fi
	fi

	echo "$ms_ir"
}

echo "perf gate: container='$container_meta' build='$build_type_meta' valgrind='$valgrind_version' threshold=${threshold}%"
echo ""
printf '%-22s %7s %16s %16s %10s\n' "script" "blocks" "baseline Ir" "measured Ir" "delta %"
echo "-------------------------------------------------------------------------"

fail_scripts=""
regen_tmp="$tmp_dir/baseline.new"
cp "$baseline" "$regen_tmp"

for s in $scripts_list; do
	base_ir=$(jq -r --arg k "$s" '.ir[$k].ir_total // 0' "$baseline")
	# measure_script exits non-zero from inside a command substitution — without
	# the explicit || propagation a subshell exit only blanks $measured.
	measured=$(measure_script "$s") || exit 1
	case "$measured" in
	'' | *[!0-9]*)
		echo "ERR: measurement for '$s' returned '$measured' (internal error)" >&2
		exit 1
		;;
	esac
	blocks=$(jq -r --arg k "$s" '.ir[$k].blocks' "$baseline")

	if [ "$regen" -eq 1 ]; then
		jq --arg k "$s" --argjson v "$measured" '.ir[$k].ir_total = $v' \
			"$regen_tmp" >"$regen_tmp.next" || exit 1
		mv "$regen_tmp.next" "$regen_tmp"
		delta_pct=0.0000
	else
		delta_pct=$(awk -v m="$measured" -v b="$base_ir" 'BEGIN{printf "%.4f", (m-b)*100.0/b}')
	fi
	printf '%-22s %7d %16d %16d %10.4f\n' "$s" "$blocks" "$base_ir" "$measured" "$delta_pct"

	if [ "$regen" -eq 0 ]; then
		over=$(awk -v m="$measured" -v b="$base_ir" -v t="$threshold" \
			'BEGIN{print (m > b*(1.0+t/100.0)) ? 1 : 0}')
		[ "$over" -eq 1 ] && fail_scripts="$fail_scripts $s"
		faster=$(awk -v m="$measured" -v b="$base_ir" 'BEGIN{print (m < b*0.90) ? 1 : 0}')
		[ "$faster" -eq 1 ] && echo "::notice::script '$s' is >10% faster than baseline — consider regenerating scripts/perf-baseline.json to lock the speedup in"
	fi
done

# --- verdict -------------------------------------------------------------------
# Best-effort report export for CI artifact upload (never gates).
if [ -n "${PERF_GATE_REPORT_DIR:-}" ] && [ -d "$tmp_dir" ]; then
	mkdir -p "$PERF_GATE_REPORT_DIR" 2>/dev/null &&
		cp "$tmp_dir"/cg-*.out "$tmp_dir"/bench-*.log "$PERF_GATE_REPORT_DIR"/ 2>/dev/null
	[ -z "$annotate_bin" ] ||
		cp "$tmp_dir"/annotate-*.txt "$PERF_GATE_REPORT_DIR"/ 2>/dev/null
fi

if [ "$regen" -eq 1 ]; then
	jq --arg v "$valgrind_version" '.meta.valgrind = $v' \
		"$regen_tmp" >"$regen_tmp.next" || exit 1
	mv "$regen_tmp.next" "$regen_tmp"
	if [ -n "$threshold_arg" ]; then
		jq --argjson t "$threshold" '.meta.threshold_pct = $t' \
			"$regen_tmp" >"$regen_tmp.next" || exit 1
		mv "$regen_tmp.next" "$regen_tmp"
	fi
	if [ -n "${PERF_CONTAINER:-}" ]; then
		jq --arg c "$PERF_CONTAINER" '.meta.container = $c' \
			"$regen_tmp" >"$regen_tmp.next" || exit 1
		mv "$regen_tmp.next" "$regen_tmp"
	fi
	# Canonical formatting (2-space indent + trailing newline) so regens diff minimally.
	jq '.' "$regen_tmp" >"$baseline" || {
		echo "ERR: failed to write regenerated baseline to '$baseline'" >&2
		exit 1
	}
	echo ""
	echo "perf gate: REGENERATED '$baseline' (threshold ${threshold}%)"
	echo "Self-verify before committing: run this script WITHOUT --regen against the"
	echo "new baseline — it must PASS at 0.0000% deltas."
	exit 0
fi

if [ -n "$fail_scripts" ]; then
	{
		echo ""
		echo "FAIL: Ir regression beyond ${threshold}% for:$fail_scripts"
		echo "This change made the render path measurably slower. Fix the regression,"
		echo "or — if it is an accepted trade-off — regenerate the baseline via"
		echo ".github/workflows/regenerate-perf-baseline.yml (dispatch on your PR"
		echo "branch) and commit scripts/perf-baseline.json in the same PR."
	} >&2
	for s in $fail_scripts; do
		if [ -n "$annotate_bin" ] && [ -s "$tmp_dir/annotate-$s.txt" ]; then
			echo "" >&2
			echo "top functions for '$s':" >&2
			sed -n 's/^/  /p' "$tmp_dir/annotate-$s.txt" | head -25 >&2
		fi
	done
	exit 1
fi

echo ""
echo "perf gate: PASS"
