#!/usr/bin/env bash

SRCDIR="$1"
DESTDIR="$2"

usage() {
    echo "usage: $0 <source-dir> <destination-dir>"
}

[[ -z "$SRCDIR" ]] && { usage; exit 1; }
[[ -z "$DESTDIR" ]] && { usage; exit 1; }

set -euo pipefail

SCRIPTDIR="$(dirname "$(realpath "$0")")"
SRCDIR="$(realpath "$SRCDIR")"
DESTDIR="$(realpath "$DESTDIR")"

cd "$SCRIPTDIR"

[[ -d "$SRCDIR" ]] || { echo "\`$SRCDIR\` doesn't exist or is not a directory"; exit 1; }
[[ -d "$DESTDIR" ]] || { echo "\`$DESTDIR\` doesn't exist or is not a directory"; exit 1; }

echo "Compiling ..."
for ff in "$SRCDIR"/*.m ; do
    echo "=== $ff"
    bash wombat-compiler.bash "$ff" "$DESTDIR"
done

echo "done!"
echo "remember to copy a working objscr.txt to $DESTDIR for items to work!"
