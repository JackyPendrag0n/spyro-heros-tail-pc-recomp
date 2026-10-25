#!/bin/sh
# Bring your own game: turn your own disc image (or an already extracted copy) into a game folder
# the port can run, and check that everything it needs is there.
#
#   ./setup-game.sh spyro.iso [game-folder]      extract the ISO (default folder: ./game)
#   ./setup-game.sh /path/to/extracted/game      only check an extracted copy
#
# Extracting needs one of:  extract-xiso  (https://github.com/XboxDev/extract-xiso)
#                           xdvdfs        (https://github.com/antangelo/xdvdfs, "cargo install xdvdfs-cli")
# Both read the "XISO" images most dumping tools make; xdvdfs also reads full-disc (Redump) images.
set -e
SRC="$1"; DEST="${2:-game}"
[ -n "$SRC" ] || { sed -n '2,10p' "$0"; exit 1; }
[ -e "$SRC" ] || { echo "No such file or folder: $SRC" >&2; exit 1; }

if [ -f "$SRC" ]; then
    mkdir -p "$DEST"
    DEST=$(cd "$DEST" && pwd)
    if command -v xdvdfs >/dev/null; then
        echo "Extracting with xdvdfs into $DEST ..."
        xdvdfs unpack "$SRC" "$DEST"
    elif command -v extract-xiso >/dev/null; then
        echo "Extracting with extract-xiso into $DEST ..."
        extract-xiso -x "$SRC" -d "$DEST"
    else
        echo "Install extract-xiso or xdvdfs first (see the top of this script / README.md)." >&2; exit 1
    fi
    GAME="$DEST"
else
    GAME=$(cd "$SRC" && pwd)
fi

# what the port needs (found case-insensitively, anywhere in the folder)
missing=0
need() {
    f=$(find "$GAME" -maxdepth 6 -iname "$1" 2>/dev/null | head -1)
    if [ -n "$f" ]; then echo "  ok       $1"; else echo "  MISSING  $1"; missing=1; fi
}
echo "Checking $GAME:"
need default.xbe
need Filelist.bin
need Filelist.000
need Filelist.001
need Filelist.002
movies=$(find "$GAME" -maxdepth 6 -iname "*.xmv" 2>/dev/null | wc -l)
echo "  movies   $movies .xmv file(s) (optional - the game runs without them)"
if [ "$missing" = 1 ]; then
    echo "Some files are missing: this doesn't look like a complete copy of Spyro: A Hero's Tail (Xbox, NTSC-U)." >&2
    exit 1
fi
echo
echo "Ready. Run it with:"
echo "  dist/run.sh \"$GAME\"        (or the AppImage: ./spyro-heros-tail-x86_64.AppImage \"$GAME\")"
