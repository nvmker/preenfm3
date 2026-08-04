#!/usr/bin/env bash
# Extract preenfm3 firmware + bootloader versions from their C headers.
#
# Reads:
#   firmware/Inc/version.h      -> #define PFM3_FIRMWARE_VERSION   "v1.03"
#   bootloader/Inc/version.h    -> #define PFM3_BOOTLOADER_VERSION "1.09"
#
# Prints one line of shell-assignable key=value pairs:
#   FW=<file-form> BL=<file-form> FWRaw=<raw> BLRaw=<raw>
#
# File form: a leading v/V is stripped and '.' -> '_'
#   "v1.03" -> 1_03 ,  "1.09" -> 1_09
#
# Used by the CI workflows to name release artifacts. The version headers are
# the single source of truth for file naming; the git tag is only the trigger.
#
# Exit non-zero if a macro is missing or empty so CI fails loudly on a malformed
# header instead of shipping a mis-named release.
set -euo pipefail

# Resolve repo root from this script's location so it works from any cwd.
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
fw_h="$repo_root/firmware/Inc/version.h"
bl_h="$repo_root/bootloader/Inc/version.h"

# extract_raw <header> <macro>  -> echoes the quoted string value
extract_raw() {
	local header="$1" macro="$2" val cnt
	val="$(sed -nE \
		"s/^[[:space:]]*#[[:space:]]*define[[:space:]]+${macro}[[:space:]]+\"([^\"]+)\".*/\1/p" \
		"$header")"
	if [ -z "$val" ]; then
		echo "error: ${macro} not found (or empty) in ${header}" >&2
		exit 1
	fi
	cnt="$(printf '%s\n' "$val" | grep -c .)"
	if [ "$cnt" -gt 1 ]; then
		echo "error: ${macro} defined $cnt times in ${header}" >&2
		exit 1
	fi
	printf '%s' "$val"
}

# to_fileform <raw>  -> echoes the file-name form (strip leading v/V, . -> _)
to_fileform() {
	local v="$1"
	v="${v#[vV]}"
	v="${v//./_}"
	printf '%s' "$v"
}

fw_raw="$(extract_raw "$fw_h" PFM3_FIRMWARE_VERSION)"
bl_raw="$(extract_raw "$bl_h" PFM3_BOOTLOADER_VERSION)"

printf 'FW=%s BL=%s FWRaw=%s BLRaw=%s\n' \
	"$(to_fileform "$fw_raw")" \
	"$(to_fileform "$bl_raw")" \
	"$fw_raw" "$bl_raw"
