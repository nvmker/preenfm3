#!/bin/sh
# Runs clang-tidy (the SECONDARY analyzer) over our firmware/lib TUs.
# Called by `make analyze`.
#
# TU selection: extracts "file" entries from compile_commands.json so the
# vendor dirs (firmware/Drivers, firmware/Middlewares — present in the DB) are
# filtered out, ours only (firmware/Src + lib/Src).
#
# CROSS-COMPILE INCLUDE RESOLUTION
# clang-tidy (libclang) does not replicate GCC's libstdc++ / multilib / newlib
# header discovery for bare-metal arm-none-eabi targets, so a naive run errors
# on every TU: 'math.h'/'stdio.h'/'algorithm'/... file not found (newlib +
# libstdc++ headers live in the Arm GNU toolchain's sysroot, which clang never
# searches), plus a cascading uint32_t typedef clash (clang models the ARM
# 32-bit int as 'unsigned int', GCC-ARM as 'unsigned long'; FatFs's integer.h
# relies on the GCC-ARM form), plus '-mslow-flash-data' is a GCC ARM codegen
# flag clang doesn't model. We fix all three so every TU is analyzed in full:
#   1. Resolve the cross-g++ the build actually used and ask it (with the same
#      MCU flags, so the right multilib variant e.g. thumb/v7e-m+dp/hard is
#      selected) for its real '#include <...>' search list; inject each system
#      dir as --extra-arg=-isystem. GCC's own fixed-include dirs
#      (.../lib/gcc/<triple>/<ver>/include{,-fixed}) are dropped — clang ships
#      its own stddef.h/stdarg.h/etc. in its resource dir and using GCC's would
#      shadow them.
#   2. Pass --target=arm-none-eabi so clang applies the ARM data model, and
#      override __UINT32_TYPE__/__INT32_TYPE__ to GCC-ARM's 'unsigned long int'
#      / 'long int' so uint32_t/int32_t match what arm-none-eabi-gcc defines
#      (resolves the FatFs integer.h / ff.h typedef-redefinition errors).
#   3. Sanitize compile_commands.json into a temp build dir with
#      -mslow-flash-data stripped (it only selects a data-placement section at
#      link/codegen time; dropping it has zero effect on static analysis) and
#      point clang-tidy (-p) at the sanitized DB.
# See _bmad-output/static-analysis/spike-findings.md "Coverage caveats".
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

# --- Cross-compile include resolution ---------------------------------------
# The cross-g++ the build used is the first whitespace token of the first
# "command" in the DB. Fall back to arm-none-eabi-gcc on PATH if it's gone.
gcc_bin=$(grep -o '"command": *"[^"]*"' "$db" | head -1 |
	sed 's/.*"command": *"//; s/"$//' | awk '{print $1}')
command -v "$gcc_bin" >/dev/null 2>&1 || gcc_bin=arm-none-eabi-gcc

# Reuse the SAME MCU flags the firmware compiles with so g++ resolves the
# matching multilib variant (e.g. thumb/v7e-m+dp/hard for cortex-m7 + fpv5-d16
# + hard-float). -mthumb is a bare flag (no '=value'), the rest are key=value.
mcu_flags=$(grep -o '"command": *"[^"]*"' "$db" | head -1 |
	sed 's/.*"command": *"//; s/"$//' |
	grep -oE -- '-mcpu=[^ ]*|-mfpu=[^ ]*|-mfloat-abi=[^ ]*|-mthumb')

# Ask the cross-g++ for its real '#include <...>' search list (printed to
# stderr by -v) and keep only the system include dirs (drop GCC's fixed
# includes — clang has its own). Empty result => the toolchain probe failed;
# abort loudly rather than run clang-tidy against headers it can't resolve.
#
# $mcu_flags is a space-separated flag list; POSIX sh has no arrays, so it must
# word-split into separate argv entries (likewise $tus further down).
# shellcheck disable=SC2086
probe_err=$(printf '#include <algorithm>\n' | "$gcc_bin" $mcu_flags -E -x c++ -v - 2>&1 >/dev/null)
sys_includes=$(printf '%s\n' "$probe_err" |
	sed -n '/#include <\.\.\.> search starts here:/,/End of search list/p' |
	grep -E '^ /' | sed 's/^ //' |
	while IFS= read -r p; do
		# Resolve '..' and symlinks so the trailing grep -v can reliably spot
		# GCC's fixed-include dirs. cd && pwd -P prints the normalized path
		# (and silently skips any non-dir entry, which shouldn't occur here).
		cd -- "$p" 2>/dev/null && pwd -P
	done |
	grep -v '/lib/gcc/') # drop GCC fixed includes; clang ships its own
[ -n "$sys_includes" ] || {
	echo "ERR: could not enumerate cross-toolchain include dirs from '$gcc_bin'" >&2
	echo "     (probe: '$gcc_bin' $mcu_flags -E -x c++ -v). Check the toolchain install." >&2
	exit 1
}

# Sanitize the DB: strip -mslow-flash-data (GCC ARM codegen flag; clang errors
# 'unknown argument'). Write the filtered DB under build_dir so it's inspectable
# alongside the real one (and cleared by `make clean`), then point clang-tidy
# (-p) at it. The token is literal and JSON-safe (no quotes / backslashes), so
# a text replace suffices and we avoid a python/jq dependency.
work="$build_dir/clang-tidy-db"
rm -rf "$work"
mkdir -p "$work"
sed 's/ -mslow-flash-data//g' "$db" >"$work/compile_commands.json"

# Build the clang-tidy extra-args via positional params so the space inside the
# -D override values survives as a single argv element (--extra-arg=-DNAME=a b c
# must reach clang as ONE arg).
set --
for d in $sys_includes; do
	set -- "$@" --extra-arg=-isystem --extra-arg="$d"
done
set -- "$@" --extra-arg=--target=arm-none-eabi
set -- "$@" --extra-arg=-U__UINT32_TYPE__ --extra-arg=-D__UINT32_TYPE__=unsigned\ long\ int
set -- "$@" --extra-arg=-U__INT32_TYPE__ --extra-arg=-D__INT32_TYPE__=long\ int
# Downgrade C++ brace-init narrowing from clang's default hard error to a
# warning so clang-tidy fully analyzes every TU (it otherwise aborts mid-TU on
# the first narrowing site, e.g. lib/Src/Encoders.cpp, and that abort cascades
# to 'Error while processing' on every later TU in the batch). arm-none-eabi-gcc
# already emits these as -Wnarrowing WARNINGS (lib isn't -Werror), so this just
# matches the real compiler's strictness — the findings still surface, as
# [clang-diagnostic-narrowing] warnings, no longer hidden behind a hard stop.
set -- "$@" --extra-arg=-Wno-error=narrowing

# clang-tidy returns nonzero on findings / compile-errors; for a spike we want
# the full output regardless, so don't let that abort the run. Setup errors are
# already caught above and DO fail. $tus (see above) is a newline-separated
# list of TU paths that must word-split into one argv entry per TU.
# shellcheck disable=SC2086
"$tidy" -p "$work" --quiet "$@" $tus 2>&1 | tee "$out" || true
