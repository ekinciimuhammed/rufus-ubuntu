#!/bin/sh
# Rufus launcher
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Like on Windows, where Rufus requests elevation when started, Rufus needs
# administrative privileges to access disks, so it is started through pkexec.
BIN=/usr/lib/rufus/rufus

if [ "$(id -u)" -eq 0 ]; then
	exec "$BIN" "$@"
fi

# pkexec runs the program from root's home directory: make image paths absolute
abs() {
	case "$1" in
		/*) printf '%s' "$1" ;;
		*) printf '%s/%s' "$(pwd)" "$1" ;;
	esac
}
n=$#
i=0
while [ $i -lt $n ]; do
	arg="$1"
	shift
	i=$((i + 1))
	case "$arg" in
		-i|--iso)
			if [ $i -lt $n ]; then
				set -- "$@" "$arg" "$(abs "$1")"
				shift
				i=$((i + 1))
				continue
			fi
			;;
		--iso=*)
			arg="--iso=$(abs "${arg#--iso=}")"
			;;
		-*) ;;
		*)
			# A file given without -i (e.g. "Open with Rufus") is the image to use
			if [ -f "$arg" ]; then
				set -- "$@" "-i" "$(abs "$arg")"
				continue
			fi
			;;
	esac
	set -- "$@" "$arg"
done

if ! command -v pkexec >/dev/null 2>&1; then
	echo "rufus: pkexec is required to run Rufus with administrative privileges" >&2
	exit 1
fi
exec pkexec "$BIN" "$@"
