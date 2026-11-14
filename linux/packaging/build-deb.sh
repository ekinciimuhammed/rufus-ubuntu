#!/bin/sh
# Rufus for Linux - build the Debian/Ubuntu package
# SPDX-License-Identifier: GPL-3.0-or-later
# usage: build-deb.sh [SYSROOT]   (SYSROOT: see the Makefile)
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
LNX="$(cd "$HERE/.." && pwd)"
TOP="$(cd "$LNX/.." && pwd)"
SYSROOT="${1:-${SYSROOT:-}}"
VERSION="$(sh "$LNX/tools/version.sh" "$TOP/src/rufus.rc")"
DEBREV="${DEBREV:-1}"
ARCH="$(dpkg --print-architecture)"
STAGE="$LNX/build/deb/rufus_${VERSION}-${DEBREV}_${ARCH}"
OUT="$LNX/build/rufus_${VERSION}-${DEBREV}_${ARCH}.deb"

echo "Building Rufus $VERSION for $ARCH"
make -C "$LNX" -j"$(nproc)" ${SYSROOT:+SYSROOT="$SYSROOT"} PREFIX=/usr
rm -rf -- "$STAGE"
sh "$HERE/install.sh" "$STAGE" /usr "$LNX/build/rufus"
strip --strip-unneeded --remove-section=.comment --remove-section=.note "$STAGE/usr/lib/rufus/rufus"

INSTALLED_SIZE=$(du -sk "$STAGE" | cut -f1)
install -d "$STAGE/DEBIAN"
cat > "$STAGE/DEBIAN/control" <<CONTROL
Package: rufus
Version: ${VERSION}-${DEBREV}
Architecture: ${ARCH}
Maintainer: Rufus Linux port <noreply@rufus.ie>
Installed-Size: ${INSTALLED_SIZE}
Depends: libc6 (>= 2.38), libgtk-3-0t64 | libgtk-3-0, libglib2.0-0t64 | libglib2.0-0, libcurl4t64 | libcurl4, libssl3t64 | libssl3, libwim15t64 | libwim15, pkexec | policykit-1, util-linux, udev, dosfstools, exfatprogs, ntfs-3g, e2fsprogs
Recommends: udftools, qemu-utils
Suggests: powershell
Section: admin
Priority: optional
Homepage: https://rufus.ie
Description: Reliable USB formatting utility - create bootable USB drives
 Rufus is a utility that helps format and create bootable USB flash drives,
 such as USB keys/pendrives, memory sticks, etc. It can be used to create
 USB installation media from bootable ISOs (Windows, Linux, UEFI, etc.),
 to work on a system that doesn't have an OS installed, to flash BIOS or
 other firmware from DOS, or to run a low-level utility.
 .
 This is the native Linux (GTK) version of Rufus, which shares its image
 processing, partitioning and boot code installation logic with the
 Windows version.
CONTROL
cat > "$STAGE/DEBIAN/postinst" <<'POSTINST'
#!/bin/sh
set -e
if [ "$1" = "configure" ]; then
	mkdir -p /var/cache/rufus
	chmod 0755 /var/cache/rufus
	command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database -q /usr/share/applications || true
	command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
fi
exit 0
POSTINST
cat > "$STAGE/DEBIAN/postrm" <<'POSTRM'
#!/bin/sh
set -e
if [ "$1" = "purge" ]; then
	rm -rf /var/cache/rufus
fi
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database -q /usr/share/applications || true
exit 0
POSTRM
chmod 0755 "$STAGE/DEBIAN/postinst" "$STAGE/DEBIAN/postrm"
( cd "$STAGE" && find . -type f ! -path './DEBIAN/*' -printf '%P\0' | sort -z | xargs -0 md5sum ) > "$STAGE/DEBIAN/md5sums"
find "$STAGE" -type d -exec chmod 0755 {} +
dpkg-deb --root-owner-group -Zxz --build "$STAGE" "$OUT"
echo "Created $OUT"
