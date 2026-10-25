#!/bin/sh
# usage: ./run.sh [game folder]
#   The game folder holds default.xbe and the Spyro/ folder. If you leave it out, the folder
#   run.sh is in and then the current folder are tried. Paths with spaces are fine (quote them
#   or not), and pointing at a folder inside the game (e.g. .../Spyro) works too.
# Writes xboxrun.log next to this script (or next to the .AppImage).
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="$HERE"; LOGDIR="$HERE"
if [ -n "$APPIMAGE" ] && [ -n "$APPDIR" ]; then
    # inside the AppImage: the program and its 32-bit SDL2 are in the image, the game is looked for
    # next to the .AppImage file and in the folder it was started from, the log goes next to it
    BIN="$APPDIR/usr/lib32"
    export LD_LIBRARY_PATH="$BIN${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    HERE="$(dirname "$APPIMAGE")"; LOGDIR="$HERE"
    [ -n "$OWD" ] && cd "$OWD"
    [ -w "$LOGDIR" ] || LOGDIR="$HOME"
fi

# all arguments joined, so an unquoted path with spaces still works
ARG="$*"

has_xbe() { [ -f "$1/default.xbe" ] || [ -f "$1/DEFAULT.XBE" ] || [ -f "$1/Default.xbe" ]; }

# turn one candidate into a game folder: itself, a parent (if it points inside the game),
# or a folder below it that holds default.xbe
resolve() {
    c="$1"
    [ -n "$c" ] || return 1
    case "$c" in "~"*) c="$HOME${c#\~}";; esac
    if [ -f "$c" ]; then c="$(dirname "$c")"; fi
    [ -d "$c" ] || return 1
    c="$(cd "$c" && pwd)"
    d="$c"
    for _ in 1 2 3 4; do
        if has_xbe "$d"; then printf '%s\n' "$d"; return 0; fi
        [ "$d" = "/" ] && break
        d="$(dirname "$d")"
    done
    found="$(find "$c" -maxdepth 4 -iname default.xbe 2>/dev/null | head -1)"
    if [ -n "$found" ]; then dirname "$found"; return 0; fi
    return 1
}

# rebuild a path one folder at a time, accepting "A-Heroes-Tail" for "A Heroes Tail" and vice versa
fuzzy() {
    p="$1"
    case "$p" in "~"*) p="$HOME${p#\~}";; esac
    case "$p" in /*) out="";; *) out="$PWD";; esac
    oldifs="$IFS"; IFS=/
    set -f; set -- $p; set +f
    IFS="$oldifs"
    for part in "$@"; do
        [ -z "$part" ] && continue
        if [ -e "$out/$part" ]; then out="$out/$part"; continue; fi
        a="$(printf '%s' "$part" | tr '-' ' ')"; b="$(printf '%s' "$part" | tr ' ' '-')"
        if [ -e "$out/$a" ]; then out="$out/$a"
        elif [ -e "$out/$b" ]; then out="$out/$b"
        else
            m="$(ls -1 "$out" 2>/dev/null | grep -ixF -- "$part" | head -1)"      # different capitals
            [ -n "$m" ] || return 1
            out="$out/$m"
        fi
    done
    printf '%s\n' "${out:-/}"
}

GAME=""
if [ -n "$ARG" ]; then
    GAME="$(resolve "$ARG")" ||
    GAME="$(resolve "$(fuzzy "$ARG")")" ||
    GAME=""
    if [ -z "$GAME" ]; then
        if [ ! -e "$ARG" ]; then echo "The folder '$ARG' does not exist." >&2
        else echo "No default.xbe in or around '$ARG'." >&2; fi
        GAME="$(resolve "$HERE")" || GAME="$(resolve "$PWD")" || GAME=""
        [ -n "$GAME" ] && echo "Found the game next to run.sh / in the current folder instead." >&2
    fi
else
    GAME="$(resolve "$HERE")" || GAME="$(resolve "$PWD")" || GAME=""
    [ -z "$GAME" ] && echo "No default.xbe next to run.sh or in the current folder." >&2
fi
if [ -z "$GAME" ]; then
    echo "Run it as  ./run.sh \"/path/to/game folder\"  (the folder holding default.xbe and Spyro/)," >&2
    echo "or put run.sh, xboxrun and default.xbsym in the game folder and just run  ./run.sh" >&2
    exit 1
fi
echo "Game folder: $GAME"

# ---- 32-bit libraries ----
# SDL2 is linked directly: ask the dynamic loader itself (authoritative).
# OpenGL is loaded at runtime by SDL, so check the library cache / usual 32-bit dirs.
has32gl() {
    ldconfig -p 2>/dev/null | grep -F "libGL.so.1 " | grep -qv 'x86-64' && return 0
    for d in /usr/lib/i386-linux-gnu /usr/lib32 /usr/lib; do
        [ -e "$d/libGL.so.1" ] && file -L "$d/libGL.so.1" 2>/dev/null | grep -q '32-bit' && return 0
    done
    return 1
}
need=""
ldd "$BIN/xboxrun" 2>/dev/null | grep -q 'libSDL2-2.0.so.0 => not found' && need="$need sdl"
has32gl || need="$need gl"
if [ -n "$need" ]; then
    . /etc/os-release 2>/dev/null
    nvidia=""; [ -e /proc/driver/nvidia/version ] && nvidia=1
    echo "Missing 32-bit libraries:$need" >&2
    echo "Install them with:" >&2
    case " $ID $ID_LIKE " in
        *" ubuntu "*|*" debian "*)
            extra=""
            if [ -n "$nvidia" ]; then
                v="$(dpkg -l 2>/dev/null | awk '/^ii  libnvidia-gl-[0-9]+:amd64/{sub(":amd64","",$2); print $2; exit}')"
                [ -n "$v" ] && extra=" $v:i386"
            fi
            echo "  sudo dpkg --add-architecture i386 && sudo apt update" >&2
            echo "  sudo apt install libsdl2-2.0-0:i386 libgl1:i386 libgl1-mesa-dri:i386$extra" >&2 ;;
        *" fedora "*|*" rhel "*)
            if [ "${VERSION_ID:-0}" -ge 42 ] 2>/dev/null; then sdl=sdl2-compat.i686; else sdl=SDL2.i686; fi
            echo "  sudo dnf install $sdl mesa-libGL.i686 mesa-dri-drivers.i686" >&2
            [ -n "$nvidia" ] && echo "  (NVIDIA from RPM Fusion: sudo dnf install xorg-x11-drv-nvidia-libs.i686)" >&2 ;;
        *" arch "*|*" steamos "*|*" manjaro "*)
            echo "  (enable [multilib] in /etc/pacman.conf first)" >&2
            if [ -n "$nvidia" ]; then echo "  sudo pacman -S lib32-sdl2-compat lib32-nvidia-utils" >&2
            else echo "  sudo pacman -S lib32-sdl2-compat lib32-mesa" >&2; fi ;;
        *" opensuse"*|*" suse "*)
            echo "  sudo zypper install libSDL2-2_0-0-32bit Mesa-libGL1-32bit" >&2 ;;
        *)
            echo "  your distro's 32-bit (i386/i686/lib32) packages of SDL2 and OpenGL/Mesa" >&2 ;;
    esac
    exit 1
fi

"$BIN/xboxrun" "$GAME" 2>&1 | tee "$LOGDIR/xboxrun.log"
