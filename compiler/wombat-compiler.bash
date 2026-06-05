#!/usr/bin/env bash

SCRIPT="$1"
DESTDIR="$2"

usage() {
    echo "usage: $0 <script> <destination-dir>"
}

[[ -z "$SCRIPT" ]] && { usage; exit 1; }
[[ -z "$DESTDIR" ]] && { usage; exit 1; }

set -euo pipefail

[[ -e "$SCRIPT" ]] || { echo "file \`$SCRIPT\` doesn't exist"; exit 1; }
[[ -d "$DESTDIR" ]] || { echo "\`$DESTDIR\` doesn't exist or is not a directory"; exit 1; }

SCRIPTBASE="$(basename "$SCRIPT")"
SCRIPTDIR="$(dirname "$(realpath "$SCRIPT")")"
DEST="$DESTDIR/$SCRIPTBASE"

[[ -f "$DEST" ]] && { echo "filename \`$DEST\` already exists"; exit 1; }
echo "operating in dir: $SCRIPTDIR"
echo "writing to: $DEST"
./wombat-compiler \
  -sdb "$DESTDIR/sdb.txt" \
  -o "$DEST" \
  -enums ./enumerations.h \
  -enum-annots ./enum-annotations.txt \
  "$SCRIPT"

