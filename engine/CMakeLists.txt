find_package(PkgConfig REQUIRED)
pkg_check_modules(PORTAUDIO  REQUIRED portaudio-2.0)
pkg_check_modules(SNDFILE    REQUIRED sndfile)
pkg_check_modules(SAMPLERATE REQUIRED samplerate)

# FFmpeg audio libraries (optional — adds AAC/M4A/WMA/… beyond libsndfile)
pkg_check_modules(AVFORMAT libavformat)
pkg_check_modules(AVCODEC  libavcodec)
pkg_check_modules(AVUTIL   libavutil)
if(AVFORMAT_FOUND AND AVCODEC_FOUND AND AVUTIL_FOUND)
    message(STATUS "FFmpeg found (avformat ${AVFORMAT_VERSION}) — extra format support enabled")
    set(MCP_HAVE_FFMPEG TRUE)
else()
    message(STATUS "FFmpeg not found — only libsndfile formats supported")
endif()

# nlohmann/json — header-only, used by ShowFile
include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# RtMidi — cross-platform MIDI I/O
set(RTMIDI_BUILD_TESTING  OFF CACHE BOOL "" FORCE)
set(RTMIDI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    rtmidi
    GIT_REPOSITORY https://github.com/thestk/rtmidi.git
    GIT_TAG        5.0.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(rtmidi)

add_library(mcp_engine STATIC
    src/AudioDecoder.cpp
    src/AudioFile.cpp
    src/AudioEngine.cpp
    src/Scheduler.cpp
    src/CueList.cpp
    src/ShowFile.cpp
    src/FadeData.cpp
    src/StreamReader.cpp
    src/MusicContext.cpp
    src/NetworkSender.cpp
    src/MidiSender.cpp
    src/Timecode.cpp
    src/LtcStreamReader.cpp
    src/MtcGenerator.cpp
    src/SnapshotManager.cpp
    src/SendRouter.cpp
)

# ── Plugin abstraction layer ──────────────────────────────────────────────────
# Standalone static library: no portaudio / sndfile / samplerate dependency.
# Both mcp_engine and the test suite link against it independently.
add_library(mcp_plugin STATIC
    src/plugin/ParameterSet.cpp
    src/plugin/PluginWrapper.cpp
    src/plugin/InternalPluginFactory.cpp
    src/plugin/TrimProcessor.cpp
    src/plugin/DelayProcessor.cpp
    src/plugin/ChannelPluginChain.cpp
    src/plugin/MissingPluginProcessor.cpp
    src/plugin/NativePluginBackend.cpp
    $<$<BOOL:${APPLE}>:src/plugin/AUPluginAdapter.mm>
    $<$<BOOL:${APPLE}>:src/plugin/AUComponentEnumerator.mm>
    $<$<BOOL:${APPLE}>:src/plugin/AUCompatibilityTester.mm>
)

target_include_directories(mcp_plugin
    PUBLIC  include
    PRIVATE src/plugin       # private processor headers
)

target_link_libraries(mcp_plugin PUBLIC nlohmann_json::nlohmann_json)
if(APPLE)
    target_link_libraries(mcp_plugin PUBLIC
        "-framework AudioToolbox"
        "-framework AudioUnit"
        "-framework CoreFoundation"
    )
endif()
target_compile_options(mcp_plugin PRIVATE -Wall -Wextra -Wpedantic)

target_include_directories(mcp_engine
    PUBLIC
        include
    PRIVATE
        ${PORTAUDIO_INCLUDE_DIRS}
        ${SNDFILE_INCLUDE_DIRS}
        ${SAMPLERATE_INCLUDE_DIRS}
        $<$<BOOL:${MCP_HAVE_FFMPEG}>:${AVFORMAT_INCLUDE_DIRS}>
        $<$<BOOL:${MCP_HAVE_FFMPEG}>:${AVCODEC_INCLUDE_DIRS}>
        $<$<BOOL:${MCP_HAVE_FFMPEG}>:${AVUTIL_INCLUDE_DIRS}>
)

target_link_directories(mcp_engine
    PUBLIC
        ${PORTAUDIO_LIBRARY_DIRS}
        ${SNDFILE_LIBRARY_DIRS}
        ${SAMPLERATE_LIBRARY_DIRS}
        $<$<BOOL:${MCP_HAVE_FFMPEG}>:${AVFORMAT_LIBRARY_DIRS}>
        $<$<BOOL:${MCP_HAVE_FFMPEG}>:${AVCODEC_LIBRARY_DIRS}>
        $<$<BOOL:${MCP_HAVE_FFMPEG}>:${AVUTIL_LIBRARY_DIRS}>
)

# LIBRARIES preserves "-framework Foo" as single tokens; LDFLAGS splits them.
target_link_libraries(mcp_engine
    PUBLIC
        mcp_plugin
        ${PORTAUDIO_LIBRARIES}
        ${SNDFILE_LIBRARIES}
        ${SAMPLERATE_LIBRARIES}
        $<$<BOOL:${MCP_HAVE_FFMPEG}>:${AVFORMAT_LIBRARIES}>
        $<$<BOOL:${MCP_HAVE_FFMPEG}>:${AVCODEC_LIBRARIES}>
        $<$<BOOL:${MCP_HAVE_FFMPEG}>:${AVUTIL_LIBRARIES}>
        nlohmann_json::nlohmann_json
        rtmidi
)

if(APPLE)
    target_link_libraries(mcp_engine PUBLIC "-framework CoreMIDI" "-framework CoreFoundation")
endif()
if(WIN32)
    target_link_libraries(mcp_engine PUBLIC winmm)
endif()

if(MCP_HAVE_FFMPEG)
    target_compile_definitions(mcp_engine PRIVATE MCP_HAVE_FFMPEG)
endif()

target_compile_options(mcp_engine PRIVATE -Wall -Wextra -Wpedantic)

if(WIN32)
    target_link_libraries(mcp_engine PUBLIC ws2_32)
endif()

add_executable(cue_runner   src/cue_runner.cpp)
add_executable(show_editor  src/show_editor.cpp)
target_link_libraries(cue_runner  PRIVATE mcp_engine)
target_link_libraries(show_editor PRIVATE mcp_engine)

# Standalone MTC diagnostic tool — no engine dependency, only RtMidi
add_executable(mtc_diag src/mtc_diag.cpp)
target_include_directories(mtc_diag PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(mtc_diag PRIVATE rtmidi)
if(APPLE)
    target_link_libraries(mtc_diag PRIVATE "-framework CoreMIDI" "-framework CoreFoundation")
endif()

enable_testing()
add_subdirectory(tests)
