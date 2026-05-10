#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# package_mac.sh  —  Build a distributable macOS DMG for Music Cue Player.
#
# Usage:
#   bash scripts/package_mac.sh            # full build + package
#   bash scripts/package_mac.sh --skip-build   # repackage existing Release build
#
# Prerequisites (installed automatically if missing via Homebrew):
#   dylibbundler, create-dmg
#
# Output: dist/Music Cue Player-<version>.dmg
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

# ── Configuration ─────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

APP_NAME="Music Cue Player"
APP_VERSION="0.1.0"
BUNDLE_NAME="${APP_NAME}.app"
BINARY_NAME="${APP_NAME}"   # CMake OUTPUT_NAME → binary has same name as bundle

PYTHON_VER="3.13"
PYTHON_BREW="/opt/homebrew/opt/python@${PYTHON_VER}"
PYTHON_FRAMEWORK_SRC="${PYTHON_BREW}/Frameworks/Python.framework"

BUILD_DIR="$PROJECT_ROOT/build/release"
FFMPEG_MINIMAL="$PROJECT_ROOT/build/ffmpeg_minimal"
DIST_DIR="$PROJECT_ROOT/dist"

APP_BUNDLE="$BUILD_DIR/bin/$BUNDLE_NAME"
BINARY="$APP_BUNDLE/Contents/MacOS/$BINARY_NAME"
FRAMEWORKS_DIR="$APP_BUNDLE/Contents/Frameworks"
RESOURCES_DIR="$APP_BUNDLE/Contents/Resources"

SKIP_BUILD=0
for arg in "$@"; do [[ "$arg" == "--skip-build" ]] && SKIP_BUILD=1; done

# ── Helpers ───────────────────────────────────────────────────────────────────
step()  { echo; echo "▶ $*"; }
die()   { echo "✗ ERROR: $*" >&2; exit 1; }

ensure_tool() {
    local cmd="$1" formula="$2"
    if ! command -v "$cmd" &>/dev/null; then
        echo "  Installing $formula via Homebrew…"
        brew install "$formula"
    fi
}

# ── 0. Tool check ─────────────────────────────────────────────────────────────
step "Checking required tools"
ensure_tool dylibbundler dylibbundler
ensure_tool create-dmg   create-dmg
ensure_tool macdeployqt  qt   # macdeployqt ships with qttools
command -v macdeployqt &>/dev/null || die "macdeployqt not found (install qttools via Homebrew)"

# ── 1. Minimal FFmpeg ─────────────────────────────────────────────────────────
step "Minimal FFmpeg"
if [ ! -f "$FFMPEG_MINIMAL/lib/pkgconfig/libavcodec.pc" ]; then
    bash "$SCRIPT_DIR/build_minimal_ffmpeg.sh"
else
    echo "  Already built (delete build/ffmpeg_minimal to rebuild)"
fi

# ── 2. CMake Release build ────────────────────────────────────────────────────
if [ "$SKIP_BUILD" -eq 0 ]; then
    step "CMake Release build"
    export PKG_CONFIG_PATH="$FFMPEG_MINIMAL/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

    cmake -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DPython3_ROOT_DIR="$PYTHON_BREW"

    cmake --build "$BUILD_DIR" --config Release -j"$(sysctl -n hw.logicalcpu)"
else
    step "Skipping build (--skip-build)"
fi

[ -d "$APP_BUNDLE" ] || die "App bundle not found: $APP_BUNDLE"

# ── 3. Qt deployment ──────────────────────────────────────────────────────────
step "macdeployqt"
macdeployqt "$APP_BUNDLE"

# ── 4. Python.framework ───────────────────────────────────────────────────────
step "Bundling Python.framework (stripped)"
[ -d "$PYTHON_FRAMEWORK_SRC" ] || die "Python.framework not found at $PYTHON_FRAMEWORK_SRC"

PYTHON_DEST="$FRAMEWORKS_DIR/Python.framework"
rm -rf "$PYTHON_DEST"
# ditto handles macOS xattrs and symlinks correctly (cp -R fails on Python.framework)
ditto "$PYTHON_FRAMEWORK_SRC" "$PYTHON_DEST"
chmod -R u+w "$PYTHON_DEST"

# Strip directories that are never needed at runtime
PY_LIB="$PYTHON_DEST/Versions/$PYTHON_VER/lib/python${PYTHON_VER}"
for unwanted in \
    test unittest/test config-* \
    tkinter idlelib idle* \
    turtle.py turtledemo \
    lib2to3 ensurepip \
    distutils \
    __phello__ __hello__.py antigravity.py; do
    rm -rf "${PY_LIB:?}/$unwanted" 2>/dev/null || true
done
rm -rf "$PYTHON_DEST/Versions/$PYTHON_VER/include"
rm -rf "$PYTHON_DEST/Versions/$PYTHON_VER/bin"
rm -rf "$PYTHON_DEST/Versions/$PYTHON_VER/share"
# Remove .pyc caches
find "$PYTHON_DEST" -name "__pycache__" -type d -exec rm -rf {} + 2>/dev/null || true

# Fix the framework's own install name
PYTHON_DYLIB="$PYTHON_DEST/Versions/$PYTHON_VER/Python"
NEW_PYTHON_ID="@executable_path/../Frameworks/Python.framework/Versions/$PYTHON_VER/Python"
install_name_tool -id "$NEW_PYTHON_ID" "$PYTHON_DYLIB"

# Rewrite the reference in the main binary
OLD_PYTHON_REF="$(otool -L "$BINARY" | awk '{print $1}' | grep -i 'Python.framework' | head -1 || true)"
if [ -n "$OLD_PYTHON_REF" ]; then
    install_name_tool -change "$OLD_PYTHON_REF" "$NEW_PYTHON_ID" "$BINARY"
    echo "  Rewrote: $(basename "$OLD_PYTHON_REF") → @executable_path/../…"
else
    echo "  WARNING: Python reference not found in binary — skipping rpath fix"
fi

SIZE_BEFORE=$(du -sh "$PYTHON_FRAMEWORK_SRC" 2>/dev/null | cut -f1)
SIZE_AFTER=$(du -sh "$PYTHON_DEST" 2>/dev/null | cut -f1)
echo "  Python.framework: $SIZE_BEFORE (source) → $SIZE_AFTER (stripped)"

# ── 5. Bundle remaining native dylibs ─────────────────────────────────────────
step "Bundling native dylibs (PortAudio, libsndfile, libsamplerate, FFmpeg, RtMidi)"
# RtMidi is built via CMake FetchContent — its dylib lives in _deps/rtmidi-build.
# Add that directory as an extra search path so dylibbundler can resolve it.
# At this point Python's rpath is already fixed, so dylibbundler won't touch it.
RTMIDI_BUILD_DIR="$BUILD_DIR/_deps/rtmidi-build"
dylibbundler \
    --overwrite-files \
    --bundle-deps \
    --fix-file "$BINARY" \
    --dest-dir "$FRAMEWORKS_DIR" \
    --install-path @executable_path/../Frameworks/ \
    --search-path "$RTMIDI_BUILD_DIR" \
    --ignore "$PYTHON_BREW"

# ── 6. scriptlets → Resources ─────────────────────────────────────────────────
step "Copying built-in scriptlets"
mkdir -p "$RESOURCES_DIR/scriptlets"
cp "$PROJECT_ROOT"/scriptlets/*.py "$RESOURCES_DIR/scriptlets/"
echo "  Copied $(ls "$PROJECT_ROOT"/scriptlets/*.py | wc -l | tr -d ' ') scriptlet(s)"

# ── 7. Ad-hoc code signing ────────────────────────────────────────────────────
step "Ad-hoc signing"
# Sign frameworks individually first, then the app bundle
find "$FRAMEWORKS_DIR" \
    \( -name "*.dylib" -o -name "*.framework" \) \
    -exec codesign --force --sign - {} \; 2>/dev/null || true
codesign --force --deep --sign - "$APP_BUNDLE"
echo "  Signed (ad-hoc)"

# ── 8. Sanity check — no remaining Homebrew rpaths ───────────────────────────
step "Sanity check"
BAD_REFS=$(otool -L "$BINARY" | grep '/opt/homebrew' | awk '{print $1}' || true)
if [ -n "$BAD_REFS" ]; then
    echo "  WARNING: binary still references Homebrew paths:"
    echo "$BAD_REFS" | sed 's/^/    /'
else
    echo "  ✓ No remaining Homebrew references in main binary"
fi

BUNDLE_SIZE=$(du -sh "$APP_BUNDLE" | cut -f1)
echo "  Bundle size: $BUNDLE_SIZE"

# ── 9. DMG ────────────────────────────────────────────────────────────────────
step "Creating DMG"
mkdir -p "$DIST_DIR"
DMG_NAME="${APP_NAME}-${APP_VERSION}.dmg"
DMG_PATH="$DIST_DIR/$DMG_NAME"
rm -f "$DMG_PATH"

create-dmg \
    --volname "$APP_NAME" \
    --window-size 620 420 \
    --icon-size 128 \
    --icon "$BUNDLE_NAME" 160 200 \
    --app-drop-link 460 200 \
    --hide-extension "$BUNDLE_NAME" \
    --no-internet-enable \
    "$DMG_PATH" \
    "$APP_BUNDLE"

DMG_SIZE=$(du -sh "$DMG_PATH" | cut -f1)
echo
echo "✓ Done"
echo "  DMG: $DMG_PATH  ($DMG_SIZE)"
echo
echo "  NOTE: This is an ad-hoc signed build."
echo "  Recipients must right-click → Open the first time to bypass Gatekeeper."
