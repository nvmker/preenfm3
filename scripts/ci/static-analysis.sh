#!/bin/sh
# cppcheck baseline gate. Runs cppcheck over firmware/Src + lib/Src (same flags
# as `make analyze`, plus -ibootloader to tighten the scope to the code we own)
# and FAILS on findings NEW vs the checked-in baseline. Findings already in the
# baseline pass; findings resolved since the baseline pass too (with a hint to
# shrink the baseline). See scripts/cppcheck-baseline.txt for the contract.
#
# Called by .github/workflows/static-analysis.yml. Run locally from the repo
# root (after `make firmware`) with the same args the workflow passes:
#   scripts/ci/static-analysis.sh build/release/compile_commands.json /tmp scripts/cppcheck-baseline.txt
#
# Why this is a separate script (not `make analyze`): `make analyze` is the
# TOLERANT triage tool (runs cppcheck + clang-tidy, never fails on findings).
# This script is the GATE (fails on new findings, cppcheck only). The cppcheck
# flags mirror the Makefile `analyze` recipe; the only divergence is the added
# -ibootloader (see D1 in the PR that landed this file). If you change the flags
# in either place, regenerate the baseline.
#
# cppcheck returns 0 on success-with-findings (we don't pass --error-exitcode),
# so any non-zero exit here is a real setup/crash failure (missing binary,
# unreadable DB) and aborts loudly — mirrors scripts/analyze-tidy.sh's philosophy.
#
# Usage: static-analysis.sh <compile_commands.json> <out_dir> <baseline>
set -u

db=${1:?usage: static-analysis.sh <compile_commands.json> <out_dir> <baseline>}
out_dir=${2:?missing out_dir}
baseline=${3:?missing baseline path}

# cppcheck resolved from $CPPCHECK (CI pins 2.21.0 via PATH) or PATH. The cwd is
# the repo root (both in CI and for the documented local invocation), so the
# relative suppression-list path below resolves.
cppcheck=${CPPCHECK:-cppcheck}

command -v "$cppcheck" >/dev/null 2>&1 || {
	echo "ERR: cppcheck '$cppcheck' not found on PATH" >&2
	exit 1
}
[ -f "$db" ] || {
	echo "ERR: compile_commands '$db' not found — run 'make firmware' first" >&2
	exit 1
}
[ -f "$baseline" ] || {
	echo "ERR: baseline '$baseline' not found" >&2
	exit 1
}

mkdir -p "$out_dir"
raw="$out_dir/analyze-cppcheck.txt"
current="$out_dir/cppcheck-current.txt"
base="$out_dir/cppcheck-baseline-normalized.txt"
new="$out_dir/cppcheck-new.txt"
resolved="$out_dir/cppcheck-resolved.txt"

# Run cppcheck. Output is gcc-template findings on stderr; merge to the raw file.
# Same flags as `make analyze` + -ibootloader (scope: firmware/Src + lib/Src).
"$cppcheck" --quiet --enable=warning,style --inline-suppr \
	--suppressions-list=scripts/cppcheck-suppressions.txt \
	--platform=arm32-wchar_t2 \
	--project="$db" \
	-ifirmware/Drivers -ifirmware/Middlewares -ibootloader \
	>"$raw" 2>&1
rc=$?
if [ "$rc" -ne 0 ]; then
	echo "ERR: cppcheck exited $rc — setup failure (crash / unreadable DB):" >&2
	sed 's/^/  /' "$raw" >&2
	exit 1
fi

# Normalize: finding lines only (path:line:col: severity: msg [id]), trailing
# whitespace trimmed, LC_ALL=C sort+dedup so macOS (BSD sort) and the CI runner
# (GNU sort) agree byte-for-byte (otherwise `comm` mismatches on punctuation/
# case collation). Comment lines (#) are dropped from the baseline.
grep -E ": (warning|style|error|portability|performance): " "$raw" |
	sed 's/[[:space:]]*$//' |
	LC_ALL=C sort -u >"$current"
grep -v -E '^[[:space:]]*#' "$baseline" |
	sed 's/[[:space:]]*$//' |
	LC_ALL=C sort -u >"$base"

# NEW = in current, not in baseline. RESOLVED = in baseline, not in current.
LC_ALL=C comm -23 "$current" "$base" >"$new"
LC_ALL=C comm -13 "$current" "$base" >"$resolved"

total=$(wc -l <"$current" | tr -d ' ')
new_count=$(wc -l <"$new" | tr -d ' ')
resolved_count=$(wc -l <"$resolved" | tr -d ' ')

echo "cppcheck: $total findings ($new_count new vs baseline, $resolved_count resolved)"

if [ "$new_count" -gt 0 ]; then
	{
		echo ""
		echo "FAIL: $new_count NEW cppcheck finding(s) not in the baseline:"
		echo "---- new findings (path:line: severity: message [id]) ----"
		sed 's/^/  /' "$new"
		echo "----------------------------------------------------------"
		echo ""
		echo "Either fix the finding, or — if accepted as noise — add it to:"
		echo "  $baseline"
		echo "and commit the baseline update in this same PR."
	} >&2
	exit 1
fi

if [ "$resolved_count" -gt 0 ]; then
	{
		echo ""
		echo "NOTE: $resolved_count baseline finding(s) no longer reported — shrink the baseline:"
		sed 's/^/  /' "$resolved"
		echo "  cp $current $baseline"
		echo "and commit the baseline update in this same PR."
	} >&2
fi

echo "cppcheck baseline gate: PASS"
