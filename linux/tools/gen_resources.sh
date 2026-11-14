#!/bin/sh
# Rufus for Linux: generate the resource ID -> data file table from rufus.rc
# SPDX-License-Identifier: GPL-3.0-or-later
# On Windows, these files are embedded in the executable. On Linux they are
# installed under RUFUS_DATADIR, with the same relative paths as in res/.
set -e
echo "/* Generated from $(basename "$1") by gen_resources.sh - DO NOT EDIT */"
echo "#pragma once"
echo "#include \"resource.h\""
echo "static const struct { int id; const char* path; } rufus_resources[] = {"
tr -d '\r' < "$1" | sed -n 's/^\(ID[RI]_[A-Z0-9_]*\)[[:space:]]\{1,\}RCDATA[[:space:]]\{1,\}"\.\.\/res\/\([^"]*\)".*/\t{ \1, "\2" },/p' | sed 's|loc/embedded.loc|loc/rufus.loc|'
tr -d '\r' < "$1" | sed -n 's/^\(IDI_[A-Z0-9_]*\)[[:space:]]\{1,\}\(ICON\|PNG\)[[:space:]]\{1,\}"\.\.\/res\/\([^"]*\)".*/\t{ \1, "\3" },/p'
echo "};"
# NB: Linux ships the full rufus.loc (the Windows build embeds a stripped copy)
