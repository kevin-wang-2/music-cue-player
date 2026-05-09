#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# build_minimal_ffmpeg.sh
#
# Builds a stripped-down FFmpeg (audio decode only) with zero external codec
# library dependencies.  All decoders used (MP3, AAC, ALAC, FLAC, Opus,
# Vorbis, WavPack, APE, PCM) are FFmpeg-native — no libmp3lame, libfaad, etc.
#
# Output: build/ffmpeg_minimal/  (lib/, include/, lib/pkgconfig/)
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

FFMPEG_TAG="n7.1.1"          # keep in sync with Homebrew ffmpeg major
SRC_DIR="$PROJECT_ROOT/build/ffmpeg_minimal/src"
INSTALL_DIR="$PROJECT_ROOT/build/ffmpeg_minimal"
ARCH="$(uname -m)"           # arm64 on Apple Silicon, x86_64 on Intel
JOBS="$(sysctl -n hw.logicalcpu)"

echo "▶ build_minimal_ffmpeg: target=$ARCH  tag=$FFMPEG_TAG"
echo "  install → $INSTALL_DIR"

# ── Already built? ────────────────────────────────────────────────────────────
if [ -f "$INSTALL_DIR/lib/pkgconfig/libavcodec.pc" ]; then
    echo "  Already built — skipping (delete build/ffmpeg_minimal to rebuild)"
    exit 0
fi

mkdir -p "$SRC_DIR"

# ── Clone FFmpeg source ───────────────────────────────────────────────────────
if [ ! -d "$SRC_DIR/ffmpeg/.git" ]; then
    echo "  Cloning FFmpeg $FFMPEG_TAG (shallow)…"
    git clone --depth 1 --branch "$FFMPEG_TAG" \
        https://git.ffmpeg.org/ffmpeg.git "$SRC_DIR/ffmpeg"
else
    echo "  FFmpeg source already present"
fi

cd "$SRC_DIR/ffmpeg"

# ── Configure ─────────────────────────────────────────────────────────────────
# Enabled components:
#   avformat   — container demuxing (mp3, m4a/mov, ogg, flac, wav, aiff…)
#   avcodec    — audio decoding (mp3, aac, alac, flac, vorbis, opus, pcm…)
#   avutil     — required base library
#   swresample — required by avcodec internally
#   protocol=file — local file I/O only (no network)
#
# All decoders are FFmpeg-native — zero external codec lib deps.
echo "  Configuring…"
./configure \
    --prefix="$INSTALL_DIR" \
    --arch="$ARCH" \
    --cc=clang \
    --enable-shared \
    --disable-static \
    --disable-everything \
    --disable-network \
    --disable-programs \
    --disable-doc \
    --disable-debug \
    --enable-optimizations \
    \
    --enable-avformat \
    --enable-avcodec \
    --enable-avutil \
    --enable-swresample \
    \
    --enable-protocol=file \
    \
    --enable-demuxer=mp3,aac,mov,ogg,flac,wav,aiff,ape,wv,matroska,caf,dsf,asf \
    \
    --enable-decoder=mp3,mp3float,mp3adu,mp3adufloat \
    --enable-decoder=aac,aac_fixed,alac \
    --enable-decoder=vorbis,opus,flac \
    --enable-decoder=wavpack,ape \
    --enable-decoder=pcm_s16le,pcm_s16be,pcm_s24le,pcm_s24be \
    --enable-decoder=pcm_s32le,pcm_s32be,pcm_f32le,pcm_f32be,pcm_f64le,pcm_f64be \
    --enable-decoder=adpcm_ima_wav,adpcm_ms \
    \
    --enable-parser=mpegaudio,aac,aac_latm,flac,vorbis,opus

echo "  Building ($JOBS jobs)…"
make -j"$JOBS"

echo "  Installing…"
make install

# ── Verify no unexpected external dylib deps ──────────────────────────────────
echo "  Verifying external dependencies…"
UNEXPECTED=0
for lib in "$INSTALL_DIR"/lib/libav*.dylib "$INSTALL_DIR"/lib/libsw*.dylib; do
    [ -f "$lib" ] || continue
    # Allow: system libs (/usr/lib, /System) and sibling FFmpeg libs
    BAD=$(otool -L "$lib" \
        | tail -n +2 \
        | awk '{print $1}' \
        | grep -v '^/usr/lib' \
        | grep -v '^/System' \
        | grep -v '^@' \
        | grep -v "$INSTALL_DIR" \
        || true)
    if [ -n "$BAD" ]; then
        echo "  WARNING: $(basename "$lib") has unexpected external deps:"
        echo "$BAD" | sed 's/^/    /'
        UNEXPECTED=1
    fi
done
if [ "$UNEXPECTED" -eq 0 ]; then
    echo "  ✓ No unexpected external dependencies"
fi

echo
echo "✓ Minimal FFmpeg built → $INSTALL_DIR"
echo "  Libs: $(ls "$INSTALL_DIR/lib"/lib*.dylib 2>/dev/null | xargs -I{} basename {} | tr '\n' ' ')"
