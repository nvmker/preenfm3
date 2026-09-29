#!/bin/sh
# Firmware size gate. Runs arm-none-eabi-size -A on the built Release ELF,
# classifies every dot-section into a firmware/preenfm3_2MB.ld memory region,
# and FAILS if any region exceeds its budget. Budgets live in
# scripts/size-budget.txt as region=bytes lines.
#
# Called by .github/workflows/build.yml. Run locally from the repo root
# (after `make all`) with the same args the workflow passes:
#   scripts/ci/size-gate.sh build/release/firmware/preenfm3.elf scripts/size-budget.txt
#
# Why this is a separate script (not the linker): the linker is the FIT check
# (it already hard-fails when a region overflows its hardware capacity).
# This script is the GATE (per-region budgets below capacity, plus per-run
# utilization visibility in the step summary). The section -> region mapping
# below mirrors the linker script — if you change either, change both in the
# same PR:
#   flash    .isr_vector .text .rodata .ARM.extab .ARM .preinit_array
#            .init_array .fini_array + .data (the .data LOAD image lives in
#            FLASH via `AT> FLASH`)
#   dtcmram  .data .bss ._user_heap_stack (.data is counted in BOTH flash
#            and dtcmram by design: flash holds the load image, dtcmram the
#            runtime copy)
#   ram_d1/ram_d2/ram_d2b/ram_d3  .ram_d1/.ram_d2/.ram_d2b/.ram_d3
#   itcmram  .instruction_ram
# Not counted (not allocated by the linker script): .ARM.attributes,
# .comment, .debug_*. Any OTHER dot-section aborts hard — a linker-script
# change must be a deliberate classification here, not a silent escape from
# the accounting. A region measured in the ELF with no budget line also
# aborts (the budget must cover every region; extra unused lines are OK).
# Budget values must be whole bytes > 0, no leading zeros, within the shell
# integer range, at or below the region's hardware capacity, and one line
# per region — anything else aborts (a nonsense budget must never silently
# disable the gate).
#
# arm-none-eabi-size resolved from $ARM_SIZE (test seam, mirrors $LLVM_COV in
# coverage-gate.sh) or PATH. A crash aborts loudly.
#
# Usage: size-gate.sh <elf> <budget_file>
set -u

elf=${1:?usage: size-gate.sh <elf> <budget_file>}
budget_file=${2:?missing budget_file}

arm_size=${ARM_SIZE:-arm-none-eabi-size}

command -v "$arm_size" >/dev/null 2>&1 || {
	echo "ERR: size tool '$arm_size' not found on PATH" >&2
	exit 1
}
[ -f "$elf" ] || {
	echo "ERR: ELF '$elf' not found — run 'make all' first" >&2
	exit 1
}
[ -f "$budget_file" ] || {
	echo "ERR: budget file '$budget_file' not found" >&2
	exit 1
}

# Hardware region capacities per firmware/preenfm3_2MB.ld (flash = the 1920K
# app partition). Used for the "% of hardware capacity" column AND to reject
# budgets at/above capacity (those would silently reduce the gate to the
# linker's own overflow check).
hw='flash=1966080 dtcmram=131072 ram_d1=524288 ram_d2=131072 ram_d2b=131072 ram_d3=65536 itcmram=64512'

# getval <region=bytes list> <region> — prints the region's bytes, or nothing.
getval() {
	# shellcheck disable=SC2086 # iterate the whitespace-split list
	for entry in $1; do
		case $entry in
		"$2"=*)
			printf '%s\n' "${entry#*=}"
			return
			;;
		esac
	done
}

# Run size -A (per-section listing: `section size addr`). Capture output to a
# variable so we can check the exit code directly (POSIX sh has no
# PIPESTATUS).
size_out=$(LC_ALL=C "$arm_size" -A "$elf" 2>&1)
rc=$?
if [ "$rc" -ne 0 ]; then
	echo "ERR: $arm_size exited $rc — setup failure (crash / not an ELF):" >&2
	printf '%s\n' "$size_out" | sed 's/^/  /' >&2
	exit 1
fi

# Budget file: one `region=bytes` per line; blank lines, `#` comments and
# inline `# ...` annotations are ignored. Region names without a
# classification are allowed (ignored); duplicate regions, non-positive
# values, leading zeros, out-of-range values and values above the region's
# hardware capacity all abort loudly.
budgets=''
while IFS= read -r line || [ -n "$line" ]; do
	line=${line%%#*}
	# shellcheck disable=SC2086 # whitespace-split what's left of the line
	set -- $line
	[ "$#" -eq 1 ] || continue
	case $1 in
	[a-zA-Z0-9_]*=*)
		region=${1%%=*}
		bytes=${1#*=}
		case $bytes in
		'' | *[!0-9]* | 0 | 0[0-9]*)
			echo "ERR: bad budget line '$1' in '$budget_file' — want region=bytes (whole bytes > 0, no leading zeros)" >&2
			exit 1
			;;
		esac
		if [ "${#bytes}" -gt 15 ]; then
			echo "ERR: budget '$1' in '$budget_file' out of range (max 15 digits)" >&2
			exit 1
		fi
		case " $budgets " in
		*" $region="*)
			echo "ERR: duplicate budget line for '$region' in '$budget_file' — keep exactly one line per region" >&2
			exit 1
			;;
		*) budgets="$budgets $region=$bytes" ;;
		esac
		;;
	esac
done <"$budget_file"

# A budget at/above the region's hardware capacity would silently reduce the
# gate to the linker's own overflow check; budgets are growth budgets BELOW
# capacity by contract (see the size-budget.txt header).
# shellcheck disable=SC2086 # iterate the whitespace-split list
for entry in $budgets; do
	region=${entry%%=*}
	bytes=${entry#*=}
	cap=$(getval "$hw" "$region")
	if [ -n "$cap" ] && [ "$bytes" -gt "$cap" ]; then
		echo "ERR: budget '$region=$bytes' is above the hardware capacity $cap — budgets must stay below capacity (the linker is the fit check)" >&2
		exit 1
	fi
done

# Classify every dot-section (mapping documented in the header comment).
# The only accepted non-section lines are the "<elf> :" banner, the
# "section size addr" column header and the "Total" summary line — a
# warning or any other unexpected line aborts: a partially-parsed table
# must never gate a build. The heredoc-fed loop runs in this shell, so the
# accumulators survive it.
flash=0 dtcmram=0 ram_d1=0 ram_d2=0 ram_d2b=0 ram_d3=0 itcmram=0
seen=''
while read -r sec sz _; do
	case $sec:$sz in
	*:) continue ;;
	section:size | Section:Size) continue ;;
	esac
	case $sec in
	Total | total) continue ;;
	.*) ;;
	*)
		echo "ERR: unrecognized size output line: '$sec $sz'" >&2
		exit 1
		;;
	esac
	case $sz in
	'' | *[!0-9]*)
		echo "ERR: unrecognized size line: '$sec' size '$sz'" >&2
		exit 1
		;;
	esac
	case $sec in
	.isr_vector | .text | .rodata | .ARM.extab | .ARM | .preinit_array | .init_array | .fini_array)
		flash=$((flash + sz))
		seen="$seen flash"
		;;
	.data)
		flash=$((flash + sz))
		dtcmram=$((dtcmram + sz))
		seen="$seen flash dtcmram"
		;;
	.bss | ._user_heap_stack)
		dtcmram=$((dtcmram + sz))
		seen="$seen dtcmram"
		;;
	# 8.8 SW5: .noinit (NOLOAD, DTCMRAM) — diagnostic fault-capture struct.
	# Present unconditionally (empty when nothing populates it); counted as
	# DTCMRAM like .bss — it occupies RAM at runtime even though it carries
	# no load image.
	.noinit)
		dtcmram=$((dtcmram + sz))
		seen="$seen dtcmram"
		;;
	.ram_d1)
		ram_d1=$((ram_d1 + sz))
		seen="$seen ram_d1"
		;;
	.ram_d2)
		ram_d2=$((ram_d2 + sz))
		seen="$seen ram_d2"
		;;
	.ram_d2b)
		ram_d2b=$((ram_d2b + sz))
		seen="$seen ram_d2b"
		;;
	.ram_d3)
		ram_d3=$((ram_d3 + sz))
		seen="$seen ram_d3"
		;;
	.instruction_ram)
		itcmram=$((itcmram + sz))
		seen="$seen itcmram"
		;;
	.ARM.attributes | .comment | .debug_*)
		;;
	*)
		echo "ERR: section '$sec' is not classified into any region" >&2
		echo "     update the mapping in this script to mirror firmware/preenfm3_2MB.ld" >&2
		exit 1
		;;
	esac
done <<EOF
$size_out
EOF

# Every canonical region must have been classified. Empty or truncated
# size output that silently omits regions would otherwise gate nothing
# while CI stays green. (If the linker script legitimately stops emitting a
# region, this error is the deliberate prompt to update the mapping, the
# budgets and this check together.)
regions='flash dtcmram ram_d1 ram_d2 ram_d2b ram_d3 itcmram'
unseen=''
# shellcheck disable=SC2086 # iterate the canonical region list
for region in $regions; do
	case " $seen " in
	*" $region "*) ;;
	*) unseen="$unseen $region" ;;
	esac
done
if [ -n "$unseen" ]; then
	echo "ERR: no sections classified for region(s):$unseen — empty or truncated size output?" >&2
	exit 1
fi

used="flash=$flash dtcmram=$dtcmram ram_d1=$ram_d1 ram_d2=$ram_d2 ram_d2b=$ram_d2b ram_d3=$ram_d3 itcmram=$itcmram"
active=$regions

# Every measured region must have a budget line (extra unused lines are OK).
missing=''
# shellcheck disable=SC2086 # iterate the measured region list
for region in $active; do
	case " $budgets " in
	*" $region="*) ;;
	*) missing="$missing $region" ;;
	esac
done
if [ -n "$missing" ]; then
	echo "ERR: region(s)$missing measured in the ELF but missing a budget line in '$budget_file'" >&2
	echo "     the budget must cover every region; extra unused lines are allowed" >&2
	exit 1
fi

# Markdown utilization table on stdout (the workflow tees it into the step
# summary). Rows are `region used budget hardware_capacity`; awk does the
# %-formatting and thousands separators. LC_ALL=C pins the number formatting
# (mirrors coverage-gate.sh), and an awk failure aborts — a corrupt table
# must never pass silently.
rows=''
# shellcheck disable=SC2086 # iterate the measured region list
for region in $active; do
	rows="$rows
$region $(getval "$used" "$region") $(getval "$budgets" "$region") $(getval "$hw" "$region")"
done
printf '%s\n' "$rows" | LC_ALL=C awk '
	function commas(n,   s, t) {
		s = sprintf("%d", n)
		t = ""
		while (length(s) > 3) {
			t = "," substr(s, length(s) - 2) t
			s = substr(s, 1, length(s) - 3)
		}
		return s t
	}
	BEGIN {
		print "## Firmware size (Flash/RAM region budgets)"
		print ""
		print "| Region | Used (B) | Budget (B) | % of budget | % of hardware capacity |"
		print "| --- | --- | --- | --- | --- |"
	}
	NF == 4 {
		printf "| %s | %s | %s | %.1f%% | %.1f%% |\n", \
			$1, commas($2), commas($3), 100 * $2 / $3, 100 * $2 / $4
	}
' || {
	echo "ERR: utilization table rendering failed" >&2
	exit 1
}

# Gate: FAIL on any over-budget region (the table above still prints — the
# utilization data is valid even when red).
over=''
# shellcheck disable=SC2086 # iterate the measured region list
for region in $active; do
	if [ "$(getval "$used" "$region")" -gt "$(getval "$budgets" "$region")" ]; then
		over="$over $region"
	fi
done

if [ -n "$over" ]; then
	{
		echo ""
		echo "FAIL: region(s) over budget:"
		# shellcheck disable=SC2086 # iterate the over-budget region list
		for region in $over; do
			used_b=$(getval "$used" "$region")
			budget_b=$(getval "$budgets" "$region")
			echo "  $region: $used_b B used > $budget_b B budget (over by $((used_b - budget_b)) B)"
		done
		echo ""
		echo "This is a size regression. Either shrink the region's usage,"
		echo "or — if the growth is accepted — bump its budget in:"
		echo "  $budget_file"
		echo "and commit the budget change in this same PR."
	} >&2
	exit 1
fi

echo "size gate: PASS"
