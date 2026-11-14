#!/bin/sh
# Rufus for Linux - installation script (used by "make install" and build-deb.sh)
# SPDX-License-Identifier: GPL-3.0-or-later
# usage: install.sh <DESTDIR> <PREFIX> <path/to/rufus/binary>
set -e

DESTDIR="$1"
PREFIX="${2:-/usr}"
BINARY="$3"
HERE="$(cd "$(dirname "$0")" && pwd)"
TOP="$(cd "$HERE/../.." && pwd)"
RES="$TOP/res"
BUILD="$(dirname "$BINARY")"
DATADIR="$DESTDIR$PREFIX/share/rufus"

[ -x "$BINARY" ] || { echo "install.sh: '$BINARY' not found - run make first" >&2; exit 1; }

install -d "$DESTDIR$PREFIX/lib/rufus" "$DESTDIR$PREFIX/bin" "$DATADIR"
install -m 0755 "$BINARY" "$DESTDIR$PREFIX/lib/rufus/rufus"
install -m 0755 "$HERE/rufus.sh" "$DESTDIR$PREFIX/bin/rufus"

# Data files: everything that the Windows version embeds as a resource
for f in $(sed -n 's/^\t{ ID[RI]_[A-Z0-9_]*, "\(.*\)" },$/\1/p' "$BUILD/resources.h" | sort -u); do
	case "$f" in
		loc/*) continue ;;	# processed below
		hogger/*) continue ;;	# Windows console helper
	esac
	install -D -m 0644 "$RES/$f" "$DATADIR/$f"
done
# The translations, stripped the same way as for the Windows build
install -d "$DATADIR/loc"
sed -f "$RES/loc/embedded.sed" "$RES/loc/rufus.loc" > "$DATADIR/loc/rufus.loc"
chmod 0644 "$DATADIR/loc/rufus.loc"
# Toolbar icons and application icons
install -d "$DATADIR/icons"
install -m 0644 "$RES"/icons/*-16.png "$RES"/icons/*-24.png "$RES"/icons/*-32.png "$RES/icons/rufus-128.png" "$DATADIR/icons/"
for s in 16 24 32 48 64 128 256 512; do
	install -D -m 0644 "$RES/icons/rufus-$s.png" "$DESTDIR$PREFIX/share/icons/hicolor/${s}x${s}/apps/rufus.png"
done

# Desktop integration
install -D -m 0644 "$HERE/rufus.desktop" "$DESTDIR$PREFIX/share/applications/rufus.desktop"
install -D -m 0644 "$HERE/ie.akeo.rufus.policy" "$DESTDIR$PREFIX/share/polkit-1/actions/ie.akeo.rufus.policy"
install -D -m 0644 "$HERE/ie.akeo.rufus.metainfo.xml" "$DESTDIR$PREFIX/share/metainfo/ie.akeo.rufus.metainfo.xml"
install -d "$DESTDIR$PREFIX/share/man/man1"
gzip -9n -c "$HERE/rufus.1" > "$DESTDIR$PREFIX/share/man/man1/rufus.1.gz"

# Documentation
install -D -m 0644 "$HERE/copyright" "$DESTDIR$PREFIX/share/doc/rufus/copyright"
install -m 0644 "$TOP/README.md" "$DESTDIR$PREFIX/share/doc/rufus/README.md"
install -m 0644 "$TOP/linux/README.md" "$DESTDIR$PREFIX/share/doc/rufus/README.Linux.md"
gzip -9n -c "$TOP/ChangeLog.txt" > "$DESTDIR$PREFIX/share/doc/rufus/changelog.gz"
