#!/bin/sh
# tools/mkappimage.sh OUT.AppImage RUN.SH XBOXRUN DEFAULT.XBSYM [APPIMAGETOOL]
# Packs the loader into one AppImage: run.sh as AppRun, xboxrun + default.xbsym + the 32-bit SDL2
# and the libraries it links, from this machine's /usr/lib/i386-linux-gnu. Not bundled: glibc
# (every system has it), and OpenGL/libdrm, which must match the user's graphics driver - the
# user still needs their distro's 32-bit OpenGL (Mesa / NVIDIA) package, as run.sh says.
# APPIMAGETOOL: path to appimagetool-x86_64.AppImage (default: ./appimagetool).
set -e
OUT="$1"; RUN="$2"; XR="$3"; SYM="$4"; TOOL="${5:-./appimagetool}"
[ -n "$SYM" ] || { echo "usage: $0 OUT.AppImage run.sh xboxrun default.xbsym [appimagetool]" >&2; exit 1; }
D="$(mktemp -d)/AppDir"
mkdir -p "$D/usr/lib32"
cp "$RUN" "$D/AppRun"; chmod 755 "$D/AppRun"
cp "$XR" "$SYM" "$D/usr/lib32/"; chmod 755 "$D/usr/lib32/xboxrun"
skip='^(linux-gate|ld-linux|libc|libm|libdl|libpthread|librt|libGL|libGLX|libGLdispatch|libEGL|libdrm)[.-]'
ldd "$XR" | awk '/=> \//{print $1, $3}' | while read -r name path; do
    echo "$name" | grep -Eq "$skip" && continue
    cp -L "$path" "$D/usr/lib32/$name"
done
cat > "$D/spyro-coop.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Spyro A Hero's Tail (Linux port)
Exec=AppRun
Icon=spyro-coop
Categories=Game;
Terminal=true
EOF
# a plain purple square (the AppImage format wants an icon)
python3 -c "
import zlib, struct
w = h = 64
raw = b''.join(b'\x00' + bytes([0x7a, 0x3c, 0xb8]) * w for _ in range(h))
def ch(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
open('$D/spyro-coop.png', 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + ch(b'IDAT', zlib.compress(raw)) + ch(b'IEND', b''))
"
ARCH=x86_64 "$TOOL" --appimage-extract-and-run --no-appstream "$D" "$OUT"
ls -la "$OUT"
