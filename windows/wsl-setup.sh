#!/bin/bash
# Runs inside WSL, called by Install-Spyro.bat / install.ps1:
#   wsl-setup.sh <repo folder> <your ISO or game folder>
# Copies the port into ~/spyro-port, builds it, installs extract-xiso if needed, extracts your
# game into ~/games/spyro and writes ~/spyro-port/play.sh, which the desktop shortcut runs.
set -e
REPO="$1"; SRC="$2"
PORT="$HOME/spyro-port"; GAME="$HOME/games/spyro"

echo "== Copying the port to $PORT"
mkdir -p "$PORT"
# dist/ and the ffmpeg build are kept, so running the installer again is quick
( cd "$REPO" && tar cf - --exclude=.git --exclude=dist --exclude=loader/third_party/ffmpeg32 . ) | ( cd "$PORT" && tar xf - )
cd "$PORT"
# a git checkout on Windows may have turned the scripts' line ends into CRLF
find . -name '*.sh' -exec sed -i 's/\r$//' {} +
sed -i 's/\r$//' run.sh
chmod +x build.sh setup-game.sh run.sh tools/*.sh

echo "== Building (this asks for your Linux password once, to install packages)"
./build.sh --deps

PATH="$PATH:$HOME/.local/bin"
if ! command -v extract-xiso >/dev/null && ! command -v xdvdfs >/dev/null; then
    echo "== Building extract-xiso"
    tools/get-extract-xiso.sh --deps
fi

echo "== Setting up your game"
if [ -f "$SRC" ]; then
    ./setup-game.sh "$SRC" "$GAME"
else
    # a folder: copy it into Linux, because loading from the Windows drive is slow
    mkdir -p "$GAME"
    cp -r "$SRC"/. "$GAME"/
    ./setup-game.sh "$GAME"
fi

cat > "$PORT/play.sh" <<PLAY
#!/bin/sh
# Started by the desktop shortcut. WSL's graphics driver is 64-bit only, so this 32-bit game most
# likely draws in software: XBOX_SCALE=1 (480 lines) keeps it playable. Raise it to 2 if it's fast.
export XBOX_SCALE=\${XBOX_SCALE:-1}
exec "$PORT/dist/run.sh" "$GAME"
PLAY
chmod +x "$PORT/play.sh"
echo
echo "== Done. Start the game with the desktop shortcut, or in Ubuntu: ~/spyro-port/play.sh"
