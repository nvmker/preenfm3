#!/bin/sh
# Coverage floor gate. Runs llvm-cov report scoped to firmware/Src (headers
# excluded, matching test-coverage-plan.md's 12.45% baseline methodology) and
# FAILS if total line coverage drops below the floor. The floor lives in
# scripts/coverage-floor.txt as a single integer; ratchet it up as coverage
# phases land.
#
# Called by .github/workflows/coverage.yml. Run locally from the repo root
# (after `make test-cov`) with the same args the workflow passes:
#   scripts/ci/coverage-gate.sh build/test-cov/pfm3_tests \
#       build/test-cov/pfm3_tests.profdata \
#       "$(pwd)/firmware/Src" \
#       scripts/coverage-floor.txt
#
# Why this is a separate script (not `make test-cov`): `make test-cov` is the
# MEASUREMENT tool (builds, runs, prints the report, never fails on coverage).
# This script is the GATE (parses the TOTAL, fails on regression only). The
# report scope/flags mirror the `make test-cov` recipe; if you change the scope
# in either place, the 12.45% baseline and the floor stop comparing like-for-like.
#
# llvm-cov resolved from $LLVM_COV (CI's apt install puts it on PATH) or PATH.
# A crash / unreadable-profdata aborts loudly — mirrors scripts/ci/static-analysis.sh.
#
# Usage: coverage-gate.sh <object> <profdata> <firmware_src_abs> <floor_file>
set -u

object=${1:?usage: coverage-gate.sh <object> <profdata> <firmware_src_abs> <floor_file>}
profdata=${2:?missing profdata}
fw_src=${3:?missing firmware_src_abs}
floor_file=${4:?missing floor_file}

llvm_cov=${LLVM_COV:-llvm-cov}

command -v "$llvm_cov" >/dev/null 2>&1 || {
	echo "ERR: llvm-cov '$llvm_cov' not found on PATH" >&2
	exit 1
}
[ -f "$object" ] || {
	echo "ERR: object '$object' not found — run 'make test-cov' first" >&2
	exit 1
}
[ -f "$profdata" ] || {
	echo "ERR: profdata '$profdata' not found — run 'make test-cov' first" >&2
	exit 1
}
[ -d "$fw_src" ] || {
	echo "ERR: firmware src '$fw_src' not found" >&2
	exit 1
}
[ -f "$floor_file" ] || {
	echo "ERR: floor file '$floor_file' not found" >&2
	exit 1
}

# Floor = the first line of the floor file that is ONLY an integer (comments
# and blank lines ignored).
floor=$(grep -E '^[[:space:]]*[0-9]+[[:space:]]*$' "$floor_file" | head -1 | tr -dc '0-9')
[ -n "$floor" ] || {
	echo "ERR: no integer floor found in '$floor_file'" >&2
	exit 1
}

# Run llvm-cov report scoped exactly like `make test-cov`: firmware/Src, headers
# excluded. LC_ALL=C forces a '.' decimal separator so the % parse below is
# locale-independent (mirrors scripts/ci/static-analysis.sh's LC_ALL=C sort).
# Capture output to a variable so we can check llvm-cov's exit code directly
# (POSIX sh has no PIPESTATUS). -summary-only keeps the output to file
# summaries + the TOTAL line (the only line we parse).
#
# TOTAL line field layout (whitespace-split):
#   TOTAL  regions missed cov%  funcs missed exec%  lines missed LINES_COV%  branches missed cov%
#   $1     $2      $3      $4   $5    $6      $7     $8    $9      $10         ...
# so $10 = lines coverage %.
report=$(LC_ALL=C "$llvm_cov" report -summary-only "$object" \
	-instr-profile="$profdata" \
	-use-color=false \
	-ignore-filename-regex='\.h$' \
	"$fw_src" 2>&1)
rc=$?
if [ "$rc" -ne 0 ]; then
	echo "ERR: llvm-cov exited $rc — setup failure (crash / unreadable profdata):" >&2
	printf '%s\n' "$report" | sed 's/^/  /' >&2
	exit 1
fi

pct=$(printf '%s\n' "$report" | awk '/^TOTAL/ {f=$10; gsub(/%/, "", f); print f}')
[ -n "$pct" ] || {
	echo "ERR: could not parse TOTAL line coverage from llvm-cov report:" >&2
	printf '%s\n' "$report" | sed 's/^/  /' >&2
	exit 1
}

# Compare as integers (truncate the fractional part of the measured %). The
# floor is an integer, so this is exact.
pct_int=$(awk -v p="$pct" 'BEGIN{printf "%d", p}')

echo "coverage: ${pct}% lines (floor: ${floor}%)"

if [ "$pct_int" -lt "$floor" ]; then
	{
		echo ""
		echo "FAIL: total line coverage ${pct}% is BELOW the floor ${floor}%."
		echo "This is a coverage regression. Either add tests to restore coverage,"
		echo "or — if the regression is accepted — lower the floor in:"
		echo "  $floor_file"
		echo "and commit the floor change in this same PR."
	} >&2
	exit 1
fi

echo "coverage gate: PASS"
