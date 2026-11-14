#!/bin/sh
# Rufus for Linux: create a build sysroot from the Ubuntu -dev packages, for
# building without installing them (no root needed): make SYSROOT=build/sysroot
# SPDX-License-Identifier: GPL-3.0-or-later
set -e
SR="${1:-$(cd "$(dirname "$0")/.." && pwd)/build/sysroot}"
mkdir -p "$SR/debs" "$SR/root"
cd "$SR/debs"
PKGS=$(apt-cache depends --recurse --no-recommends --no-suggests --no-conflicts --no-breaks --no-replaces --no-enhances \
	libgtk-3-dev libcurl4-openssl-dev libssl-dev libwim-dev 2>/dev/null | grep -E '^[a-z0-9]' | sort -u | \
	grep -E -- '-dev$|^libwim15t64$')
apt-get download $PKGS >/dev/null
for d in *.deb; do dpkg-deb -x "$d" "$SR/root"; done
# Point the development symlinks at the libraries installed on this system
cd "$SR/root/usr/lib/x86_64-linux-gnu"
for l in $(find . -maxdepth 1 -type l -name '*.so'); do
	t=$(readlink "$l")
	if [ ! -e "$t" ] && [ -e "/usr/lib/x86_64-linux-gnu/$(basename "$t")" ]; then
		ln -sf "/usr/lib/x86_64-linux-gnu/$(basename "$t")" "$l"
	fi
done
echo "Sysroot ready: $SR/root  (build with: make SYSROOT=$SR/root)"
