# Build Instructions

## Dependencies

Install via Homebrew:

```bash
brew install cmake qt portaudio libsndfile libsamplerate ffmpeg
```

## First-time configure

```bash
cmake -S . -B build
```

To build a Release binary instead:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

## Build

```bash
cmake --build build
```

The app bundle is output to `build/bin/mcp_qt_ui.app`.

## Incremental rebuild

After editing source files, just re-run:

```bash
cmake --build build
```

CMake detects which files changed and only recompiles those.
If CMake misses a change (rare), touch the file first:

```bash
touch qt_ui/src/SomeFile.cpp && cmake --build build
```

## Run

```bash
open build/bin/mcp_qt_ui.app
```

or directly:

```bash
build/bin/mcp_qt_ui.app/Contents/MacOS/mcp_qt_ui
```

## Clean rebuild

```bash
rm -rf build && cmake -S . -B build && cmake --build build
```
