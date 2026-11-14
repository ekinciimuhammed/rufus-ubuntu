#!/bin/bash
# Rufus for Linux - end to end tests
# Copyright © 2026 Rufus Linux port contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs the real application, in automation mode (see linux/ui/automation_linux.c),
# against loop devices backed by image files, and checks the result with standard
# tools (sfdisk, blkid, fsck, cmp...). Nothing but these loop devices is written to:
# Rufus itself refuses any automation target that isn't the loop device it was given.
#
# Usage: linux/tests/run-tests.sh [-l] [test...]   (default: all tests; -l lists them)
# Environment:
#   RUFUS=<binary>          binary to test (default: linux/build/rufus)
#   RUFUS_TEST_ISO=<iso>    also test writing this ISO image (in ISO and DD mode)
#   RUFUS_TEST_KEEP=1       keep the disk images of each test
#   RUFUS_TEST_OUT=<dir>    where to write the logs (default: linux/build/tests)
#   RUFUS_TEST_TIMEOUT=<s>  maximum duration of a single test (default: 1800)
#   RUFUS_TEST_TAG=<tag>    suffix for the log names (to test several ISOs)

set -u
export LC_ALL=C

HERE=$(cd "$(dirname "$0")" && pwd)
SELF="$HERE/$(basename "$0")"
TOP=$(cd "$HERE/../.." && pwd)
RUFUS=${RUFUS:-$TOP/linux/build/rufus}
OUT=${RUFUS_TEST_OUT:-$TOP/linux/build/tests}
TIMEOUT=${RUFUS_TEST_TIMEOUT:-1800}

TESTS="mbr_fat32 gpt_ntfs mbr_exfat gpt_ext3 large_fat32 freedos syslinux4 syslinux6 grub2 grub4dos reactos uefintfs badblocks udf dd_raw dd_xz dd_gz vhd_source vhdx_source save_vhd save_vhdx hash zero hotplug cancel"
[ -n "${RUFUS_TEST_ISO:-}" ] && TESTS="$TESTS iso iso_fat32 iso_esp persistence iso_hash iso_dd"

if [ "${1:-}" = "-l" ]; then
	echo $TESTS
	exit 0
fi

# Elevate (sudo from a terminal, polkit otherwise)
if [ "$(id -u)" -ne 0 ]; then
	ENV_VARS="DISPLAY XAUTHORITY RUFUS RUFUS_TEST_ISO RUFUS_TEST_KEEP RUFUS_TEST_OUT RUFUS_TEST_TIMEOUT RUFUS_TEST_TAG"
	if [ -t 0 ]; then
		exec sudo --preserve-env=$(echo $ENV_VARS | tr ' ' ',') "$SELF" "$@"
	fi
	args=()
	for v in $ENV_VARS; do
		[ -n "${!v:-}" ] && args+=("$v=${!v}")
	done
	exec pkexec env PKEXEC_CALLER_UID="$(id -u)" "${args[@]}" "$SELF" "$@"
fi

die() { echo "ERROR: $*" >&2; exit 1; }

[ -x "$RUFUS" ] || die "'$RUFUS' not found (run 'make -C linux' first)"
for t in losetup sfdisk blkid mount umount cmp xz gzip mkfs.fat fsck.fat; do
	command -v $t >/dev/null || die "'$t' is required"
done

# The user the logs should belong to
OWNER=${SUDO_UID:-${PKEXEC_UID:-${PKEXEC_CALLER_UID:-0}}}

# Rufus needs a display: use the invoking user's
if [ -z "${DISPLAY:-}" ]; then
	export DISPLAY=:0
fi
if [ -z "${XAUTHORITY:-}" ] && [ "$OWNER" != 0 ]; then
	for f in /run/user/$OWNER/.mutter-Xwaylandauth.* "$(getent passwd "$OWNER" | cut -d: -f6)/.Xauthority"; do
		[ -f "$f" ] && export XAUTHORITY="$f" && break
	done
fi

# Only one test run at a time (Rufus itself is single instance)
exec 9> /run/rufus-tests.lock
if ! flock -n 9; then
	echo "Another test run is in progress, waiting for it to complete..."
	flock 9
fi

WORK=$(mktemp -d /var/tmp/rufus-tests.XXXXXX) || die "mktemp failed"
mkdir -p "$OUT" || die "can't create '$OUT'"
LOOP=""
MNT="$WORK/mnt"
mkdir -p "$MNT" "$WORK/home"

cleanup() {
	mountpoint -q "$MNT" && umount "$MNT"
	mountpoint -q "$WORK/iso" && umount "$WORK/iso"
	[ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null
	LOOP=""
	if [ -z "${RUFUS_TEST_KEEP:-}" ]; then
		rm -rf -- "$WORK"
	else
		echo "Test images kept in $WORK"
	fi
	[ "$OWNER" != 0 ] && chown -R "$OWNER" "$OUT" 2>/dev/null
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# Create the disk image for a test and attach it to a loop device (with partition scanning)
# $1 = size, $2 = initial contents: "empty", "random", "mbr2" (2 old MBR partitions) or "gpt3"
new_disk() {
	local img="$WORK/disk.img"
	[ -n "$LOOP" ] && losetup -d "$LOOP"
	rm -f -- "$img"
	truncate -s "$1" "$img"
	case "$2" in
	random)
		dd if=/dev/urandom of="$img" bs=1M count=64 conv=notrunc status=none ;;
	mbr2)
		printf 'label: dos\n,32M,83\n,32M,7\n' | sfdisk -q "$img" >/dev/null ;;
	gpt3)
		printf 'label: gpt\n,16M,L\n,16M,L\n,16M,L\n' | sfdisk -q "$img" >/dev/null ;;
	esac
	LOOP=$(losetup -fP --show "$img") || die "losetup failed"
	udevadm settle 2>/dev/null
}

# Run Rufus on $LOOP. $1 = automation options, other arguments are passed to Rufus
run_rufus() {
	local opts="$1"
	shift
	rm -f -- "$WORK/result"
	rm -rf -- "$WORK/home"
	mkdir -p "$WORK/home/.config/rufus"
	# Settings for this run (e.g. what an Alt-key cheat mode would have saved)
	[ -n "${INI_SETTINGS:-}" ] && printf '%s\n' "$INI_SETTINGS" > "$WORK/home/.config/rufus/rufus.ini"
	env -u SUDO_UID -u SUDO_USER -u PKEXEC_UID HOME="$WORK/home" GDK_BACKEND=x11 NO_AT_BRIDGE=1 \
		RUFUS_AUTOMATION="device=$LOOP;result=$WORK/result;$opts" \
		timeout "$TIMEOUT" "$RUFUS" "$@" > "$WORK/stdout" 2>&1
	local rc=$?
	cp -f -- "$WORK/result" "$OUT/$CUR.log" 2>/dev/null || cp -f -- "$WORK/stdout" "$OUT/$CUR.log"
	if [ $rc -eq 124 ]; then
		echo "Rufus timed out" >> "$OUT/$CUR.verify"
		return 1
	fi
	if [ ! -f "$WORK/result" ] || [ "$(head -c 2 "$WORK/result")" != "OK" ]; then
		echo "Rufus reported: $(head -1 "$WORK/result" 2>/dev/null || echo "no result (exit code $rc)")" >> "$OUT/$CUR.verify"
		tail -20 "$WORK/stdout" >> "$OUT/$CUR.verify"
		return 1
	fi
	[ $rc -eq 0 ] || { echo "Rufus exit code $rc" >> "$OUT/$CUR.verify"; return 1; }
	blockdev --rereadpt "$LOOP" 2>/dev/null
	udevadm settle 2>/dev/null
	return 0
}

# Verification helpers (all output goes to the .verify file)
log() { echo "$*" >> "$OUT/$CUR.verify"; }
check() {
	local what="$1"
	shift
	if "$@" >> "$OUT/$CUR.verify" 2>&1; then
		log "PASS: $what"
		return 0
	fi
	log "FAIL: $what"
	return 1
}
expect() {
	# $1 = description, $2 = expected, $3 = actual
	if [ "$2" = "$3" ]; then
		log "PASS: $1 = '$3'"
		return 0
	fi
	log "FAIL: $1 = '$3' (expected '$2')"
	return 1
}
part() { echo "${LOOP}p$1"; }
blk() { blkid -p -o value -s "$2" "$(part "$1")" 2>/dev/null; }
pt_label() { sfdisk -d "$LOOP" 2>/dev/null | sed -n 's/^label: //p'; }
nparts() { sfdisk -d "$LOOP" 2>/dev/null | grep -c "^$LOOP"; }
ptype() { sfdisk --part-type "$LOOP" "$1" 2>/dev/null | tr -d ' ' | tr 'a-z' 'A-Z'; }
is_boot() { sfdisk -d "$LOOP" 2>/dev/null | grep "^$(part "$1") " | grep -q bootable; }
mount_ro() { mount -o ro "$(part "$1")" "$MNT"; }
has_file() { [ -n "$(find "$MNT" -maxdepth 1 -iname "$1" -print -quit)" ]; }
mbr_code() {
	# The MBR boot code must not be blank
	[ "$(dd if="$LOOP" bs=440 count=1 status=none | tr -d '\0' | wc -c)" -gt 0 ]
}
log_has() { grep -q -- "$1" "$OUT/$CUR.log"; }
# fsck.fat, accepting a boot sector label of "NO NAME" while the root directory has the
# real label: the ms-sys boot records (DOS, ReactOS...) do that on Windows too
fsck_fat() {
	local out rc
	out=$(fsck.fat -n "$1" 2>&1)
	rc=$?
	echo "$out"
	[ $rc -eq 0 ] && return 0
	echo "$out" | grep -q "no label in boot sector" || return 1
	! echo "$out" | grep -v -e "^fsck.fat " -e "no label in boot sector" -e "Auto-copying volume label" \
		-e "Leaving filesystem unchanged" -e "files, .* clusters" -e "^ *$" | grep -q .
}

# A small partitioned "disk image" with random contents, for the DD tests
make_dd_source() {
	local src="$WORK/source.img"
	if [ ! -f "$src" ]; then
		dd if=/dev/urandom of="$src" bs=1M count=48 status=none
		printf 'label: dos\nlabel-id: 0x52554655\n,24M,c,*\n,,83\n' | sfdisk -q "$src" >/dev/null
	fi
	echo "$src"
}

t_mbr_fat32() {
	new_disk 512M mbr2
	run_rufus "boot=nonbootable;pt=MBR;fs=FAT32;label=RUFUS_TEST;quick=1" || return 1
	local r=0
	expect "partition table" dos "$(pt_label)" || r=1
	expect "partitions" 1 "$(nparts)" || r=1
	expect "partition type" C "$(ptype 1)" || r=1
	expect "file system" vfat "$(blk 1 TYPE)" || r=1
	expect "FAT version" FAT32 "$(blk 1 VERSION)" || r=1
	expect "label" RUFUS_TEST "$(blk 1 LABEL)" || r=1
	check "fsck.fat" fsck_fat "$(part 1)" || r=1
	return $r
}

t_gpt_ntfs() {
	new_disk 512M mbr2
	run_rufus "boot=nonbootable;pt=GPT;fs=NTFS;label=Rufus NTFS;quick=1" || return 1
	local r=0
	expect "partition table" gpt "$(pt_label)" || r=1
	expect "partitions" 1 "$(nparts)" || r=1
	expect "partition type" EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 "$(ptype 1)" || r=1
	expect "file system" ntfs "$(blk 1 TYPE)" || r=1
	expect "label" "Rufus NTFS" "$(blk 1 LABEL)" || r=1
	if command -v ntfsfix >/dev/null; then
		check "ntfsfix" ntfsfix --no-action "$(part 1)" || r=1
	fi
	check "mount" mount_ro 1 || r=1
	umount "$MNT" 2>/dev/null
	return $r
}

t_mbr_exfat() {
	new_disk 512M gpt3
	run_rufus "boot=nonbootable;pt=MBR;fs=exFAT;label=RufusExFAT;quick=1" || return 1
	local r=0
	expect "partition table" dos "$(pt_label)" || r=1
	expect "partitions" 1 "$(nparts)" || r=1
	expect "partition type" 7 "$(ptype 1)" || r=1
	expect "file system" exfat "$(blk 1 TYPE)" || r=1
	# Like on Windows, FAT and exFAT labels are converted to uppercase (format.c's ToValidLabel)
	expect "label" RUFUSEXFAT "$(blk 1 LABEL)" || r=1
	if command -v fsck.exfat >/dev/null; then
		check "fsck.exfat" fsck.exfat -n "$(part 1)" || r=1
	fi
	return $r
}

# NB: like on Windows, ext2/ext3 are offered for non bootable drives in advanced format mode
t_gpt_ext3() {
	new_disk 512M random
	run_rufus "boot=nonbootable;pt=GPT;fs=ext3;label=rufus-ext3;quick=1" || return 1
	local r=0
	expect "partition table" gpt "$(pt_label)" || r=1
	expect "partitions" 1 "$(nparts)" || r=1
	# NB: like on Windows, Rufus declares the main partition as "Microsoft basic data" (drive.c)
	expect "partition type" EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 "$(ptype 1)" || r=1
	expect "file system" ext3 "$(blk 1 TYPE)" || r=1
	expect "label" rufus-ext3 "$(blk 1 LABEL)" || r=1
	check "e2fsck" e2fsck -fn "$(part 1)" || r=1
	return $r
}

# Drives larger than 32 GB are formatted with Rufus' own FAT32 formatter (format_fat32.c)
t_large_fat32() {
	new_disk 40G empty
	run_rufus "boot=nonbootable;pt=MBR;fs=FAT32;label=LARGEFAT;quick=1" || return 1
	local r=0
	expect "partition type" C "$(ptype 1)" || r=1
	expect "file system" vfat "$(blk 1 TYPE)" || r=1
	expect "FAT version" FAT32 "$(blk 1 VERSION)" || r=1
	expect "label" LARGEFAT "$(blk 1 LABEL)" || r=1
	check "Large FAT32 formatter used" log_has "Large FAT32" || r=1
	check "fsck.fat" fsck_fat "$(part 1)" || r=1
	return $r
}

t_freedos() {
	new_disk 512M mbr2
	run_rufus "boot=freedos;pt=MBR;target=BIOS;fs=FAT32;label=FREEDOS" || return 1
	local r=0
	expect "partition table" dos "$(pt_label)" || r=1
	check "active partition" is_boot 1 || r=1
	check "MBR boot code" mbr_code || r=1
	check "fsck.fat" fsck_fat "$(part 1)" || r=1
	if check "mount" mount_ro 1; then
		for f in KERNEL.SYS COMMAND.COM AUTOEXEC.BAT CONFIG.SYS; do
			check "$f present" has_file "$f" || r=1
		done
		check "LOCALE directory" test -d "$MNT/LOCALE" || r=1
		umount "$MNT"
	else
		r=1
	fi
	# The FAT32 boot record must be the FreeDOS one
	check "FreeDOS boot record" sh -c "dd if='$(part 1)' bs=512 count=4 status=none | grep -aq 'Loading FreeDOS'" || r=1
	return $r
}

t_syslinux6() {
	new_disk 512M empty
	run_rufus "boot=syslinux6;pt=MBR;target=BIOS;fs=FAT32;label=SYSLINUX" || return 1
	local r=0
	check "active partition" is_boot 1 || r=1
	check "MBR boot code" mbr_code || r=1
	check "fsck.fat" fsck_fat "$(part 1)" || r=1
	if check "mount" mount_ro 1; then
		check "ldlinux.sys present" has_file ldlinux.sys || r=1
		umount "$MNT"
	else
		r=1
	fi
	check "Syslinux boot record" sh -c "dd if='$(part 1)' bs=512 count=1 status=none | grep -aq SYSLINUX" || r=1
	return $r
}

t_syslinux4() {
	new_disk 512M empty
	run_rufus "boot=syslinux4;pt=MBR;target=BIOS;fs=FAT32;label=SYSLINUX4" || return 1
	local r=0
	check "active partition" is_boot 1 || r=1
	check "MBR boot code" mbr_code || r=1
	check "fsck.fat" fsck_fat "$(part 1)" || r=1
	if check "mount" mount_ro 1; then
		check "ldlinux.sys present" has_file ldlinux.sys || r=1
		umount "$MNT"
	else
		r=1
	fi
	check "Syslinux boot record" sh -c "dd if='$(part 1)' bs=512 count=1 status=none | grep -aq SYSLINUX" || r=1
	return $r
}

# GRUB 2: GRUB's MBR, followed by the embedded core.img
t_grub2() {
	new_disk 512M empty
	run_rufus "boot=grub2;pt=MBR;target=BIOS;fs=FAT32;label=GRUB2" || return 1
	local r=0
	local core="$TOP/res/grub2/core.img"
	check "GRUB 2 MBR" sh -c "dd if='$LOOP' bs=512 count=1 status=none | grep -aq 'GRUB '" || r=1
	check "core.img written after the MBR" cmp -n "$(stat -c %s "$core")" "$core" <(dd if="$LOOP" bs=512 skip=1 count=128 status=none) || r=1
	check "fsck.fat" fsck_fat "$(part 1)" || r=1
	return $r
}

# Grub4DOS: grldr.mbr in the first sectors, and grldr (downloaded and signature checked by Rufus)
t_grub4dos() {
	new_disk 512M empty
	run_rufus "boot=grub4dos;pt=MBR;target=BIOS;fs=FAT32;label=GRUB4DOS" || return 1
	local r=0
	local mbr="$TOP/res/grub/grldr.mbr"
	check "grldr.mbr written" cmp -i 512:512 -n 7680 "$mbr" "$LOOP" || r=1
	check "fsck.fat" fsck_fat "$(part 1)" || r=1
	if check "mount" mount_ro 1; then
		check "grldr present" has_file grldr || r=1
		umount "$MNT"
	else
		r=1
	fi
	return $r
}

t_reactos() {
	new_disk 512M empty
	run_rufus "boot=reactos;pt=MBR;target=BIOS;fs=FAT32;label=REACTOS" || return 1
	local r=0
	check "active partition" is_boot 1 || r=1
	check "ReactOS MBR" sh -c "dd if='$LOOP' bs=512 count=1 status=none | grep -aq 'no active partition found'" || r=1
	check "ReactOS boot record" sh -c "dd if='$(part 1)' bs=512 count=16 status=none | grep -aq 'freeldr.sys not found'" || r=1
	check "fsck.fat" fsck_fat "$(part 1)" || r=1
	return $r
}

# UEFI:NTFS: NTFS data partition + the UEFI:NTFS FAT partition
t_uefintfs() {
	new_disk 512M mbr2
	run_rufus "boot=uefintfs;fs=NTFS;label=UEFI NTFS" || return 1
	local r=0
	expect "partitions" 2 "$(nparts)" || r=1
	expect "file system" ntfs "$(blk 1 TYPE)" || r=1
	expect "UEFI:NTFS partition" vfat "$(blk 2 TYPE)" || r=1
	# drive.c recognizes both labels (RUFUS_BOOT is used by current versions of the image)
	check "UEFI:NTFS label" sh -c "blkid -p -o value -s LABEL '$(part 2)' | grep -qx -e UEFI_NTFS -e RUFUS_BOOT" || r=1
	check "UEFI:NTFS fsck.fat" fsck_fat "$(part 2)" || r=1
	if check "mount UEFI:NTFS" mount_ro 2; then
		check "UEFI:NTFS bootloader present" test -n "$(find "$MNT" -ipath '*efi/boot/boot*.efi' -print -quit)" || r=1
		umount "$MNT"
	else
		r=1
	fi
	if command -v ntfsfix >/dev/null; then
		check "ntfsfix" ntfsfix --no-action "$(part 1)" || r=1
	fi
	return $r
}

# Bad blocks check (1 pass) before formatting
t_badblocks() {
	new_disk 64M empty
	run_rufus "boot=nonbootable;pt=MBR;fs=FAT32;label=BADBLOCKS;badblocks=1" || return 1
	local r=0
	check "bad blocks check ran" log_has "Bad Blocks: Check completed" || r=1
	check "no bad blocks found" log_has "0 bad block" || r=1
	expect "file system" vfat "$(blk 1 TYPE)" || r=1
	check "fsck.fat" fsck_fat "$(part 1)" || r=1
	return $r
}

dd_test() {
	# $1 = image to write, $2 = uncompressed source
	new_disk 256M random
	run_rufus "boot=image;write=dd" -i "$1" || return 1
	local r=0
	local size=$(stat -c %s "$2")
	check "device contents match the image" cmp -n "$size" "$2" "$LOOP" || r=1
	expect "partition table" dos "$(pt_label)" || r=1
	expect "partitions" 2 "$(nparts)" || r=1
	return $r
}

t_dd_raw() {
	local src=$(make_dd_source)
	dd_test "$src" "$src"
}

t_dd_xz() {
	local src=$(make_dd_source)
	[ -f "$WORK/source.img.xz" ] || xz -T0 -k -3 -c "$src" > "$WORK/source.img.xz"
	dd_test "$WORK/source.img.xz" "$src"
}

t_dd_gz() {
	local src=$(make_dd_source)
	[ -f "$WORK/source.img.gz" ] || gzip -1 -c "$src" > "$WORK/source.img.gz"
	dd_test "$WORK/source.img.gz" "$src"
}

t_zero() {
	new_disk 128M random
	run_rufus "zero=1" || return 1
	check "device zeroed" cmp -n $((128 * 1024 * 1024)) "$LOOP" /dev/zero
}

# Save a drive to a (fixed) VHD image: the raw contents of the drive followed by a VHD footer
t_save_vhd() {
	new_disk 64M random
	rm -f -- "$WORK/saved.vhd"
	run_rufus "action=save;file=$WORK/saved.vhd" || return 1
	local r=0 size=$((64 * 1024 * 1024))
	expect "VHD size" $((size + 512)) "$(stat -c %s "$WORK/saved.vhd" 2>/dev/null)" || r=1
	check "VHD contents match the drive" cmp -n $size "$WORK/saved.vhd" "$LOOP" || r=1
	check "VHD footer" sh -c "tail -c 512 '$WORK/saved.vhd' | head -c 8 | grep -q conectix" || r=1
	rm -f -- "$WORK/saved.vhd"
	return $r
}

# The checksum button: MD5, SHA-1 and SHA-256 of the selected image
t_hash() {
	local src=$(make_dd_source)
	new_disk 64M empty
	run_rufus "action=hash" -i "$src" || return 1
	local r=0
	check "MD5" log_has "MD5:    $(md5sum "$src" | cut -d' ' -f1)" || r=1
	check "SHA1" log_has "SHA1:   $(sha1sum "$src" | cut -d' ' -f1)" || r=1
	check "SHA256" log_has "SHA256: $(sha256sum "$src" | cut -d' ' -f1)" || r=1
	return $r
}

# Fixed VHD images are written like raw disk images (without their footer)
t_vhd_source() {
	local src=$(make_dd_source)
	cp -f -- "$src" "$WORK/source.vhd"
	python3 -I - "$WORK/source.vhd" "$(stat -c %s "$src")" <<'PYEOF' || return 1
import struct, sys, time, uuid
path, size = sys.argv[1], int(sys.argv[2])
# Geometry, as per the VHD specifications
sectors = min(size // 512, 65535 * 16 * 255)
if sectors >= 65535 * 16 * 63:
	spt, heads = 255, 16
	cth = sectors // spt
else:
	spt = 17
	cth = sectors // spt
	heads = max((cth + 1023) // 1024, 4)
	if cth >= heads * 1024 or heads > 16:
		spt, heads, cth = 31, 16, sectors // 31
	if cth >= heads * 1024:
		spt, heads, cth = 63, 16, sectors // 63
cyl = cth // heads
f = bytearray(struct.pack(">8sIIQI4sI4sQQHBBII16sB", b"conectix", 2, 0x10000, 0xFFFFFFFFFFFFFFFF,
	int(time.time()) - 946684800, b"rufs", 0x10000, b"Wi2k", size, size, cyl, heads, spt, 2, 0,
	uuid.uuid4().bytes, 0))
f += bytes(512 - len(f))
struct.pack_into(">I", f, 64, ~sum(f) & 0xFFFFFFFF)
open(path, "ab").write(f)
PYEOF
	new_disk 256M random
	run_rufus "boot=image;write=dd" -i "$WORK/source.vhd" || return 1
	local r=0
	check "device contents match the VHD's disk" cmp -n "$(stat -c %s "$src")" "$src" "$LOOP" || r=1
	expect "partitions" 2 "$(nparts)" || r=1
	rm -f -- "$WORK/source.vhd"
	return $r
}

# UDF (requires udftools)
t_udf() {
	command -v mkudffs >/dev/null || { log "SKIP: mkudffs (udftools) is not installed"; return 2; }
	new_disk 512M mbr2
	run_rufus "boot=nonbootable;pt=MBR;fs=UDF;label=Rufus UDF" || return 1
	local r=0
	expect "file system" udf "$(blk 1 TYPE)" || r=1
	expect "label" "Rufus UDF" "$(blk 1 LABEL)" || r=1
	check "mount" mount_ro 1 && umount "$MNT" || r=1
	return $r
}

# Save a drive to a dynamic VHDX (requires qemu-utils)
t_save_vhdx() {
	command -v qemu-img >/dev/null || { log "SKIP: qemu-img (qemu-utils) is not installed"; return 2; }
	new_disk 64M random
	rm -f -- "$WORK/saved.vhdx"
	run_rufus "action=save;file=$WORK/saved.vhdx" || return 1
	local r=0
	expect "image format" vhdx "$(qemu-img info "$WORK/saved.vhdx" | sed -n 's/^file format: //p')" || r=1
	check "VHDX contents match the drive" qemu-img compare -f vhdx -F raw "$WORK/saved.vhdx" "$LOOP" || r=1
	rm -f -- "$WORK/saved.vhdx"
	return $r
}

# Dynamic VHDX images are written through qemu-nbd (requires qemu-utils)
t_vhdx_source() {
	command -v qemu-img >/dev/null || { log "SKIP: qemu-img (qemu-utils) is not installed"; return 2; }
	local src=$(make_dd_source)
	rm -f -- "$WORK/source.vhdx"
	qemu-img convert -f raw -O vhdx -o subformat=dynamic "$src" "$WORK/source.vhdx" || return 1
	new_disk 256M random
	run_rufus "boot=image;write=dd" -i "$WORK/source.vhdx" || return 1
	local r=0
	check "device contents match the VHDX's disk" cmp -n "$(stat -c %s "$src")" "$src" "$LOOP" || r=1
	check "nbd device released" sh -c "! grep -qs . /sys/block/nbd*/pid" || r=1
	rm -f -- "$WORK/source.vhdx"
	return $r
}

# Cancelling an operation must leave the drive in a usable state
t_cancel() {
	new_disk 1G empty
	run_rufus "boot=nonbootable;pt=MBR;fs=FAT32;label=CANCEL;badblocks=2;cancel=1000"
	mv -f -- "$OUT/$CUR.log" "$OUT/$CUR.cancelled.log"
	local r=0
	check "operation cancelled" sh -c "head -1 '$WORK/result' | grep -qi 'cancel'" || r=1
	check "cancellation logged" grep -q "Cancelling" "$OUT/$CUR.cancelled.log" || r=1
	check "drive not left mounted" sh -c "! grep -q '^$LOOP' /proc/mounts" || r=1
	check "drive not left locked" flock -n "$LOOP" true || r=1
	: > "$WORK/cancel.verify"
	# The drive must still be usable
	run_rufus "boot=nonbootable;pt=MBR;fs=FAT32;label=AFTERCANCEL;quick=1" || return 1
	expect "label after a new format" AFTERCANCEL "$(blk 1 LABEL)" || r=1
	check "fsck.fat" fsck_fat "$(part 1)" || r=1
	return $r
}

# Hotplug: the drive appears while Rufus is already running (like plugging a USB drive)
t_hotplug() {
	local img="$WORK/disk.img" pid rc
	[ -n "$LOOP" ] && losetup -d "$LOOP"
	LOOP=""
	rm -f -- "$img" "$WORK/result"
	truncate -s 256M "$img"
	local dev=$(losetup -f) || return 1
	rm -rf -- "$WORK/home"
	mkdir -p "$WORK/home/.config/rufus"
	env -u SUDO_UID -u SUDO_USER -u PKEXEC_UID HOME="$WORK/home" GDK_BACKEND=x11 NO_AT_BRIDGE=1 \
		RUFUS_AUTOMATION="device=$dev;result=$WORK/result;boot=nonbootable;pt=MBR;fs=FAT32;label=HOTPLUG;quick=1" \
		timeout "$TIMEOUT" "$RUFUS" > "$WORK/stdout" 2>&1 &
	pid=$!
	sleep 4
	LOOP=$(losetup -P --show "$dev" "$img") || { kill $pid; return 1; }
	log "attached $LOOP while Rufus was running"
	wait $pid
	rc=$?
	cp -f -- "$WORK/result" "$OUT/$CUR.log" 2>/dev/null || cp -f -- "$WORK/stdout" "$OUT/$CUR.log"
	if [ ! -f "$WORK/result" ] || [ "$(head -c 2 "$WORK/result")" != "OK" ] || [ $rc -ne 0 ]; then
		log "Rufus reported: $(head -1 "$WORK/result" 2>/dev/null || echo "no result (exit code $rc)")"
		return 1
	fi
	blockdev --rereadpt "$LOOP" 2>/dev/null
	udevadm settle 2>/dev/null
	local r=0
	expect "file system" vfat "$(blk 1 TYPE)" || r=1
	expect "label" HOTPLUG "$(blk 1 LABEL)" || r=1
	check "fsck.fat" fsck_fat "$(part 1)" || r=1
	return $r
}

# Every file of the ISO must be present on the target, with the same size
# (except for the WIM files that Rufus is expected to split or modify)
iso_files_match() {
	mkdir -p "$WORK/iso"
	mount -o ro,loop "$RUFUS_TEST_ISO" "$WORK/iso" || return 1
	(cd "$WORK/iso" && find . -type f -printf '%s %p\n' | tr 'A-Z' 'a-z' | sort -k2) > "$WORK/iso.list"
	(cd "$MNT" && find . -type f -printf '%s %p\n' | tr 'A-Z' 'a-z' | sort -k2) > "$WORK/usb.list"
	umount "$WORK/iso"
	# NB: with the Windows User Experience options, setup.exe and appraiserres.dll are
	# replaced, and the originals renamed to setup.dll and appraiserres.bak
	awk 'BEGIN { renamed["./setup.exe"] = "./setup.dll"; renamed["./sources/appraiserres.dll"] = "./sources/appraiserres.bak" }
		NR == FNR { usb[$2] = $1; next }
		{
			n++
			if (!($2 in usb)) {
				if ($2 ~ /\/install\.wim$/ && ("./sources/install.swm" in usb))
					next
				print "missing: " $2; bad++
			} else if (usb[$2] != $1 && $2 !~ /\/(boot|install)\.wim$/) {
				if (($2 in renamed) && usb[renamed[$2]] == $1) {
					print "replaced by Rufus: " $2 " (original kept as " renamed[$2] ")"
					next
				}
				# Syslinux/GRUB configuration (label patching) and Syslinux modules (version matching)
				if ($2 ~ /\.(cfg|conf|c32)$/) {
					print "modified by Rufus: " $2 " (" $1 " vs " usb[$2] ")"
					next
				}
				print "size mismatch: " $2 " (" $1 " vs " usb[$2] ")"; bad++
			}
		}
		END { print n " files checked"; exit (bad > 0) }' "$WORK/usb.list" "$WORK/iso.list"
}

# ISOHybrid images (and only them) can be written in DD mode
is_hybrid() {
	[ "$(dd if="$1" bs=1 skip=510 count=2 status=none | od -An -tx1 | tr -d ' ')" = "55aa" ]
}

t_iso() {
	local size=$(( $(stat -c %s "$RUFUS_TEST_ISO") / 1073741824 + 4 ))
	new_disk "${size}G" mbr2
	run_rufus "boot=image;write=iso;wue=1" -i "$RUFUS_TEST_ISO" || return 1
	local r=0
	log "partition table: $(pt_label), $(nparts) partition(s)"
	sfdisk -d "$LOOP" >> "$OUT/$CUR.verify" 2>&1
	local fs=$(blk 1 TYPE)
	log "file system: $fs, label: '$(blk 1 LABEL)'"
	case "$fs" in
	vfat) check "fsck.fat" fsck_fat "$(part 1)" || r=1 ;;
	ntfs) command -v ntfsfix >/dev/null && { check "ntfsfix" ntfsfix --no-action "$(part 1)" || r=1; } ;;
	esac
	if check "mount" mount_ro 1; then
		check "ISO files copied" iso_files_match || r=1
		if log_has "Uses: Syslinux"; then
			check "Syslinux installed (ldlinux.sys)" test -n "$(find "$MNT" -iname ldlinux.sys -print -quit)" || r=1
			check "MBR boot code" mbr_code || r=1
		fi
		if [ -f "$MNT/sources/install.wim" ] || [ -f "$MNT/sources/install.esd" ] || [ -f "$MNT/sources/install.swm" ]; then
			check "unattend.xml created" sh -c "ls '$MNT' '$MNT/sources' | grep -qi unattend" || r=1
		fi
		umount "$MNT"
	else
		r=1
	fi
	if [ "$fs" = ntfs ]; then
		# UEFI:NTFS partition
		expect "UEFI:NTFS partition" vfat "$(blk 2 TYPE)" || r=1
		check "UEFI:NTFS label" sh -c "blkid -p -o value -s LABEL '$(part 2)' | grep -qx -e UEFI_NTFS -e RUFUS_BOOT" || r=1
	fi
	return $r
}

# Windows images with a >4GB install.wim can also be written to FAT32: Rufus then splits it
t_iso_fat32() {
	local size=$(( $(stat -c %s "$RUFUS_TEST_ISO") / 1073741824 + 4 ))
	new_disk "${size}G" empty
	# FAT32 is only offered for Windows images with a >4GB install.wim in dual UEFI/BIOS mode (Alt-E)
	INI_SETTINGS="EnableWindowsDualUefiBiosMode = 1" run_rufus "boot=image;write=iso;fs=FAT32;wue=0" -i "$RUFUS_TEST_ISO" || return 1
	local r=0
	log "partition table: $(pt_label), $(nparts) partition(s)"
	expect "file system" vfat "$(blk 1 TYPE)" || r=1
	check "fsck.fat" fsck_fat "$(part 1)" || r=1
	if check "mount" mount_ro 1; then
		check "ISO files copied" iso_files_match || r=1
		if [ -f "$WORK/iso.list" ] && grep -q "install\.wim" "$WORK/iso.list"; then
			if [ "$(grep "install\.wim" "$WORK/iso.list" | cut -d' ' -f1)" -gt 4294967295 ]; then
				check "install.wim split into .swm files" sh -c "ls '$MNT/sources' | grep -qi 'install.*\.swm'" || r=1
			fi
		fi
		umount "$MNT"
	else
		r=1
	fi
	return $r
}

# Persistence (Ubuntu/casper or Debian/live-boot ISOs): an extra ext partition, and the
# 'persistent' kernel option added to the boot menus
t_persistence() {
	# Only for Ubuntu (casper) and Debian (live-boot) based images
	mkdir -p "$WORK/iso"
	mount -o ro,loop "$RUFUS_TEST_ISO" "$WORK/iso" || return 1
	local supported=0
	[ -d "$WORK/iso/casper" ] || [ -d "$WORK/iso/live" ] && supported=1
	umount "$WORK/iso"
	if [ $supported -eq 0 ]; then
		log "SKIP: not an Ubuntu or Debian live image"
		return 2
	fi
	local size=$(( $(stat -c %s "$RUFUS_TEST_ISO") / 1073741824 + 3 ))
	new_disk "${size}G" empty
	run_rufus "boot=image;write=iso;persistence=1024" -i "$RUFUS_TEST_ISO" || return 1
	local r=0 i n label="" pers=""
	sfdisk -d "$LOOP" >> "$OUT/$CUR.verify" 2>&1
	n=$(nparts)
	for i in $(seq 1 "$n"); do
		label=$(blk "$i" LABEL)
		if [ "$label" = "casper-rw" ] || [ "$label" = "persistence" ]; then
			pers=$i
			break
		fi
	done
	if [ -z "$pers" ]; then
		log "FAIL: no persistence partition found"
		return 1
	fi
	log "PASS: persistence partition $pers ('$label', $(blk "$pers" TYPE))"
	check "persistence partition size >= 1 GB" test "$(blockdev --getsize64 "$(part "$pers")")" -ge $((1000 * 1024 * 1024)) || r=1
	check "e2fsck" e2fsck -fn "$(part "$pers")" || r=1
	if [ "$label" = "persistence" ]; then
		mount -o ro "$(part "$pers")" "$MNT" && { check "persistence.conf" grep -q "union" "$MNT/persistence.conf" || r=1; umount "$MNT"; }
	fi
	check "'persistent' kernel option added" log_has "Added 'persistent' kernel option" || r=1
	if check "mount" mount_ro 1; then
		check "boot menu has the 'persistent' option" sh -c "grep -rqs persistent '$MNT/boot/grub/grub.cfg' '$MNT/isolinux' '$MNT/syslinux' '$MNT/boot/grub/loopback.cfg'" || r=1
		umount "$MNT"
	else
		r=1
	fi
	return $r
}

# ISO -> ESP: small EFI images can be written to an EFI System Partition (GPT + FAT)
t_iso_esp() {
	if [ "$(stat -c %s "$RUFUS_TEST_ISO")" -ge $((1024 * 1024 * 1024)) ] || ! is_hybrid "$RUFUS_TEST_ISO"; then
		log "SKIP: only for ISOHybrid images smaller than 1 GB"
		return 2
	fi
	new_disk 2G empty
	run_rufus "boot=image;pt=GPT;fs=FAT32;write=esp" -i "$RUFUS_TEST_ISO" || return 1
	local r=0
	sfdisk -d "$LOOP" >> "$OUT/$CUR.verify" 2>&1
	expect "partition type" C12A7328-F81F-11D2-BA4B-00A0C93EC93B "$(ptype 1)" || r=1
	expect "file system" vfat "$(blk 1 TYPE)" || r=1
	check "fsck.fat" fsck_fat "$(part 1)" || r=1
	if check "mount" mount_ro 1; then
		check "ISO files copied" iso_files_match || r=1
		umount "$MNT"
	else
		r=1
	fi
	return $r
}

# Checksums of the ISO (MD5, SHA-1 and SHA-256, computed in parallel by Rufus)
t_iso_hash() {
	new_disk 64M empty
	local start=$(date +%s)
	run_rufus "action=hash" -i "$RUFUS_TEST_ISO" || return 1
	log "Rufus computed the checksums in $(( $(date +%s) - start ))s"
	local r=0
	check "MD5" log_has "MD5:    $(md5sum "$RUFUS_TEST_ISO" | cut -d' ' -f1)" || r=1
	check "SHA1" log_has "SHA1:   $(sha1sum "$RUFUS_TEST_ISO" | cut -d' ' -f1)" || r=1
	check "SHA256" log_has "SHA256: $(sha256sum "$RUFUS_TEST_ISO" | cut -d' ' -f1)" || r=1
	return $r
}

t_iso_dd() {
	local size=$(( $(stat -c %s "$RUFUS_TEST_ISO") / 1073741824 + 2 ))
	is_hybrid "$RUFUS_TEST_ISO" && new_disk "${size}G" empty
	if ! is_hybrid "$RUFUS_TEST_ISO"; then
		log "SKIP: not an ISOHybrid image"
		return 2
	fi
	run_rufus "boot=image;write=dd" -i "$RUFUS_TEST_ISO" || return 1
	check "device contents match the ISO" cmp -n "$(stat -c %s "$RUFUS_TEST_ISO")" "$RUFUS_TEST_ISO" "$LOOP"
}

[ $# -gt 0 ] && TESTS="$*"
TAG=${RUFUS_TEST_TAG:+-$RUFUS_TEST_TAG}
pass=0
fail=0
skip=0
failed=""
echo "Rufus: $RUFUS"
echo "Logs:  $OUT"
for T in $TESTS; do
	type "t_$T" >/dev/null 2>&1 || die "unknown test '$T'"
	CUR="$T$TAG"
	printf '%-20s ' "$CUR"
	start=$(date +%s)
	rm -f -- "$OUT/$CUR.log"
	: > "$OUT/$CUR.verify"
	"t_$T"
	case $? in
	0)	res=PASS
		pass=$((pass + 1)) ;;
	2)	res=SKIP
		skip=$((skip + 1)) ;;
	*)	res=FAIL
		fail=$((fail + 1))
		failed="$failed $CUR" ;;
	esac
	mountpoint -q "$MNT" && umount "$MNT"
	printf '%s (%ss)\n' "$res" $(( $(date +%s) - start ))
	# summary.txt keeps the last result of every test
	[ -f "$OUT/summary.txt" ] && sed -i "/^$CUR /d" "$OUT/summary.txt"
	echo "$CUR $res $(date '+%F %T')" >> "$OUT/summary.txt"
	[ $res = FAIL ] && grep -h "^FAIL\|^Rufus" "$OUT/$CUR.verify" | sed 's/^/    /'
done
[ -n "$LOOP" ] && losetup -d "$LOOP" && LOOP=""
echo "$pass passed, $fail failed${failed:+ (}${failed# }${failed:+)}, $skip skipped"
[ $fail -eq 0 ]
