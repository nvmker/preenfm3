#!/usr/bin/env bash
# Stage the preenfm3 release folder — mirrors the layout of the reference
# pfm3_firmware_<ver>.zip, plus new .sh flasher scripts alongside the .cmd ones.
#
# Usage:
#   stage-release.sh <staging-dir> <fwver> <blver> <fw_bin> <bl_bin>
#     fwver/blver : version in FILE form, e.g. 1_03 / 1_09 (from parse-versions.sh)
#     fw_bin/bl_bin: built flash .bin paths to copy + rename
#
# Creates <staging-dir>/pfm3_firmware_<fwver>/ containing:
#   p3_<fwver>.bin              (firmware, copied from <fw_bin>)
#   p3_boot_<blver>.bin         (bootloader, copied from <bl_bin>)
#   flash_firmware.cmd          dfu-util one-liner (matches reference .cmd)
#   flash_bootloader.cmd        dfu-util one-liner (matches reference .cmd)
#   flash_firmware.sh           bash port of flash_firmware.cmd
#   flash_bootloader.sh         bash port of flash_bootloader.cmd
#
# Does NOT zip — the caller zips the staging directory so the top-level folder
# name (pfm3_firmware_<fwver>/) is preserved inside the archive.
set -euo pipefail

if [ "$#" -ne 5 ]; then
	echo "usage: $0 <staging-dir> <fwver> <blver> <fw_bin> <bl_bin>" >&2
	exit 2
fi

staging="$1"
fwver="$2"
blver="$3"
fw_bin="$4"
bl_bin="$5"
folder="$staging/pfm3_firmware_$fwver"

# Fail early on a missing build artifact rather than shipping a partial folder.
for f in "$fw_bin" "$bl_bin"; do
	if [ ! -f "$f" ]; then
		echo "error: missing build artifact: $f" >&2
		exit 1
	fi
done

mkdir -p "$folder"
cp "$fw_bin" "$folder/p3_$fwver.bin"
cp "$bl_bin" "$folder/p3_boot_$blver.bin"

# .cmd flashers — exact one-liners matching the reference release. The concrete
# version is baked into the bin filename (e.g. p3_1_03.bin), same as upstream.
printf 'dfu-util -a0 -d 0x0483:0xdf11 -D p3_%s.bin -s 0x8020000\n' "$fwver" \
	>"$folder/flash_firmware.cmd"
printf 'dfu-util -a0 -d 0x0483:0xdf11 -D p3_boot_%s.bin -s 0x8000000\n' "$blver" \
	>"$folder/flash_bootloader.cmd"

# write_flash_sh <path> <bin> <addr> <blurb>  -> portable bash port of a .cmd
write_flash_sh() {
	cat >"$1" <<EOF
#!/usr/bin/env bash
# Flash preenfm3 via USB DFU. $4
# Requires dfu-util (macOS: brew install dfu-util; Debian/Ubuntu: apt install dfu-util).
set -euo pipefail
command -v dfu-util >/dev/null 2>&1 || {
  echo "dfu-util not found. Install:  macOS: brew install dfu-util | Debian/Ubuntu: sudo apt install dfu-util" >&2
  exit 1
}
dfu-util -a0 -d 0x0483:0xdf11 -D "$2" -s "$3"
EOF
	chmod +x "$1"
}

# Firmware: 0x8020000 (after the 128 KiB bootloader region). Flash AFTER bootloader.
write_flash_sh "$folder/flash_firmware.sh" \
	"p3_$fwver.bin" "0x8020000" \
	"Enter DFU mode (set BOOT0, power on); flash firmware AFTER the bootloader."
# Bootloader: 0x8000000 (bank-1 start). Flash BEFORE firmware.
write_flash_sh "$folder/flash_bootloader.sh" \
	"p3_boot_$blver.bin" "0x8000000" \
	"Enter DFU mode (set BOOT0, power on); flash the bootloader BEFORE firmware."

echo "staged: $folder"
