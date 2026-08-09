#!/bin/sh
# Runs clang-tidy (the SECONDARY analyzer) over our firmware/lib TUs.
# Called by `make analyze`. Extracts TU paths from compile_commands.json so the
# vendor dirs (firmware/Drivers, firmware/Middlewares — present in the DB) are
# filtered out, then hands the list to clang-tidy with the build dir for flag
# resolution. clang-tidy may emit cross-compile noise from arm-none-eabi-gcc
# flags it doesn't model (-mcpu/-mfpu/etc.); that is expected on the first spike
# run and noted in the triage report
# (_bmad-output/static-analysis/spike-findings.md).
#
# Usage: analyze-tidy.sh <compile_commands.json> <build_dir> <out.txt> <clang-tidy>
set -u

db=${1:?usage: analyze-tidy.sh <compile_commands.json> <build_dir> <out.txt> <clang-tidy>}
build_dir=${2:?missing build_dir}
out=${3:?missing out path}
tidy=${4:?missing clang-tidy executable}

# Fail loudly on SETUP errors so `make analyze` can't silently skip this
# analyzer. Only clang-tidy's OWN post-launch exit (nonzero on findings /
# compile-errors) is tolerated below — that's the spike's "full output
# regardless" contract. A missing binary, an unreadable DB, or an empty TU list
# are all setup failures that must abort, not be masked by the trailing || true.
command -v "$tidy" >/dev/null 2>&1 || {
	echo "ERR: clang-tidy '$tidy' not found on PATH" >&2
	exit 1
}
[ -f "$db" ] || {
	echo "ERR: compile_commands '$db' not found — run 'make firmware' first" >&2
	exit 1
}

# TU "file" entries from the DB, ours only (firmware/Src + lib/Src), deduped.
tus=$(grep -o '"file": "[^"]*"' "$db" |
	sed 's/"file": "//;s/"//' |
	grep -E '/(firmware/Src|lib/Src)/' |
	sort -u)

[ -n "$tus" ] || {
	echo "ERR: no firmware/lib TUs extracted from '$db' — aborting (clang-tidy would run on nothing)" >&2
	exit 1
}

# clang-tidy returns nonzero on findings / compile-errors; for a spike we want
# the full output regardless, so don't let that abort the run. Setup errors are
# already caught above and DO fail.
"$tidy" -p "$build_dir" --quiet $tus 2>&1 | tee "$out" || true
