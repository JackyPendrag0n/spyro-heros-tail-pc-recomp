#!/bin/sh
# Builds the small static 32-bit ffmpeg that xmv.c links against (XMV demuxer, WMV2 video,
# Xbox ADPCM / PCM / WMA audio, swscale, swresample) into loader/third_party/ffmpeg32.
# Usage: tools/build_ffmpeg.sh [ffmpeg source dir or .tar.xz]   (default: download 6.1.1)
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
out="$here/loader/third_party/ffmpeg32"
work=$(mktemp -d)
src="$1"
if [ -z "$src" ]; then
    src="$work/ffmpeg-6.1.1.tar.xz"
    curl -fL -o "$src" http://archive.ubuntu.com/ubuntu/pool/universe/f/ffmpeg/ffmpeg_6.1.1.orig.tar.xz \
        || curl -fL -o "$src" https://ffmpeg.org/releases/ffmpeg-6.1.1.tar.xz
fi
if [ -f "$src" ]; then tar xJf "$src" -C "$work"; src=$(ls -d "$work"/ffmpeg-*/ | head -1); fi
cd "$src"
./configure --prefix="$out" --arch=x86_32 --target-os=linux --cc="gcc -m32" \
    --extra-cflags="-m32 -fno-pie" --extra-ldflags="-m32" --enable-static --disable-shared \
    --disable-programs --disable-doc --disable-everything --disable-x86asm --disable-network \
    --disable-autodetect --disable-avdevice --disable-avfilter --disable-postproc \
    --enable-protocol=file --enable-demuxer=xmv \
    --enable-decoder=wmv2,wmav1,wmav2,adpcm_ima_wav,adpcm_ms,pcm_s16le,pcm_u8 \
    --enable-swscale --enable-swresample
make -j"$(nproc)"
make install
for f in "$out"/lib/*.a; do strip --strip-debug "$f"; done   # keeps xboxrun small
rm -rf "$work"
echo "ffmpeg installed in $out"
