#!/bin/sh
# Build the loader (xboxrun) and put everything needed to play in dist/.
#
#   ./build.sh              build (needs the packages below)
#   ./build.sh --deps       install the build packages first (Debian/Ubuntu, Fedora or Arch; uses sudo)
#   ./build.sh --appimage   also pack dist/spyro-heros-tail-x86_64.AppImage (downloads appimagetool once)
#   ./build.sh --clean      remove build output (keeps the downloaded ffmpeg build)
#
# The first build also compiles a small static 32-bit ffmpeg for the game's movies
# (tools/build_ffmpeg.sh, downloads the ffmpeg 6.1.1 source, a few minutes).
set -e
cd "$(dirname "$0")"
ROOT=$(pwd)
DEPS=0; APPIMAGE=0
for a in "$@"; do
    case "$a" in
        --deps) DEPS=1 ;;
        --appimage) APPIMAGE=1 ;;
        --clean) make -C loader clean; rm -rf dist; exit 0 ;;
        -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
        *) echo "unknown option: $a" >&2; exit 1 ;;
    esac
done

if [ "$DEPS" = 1 ]; then
    . /etc/os-release 2>/dev/null || true
    case " $ID $ID_LIKE " in
        *" ubuntu "*|*" debian "*)
            sudo dpkg --add-architecture i386
            sudo apt-get update
            sudo apt-get install -y gcc-multilib make python3 curl xz-utils file \
                libsdl2-2.0-0:i386 libgl1:i386 libgl1-mesa-dri:i386 ;;
        *" fedora "*|*" rhel "*)
            if [ "${VERSION_ID:-0}" -ge 42 ] 2>/dev/null; then sdl=sdl2-compat.i686; else sdl=SDL2.i686; fi
            sudo dnf install -y gcc make python3 curl xz file glibc-devel.i686 libgcc.i686 \
                $sdl mesa-libGL.i686 mesa-dri-drivers.i686 ;;
        *" arch "*|*" manjaro "*|*" steamos "*)
            echo "(needs [multilib] enabled in /etc/pacman.conf)"
            sudo pacman -S --needed --noconfirm gcc make python curl xz file lib32-glibc lib32-gcc-libs \
                lib32-sdl2-compat lib32-mesa ;;
        *) echo "Unknown distro: install a 32-bit (multilib) gcc, make, python3, curl and 32-bit SDL2 + OpenGL yourself." >&2 ;;
    esac
fi

# sanity checks with useful messages
command -v gcc >/dev/null || { echo "gcc not found (run ./build.sh --deps)" >&2; exit 1; }
echo 'int main(void){return 0;}' > /tmp/m32test.$$.c
if ! gcc -m32 /tmp/m32test.$$.c -o /tmp/m32test.$$ 2>/dev/null; then
    rm -f /tmp/m32test.$$.c; echo "gcc can't build 32-bit programs: install gcc-multilib (./build.sh --deps)" >&2; exit 1
fi
rm -f /tmp/m32test.$$.c /tmp/m32test.$$
chmod +x tools/*.sh run.sh

make -C loader -j"$(nproc 2>/dev/null || echo 2)"

mkdir -p dist
cp loader/xboxrun loader/default.xbsym run.sh dist/
chmod +x dist/xboxrun dist/run.sh
echo
echo "Built: dist/xboxrun (+ dist/run.sh, dist/default.xbsym)"

if [ "$APPIMAGE" = 1 ]; then
    TOOL="$ROOT/tools/appimagetool-x86_64.AppImage"
    if [ ! -x "$TOOL" ]; then
        curl -fL -o "$TOOL" https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
        chmod +x "$TOOL"
    fi
    sh tools/mkappimage.sh dist/spyro-heros-tail-x86_64.AppImage dist/run.sh dist/xboxrun dist/default.xbsym "$TOOL"
fi
echo
echo "Next: ./setup-game.sh <your ISO or extracted game folder>   (see README.md)"
