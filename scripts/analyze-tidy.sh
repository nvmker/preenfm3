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
# Usage: analyze-tidy.sh <compile_commands.json> <build_dir> <out.txt>
set -u

db=${1:?usage: analyze-tidy.sh <compile_commands.json> <build_dir> <out.txt>}
build_dir=${2:?missing build_dir}
out=${3:?missing out path}

# TU "file" entries from the DB, ours only (firmware/Src + lib/Src), deduped.
tus=$(grep -o '"file": "[^"]*"' "$db" |
	sed 's/"file": "//;s/"//' |
	grep -E '/(firmware/Src|lib/Src)/' |
	sort -u)

# clang-tidy returns nonzero on findings / compile-errors; for a spike we want
# the full output regardless, so don't let that abort the run.
clang-tidy -p "$build_dir" --quiet $tus 2>&1 | tee "$out" || true
