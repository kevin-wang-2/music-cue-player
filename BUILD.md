# Build Instructions

## Dependencies

Install via Homebrew:

```bash
brew install cmake qt portaudio libsndfile libsamplerate ffmpeg
```

## Build directories

There are **two** CMake build directories. Never mix them up.

| Directory      | Config   | App bundle output                              |
|----------------|----------|------------------------------------------------|
| `build/`       | Debug    | `build/bin/Music Cue Player.app`               |
| `build/release/` | Release | `build/release/bin/Music Cue Player.app`     |

The Debug directory is the one used for day-to-day development.

## First-time configure

```bash
# Debug (default, used for development)
cmake -S . -B build

# Release (optional, separate directory)
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
```

## Build (Debug — use this one)

```bash
cmake --build build
```

Output: `build/bin/Music Cue Player.app`

## Build (Release)

```bash
cmake --build build/release
```

Output: `build/release/bin/Music Cue Player.app`

## Run

```bash
open "build/bin/Music Cue Player.app"
```

## Incremental rebuild

After editing source files, re-run the same build command. CMake detects changed files automatically.

## Clean rebuild

```bash
rm -rf build && cmake -S . -B build && cmake --build build
```

## For Claude

- **Always build with:** `cmake --build /Users/kaibinwa/Desktop/click-in/music-cue-player/build`
- **App is at:** `build/bin/Music Cue Player.app`
- **Do not** use `build/release` unless the user explicitly asks for a Release build.
- After building, verify the binary exists: `ls "build/bin/Music Cue Player.app/Contents/MacOS/"`
