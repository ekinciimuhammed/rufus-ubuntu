#!/bin/sh
# Print the Rufus version, as displayed in the title of the main dialog (e.g. "4.16.2443")
# SPDX-License-Identifier: GPL-3.0-or-later
# usage: version.sh <path/to/rufus.rc>
tr -d '\r' < "$1" | sed -n 's/^CAPTION "Rufus \([0-9][0-9.]*\)"$/\1/p' | head -1
