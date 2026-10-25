#!/bin/sh
# Build extract-xiso (https://github.com/XboxDev/extract-xiso) from source into ~/.local/bin,
# for distros that don't package it. Needs git, cmake and a C compiler (installed with --deps).
#
#   tools/get-extract-xiso.sh [--deps]
set -e
if [ "$1" = "--deps" ]; then
    . /etc/os-release 2>/dev/null || true
    case " $ID $ID_LIKE " in
        *" ubuntu "*|*" debian "*) sudo apt-get install -y git cmake gcc make ;;
        *" fedora "*|*" rhel "*) sudo dnf install -y git cmake gcc make ;;
        *" arch "*|*" manjaro "*|*" steamos "*) sudo pacman -S --needed --noconfirm git cmake gcc make ;;
    esac
fi
command -v extract-xiso >/dev/null && { echo "extract-xiso is already installed: $(command -v extract-xiso)"; exit 0; }
for t in git cmake cc; do command -v $t >/dev/null || { echo "$t not found (run: $0 --deps)" >&2; exit 1; }; done
T=$(mktemp -d)
git clone --depth 1 https://github.com/XboxDev/extract-xiso "$T/src"
cmake -S "$T/src" -B "$T/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$T/build" -j"$(nproc 2>/dev/null || echo 2)"
mkdir -p "$HOME/.local/bin"
cp "$T/build/extract-xiso" "$HOME/.local/bin/"
rm -rf "$T"
echo "Installed $HOME/.local/bin/extract-xiso"
case ":$PATH:" in *":$HOME/.local/bin:"*) ;; *) echo "(add ~/.local/bin to your PATH, or open a new terminal)";; esac
